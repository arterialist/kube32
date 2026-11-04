// SPDX-License-Identifier: GPL-2.0-or-later
/* The game state, digested: the full and the sim-only builds must agree on
 * it every tic (bench/drawlist_check.cpp --state), as two players in
 * lockstep must.
 *
 * doom_state_digest(group): a CRC-32 of one group of the state: 1 the
 * players, 2 the map objects (the thinkers that are mobjs, in thinker
 * order), 3 the sectors and the lines' specials, 4 the random indices,
 * gametic and leveltime, 5 the other thinkers (their count); 0 all of them;
 * 6 M_Random's index alone, which the screen side advances.
 * doom_mobj_field(i, f): field f of the i-th mobj (0 x, 1 y, 2 z, 3 angle,
 * 4 type, 5 health, 6 state index, 7 tics, 8 momx, 9 momy, 10 momz,
 * 11 flags, 12 movedir, 13 movecount, 14 reactiontime, 15 threshold,
 * 16 lastlook, 17 floorz, 18 ceilingz, 19 sprite, 20 frame), 1 for f < 0
 * when the mobj exists, -1 when it does not. */
#include "include/k32libc.h"
#include "../doomgeneric/doomstat.h"
#include "../doomgeneric/p_local.h"
#include "../doomgeneric/r_state.h"
#include "../doomgeneric/info.h"

#define K32_EXPORT(name) __attribute__((export_name(name)))
extern int prndindex, rndindex;

