//! Native products for quantized neural networks (`k32_matvec_i8`,
//! `k32_matvec_i4`, `k32_matmul_i8`, `k32_matmul_i4`, `k32_matvec_f32_i8`,
//! `k32_vecmat_f32_i8`, `k32_softmax_f32`, feature `linalg`).
//!
//! The node computes them in native code, with the same bits on every node
//! (`docs/app-development.md`, Linear algebra). Weights are rows of int8 or
//! int4 values with one `f32` scale per row. A metered pod pays for the work
//! from its execution budget.
//!
//! The wrappers check the slices against the matrix they describe, so a call
//! never traps: one that does not fit its slices returns [`Error::Invalid`].
//! Strides count elements of the slice they step through: `row_stride`
//! bytes of `w`, `scale_stride` values of `w_scale`; `0` packs them.

use crate::abi;
use crate::Error;

// The slice length `count` items need, `stride` apart and `last` long each
fn span(count: usize, stride: usize, last: usize) -> Option<usize> {
    if count == 0 {
        return Some(0);
    }
    (count - 1).checked_mul(stride)?.checked_add(last)
}

fn fits(len: usize, count: usize, stride: usize, last: usize) -> bool {
    span(count, stride, last).is_some_and(|n| n <= len)
}

fn u32_of(n: usize) -> Result<u32, Error> {
    u32::try_from(n).map_err(|_| Error::Invalid)
}

/// `out[r] = (sum over c of w[r][c] * x[c]) as f32 * (w_scale[r] * x_scale)`
/// for `out.len()` rows of `x.len()` int8 weights, row `r` at
/// `w[r * row_stride..]`.
pub fn matvec_i8(out: &mut [f32], w: &[i8], w_scale: &[f32], x: &[i8], x_scale: f32, row_stride: usize) -> Result<(), Error> {
    let (rows, cols) = (out.len(), x.len());
    let stride = if row_stride == 0 { cols } else { row_stride };
    if stride < cols || !fits(w.len(), rows, stride, cols) || w_scale.len() < rows {
        return Err(Error::Invalid);
    }
    let rc = unsafe {
        abi::k32_matvec_i8(out.as_mut_ptr(), w.as_ptr(), w_scale.as_ptr(), x.as_ptr(), x_scale, u32_of(rows)?, u32_of(cols)?, u32_of(stride)?)
    };
    Error::from_rc(rc).map(|_| ())
}

/// [`matvec_i8`] for int4 weights, two a byte: column `2j` in the low nibble
/// and `2j + 1` in the high one, each a signed value -8..7. A row is
/// `(x.len() + 1) / 2` bytes.
pub fn matvec_i4(out: &mut [f32], w: &[u8], w_scale: &[f32], x: &[i8], x_scale: f32, row_stride: usize) -> Result<(), Error> {
    let (rows, cols) = (out.len(), x.len());
    let row_bytes = cols.div_ceil(2);
    let stride = if row_stride == 0 { row_bytes } else { row_stride };
    if stride < row_bytes || !fits(w.len(), rows, stride, row_bytes) || w_scale.len() < rows {
        return Err(Error::Invalid);
    }
    let rc = unsafe {
        abi::k32_matvec_i4(out.as_mut_ptr(), w.as_ptr(), w_scale.as_ptr(), x.as_ptr(), x_scale, u32_of(rows)?, u32_of(cols)?, u32_of(stride)?)
    };
    Error::from_rc(rc).map(|_| ())
}

