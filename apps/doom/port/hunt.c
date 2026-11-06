// SPDX-License-Identifier: GPL-2.0-or-later
/* The hunt: with env DOOM_HUNT=1 the sim shoots the cluster it runs on. The
 * idea comes from KubeDoom.
 *
 * Every demon on the map is a pod. Pods announce themselves on the channel
 * topic doom.pods ("<pod> <node>", hunt/target.c): an announcement from a pod
 * the sim has not seen puts a demon on the map, near the player and in sight,
 * and a pod that stays silent for SILENT_MS loses its demon. A pod on the sim's
 * node is a zombieman, one on another node an imp. The map's own monsters stay
 * away (nomonsters), and the sim is a demon itself: a red marine that stands
 * where the map starts.
 *
 * A demon that dies goes out as an event on doom.kill, from k32_tick, never
 * from a tic; doom-reaper (hunt/reaper.c) deletes the pod through the API
 * server and answers on doom.reaped ("<pod> <http status> <ms>"). The event
 * goes out again every RESEND_MS until the answer arrives. The Deployment
 * then starts a replacement, which announces itself and comes back as a new
 * demon. When the pod is this one, the Deployment restarts it, and the new
 * pod starts the map and hears the pods again.
 *
 * The game is a single player's: a demon appears at no fixed tic, so two
 * engines would not agree. The player cannot die and does not run out of
 * ammo, one bullet that finds a demon kills it, and a shot takes the demon
 * the HUD names. Only the player hurts anything, so a stray shot does not
 * delete a pod. A demon in pursuit costs the devkit's interpreter about 3 ms
 * a tic, so the AWAKE nearest ones hunt the player and the rest stand where
 * they appeared.
 *
 * What the draw list carries for the view, after the status bar's record:
 *   'P' u8 bytes  u8 line colour  u8 line length  the line
 *   u8 tags, then each tag: u8 x  u8 y  u8 colour  u8 length  its text
 * x is the screen column of the sprite's centre, y the row of its top.
 * Colours: 0 zombie, 1 imp, 2 the sim, 3 a pod just started. */
#include "include/k32libc.h"
#include "../../../include/runtime/k32_app_abi.h"

#include "../doomgeneric/doomdef.h"
#include "../doomgeneric/doomstat.h"
#include "../doomgeneric/p_local.h"
#include "../doomgeneric/r_main.h"
#include "../doomgeneric/r_state.h"
#include "../doomgeneric/r_things.h"
#include "../doomgeneric/tables.h"
#include "hunt.h"

#define PODS 16
#define NAME_BYTES 40
#define NODE_BYTES 16
#define SILENT_MS 8000    /* a pod announces itself every 2 s */
#define RESEND_MS 1000
#define FORGET_MS 12000   /* a dead pod's name is kept this long past its last announcement */
#define LINE_MS 3500      /* how long a kill or a new pod stays on the line */
#define CORPSES 8
#define CORPSE_TICS (20 * TICRATE)
#define TAGS 6
#define AWAKE 2           /* demons in pursuit at a time */
#define SPOT_TRIES 12     /* places tried in a k32_tick */

enum { C_ZOMBIE, C_IMP, C_SELF, C_NEW };
enum { FREE, WAITING, LIVE, DEAD };

typedef struct {
    char name[NAME_BYTES], node[NODE_BYTES];
    uint8_t state, kind, acked, awake;
    mobj_t* mo;
    int64_t seen_ms, sent_ms;
} pod_t;

extern int k32_libc_quiet;

int hunt_on;
static pod_t pods[PODS];                /* pods[0] is this pod */
static uint32_t kills, spawns, reaped, resent, ignored;
static int aimed = -1;
static char line[64];
static uint8_t line_color;
static int64_t line_until;
static int last_leveltime;

static const mobjtype_t kinds[] = {MT_POSSESSED, MT_TROOP, MT_PLAYER};

static struct { mobj_t* mo; int tic; } corpses[CORPSES];
static unsigned corpse_next;

static struct {
    const vissprite_t* vis;
    uint8_t pod, x, y, seen;
} tags[TAGS];
static unsigned ntags;

