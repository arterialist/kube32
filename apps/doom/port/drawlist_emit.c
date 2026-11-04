// SPDX-License-Identifier: GPL-2.0-or-later
/* The sim-only pod's frame: a draw list instead of pixels (built with
 * -DDG_DRAWLIST_STATS -DDG_SIM_ONLY, see `make sim`).
 *
 * The renderer runs its BSP and visibility pass as usual; where it would
 * rasterize, this module appends to a 16 KiB list. A rasterizer with the
 * render pack (wad.py pack) draws the frame from the list alone. Textures,
 * flats and sprite patches are never read here: R_GetColumn and the flat
 * lookup answer with dummy data, and a sprite is recorded with its
 * per-column clip instead of being walked.
 *
 * The list keeps what the engine computes per seg or per plane once, and
 * leaves to the rasterizer what the engine derives per column or span with
 * integer arithmetic it can repeat exactly:
 *   wall column k of a strip: scale = scale0 + k * scalestep,
 *     iscale = 0xffffffff / scale, light = scalelight[lightnum][scale >> 12]
 *     (scalelight as R_ExecuteSetViewSize builds it, capped at 47)
 *   span at y of a plane: distance = planeheight * yslope[y],
 *     xstep = distance * basexscale, ystep = distance * baseyscale,
 *     light = zlight[lightnum][distance >> 20] (capped at 127)
 * Little-endian; i32 are the engine's 16.16 fixed-point values.
 *   header 'D' 1  u16 count  u16 bytes  u16 viewwidth  u8 viewheight  u8 truncated
 *          i32 sky_iscale  i32 sky_texturemid  i32 basexscale  i32 baseyscale  (26 B)
 *   1 strip  u16 x0  u8 n  u8 texture  u8 light  i32 texturemid  i32 scale0
 *            i32 scalestep, then n x (u8 yl, u8 yh, u8 column)          (16 B + 3/column)
 *            light: 0..15 a scalelight row, 0x80|map a fixed colormap;
 *            texture 255 is the sky (scale0 and scalestep 0)
 *   2 plane  u8 flat  u8 light  i32 planeheight  u16 nspans,
 *            then nspans x (u8 y, u16 x1, u16 x2, i32 xfrac, i32 yfrac)  (9 B + 13/span)
 *            light: 0..15 a zlight row, 0x80|map a fixed colormap
 *   3 sprite u16 lump  u16 x1  u16 x2  u8 light(255: fuzz)  u8 translation
 *            i32 scale  i32 xiscale  i32 startfrac  i32 texturemid
 *            then (x2-x1+1) x (u8 cliptop+2, u8 clipbot+2)             (25 B + 2/column)
 *   4 masked u16 x  u8 texture  u8 column  u8 light  i32 iscale  i32 scale
 *            i32 texturemid  u8 cliptop+2  u8 clipbot+2                 (20 B)
 * Entries come in the engine's order: walls, then planes (sky strips
 * among them), then masked columns and sprites back to front. */
#include <stdint.h>
#include <string.h>

#include "../doomgeneric/doomtype.h"
#include "../doomgeneric/r_defs.h"
#include "../doomgeneric/r_state.h"
#include "../doomgeneric/r_draw.h"
#include "../doomgeneric/r_main.h"
#include "../doomgeneric/r_things.h"
#include "../doomgeneric/r_sky.h"
#include "../doomgeneric/doomstat.h"
#include "../doomgeneric/m_fixed.h"

#define K32_EXPORT(name) __attribute__((export_name(name)))
#define LIST_BYTES 16384
#define STRIPS 3

extern void (*colfunc)(void);
extern void (*basecolfunc)(void);
extern void (*fuzzcolfunc)(void);
extern void (*transcolfunc)(void);
extern void (*spanfunc)(void);
extern int* texturewidthmask;
extern fixed_t pspriteiscale;
extern fixed_t skytexturemid;
extern fixed_t rw_scale, rw_scalestep;
extern lighttable_t** walllights;
extern lighttable_t** planezlight;
extern fixed_t planeheight, basexscale, baseyscale;

byte dg_stats_dummy_column[256];
byte dg_stats_dummy_flat[4096];

