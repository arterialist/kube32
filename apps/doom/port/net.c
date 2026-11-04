// SPDX-License-Identifier: GPL-2.0-or-later
/* Two players in lockstep: a netgame (-lockstep 2) whose ticcmds travel
 * through kube32.
 *
 * The authority is the pod (env DOOM_PLAYERS=2). It plays player 1 from its
 * own keys and orders the ticcmds: it runs tic T once player 2's ticcmd for
 * T is in, building player 1's at that moment, and publishes both. A client,
 * such as the browser page in web/, runs the same engine from those
 * confirmed ticcmds alone and sends player 2's. Both sides run every tic
 * with the same ticcmds in the same order, so their game states agree tic by
 * tic (port/state_digest.c digests them; the two sides compare digests).
 *
 * A client builds its ticcmd for tic T + delay when it has run tic T, so a
 * ticcmd has `delay` tics to reach the authority. A late ticcmd holds the
 * authority's next tic, and its clock with it, until it arrives; a client
 * that sends nothing for DOOM_NET_TIMEOUT_MS leaves the game ("Player 2
 * left the game") from the next tic on, as in any netgame. Every POST
 * carries all of the client's ticcmds the authority has not confirmed, and
 * every answer the confirmed tics the client lacks, so a lost POST, a lost
 * or reordered event and a dropped event stream all cost time, never the
 * game state.
 *
 * Confirmed tics (authority to client), on the channel topic DOOM_NET_TOPIC
 * (an Ingress serves it as Server-Sent Events) and as the answer to POST
 * /tics:
 *   {"t":"tics","g":<session>,"s":<first tic>,"m":<players in the game, bits>,
 *    "c":"<hex: each tic, each player's ticcmd>"[,"d":<tic>,"h":"<digest>"]}
 * "d" and "h": the authority's state digest after tic d ran.
 *
 * Ticcmds (client to authority), the body of POST /tics:
 *   g=<session>&a=<first tic the client lacks>&s=<first tic>&c=<hex>[&d=<tic>&h=<digest>]
 *
 * A ticcmd on the wire is 7 bytes: forwardmove, sidemove, angleturn (little
 * endian), chatchar, buttons, consistancy.
 *
 *   GET  /net        the game: players, session, tic, and the arguments and
 *                    IWAD a client starts its engine with
 *   POST /join       a client takes player 2's place; the session restarts
 *                    the map when a game was running
 *   POST /tics       player 2's ticcmds; answers the confirmed tics from a=
 *   GET  /netstats   counters as JSON
 *   GET  /, /doom-net.js, /doom.wasm, /doom.wad
 *                    the browser client, from the blob DOOM_WEB_BLOB
 *                    (wad.py web)
 */
#include "include/k32libc.h"
#include "../../../include/runtime/k32_app_abi.h"

#include "../doomgeneric/doomdef.h"
#include "../doomgeneric/doomstat.h"
#include "../doomgeneric/d_loop.h"
#include "../doomgeneric/d_ticcmd.h"
#include "../doomgeneric/g_game.h"
#include "../doomgeneric/p_local.h"

#include "net.h"

uint32_t doom_state_digest(int32_t group);
extern byte consistancy[MAXPLAYERS][BACKUPTICS];
extern int iquetail;

#define NETP 2             /* players */
#define CMD_BYTES 7        /* a ticcmd on the wire */
#define HIST 256           /* confirmed tics kept (authority) and accepted ahead (client) */
#define AHEAD 128          /* player 2's ticcmds the authority holds ahead of the game */
#define EVENT_TICS 4       /* a channel message holds K32_MAX_EVENT_BYTES */
#define ANSWER_TICS 64     /* confirmed tics in one answer to POST /tics */

typedef struct {
    int32_t tic;           /* -1: empty */
    uint8_t mask;          /* players in the game */
    uint8_t has_digest;
    uint32_t digest;
    uint8_t cmd[NETP][CMD_BYTES];
} nettic_t;

static int role;           /* NET_SINGLE, NET_AUTHORITY, NET_CLIENT */
static int32_t session;
static int32_t session_start;
static int32_t digest_every = 35;
static nettic_t tics[HIST];
static char out_buf[4096];

/* ---- the wire ------------------------------------------------------------ */

