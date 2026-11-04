// SPDX-License-Identifier: GPL-2.0-or-later
/* DOOM as a kube32 pod: the doomgeneric platform layer.
 *
 * k32_init starts the engine with the IWAD from a blob (env DOOM_IWAD names
 * the IWAD the engine looks for, DOOM_WAD_BLOB the blob that holds it,
 * DOOM_ARGS adds command line arguments such as "-warp 1 1 -skill 3").
 * k32_tick runs the game tics that are due at 35 a second and draws one
 * frame; it returns 1 so the node calls it again 10 ms later.
 *
 *   GET  /            a page that shows the game and sends keys
 *   GET  /frame       768 bytes of palette (RGB), then 320x200 palette
 *                     indices; header X-Doom-Frame counts the frames drawn
 *   POST /key         body: lines "d <code>" (pressed) or "u <code>"
 *                     (released), Doom key codes (doomkeys.h)
 *   GET  /stats       counters as JSON
 *   GET  /bench?tics=<n>&frames=<n>
 *                     runs n tics without drawing, then draws n frames of
 *                     the same game state; the microseconds of each part
 *
 * A path matches by its last segment, so the pod also answers under an
 * Ingress prefix (/doom/frame).
 *
 * With env DOOM_PLAYERS=2 the pod plays player 1 of a two-player netgame
 * and orders both players' ticcmds (port/net.c): it waits for player 2 to
 * join, then runs each tic once player 2's ticcmd for it is in. */
#include "include/k32libc.h"
#include "../../../include/runtime/k32_app_abi.h"

#include "../doomgeneric/doomgeneric.h"
#include "../doomgeneric/doomkeys.h"
#include "../doomgeneric/d_loop.h"
#include "../doomgeneric/i_video.h"
#include "../doomgeneric/doomstat.h"
#include "net.h"

K32_IMPORT("esp32_get_time_us") int64_t esp32_get_time_us(void);
#ifndef K32_MAX_MESSAGE_BYTES
K32_IMPORT("k32_publish") int32_t k32_publish(const char* topic, uint32_t topic_len, const void* data, uint32_t len);
#endif

void D_Display(void);
void TryRunTics(void);
void I_StartFrame(void);
void Z_UsageBytes(unsigned int* fixed, unsigned int* cache);
int Z_FreeMemory(void);
extern unsigned int z_purges, z_purged_bytes;
extern int screenblocks;
extern uint32_t k32_libc_heap_peak, k32_libc_blob_reads, k32_libc_blob_bytes;

/* ---- doomgeneric callbacks --------------------------------------------- */

#define KEYQ 64
static uint16_t keyq[KEYQ];
static unsigned keyq_head, keyq_tail;
static uint32_t frames_drawn;
static int64_t clock_offset_ms;   /* the bench moves the game clock along */

static void key_push(int pressed, unsigned code) {
    unsigned next = (keyq_tail + 1) % KEYQ;
    if (next == keyq_head || code == 0 || code > 255) return;
    keyq[keyq_tail] = (uint16_t)((pressed ? 0x100 : 0) | code);
    keyq_tail = next;
}

void DG_Init(void) {}
void DG_DrawFrame(void) { frames_drawn++; }
void DG_SleepMs(uint32_t ms) { (void)ms; }
uint32_t DG_GetTicksMs(void) { return (uint32_t)(k32_time_ms() + clock_offset_ms); }
void DG_SetWindowTitle(const char* title) { (void)title; }
int DG_GetKey(int* pressed, unsigned char* key) {
    if (keyq_head == keyq_tail) return 0;
    uint16_t e = keyq[keyq_head];
    keyq_head = (keyq_head + 1) % KEYQ;
    *pressed = (e >> 8) & 1;
    *key = (unsigned char)e;
    return 1;
}

/* ---- the game loop ------------------------------------------------------ */

static int started;
static uint32_t tics_run;
static int64_t start_ms;

/* One game tic: input, then the game state advances by 1/35 s */
K32_EXPORT("doom_run_tic") void doom_run_tic(void) {
    I_StartFrame();
    TryRunTics();
    tics_run++;
}

