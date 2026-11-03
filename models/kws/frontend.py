"""The keyword spotter's features from 16 kHz int16 audio, in integers only,
so that C on any node computes the same values (frontend.c).

One second (16000 samples) gives 49 frames of 40 log-mel energies:
  frame f covers samples 320 f .. 320 f + 479 (30 ms every 20 ms)
  windowed:  x[n] = (s[n] * hann[n] + 16384) >> 15         (hann: Q15, 480 values)
  the 512-point FFT of x (zero after 480), int32 values, int64 products:
      the input in bit-reversed order; for size 2, 4, ..., 512, for each
      block start (a multiple of size) and j < size/2, with w = tw[j * 512/size]
      (Q15 cos, -sin):
          t.re = (b.re w.re - b.im w.im + 16384) >> 15
          t.im = (b.re w.im + b.im w.re + 16384) >> 15
          a, b = a + t, a - t
  power:     p[k] = (re^2 + im^2) >> 8 for k = 0..256 (int64)
  mel:       E[m] = sum over k of p[k] * mel[m][k] (Q10 triangle weights,
             stored as each triangle's first bin, length and weights)
  log:       L = 0 for E = 0; else with n = floor(log2 E) and the 8 bits
             after the leading one, frac (E >> (n - 8) & 255, or
             E << (8 - n) & 255 for n < 8): L = 256 n + frac
             (log2 E in Q8 with a linear mantissa)
>> is an arithmetic shift (floor). The features are L (int32, 49 x 40); the
network quantizes them with its first scale.
"""
import numpy as np

RATE = 16000
SAMPLES = 16000
WIN = 480
HOP = 320
NFFT = 512
FRAMES = 1 + (SAMPLES - WIN) // HOP     # 49
MELS = 40
BINS = NFFT // 2 + 1
F_LO, F_HI = 60.0, 7600.0


def hann_q15():
    n = np.arange(WIN)
    return np.round(32767 * (0.5 - 0.5 * np.cos(2 * np.pi * n / WIN))).astype(np.int16)


def twiddles_q15():
    k = np.arange(NFFT // 2)
    re = np.round(32767 * np.cos(2 * np.pi * k / NFFT)).astype(np.int16)
    im = np.round(-32767 * np.sin(2 * np.pi * k / NFFT)).astype(np.int16)
    return re, im


def mel_q10():
    """40 triangles evenly spaced on the mel scale, weights in Q10 (0..1024)"""
    def mel(f):
        return 1127.0 * np.log(1.0 + f / 700.0)

    def hz(m):
        return 700.0 * (np.exp(m / 1127.0) - 1.0)

    edges = hz(np.linspace(mel(F_LO), mel(F_HI), MELS + 2))
    f = np.arange(BINS) * RATE / NFFT
    w = np.zeros((MELS, BINS))
    for m in range(MELS):
        lo, c, hi = edges[m], edges[m + 1], edges[m + 2]
        w[m] = np.clip(np.minimum((f - lo) / (c - lo), (hi - f) / (hi - c)), 0, None)
    return np.round(w * 1024).astype(np.int16)


HANN = hann_q15()
TW_RE, TW_IM = twiddles_q15()
MEL = mel_q10()
BITREV = np.array([int(format(i, "09b")[::-1], 2) for i in range(NFFT)])


def fft_q(x):
    """x: (..., 512) int64 real input; returns re, im (int64) as the
    integer radix-2 FFT computes them"""
    re = x[..., BITREV].astype(np.int64)
    im = np.zeros_like(re)
    size = 2
    while size <= NFFT:
        half = size // 2
        step = NFFT // size
        wr = TW_RE[np.arange(half) * step].astype(np.int64)
        wi = TW_IM[np.arange(half) * step].astype(np.int64)
        shp = re.shape[:-1] + (NFFT // size, size)
        r = re.reshape(shp)
        i = im.reshape(shp)
        ar, ai = r[..., :half], i[..., :half]
        br, bi = r[..., half:], i[..., half:]
        tr = (br * wr - bi * wi + 16384) >> 15
        ti = (br * wi + bi * wr + 16384) >> 15
        re = np.concatenate([ar + tr, ar - tr], axis=-1).reshape(re.shape)
        im = np.concatenate([ai + ti, ai - ti], axis=-1).reshape(im.shape)
        size *= 2
    return re, im


def log2_q8(e):
    e = np.asarray(e, np.int64)
    out = np.zeros(e.shape, np.int64)
    nz = e > 0
    v = e[nz]
    n = np.floor(np.log2(v.astype(np.float64))).astype(np.int64)
    # exact bit length (float log2 can be off by one near powers of two)
    n = np.where((np.int64(1) << n) > v, n - 1, n)
    n = np.where((np.int64(1) << (n + 1)) <= v, n + 1, n)
    frac = np.where(n >= 8, (v >> np.maximum(n - 8, 0)) & 255, (v << np.maximum(8 - n, 0)) & 255)
    out[nz] = 256 * n + frac
    return out


def features(audio):
    """audio: (..., 16000) int16 -> (..., 49, 40) int32 log-mel (Q8 log2)"""
    a = np.asarray(audio, np.int64)
    idx = np.arange(FRAMES)[:, None] * HOP + np.arange(WIN)[None, :]
    fr = a[..., idx]                                         # (..., 49, 480)
    x = (fr * HANN.astype(np.int64) + 16384) >> 15
    x = np.concatenate([x, np.zeros(x.shape[:-1] + (NFFT - WIN,), np.int64)], axis=-1)
    re, im = fft_q(x)
    p = (re[..., :BINS] ** 2 + im[..., :BINS] ** 2) >> 8
    e = p @ MEL.T.astype(np.int64)                           # (..., 49, 40)
    return log2_q8(e).astype(np.int32)


def mel_sparse():
    """each triangle's first bin and length, and the weights one after the other"""
    start, length, w = [], [], []
    for row in MEL:
        nz = np.flatnonzero(row)
        start.append(nz[0])
        length.append(nz[-1] - nz[0] + 1)
        w.append(row[nz[0]:nz[-1] + 1])
    return np.array(start, np.uint16), np.array(length, np.uint16), np.concatenate(w).astype(np.int16)


def tables_bytes():
    """int16 hann[480], tw_re[256], tw_im[256]; uint16 mel_start[40],
    mel_len[40]; int16 mel_w[sum of mel_len]; zero bytes to a multiple of 4"""
    start, length, w = mel_sparse()
    b = HANN.astype("<i2").tobytes() + TW_RE.astype("<i2").tobytes() + TW_IM.astype("<i2").tobytes() + \
        start.astype("<u2").tobytes() + length.astype("<u2").tobytes() + w.astype("<i2").tobytes()
    return b + b"\0" * (-len(b) % 4)