static void cmd_pack(const ticcmd_t* c, uint8_t* b) {
    b[0] = (uint8_t)c->forwardmove;
    b[1] = (uint8_t)c->sidemove;
    b[2] = (uint8_t)(c->angleturn & 0xff);
    b[3] = (uint8_t)((uint16_t)c->angleturn >> 8);
    b[4] = c->chatchar;
    b[5] = c->buttons;
    b[6] = c->consistancy;
}
static void cmd_unpack(const uint8_t* b, ticcmd_t* c) {
    memset(c, 0, sizeof *c);
    c->forwardmove = (signed char)b[0];
    c->sidemove = (signed char)b[1];
    c->angleturn = (short)(uint16_t)(b[2] | (b[3] << 8));
    c->chatchar = b[4];
    c->buttons = b[5];
    c->consistancy = b[6];
}

static const char hexd[] = "0123456789abcdef";
static char* hex_put(char* p, const uint8_t* b, int n) {
    for (int i = 0; i < n; i++) { *p++ = hexd[b[i] >> 4]; *p++ = hexd[b[i] & 15]; }
    return p;
}
static int hex_nib(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
/* decodes up to n bytes; returns the bytes decoded */
static int hex_get(const char* p, int len, uint8_t* b, int n) {
    int k = 0;
    for (; k < n && 2 * k + 1 < len; k++) {
        int hi = hex_nib(p[2 * k]), lo = hex_nib(p[2 * k + 1]);
        if (hi < 0 || lo < 0) break;
        b[k] = (uint8_t)(hi << 4 | lo);
    }
    return k;
}

/* "key":<int> in a JSON message, or key=<int> in a form body */
static int find_key(const char* msg, const char* key, int json, const char** at) {
    char pat[16];
    size_t k = strlen(key);
    if (json) { pat[0] = '"'; memcpy(pat + 1, key, k); pat[k + 1] = '"'; pat[k + 2] = ':'; pat[k + 3] = 0; }
    else { memcpy(pat, key, k); pat[k] = '='; pat[k + 1] = 0; }
    for (const char* p = msg; (p = strstr(p, pat)); p++) {
        if (!json && p != msg && p[-1] != '&' && p[-1] != '\n') continue;
        *at = p + strlen(pat);
        return 1;
    }
    return 0;
}
static long get_int(const char* msg, const char* key, int json, long def) {
    const char* at;
    return find_key(msg, key, json, &at) ? strtol(at, 0, 10) : def;
}
/* a hex string value: its start and length */
static int get_hex(const char* msg, const char* key, int json, const char** start) {
    const char* at;
    if (!find_key(msg, key, json, &at)) return 0;
    if (json && *at == '"') at++;
    *start = at;
    int n = 0;
    while (hex_nib(at[n]) >= 0) n++;
    return n;
}

/* {"t":"tics",...}: the confirmed tics from..to-1 of the ring */
static int tics_message(char* out, int cap, int32_t from, int32_t to) {
    char* p = out;
    uint8_t mask = from < to ? tics[from % HIST].mask : 0;
    int32_t dtic = -1;
    uint32_t digest = 0;
    p += snprintf(p, (size_t)cap, "{\"t\":\"tics\",\"g\":%d,\"s\":%d,\"m\":%u,\"c\":\"", (int)session, (int)from, (unsigned)mask);
    for (int32_t t = from; t < to; t++) {
        const nettic_t* e = &tics[t % HIST];
        if (p - out + 2 * NETP * CMD_BYTES + 48 > cap) break;
        for (int i = 0; i < NETP; i++) p = hex_put(p, e->cmd[i], CMD_BYTES);
        if (e->has_digest) { dtic = t; digest = e->digest; }
    }
    p += snprintf(p, (size_t)(cap - (p - out)), "\"");
    if (dtic >= 0) p += snprintf(p, (size_t)(cap - (p - out)), ",\"d\":%d,\"h\":\"%08x\"", (int)dtic, (unsigned)digest);
    p += snprintf(p, (size_t)(cap - (p - out)), "}");
    return (int)(p - out);
}

/* ---- the authority ------------------------------------------------------- */

static struct { int32_t tic; uint8_t cmd[CMD_BYTES]; } remote[AHEAD];
static int phase;                       /* 0: waiting for player 2, 1: playing */
static int p2_in;                       /* player 2 is in the game */
static int64_t p2_heard_ms;
static int64_t clock_ms;                /* tic clock_tic is due at clock_ms */
static int32_t clock_tic;
static int32_t emitted;                 /* the first tic not yet published */
static int32_t batch = 2, timeout_ms = 5000, delay = 4;
static char topic[64] = "doom.tics", game_args[256], game_iwad[48], web_blob[48] = "doom-web";
static int32_t stall_tic = -1, checked_tic = -1;
static int64_t stall_since;
static struct {
    uint32_t tics, stalled_tics, stall_ms, longest_stall_ms, posts, cmds, cmds_old, events, event_errors, joins, drops;
    uint32_t digests_checked, digests_bad, lead_sum, lead_n, lead_min;
    int32_t first_bad_tic;
} st = { .lead_min = 0xffffffffu, .first_bad_tic = -1 };

static int32_t env_int(const char* key, int32_t def) {
    char b[16];
    int32_t n = k32_env_get(key, (uint32_t)strlen(key), b, sizeof b - 1);
    if (n <= 0 || n >= (int32_t)sizeof b) return def;
    b[n] = 0;
    return (int32_t)strtol(b, 0, 10);
}

int net_role(void) { return role; }
/* An authority with a session: its tics wait for player 2's ticcmds (net_tick); before the
 * first join player 1 plays alone, as a single player does */
int net_active(void) { return role == NET_AUTHORITY && phase != 0; }

void net_init_authority(const char* args, const char* iwad) {
    role = NET_AUTHORITY;
    strncpy(game_args, args, sizeof game_args - 1);
    strncpy(game_iwad, iwad, sizeof game_iwad - 1);
    int32_t n = k32_env_get("DOOM_NET_TOPIC", 14, topic, sizeof topic - 1);
    if (n > 0 && n < (int32_t)sizeof topic) topic[n] = 0;
    else strcpy(topic, "doom.tics");
    n = k32_env_get("DOOM_WEB_BLOB", 13, web_blob, sizeof web_blob - 1);
    if (n > 0 && n < (int32_t)sizeof web_blob) web_blob[n] = 0;
    else strcpy(web_blob, "doom-web");
    batch = env_int("DOOM_NET_BATCH", 2);
    if (batch < 1) batch = 1;
    if (batch > EVENT_TICS) batch = EVENT_TICS;
    timeout_ms = env_int("DOOM_NET_TIMEOUT_MS", 5000);
    delay = env_int("DOOM_NET_DELAY", 4);
    digest_every = env_int("DOOM_NET_DIGEST", 35);
    for (int i = 0; i < HIST; i++) tics[i].tic = -1;
    for (int i = 0; i < AHEAD; i++) remote[i].tic = -1;
}

/* A new session restarts the map: the state a client's engine starts in */
static void restart_map(void) {
    memset(consistancy, 0, sizeof consistancy);
    iquehead = iquetail = 0;
    for (int i = 0; i < MAXPLAYERS; i++) playeringame[i] = i < NETP;
    G_InitNew(startskill, startepisode, startmap);
}

static void publish(int32_t from, int32_t to) {
    char msg[K32_MAX_EVENT_BYTES + 1];
    while (from < to) {
        int32_t n = to - from > EVENT_TICS ? EVENT_TICS : to - from;
        /* a message holds tics of one mask */
        for (int32_t k = 1; k < n; k++) if (tics[(from + k) % HIST].mask != tics[from % HIST].mask) { n = k; break; }
        int len = tics_message(msg, sizeof msg, from, from + n);
        if (k32_event_emit(topic, (uint32_t)strlen(topic), msg, (uint32_t)len) < 0) st.event_errors++;
        else st.events++;
        from += n;
    }
}

int net_tick(int64_t now) {
    if (role != NET_AUTHORITY || !phase) return 0;
    int ran = 0, stalled = 0;
    int32_t due = clock_tic + (int32_t)((now - clock_ms) * TICRATE / 1000) + 1;   /* tics due by now */
    while (gametic < due && ran < 4) {
        const int32_t t = gametic;
        ticcmd_t cmds[NET_MAXPLAYERS];
        boolean ingame[NET_MAXPLAYERS];
        memset(cmds, 0, sizeof cmds);
        memset(ingame, 0, sizeof ingame);
        ingame[0] = true;
        if (p2_in) {
            if (remote[t % AHEAD].tic != t) {
                if (now - p2_heard_ms <= timeout_ms) { stalled = 1; break; }
                p2_in = 0;              /* player 2 leaves the game from this tic on */
                st.drops++;
                printf("doom-net: player 2 sent nothing for %d ms; leaves the game at tic %d\n", (int)timeout_ms, (int)t);
            } else {
                cmd_unpack(remote[t % AHEAD].cmd, &cmds[1]);
                ingame[1] = true;
            }
        }
        D_BuildLocalTiccmd(&cmds[0], t);
        nettic_t* e = &tics[t % HIST];
        e->tic = t;
        e->mask = (uint8_t)(1 | (ingame[1] ? 2 : 0));
        for (int i = 0; i < NETP; i++) cmd_pack(&cmds[i], e->cmd[i]);
        D_RunNetTic(cmds, ingame);
        e->has_digest = digest_every > 0 && (t + 1) % digest_every == 0;
        if (e->has_digest) e->digest = doom_state_digest(0);
        ran++;
    }
    if (stalled) {
        if (stall_tic != gametic) { stall_tic = gametic; st.stalled_tics++; }
        if (!stall_since) stall_since = now;
        clock_tic = gametic;            /* the waiting tic stays due; the clock waits with it */
        clock_ms = now;
    } else {
        if (stall_since) {
            uint32_t ms = (uint32_t)(now - stall_since);
            if (ms >= 250) printf("doom-net: tic %d waited %u ms for player 2's ticcmd\n", (int)stall_tic, (unsigned)ms);
            st.stall_ms += ms;
            if (ms > st.longest_stall_ms) st.longest_stall_ms = ms;
            stall_since = 0;
        }
        if (due - gametic > 4) { clock_tic = gametic; clock_ms = now; }   /* the node was busy: drop the time */
    }
    st.tics += (uint32_t)ran;
    if (gametic - emitted >= batch || (stalled && gametic > emitted)) {
        int32_t to = stalled ? gametic : emitted + (gametic - emitted) / batch * batch;
        publish(emitted, to);
        emitted = to;
    }
    return ran;
}

/* Before a browser joins, player 1 plays alone: the tics due by the clock, each with
 * player 1's ticcmd only, nothing published; a join restarts the map */
static int64_t alone_ms;
static int32_t alone_tic;
int net_tick_alone(int64_t now) {
    if (role != NET_AUTHORITY || phase) return 0;
    if (!alone_ms) { alone_ms = now; alone_tic = gametic; }
    int32_t due = alone_tic + (int32_t)((now - alone_ms) * TICRATE / 1000);
    if (due - gametic > 4) { alone_tic = gametic; alone_ms = now; due = gametic + 4; }   /* the node was busy: drop the time */
    int ran = 0;
    while (gametic < due && ran < 4) {
        ticcmd_t cmds[NET_MAXPLAYERS];
        boolean ingame[NET_MAXPLAYERS];
        memset(cmds, 0, sizeof cmds);
        memset(ingame, 0, sizeof ingame);
        ingame[0] = true;
        D_BuildLocalTiccmd(&cmds[0], gametic);
        D_RunNetTic(cmds, ingame);
        ran++;
    }
    return ran;
}

static int32_t join(int64_t now) {
    if (phase || gametic > 0) restart_map();
    session++;
    session_start = gametic;
    phase = 1;
    p2_in = 1;
    p2_heard_ms = now;
    clock_tic = gametic;
    clock_ms = now;
    emitted = gametic;
    stall_tic = checked_tic = -1;
    stall_since = 0;
    for (int i = 0; i < AHEAD; i++) remote[i].tic = -1;
    st.joins++;
    printf("doom-net: player 2 joins, session %d from tic %d\n", (int)session, (int)gametic);
    return session;
}

/* POST /tics: stores player 2's ticcmds, answers the confirmed tics from a= */
static int handle_tics(const char* body, int64_t now, int* status) {
    *status = 200;
    if (!phase || get_int(body, "g", 0, -1) != session) {
        *status = 409;
        return snprintf(out_buf, sizeof out_buf, "{\"t\":\"session\",\"g\":%d}", (int)session);
    }
    st.posts++;
    p2_heard_ms = now;
    const char* hex;
    int32_t s = (int32_t)get_int(body, "s", 0, -1);
    int n = get_hex(body, "c", 0, &hex) / (2 * CMD_BYTES);
    for (int i = 0; i < n && s >= 0; i++) {
        const int32_t t = s + i;
        if (t < gametic || !p2_in) { st.cmds_old++; continue; }
        if (t >= gametic + AHEAD) break;
        if (remote[t % AHEAD].tic == t) continue;
        remote[t % AHEAD].tic = t;
        hex_get(hex + 2 * CMD_BYTES * i, 2 * CMD_BYTES, remote[t % AHEAD].cmd, CMD_BYTES);
        st.cmds++;
        const uint32_t lead = (uint32_t)(t - gametic);
        st.lead_sum += lead;
        st.lead_n++;
        if (lead < st.lead_min) st.lead_min = lead;
    }
    if (stall_since) net_tick(now);     /* the tic waiting for these runs now, not at the next tick */
    const int32_t dtic = (int32_t)get_int(body, "d", 0, -1);
    const char* dh;
    if (dtic >= session_start && dtic > checked_tic && get_hex(body, "h", 0, &dh) == 8) {
        const nettic_t* e = &tics[dtic % HIST];
        if (e->tic == dtic && e->has_digest) {
            uint8_t b[4];
            hex_get(dh, 8, b, 4);
            const uint32_t d = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
            checked_tic = dtic;
            st.digests_checked++;
            if (d != e->digest) {
                if (!st.digests_bad++) st.first_bad_tic = dtic;
                printf("doom-net: the states differ at tic %d (%08x here, %08x on player 2's side)\n", (int)dtic, (unsigned)e->digest, (unsigned)d);
            }
        }
    }
    int32_t a = (int32_t)get_int(body, "a", 0, gametic);
    if (a < session_start || a < gametic - HIST + 1) {
        *status = 410;
        return snprintf(out_buf, sizeof out_buf, "{\"t\":\"lost\",\"g\":%d,\"s\":%d}", (int)session, (int)gametic);
    }
    int32_t to = gametic;
    if (to > a + ANSWER_TICS) to = a + ANSWER_TICS;
    if (a > to) a = to;
    /* one mask an answer */
    for (int32_t t = a + 1; t < to; t++) if (tics[t % HIST].mask != tics[a % HIST].mask) { to = t; break; }
    return tics_message(out_buf, sizeof out_buf, a, to);
}

static int stats_json(char* out, int cap) {
    return snprintf(out, (size_t)cap,
                    "{\"role\":\"%s\",\"session\":%d,\"playing\":%d,\"player2\":%d,\"tic\":%d,\"tics\":%u,\"stalled_tics\":%u,\"stall_ms\":%u,"
                    "\"longest_stall_ms\":%u,\"posts\":%u,\"cmds\":%u,\"cmds_old\":%u,\"lead_mean_x100\":%u,\"lead_min\":%d,\"events\":%u,"
                    "\"event_errors\":%u,\"joins\":%u,\"drops\":%u,\"delay\":%d,\"batch\":%d,\"digests_checked\":%u,\"digests_bad\":%u,"
                    "\"first_bad_tic\":%d,\"digest\":\"%08x\"}",
                    role == NET_AUTHORITY ? "authority" : role == NET_CLIENT ? "client" : "single", (int)session, phase, p2_in,
                    (int)gametic, st.tics, st.stalled_tics, st.stall_ms, st.longest_stall_ms, st.posts, st.cmds, st.cmds_old,
                    st.lead_n ? st.lead_sum * 100 / st.lead_n : 0, st.lead_n ? (int)st.lead_min : -1, st.events, st.event_errors, st.joins,
                    st.drops, (int)delay, (int)batch, st.digests_checked, st.digests_bad, (int)st.first_bad_tic, (unsigned)doom_state_digest(0));
}

static int32_t reply(int32_t status, const char* type, const void* body, uint32_t len) {
    k32_resp_header("Cache-Control", 13, "no-store", 8);
    k32_resp_begin(status, type, (uint32_t)strlen(type));
    if (len) k32_resp_write(body, len);
    k32_resp_end();
    return status;
}

/* The browser client: a lump of the PWAD in the blob DOOM_WEB_BLOB (wad.py web) */
static int32_t serve_web(const char* lump, const char* type) {
    int32_t h = k32_blob_open(web_blob, (uint32_t)strlen(web_blob));
    if (h < 0) return 0;
    uint8_t head[12];
    int32_t status = 0;
    if (k32_blob_read(h, 0, head, 12) == 12 && !memcmp(head, "PWAD", 4)) {
        const uint32_t n = head[4] | head[5] << 8 | head[6] << 16 | (uint32_t)head[7] << 24;
        const uint32_t dir = head[8] | head[9] << 8 | head[10] << 16 | (uint32_t)head[11] << 24;
        for (uint32_t i = 0; i < n && i < 16; i++) {
            uint8_t e[16];
            if (k32_blob_read(h, dir + 16 * i, e, 16) != 16) break;
            if (strncmp((const char*)e + 8, lump, 8)) continue;
            uint32_t pos = e[0] | e[1] << 8 | e[2] << 16 | (uint32_t)e[3] << 24;
            uint32_t left = e[4] | e[5] << 8 | e[6] << 16 | (uint32_t)e[7] << 24;
            k32_resp_header("Cache-Control", 13, "no-cache", 8);
            k32_resp_begin(200, type, (uint32_t)strlen(type));
            while (left) {
                uint32_t want = left < sizeof out_buf ? left : sizeof out_buf;
                int32_t got = k32_blob_read(h, pos, out_buf, want);
                if (got <= 0) break;
                k32_resp_write(out_buf, (uint32_t)got);
                pos += (uint32_t)got;
                left -= (uint32_t)got;
            }
            k32_resp_end();
            status = 200;
            break;
        }
    }
    k32_blob_close(h);
    return status;
}

int32_t net_route(const char* method, const char* seg, const char* body) {
    if (role != NET_AUTHORITY) return 0;
    const int64_t now = k32_time_ms();
    int status;
    if (!strcmp(seg, "tics") && !strcmp(method, "POST")) {
        int len = handle_tics(body, now, &status);
        return reply(status, "application/json", out_buf, (uint32_t)len);
    }
    if (!strcmp(seg, "join") && !strcmp(method, "POST")) {
        join(now);
        int len = snprintf(out_buf, sizeof out_buf, "{\"t\":\"join\",\"g\":%d,\"s\":%d,\"player\":1,\"delay\":%d,\"digest\":%d}",
                           (int)session, (int)session_start, (int)delay, (int)digest_every);
        return reply(200, "application/json", out_buf, (uint32_t)len);
    }
    if (!strcmp(seg, "net")) {
        int len = snprintf(out_buf, sizeof out_buf,
                           "{\"players\":%d,\"session\":%d,\"playing\":%d,\"player2\":%d,\"tic\":%d,\"iwad\":\"%s\",\"args\":\"%s\","
                           "\"delay\":%d,\"digest\":%d,\"timeout_ms\":%d}",
                           NETP, (int)session, phase, p2_in, (int)gametic, game_iwad, game_args, (int)delay, (int)digest_every, (int)timeout_ms);
        return reply(200, "application/json", out_buf, (uint32_t)len);
    }
    if (!strcmp(seg, "netstats")) {
        int len = stats_json(out_buf, sizeof out_buf);
        return reply(200, "application/json", out_buf, (uint32_t)len);
    }
    if (!*seg || !strcmp(seg, "doom") || !strcmp(seg, "index.html")) return serve_web("INDEX", "text/html; charset=utf-8");
    if (!strcmp(seg, "doom-net.js")) return serve_web("DOOMJS", "text/javascript");
    if (!strcmp(seg, "doom.wasm")) return serve_web("DOOMWASM", "application/wasm");
    if (!strcmp(seg, "doom.wad")) return serve_web("IWAD", "application/octet-stream");
    return 0;
}

/* The next tic to run */
K32_EXPORT("doom_net_tic") int32_t doom_net_tic(void) { return gametic; }

/* ---- the client (the full engine; the sim-only pod is always the authority) ---- */
#ifndef DG_SIM_ONLY

static char in_buf[4096];
static uint8_t own[AHEAD][CMD_BYTES];
static int32_t own_tic[AHEAD];
static int32_t make_tic;                /* the next ticcmd this side builds */
static int32_t recv_hi;                 /* one past the highest confirmed tic received */
static uint32_t fresh;                  /* ticcmds built since the last POST */
static struct { int32_t tic; uint32_t digest; } theirs[16], mine[16];
static int32_t my_digest_tic = -1;
static uint32_t my_digest;
static struct { uint32_t run, messages, tics_in, tics_dup, checked, bad; int32_t first_bad; } cs = { .first_bad = -1 };

K32_EXPORT("doom_net_in") uint32_t doom_net_in(void) { return (uint32_t)(uintptr_t)in_buf; }
K32_EXPORT("doom_net_out") uint32_t doom_net_out(void) { return (uint32_t)(uintptr_t)out_buf; }

static void compare(int32_t tic) {
    const int a = tic & 15;
    if (theirs[a].tic != tic || mine[a].tic != tic) return;
    cs.checked++;
    if (theirs[a].digest != mine[a].digest) {
        if (!cs.bad++) cs.first_bad = tic;
        printf("doom-net: the states differ at tic %d (%08x here, %08x on the authority)\n", (int)tic, (unsigned)mine[a].digest, (unsigned)theirs[a].digest);
    }
    theirs[a].tic = mine[a].tic = -1;
}

static void build_ahead(void) {
    while (make_tic < gametic + delay && make_tic - gametic < AHEAD) {
        ticcmd_t c;
        D_BuildLocalTiccmd(&c, make_tic);
        cmd_pack(&c, own[make_tic % AHEAD]);
        own_tic[make_tic % AHEAD] = make_tic;
        make_tic++;
        fresh++;
    }
}

/* Joins session g, whose first tic is s, with an input delay of d tics and
 * a state digest every e tics; call it once, after k32_init */
K32_EXPORT("doom_net_join") int32_t doom_net_join(int32_t g, int32_t s, int32_t d, int32_t e) {
    role = NET_CLIENT;
    session = g;
    session_start = s;
    delay = d < 1 ? 1 : d > 64 ? 64 : d;
    digest_every = e;
    gametic = s;
    make_tic = s;
    recv_hi = s;
    for (int i = 0; i < HIST; i++) tics[i].tic = -1;
    for (int i = 0; i < AHEAD; i++) own_tic[i] = -1;
    for (int i = 0; i < 16; i++) theirs[i].tic = mine[i].tic = -1;
    build_ahead();
    return 0;
}

/* A message from the authority in doom_net_in: 0, or -2 when the session
 * is gone (the client joins again), -1 for a message it cannot read */
K32_EXPORT("doom_net_recv") int32_t doom_net_recv(int32_t len) {
    if (len < 0 || len >= (int32_t)sizeof in_buf) return -1;
    in_buf[len] = 0;
    cs.messages++;
    if (strstr(in_buf, "\"t\":\"session\"") || strstr(in_buf, "\"t\":\"lost\"")) return -2;
    if (!strstr(in_buf, "\"t\":\"tics\"")) return -1;
    if (get_int(in_buf, "g", 1, -1) != session) return get_int(in_buf, "g", 1, -1) > session ? -2 : 0;
    const int32_t s = (int32_t)get_int(in_buf, "s", 1, -1);
    const uint8_t mask = (uint8_t)get_int(in_buf, "m", 1, 1);
    const char* hex;
    const int n = get_hex(in_buf, "c", 1, &hex) / (2 * NETP * CMD_BYTES);
    if (s < 0) return -1;
    for (int i = 0; i < n; i++) {
        const int32_t t = s + i;
        if (t < gametic || t >= gametic + HIST) { cs.tics_dup++; continue; }
        nettic_t* e = &tics[t % HIST];
        if (e->tic == t) { cs.tics_dup++; continue; }
        e->tic = t;
        e->mask = mask;
        for (int p = 0; p < NETP; p++) hex_get(hex + 2 * CMD_BYTES * (NETP * i + p), 2 * CMD_BYTES, e->cmd[p], CMD_BYTES);
        cs.tics_in++;
        if (t + 1 > recv_hi) recv_hi = t + 1;
    }
    const int32_t d = (int32_t)get_int(in_buf, "d", 1, -1);
    const char* dh;
    if (d >= 0 && get_hex(in_buf, "h", 1, &dh) == 8) {
        uint8_t b[4];
        hex_get(dh, 8, b, 4);
        theirs[d & 15].tic = d;
        theirs[d & 15].digest = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
        compare(d);
    }
    return 0;
}

/* Runs up to max confirmed tics in order; returns the tics run */
K32_EXPORT("doom_net_run") int32_t doom_net_run(int32_t max) {
    if (role != NET_CLIENT) return 0;
    int32_t n = 0;
    while (n < max) {
        const nettic_t* e = &tics[gametic % HIST];
        if (e->tic != gametic) break;
        ticcmd_t cmds[NET_MAXPLAYERS];
        boolean ingame[NET_MAXPLAYERS];
        memset(cmds, 0, sizeof cmds);
        memset(ingame, 0, sizeof ingame);
        for (int p = 0; p < NETP; p++) {
            cmd_unpack(e->cmd[p], &cmds[p]);
            ingame[p] = (e->mask >> p) & 1;
        }
        const int32_t t = gametic;
        D_RunNetTic(cmds, ingame);
        n++;
        if (digest_every > 0 && (t + 1) % digest_every == 0) {
            my_digest_tic = t;
            my_digest = doom_state_digest(0);
            mine[t & 15].tic = t;
            mine[t & 15].digest = my_digest;
            compare(t);
        }
        build_ahead();
    }
    cs.run += (uint32_t)n;
    return n;
}

/* The body of the next POST /tics into doom_net_out; returns its length */
K32_EXPORT("doom_net_post") int32_t doom_net_post(void) {
    int32_t lack = gametic;
    while (lack < gametic + HIST && tics[lack % HIST].tic == lack) lack++;
    int32_t s = recv_hi > make_tic ? make_tic : recv_hi;
    char* p = out_buf;
    p += snprintf(p, sizeof out_buf, "g=%d&a=%d&s=%d&c=", (int)session, (int)lack, (int)s);
    for (int32_t t = s; t < make_tic && p - out_buf + 2 * CMD_BYTES + 40 < (int)sizeof out_buf; t++)
        if (own_tic[t % AHEAD] == t) p = hex_put(p, own[t % AHEAD], CMD_BYTES);
    if (my_digest_tic >= 0) p += snprintf(p, sizeof out_buf - (size_t)(p - out_buf), "&d=%d&h=%08x", (int)my_digest_tic, (unsigned)my_digest);
    fresh = 0;
    return (int32_t)(p - out_buf);
}

/* Ticcmds built since the last doom_net_post */
K32_EXPORT("doom_net_fresh") int32_t doom_net_fresh(void) { return (int32_t)fresh; }

/* Counters as JSON into doom_net_out; returns the length */
K32_EXPORT("doom_net_stats") int32_t doom_net_stats(void) {
    if (role != NET_CLIENT) return stats_json(out_buf, sizeof out_buf);
    const nettic_t* last = &tics[(gametic + HIST - 1) % HIST];
    return snprintf(out_buf, sizeof out_buf,
                    "{\"role\":\"client\",\"session\":%d,\"tic\":%d,\"received\":%d,\"ahead\":%d,\"in_game\":%d,\"run\":%u,\"messages\":%u,"
                    "\"tics_in\":%u,\"tics_dup\":%u,\"digests_checked\":%u,\"digests_bad\":%u,\"first_bad_tic\":%d,\"delay\":%d,\"digest\":\"%08x\"}",
                    (int)session, (int)gametic, (int)recv_hi, (int)(make_tic - gametic),
                    last->tic == gametic - 1 ? (last->mask >> 1) & 1 : 1, cs.run, cs.messages, cs.tics_in, cs.tics_dup, cs.checked, cs.bad,
                    (int)cs.first_bad, (int)delay, (unsigned)doom_state_digest(0));
}

#endif /* DG_SIM_ONLY */