/* One frame of the current game state */
#ifdef DG_SIM_ONLY
void doom_render_view(void);
K32_EXPORT("doom_render") void doom_render(void) { doom_render_view(); }
#else
K32_EXPORT("doom_render") void doom_render(void) { D_Display(); }
#endif

#ifndef DG_SIM_ONLY
/* The frame the engine drew, palette indices, SCREENWIDTH x SCREENHEIGHT (a host check reads it) */
K32_EXPORT("doom_screen_ptr") uint32_t doom_screen_ptr(void) { return (uint32_t)(uintptr_t)I_VideoBuffer; }
/* The palette the frame uses, 256 RGB triplets */
K32_EXPORT("doom_palette") uint32_t doom_palette(void) {
    static uint8_t pal[768];
    for (int i = 0; i < 256; i++) {
        pal[i * 3] = colors[i].r;
        pal[i * 3 + 1] = colors[i].g;
        pal[i * 3 + 2] = colors[i].b;
    }
    return (uint32_t)(uintptr_t)pal;
}
#endif

/* The 3D view alone (no status bar, HUD or menu): what a split renderer ships */
void R_RenderPlayerView(player_t* player);
void R_ExecuteSetViewSize(void);
extern boolean setsizeneeded;
#ifdef DG_SIM_ONLY
void DG_DrawListBegin(void);
void DG_DrawListEnd(void);
uint32_t doom_drawlist_ptr(void);
uint32_t doom_drawlist_len(void);
#endif
K32_EXPORT("doom_render_view") void doom_render_view(void) {
    if (setsizeneeded) R_ExecuteSetViewSize();
#ifdef DG_SIM_ONLY
    DG_DrawListBegin();
    if (gamestate == GS_LEVEL) R_RenderPlayerView(&players[displayplayer]);
    DG_DrawListEnd();
    frames_drawn++;
#else
    R_RenderPlayerView(&players[displayplayer]);
#endif
}

/* For a host that drives the pod without HTTP: a key event */
K32_EXPORT("doom_key") void doom_key(int32_t pressed, int32_t code) { key_push(pressed, (unsigned)code); }

#ifdef DG_SIM_ONLY
static void sim_env(void);
#endif
static char* argv_store[24];
static char args_buf[256];

K32_EXPORT("k32_init") int32_t k32_init(void) {
    static char iwad[48];
    int argc = 0;
    argv_store[argc++] = "doom";
    int32_t n = k32_env_get("DOOM_IWAD", 9, iwad, sizeof iwad - 1);
    if (n <= 0 || n >= (int32_t)sizeof iwad) strcpy(iwad, "freedoom1.wad");
    else iwad[n] = 0;
    argv_store[argc++] = "-iwad";
    argv_store[argc++] = iwad;
    static char client_args[256];   /* DOOM_ARGS less the zone size: what a client's engine starts with */
    n = k32_env_get("DOOM_ARGS", 9, args_buf, sizeof args_buf - 1);
    if (n > 0 && n < (int32_t)sizeof args_buf) {
        args_buf[n] = 0;
        char* p = args_buf;
        while (*p && argc < 21) {
            while (*p == ' ') *p++ = 0;
            if (!*p) break;
            argv_store[argc++] = p;
            while (*p && *p != ' ') p++;
        }
    }
    for (int i = 3, skip = 0; i < argc; i++) {
        if (skip) { skip = 0; continue; }
        if (!strcmp(argv_store[i], "-kb")) { skip = 1; continue; }
        if (client_args[0]) strcat(client_args, " ");
        strcat(client_args, argv_store[i]);
    }
    int32_t players = 1;
    {
        char b[8];
        int32_t m = k32_env_get("DOOM_PLAYERS", 12, b, sizeof b - 1);
        if (m > 0 && m < (int32_t)sizeof b) { b[m] = 0; players = atoi(b); }
    }
    if (players == 2) {             /* player 1 of a lockstep netgame (port/net.c) */
        argv_store[argc++] = "-lockstep";
        argv_store[argc++] = "2";
    }
    argv_store[argc] = 0;
    singletics = true;          /* every TryRunTics runs exactly one tic */
#ifdef DG_SIM_ONLY
    sim_env();
#endif
    {   /* DOOM_SCREENBLOCKS: 10 leaves room for the status bar, 11 is the whole screen */
        char sb[8];
        int32_t m = k32_env_get("DOOM_SCREENBLOCKS", 17, sb, sizeof sb - 1);
        if (m > 0 && m < (int32_t)sizeof sb) { sb[m] = 0; screenblocks = atoi(sb); }
    }
    doomgeneric_Create(argc, argv_store);
    if (players == 2) net_init_authority(client_args, iwad);
    started = 1;
    start_ms = k32_time_ms();
    return 0;
}