static uint32_t crc_bytes(uint32_t crc, const void* p, size_t n) {
    const uint8_t* b = p;
    for (size_t i = 0; i < n; i++) {
        crc ^= b[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}
static uint32_t crc_i32(uint32_t crc, int32_t v) { return crc_bytes(crc, &v, 4); }

#define MOBJ_FIELDS 21
static int32_t mobj_field(const mobj_t* m, int32_t f) {
    switch (f) {
    case 0: return m->x; case 1: return m->y; case 2: return m->z; case 3: return (int32_t)m->angle;
    case 4: return m->type; case 5: return m->health; case 6: return (int32_t)(m->state - states); case 7: return m->tics;
    case 8: return m->momx; case 9: return m->momy; case 10: return m->momz; case 11: return m->flags;
    case 12: return m->movedir; case 13: return m->movecount; case 14: return m->reactiontime; case 15: return m->threshold;
    case 16: return m->lastlook; case 17: return m->floorz; case 18: return m->ceilingz; case 19: return m->sprite; case 20: return m->frame;
    default: return 0;
    }
}

static uint32_t digest_players(uint32_t crc) {
    for (int p = 0; p < MAXPLAYERS; p++) {
        const player_t* pl = &players[p];
        crc = crc_i32(crc, playeringame[p]);
        if (!playeringame[p]) continue;
        crc = crc_i32(crc, pl->viewz); crc = crc_i32(crc, pl->viewheight); crc = crc_i32(crc, pl->deltaviewheight); crc = crc_i32(crc, pl->bob);
        crc = crc_i32(crc, pl->health); crc = crc_i32(crc, pl->armorpoints); crc = crc_i32(crc, pl->armortype);
        crc = crc_bytes(crc, pl->powers, sizeof pl->powers); crc = crc_bytes(crc, pl->cards, sizeof pl->cards);
        crc = crc_bytes(crc, pl->weaponowned, sizeof pl->weaponowned); crc = crc_bytes(crc, pl->ammo, sizeof pl->ammo);
        crc = crc_i32(crc, pl->readyweapon); crc = crc_i32(crc, pl->pendingweapon); crc = crc_i32(crc, pl->attackdown); crc = crc_i32(crc, pl->usedown);
        crc = crc_i32(crc, pl->cheats); crc = crc_i32(crc, pl->refire); crc = crc_i32(crc, pl->killcount); crc = crc_i32(crc, pl->damagecount);
        crc = crc_i32(crc, pl->bonuscount); crc = crc_i32(crc, pl->extralight); crc = crc_i32(crc, pl->fixedcolormap); crc = crc_i32(crc, pl->playerstate);
        crc = crc_bytes(crc, &pl->cmd, sizeof pl->cmd);
        for (int s = 0; s < NUMPSPRITES; s++) {
            crc = crc_i32(crc, pl->psprites[s].state ? (int32_t)(pl->psprites[s].state - states) : -1);
            crc = crc_i32(crc, pl->psprites[s].tics); crc = crc_i32(crc, pl->psprites[s].sx); crc = crc_i32(crc, pl->psprites[s].sy);
        }
        if (pl->mo) for (int f = 0; f < MOBJ_FIELDS; f++) crc = crc_i32(crc, mobj_field(pl->mo, f));
    }
    return crc;
}
static uint32_t digest_mobjs(uint32_t crc) {
    for (thinker_t* th = thinkercap.next; th != &thinkercap; th = th->next) {
        if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
        const mobj_t* m = (const mobj_t*)th;
        for (int f = 0; f < MOBJ_FIELDS; f++) crc = crc_i32(crc, mobj_field(m, f));
    }
    return crc;
}
static uint32_t digest_sectors(uint32_t crc) {
    for (int i = 0; i < numsectors; i++) {
        const sector_t* s = &sectors[i];
        crc = crc_i32(crc, s->floorheight); crc = crc_i32(crc, s->ceilingheight); crc = crc_i32(crc, s->lightlevel);
        crc = crc_i32(crc, s->special); crc = crc_i32(crc, s->tag); crc = crc_i32(crc, s->soundtraversed); crc = crc_i32(crc, s->specialdata != 0);
        crc = crc_i32(crc, s->floorpic); crc = crc_i32(crc, s->ceilingpic);
    }
    for (int i = 0; i < numlines; i++) crc = crc_i32(crc, lines[i].special);
    return crc;
}
static uint32_t digest_rng(uint32_t crc) {
    crc = crc_i32(crc, prndindex); crc = crc_i32(crc, gametic); crc = crc_i32(crc, leveltime);
    return crc;
}
/* M_Random's index is the screen side's (the status bar's face, the wipe): kept apart */
static uint32_t digest_mrandom(uint32_t crc) { return crc_i32(crc, rndindex); }
static uint32_t digest_other(uint32_t crc) {
    int n = 0;
    for (thinker_t* th = thinkercap.next; th != &thinkercap; th = th->next) {
        if (th->function.acp1 == (actionf_p1)P_MobjThinker) continue;
        n++;
        crc = crc_i32(crc, th->function.acv != 0);
    }
    return crc_i32(crc, n);
}

K32_EXPORT("doom_state_digest") uint32_t doom_state_digest(int32_t group) {
    uint32_t crc = 0xFFFFFFFFu;
    if (group == 0 || group == 1) crc = digest_players(crc);
    if (group == 0 || group == 2) crc = digest_mobjs(crc);
    if (group == 0 || group == 3) crc = digest_sectors(crc);
    if (group == 0 || group == 4) crc = digest_rng(crc);
    if (group == 0 || group == 5) crc = digest_other(crc);
    if (group == 6) crc = digest_mrandom(crc);
    return ~crc;
}
K32_EXPORT("doom_mobj_field") int32_t doom_mobj_field(int32_t i, int32_t f) {
    for (thinker_t* th = thinkercap.next; th != &thinkercap; th = th->next) {
        if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
        if (i-- == 0) return f < 0 ? 1 : mobj_field((const mobj_t*)th, f);
    }
    return -1;
}

/* ---- the renderer's frame, digested (after R_RenderPlayerView) ----
 * doom_render_digest(part): 10 the drawsegs (x1, x2, silhouette, heights,
 * scales and the clip values they point at), 11 the vissprites, 12 the
 * visplanes, 13 the openings in use; 0 all. */
#include "../doomgeneric/r_bsp.h"
#include "../doomgeneric/r_plane.h"
#include "../doomgeneric/r_things.h"
extern short openings[];
extern visplane_t visplanes[];
extern visplane_t* lastvisplane;

static uint32_t digest_drawsegs(uint32_t crc) {
    for (drawseg_t* ds = drawsegs; ds < ds_p; ds++) {
        crc = crc_i32(crc, ds->x1); crc = crc_i32(crc, ds->x2); crc = crc_i32(crc, ds->scale1); crc = crc_i32(crc, ds->scale2);
        crc = crc_i32(crc, ds->scalestep); crc = crc_i32(crc, ds->silhouette); crc = crc_i32(crc, ds->bsilheight); crc = crc_i32(crc, ds->tsilheight);
        crc = crc_i32(crc, ds->curline ? (int32_t)(ds->curline - segs) : -1);
        for (int x = ds->x1; x <= ds->x2; x++) {
            crc = crc_i32(crc, ds->sprtopclip ? ds->sprtopclip[x] : -9999);
            crc = crc_i32(crc, ds->sprbottomclip ? ds->sprbottomclip[x] : -9999);
            crc = crc_i32(crc, ds->maskedtexturecol ? ds->maskedtexturecol[x] : -9999);
        }
    }
    return crc_i32(crc, (int32_t)(ds_p - drawsegs));
}
static uint32_t digest_vissprites(uint32_t crc) {
    for (vissprite_t* v = vissprites; v < vissprite_p; v++) {
        crc = crc_i32(crc, v->x1); crc = crc_i32(crc, v->x2); crc = crc_i32(crc, v->gx); crc = crc_i32(crc, v->gy); crc = crc_i32(crc, v->gz);
        crc = crc_i32(crc, v->gzt); crc = crc_i32(crc, v->startfrac); crc = crc_i32(crc, v->scale); crc = crc_i32(crc, v->xiscale);
        crc = crc_i32(crc, v->texturemid); crc = crc_i32(crc, v->patch); crc = crc_i32(crc, v->colormap ? (int32_t)((v->colormap - colormaps) / 256) : -1);
        crc = crc_i32(crc, v->mobjflags);
    }
    return crc_i32(crc, (int32_t)(vissprite_p - vissprites));
}
static uint32_t digest_visplanes(uint32_t crc) {
    for (visplane_t* pl = visplanes; pl < lastvisplane; pl++) {
        crc = crc_i32(crc, pl->minx); crc = crc_i32(crc, pl->maxx); crc = crc_i32(crc, pl->height); crc = crc_i32(crc, pl->picnum);
        crc = crc_i32(crc, pl->lightlevel);
        for (int x = pl->minx; x <= pl->maxx; x++) { crc = crc_i32(crc, pl->top[x]); crc = crc_i32(crc, pl->bottom[x]); }
    }
    return crc_i32(crc, (int32_t)(lastvisplane - visplanes));
}
static uint32_t digest_openings(uint32_t crc) {
    crc = crc_bytes(crc, openings, (size_t)(lastopening - openings) * sizeof(short));
    return crc_i32(crc, (int32_t)(lastopening - openings));
}
K32_EXPORT("doom_render_digest") uint32_t doom_render_digest(int32_t part) {
    uint32_t crc = 0xFFFFFFFFu;
    if (part == 0 || part == 10) crc = digest_drawsegs(crc);
    if (part == 0 || part == 11) crc = digest_vissprites(crc);
    if (part == 0 || part == 12) crc = digest_visplanes(crc);
    if (part == 0 || part == 13) crc = digest_openings(crc);
    return ~crc;
}
