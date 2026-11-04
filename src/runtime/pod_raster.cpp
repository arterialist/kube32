// Native software-rasterizer kernels for pods (include/runtime/pod_raster.hpp).
//
// The loops are byte lookups through a light table: plain C for the Xtensa
// LX7 core and any host. Reads from a source go through the caller's
// resolver, which hands over a window of bytes it has checked, so a record
// never reads outside the pod's memory or an open blob.
#include "runtime/pod_raster.hpp"
#include "runtime/pod_linalg.hpp"
#include "runtime/app_pod.hpp"
#include "runtime/pod_blobs.hpp"
#include "runtime/k32_app_abi.h"
#include "wasm3.h"
#include "m3_env.h"

#include <cstdint>
#include <cstring>

namespace kube32::runtime {

static inline uint32_t ld32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
static inline uint32_t ld16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static inline int32_t fixed_mul(int32_t a, int32_t b) { return static_cast<int32_t>((static_cast<int64_t>(a) * b) >> 16); }

// The address of offset `off` in the memory that holds `src`: the same
// blob for a blob address, the pod's memory otherwise
static inline uint32_t sibling(uint32_t src, uint32_t off) {
    return (src & K32_BLOB_ADDR_FLAG) ? (src & ~K32_BLOB_OFFSET_MASK) | (off & K32_BLOB_OFFSET_MASK) : off;
}
// The address `n` bytes past `src`, in the same memory
static inline uint32_t advance(uint32_t src, uint32_t n) {
    return (src & K32_BLOB_ADDR_FLAG) ? (src & ~K32_BLOB_OFFSET_MASK) | ((src + n) & K32_BLOB_OFFSET_MASK) : src + n;
}

// A solid column: rows yl .. yh of frame column x from `texels` (128 bytes)
static inline void solid(uint8_t* dst, uint32_t stride, int32_t y0, uint32_t rows, uint32_t x, int32_t yl, int32_t yh, int32_t iscale,
                         int32_t texturemid, int32_t centery, const uint8_t* texels, const uint8_t* map, const uint8_t* translation) {
    if (yh < yl) return;
    // the engine's fixed_t arithmetic wraps; unsigned keeps that defined
    uint32_t frac = static_cast<uint32_t>(texturemid) + static_cast<uint32_t>(yl - centery) * static_cast<uint32_t>(iscale);
    const int32_t y1 = y0 + static_cast<int32_t>(rows);
    for (int32_t y = yl; y <= yh; y++, frac += static_cast<uint32_t>(iscale)) {
        if (y < y0 || y >= y1) continue;
        uint8_t t;
        if (translation) {
            const uint32_t i = frac >> 16;
            if (i >= kRasterTexelWindow) continue;   // outside the window the resolver checked
            t = translation[texels[i]];
        } else {
            t = texels[(frac >> 16) & (kRasterTexelWindow - 1)];
        }
        dst[static_cast<uint32_t>(y - y0) * stride + x] = map[t];
    }
}

int32_t raster_columns(uint8_t* dst, uint32_t dst_stride, int32_t y0, uint32_t rows, const uint8_t* records, uint32_t n,
                       const uint8_t* colormap, const uint8_t* translation, int32_t centery, RasterResolve resolve, void* ctx,
                       uint32_t* bad) {
    for (uint32_t r = 0; r < n; r++) {
        const uint8_t* rec = records + static_cast<size_t>(r) * kRasterColumnBytes;
        const uint32_t src = ld32(rec);
        const int32_t iscale = static_cast<int32_t>(ld32(rec + 4)), texturemid = static_cast<int32_t>(ld32(rec + 8));
        const int32_t scale = static_cast<int32_t>(ld32(rec + 12));
        const uint32_t x = ld16(rec + 16), index = ld16(rec + 18);
        const uint8_t a = rec[20], b = rec[21], light = rec[22], kind = rec[23];
        *bad = r;
        if (x >= dst_stride || light >= kRasterLightRows || kind > kRasterTablePosts) return K32_ERR_INVAL;
        const uint8_t* map = colormap + static_cast<size_t>(light) * 256;
        if (kind == kRasterSolid) {
            const uint8_t* entry = resolve(ctx, advance(src, 4 * index), 4);
            if (!entry) return K32_ERR_FAULT;
            const uint8_t* texels = resolve(ctx, sibling(src, ld32(entry)), kRasterTexelWindow);
            if (!texels) return K32_ERR_FAULT;
            solid(dst, dst_stride, y0, rows, x, a, b, iscale, texturemid, centery, texels, map, translation);
            continue;
        }
        // posts: the column's start
        uint32_t column;
        if (kind == kRasterPatchPosts) {
            const uint8_t* header = resolve(ctx, src, 8 + 4 * (index + 1));
            if (!header) return K32_ERR_FAULT;
            if (index >= ld16(header)) return K32_ERR_INVAL;   // past the patch's width
            column = advance(src, ld32(header + 8 + 4 * index));
        } else {
            const uint8_t* entry = resolve(ctx, advance(src, 4 * index), 4);
            if (!entry) return K32_ERR_FAULT;
            column = sibling(src, ld32(entry) - 3);
        }
        // the whole column is checked before a pixel is drawn
        uint32_t posts = 0, at = column;
        for (;;) {
            const uint8_t* post = resolve(ctx, at, 1);
            if (!post) return K32_ERR_FAULT;
            if (*post == 0xff) break;
            if (++posts > kRasterMaxPosts) return K32_ERR_INVAL;
            const uint8_t* head = resolve(ctx, at, 2);
            if (!head || !resolve(ctx, advance(at, 3), kRasterTexelWindow)) return K32_ERR_FAULT;
            at = advance(at, 4u + head[1]);
        }
        const int32_t ceilingclip = static_cast<int32_t>(a) - 2, floorclip = static_cast<int32_t>(b) - 2;
        const uint32_t sprtopscreen = (static_cast<uint32_t>(centery) << 16) - static_cast<uint32_t>(fixed_mul(texturemid, scale));
        at = column;
        for (uint32_t p = 0; p < posts; p++) {
            const uint8_t* head = resolve(ctx, at, 2);
            const uint32_t topdelta = head[0], length = head[1];
            const uint32_t topscreen = sprtopscreen + static_cast<uint32_t>(scale) * topdelta;
            const uint32_t bottomscreen = topscreen + static_cast<uint32_t>(scale) * length;
            int32_t yl = static_cast<int32_t>(topscreen + 0xffffu) >> 16, yh = static_cast<int32_t>(bottomscreen - 1) >> 16;
            if (yh >= floorclip) yh = floorclip - 1;
            if (yl <= ceilingclip) yl = ceilingclip + 1;
            if (yl <= yh) {
                const uint8_t* texels = resolve(ctx, advance(at, 3), kRasterTexelWindow);
                solid(dst, dst_stride, y0, rows, x, yl, yh, iscale, static_cast<int32_t>(static_cast<uint32_t>(texturemid) - (topdelta << 16)),
                      centery, texels, map, translation);
            }
            at = advance(at, 4 + length);
        }
    }
    return K32_OK;
}

int32_t raster_spans(uint8_t* dst, uint32_t dst_stride, int32_t y0, uint32_t rows, const uint8_t* records, uint32_t n,
                     const uint8_t* colormap, RasterResolve resolve, void* ctx, uint32_t* bad) {
    const int32_t y1 = y0 + static_cast<int32_t>(rows);
    for (uint32_t r = 0; r < n; r++) {
        const uint8_t* rec = records + static_cast<size_t>(r) * kRasterSpanBytes;
        const uint32_t src = ld32(rec), xfrac = ld32(rec + 4), yfrac = ld32(rec + 8), xstep = ld32(rec + 12), ystep = ld32(rec + 16);
        const uint32_t x1 = ld16(rec + 20), x2 = ld16(rec + 22);
        const uint8_t y = rec[24], light = rec[25];
        *bad = r;
        if (x2 < x1 || x2 >= dst_stride || light >= kRasterLightRows) return K32_ERR_INVAL;
        const uint8_t* tile = resolve(ctx, src, kRasterFlatBytes);
        if (!tile) return K32_ERR_FAULT;
        if (y < y0 || y >= y1) continue;
        const uint8_t* map = colormap + static_cast<size_t>(light) * 256;
        uint8_t* out = dst + static_cast<uint32_t>(y - y0) * dst_stride + x1;
        // x in the top 16 bits and y in the bottom 16, each 6.10: the engine's packed stepping
        uint32_t position = ((xfrac << 10) & 0xffff0000u) | ((yfrac >> 6) & 0x0000ffffu);
        const uint32_t step = ((xstep << 10) & 0xffff0000u) | ((ystep >> 6) & 0x0000ffffu);
        for (uint32_t x = x1; x <= x2; x++, position += step) {
            const uint32_t spot = (position >> 26) | ((position >> 4) & 0x0fc0u);
            *out++ = map[tile[spot]];
        }
    }
    return K32_OK;
}

// ---- the imports -------------------------------------------------------------
// (K32_RASTER_KERNELS_ONLY builds the kernels alone, for a host check without the pod runtime)
#ifndef K32_RASTER_KERNELS_ONLY

#define K32_OOB(fn, operand) "[trap] out of bounds memory access: " operand " of " fn
#define K32_BLOB_OOB(fn, operand) K32_OOB(fn, operand) " (a blob address outside every open blob)"
static const char kTrapColsDst[] = K32_OOB("k32_raster_columns", "dst");
static const char kTrapColsRecords[] = K32_OOB("k32_raster_columns", "records");
static const char kTrapColsColormap[] = K32_OOB("k32_raster_columns", "colormap");
static const char kTrapColsBlobColormap[] = K32_BLOB_OOB("k32_raster_columns", "colormap");
static const char kTrapColsTranslation[] = K32_OOB("k32_raster_columns", "translation");
static const char kTrapColsBlobTranslation[] = K32_BLOB_OOB("k32_raster_columns", "translation");
static const char kTrapColsSrc[] = K32_OOB("k32_raster_columns", "src");
static const char kTrapColsBlobSrc[] = K32_BLOB_OOB("k32_raster_columns", "src");
static const char kTrapSpansDst[] = K32_OOB("k32_raster_spans", "dst");
static const char kTrapSpansRecords[] = K32_OOB("k32_raster_spans", "records");
static const char kTrapSpansColormap[] = K32_OOB("k32_raster_spans", "colormap");
static const char kTrapSpansBlobColormap[] = K32_BLOB_OOB("k32_raster_spans", "colormap");
static const char kTrapSpansSrc[] = K32_OOB("k32_raster_spans", "src");
static const char kTrapSpansBlobSrc[] = K32_BLOB_OOB("k32_raster_spans", "src");

static inline uint32_t arg(const uint64_t* sp, int i) { return static_cast<uint32_t>(sp[i]); }
static const void* result(uint64_t* sp, int32_t rc) {
    *reinterpret_cast<int32_t*>(sp) = rc;
    return m3Err_none;
}
static bool inside(uint64_t size, uint32_t off, uint64_t bytes) { return off <= size && bytes <= size - off; }

// A read-only operand in the pod's memory or, for a blob address, in a blob
// the pod has open. Null when it is not there (not_mapped tells a blob that
// cannot be read in place from one outside every blob).
struct Memory {
    IM3Runtime rt;
    uint8_t* base;
    uint64_t size;
    bool blob_fault;   // the last miss was a blob address
    bool not_mapped;
    // A blob's mapping, looked up once a call (a frame's columns read thousands of windows
    // of the same blob; the lookup takes the blob table's lock and the pod's name)
    uint32_t blob_handle{8};
    const uint8_t* blob_base{nullptr};
    uint64_t blob_size{0};
};
static const uint8_t* resolve(void* ctx, uint32_t addr, uint32_t len) {
    Memory& m = *static_cast<Memory*>(ctx);
    const bool blob = (addr & K32_BLOB_ADDR_FLAG) != 0 && addr >= m.size;
    m.blob_fault = blob;
    if (!blob) return inside(m.size, addr, len) ? m.base + addr : nullptr;
    const uint32_t handle = (addr >> K32_BLOB_HANDLE_SHIFT) & 7, off = addr & K32_BLOB_OFFSET_MASK;
    if (handle != m.blob_handle) {
        int32_t rc = K32_OK;
        const std::string& pod = app_pod_id(m.rt);
        const int32_t size = PodBlobs::instance().size(pod, static_cast<int32_t>(handle));
        const uint8_t* base = size > 0 ? PodBlobs::instance().view(pod, handle, 0, static_cast<uint64_t>(size), rc) : nullptr;
        if (!base) {
            if (rc == K32_ERR_NOTSUP) m.not_mapped = true;
            return nullptr;
        }
        m.blob_handle = handle;
        m.blob_base = base;
        m.blob_size = static_cast<uint64_t>(size);
    }
    return off <= m.blob_size && len <= m.blob_size - off ? m.blob_base + off : nullptr;
}

static M3Result charge(IM3Runtime rt, uint64_t units) {
#if d_m3HasGasMetering
    if (rt->gasLimit == 0) return m3Err_none;
    rt->gasRemaining -= static_cast<int64_t>(linalg_gas(units));
    if (rt->gasRemaining < 0) return m3Err_trapOutOfGas;
#else
    (void)rt;
    (void)units;
#endif
    return m3Err_none;
}

// "i(iiiiiiiii)": dst, dst_stride, y0, rows, records, n, colormap, translation, centery
static const void* raw_columns(IM3Runtime rt, IM3ImportContext, uint64_t* sp, void* mem) {
    const uint32_t dst = arg(sp, 1), stride = arg(sp, 2), y0 = arg(sp, 3), rows = arg(sp, 4), records = arg(sp, 5), n = arg(sp, 6);
    const uint32_t colormap = arg(sp, 7), translation = arg(sp, 8), centery = arg(sp, 9);
    if (stride == 0 || rows == 0) return result(sp, K32_ERR_INVAL);
    Memory m{rt, static_cast<uint8_t*>(mem), m3_GetMemorySizeAt(mem), false, false};
    if (!inside(m.size, dst, static_cast<uint64_t>(stride) * rows)) return kTrapColsDst;
    if (!inside(m.size, records, static_cast<uint64_t>(n) * kRasterColumnBytes)) return kTrapColsRecords;
    const uint8_t* cmap = resolve(&m, colormap, kRasterLightRows * 256);
    if (!cmap) return m.not_mapped ? result(sp, K32_ERR_NOTSUP) : m.blob_fault ? kTrapColsBlobColormap : kTrapColsColormap;
    const uint8_t* trans = nullptr;
    if (translation) {
        trans = resolve(&m, translation, 256);
        if (!trans) return m.not_mapped ? result(sp, K32_ERR_NOTSUP) : m.blob_fault ? kTrapColsBlobTranslation : kTrapColsTranslation;
    }
    if (M3Result r = charge(rt, kRasterUnitsPerRecord * n)) return r;
    uint32_t bad = 0;
    const int32_t rc = raster_columns(m.base + dst, stride, static_cast<int32_t>(y0), rows, m.base + records, n, cmap, trans,
                                      static_cast<int32_t>(centery), &resolve, &m, &bad);
    if (rc == K32_ERR_FAULT) return m.not_mapped ? result(sp, K32_ERR_NOTSUP) : m.blob_fault ? kTrapColsBlobSrc : kTrapColsSrc;
    return result(sp, rc);
}

// "i(iiiiiii)": dst, dst_stride, y0, rows, records, n, colormap
static const void* raw_spans(IM3Runtime rt, IM3ImportContext, uint64_t* sp, void* mem) {
    const uint32_t dst = arg(sp, 1), stride = arg(sp, 2), y0 = arg(sp, 3), rows = arg(sp, 4), records = arg(sp, 5), n = arg(sp, 6);
    const uint32_t colormap = arg(sp, 7);
    if (stride == 0 || rows == 0) return result(sp, K32_ERR_INVAL);
    Memory m{rt, static_cast<uint8_t*>(mem), m3_GetMemorySizeAt(mem), false, false};
    if (!inside(m.size, dst, static_cast<uint64_t>(stride) * rows)) return kTrapSpansDst;
    if (!inside(m.size, records, static_cast<uint64_t>(n) * kRasterSpanBytes)) return kTrapSpansRecords;
    const uint8_t* cmap = resolve(&m, colormap, kRasterLightRows * 256);
    if (!cmap) return m.not_mapped ? result(sp, K32_ERR_NOTSUP) : m.blob_fault ? kTrapSpansBlobColormap : kTrapSpansColormap;
    if (M3Result r = charge(rt, kRasterUnitsPerRecord * n)) return r;
    uint32_t bad = 0;
    const int32_t rc = raster_spans(m.base + dst, stride, static_cast<int32_t>(y0), rows, m.base + records, n, cmap, &resolve, &m, &bad);
    if (rc == K32_ERR_FAULT) return m.not_mapped ? result(sp, K32_ERR_NOTSUP) : m.blob_fault ? kTrapSpansBlobSrc : kTrapSpansSrc;
    return result(sp, rc);
}

void link_raster_host_functions(IM3Module module) {
    m3_LinkRawFunction(module, "env", "k32_raster_columns", "i(iiiiiiiii)", &raw_columns);
    m3_LinkRawFunction(module, "env", "k32_raster_spans", "i(iiiiiii)", &raw_spans);
}
#endif

}  // namespace kube32::runtime
