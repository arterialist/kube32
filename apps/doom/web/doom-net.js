// SPDX-License-Identifier: GPL-2.0-or-later
// Player 2 of a two-player DOOM netgame (port/net.c), in the browser.
//
// The page runs the same engine as the pod (doom.wasm, the full build) and
// draws its own view. It implements the imports the engine calls (the
// clock, the IWAD as a blob, the log), joins the pod's game, and then plays
// from the confirmed ticcmds alone: the pod's stream of both players'
// ticcmds (Server-Sent Events from the Ingress, and the answer to every
// POST) runs the tics in order; this side's ticcmds go to the pod with
// POST <base>/tics, two at a time, built by the clock `delay` tics past the
// pod's tic as the page estimates it from the newest confirmed tic and the
// round trip.
//
// Query parameters: delay=<tics> (the input delay, default the pod's),
// events=<url> (the event stream, default <base>/events), wad=<url> and
// wasm=<url> (default <base>/doom.wad and <base>/doom.wasm).

const DOOM_KEYS = {
  ArrowUp: 0xad, ArrowDown: 0xaf, ArrowLeft: 0xac, ArrowRight: 0xae,
  KeyW: 0xad, KeyS: 0xaf, KeyA: 0xac, KeyD: 0xae,
  ControlLeft: 0xa3, ControlRight: 0xa3, Space: 0xa2, ShiftLeft: 0xb6, ShiftRight: 0xb6,
  AltLeft: 0xb8, AltRight: 0xb8, Enter: 13, NumpadEnter: 13, Escape: 27, Tab: 9, Backspace: 0x7f,
  Comma: 0x2c, Period: 0x2e, Pause: 0xff, F1: 0xbb, F2: 0xbc, F3: 0xbd, F4: 0xbe, F5: 0xbf, F6: 0xc0,
  F7: 0xc1, F8: 0xc2, F9: 0xc3, F10: 0xc4, F11: 0xd7, F12: 0xd8, Minus: 0x2d, Equal: 0x3d,
};

const POST_EVERY_MS = 250;    // a POST at least this often, new ticcmds or not
const POST_HOLD_MS = 35;      // a lone new ticcmd waits this long for a second one to share its POST
const POSTS_IN_FLIGHT = 4;
const POST_TIMEOUT_MS = 1000;
const TICS_PER_STEP = 8;

export class DoomNet {
  constructor(canvas, status) {
    this.canvas = canvas;
    this.status = status;
    const here = new URL(location.href);
    this.base = here.pathname.endsWith('/') ? here.pathname : here.pathname.replace(/[^/]*$/, '') ;
    if (!/\.html?$/.test(here.pathname) && !here.pathname.endsWith('/')) this.base = here.pathname + '/';
    this.params = here.searchParams;
    this.inFlight = 0;
    this.lastPost = -1e9;
    this.freshAt = -1;
    this.posts = 0;
    this.answers = 0;
    this.rtt = 0;
    this.stalls = 0;
    this.dirty = false;
    this.state = 'loading';
  }

  url(name) { return this.params.get(name) || this.base + (name === 'events' ? 'events' : name === 'wad' ? 'doom.wad' : 'doom.wasm'); }

  say(text) { this.status.textContent = text; }