static uint8_t list[LIST_BYTES];
static uint32_t len, count;
static int truncated;
static int cur_tex, cur_col;      /* the last R_GetColumn */
static int cur_flat;

/* open wall strips: a seg draws its middle, top and bottom parts column by column, interleaved */
struct strip {
    int open, age;
    uint16_t x0;
    uint8_t n, tex, light;
    fixed_t texturemid, scale0, scalestep;
    uint8_t cols[255][3];
};
static struct strip strips[STRIPS];
static int strip_age;

/* the open plane: its header is in the list, nspans is patched when it closes */
static uint32_t plane_at;
static uint16_t plane_spans;

static uint8_t* room(uint32_t n) {
    if (len + n > LIST_BYTES) { truncated = 1; return 0; }
    uint8_t* p = list + len;
    len += n;
    return p;
}
static void put16(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint8_t map_of(const lighttable_t* map) { return map ? (uint8_t)((map - colormaps) / 256) : 255; }
static uint8_t clip8(short v) { int c = v + 2; return (uint8_t)(c < 0 ? 0 : c > 255 ? 255 : c); }

static void strip_flush(struct strip* s) {
    if (!s->open) return;
    s->open = 0;
    uint8_t* p = room(16 + 3 * s->n);
    if (!p) return;
    count++;
    p[0] = 1; put16(p + 1, s->x0); p[3] = s->n; p[4] = s->tex; p[5] = s->light;
    put32(p + 6, s->texturemid); put32(p + 10, s->scale0); put32(p + 14, s->scalestep);
    memcpy(p + 16, s->cols, 3 * s->n);
}
static void strips_flush(void) {
    for (int i = 0; i < STRIPS; i++) strip_flush(&strips[i]);
}
static void plane_close(void) {
    if (plane_at) put16(list + plane_at + 7, plane_spans);
    plane_at = 0;
}

static void emit_col(void) {
    int sky = cur_tex == skytexture;
    uint8_t tex = sky ? 255 : (uint8_t)cur_tex;
    uint8_t light;
    fixed_t scale0 = sky ? 0 : rw_scale, step = sky ? 0 : rw_scalestep, mid = sky ? 0 : dc_texturemid;
    if (sky) light = 0;
    else if (fixedcolormap) light = 0x80 | map_of(fixedcolormap);
    else light = (uint8_t)((walllights - &scalelight[0][0]) / MAXLIGHTSCALE);
    if (sky) strips_flush();   /* the sky is drawn with the planes: close the wall strips first */
    struct strip* s = 0;
    for (int i = 0; i < STRIPS; i++) {
        struct strip* t = &strips[i];
        if (t->open && t->n < 255 && t->x0 + t->n == dc_x && t->tex == tex && t->light == light && t->texturemid == mid &&
            t->scalestep == step && (sky || t->scale0 + t->n * step == scale0)) { s = t; break; }
    }
    if (!s) {
        for (int i = 0; i < STRIPS; i++) if (!strips[i].open) { s = &strips[i]; break; }
        if (!s) {
            s = &strips[0];
            for (int i = 1; i < STRIPS; i++) if (strips[i].age < s->age) s = &strips[i];
            strip_flush(s);
        }
        s->open = 1; s->x0 = (uint16_t)dc_x; s->n = 0; s->tex = tex; s->light = light;
        s->texturemid = mid; s->scale0 = scale0; s->scalestep = step;
    }
    s->age = ++strip_age;
    s->cols[s->n][0] = (uint8_t)dc_yl; s->cols[s->n][1] = (uint8_t)dc_yh; s->cols[s->n][2] = (uint8_t)cur_col;
    s->n++;
}
static void emit_span(void) {
    if (!plane_at) return;
    if (!plane_spans) {   /* the first span: the plane's height and light are known now */
        uint8_t* h = list + plane_at;
        h[2] = fixedcolormap ? (uint8_t)(0x80 | map_of(fixedcolormap)) : (uint8_t)((planezlight - &zlight[0][0]) / MAXLIGHTZ);
        put32(h + 3, planeheight);
    }
    uint8_t* p = room(13);
    if (!p) return;
    plane_spans++;
    p[0] = (uint8_t)ds_y; put16(p + 1, ds_x1); put16(p + 3, ds_x2); put32(p + 5, ds_xfrac); put32(p + 9, ds_yfrac);
}

void DG_StatsHookDrawFuncs(void) {
    colfunc = basecolfunc = fuzzcolfunc = transcolfunc = emit_col;
    spanfunc = emit_span;
}
int DG_StatsGetColumn(int tex, int col) {
    cur_tex = tex;
    cur_col = col & texturewidthmask[tex];
    return 1;
}
int DG_StatsFlat(int lumpnum) {
    strips_flush();
    plane_close();
    cur_flat = lumpnum - firstflat;
    uint8_t* p = room(9);
    if (!p) return 1;
    count++;
    plane_at = (uint32_t)(p - list);
    plane_spans = 0;
    p[0] = 2; p[1] = (uint8_t)cur_flat; p[2] = 0; put32(p + 3, 0); put16(p + 7, 0);
    return 1;
}
int DG_StatsSprite(vissprite_t* vis) {
    strips_flush();
    plane_close();
    uint32_t cols = (uint32_t)(vis->x2 - vis->x1 + 1);
    uint8_t* p = room(25 + 2 * cols);
    if (!p) return 1;
    count++;
    p[0] = 3; put16(p + 1, vis->patch + firstspritelump); put16(p + 3, vis->x1); put16(p + 5, vis->x2);
    p[7] = map_of(vis->colormap);
    p[8] = (uint8_t)((vis->mobjflags & MF_TRANSLATION) >> MF_TRANSSHIFT);
    put32(p + 9, vis->scale); put32(p + 13, vis->xiscale); put32(p + 17, vis->startfrac); put32(p + 21, vis->texturemid);
    uint8_t* c = p + 25;
    for (int x = vis->x1; x <= vis->x2; x++) { *c++ = clip8(mceilingclip[x]); *c++ = clip8(mfloorclip[x]); }
    return 1;
}
int DG_StatsMaskedSeg(int tex, int col) {
    strips_flush();
    plane_close();
    uint8_t* p = room(20);
    if (!p) return 1;
    count++;
    p[0] = 4; put16(p + 1, dc_x); p[3] = (uint8_t)tex; p[4] = (uint8_t)(col & texturewidthmask[tex]); p[5] = map_of(dc_colormap);
    put32(p + 6, dc_iscale); put32(p + 10, spryscale); put32(p + 14, dc_texturemid);
    p[18] = clip8(mceilingclip[dc_x]); p[19] = clip8(mfloorclip[dc_x]);
    return 1;
}
void DG_StatsMaskedBegin(void) {}
void DG_StatsMaskedEnd(void) {}

/* The frame boundary: doom_render_view calls begin before and end after R_RenderPlayerView */
void DG_DrawListBegin(void) {
    len = 26;
    count = 0;
    truncated = 0;
    plane_at = 0;
    for (int i = 0; i < STRIPS; i++) strips[i].open = 0;
}
void DG_DrawListEnd(void) {
    strips_flush();
    plane_close();
    uint8_t* p = list;
    p[0] = 'D'; p[1] = 1; put16(p + 2, count); put16(p + 4, len); put16(p + 6, viewwidth); p[8] = (uint8_t)viewheight; p[9] = (uint8_t)truncated;
    put32(p + 10, pspriteiscale); put32(p + 14, skytexturemid); put32(p + 18, basexscale); put32(p + 22, baseyscale);
    /* gametic rides in the HTTP header (X-Doom-Frame counts frames); the list stays 26 B of header */
}

K32_EXPORT("doom_drawlist_ptr") uint32_t doom_drawlist_ptr(void) { return (uint32_t)(uintptr_t)list; }
K32_EXPORT("doom_drawlist_len") uint32_t doom_drawlist_len(void) { return len; }
K32_EXPORT("doom_drawlist_count") uint32_t doom_drawlist_count(void) { return count; }
K32_EXPORT("doom_drawlist_truncated") uint32_t doom_drawlist_truncated(void) { return (uint32_t)truncated; }