/* What happened lately, for GET /doom/hunt. No k32_log: it allocates on the node, and kills come
 * when the control plane is shortest of memory (a failed allocation there ends the node). */
#define LOG_EVENTS 24
static struct { char name[NAME_BYTES - 8]; char what; int16_t status, ms; int64_t t; } events[LOG_EVENTS];
static uint32_t events_next;

static void note(char what, const char* name, int status, int ms) {
    const unsigned i = events_next++ % LOG_EVENTS;
    strncpy(events[i].name, name, sizeof events[i].name - 1);
    events[i].what = what;
    events[i].status = (int16_t)status;
    events[i].ms = (int16_t)ms;
    events[i].t = k32_unix_time_ms();
}

static void set_line(const char* what, const pod_t* p, int color, int64_t until) {
    snprintf(line, sizeof line, "%s%s @%s", what, p->name, p->node);
    line_color = (uint8_t)color;
    line_until = until;
}

static int pod_of(const mobj_t* mo) {
    for (int i = 0; i < PODS; i++) if (pods[i].mo == mo && pods[i].state == LIVE) return i;
    return -1;
}

static pod_t* find(const char* name, unsigned n) {
    for (int i = 0; i < PODS; i++)
        if (pods[i].state != FREE && !strncmp(pods[i].name, name, n) && !pods[i].name[n]) return &pods[i];
    return 0;
}

void hunt_init(void) {
    char v[8];
    int32_t n = k32_env_get("DOOM_HUNT", 9, v, sizeof v - 1);
    hunt_on = n > 0 && v[0] == '1';
    if (!hunt_on) return;
    k32_libc_quiet = 1;
    pod_t* me = &pods[0];
    n = k32_env_get("POD_NAME", 8, me->name, sizeof me->name - 1);
    if (n <= 0) strcpy(me->name, "doom-sim");
    n = k32_env_get("NODE_NAME", 9, me->node, sizeof me->node - 1);
    if (n <= 0) strcpy(me->node, "control-plane");
    me->kind = C_SELF;
    me->state = WAITING;
    note('s', me->name, 0, 0);
}

/* ---- events ------------------------------------------------------------ */

static void announced(const char* msg, uint32_t len) {
    const char* end = msg + len;
    const char* sp = memchr(msg, ' ', len);
    if (!sp || sp == msg || sp - msg >= NAME_BYTES) return;
    const unsigned n = (unsigned)(sp - msg);
    const int64_t now = k32_time_ms();
    pod_t* p = find(msg, n);
    if (p) { p->seen_ms = now; return; }
    for (int i = 1; i < PODS; i++) {
        p = &pods[i];
        if (p->state != FREE) continue;
        memcpy(p->name, msg, n);
        p->name[n] = 0;
        const unsigned m = (unsigned)(end - sp - 1);
        memcpy(p->node, sp + 1, m < NODE_BYTES ? m : NODE_BYTES - 1);
        p->node[m < NODE_BYTES ? m : NODE_BYTES - 1] = 0;
        p->kind = strcmp(p->node, pods[0].node) ? C_IMP : C_ZOMBIE;
        p->state = WAITING;
        p->mo = 0;
        p->seen_ms = now;
        return;
    }
    ignored++;     /* more pods than the table holds: the rest stay out of the game */
}

static void answered(const char* msg, uint32_t len) {
    const char* sp = memchr(msg, ' ', len);
    if (!sp) return;
    pod_t* p = find(msg, (unsigned)(sp - msg));
    if (!p || p->state != DEAD || p->acked) return;
    p->acked = 1;
    reaped++;
    const char* end = msg + len;
    const char* sp2 = memchr(sp + 1, ' ', (size_t)(end - sp - 1));
    note('r', p->name, atoi(sp + 1), sp2 ? atoi(sp2 + 1) : 0);
    set_line("DELETED ", p, p->kind == C_SELF ? C_SELF : C_NEW, k32_time_ms() + LINE_MS);
}

int hunt_event(const char* topic, uint32_t topic_len, const char* msg, uint32_t len) {
    if (!hunt_on) return 0;
    if (topic_len == 9 && !memcmp(topic, "doom.pods", 9)) { announced(msg, len); return 1; }
    if (topic_len == 11 && !memcmp(topic, "doom.reaped", 11)) { answered(msg, len); return 1; }
    return 0;
}

