// SPDX-License-Identifier: GPL-2.0-or-later
/* Draw-list statistics for the split-render study (built with
 * -DDG_DRAWLIST_STATS, see `make stats`).
 *
 * doom_stats_mode(m) picks how a frame is drawn:
 *   0  the engine's own column and span rasterizers (the normal build)
 *   1  the rasterizers are stubs that count what they would have drawn:
 *      the frame's cost is then the BSP and visibility pass plus the
 *      texture and flat lookups
 *   2  as 1, and R_GetColumn and the flat lookups answer with dummy data,
 *      so no texture is composed or cached: the cost a sim-only node pays
 *      when it emits a draw list and never touches a texture
 *
 * doom_stats_read(ptr) copies the counters of the frame drawn since the
 * last read to ptr (struct dg_stats, 4-byte fields) and clears them. */
#include <stdint.h>
#include <string.h>

#include "../doomgeneric/doomtype.h"
#include "../doomgeneric/r_defs.h"
#include "../doomgeneric/r_state.h"
#include "../doomgeneric/r_draw.h"
#include "../doomgeneric/r_main.h"
#include "../doomgeneric/w_wad.h"
#include "../doomgeneric/m_fixed.h"

#define K32_EXPORT(name) __attribute__((export_name(name)))

struct dg_stats {
    uint32_t wall_cols, wall_px;        /* R_DrawColumn from segs and sky */
    uint32_t sky_cols;
    uint32_t sprite_posts, sprite_px;   /* colfunc calls from R_DrawMaskedColumn */
    uint32_t fuzz_posts, trans_posts;
    uint32_t sprites;                   /* R_DrawVisSprite calls */
    uint32_t spans, span_px;
    uint32_t getcolumn_calls;
    uint32_t tex_cols;                  /* distinct (texture, column) */
    uint32_t tex_col_bytes;             /* their heights summed */
    uint32_t flats, flat_bytes;         /* distinct flats, 4096 B each */
    uint32_t sprite_lumps, sprite_lump_bytes;
    uint32_t textures;                  /* distinct textures */
};

static struct dg_stats st;
static int mode;
static int in_masked;

extern void (*colfunc)(void);
extern void (*basecolfunc)(void);
extern void (*fuzzcolfunc)(void);
extern void (*transcolfunc)(void);
extern void (*spanfunc)(void);
extern int* texturewidthmask;

byte dg_stats_dummy_column[256];
byte dg_stats_dummy_flat[4096];

/* distinct (texture, column): open-addressed set, cleared every frame */
#define SETN 8192
static uint32_t colset[SETN];
static uint8_t flatset[256 / 8];
#define SPRN 512
static uint32_t sprset[SPRN];
static uint8_t texset[1024 / 8];

static int set_add(uint32_t* set, unsigned n, uint32_t key) {
    key += 1;   /* 0 marks empty */
    unsigned i = (key * 2654435761u) & (n - 1);
    for (unsigned k = 0; k < n; k++, i = (i + 1) & (n - 1)) {
        if (set[i] == key) return 0;
        if (!set[i]) { set[i] = key; return 1; }
    }
    return 0;
}

static void stub_col(void) {
    if (in_masked) {
        st.sprite_posts++;
        st.sprite_px += dc_yh - dc_yl + 1;
    } else {
        st.wall_cols++;
        st.wall_px += dc_yh - dc_yl + 1;
    }
}
static void stub_fuzz(void) { st.fuzz_posts++; stub_col(); }
static void stub_trans(void) { st.trans_posts++; stub_col(); }
static void stub_span(void) {
    st.spans++;
    st.span_px += ds_x2 - ds_x1 + 1;
}

/* r_main.c, after it picks the rasterizers */
void DG_StatsHookDrawFuncs(void) {
    if (!mode) return;
    colfunc = basecolfunc = stub_col;
    fuzzcolfunc = stub_fuzz;
    transcolfunc = stub_trans;
    spanfunc = stub_span;
}

/* r_data.c R_GetColumn: returns 1 when the caller should use the dummy column */
int DG_StatsGetColumn(int tex, int col) {
    st.getcolumn_calls++;
    col &= texturewidthmask[tex];
    if (set_add(colset, SETN, ((uint32_t)tex << 12) | (uint32_t)col)) {
        st.tex_cols++;
        st.tex_col_bytes += (uint32_t)(textureheight[tex] >> FRACBITS);
    }
    if (tex < 1024 && !(texset[tex >> 3] & (1 << (tex & 7)))) {
        texset[tex >> 3] |= 1 << (tex & 7);
        st.textures++;
    }
    return mode == 2;
}

/* r_plane.c: a flat is about to be drawn; returns 1 when the caller should
 * use the dummy flat */
int DG_StatsFlat(int lumpnum) {
    unsigned f = (unsigned)lumpnum & 255;
    if (!(flatset[f >> 3] & (1 << (f & 7)))) {
        flatset[f >> 3] |= 1 << (f & 7);
        st.flats++;
        st.flat_bytes += 4096;
    }
    return mode == 2;
}

/* r_things.c R_DrawVisSprite */
int DG_StatsSprite(vissprite_t* vis) {
    int lump = vis->patch + firstspritelump;
    st.sprites++;
    if (set_add(sprset, SPRN, (uint32_t)lump)) {
        st.sprite_lumps++;
        st.sprite_lump_bytes += (uint32_t)W_LumpLength((unsigned)lump);
    }
    return 0;
}
/* r_segs.c: a two-sided middle texture column; counted as a wall column by the stub */
int DG_StatsMaskedSeg(int tex, int col) { (void)tex; (void)col; return 0; }
void DG_StatsMaskedBegin(void) { in_masked = 1; }
void DG_StatsMaskedEnd(void) { in_masked = 0; }

K32_EXPORT("doom_stats_mode") void doom_stats_mode(int32_t m) {
    mode = m;
    DG_StatsHookDrawFuncs();
}

K32_EXPORT("doom_stats_read") void doom_stats_read(uint32_t ptr) {
    memcpy((void*)(uintptr_t)ptr, &st, sizeof st);
    memset(&st, 0, sizeof st);
    memset(colset, 0, sizeof colset);
    memset(flatset, 0, sizeof flatset);
    memset(sprset, 0, sizeof sprset);
    memset(texset, 0, sizeof texset);
}
