"""The node's arithmetic in numpy, over a batch of vectors at once.

Each function computes the bits that kube32's linear algebra imports
(src/runtime/pod_linalg.cpp, k32_app_abi.h) and the C helpers of the pods
compute: every float step is one IEEE binary32 operation, sums run from 0 in
index order, no fused multiply-add. Leading axes are a batch; the
arithmetic of each vector is the C loop's.
"""
import numpy as np

F32 = np.float32


def k_exp(x):
    """k32_expf: x = n ln2 + r, a degree-6 polynomial for e^r"""
    x = np.asarray(x, dtype=F32)
    under = x < F32(-87.0)
    x = np.minimum(x, F32(88.0))
    t = x * F32(1.44269504)
    n = (t + np.where(t >= 0, F32(0.5), F32(-0.5))).astype(np.int32)
    nf = n.astype(F32)
    r = x - nf * F32(0.693145752)
    r = r - nf * F32(1.42860677e-6)
    p = np.full_like(x, F32(1.38888889e-3))
    for c in (8.33333333e-3, 4.16666667e-2, 1.66666667e-1, 0.5, 1.0, 1.0):
        p = p * r + F32(c)
    b = (np.clip(n + 127, 1, 254).astype(np.uint32) << 23).view(F32)
    return np.where(under, F32(0.0), p * b).astype(F32)


def seqsum(a):
    """sum over the last axis from 0.0f, left to right"""
    acc = np.zeros(a.shape[:-1], F32)
    for i in range(a.shape[-1]):
        acc = acc + a[..., i]
    return acc


def rmsnorm(x, w):
    """w * (x / sqrt(mean(x^2) + 1e-5)) as llm-shard's norm()"""
    ss = seqsum((x * x).astype(F32))
    ss = (ss / F32(x.shape[-1])).astype(F32)
    ss = (ss + F32(1e-5)).astype(F32)
    ss = (F32(1.0) / np.sqrt(ss)).astype(F32)
    return (w * (ss[..., None] * x)).astype(F32)


def quantize(x):
    """int8 with one scale per vector: q = round(x * (127 / max|x|)) half away
    from zero, scale = max|x| / 127 (0 for a zero vector)"""
    x = np.asarray(x, F32)
    amax = np.max(np.abs(x), axis=-1)
    safe = np.where(amax == 0, F32(1.0), amax)
    inv = np.where(amax == 0, F32(0.0), (F32(127.0) / safe).astype(F32))
    t = (x * inv[..., None]).astype(F32)
    v = (t + np.where(t >= 0, F32(0.5), F32(-0.5))).astype(np.int32)
    return np.clip(v, -127, 127).astype(np.int8), (amax / F32(127.0)).astype(F32)


def quantize_static(x, inv_scale):
    """int8 with a fixed scale: round(x * inv_scale) half away from zero,
    clamped to -127..127"""
    t = (np.asarray(x, F32) * F32(inv_scale)).astype(F32)
    v = (t + np.where(t >= 0, F32(0.5), F32(-0.5))).astype(np.int64)
    return np.clip(v, -127, 127).astype(np.int8)


def matvec_i8(wq, ws, xq, xs):
    """k32_matvec_i8 (and _i4 with unpacked rows) for a batch of x:
    (float)(sum w[r][c] x[c]) * (w_scale[r] * x_scale)"""
    acc = xq.astype(np.int32) @ wq.astype(np.int32).T
    return (acc.astype(F32) * (ws * np.asarray(xs, F32)[..., None]).astype(F32)).astype(F32)


def matvec_f32_i8(w, ws, x, alpha):
    """k32_matvec_f32_i8: ((sum over c of x[c] * w[r][c]) * w_scale[r]) * alpha.
    w: (..., rows, cols) int8, ws: (..., rows), x: (..., cols)"""
    wf = w.astype(F32)
    d = np.zeros(np.broadcast_shapes(w.shape[:-1], x.shape[:-1] + (1,)), F32)
    for c in range(w.shape[-1]):
        d = d + x[..., c:c + 1] * wf[..., c]
    return ((d * ws).astype(F32) * F32(alpha)).astype(F32)


def vecmat_f32_i8(w, ws, x):
    """k32_vecmat_f32_i8: out[c] = sum over r of (x[r] * w_scale[r]) * w[r][c].
    w: (..., rows, cols) int8, ws: (..., rows), x: (..., rows)"""
    a = (x * ws).astype(F32)
    wf = w.astype(F32)
    out = np.zeros(np.broadcast_shapes(a.shape[:-1] + (1,), wf.shape[:-2] + (wf.shape[-1],)), F32)
    for r in range(w.shape[-2]):
        out = out + a[..., r:r + 1] * wf[..., r, :]
    return out


def softmax(x, mask=None):
    """k32_softmax_f32 over the last axis; mask (same shape, bool) leaves out
    entries as a shorter call would"""
    x = np.asarray(x, F32)
    if mask is None:
        m = np.max(x, axis=-1, keepdims=True)
        e = k_exp((x - m).astype(F32))
    else:
        m = np.max(np.where(mask, x, -np.inf), axis=-1, keepdims=True).astype(F32)
        e = np.where(mask, k_exp((x - m).astype(F32)), F32(0.0))
    return (e / seqsum(e)[..., None]).astype(F32)


class Rng:
    """xorshift32; uniform() = (next >> 8) / 2^24, a float32 in [0, 1)"""

    def __init__(self, seed):
        self.s = (seed & 0xFFFFFFFF) or 0x6D2B79F5

    def next(self):
        s = self.s
        s ^= (s << 13) & 0xFFFFFFFF
        s ^= s >> 17
        s ^= (s << 5) & 0xFFFFFFFF
        self.s = s
        return s

    def uniform(self):
        return F32((self.next() >> 8) * (1.0 / 16777216.0))

    def gauss(self):
        """twelve uniforms summed in order, minus 6.0f"""
        acc = F32(0.0)
        for _ in range(12):
            acc = F32(acc + self.uniform())
        return F32(acc - F32(6.0))
