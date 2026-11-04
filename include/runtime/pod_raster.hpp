#pragma once
// Native software-rasterizer kernels for pods: k32_raster_columns and
// k32_raster_spans (include/runtime/k32_app_abi.h), the inner loops of a
// paletted 2.5D renderer (textured vertical columns and horizontal spans
// through a light table) over a band of an 8-bit frame, with the texels in
// the pod's memory or in a mapped blob. Every operation is integer and
// fixed in order: a board and kube32-sim write the same bytes.
#include <cstddef>
#include <cstdint>

typedef struct M3Module* IM3Module;

namespace kube32::runtime {

// A column record, 24 bytes in the pod's memory, little-endian
//    0 u32 src        a table of u32 offsets (kind 0, 2) or a patch (kind 1),
//                     in the pod's memory or a blob address
//    4 i32 iscale     texels a pixel, 16.16
//    8 i32 texturemid the texel at the view's centre row, 16.16
//   12 i32 scale      pixels a texel, 16.16 (posts only)
//   16 u16 x          the frame column
//   18 u16 index      the column of the table or patch
//   20 u8  a, b       kind 0: the first and last row drawn; posts: the row
//                     above the column (ceiling clip) + 2 and the row below
//                     it (floor clip) + 2
//   22 u8  light      the row of the light table (0 .. kRasterLightRows - 1)
//   23 u8  kind       0 a solid column: table[index] is the offset of its
//                     texels in src's memory; 1 the posts of a patch column:
//                     columnofs[index] of the patch at src; 2 posts behind a
//                     table: the column at table[index] - 3
constexpr uint32_t kRasterColumnBytes = 24;
constexpr uint32_t kRasterSpanBytes = 28;
constexpr uint32_t kRasterLightRows = 34;
constexpr uint32_t kRasterTexelWindow = 128;   // a solid column wraps its texel index at 128
constexpr uint32_t kRasterFlatBytes = 4096;    // a span's source: 64 x 64 texels
constexpr uint32_t kRasterMaxPosts = 256;      // posts a column may hold

enum RasterKind : uint8_t { kRasterSolid = 0, kRasterPatchPosts = 1, kRasterTablePosts = 2 };

// Hands the kernel the bytes at a 32-bit address of the pod (its memory or
// a blob address): a pointer to len bytes, or null when they are not there
typedef const uint8_t* (*RasterResolve)(void* ctx, uint32_t addr, uint32_t len);

// Draws n column records into the band dst of rows y0 .. y0 + rows - 1
// (dst_stride bytes a row, x below dst_stride); pixels outside the band are
// skipped, so a frame is drawn band by band from the same records. A
// solid column writes rows a .. b: pixel y takes texel
// ((texturemid + (y - centery) * iscale) >> 16) & 127 of the column, mapped
// through row light of colormap (kRasterLightRows x 256 bytes); with a
// translation (256 bytes) the texel goes through it first and the index
// is not masked. A post column walks the patch column's posts (topdelta,
// length, a pad, length texels, a pad; 0xff ends them) from the top of the
// sprite at (centery << 16) - texturemid * scale, each post a solid column
// of rows (top + 65535) >> 16 .. (bottom - 1) >> 16 inside the clips with
// texturemid - (topdelta << 16), as a 2.5D engine draws masked columns.
// Returns K32_OK; K32_ERR_INVAL for a record with x >= dst_stride, light
// beyond the table, an unknown kind or more than kRasterMaxPosts posts;
// K32_ERR_FAULT with *bad the record whose source the resolver did not
// have. The records before a bad one are drawn.
int32_t raster_columns(uint8_t* dst, uint32_t dst_stride, int32_t y0, uint32_t rows, const uint8_t* records, uint32_t n,
                       const uint8_t* colormap, const uint8_t* translation, int32_t centery, RasterResolve resolve, void* ctx,
                       uint32_t* bad);

// A span record, 28 bytes, little-endian
//    0 u32 src          4096 texels, a 64 x 64 tile, row after row
//    4 i32 xfrac, yfrac the texel at x1, 16.16
//   12 i32 xstep, ystep texels a pixel, 16.16
//   20 u16 x1, x2       the first and last frame column (x1 <= x2 < dst_stride)
//   24 u8  y            the frame row
//   25 u8  light        the row of the light table
//   26 u16 pad
// Draws n span records into the band: pixel x of a span takes the tile's
// texel at the 6-bit integer parts of a position that steps in 6.10 fixed
// point (the top ten fraction bits of xfrac, yfrac, xstep and ystep).
int32_t raster_spans(uint8_t* dst, uint32_t dst_stride, int32_t y0, uint32_t rows, const uint8_t* records, uint32_t n,
                     const uint8_t* colormap, RasterResolve resolve, void* ctx, uint32_t* bad);

// A metered pod pays linalg_gas(kRasterUnitsPerRecord x records) a call
constexpr uint64_t kRasterUnitsPerRecord = 64;

void link_raster_host_functions(IM3Module module);

}  // namespace kube32::runtime