/// [`matvec_i8`] for `n = x_scale.len()` vectors at once: `x` holds them one
/// after another (`x.len() / n` columns each) and `out` the `n` results
/// (`out.len() / n` rows each), the bits of `n` [`matvec_i8`] calls. The node
/// reads each row of `w` once for all of them.
pub fn matmul_i8(out: &mut [f32], w: &[i8], w_scale: &[f32], x: &[i8], x_scale: &[f32], row_stride: usize) -> Result<(), Error> {
    let n = x_scale.len();
    if n == 0 || out.len() % n != 0 || x.len() % n != 0 {
        return Err(Error::Invalid);
    }
    let (rows, cols) = (out.len() / n, x.len() / n);
    let stride = if row_stride == 0 { cols } else { row_stride };
    if stride < cols || !fits(w.len(), rows, stride, cols) || w_scale.len() < rows {
        return Err(Error::Invalid);
    }
    let rc = unsafe {
        abi::k32_matmul_i8(
            out.as_mut_ptr(),
            w.as_ptr(),
            w_scale.as_ptr(),
            x.as_ptr(),
            x_scale.as_ptr(),
            u32_of(rows)?,
            u32_of(cols)?,
            u32_of(stride)?,
            u32_of(n)?,
        )
    };
    Error::from_rc(rc).map(|_| ())
}

/// [`matmul_i8`] for int4 weights, packed as for [`matvec_i4`].
pub fn matmul_i4(out: &mut [f32], w: &[u8], w_scale: &[f32], x: &[i8], x_scale: &[f32], row_stride: usize) -> Result<(), Error> {
    let n = x_scale.len();
    if n == 0 || out.len() % n != 0 || x.len() % n != 0 {
        return Err(Error::Invalid);
    }
    let (rows, cols) = (out.len() / n, x.len() / n);
    let row_bytes = cols.div_ceil(2);
    let stride = if row_stride == 0 { row_bytes } else { row_stride };
    if stride < row_bytes || !fits(w.len(), rows, stride, row_bytes) || w_scale.len() < rows {
        return Err(Error::Invalid);
    }
    let rc = unsafe {
        abi::k32_matmul_i4(
            out.as_mut_ptr(),
            w.as_ptr(),
            w_scale.as_ptr(),
            x.as_ptr(),
            x_scale.as_ptr(),
            u32_of(rows)?,
            u32_of(cols)?,
            u32_of(stride)?,
            u32_of(n)?,
        )
    };
    Error::from_rc(rc).map(|_| ())
}

/// `out[r] = ((sum over c of x[c] * w[r][c] as f32) * w_scale[r]) * alpha`
/// for `out.len()` rows of `x.len()` int8 weights, the sum from 0 in column
/// order: attention scores over an int8 key cache.
pub fn matvec_f32_i8(
    out: &mut [f32],
    w: &[i8],
    w_scale: &[f32],
    x: &[f32],
    alpha: f32,
    row_stride: usize,
    scale_stride: usize,
) -> Result<(), Error> {
    let (rows, cols) = (out.len(), x.len());
    let stride = if row_stride == 0 { cols } else { row_stride };
    let sstride = if scale_stride == 0 { 1 } else { scale_stride };
    if stride < cols || !fits(w.len(), rows, stride, cols) || !fits(w_scale.len(), rows, sstride, 1) {
        return Err(Error::Invalid);
    }
    let rc = unsafe {
        abi::k32_matvec_f32_i8(
            out.as_mut_ptr(),
            w.as_ptr(),
            w_scale.as_ptr(),
            x.as_ptr(),
            alpha,
            u32_of(rows)?,
            u32_of(cols)?,
            u32_of(stride)?,
            u32_of(sstride.checked_mul(4).ok_or(Error::Invalid)?)?,
        )
    };
    Error::from_rc(rc).map(|_| ())
}

/// `out[c] = sum over r of (x[r] * w_scale[r]) * w[r][c] as f32` for
/// `x.len()` rows of `out.len()` int8 weights, the sum from 0 in row order:
/// attention over an int8 value cache.
pub fn vecmat_f32_i8(out: &mut [f32], w: &[i8], w_scale: &[f32], x: &[f32], row_stride: usize, scale_stride: usize) -> Result<(), Error> {
    let (rows, cols) = (x.len(), out.len());
    let stride = if row_stride == 0 { cols } else { row_stride };
    let sstride = if scale_stride == 0 { 1 } else { scale_stride };
    if stride < cols || !fits(w.len(), rows, stride, cols) || !fits(w_scale.len(), rows, sstride, 1) {
        return Err(Error::Invalid);
    }
    let rc = unsafe {
        abi::k32_vecmat_f32_i8(
            out.as_mut_ptr(),
            w.as_ptr(),
            w_scale.as_ptr(),
            x.as_ptr(),
            u32_of(rows)?,
            u32_of(cols)?,
            u32_of(stride)?,
            u32_of(sstride.checked_mul(4).ok_or(Error::Invalid)?)?,
        )
    };
    Error::from_rc(rc).map(|_| ())
}