/* ---- demons ------------------------------------------------------------ */

/* A place on a ring around the player, in front of the player first. In sight and free
 * for a thing of the kind's size; the search goes on where the last one stopped, so demons
 * do not stack up on one spot. */
static const int16_t ring_deg[] = {0, 30, -30, 60, -60, 90, -90, 120, -120, 150, -150, 180};
static const int16_t ring_dist[] = {256, 384, 192, 512};
#define RING_SPOTS 48

static int spot_try(const mobj_t* pmo, unsigned i, const mobjinfo_t* info, fixed_t* x, fixed_t* y) {
    static mobj_t probe;
    const angle_t a = pmo->angle + (angle_t)(ring_deg[i % 12] * (int)(ANG45 / 45));
    const fixed_t r = ring_dist[(i / 12) % 4] * FRACUNIT;
    fixed_t cx = pmo->x + FixedMul(r, finecosine[a >> ANGLETOFINESHIFT]);
    fixed_t cy = pmo->y + FixedMul(r, finesine[a >> ANGLETOFINESHIFT]);
    probe.flags = info->flags;
    probe.radius = info->radius;
    probe.height = info->height;
    if (!P_CheckPosition(&probe, cx, cy) || tmceilingz - tmfloorz < info->height) return 0;
    probe.x = cx;
    probe.y = cy;
    probe.z = tmfloorz;
    probe.subsector = R_PointInSubsector(cx, cy);
    if (!P_CheckSight((mobj_t*)pmo, &probe)) return 0;
    *x = cx;
    *y = cy;
    return 1;
}

static int spawn_demon(pod_t* p, const mobj_t* pmo) {
    static unsigned ring_i;
    const mobjinfo_t* info = &mobjinfo[kinds[p->kind]];
    fixed_t x, y;
    int found = 0;
    if (p->kind == C_SELF) {             /* straight ahead of where the map starts, close */
        for (unsigned i = 24; i < 24 + RING_SPOTS && !found; i++) found = spot_try(pmo, i, info, &x, &y);
    } else {
        for (int t = 0; t < SPOT_TRIES && !found; t++) {
            if (!(ring_i % 12)) ring_i++;    /* the line ahead stays clear of demons */
            found = spot_try(pmo, ring_i++ % RING_SPOTS, info, &x, &y);
        }
    }
    if (!found) return 0;
    mobj_t* mo = P_SpawnMobj(x, y, ONFLOORZ, kinds[p->kind]);
    mo->angle = R_PointToAngle2(x, y, pmo->x, pmo->y);
    if (p->kind == C_SELF) mo->flags |= 3 << MF_TRANSSHIFT;   /* the engine's red player */
    else {
        P_SpawnMobj(x, y, mo->z, MT_TFOG);
        mo->tics = -1;                   /* stands in its first frame until wake_nearest */
    }
    p->mo = mo;
    p->state = LIVE;
    p->awake = 0;
    spawns++;
    note('d', p->name, 0, 0);
    if (p->kind != C_SELF && k32_time_ms() >= line_until) set_line("NEW ", p, C_NEW, k32_time_ms() + LINE_MS);
    return 1;
}

/* The nearest demons that stand wake up, until AWAKE of them are in pursuit */
static void wake_nearest(mobj_t* pmo) {
    int awake = 0, nearest = -1;
    fixed_t best = INT_MAX;
    for (int i = 1; i < PODS; i++) {
        const pod_t* p = &pods[i];
        if (p->state != LIVE) continue;
        if (p->awake) { awake++; continue; }
        const fixed_t d = P_AproxDistance(p->mo->x - pmo->x, p->mo->y - pmo->y);
        if (d < best) { best = d; nearest = i; }
    }
    if (awake >= AWAKE || nearest < 0) return;
    pod_t* p = &pods[nearest];
    p->awake = 1;
    p->mo->target = pmo;
    p->mo->threshold = BASETHRESHOLD;
    P_SetMobjState(p->mo, p->mo->info->seestate);
}

