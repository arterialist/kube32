// SPDX-License-Identifier: GPL-2.0-or-later
/* The draw-list decoder (drawlist.h). The arithmetic the sim leaves to the
 * rasterizer is the engine's own, in the engine's order: scale stepping,
 * the light rows of R_ExecuteSetViewSize and R_InitLightTables, the plane
 * steps of R_MapPlane. All of it is 32-bit integer and fixed point. */
#include "drawlist.h"

#define FRACBITS 16
#define FRACUNIT (1 << FRACBITS)
#define LIGHTLEVELS 16
#define NUMCOLORMAPS 32
#define MAXLIGHTSCALE 48
#define LIGHTSCALESHIFT 12
#define MAXLIGHTZ 128
#define LIGHTZSHIFT 20
#define DISTMAP 2
#define BLOB_FLAG 0x80000000u
#define BLOB_MASK 0x0FFFFFFFu

static uint32_t ld32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t ld16(const uint8_t* p) { return p[0] | (p[1] << 8); }
/* the records are 4-byte aligned; wasm stores little-endian */
static void st32(uint8_t* p, uint32_t v) { __builtin_memcpy(p, &v, 4); }
static void st16(uint8_t* p, uint32_t v) { uint16_t h = (uint16_t)v; __builtin_memcpy(p, &h, 2); }
static int32_t iabs(int32_t v) { return v < 0 ? -v : v; }

int32_t dl_fixed_mul(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> FRACBITS); }
int32_t dl_fixed_div(int32_t a, int32_t b) {
    if ((iabs(a) >> 14) >= iabs(b)) return (a ^ b) < 0 ? (int32_t)0x80000000 : 0x7fffffff;
    return (int32_t)(((int64_t)a << FRACBITS) / b);
}

static uint32_t pack_addr(const dl_ctx* c, uint32_t off) {
    if (c->pack & BLOB_FLAG) return (c->pack & ~BLOB_MASK) | (((c->pack & BLOB_MASK) + off) & BLOB_MASK);
    return c->pack + off;
}
static uint32_t coltable(const dl_ctx* c, uint32_t tex) {
    return tex < c->ntextures ? pack_addr(c, ld32(c->tex_table + 8 * tex + 4)) : pack_addr(c, 0);
}

/* The light rows: the engine's tables, computed on demand (SCREENWIDTH is the view's width here) */
static uint8_t scalelight(const dl_ctx* c, uint32_t lightnum, uint32_t index) {
    const int32_t startmap = ((LIGHTLEVELS - 1 - (int32_t)lightnum) * 2) * NUMCOLORMAPS / LIGHTLEVELS;
    int32_t level = startmap - (int32_t)index * (int32_t)c->viewwidth / (int32_t)c->viewwidth / DISTMAP;
    if (level < 0) level = 0;
    if (level >= NUMCOLORMAPS) level = NUMCOLORMAPS - 1;
    return (uint8_t)level;
}
static uint8_t zlight(const dl_ctx* c, uint32_t lightnum, uint32_t index) {
    const int32_t startmap = ((LIGHTLEVELS - 1 - (int32_t)lightnum) * 2) * NUMCOLORMAPS / LIGHTLEVELS;
    int32_t scale = dl_fixed_div((int32_t)c->viewwidth / 2 * FRACUNIT, (int32_t)(index + 1) << LIGHTZSHIFT);
    scale >>= LIGHTSCALESHIFT;
    int32_t level = startmap - scale / DISTMAP;
    if (level < 0) level = 0;
    if (level >= NUMCOLORMAPS) level = NUMCOLORMAPS - 1;
    return (uint8_t)level;
}
static int32_t yslope(const dl_ctx* c, int32_t y) {
    int32_t dy = ((y - (int32_t)c->viewheight / 2) << FRACBITS) + FRACUNIT / 2;
    dy = iabs(dy);
    return dl_fixed_div((int32_t)c->viewwidth / 2 * FRACUNIT, dy);
}

uint32_t dl_head_len(const uint8_t* h) {
    const uint32_t n = 28 + 8 * ld16(h + 10) + 4 * ld16(h + 12) + 8 * ld16(h + 14);
    return (n + 3) & ~3u;
}

int dl_init(dl_ctx* c, uint32_t pack, const uint8_t* head, uint32_t head_len) {
    static const char magic[] = "K32DOOMPK";
    for (int i = 0; i < 9; i++) if (head[i] != magic[i]) return -1;
    if (head[9] != 2 || head_len < dl_head_len(head)) return -1;
    c->pack = pack;
    c->head = head;
    c->head_len = head_len;
    c->ntextures = ld16(head + 10);
    c->nflats = ld16(head + 12);
    c->nsprites = ld16(head + 14);
    c->first_flat = ld16(head + 16);
    c->first_sprite = ld16(head + 18);
    c->palette = ld32(head + 20);
    c->colormap = ld32(head + 24);
    c->tex_table = head + 28;
    c->flat_table = c->tex_table + 8 * c->ntextures;
    c->sprite_table = c->flat_table + 4 * c->nflats;
    for (int i = 0; i < 256; i++) {
        uint8_t v0 = i, v1 = i, v2 = i;
        if (i >= 0x70 && i <= 0x7f) { v0 = 0x60 + (i & 0xf); v1 = 0x40 + (i & 0xf); v2 = 0x20 + (i & 0xf); }
        c->translation[i] = v0; c->translation[256 + i] = v1; c->translation[512 + i] = v2;
    }
    c->unsupported = 0;
    return 0;
}