#ifdef DG_SIM_ONLY
/* The sim publishes a frame's draw list whole on a channel topic (DOOM_DRAW_TOPIC,
 * default doom.draw) at DOOM_FPS frames a second (default 8), and takes keys
 * published on DOOM_KEYS_TOPIC (default doom.keys): "d<code>,u<code>,..." */
static char draw_topic[48] = "doom.draw", keys_topic[48] = "doom.keys";
static uint32_t frame_ms = 125, publish_errors, frames_published, keys_received;
static int64_t last_frame_ms;
/* k32_publish sends no faster than the bytes a second the view's node announces, and waits
 * for the last message to leave before it sends the next: the pod, and every POST behind
 * it, would wait with it. A frame is drawn and published once the last list has left at
 * DOOM_DRAW_RATE bytes a second (default 49152, the Cardputer's; 0 publishes at once). */
static uint32_t draw_rate = 49152;
static int64_t drained_ms;
static uint32_t frame_waits, publish_ms_max;
static uint64_t publish_ms_sum;
uint32_t doom_drawlist_ptr(void);
uint32_t doom_drawlist_len(void);
static void sim_env(void) {
    char v[16];
    int32_t n = k32_env_get("DOOM_DRAW_TOPIC", 15, draw_topic, sizeof draw_topic - 1);
    if (n > 0 && n < (int32_t)sizeof draw_topic) draw_topic[n] = 0; else strcpy(draw_topic, "doom.draw");
    n = k32_env_get("DOOM_KEYS_TOPIC", 15, keys_topic, sizeof keys_topic - 1);
    if (n > 0 && n < (int32_t)sizeof keys_topic) keys_topic[n] = 0; else strcpy(keys_topic, "doom.keys");
    n = k32_env_get("DOOM_FPS", 8, v, sizeof v - 1);
    if (n > 0 && n < (int32_t)sizeof v) { v[n] = 0; long fps = atoi(v); if (fps >= 1 && fps <= 35) frame_ms = (uint32_t)(1000 / fps); }
    n = k32_env_get("DOOM_DRAW_RATE", 14, v, sizeof v - 1);
    if (n > 0 && n < (int32_t)sizeof v) { v[n] = 0; draw_rate = (uint32_t)atoi(v); }
}
static void parse_keys(const char* p, const char* end) {
    while (p < end) {
        if ((*p == 'd' || *p == 'u') && p + 1 < end && p[1] >= '0' && p[1] <= '9') { key_push(*p == 'd', (unsigned)strtol(p + 1, 0, 10)); keys_received++; }
        while (p < end && *p != ',') p++;
        if (p < end) p++;
    }
}
/* keys arrive as events on the keys topic */
K32_EXPORT("k32_event") int32_t k32_event(uint32_t topic, uint32_t topic_len, uint32_t ptr, uint32_t len) {
    (void)topic; (void)topic_len;
    const char* p = (const char*)(uintptr_t)ptr;
    parse_keys(p, p + len);
    return 0;
}
#endif

/* How long each k32_tick takes (GET /stats) */
static uint32_t tick_ms_max;
static uint64_t tick_ms_sum, ticks_timed;
static int32_t tick(int64_t now);

K32_EXPORT("k32_tick") int32_t k32_tick(uint32_t uptime_ms) {
    (void)uptime_ms;
    if (!started) return 0;
    const int64_t t0 = k32_time_ms();
    const int32_t rc = tick(t0);
    const uint32_t ms = (uint32_t)(k32_time_ms() - t0);
    tick_ms_sum += ms;
    ticks_timed++;
    if (ms > tick_ms_max) tick_ms_max = ms;
    return rc;
}