/* The next kill's slot stays empty: the kill itself only writes it down */
static void corpses_expire(void) {
    for (unsigned i = 0; i < CORPSES; i++) {
        if (corpses[i].mo && (gametic - corpses[i].tic > CORPSE_TICS || i == corpse_next % CORPSES)) {
            P_RemoveMobj(corpses[i].mo);
            corpses[i].mo = 0;
        }
    }
}

/* Runs inside a tic, at the bottom of the player's thinker: the interpreter's native stack is
 * deep there, so this writes the kill down and hunt_tick does the rest */
void hunt_killed(mobj_t* target) {
    const int i = pod_of(target);
    if (i < 0) return;
    pods[i].state = DEAD;
    pods[i].mo = 0;
    pods[i].acked = 0;
    pods[i].sent_ms = 0;
    const unsigned c = corpse_next++ % CORPSES;
    if (!corpses[c].mo) {
        corpses[c].mo = target;
        corpses[c].tic = gametic;
    }
}

/* Only the player hurts anything: demons do not shoot each other's pods down */
int hunt_shielded(const mobj_t* target, const mobj_t* source) {
    return !target->player && !(source && source->player);
}

/* The demon a shot from this mobj takes: straight ahead, or a little to either side, as
 * the engine's own aim looks for a target up and down */
static int aim_from(mobj_t* mo) {
    static const int8_t deg[] = {0, 4, -4, 8, -8};
    int sim = -1;
    for (unsigned i = 0; i < sizeof deg; i++) {
        P_AimLineAttack(mo, mo->angle + (angle_t)(deg[i] * (int)(ANG45 / 45)), 32 * 64 * FRACUNIT);
        if (!linetarget) continue;
        const int p = pod_of(linetarget);
        if (p > 0 || (p == 0 && i == 0)) return p;
        if (p == 0) sim = 0;       /* to the side, the sim's demon gives way to any other */
    }
    return sim;
}

int hunt_shot(mobj_t* shooter) {
    const int p = aim_from(shooter);
    if (p < 0) return 0;
    P_DamageMobj(pods[p].mo, NULL, shooter, 10000);
    return 1;
}

void hunt_tick(int64_t now) {
    if (!hunt_on || gamestate != GS_LEVEL) return;
    player_t* pl = &players[consoleplayer];
    mobj_t* pmo = pl->mo;
    if (!pmo) return;
    pl->cheats |= CF_GODMODE;
    pl->ammo[am_clip] = 50;
    if (leveltime < last_leveltime) {     /* the map started over: its things are gone */
        for (int i = 0; i < PODS; i++) {
            if (pods[i].state == LIVE) { pods[i].state = WAITING; pods[i].mo = 0; }
        }
        memset(corpses, 0, sizeof corpses);
    }
    last_leveltime = leveltime;
    corpses_expire();
    wake_nearest(pmo);
    int spawned = 0;
    for (int i = 0; i < PODS; i++) {
        pod_t* p = &pods[i];
        switch (p->state) {
        case WAITING:
            if (i && now - p->seen_ms > SILENT_MS) p->state = FREE;
            else if (!spawned) spawned = spawn_demon(p, pmo) || 1;   /* one a tick: the search takes time */
            break;
        case LIVE:
            if (i && now - p->seen_ms > SILENT_MS) {    /* deleted from outside, or its node went away */
                P_SpawnMobj(p->mo->x, p->mo->y, p->mo->z, MT_TFOG);
                P_RemoveMobj(p->mo);
                p->mo = 0;
                p->state = FREE;
                note('g', p->name, 0, 0);
            }
            break;
        case DEAD:
            if (!p->sent_ms) {
                kills++;
                note('k', p->name, 0, 0);
                set_line("KILLED ", p, C_SELF, now + LINE_MS);
            }
            if (!p->acked && now - p->sent_ms >= RESEND_MS) {
                if (p->sent_ms) resent++;
                p->sent_ms = now;
                k32_event_emit("doom.kill", 9, p->name, (uint32_t)strlen(p->name));
            }
            if (i && p->acked && now - p->seen_ms > FORGET_MS) p->state = FREE;
            break;
        }
    }
}

