// SPDX-License-Identifier: GPL-2.0-or-later
/* The draw-list decoder: turns a frame's draw list (port/drawlist_emit.c)
 * and the render pack (wad.py pack) into kernel records, and draws them
 * band by band through the k32_raster_* kernels. The same C runs in the
 * view pod and in the host check (bench/drawlist_check.cpp): the caller
 * supplies the two kernel entry points and the pack's address space.
 *
 * Addresses are 32-bit, as the kernels take them: an offset into the pod's
 * memory, or a blob address (K32_BLOB_ADDR). The pack's bytes are at
 * `pack`, and the decoder adds the pack's own offsets to it. */
#ifndef DL_DRAWLIST_H
#define DL_DRAWLIST_H
#include <stdint.h>

#define DL_HEADER_BYTES 28
#define DL_COLUMN_RECORDS 64   /* records built before a kernel call */
#define DL_SPAN_RECORDS 32

typedef struct {
    uint32_t pack;           /* the pack's address (see above) */
    const uint8_t* head;     /* the pack's header and tables, readable here */
    uint32_t head_len;
    uint16_t ntextures, nflats, nsprites, first_flat, first_sprite;
    uint32_t palette, colormap;   /* pack offsets */
    const uint8_t* tex_table;     /* 8 B a texture: u16 w, u16 h, u32 column table offset */
    const uint8_t* flat_table;    /* u32 offset a flat */
    const uint8_t* sprite_table;  /* u32 offset, u32 size a sprite lump */
    uint8_t translation[3 * 256]; /* the engine's player colour translations */
    /* the frame being decoded */
    uint16_t viewwidth;
    uint8_t viewheight, skytexture;
    int32_t sky_iscale, sky_texturemid, basexscale, baseyscale;
    /* records under construction */
    uint8_t cols[DL_COLUMN_RECORDS * 24];
    uint8_t spans[DL_SPAN_RECORDS * 28];
    uint32_t ncols, nspans;
    const uint8_t* col_translation;   /* the translation of the records in cols, or 0 */
    uint32_t unsupported;             /* entries skipped (fuzz columns) */
    /* the band */
    uint8_t* band;
    uint32_t stride;
    int32_t y0;
    uint32_t rows;
} dl_ctx;

/* The kernels, supplied by the host: the pod's imports, or the native
 * kernels over an arena in the check */
int32_t dl_host_columns(uint8_t* dst, uint32_t stride, int32_t y0, uint32_t rows, const void* records, uint32_t n, uint32_t colormap,
                        const uint8_t* translation, int32_t centery);
int32_t dl_host_spans(uint8_t* dst, uint32_t stride, int32_t y0, uint32_t rows, const void* records, uint32_t n, uint32_t colormap);

/* Reads the pack's header and tables (head: at least the header's 28 bytes
 * plus the tables; dl_head_len tells how many from the first 28). Returns
 * 0, or -1 for a pack this decoder does not read. */
uint32_t dl_head_len(const uint8_t* head28);
int dl_init(dl_ctx* c, uint32_t pack, const uint8_t* head, uint32_t head_len);

/* Draws rows y0 .. y0 + rows - 1 of the frame the list describes into band
 * (stride bytes a row). Returns 0, or a negative kernel error. */
int32_t dl_draw(dl_ctx* c, const uint8_t* list, uint32_t len, uint8_t* band, uint32_t stride, int32_t y0, uint32_t rows);

extern uint32_t dl_error_at, dl_error_type;

/* The engine's fixed-point helpers, exact */
int32_t dl_fixed_mul(int32_t a, int32_t b);
int32_t dl_fixed_div(int32_t a, int32_t b);

#endif