static int32_t flush_cols(dl_ctx* c) {
    if (!c->ncols) return 0;
    int32_t rc = dl_host_columns(c->band, c->stride, c->y0, c->rows, c->cols, c->ncols, pack_addr(c, c->colormap), c->col_translation,
                                 (int32_t)c->viewheight / 2);
    c->ncols = 0;
    return rc;
}
static int32_t flush_spans(dl_ctx* c) {
    if (!c->nspans) return 0;
    int32_t rc = dl_host_spans(c->band, c->stride, c->y0, c->rows, c->spans, c->nspans, pack_addr(c, c->colormap));
    c->nspans = 0;
    return rc;
}
static int32_t add_col(dl_ctx* c, uint32_t src, int32_t iscale, int32_t texturemid, int32_t scale, uint32_t x, uint32_t index,
                       uint8_t a, uint8_t b, uint8_t light, uint8_t kind, const uint8_t* translation) {
    int32_t rc = 0;
    if (c->ncols == DL_COLUMN_RECORDS || (c->ncols && translation != c->col_translation)) rc = flush_cols(c);
    c->col_translation = translation;
    uint8_t* r = c->cols + 24 * c->ncols++;
    st32(r, src); st32(r + 4, (uint32_t)iscale); st32(r + 8, (uint32_t)texturemid); st32(r + 12, (uint32_t)scale);
    st16(r + 16, x); st16(r + 18, index); r[20] = a; r[21] = b; r[22] = light; r[23] = kind;
    return rc;
}
static int32_t add_span(dl_ctx* c, uint32_t src, int32_t xfrac, int32_t yfrac, int32_t xstep, int32_t ystep, uint32_t x1, uint32_t x2,
                        uint8_t y, uint8_t light) {
    int32_t rc = 0;
    if (c->nspans == DL_SPAN_RECORDS) rc = flush_spans(c);
    uint8_t* r = c->spans + 28 * c->nspans++;
    st32(r, src); st32(r + 4, (uint32_t)xfrac); st32(r + 8, (uint32_t)yfrac); st32(r + 12, (uint32_t)xstep); st32(r + 16, (uint32_t)ystep);
    st16(r + 20, x1); st16(r + 22, x2); r[24] = y; r[25] = light; r[26] = r[27] = 0;
    return rc;
}
static int32_t keep(int32_t rc, int32_t r) { return rc ? rc : r; }
uint32_t dl_error_at, dl_error_type;   /* where a malformed list stopped the decoder */