void hunt_frame(int64_t now) {
    if (!hunt_on) return;
    static int framed;
    if (!framed++) note('f', pods[0].name, 0, 0);
    ntags = 0;
    aimed = -1;
    if (gamestate != GS_LEVEL || !players[displayplayer].mo) return;
    aimed = aim_from(players[displayplayer].mo);
    if (now >= line_until) {
        line[0] = 0;
        if (aimed >= 0) set_line("", &pods[aimed], pods[aimed].kind, 0);
    }
}

/* ---- what the view shows ----------------------------------------------- */

void hunt_project(const mobj_t* thing, const vissprite_t* vis) {
    if (ntags == TAGS) return;
    const int p = pod_of(thing);
    if (p < 0) return;
    const int x = (vis->x1 + vis->x2 + 1) << detailshift >> 1;
    const int y = centery - (FixedMul(vis->texturemid, vis->scale) >> FRACBITS);
    tags[ntags].vis = vis;
    tags[ntags].pod = (uint8_t)p;
    tags[ntags].x = (uint8_t)(x < 0 ? 0 : x > 239 ? 239 : x);
    tags[ntags].y = (uint8_t)(y < 0 ? 0 : y > 134 ? 134 : y);
    tags[ntags].seen = 0;
    ntags++;
}

/* A sprite behind a wall gets no tag: at least one of its columns must be open */
void hunt_sprite_drawn(const vissprite_t* vis, const short* floorclip, const short* ceilingclip) {
    for (unsigned t = 0; t < ntags; t++) {
        if (tags[t].vis != vis) continue;
        for (int x = vis->x1; x <= vis->x2; x++) {
            if (floorclip[x] > ceilingclip[x] + 1) { tags[t].seen = 1; break; }
        }
        return;
    }
}

/* Tags that would overlap stack upward; the sim's is laid out first and stays where it is */
static void tag_place(uint8_t* placed, unsigned n, uint8_t* tag) {
    for (int moved = 1; moved;) {
        moved = 0;
        for (unsigned i = 0; i < n; i++) {
            const uint8_t* o = placed + i * 3;     /* centre, top row, half the width */
            const int dx = o[0] > tag[0] ? o[0] - tag[0] : tag[0] - o[0], dy = o[1] > tag[1] ? o[1] - tag[1] : tag[1] - o[1];
            if (dx < o[2] + tag[2] && dy < 9 && tag[1] >= 9) { tag[1] -= 9; moved = 1; }
        }
    }
}

/* "zombie-58gs7cf6-d688d" tags as "zombie-d688d": the name less the template's hash */
static unsigned tag_text(const pod_t* p, char* out) {
    if (p->kind == C_SELF) { memcpy(out, "doom-sim", 8); return 8; }
    const unsigned n = (unsigned)strlen(p->name);
    const char* end = strrchr(p->name, '-');
    const char* mid = end;
    while (mid && mid > p->name && mid[-1] != '-') mid--;
    if (!end || !mid || mid == p->name) {
        unsigned m = n < 16 ? n : 16;
        memcpy(out, p->name, m);
        return m;
    }
    const unsigned a = (unsigned)(mid - p->name), b = (unsigned)(p->name + n - end - 1);
    if (a + b > 20) { memcpy(out, p->name, 16); return 16; }
    memcpy(out, p->name, a);
    memcpy(out + a, end + 1, b);
    return a + b;
}

uint32_t hunt_record(uint8_t* p) {
    if (!hunt_on) return 0;
    uint32_t n = 2;
    const unsigned ll = (unsigned)strlen(line);
    p[n++] = line_color;
    p[n++] = (uint8_t)ll;
    memcpy(p + n, line, ll);
    n += ll;
    const uint32_t at = n++;
    uint8_t placed[TAGS * 3];
    unsigned shown = 0;
    for (int pass = 0; pass < 2; pass++) {       /* the sim's tag first */
        for (unsigned t = 0; t < ntags && n + 4 + 20 <= HUNT_RECORD_MAX; t++) {
            if (!tags[t].seen || (tags[t].pod != 0) != (pass != 0)) continue;
            const pod_t* q = &pods[tags[t].pod];
            const unsigned m = tag_text(q, (char*)p + n + 4);
            uint8_t* pl = placed + shown * 3;
            pl[0] = tags[t].x;
            pl[1] = tags[t].y;
            pl[2] = (uint8_t)(m * 3);
            tag_place(placed, shown, pl);
            p[n] = pl[0];
            p[n + 1] = pl[1];
            p[n + 2] = q->kind;
            p[n + 3] = (uint8_t)m;
            n += 4 + m;
            shown++;
        }
    }
    p[at] = (uint8_t)shown;
    p[0] = 'P';
    p[1] = (uint8_t)n;
    return n;
}