/// `x[i] = expf(x[i] - max) / sum` in place, the sum from 0 in order.
pub fn softmax_f32(x: &mut [f32]) -> Result<(), Error> {
    let rc = unsafe { abi::k32_softmax_f32(x.as_mut_ptr(), u32_of(x.len())?) };
    Error::from_rc(rc).map(|_| ())
}

/// e^x as `k32_softmax_f32` computes it on every node (`k32_expf` in
/// `k32_app_abi.h`): x = n ln2 + r, then a degree-6 polynomial for e^r, each
/// step one `f32` operation. 0 below -87, e^88 above 88.
// The constants are the header's decimals, which round to the same f32
#[allow(clippy::excessive_precision, clippy::approx_constant)]
pub fn expf(x: f32) -> f32 {
    if x < -87.0 {
        return 0.0;
    }
    let x = if x > 88.0 { 88.0 } else { x };
    let t = x * 1.442_695_04;
    let n = (t + if t >= 0.0 { 0.5 } else { -0.5 }) as i32;
    let mut r = x - n as f32 * 0.693_145_752;
    r -= n as f32 * 1.428_606_77e-6;
    let mut p = 1.388_888_89e-3_f32;
    for c in [8.333_333_33e-3, 4.166_666_67e-2, 1.666_666_67e-1, 0.5, 1.0, 1.0] {
        p = p * r + c;
    }
    p * f32::from_bits(((n + 127) as u32) << 23)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn expf_has_the_nodes_bits() {
        // The bits k32_expf gives on the node (tests/test_pod_linalg.cpp)
        let cases: [(f32, u32); 8] = [
            (-100.0, 0),
            (-20.25, 0x30dc9ef1),
            (-19.890625, 0x311e032c),
            (-1.15625, 0x3ea11ba2),
            (0.0, 0x3f800000),
            (1.0, 0x402df854),
            (7.953125, 0x4531c732),
            (95.0, 0x7ef882b7),
        ];
        for (x, bits) in cases {
            assert_eq!(expf(x).to_bits(), bits, "expf({x})");
        }
    }

    #[test]
    fn matrices_that_do_not_fit_their_slices_are_refused() {
        let mut out = [0.0f32; 4];
        let x = [1i8; 8];
        // 4 rows of 8: 32 bytes packed, 3 * 10 + 8 with a stride of 10
        assert_eq!(matvec_i8(&mut out, &[0; 31], &[1.0; 4], &x, 1.0, 0), Err(Error::Invalid));
        assert_eq!(matvec_i8(&mut out, &[0; 37], &[1.0; 4], &x, 1.0, 10), Err(Error::Invalid));
        assert_eq!(matvec_i8(&mut out, &[0; 64], &[1.0; 4], &x, 1.0, 7), Err(Error::Invalid));
        assert_eq!(matvec_i8(&mut out, &[0; 32], &[1.0; 3], &x, 1.0, 0), Err(Error::Invalid));
        assert_eq!(matvec_i4(&mut out, &[0; 15], &[1.0; 4], &x, 1.0, 0), Err(Error::Invalid));
        // Scales 4 apart: the last row's scale is at 12
        assert_eq!(matvec_f32_i8(&mut out, &[0; 32], &[1.0; 12], &[0.0; 8], 1.0, 0, 4), Err(Error::Invalid));
        assert_eq!(vecmat_f32_i8(&mut out, &[0; 31], &[1.0; 8], &[0.0; 8], 0, 1), Err(Error::Invalid));
        assert_eq!(span(4, usize::MAX, 1), None);
    }
}