static int32_t tick(int64_t now) {
    static int drawn_tic = -1;
    if (net_role() == NET_AUTHORITY) {
        /* two players: the tics whose ticcmds are in (a POST may have run some);
         * before a browser joins, player 1's tics alone */
        if (net_active()) net_tick(now); else net_tick_alone(now);
    } else {   /* a single player: the tics due by the clock */
        uint32_t due = (uint32_t)((now - start_ms) * TICRATE / 1000);
        if (due > tics_run) {
            uint32_t n = due - tics_run;
            if (n > 4) {                 /* behind: drop the time rather than catch up */
                tics_run = due - 4;
                n = 4;
            }
            while (n--) doom_run_tic();
        }
    }
    if (gametic == drawn_tic) return 1;   /* a frame when the game moved (not in the lobby) */
#ifdef DG_SIM_ONLY
    if (now - last_frame_ms < (int64_t)frame_ms) return 1;
    if (draw_rate && now < drained_ms) { frame_waits++; return 1; }   /* the last list is still leaving */
    last_frame_ms = now;
    drawn_tic = gametic;
    doom_render();
    const int64_t p0 = k32_time_ms();
    int32_t rc = k32_publish(draw_topic, (uint32_t)strlen(draw_topic), (const void*)(uintptr_t)doom_drawlist_ptr(), doom_drawlist_len());
    const int64_t p1 = k32_time_ms();
    publish_ms_sum += (uint64_t)(p1 - p0);
    if ((uint32_t)(p1 - p0) > publish_ms_max) publish_ms_max = (uint32_t)(p1 - p0);
    if (draw_rate) {
        /* a publish that still waited means the view takes less than the rate assumed: assume a
         * quarter less; after 20 that did not wait, a tenth more, up to DOOM_DRAW_RATE */
        static uint32_t rate, clean;
        if (!rate) rate = draw_rate;
        if (p1 - p0 > 10) { rate -= rate / 4; if (rate < 4096) rate = 4096; clean = 0; }
        else if (++clean >= 20 && rate < draw_rate) { rate += rate / 10; if (rate > draw_rate) rate = draw_rate; clean = 0; }
        drained_ms = p1 + (int64_t)doom_drawlist_len() * 1000 / rate;
    }
    if (rc < 0) publish_errors++; else frames_published++;
    if ((frames_published + publish_errors) % 100 == 0) {
        char line[160];
        const uint32_t sent = frames_published + publish_errors;
        snprintf(line, sizeof line, "doom-sim: frames %u publish_errors %u keys %u list %u B; publish %u ms mean, %u max; tick %u ms mean, %u max",
                 (unsigned)frames_published, (unsigned)publish_errors, (unsigned)keys_received, (unsigned)doom_drawlist_len(),
                 (unsigned)(publish_ms_sum / sent), (unsigned)publish_ms_max, (unsigned)(ticks_timed ? tick_ms_sum / ticks_timed : 0), (unsigned)tick_ms_max);
        k32_log(line, (uint32_t)strlen(line));
    }
#else
    drawn_tic = gametic;
    doom_render();
#endif
    return 1;
}

/* ---- HTTP --------------------------------------------------------------- */

/* A request; one that does not fit (a browser's cookies) comes from the heap until k32_handle returns */
static unsigned char req_buf[2048];
static unsigned char* req_big;

K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    if (size < sizeof req_buf) return (uint32_t)(uintptr_t)req_buf;
    free(req_big);
    req_big = malloc(size + 1);
    return (uint32_t)(uintptr_t)req_big;
}

static int32_t handle_request(char* req, uint32_t len);

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    int32_t status = handle_request((char*)(uintptr_t)ptr, len);
    if (req_big) { free(req_big); req_big = 0; }
    return status;
}

static char out[640];
static unsigned out_len;
static void put(const char* s) { while (*s && out_len < sizeof out - 1) out[out_len++] = *s++; }
static void put_kv(const char* k, int64_t v) {
    char b[48];
    snprintf(b, sizeof b, "%s\"%s\":%lld", out_len > 1 ? "," : "", k, (long long)v);
    put(b);
}
static int32_t reply(int32_t status, const char* type, const void* body, uint32_t len) {
    k32_resp_begin(status, type, (uint32_t)strlen(type));
    if (len) k32_resp_write(body, len);
    k32_resp_end();
    return status;
}
static int32_t reply_json(void) {
    put("}");
    return reply(200, "application/json", out, out_len);
}