/* ---- requests ---------------------------------------------------------- */

static int32_t reply(int32_t status, const char* type, const char* body, uint32_t len) {
    k32_resp_begin(status, type, (uint32_t)strlen(type));
    k32_resp_write(body, len);
    k32_resp_end();
    return status;
}

static const char refused[] =
    "<!doctype html><meta charset=utf-8><title>DOOM hunt</title>"
    "<p>The hunt is a single player's game: demons come and go with pods, so a second engine could not follow it. "
    "<code>kubectl apply -f doom.yaml</code> brings back the two-player game.</p>";

int32_t hunt_route(const char* method, const char* seg) {
    if (!hunt_on) return 0;
    if (!strcmp(seg, "join") || !strcmp(seg, "tics") || !strcmp(seg, "net")) {
        static const char body[] = "{\"error\":\"the hunt is a single player's game\"}";
        return reply(409, "application/json", body, sizeof body - 1);
    }
    if (!*seg || !strcmp(seg, "doom") || !strcmp(seg, "index.html")) {
        return reply(200, "text/html; charset=utf-8", refused, sizeof refused - 1);
    }
    if (strcmp(seg, "hunt")) return 0;
    static char b[3072];
    int n = snprintf(b, sizeof b, "{\"now_ms\":%lld,\"unix_ms\":%lld,\"gametic\":%d,\"kills\":%u,\"spawns\":%u,\"reaped\":%u,\"resent\":%u,\"ignored\":%u,\"aim\":\"%s\",\"pods\":[",
                     (long long)k32_time_ms(), (long long)k32_unix_time_ms(), gametic, (unsigned)kills, (unsigned)spawns, (unsigned)reaped,
                     (unsigned)resent, (unsigned)ignored, aimed >= 0 ? pods[aimed].name : "");
    const mobj_t* pmo = players[displayplayer].mo;
    int first = 1;
    for (int i = 0; i < PODS; i++) {
        const pod_t* p = &pods[i];
        if (p->state == FREE) continue;
        int bearing = 0, dist = 0;
        if (p->state == LIVE && p->mo && pmo) {
            const angle_t a = R_PointToAngle2(pmo->x, pmo->y, p->mo->x, p->mo->y) - pmo->angle;
            bearing = (int)((int64_t)(int32_t)a * 45 / (int64_t)ANG45);
            dist = P_AproxDistance(p->mo->x - pmo->x, p->mo->y - pmo->y) >> FRACBITS;
        }
        n += snprintf(b + n, sizeof b - (size_t)n, "%s{\"pod\":\"%s\",\"node\":\"%s\",\"kind\":%d,\"state\":%d,\"bearing\":%d,\"dist\":%d}",
                      first ? "" : ",", p->name, p->node, p->kind, p->state, bearing, dist);
        first = 0;
    }
    n += snprintf(b + n, sizeof b - (size_t)n, "],\"events\":[");
    const uint32_t from = events_next > LOG_EVENTS ? events_next - LOG_EVENTS : 0;
    for (uint32_t e = from; e < events_next; e++) {
        const unsigned i = e % LOG_EVENTS;
        n += snprintf(b + n, sizeof b - (size_t)n, "%s[%u,\"%c\",\"%s\",%lld,%d,%d]", e > from ? "," : "", (unsigned)e, events[i].what, events[i].name,
                      (long long)events[i].t, events[i].status, events[i].ms);
    }
    n += snprintf(b + n, sizeof b - (size_t)n, "]}");
    return reply(200, "application/json", b, (uint32_t)n);
}