int32_t dl_draw(dl_ctx* c, const uint8_t* list, uint32_t len, uint8_t* band, uint32_t stride, int32_t y0, uint32_t rows) {
    if (len < DL_HEADER_BYTES || list[0] != 'D' || list[1] != 1) return -1;
    const uint32_t bytes = ld16(list + 4);
    if (bytes > len) return -1;
    c->viewwidth = (uint16_t)ld16(list + 6);
    c->viewheight = list[8];
    c->skytexture = list[10];
    c->sky_iscale = (int32_t)ld32(list + 12);
    c->sky_texturemid = (int32_t)ld32(list + 16);
    c->basexscale = (int32_t)ld32(list + 20);
    c->baseyscale = (int32_t)ld32(list + 24);
    c->band = band;
    c->stride = stride;
    c->y0 = y0;
    c->rows = rows;
    c->ncols = c->nspans = 0;
    c->col_translation = 0;
    int32_t rc = 0;
    uint32_t at = DL_HEADER_BYTES;
    while (at < bytes) {
        const uint8_t* e = list + at;
        switch (e[0]) {
        case 1: {   /* a wall or sky strip */
            if (at + 18 > bytes) return -1;
            const uint32_t x0 = ld16(e + 1), n = e[3], tex = e[4], light = e[5];
            const int32_t texturemid = (int32_t)ld32(e + 6);
            const uint32_t scale0 = ld32(e + 10), scalestep = ld32(e + 14);
            if (at + 18 + 3 * n > bytes) return -1;
            const uint8_t* col = e + 18;
            const int sky = tex == 255;
            const uint32_t table = coltable(c, sky ? c->skytexture : tex);
            const int32_t band_last = y0 + (int32_t)rows - 1;
            for (uint32_t k = 0; k < n; k++, col += 3) {
                int32_t iscale, mid;
                uint8_t row;
                if ((int32_t)col[0] > band_last || (int32_t)col[1] < y0) continue;   /* the column misses this band */
                const uint32_t scale = scale0 + k * scalestep;
                if (sky) { iscale = c->sky_iscale; mid = c->sky_texturemid; row = 0; }
                else {
                    iscale = (int32_t)(0xffffffffu / scale);
                    mid = texturemid;
                    if (light & 0x80) row = light & 0x7f;
                    else {
                        uint32_t index = scale >> LIGHTSCALESHIFT;
                        if (index >= MAXLIGHTSCALE) index = MAXLIGHTSCALE - 1;
                        row = scalelight(c, light, index);
                    }
                }
                rc = keep(rc, add_col(c, table, iscale, mid, 0, x0 + k, col[2], col[0], col[1], row, 0, 0));
            }
            at += 18 + 3 * n;
            break;
        }
        case 2: {   /* a plane and its spans */
            if (at + 9 > bytes) return -1;
            const uint32_t flat = e[1], light = e[2];
            const int32_t planeheight = (int32_t)ld32(e + 3);
            const uint32_t nspans = ld16(e + 7);
            if (at + 9 + 13 * nspans > bytes) return -1;
            rc = keep(rc, flush_cols(c));   /* planes come after the walls they share pixels with */
            const uint32_t src = pack_addr(c, flat < c->nflats ? ld32(c->flat_table + 4 * flat) : 0);
            const uint8_t* s = e + 9;
            for (uint32_t k = 0; k < nspans; k++, s += 13) {
                const uint32_t y = s[0], x1 = ld16(s + 1), x2 = ld16(s + 3);
                if ((int32_t)y < y0 || (int32_t)y >= y0 + (int32_t)rows) continue;   /* not this band's row */
                const int32_t xfrac = (int32_t)ld32(s + 5), yfrac = (int32_t)ld32(s + 9);
                const int32_t distance = dl_fixed_mul(planeheight, yslope(c, (int32_t)y));
                const int32_t xstep = dl_fixed_mul(distance, c->basexscale), ystep = dl_fixed_mul(distance, c->baseyscale);
                uint8_t row;
                if (light & 0x80) row = light & 0x7f;
                else {
                    uint32_t index = (uint32_t)distance >> LIGHTZSHIFT;
                    if (index >= MAXLIGHTZ) index = MAXLIGHTZ - 1;
                    row = zlight(c, light, index);
                }
                rc = keep(rc, add_span(c, src, xfrac, yfrac, xstep, ystep, x1, x2, (uint8_t)y, row));
            }
            at += 9 + 13 * nspans;
            break;
        }
        case 3: {   /* a sprite: a column of posts per frame column */
            if (at + 25 > bytes) return -1;
            const uint32_t lump = ld16(e + 1), x1 = ld16(e + 3), x2 = ld16(e + 5), light = e[7], trans = e[8];
            const int32_t scale = (int32_t)ld32(e + 9), xiscale = (int32_t)ld32(e + 13);
            const int32_t startfrac = (int32_t)ld32(e + 17), texturemid = (int32_t)ld32(e + 21);
            const uint32_t cols = x2 < x1 ? 0 : x2 - x1 + 1;
            if (at + 25 + 2 * cols > bytes) { dl_error_at = at; dl_error_type = 3; return -1; }
            rc = keep(rc, flush_spans(c));
            if (light == 255) { c->unsupported++; at += 25 + 2 * cols; break; }   /* a fuzz (spectre) column */
            const uint32_t idx = lump - c->first_sprite;
            const uint32_t src = pack_addr(c, idx < c->nsprites ? ld32(c->sprite_table + 8 * idx) : 0);
            const uint8_t* translation = trans ? c->translation + (trans - 1) * 256 : 0;
            const int32_t iscale = iabs(xiscale);
            const uint8_t* clip = e + 25;
            uint32_t frac = (uint32_t)startfrac;
            for (uint32_t x = x1; x <= x2; x++, frac += (uint32_t)xiscale, clip += 2) {
                if ((int32_t)clip[0] - 2 + 1 > y0 + (int32_t)rows - 1 || (int32_t)clip[1] - 2 - 1 < y0) continue;   /* clipped out of this band */
                const uint32_t column = (uint32_t)((int32_t)frac >> FRACBITS);
                rc = keep(rc, add_col(c, src, iscale, texturemid, scale, x, column, clip[0], clip[1], (uint8_t)light, 1, translation));
            }
            at += 25 + 2 * cols;
            break;
        }
        case 4: {   /* a two-sided middle texture column */
            if (at + 20 > bytes) return -1;
            const uint32_t x = ld16(e + 1), tex = e[3], col = e[4], light = e[5];
            const int32_t iscale = (int32_t)ld32(e + 6), scale = (int32_t)ld32(e + 10), texturemid = (int32_t)ld32(e + 14);
            rc = keep(rc, flush_spans(c));
            rc = keep(rc, add_col(c, coltable(c, tex), iscale, texturemid, scale, x, col, e[18], e[19], (uint8_t)light, 2, 0));
            at += 20;
            break;
        }
        default:
            dl_error_at = at; dl_error_type = e[0];
            return -1;
        }
    }
    rc = keep(rc, flush_cols(c));
    rc = keep(rc, flush_spans(c));
    return rc;
}