static long query_int(const char* query, const char* key, long def) {
    size_t k = strlen(key);
    for (const char* p = query; p && *p; ) {
        if (!strncmp(p, key, k) && p[k] == '=') return strtol(p + k + 1, 0, 10);
        p = strchr(p, '&');
        if (p) p++;
    }
    return def;
}

static const char page[] =
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
    "<title>DOOM</title><style>body{margin:0;background:#000;display:grid;place-items:center;height:100vh}"
    "canvas{width:min(100vw,160vh);image-rendering:pixelated}</style><canvas width=320 height=200></canvas><script>"
    "const c=document.querySelector('canvas').getContext('2d'),img=c.createImageData(320,200),base=location.pathname.replace(/\\/$/,'');"
    "const K={ArrowUp:173,ArrowDown:175,ArrowLeft:172,ArrowRight:174,Control:163,' ':162,Enter:13,Escape:27,Shift:182,Alt:184,Tab:9,"
    "',':160,'.':161,y:121,n:110};let q=[],busy=0;"
    "function key(e,d){let k=K[e.key]??(e.key.length==1?e.key.toLowerCase().charCodeAt(0):0);if(!k)return;e.preventDefault();"
    "q.push((d?'d ':'u ')+k);if(!busy)send()}"
    "async function send(){busy=1;while(q.length){const b=q.splice(0).join('\\n');await fetch(base+'/key',{method:'POST',body:b})}busy=0}"
    "addEventListener('keydown',e=>{if(!e.repeat)key(e,1)});addEventListener('keyup',e=>key(e,0));"
    "async function loop(){try{const r=await fetch(base+'/frame');const a=new Uint8Array(await r.arrayBuffer());"
    "for(let i=0;i<64000;i++){const p=a[768+i]*3;img.data.set([a[p],a[p+1],a[p+2],255],i*4)}c.putImageData(img,0,0)}catch(e){}"
    "requestAnimationFrame(loop)}loop()</script>";