  async fetchBytes(url, what) {
    const r = await fetch(url);
    if (!r.ok) throw new Error(`${what}: ${r.status} ${r.statusText} (${url})`);
    const total = +r.headers.get('Content-Length') || 0;
    const reader = r.body.getReader();
    const parts = [];
    let got = 0;
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      parts.push(value);
      got += value.length;
      this.say(`${what}: ${(got / 1024) | 0} KiB${total ? ` of ${(total / 1024) | 0}` : ''}`);
    }
    const out = new Uint8Array(got);
    let at = 0;
    for (const p of parts) { out.set(p, at); at += p.length; }
    return out;
  }

  bytes() { return new Uint8Array(this.memory.buffer); }
  text(ptr, len) { return new TextDecoder().decode(this.bytes().subarray(ptr, ptr + len)); }
  write(ptr, str) {
    const b = new TextEncoder().encode(str);
    this.bytes().set(b, ptr);
    return b.length;
  }

  imports() {
    const env = () => this.env;
    const wad = () => this.wad;
    const t0 = performance.now();
    return {
      env: {
        k32_time_ms: () => BigInt(Math.floor(performance.now() - t0)),
        esp32_get_time_us: () => BigInt(Math.floor((performance.now() - t0) * 1000)),
        k32_log: (ptr, len) => console.log('[doom]', this.text(ptr, len)),
        k32_env_get: (kp, kl, out, cap) => {
          const v = env()[this.text(kp, kl)];
          if (v === undefined) return -1;
          const b = new TextEncoder().encode(v);
          this.bytes().set(b.subarray(0, Math.min(cap, b.length)), out);
          return b.length;
        },
        k32_blob_open: () => 0,
        k32_blob_size: () => wad().length,
        k32_blob_read: (h, off, buf, len) => {
          const w = wad();
          if (off >= w.length) return 0;
          const n = Math.min(len, w.length - off);
          this.bytes().set(w.subarray(off, off + n), buf);
          return n;
        },
        k32_blob_close: () => 0,
        k32_event_emit: () => -95,
        k32_resp_begin: () => {}, k32_resp_header: () => {}, k32_resp_write: (p, n) => n, k32_resp_end: () => {},
      },
    };
  }

  async start() {
    this.say('asking the pod for its game');
    const hello = await (await fetch(this.base + 'net', { cache: 'no-store' })).json();
    const [wasm, wad] = await Promise.all([this.fetchBytes(this.url('wasm'), 'engine'), this.fetchBytes(this.url('wad'), 'IWAD')]);
    this.wad = wad;
    this.env = {
      DOOM_IWAD: hello.iwad,
      DOOM_WAD_BLOB: 'doom-wad',
      DOOM_ARGS: `${hello.args} -lockstep ${hello.players} -consoleplayer 1`,
      DOOM_SCREENBLOCKS: '10',
    };
    const { instance } = await WebAssembly.instantiate(wasm, this.imports());
    this.x = instance.exports;
    this.memory = this.x.memory;
    this.say('starting the engine');
    if (this.x.k32_init() !== 0) throw new Error('the engine did not start');
    this.say('joining');
    const join = await (await fetch(this.base + 'join', { method: 'POST', cache: 'no-store' })).json();
    this.delay = +(this.params.get('delay') || join.delay);
    this.session = join.g;
    this.x.doom_net_join(join.g, join.s, this.delay, join.digest);
    this.state = 'playing';
    this.listen();
    this.keys();
    this.image = this.canvas.getContext('2d').createImageData(320, 200);
    this.pixels = new Uint32Array(this.image.data.buffer);
    this.lut = new Uint32Array(256);
    setInterval(() => this.step(), 4);
    document.addEventListener('visibilitychange', () => {
      if (!document.hidden && this.state === 'playing') { this.x.doom_render(); this.dirty = true; }
    });
    const draw = () => { if (this.dirty) this.draw(); requestAnimationFrame(draw); };
    requestAnimationFrame(draw);
    setInterval(() => this.report(), 500);
  }

  // A confirmed-tics message from the pod. The tics run from here, not from
  // a timer alone: a browser slows the timers of a hidden tab to once a
  // second, and the pod would wait for player 2 that long every few tics.
  receive(msg) {
    if (this.state !== 'playing') return;
    const n = this.write(this.x.doom_net_in(), msg);
    if (this.x.doom_net_recv(n) === -2) this.gone();
    else this.step();
  }

  gone() {
    this.state = 'gone';
    if (this.events) this.events.close();
    this.say('Another player 2 joined, or the pod restarted the game. Reload the page to join again.');
  }

  listen() {
    this.events = new EventSource(this.url('events'));
    this.events.addEventListener('tics', (e) => this.receive(e.data));
  }

  keys() {
    const send = (e, down) => {
      let k = DOOM_KEYS[e.code];
      if (k === undefined && e.key.length === 1) k = e.key.toLowerCase().charCodeAt(0);
      if (!k || k > 255) return;
      e.preventDefault();
      if (down && e.repeat) return;
      this.x.doom_key(down ? 1 : 0, k);
    };
    addEventListener('keydown', (e) => send(e, true));
    addEventListener('keyup', (e) => send(e, false));
    addEventListener('blur', () => { for (const k of new Set(Object.values(DOOM_KEYS))) this.x.doom_key(0, k); });
  }

  // Runs the confirmed tics and sends this side's ticcmds
  step() {
    if (this.state !== 'playing') return;
    const ran = this.x.doom_net_run(TICS_PER_STEP);
    if (ran && !document.hidden) {
      this.x.doom_render();
      this.dirty = true;
    }
    // this side's ticcmds by the clock: `delay` tics past the authority's tic as estimated
    // from the newest confirmed tic and the round trip (port/net.c, doom_net_pace)
    const now = performance.now();
    const fresh = this.x.doom_net_pace(Math.round(this.rtt));
    if (fresh > 0 && this.freshAt < 0) this.freshAt = now;
    const due = fresh >= 2 || (fresh > 0 && now - this.freshAt >= POST_HOLD_MS) || now - this.lastPost >= POST_EVERY_MS;
    if (due && this.inFlight < POSTS_IN_FLIGHT) this.post(now);
  }

  async post(now) {
    const body = this.text(this.x.doom_net_out(), this.x.doom_net_post());
    this.freshAt = -1;
    this.inFlight++;
    this.lastPost = now;
    this.posts++;
    const abort = new AbortController();
    const timer = setTimeout(() => abort.abort(), POST_TIMEOUT_MS);
    try {
      const r = await fetch(this.base + 'tics', { method: 'POST', body, cache: 'no-store', signal: abort.signal });
      const answer = await r.text();
      this.answers++;
      this.rtt = this.rtt ? this.rtt * 0.9 + (performance.now() - now) * 0.1 : performance.now() - now;
      this.receive(answer);
    } catch (e) {
      // lost: the next POST carries the same ticcmds
    } finally {
      clearTimeout(timer);
      this.inFlight--;
    }
  }

  draw() {
    this.dirty = false;
    const mem = this.bytes();
    const pal = mem.subarray(this.x.doom_palette(), this.x.doom_palette() + 768);
    for (let i = 0; i < 256; i++) this.lut[i] = 0xff000000 | (pal[3 * i + 2] << 16) | (pal[3 * i + 1] << 8) | pal[3 * i];
    const screen = mem.subarray(this.x.doom_screen_ptr(), this.x.doom_screen_ptr() + 64000);
    for (let i = 0; i < 64000; i++) this.pixels[i] = this.lut[screen[i]];
    this.canvas.getContext('2d').putImageData(this.image, 0, 0);
  }

  stats() { return JSON.parse(this.text(this.x.doom_net_out(), this.x.doom_net_stats())); }

  report() {
    if (this.state !== 'playing') return;
    const s = this.stats();
    this.last = s;
    const sync = s.digests_bad ? `OUT OF SYNC at tic ${s.first_bad_tic}` : `${s.digests_checked} state checks agree`;
    this.say(`player 2 · tic ${s.tic} · input delay ${s.delay} tics · round trip ${this.rtt.toFixed(0)} ms · ${sync}${s.in_game ? '' : ' · you left the game: reload to join again'}`);
  }
}
