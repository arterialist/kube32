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
 * Ingress prefix (/doom/frame). */
#include "include/k32libc.h"
#include "../../../include/runtime/k32_app_abi.h"

#include "../doomgeneric/doomgeneric.h"
#include "../doomgeneric/doomkeys.h"
#include "../doomgeneric/d_loop.h"
#include "../doomgeneric/i_video.h"
#include "../doomgeneric/doomstat.h"

K32_IMPORT("esp32_get_time_us") int64_t esp32_get_time_us(void);

void D_Display(void);
void TryRunTics(void);
void I_StartFrame(void);
void Z_UsageBytes(unsigned int* fixed, unsigned int* cache);
int Z_FreeMemory(void);
extern unsigned int z_purges, z_purged_bytes;
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
    n = k32_env_get("DOOM_ARGS", 9, args_buf, sizeof args_buf - 1);
    if (n > 0 && n < (int32_t)sizeof args_buf) {
        args_buf[n] = 0;
        char* p = args_buf;
        while (*p && argc < 23) {
            while (*p == ' ') *p++ = 0;
            if (!*p) break;
            argv_store[argc++] = p;
            while (*p && *p != ' ') p++;
        }
    }
    argv_store[argc] = 0;
    singletics = true;          /* every TryRunTics runs exactly one tic */
    doomgeneric_Create(argc, argv_store);
    started = 1;
    start_ms = k32_time_ms();
    return 0;
}

K32_EXPORT("k32_tick") int32_t k32_tick(uint32_t uptime_ms) {
    (void)uptime_ms;
    if (!started) return 0;
    uint32_t due = (uint32_t)((k32_time_ms() - start_ms) * TICRATE / 1000);
    if (due <= tics_run) return 1;
    uint32_t n = due - tics_run;
    if (n > 4) {                 /* behind: drop the time rather than catch up */
        tics_run = due - 4;
        n = 4;
    }
    while (n--) doom_run_tic();
    doom_render();
    return 1;
}

/* ---- HTTP --------------------------------------------------------------- */

static unsigned char req_buf[2048];

K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof req_buf ? (uint32_t)(uintptr_t)req_buf : 0;
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

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    char* req = (char*)(uintptr_t)ptr;
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