static int32_t handle_request(char* req, uint32_t len) {
    req[len] = 0;
    char* sp = strchr(req, ' ');
    if (!sp) return K32_HANDLE_ERR_BAD_REQUEST;
    *sp = 0;
    const char* method = req;
    char* path = sp + 1;
    char* eol = strchr(path, '\n');
    if (!eol) return K32_HANDLE_ERR_BAD_REQUEST;
    *eol = 0;
    char* body = strstr(eol + 1, "\n\n");
    body = body ? body + 2 : eol + 1;
    char* query = strchr(path, '?');
    if (query) *query++ = 0;
    char* seg = strrchr(path, '/');
    seg = seg ? seg + 1 : path;
    out_len = 0;
    put("{");
    {
        int32_t net_status = net_route(method, seg, body);
        if (net_status) return net_status;
    }

#ifndef DG_SIM_ONLY
    if (!strcmp(seg, "frame")) {
        static uint8_t pal[768];
        for (int i = 0; i < 256; i++) {
            pal[i * 3] = colors[i].r;
            pal[i * 3 + 1] = colors[i].g;
            pal[i * 3 + 2] = colors[i].b;
        }
        char n[16];
        snprintf(n, sizeof n, "%u", (unsigned)frames_drawn);
        k32_resp_header("X-Doom-Frame", 12, n, (uint32_t)strlen(n));
        k32_resp_header("Cache-Control", 13, "no-store", 8);
        k32_resp_begin(200, "application/octet-stream", 24);
        k32_resp_write(pal, sizeof pal);
        k32_resp_write(DG_ScreenBuffer, DOOMGENERIC_RESX * DOOMGENERIC_RESY);
        k32_resp_end();
        return 200;
    }
#endif
#ifdef DG_SIM_ONLY
    if (!strcmp(seg, "drawlist")) {
        /* keys ride along: keys=d<code>,u<code>,... */
        for (const char* p = query; p && *p; ) {
            if (!strncmp(p, "keys=", 5)) {
                for (p += 5; *p && *p != '&'; ) {
                    if ((*p == 'd' || *p == 'u') && p[1] >= '0' && p[1] <= '9') key_push(*p == 'd', (unsigned)strtol(p + 1, 0, 10));
                    while (*p && *p != ',' && *p != '&') p++;
                    if (*p == ',') p++;
                }
                break;
            }
            p = strchr(p, '&');
            if (p) p++;
        }
        char n[16];
        snprintf(n, sizeof n, "%u", (unsigned)frames_drawn);
        k32_resp_header("X-Doom-Frame", 12, n, (uint32_t)strlen(n));
        k32_resp_header("Cache-Control", 13, "no-store", 8);
        k32_resp_begin(200, "application/octet-stream", 24);
        k32_resp_write((const void*)(uintptr_t)doom_drawlist_ptr(), doom_drawlist_len());
        k32_resp_end();
        return 200;
    }
#endif
    if (!strcmp(seg, "key") && !strcmp(method, "POST")) {
        int count = 0;
        for (char* p = body; *p; ) {
            if ((*p == 'd' || *p == 'u') && p[1] == ' ') {
                key_push(*p == 'd', (unsigned)strtol(p + 2, 0, 10));
                count++;
            }
            p = strchr(p, '\n');
            if (!p) break;
            p++;
        }
        put_kv("keys", count);
        return reply_json();
    }
    if (!strcmp(seg, "stats")) {
        unsigned fixed, cache;
        Z_UsageBytes(&fixed, &cache);
        put_kv("frames", frames_drawn);
        put_kv("gametic", gametic);
        put_kv("zone_fixed", fixed);
        put_kv("zone_cache", cache);
        put_kv("zone_free", Z_FreeMemory());
        put_kv("zone_purges", z_purges);
        put_kv("zone_purged_bytes", z_purged_bytes);
        put_kv("heap_end", k32_libc_heap_peak);
        put_kv("memory_bytes", (int64_t)__builtin_wasm_memory_size(0) * 65536);
        put_kv("blob_reads", k32_libc_blob_reads);
        put_kv("blob_bytes", k32_libc_blob_bytes);
        put_kv("tick_ms_mean", ticks_timed ? (int64_t)(tick_ms_sum / ticks_timed) : 0);
        put_kv("tick_ms_max", tick_ms_max);
#ifdef DG_SIM_ONLY
        put_kv("frames_published", frames_published);
        put_kv("publish_errors", publish_errors);
        put_kv("keys_received", keys_received);
        put_kv("publish_ms_mean", frames_published + publish_errors ? (int64_t)(publish_ms_sum / (frames_published + publish_errors)) : 0);
        put_kv("publish_ms_max", publish_ms_max);
        put_kv("frame_waits", frame_waits);
#endif
        return reply_json();
    }
    if (!strcmp(seg, "bench")) {
        long tics = query_int(query, "tics", 35), nframes = query_int(query, "frames", 35);
        uint32_t reads0 = k32_libc_blob_reads, purges0 = z_purges;
        int64_t t0 = esp32_get_time_us();
        for (long i = 0; i < tics; i++) {
            clock_offset_ms += 1000 / TICRATE;
            doom_run_tic();
        }
        int64_t t1 = esp32_get_time_us();
        for (long i = 0; i < nframes; i++) doom_render();
        int64_t t2 = esp32_get_time_us();
        put_kv("tics", tics);
        put_kv("tic_us", t1 - t0);
        put_kv("frames", nframes);
        put_kv("frame_us", t2 - t1);
        put_kv("blob_reads", k32_libc_blob_reads - reads0);
        put_kv("zone_purges", z_purges - purges0);
        start_ms -= (t2 - t0) / 1000;   /* the live loop does not catch up on the bench */
        return reply_json();
    }
    if (!*seg || !strcmp(seg, "doom") || !strcmp(seg, "index.html")) {
        k32_resp_header("Cache-Control", 13, "no-store", 8);
        return reply(200, "text/html; charset=utf-8", page, sizeof page - 1);
    }
    put("\"error\":\"not found\"");
    put("}");
    return reply(404, "application/json", out, out_len);
}
