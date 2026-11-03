"""The doodle denoiser: a small convolutional U-Net over 14x14 maps.

The 28x28 image goes in as a 14x14 map of 4 channels (each 2x2 block of
pixels, channel 2 dy + dx), and the prediction comes out the same way.
Seven 3x3 convolutions, each followed by a bias from the step and the class
and SiLU (except the last):

  in   c0 x 14x14   from the 4 input channels
  e1   c0 x 14x14
  down c1 x 7x7     stride 2
  mid  c1 x 7x7     (times `mids`)
  up   c0 x 14x14   from the 7x7 map repeated 2x2, plus e1's output
  d1   c0 x 14x14   plus in's output
  out  4 x 14x14    v = alpha * noise - sigma * image

Diffusion: alpha(t) = cos(pi/2 t), sigma(t) = sin(pi/2 t), t in [0, 1];
the class 6 means "no class" (classifier-free guidance).
"""
import math

import torch
import torch.nn as nn
import torch.nn.functional as F

CLASSES = ("cat", "dog", "bird", "house", "tree", "bed")
N_COND = len(CLASSES) + 1
TIME_FREQS = 16


def alpha_sigma(t):
    return torch.cos(0.5 * math.pi * t), torch.sin(0.5 * math.pi * t)


def time_features(t):
    """sin and cos of 1000 t at TIME_FREQS/2 frequencies"""
    half = TIME_FREQS // 2
    f = torch.exp(-math.log(1000.0) * torch.arange(half, device=t.device) / half)
    a = 1000.0 * t[:, None] * f[None]
    return torch.cat([torch.sin(a), torch.cos(a)], dim=1)


def fake_quant(x, scale):
    """int8 with a fixed scale, rounded half away from zero; gradients pass
    straight through"""
    q = torch.clamp(torch.sign(x / scale) * torch.floor((x / scale).abs() + 0.5), -127, 127) * scale
    return x + (q - x).detach()


def fake_quant_rows(w):
    s = w.detach().abs().flatten(1).amax(1).clamp_min(1e-12) / 127.0
    shape = (-1,) + (1,) * (w.dim() - 1)
    q = torch.clamp(torch.round(w / s.view(shape)), -127, 127) * s.view(shape)
    return w + (q - w).detach()


class Denoiser(nn.Module):
    def __init__(self, c0=20, c1=32, mids=1):
        super().__init__()
        self.c0, self.c1, self.mids = c0, c1, mids
        self.convs = nn.ModuleList([
            nn.Conv2d(4, c0, 3, padding=1),                 # in
            nn.Conv2d(c0, c0, 3, padding=1),                # e1
            nn.Conv2d(c0, c1, 3, stride=2, padding=1),      # down
            *[nn.Conv2d(c1, c1, 3, padding=1) for _ in range(mids)],
            nn.Conv2d(c1, c0, 3, padding=1),                # up
            nn.Conv2d(c0, c0, 3, padding=1),                # d1
            nn.Conv2d(c0, 4, 3, padding=1),                 # out
        ])
        self.widths = [c.out_channels for c in self.convs[:-1]]
        n = sum(self.widths)
        self.time = nn.Sequential(nn.Linear(TIME_FREQS, 64), nn.SiLU(), nn.Linear(64, n))
        self.cls = nn.Embedding(N_COND, n)
        nn.init.zeros_(self.cls.weight)
        nn.init.zeros_(self.convs[-1].weight)
        nn.init.zeros_(self.convs[-1].bias)
        self.act_scales = None     # fixed input scales per convolution (quantization-aware)
        self.quant_weights = False
        self.record = None         # a list per convolution: samples of |input|

    def cond(self, t, c):
        return self.time(time_features(t)) + self.cls(c)

    def forward(self, x, t, c, cond=None):
        """x: (B, 1, 28, 28) noisy image; returns v, (B, 1, 28, 28)"""
        cond = self.cond(t, c) if cond is None else cond
        bias = torch.split(cond, self.widths, dim=1)
        h = F.pixel_unshuffle(x, 2)
        skips = []
        n = len(self.convs)
        up = 3 + self.mids
        for i, conv in enumerate(self.convs):
            if self.record is not None:
                a = h.detach().abs().flatten()
                self.record[i].append(a[torch.randint(0, a.numel(), (8192,), device=a.device)].cpu())
            if self.act_scales is not None:
                h = fake_quant(h, self.act_scales[i])
            if i == up:
                h = F.interpolate(h, scale_factor=2, mode="nearest")
            w = fake_quant_rows(conv.weight) if self.quant_weights else conv.weight
            h = F.conv2d(h, w, conv.bias, stride=conv.stride, padding=1)
            if i == n - 1:
                break
            h = h + bias[i][:, :, None, None]
            if i == up:
                h = h + skips[1]
            elif i == up + 1:
                h = h + skips[0]
            h = F.silu(h)
            if i < 2:
                # the skips as the next layers read them: quantized at their scale
                skips.append(fake_quant(h, self.act_scales[i + 1]) if self.act_scales is not None else h)
        return F.pixel_shuffle(h, 2)

    def n_weights(self):
        return sum(c.weight.numel() for c in self.convs)
