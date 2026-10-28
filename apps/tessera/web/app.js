'use strict';
/* Tessera client. Vanilla ES2020, no dependencies. */

// ---------------------------------------------------------------- utils
const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];
const esc = s => String(s ?? '').replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
const el = html => { const t = document.createElement('template'); t.innerHTML = html.trim(); return t.content.firstChild; };
const debounce = (fn, ms) => { let t; return (...a) => { clearTimeout(t); t = setTimeout(() => fn(...a), ms); }; };
const fmt = n => n == null ? '—' : Number(n).toLocaleString('en-US');
const bytes = n => n >= 1048576 ? (n / 1048576).toFixed(2) + ' MB' : n >= 1024 ? (n / 1024).toFixed(1) + ' KB' : n + ' B';
const us = n => n == null ? '—' : n >= 1e6 ? (n / 1e6).toFixed(2) + ' s' : n >= 1000 ? (n / 1000).toFixed(1) + ' ms' : Math.round(n) + ' µs';
const ms = n => n >= 1000 ? (n / 1000).toFixed(2) + ' s' : n.toFixed(1) + ' ms';
const dur = m => { const s = Math.floor(m / 1000), d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), mi = Math.floor(s % 3600 / 60); return d ? `${d}d ${h}h` : h ? `${h}h ${mi}m` : `${mi}m ${s % 60}s`; };
const aborted = e => e && e.name === 'AbortError';
const cap = s => s ? s[0].toUpperCase() + s.slice(1) : '';
const SL = { backlog: 'Backlog', todo: 'Todo', in_progress: 'In progress', review: 'In review', done: 'Done' };
const PL = ['No priority', 'Low', 'Medium', 'High', 'Urgent'];
const HUD = { id: 0, username: 'hud', name: 'Device keyboard', initials: 'HK', color: '#8b949e' };

// ---------------------------------------------------------------- icons
const ICONS = {
  board: '<rect x="3" y="4" width="5" height="16" rx="1"/><rect x="10" y="4" width="5" height="11" rx="1"/><rect x="17" y="4" width="4" height="7" rx="1"/>',
  list: '<path d="M8 6h13M8 12h13M8 18h13M3 6h.01M3 12h.01M3 18h.01"/>',
  chart: '<path d="M3 20h18M6 16v-5M11 16V7M16 16v-3M21 16V4"/>',
  pulse: '<path d="M3 12h4l3-8 4 16 3-8h4"/>',
  user: '<circle cx="12" cy="8" r="4"/><path d="M4 21a8 8 0 0 1 16 0"/>',
  cpu: '<rect x="4" y="4" width="16" height="16" rx="2"/><rect x="9" y="9" width="6" height="6"/><path d="M9 1v3M15 1v3M9 20v3M15 20v3M1 9h3M1 15h3M20 9h3M20 15h3"/>',
  search: '<circle cx="11" cy="11" r="7"/><path d="m20 20-3.5-3.5"/>',
  plus: '<path d="M12 5v14M5 12h14"/>',
  x: '<path d="M18 6 6 18M6 6l12 12"/>',
  check: '<path d="m5 12 5 5L20 7"/>',
  chev: '<path d="m6 9 6 6 6-6"/>',
  up: '<path d="m18 15-6-6-6 6"/>',
  back: '<path d="m15 18-6-6 6-6"/>',
  trash: '<path d="M3 6h18M8 6V4h8v2M6 6l1 14h10l1-14"/>',
  dots: '<circle cx="5" cy="12" r="1.2"/><circle cx="12" cy="12" r="1.2"/><circle cx="19" cy="12" r="1.2"/>',
  msg: '<path d="M21 12a8 8 0 0 1-11.6 7.1L4 20l1-4.6A8 8 0 1 1 21 12z"/>',
  kb: '<rect x="2" y="6" width="20" height="12" rx="2"/><path d="M6 10h.01M10 10h.01M14 10h.01M18 10h.01M8 14h8"/>',
  arrow: '<path d="M5 12h14m-6-6 6 6-6 6"/>',
};
const icon = (n, c = '') => `<svg class="i ${c}" viewBox="0 0 24 24">${ICONS[n]}</svg>`;
const MARK = '<svg class="mark" viewBox="0 0 32 32"><rect width="32" height="32" rx="7" fill="#15181e"/><path d="M7 7h18v5H7zM7 14h11v5H7zM7 21h6v4H7zM20 14h5v11h-5z" fill="#f2a93b"/></svg>';
function stIcon(s) {
  const c = `var(--s-${s})`, ring = `<circle cx="8" cy="8" r="6" fill="none" stroke="${c}" stroke-width="1.5"`;
  const inner = s === 'backlog' ? ring + ' stroke-dasharray="2.2 2"/>'
    : s === 'todo' ? ring + '/>'
      : s === 'in_progress' ? ring + `/><path d="M8 4.5A3.5 3.5 0 0 1 8 11.5z" fill="${c}"/>`
        : s === 'review' ? ring + `/><path d="M8 4.5A3.5 3.5 0 1 1 4.5 8H8z" fill="${c}"/>`
          : `<circle cx="8" cy="8" r="6.5" fill="${c}"/><path d="m5 8 2 2 4-4" fill="none" stroke="#0b0b0b" stroke-width="1.6"/>`;
  return `<svg class="st" viewBox="0 0 16 16">${inner}</svg>`;
}
const prIcon = p => `<svg class="pr p${p}" viewBox="0 0 16 16"><title>${PL[p]}</title><rect x="2" y="9" width="3" height="5" rx=".5"/><rect x="6.5" y="6" width="3" height="8" rx=".5"/><rect x="11" y="3" width="3" height="11" rx=".5"/></svg>`;
const stPill = s => `<span class="pill">${stIcon(s)}${SL[s] || s}</span>`;
const userOf = id => id === 0 ? HUD : S.users[id] || { id, name: id == null ? 'Unassigned' : 'Unknown', initials: '?', color: '#444' };
const av = (id, cls = '') => id == null ? `<span class="av none ${cls}">${icon('user')}</span>` : (u => `<span class="av ${cls}" style="background:${u.color}" title="${esc(u.name)}">${esc(u.initials)}</span>`)(userOf(id));
const labelChip = id => { const l = S.labels[id]; return l ? `<span class="chip" style="--c:${l.color}"><i></i>${esc(l.name)}</span>` : ''; };

// ---------------------------------------------------------------- state
const S = {
  me: null, boot: null, users: {}, labels: {}, projects: {}, clock: { s: Date.now(), at: performance.now() },
  project: +localStorage.getItem('ts.project') || 0, view: null, gen: 0, ctl: null, es: null, live: 'off', viewers: 0,
  lastList: '#/board', cluster: null, heap: {}, esRetry: null,
};
const now = () => S.clock.s + (performance.now() - S.clock.at);
const syncNow = n => { if (n) S.clock = { s: n, at: performance.now() }; };
function rel(ts, long) {
  const d = Math.max(0, now() - ts), m = d / 6e4, h = m / 60, dd = h / 24;
  const f = (n, u, w) => long ? `${n} ${w}${n === 1 ? '' : 's'} ago` : n + u;
  if (m < 1) return 'just now';
  if (m < 60) return f(Math.floor(m), 'm', 'minute');
  if (h < 24) return f(Math.floor(h), 'h', 'hour');
  if (dd < 30) return f(Math.floor(dd), 'd', 'day');
  if (dd < 365) return f(Math.floor(dd / 30), 'mo', 'month');
  return f(Math.floor(dd / 365), 'y', 'year');
}
const abs = ts => new Date(ts).toLocaleString();
const projectOf = id => S.projects[id] || { key: '?', name: 'Unknown', color: '#444' };

// ---------------------------------------------------------------- api
class ApiError extends Error { constructor(status, code, message) { super(message || code || 'error'); this.status = status; this.code = code; } }
async function api(path, { method = 'GET', body, signal, quiet } = {}) {
  let r;
  try {
    r = await fetch(path, { method, headers: body ? { 'Content-Type': 'application/json' } : {}, body: body && JSON.stringify(body), signal, credentials: 'same-origin' });
  } catch (e) {
    if (aborted(e)) throw e;
    toast('Network error. Retrying when you act again.', { kind: 'error', id: 'net' });
    throw new ApiError(0, 'network', 'network error');
  }
  if (r.status === 204) return null;
  let data = null;
  try { data = await r.json(); } catch (e) { data = null; }
  if (r.status === 401 && !quiet) { onUnauthorized(); throw new ApiError(401, 'unauthorized', 'Signed out'); }
  if (!r.ok) {
    if (r.status >= 500) toast(`Server error ${r.status}: ${data && data.message || 'request failed'}`, { kind: 'error' });
    throw new ApiError(r.status, data && data.error, data && data.message);
  }
  syncNow(data && data.now);
  return data;
}
function onUnauthorized() {
  if (!S.me) return;
  S.me = null;
  if (S.es) { S.es.close(); S.es = null; }
  toast('Your session ended. Please sign in again.', { kind: 'error' });
  renderLogin();
}

// ---------------------------------------------------------------- toasts
function toast(msg, { kind = '', id, ttl = 4500, title } = {}) {
  let box = $('#toasts');
  if (!box) { box = el('<div id="toasts" class="toasts" aria-live="polite"></div>'); document.body.appendChild(box); }
  let t = id && $(`#toast-${id}`);
  const inner = `${title ? `<div class="ttl">${esc(title)}</div>` : ''}<p>${esc(msg)}</p>`;
  if (t) { $('.b', t).innerHTML = inner; clearTimeout(t._t); }
  else {
    t = el(`<div class="toast ${kind}" ${id ? `id="toast-${id}"` : ''}><div class="b">${inner}</div><button class="x" aria-label="Dismiss">${icon('x')}</button></div>`);
    $('.x', t).onclick = () => t.remove();
    box.appendChild(t);
  }
  if (ttl) t._t = setTimeout(() => t.remove(), ttl);
  return t;
}

// ---------------------------------------------------------------- popover
let POP = null;
function closePop() { if (POP) { POP.el.remove(); document.removeEventListener('mousedown', POP.out, true); POP = null; } }
/** items: [{id, html, on, hd}] ; opts: {multi, search, onPick(id, item), width} */
function popover(anchor, items, opts = {}) {
  closePop();
  const p = el(`<div class="pop" style="min-width:${opts.width || 200}px"></div>`);
  if (opts.search) p.appendChild(el(`<input class="inp" placeholder="${opts.search}">`));
  const list = el('<div></div>'); p.appendChild(list);
  const render = (filter = '') => {
    const f = filter.toLowerCase();
    list.innerHTML = items.filter(it => !f || it.hd || (it.text || '').toLowerCase().includes(f)).map(it => it.hd ? `<div class="hd">${it.hd}</div>`
      : `<button class="it ${it.on ? 'on' : ''}" data-id="${esc(it.id)}">${it.html}${icon('check', 'ck')}</button>`).join('');
  };
  render();
  document.body.appendChild(p);
  const r = anchor.getBoundingClientRect(), pw = p.offsetWidth, ph = p.offsetHeight;
  let x = Math.min(r.left, innerWidth - pw - 8), y = r.bottom + 4;
  if (y + ph > innerHeight - 8) y = Math.max(8, r.top - ph - 4);
  p.style.left = x + 'px'; p.style.top = y + 'px';
  list.onclick = e => {
    const b = e.target.closest('.it'); if (!b) return;
    const it = items.find(i => String(i.id) === b.dataset.id);
    if (opts.multi) { it.on = !it.on; b.classList.toggle('on', it.on); opts.onPick(it.id, it); }
    else { closePop(); opts.onPick(it.id, it); }
  };
  if (opts.search) { const inp = $('.inp', p); inp.oninput = () => render(inp.value); setTimeout(() => inp.focus(), 0); }
  POP = { el: p, out: e => { if (!p.contains(e.target) && e.target !== anchor && !anchor.contains(e.target)) closePop(); } };
  document.addEventListener('mousedown', POP.out, true);
  p.addEventListener('keydown', e => { if (e.key === 'Escape') { closePop(); anchor.focus(); e.stopPropagation(); } });
}
const statusItems = cur => S.boot.statuses.map(s => ({ id: s, text: SL[s], on: cur === s, html: `${stIcon(s)}${SL[s]}` }));
const priorityItems = cur => PL.map((n, i) => ({ id: i, text: n, on: cur === i, html: `${prIcon(i)}${n}` }));
const assigneeItems = cur => [{ id: 'none', text: 'Unassigned', on: cur == null, html: `${av(null)}Unassigned` }, ...S.boot.users.map(u => ({ id: u.id, text: u.name, on: cur === u.id, html: `${av(u.id)}${esc(u.name)}` }))];
const labelItems = cur => S.boot.labels.map(l => ({ id: l.id, text: l.name, on: cur.includes(l.id), html: `<span class="chip" style="--c:${l.color}"><i></i>${esc(l.name)}</span>` }));

// ---------------------------------------------------------------- shell
function renderLogin() {
  closePop(); closeOverlay();
  S.view = null;
  const app = $('#app');
  app.innerHTML = `<div class="login"><div class="box">
    <div class="wm">${MARK}<div><h1>Tessera</h1><p>Issue tracking for the Kube32 team</p></div></div>
    <form id="lf" autocomplete="off">
      <label>Sign in as</label>
      <div class="users" id="users">${'<span class="sk"></span>'.repeat(8)}</div>
      <label for="un">Username</label><input class="inp" id="un" name="username" autocapitalize="off" spellcheck="false" required>
      <label for="pw">Password</label><input class="inp" id="pw" name="password" type="password" value="demo" required>
      <div class="err" id="lerr"></div>
      <button class="btn pri" type="submit">Sign in</button>
    </form>
    <p class="note">Demo accounts use the password <kbd>demo</kbd>.</p>
  </div></div>`;
  const grid = $('#users'), un = $('#un'), pw = $('#pw'), err = $('#lerr');
  const fillUsers = () => {
    grid.innerHTML = S.boot.users.map(u => `<button type="button" data-u="${esc(u.username)}">${av(u.id, 'lg')}<span>${esc(u.name.split(' ')[0])}</span></button>`).join('');
    grid.onclick = e => { const b = e.target.closest('button'); if (!b) return; $$('button', grid).forEach(x => x.classList.toggle('on', x === b)); un.value = b.dataset.u; (pw.value ? $('button[type=submit]', app) : pw).focus(); };
  };
  if (S.boot) fillUsers(); else loadBoot().then(fillUsers).catch(() => { err.textContent = 'Could not reach the server.'; });
  $('#lf').onsubmit = async e => {
    e.preventDefault(); err.textContent = '';
    const btn = $('button[type=submit]', app); btn.disabled = true;
    try {
      const r = await api('/api/auth/login', { method: 'POST', body: { username: un.value.trim(), password: pw.value }, quiet: true });
      S.me = r.user; enterApp();
    } catch (ex) { err.textContent = ex.status === 401 ? 'Wrong username or password.' : 'Sign in failed. ' + (ex.message || ''); }
    btn.disabled = false;
  };
}
async function loadBoot() {
  const b = await api('/api/bootstrap', { quiet: true });
  S.boot = b; S.users = {}; S.labels = {}; S.projects = {};
  b.users.forEach(u => S.users[u.id] = u); b.labels.forEach(l => S.labels[l.id] = l); b.projects.forEach(p => S.projects[p.id] = p);
  if (S.project && !S.projects[S.project]) S.project = 0;
  return b;
}
function enterApp() {
  renderShell(); connectSSE(); route();
}
function projLabel() { const p = S.projects[S.project]; return p ? `<i class="dot" style="background:${p.color}"></i><span>${esc(p.name)}</span>` : `<i class="dot"></i><span>All projects</span>`; }
function renderShell() {
  const nav = [['board', 'Board', 'board'], ['issues', 'Issues', 'list'], ['analytics', 'Analytics', 'chart'], ['activity', 'Activity', 'pulse']];
  $('#app').innerHTML = `<div class="shell">
    <aside class="side">
      <a class="brand" href="#/board">${MARK}Tessera</a>
      <button class="proj" id="projbtn">${projLabel()}${icon('chev')}</button>
      <nav class="nav" id="nav">
        ${nav.map(([r, n, i]) => `<a href="#/${r}" data-r="${r}">${icon(i)}${n}${r === 'issues' ? `<span class="cnt" id="opencnt">${fmt(S.boot.counts.open)}</span>` : ''}</a>`).join('')}
        <h6>Settings</h6>
        <a href="#/settings" data-r="settings" class="sub">${icon('user')}Account</a>
        <a href="#/settings/infra" data-r="settings/infra">${icon('cpu')}Infrastructure</a>
      </nav>
      <div class="foot">${av(S.me.id)}<span>${esc(S.me.name)}</span></div>
    </aside>
    <header class="top">
      <div class="crumb" id="crumb"></div>
      <button class="proj" id="projbtn2">${projLabel()}${icon('chev')}</button>
      <button class="searchbtn" id="searchbtn">${icon('search')}<span>Search</span><kbd>⌘K</kbd></button>
      <div class="live" id="live" title="Live updates"><i class="d"></i><span id="livetxt">connecting</span></div>
      <button class="btn pri newbtn" id="newbtn">${icon('plus')}<span>New issue</span><kbd>c</kbd></button>
      <a href="#/settings" title="Account">${av(S.me.id, 'lg')}</a>
    </header>
    <main class="main" id="main"></main>
  </div>`;
  const pick = e => popover(e.currentTarget, [{ id: 0, text: 'All projects', on: !S.project, html: '<i class="dot" style="width:8px;height:8px;border-radius:2px;background:var(--fg3)"></i>All projects' },
    ...S.boot.projects.map(p => ({ id: p.id, text: p.name, on: S.project === p.id, html: `<i style="width:8px;height:8px;border-radius:2px;background:${p.color}"></i>${esc(p.name)} <span class="key">${esc(p.key)}</span>` }))],
    { onPick: id => { S.project = +id; localStorage.setItem('ts.project', id); $$('.proj').forEach(b => b.innerHTML = projLabel() + icon('chev')); if (S.view && S.view.onProject) S.view.onProject(); } });
  $('#projbtn').onclick = pick; $('#projbtn2').onclick = pick;
  $('#searchbtn').onclick = openPalette;
  $('#newbtn').onclick = () => openNewIssue();
  setLive(S.live);
}
function setNav(r) {
  const cur = r.parts.join('/');
  $$('#nav a').forEach(a => a.classList.toggle('on', a.dataset.r === cur || (a.dataset.r === 'issues' && cur.startsWith('issue/'))));
}
function setCrumb(html) { const c = $('#crumb'); if (c) c.innerHTML = html; }
function setLive(state) {
  S.live = state; const l = $('#live'); if (!l) return;
  l.className = 'live ' + (state === 'on' ? 'on' : state === 'err' ? 'err' : '');
  $('#livetxt').textContent = state === 'on' ? (S.viewers > 1 ? `${S.viewers} viewing` : 'live') : state === 'err' ? 'reconnecting' : 'connecting';
}

// ---------------------------------------------------------------- SSE
function connectSSE() {
  clearTimeout(S.esRetry);
  if (S.es) S.es.close();
  const es = new EventSource('/events'); S.es = es; setLive('connecting');
  es.onopen = () => setLive('on');
  es.onerror = () => {
    setLive('err');
    if (es.readyState === EventSource.CLOSED) { // server refused (e.g. 401) -> manual retry
      S.esRetry = setTimeout(async () => {
        try { await api('/api/me'); if (S.me) connectSSE(); } catch (e) { /* onUnauthorized handled it */ }
      }, 3000);
    }
  };
  for (const t of ['issue', 'comment', 'hud', 'cluster', 'presence']) es.addEventListener(t, e => { let d; try { d = JSON.parse(e.data); } catch (x) { return; } onEvent(t, d); });
}
function onEvent(t, d) {
  if (t === 'presence') { S.viewers = d.viewers; setLive(S.live === 'on' ? 'on' : S.live); return; }
  if (t === 'cluster') { S.cluster = d; syncNow(d.now); for (const n of d.nodes) { const h = S.heap[n.name] = S.heap[n.name] || []; h.push(n.free_heap); if (h.length > 40) h.shift(); } }
  if (t === 'hud') {
    if (d.text) toast(d.text, { kind: 'hud', id: 'hud', ttl: 8000, title: 'Typed on the device keyboard' });
    else { const x = $('#toast-hud'); if (x) x.remove(); }
    return;
  }
  if (t === 'issue' && d.op === 'create' && d.actor !== S.me.id) toast(`${userOf(d.actor).name} created ${d.key}: ${d.title}`, { id: 'new-' + d.key, ttl: 5000 });
  if (S.view && S.view.onEvent) S.view.onEvent(t, d);
}

// ---------------------------------------------------------------- router
function parseHash() {
  const h = location.hash.slice(1) || '/board', i = h.indexOf('?');
  const path = i < 0 ? h : h.slice(0, i), qs = i < 0 ? '' : h.slice(i + 1);
  return { path, parts: path.split('/').filter(Boolean), q: new URLSearchParams(qs) };
}
const VIEWS = { board: BoardView, issues: IssuesView, issue: IssueView, analytics: AnalyticsView, activity: ActivityView, settings: SettingsView };
function route() {
  if (!S.me) return;
  const r = parseHash(); const name = r.parts[0] || 'board';
  if (!VIEWS[name]) { location.replace('#/board'); return; }
  const vname = name === 'settings' ? r.parts.join('/') : name;
  if (['board', 'issues', 'analytics', 'activity'].includes(name)) S.lastList = location.hash || '#/board';
  closePop();
  if (S.view && S.view.name === vname && S.view.update) { S.view.update(r); setNav(r); return; }
  if (S.view && S.view.unmount) S.view.unmount();
  S.gen++; if (S.ctl) S.ctl.abort(); S.ctl = new AbortController();
  const root = el('<div class="view"></div>');
  const main = $('#main'); main.replaceChildren(root); main.scrollTop = 0;
  S.view = VIEWS[name](root, r, S.ctl.signal); S.view.name = vname; setNav(r);
}
const go = h => { location.hash = h; };

// ---------------------------------------------------------------- shared renderers
function cardHtml(i) {
  return `<div class="card" draggable="true" data-key="${esc(i.key)}" data-status="${i.status}">
    <div class="key"><span>${esc(i.key)}</span>${i.comment_count ? `<span class="key" title="${i.comment_count} comments">${icon('msg')}</span>` : ''}${prIcon(i.priority)}</div>
    <div class="t">${esc(i.title)}</div>
    <div class="r">${i.labels.map(labelChip).join('')}${i.estimate != null ? `<span class="key">${i.estimate}pt</span>` : ''}${av(i.assignee)}</div></div>`;
}
function projQ(extra = '') { return (S.project ? `project=${S.project}&` : '') + extra; }

// ---------------------------------------------------------------- board
function BoardView(root, r, signal) {
  const cols = S.boot.statuses;
  setCrumb(`Board${S.project ? `<span class="sub">· ${esc(projectOf(S.project).name)}</span>` : ''}`);
  root.innerHTML = `<div class="vh"><h2>Board</h2><span class="meta" id="bmeta"></span><span class="sp"></span></div>
    <div class="board" id="board">${cols.map(s => `<div class="col" data-s="${s}"><div class="ch">${stIcon(s)}${SL[s]}<span class="n sk" style="width:18px"></span></div><div class="cards">${'<div class="card sk"></div>'.repeat(3)}</div></div>`).join('')}</div>`;
  let dragKey = null, loading = false, dirty = false;
  const col = s => $(`.col[data-s="${s}"] .cards`, root);
  const header = (s, n, trunc) => {
    const h = $(`.col[data-s="${s}"] .ch`, root);
    h.innerHTML = `${stIcon(s)}${SL[s]}<span class="n">${n}${trunc ? '+' : ''}</span>${trunc ? `<a class="more" href="#/issues?status=${s}">view all</a>` : ''}`;
  };
  async function load() {
    if (loading) { dirty = true; return; }
    loading = true;
    try {
      const d = await api(`/api/board?${projQ()}`, { signal });
      for (const s of cols) {
        const items = d.columns[s] || [];
        col(s).innerHTML = items.map(cardHtml).join('') || '<div class="empty" style="padding:20px 0;font-size:12px">No issues</div>';
        header(s, items.length, d.truncated && d.truncated[s]);
      }
      $('#bmeta', root).textContent = `${Object.values(d.columns).reduce((a, c) => a + c.length, 0)} issues shown · top ${d.limit} per column`;
    } catch (e) { if (!aborted(e) && e.status !== 401) toast('Could not load the board.', { kind: 'error' }); }
    loading = false;
    if (dirty) { dirty = false; load(); }
  }
  const reload = debounce(load, 400);
  load();
  const board = $('#board', root);
  board.addEventListener('click', e => { const c = e.target.closest('.card'); if (c && !c.classList.contains('sk')) go('#/issue/' + c.dataset.key); });
  board.addEventListener('dragstart', e => {
    const c = e.target.closest('.card'); if (!c) return;
    dragKey = c.dataset.key; c.classList.add('drag'); e.dataTransfer.effectAllowed = 'move'; e.dataTransfer.setData('text/plain', dragKey);
  });
  board.addEventListener('dragend', e => { const c = e.target.closest('.card'); if (c) c.classList.remove('drag'); $$('.col.over', board).forEach(x => x.classList.remove('over')); dragKey = null; });
  board.addEventListener('dragover', e => { const c = e.target.closest('.col'); if (!c || !dragKey) return; e.preventDefault(); e.dataTransfer.dropEffect = 'move'; if (!c.classList.contains('over')) { $$('.col.over', board).forEach(x => x.classList.remove('over')); c.classList.add('over'); } });
  board.addEventListener('dragleave', e => { const c = e.target.closest('.col'); if (c && !c.contains(e.relatedTarget)) c.classList.remove('over'); });
  board.addEventListener('drop', async e => {
    e.preventDefault();
    const c = e.target.closest('.col'); const key = dragKey || e.dataTransfer.getData('text/plain'); if (!c || !key) return;
    c.classList.remove('over');
    const card = $(`.card[data-key="${key}"]`, board); const to = c.dataset.s; if (!card || card.dataset.status === to) return;
    const from = card.dataset.status, fromCol = card.parentNode, next = card.nextSibling;
    moveCard(card, to);
    try { await api(`/api/issues/${key}`, { method: 'PATCH', body: { status: to } }); }
    catch (ex) { if (ex.status !== 401) { fromCol.insertBefore(card, next); card.dataset.status = from; recount(); toast(`Could not move ${key}.`, { kind: 'error' }); } }
  });
  function moveCard(card, to) {
    card.classList.remove('drag'); card.dataset.status = to;
    const target = col(to); const ph = $('.empty', target); if (ph) ph.remove();
    target.prepend(card); card.classList.remove('moved'); void card.offsetWidth; card.classList.add('moved'); recount();
  }
  function recount() { for (const s of cols) { const n = $(`.col[data-s="${s}"] .ch .n`, root); if (n && !n.textContent.includes('+')) n.textContent = $$('.card', col(s)).length; } }
  return {
    onProject() { setCrumb(`Board${S.project ? `<span class="sub">· ${esc(projectOf(S.project).name)}</span>` : ''}`); load(); },
    onEvent(t, d) {
      if (t !== 'issue') return;
      const card = $(`.card[data-key="${d.key}"]`, board);
      if (d.op === 'delete') { if (card) { card.remove(); recount(); } return; }
      if (d.op === 'update' && card) { $('.t', card).textContent = d.title; if (card.dataset.status !== d.status) moveCard(card, d.status); return; }
      reload();
    },
  };
}

// ---------------------------------------------------------------- issues list
function IssuesView(root, r, signal) {
  setCrumb('Issues');
  const COLS = [['key', 'Key', 'c-key'], ['priority', 'Pri', 'c-pr'], ['', 'Title', ''], ['', 'Labels', 'c-lab'], ['', 'Status', 'c-st'], ['', 'Assignee', 'c-as'], ['updated', 'Updated', 'c-up']];
  root.innerHTML = `<div class="vh"><h2>Issues</h2><span class="meta" id="lmeta"></span><span class="sp"></span></div>
    <div class="filters">
      <input class="inp" id="q" placeholder="Search title, description, comments" spellcheck="false">
      <button class="fbtn" data-f="status"></button><button class="fbtn" data-f="assignee"></button>
      <button class="fbtn" data-f="label"></button><button class="fbtn" data-f="priority"></button>
      <button class="btn ghost sm hide" id="clear">Clear</button>
    </div>
    <div class="vbody"><table class="tbl"><thead><tr>${COLS.map(([k, n, c]) => `<th class="${c}" ${k ? `data-sort="${k}"` : ''}>${n}</th>`).join('')}</tr></thead>
    <tbody id="rows"></tbody></table><div class="pager" id="pager"></div></div>`;
  let st = {}, ctl = null, items = [], sel = -1, total = 0, loading = false, dirty = false;
  const q = $('#q', root), rows = $('#rows', root);
  const skel = () => { rows.innerHTML = Array.from({ length: Math.min(50, Math.max(5, items.length || 20)) }, () => `<tr><td class="c-key"><span class="sk" style="width:48px"></span></td><td class="c-pr"></td><td><span class="sk" style="width:${40 + Math.random() * 40}%"></span></td><td class="c-lab"></td><td class="c-st"><span class="sk" style="width:60px"></span></td><td class="c-as"><span class="sk" style="width:80px"></span></td><td class="c-up"></td></tr>`).join(''); };
  const readState = rr => { const g = k => rr.q.get(k) || ''; st = { status: g('status') ? g('status').split(',') : [], assignee: g('assignee'), label: g('label'), priority: g('priority'), q: g('q'), sort: g('sort') || 'updated', order: g('order') || 'desc', page: +g('page') || 1 }; };
  const writeState = (replace) => {
    const p = new URLSearchParams();
    if (st.status.length) p.set('status', st.status.join(','));
    for (const k of ['assignee', 'label', 'priority', 'q']) if (st[k]) p.set(k, st[k]);
    if (st.sort !== 'updated') p.set('sort', st.sort); if (st.order !== 'desc') p.set('order', st.order); if (st.page > 1) p.set('page', st.page);
    const h = '#/issues' + (p.toString() ? '?' + p : '');
    if (h === location.hash) return;
    if (replace) history.replaceState(null, '', h); else location.hash = h;
    S.lastList = h; if (replace) load();
  };
  function paintFilters() {
    const fb = f => $(`.fbtn[data-f="${f}"]`, root);
    const set = (f, on, html) => { const b = fb(f); b.className = 'fbtn' + (on ? ' on' : ''); b.innerHTML = html + (on ? `<span class="x" data-x="${f}">${icon('x')}</span>` : ''); };
    set('status', st.status.length, st.status.length ? st.status.map(s => stIcon(s)).join('') + (st.status.length === 1 ? SL[st.status[0]] : `${st.status.length} statuses`) : icon('plus') + 'Status');
    set('assignee', st.assignee, st.assignee ? (st.assignee === 'none' ? av(null) + 'Unassigned' : av(+st.assignee) + esc(userOf(+st.assignee).name)) : icon('plus') + 'Assignee');
    set('label', st.label, st.label ? labelChip(+st.label) : icon('plus') + 'Label');
    set('priority', st.priority, st.priority ? prIcon(+st.priority) + PL[+st.priority] : icon('plus') + 'Priority');
    $('#clear', root).classList.toggle('hide', !(st.status.length || st.assignee || st.label || st.priority || st.q));
    if (q.value !== st.q) q.value = st.q;
    $$('th[data-sort]', root).forEach(th => { th.classList.toggle('on', th.dataset.sort === st.sort); th.innerHTML = COLS.find(c => c[0] === th.dataset.sort)[1] + (th.dataset.sort === st.sort ? icon(st.order === 'asc' ? 'up' : 'chev') : ''); });
  }
  async function load() {
    if (ctl) ctl.abort(); ctl = new AbortController();
    const sig = ctl.signal; signal.addEventListener('abort', () => ctl.abort(), { once: true });
    const p = new URLSearchParams(); if (S.project) p.set('project', S.project);
    if (st.status.length) p.set('status', st.status.join(','));
    for (const k of ['assignee', 'label', 'priority', 'q', 'sort', 'order', 'page']) if (st[k]) p.set(k, st[k]);
    p.set('limit', 50);
    paintFilters(); skel();
    try {
      const d = await api('/api/issues?' + p, { signal: sig });
      items = d.items; total = d.total;
      rows.innerHTML = items.map((i, n) => `<tr class="row ${n === sel ? 'sel' : ''}" data-key="${esc(i.key)}">
        <td class="c-key key">${esc(i.key)}</td><td class="c-pr">${prIcon(i.priority)}</td><td class="title">${esc(i.title)}</td>
        <td class="c-lab"><div class="labels">${i.labels.map(labelChip).join('')}</div></td><td class="c-st">${stPill(i.status)}</td>
        <td class="c-as"><span class="pill">${av(i.assignee)}${esc(userOf(i.assignee).name)}</span></td><td class="c-up dim" title="${abs(i.updated)}">${rel(i.updated)}</td></tr>`).join('')
        || `<tr><td colspan="7"><div class="empty"><b>No issues match</b>Try removing a filter.</div></td></tr>`;
      const a = (d.page - 1) * d.limit + 1, b = Math.min(total, d.page * d.limit);
      $('#lmeta', root).textContent = `${fmt(total)} issues`;
      $('#pager', root).innerHTML = total ? `<span>${a}–${b} of ${fmt(total)}</span><span class="sp"></span>
        <button class="btn sm" id="prev" ${d.page <= 1 ? 'disabled' : ''}>Previous</button><button class="btn sm" id="next" ${b >= total ? 'disabled' : ''}>Next</button>` : '';
      if (total) { $('#prev', root).onclick = () => { st.page--; writeState(); }; $('#next', root).onclick = () => { st.page++; writeState(); }; }
    } catch (e) { if (!aborted(e) && e.status !== 401) rows.innerHTML = `<tr><td colspan="7"><div class="empty"><b>Could not load issues</b>${esc(e.message)}</div></td></tr>`; }
  }
  readState(r); load();
  q.oninput = debounce(() => { st.q = q.value.trim(); st.page = 1; writeState(true); }, 250);
  $('.filters', root).onclick = e => {
    const x = e.target.closest('[data-x]'); if (x) { e.stopPropagation(); const f = x.dataset.x; st[f] = f === 'status' ? [] : ''; st.page = 1; writeState(); return; }
    const b = e.target.closest('.fbtn'); if (!b) return;
    const f = b.dataset.f;
    if (f === 'status') popover(b, statusItems().map(i => ({ ...i, on: st.status.includes(i.id) })), { multi: true, onPick: (id, it) => { st.status = it.on ? [...st.status, id] : st.status.filter(s => s !== id); st.page = 1; writeState(); } });
    else if (f === 'assignee') popover(b, assigneeItems(st.assignee === 'none' ? null : +st.assignee || undefined), { search: 'Filter people', onPick: id => { st.assignee = String(id); st.page = 1; writeState(); } });
    else if (f === 'label') popover(b, labelItems(st.label ? [+st.label] : []), { search: 'Filter labels', onPick: id => { st.label = String(id); st.page = 1; writeState(); } });
    else if (f === 'priority') popover(b, priorityItems(st.priority === '' ? -1 : +st.priority), { onPick: id => { st.priority = String(id); st.page = 1; writeState(); } });
  };
  $('#clear', root).onclick = () => { st = { ...st, status: [], assignee: '', label: '', priority: '', q: '', page: 1 }; writeState(); };
  $('thead', root).onclick = e => { const th = e.target.closest('th[data-sort]'); if (!th) return; const k = th.dataset.sort; if (st.sort === k) st.order = st.order === 'asc' ? 'desc' : 'asc'; else { st.sort = k; st.order = k === 'key' ? 'asc' : 'desc'; } st.page = 1; writeState(); };
  rows.onclick = e => { const tr = e.target.closest('tr.row'); if (tr) go('#/issue/' + tr.dataset.key); };
  const reload = debounce(load, 500);
  return {
    update(rr) { readState(rr); load(); },
    onProject() { st.page = 1; load(); },
    onEvent(t) { if (t === 'issue') reload(); },
    onKey(e) {
      if (e.key === 'j' || e.key === 'k' || e.key === 'ArrowDown' || e.key === 'ArrowUp') {
        if (!items.length) return; e.preventDefault();
        sel = Math.max(0, Math.min(items.length - 1, sel + (e.key === 'j' || e.key === 'ArrowDown' ? 1 : -1)));
        $$('tr.row', rows).forEach((tr, i) => tr.classList.toggle('sel', i === sel));
        const tr = $$('tr.row', rows)[sel]; if (tr) tr.scrollIntoView({ block: 'nearest' });
      } else if (e.key === 'Enter' && sel >= 0 && items[sel]) go('#/issue/' + items[sel].key);
      else if (e.key === 'Escape') { sel = -1; $$('tr.row.sel', rows).forEach(t => t.classList.remove('sel')); }
    },
  };
}

// ---------------------------------------------------------------- issue page
function IssueView(root, r, signal) {
  let key = r.parts[1], issue = null, ctl = null;
  const grow = ta => { ta.style.height = 'auto'; ta.style.height = ta.scrollHeight + 2 + 'px'; };
  function skeleton() {
    setCrumb(`<a href="${S.lastList}">Issues</a><span class="sub">/ ${esc(key)}</span>`);
    root.innerHTML = `<div class="issue"><div class="doc"><div class="bc"><a href="${S.lastList}">${icon('back')}</a><span class="key">${esc(key)}</span></div>
      <span class="sk" style="height:22px;width:70%;margin:6px 0 12px"></span><span class="sk" style="width:95%"></span><span class="sk" style="width:80%;margin-top:8px"></span><span class="sk" style="width:60%;margin-top:8px"></span></div>
      <div class="props">${'<span class="sk" style="width:100%;height:14px;margin-bottom:14px"></span>'.repeat(6)}</div></div>`;
  }
  async function load() {
    if (ctl) ctl.abort(); ctl = new AbortController(); signal.addEventListener('abort', () => ctl.abort(), { once: true });
    try {
      const d = await api(`/api/issues/${encodeURIComponent(key)}`, { signal: ctl.signal });
      const first = !issue; issue = d;
      if (first) render(); else refresh();
    } catch (e) {
      if (aborted(e) || e.status === 401) return;
      root.innerHTML = `<div class="empty"><b>${e.status === 404 ? `${esc(key)} does not exist` : 'Could not load issue'}</b><a href="${S.lastList}" class="btn sm" style="margin-top:12px">Back</a></div>`;
    }
  }
  function actLine(a) {
    const u = userOf(a.actor); let what;
    if (a.op === 'create') what = 'created the issue';
    else if (a.op === 'comment') what = 'commented';
    else if (a.op === 'delete') what = 'deleted the issue';
    else if (a.field === 'status') what = `moved ${stPill(a.from)} ${icon('arrow')} ${stPill(a.to)}`;
    else if (a.field === 'assignee') what = a.to == null ? 'removed the assignee' : `assigned <b>${esc(userOf(a.to).name)}</b>`;
    else if (a.field === 'priority') what = `set priority to <b>${PL[a.to] || a.to}</b>`;
    else if (a.field === 'labels') what = `changed labels to ${(a.to || []).map(labelChip).join(' ') || '<i>none</i>'}`;
    else if (a.field === 'estimate') what = a.to == null ? 'cleared the estimate' : `set estimate to <b>${a.to}</b>`;
    else if (a.field === 'title') what = 'renamed the issue';
    else if (a.field === 'project') what = 'moved the issue to another project';
    else what = `edited the ${esc(a.field || 'issue')}`;
    return `<b>${esc(u.name)}</b> ${what}`;
  }
  const commentHtml = c => { const u = userOf(c.author); return `<div class="cm ${c.author === 0 ? 'hud' : ''}" data-id="${c.id}">${av(c.author, 'lg')}<div class="b"><div class="h"><b>${esc(u.name)}</b><span title="${abs(c.created)}">${rel(c.created, true)}</span>${c.author === 0 ? `<span>${icon('kb')}</span>` : ''}</div><p>${esc(c.body)}</p></div></div>`; };
  function propsHtml() {
    const i = issue;
    return `<div class="prop"><label>Status</label><button class="pb" data-p="status">${stIcon(i.status)}${SL[i.status]}</button></div>
      <div class="prop"><label>Priority</label><button class="pb" data-p="priority">${prIcon(i.priority)}${PL[i.priority]}</button></div>
      <div class="prop"><label>Assignee</label><button class="pb" data-p="assignee">${av(i.assignee)}${esc(userOf(i.assignee).name)}</button></div>
      <div class="prop"><label>Labels</label><button class="pb" data-p="labels"><span class="lbls">${i.labels.map(labelChip).join('') || '<span class="v">None</span>'}</span></button></div>
      <div class="prop"><label>Estimate</label><input class="inp" id="est" type="number" min="0" max="999" placeholder="—" value="${i.estimate ?? ''}"></div>
      <div class="div"></div>
      <div class="prop"><label>Project</label><span class="v"><i class="dot" style="display:inline-block;width:8px;height:8px;border-radius:2px;background:${projectOf(i.project).color};margin-right:6px"></i>${esc(projectOf(i.project).name)}</span></div>
      <div class="prop"><label>Reporter</label><span class="v pill">${av(i.reporter)}${esc(userOf(i.reporter).name)}</span></div>
      <div class="prop"><label>Created</label><span class="v" title="${abs(i.created)}">${rel(i.created, true)}</span></div>
      <div class="prop"><label>Updated</label><span class="v" title="${abs(i.updated)}">${rel(i.updated, true)}</span></div>
      <div class="div"></div>
      <h4 style="font-size:12px;color:var(--fg2);margin-bottom:8px">Activity</h4>
      <div class="tl">${i.activity.map(a => `<div class="ev"><i></i><span>${actLine(a)}</span><span class="tm" title="${abs(a.ts)}">${rel(a.ts)}</span></div>`).join('') || '<div class="v">No activity yet</div>'}</div>`;
  }
  function render() {
    const i = issue;
    setCrumb(`<a href="${S.lastList}">Issues</a><span class="sub">/ ${esc(i.key)}</span>`);
    root.innerHTML = `<div class="issue"><div class="doc">
      <div class="bc"><a href="${S.lastList}" title="Back">${icon('back')}</a><span class="key">${esc(i.key)}</span><span>·</span><span>${esc(projectOf(i.project).name)}</span><span class="sp"></span>
        <button class="btn ghost sm" id="more" title="More">${icon('dots')}</button></div>
      <textarea class="ttl" id="title" rows="1" maxlength="120" placeholder="Issue title" spellcheck="false">${esc(i.title)}</textarea>
      <textarea class="desc" id="desc" maxlength="1000" placeholder="Add a description…">${esc(i.description)}</textarea>
      <div class="saved" id="saved"></div>
      <div class="sec"><h4>Comments <span class="n">${i.comments.length}</span></h4><div id="comments">${i.comments.map(commentHtml).join('')}</div>
        <div class="composer">${av(S.me.id, 'lg')}<div class="b"><textarea class="ta" id="cbody" maxlength="280" placeholder="Leave a comment…"></textarea>
        <div class="row"><span id="ccount"></span><span><kbd>⌘</kbd>+<kbd>↵</kbd> to send</span><button class="btn sm pri" id="csend">Comment</button></div></div></div></div>
    </div><aside class="props" id="props">${propsHtml()}</aside></div>`;
    const title = $('#title', root), desc = $('#desc', root), saved = $('#saved', root);
    [title, desc].forEach(t => { grow(t); t.addEventListener('input', () => grow(t)); });
    requestAnimationFrame(() => [title, desc].forEach(grow));
    const flash = txt => { saved.textContent = txt; saved.style.opacity = 1; setTimeout(() => saved.style.opacity = 0, 1800); };
    title.addEventListener('keydown', e => { if (e.key === 'Enter') { e.preventDefault(); title.blur(); } });
    title.addEventListener('blur', () => { const v = title.value.trim(); if (!v) { title.value = issue.title; grow(title); return; } if (v !== issue.title) patch({ title: v }, () => flash('Saved')); });
    desc.addEventListener('blur', () => { const v = desc.value; if (v !== issue.description) patch({ description: v }, () => flash('Saved')); });
    $('#more', root).onclick = e => popover(e.currentTarget, [{ id: 'copy', text: 'Copy link', html: icon('arrow') + 'Copy link' }, { id: 'del', text: 'Delete', html: `<span style="color:var(--red)">${icon('trash')}Delete issue</span>` }], {
      onPick: async id => {
        if (id === 'copy') { try { await navigator.clipboard.writeText(location.href); toast('Link copied'); } catch (x) { toast(location.href, { ttl: 8000 }); } }
        if (id === 'del' && confirm(`Delete ${issue.key}? This cannot be undone.`)) { try { await api(`/api/issues/${issue.key}`, { method: 'DELETE' }); toast(`${issue.key} deleted`, { kind: 'ok' }); go(S.lastList); } catch (x) { /* toasted */ } }
      },
    });
    const cb = $('#cbody', root), cc = $('#ccount', root);
    cb.addEventListener('input', () => cc.textContent = cb.value.length > 200 ? `${cb.value.length}/280` : '');
    cb.addEventListener('keydown', e => { if (e.key === 'Enter' && (e.metaKey || e.ctrlKey)) { e.preventDefault(); send(); } });
    $('#csend', root).onclick = send;
    async function send() {
      const body = cb.value.trim(); if (!body) return;
      const btn = $('#csend', root); btn.disabled = true;
      try {
        const c = await api(`/api/issues/${issue.key}/comments`, { method: 'POST', body: { body } });
        cb.value = ''; cc.textContent = ''; issue.comments.push(c); issue.comment_count++;
        $('#comments', root).insertAdjacentHTML('beforeend', commentHtml(c)); $('.sec h4 .n', root).textContent = issue.comments.length;
      } catch (x) { if (x.status === 400) toast(x.message, { kind: 'error' }); }
      btn.disabled = false;
    }
    bindProps();
  }
  function bindProps() {
    const props = $('#props', root);
    props.onclick = e => {
      const b = e.target.closest('.pb'); if (!b) return; const p = b.dataset.p, i = issue;
      if (p === 'status') popover(b, statusItems(i.status), { onPick: id => patch({ status: id }) });
      else if (p === 'priority') popover(b, priorityItems(i.priority), { onPick: id => patch({ priority: +id }) });
      else if (p === 'assignee') popover(b, assigneeItems(i.assignee), { search: 'Assign to…', onPick: id => patch({ assignee: id === 'none' ? null : +id }) });
      else if (p === 'labels') { const cur = [...i.labels]; popover(b, labelItems(cur), { multi: true, width: 220, onPick: (id, it) => { const n = it.on ? [...cur, +id] : cur.filter(x => x !== +id); cur.length = 0; cur.push(...n); patch({ labels: [...cur].sort((a, b2) => a - b2) }); } }); }
    };
    const est = $('#est', props);
    est.onchange = () => { const v = est.value === '' ? null : Math.max(0, +est.value | 0); if (v !== issue.estimate) patch({ estimate: v }); };
  }
  async function patch(fields, after) {
    try {
      const d = await api(`/api/issues/${issue.key}`, { method: 'PATCH', body: fields });
      Object.assign(issue, d, { comments: issue.comments, activity: issue.activity });
      if (after) after();
      load(); // pull fresh activity (cheap: one request)
    } catch (e) { if (e.status === 400) toast(e.message, { kind: 'error' }); else if (e.status !== 401) load(); }
  }
  function refresh() {
    // Re-render everything except fields the user is editing.
    const i = issue, a = document.activeElement;
    const title = $('#title', root), desc = $('#desc', root);
    if (title && a !== title && title.value !== i.title) { title.value = i.title; grow(title); }
    if (desc && a !== desc && desc.value !== i.description) { desc.value = i.description; grow(desc); }
    if ($('#comments', root)) { $('#comments', root).innerHTML = i.comments.map(commentHtml).join(''); $('.sec h4 .n', root).textContent = i.comments.length; }
    const props = $('#props', root); if (props && !props.contains(a)) { props.innerHTML = propsHtml(); bindProps(); }
    setCrumb(`<a href="${S.lastList}">Issues</a><span class="sub">/ ${esc(i.key)}</span>`);
  }
  skeleton(); load();
  const reload = debounce(load, 300);
  return {
    update(rr) { if (rr.parts[1] !== key) { key = rr.parts[1]; issue = null; skeleton(); load(); } },
    onEvent(t, d) {
      if (!issue || d.key !== issue.key) return;
      if (t === 'issue' && d.op === 'delete') { toast(`${issue.key} was deleted by ${userOf(d.actor).name}`, { kind: 'error' }); go(S.lastList); return; }
      if (t === 'issue' || t === 'comment') reload();
    },
  };
}

// ---------------------------------------------------------------- analytics
function AnalyticsView(root, r, signal) {
  let days = +(localStorage.getItem('ts.days') || 14);
  const title = () => setCrumb(`Analytics${S.project ? `<span class="sub">· ${esc(projectOf(S.project).name)}</span>` : ''}`);
  title();
  root.innerHTML = `<div class="vh"><h2>Analytics</h2><span class="meta" id="ameta"></span><span class="sp"></span>
    <div id="days">${[7, 14, 30].map(d => `<button class="btn sm ${d === days ? '' : 'ghost'}" data-d="${d}">${d}d</button>`).join('')}</div></div>
    <div class="vbody"><div class="an" id="an">${['c8', 'c4', 'c4', 'c5', 'c3', 'c12'].map(c => `<div class="g ${c}"><span class="sk" style="width:120px;margin-bottom:14px"></span><span class="sk" style="width:100%;height:${c === 'c12' ? 60 : 160}px"></span></div>`).join('')}</div></div>`;
  $('#days', root).onclick = e => { const b = e.target.closest('[data-d]'); if (!b) return; days = +b.dataset.d; localStorage.setItem('ts.days', days); $$('button', e.currentTarget).forEach(x => x.classList.toggle('ghost', +x.dataset.d !== days)); load(); };
  const SC = { backlog: '#6b7483', todo: '#8ea2c9', in_progress: '#f2a93b', review: '#a173e8', done: '#3ec98a' };
  const tip = (t) => `data-tip="${esc(t)}"`;
  function ticks(max) { const raw = max / 4, mag = Math.pow(10, Math.floor(Math.log10(raw || 1))), n = [1, 2, 5, 10].map(x => x * mag).find(x => x >= raw) || mag; const out = []; for (let v = 0; v <= max + n - 1; v += n) out.push(v); return out; }
  function bars(d, W) {
    const H = 190, L = 30, B = 22, T = 8, n = days, iw = W - L - 6, ih = H - T - B;
    const max = Math.max(1, ...d.created_per_day, ...d.resolved_per_day), tk = ticks(max), top = tk[tk.length - 1];
    const y = v => T + ih - v / top * ih, gw = iw / n, bw = Math.max(2, gw * 0.34);
    const date = i => new Date(now() - (n - 1 - i) * 864e5), every = n > 20 ? 5 : n > 10 ? 2 : 1;
    return `<svg viewBox="0 0 ${W} ${H}" width="${W}" height="${H}" style="width:${W}px">
      ${tk.map(v => `<line class="gl" x1="${L}" x2="${W}" y1="${y(v)}" y2="${y(v)}"/><text class="ax" x="${L - 6}" y="${y(v) + 3}" text-anchor="end">${v}</text>`).join('')}
      ${d.created_per_day.map((c, i) => { const x = L + i * gw + gw / 2, rs = d.resolved_per_day[i], dt = date(i).toLocaleDateString(undefined, { month: 'short', day: 'numeric' });
        return `<rect x="${x - bw - 1}" y="${y(c)}" width="${bw}" height="${ih - (y(c) - T)}" fill="#5d6778" rx="1" ${tip(`${dt} · created ${c}`)}/><rect x="${x + 1}" y="${y(rs)}" width="${bw}" height="${ih - (y(rs) - T)}" fill="#f2a93b" rx="1" ${tip(`${dt} · resolved ${rs}`)}/>${i % every === 0 ? `<text class="ax" x="${x}" y="${H - 6}" text-anchor="middle">${dt}</text>` : ''}`; }).join('')}
    </svg>`;
  }
  function donut(d) {
    const tot = Object.values(d.by_status).reduce((a, b) => a + b, 0) || 1, R = 46, C = 2 * Math.PI * R; let off = 0;
    const segs = S.boot.statuses.map(s => { const v = d.by_status[s], len = v / tot * C, o = off; off += len; return `<circle r="${R}" cx="60" cy="60" fill="none" stroke="${SC[s]}" stroke-width="13" stroke-dasharray="${Math.max(0, len - 1.5)} ${C}" stroke-dashoffset="${-o}" transform="rotate(-90 60 60)" ${tip(`${SL[s]} · ${v}`)}/>`; }).join('');
    return `<div style="display:flex;align-items:center;gap:20px"><svg viewBox="0 0 120 120" style="width:120px;flex:none">${segs}<text x="60" y="57" text-anchor="middle" fill="var(--fg)" font-size="22" font-family="var(--mono)" font-weight="500">${tot - d.by_status.done}</text><text x="60" y="73" text-anchor="middle" class="ax">open</text></svg>
      <div style="flex:1;font-size:12px">${S.boot.statuses.map(s => `<div style="display:flex;align-items:center;gap:8px;padding:3px 0"><i style="width:8px;height:8px;border-radius:2px;background:${SC[s]}"></i><span style="color:var(--fg2);flex:1">${SL[s]}</span><span class="mono" style="color:var(--fg3)">${Math.round(d.by_status[s] / tot * 100)}%</span><span class="mono" style="width:40px;text-align:right">${d.by_status[s]}</span></div>`).join('')}</div></div>`;
  }
  function assignees(d) {
    const rows = d.by_assignee.filter(a => S.users[a.user]).sort((a, b) => (b.open + b.done) - (a.open + a.done));
    const max = Math.max(1, ...rows.map(a => a.open + a.done)), W = 420, RH = 24, L = 120, iw = W - L - 56;
    return `<svg viewBox="0 0 ${W} ${rows.length * RH}" style="height:${rows.length * RH}px">${rows.map((a, i) => { const u = userOf(a.user), y = i * RH, ow = a.open / max * iw, dw = a.done / max * iw;
      return `<circle cx="9" cy="${y + 12}" r="8" fill="${u.color}"/><text x="9" y="${y + 15}" text-anchor="middle" font-size="7" font-weight="700" fill="#0b0b0b">${esc(u.initials)}</text><text x="24" y="${y + 16}" fill="var(--fg2)" font-size="11.5">${esc(u.name.split(' ')[0])}</text>
      <rect x="${L}" y="${y + 6}" width="${ow}" height="12" fill="#f2a93b" rx="1" ${tip(`${u.name} · ${a.open} open`)}/><rect x="${L + ow + 1}" y="${y + 6}" width="${dw}" height="12" fill="#3d4654" rx="1" ${tip(`${u.name} · ${a.done} done`)}/>
      <text class="ax" x="${W}" y="${y + 16}" text-anchor="end"><tspan fill="var(--fg)">${a.open}</tspan> / ${a.done}</text>`; }).join('')}</svg>`;
  }
  function age(d) {
    const b = d.open_age_buckets, keys = [['lt1d', '< 1 day', '#3ec98a'], ['lt7d', '1–7 days', '#8ea2c9'], ['lt30d', '7–30 days', '#f2a93b'], ['gte30d', '30+ days', '#e5484d']], tot = Math.max(1, keys.reduce((a, k) => a + b[k[0]], 0));
    let x = 0;
    return `<svg viewBox="0 0 600 14" preserveAspectRatio="none" style="height:14px">${keys.map(([k, n, c]) => { const w = b[k] / tot * 600, s = `<rect x="${x}" y="0" width="${Math.max(0, w - 1)}" height="14" fill="${c}" rx="1" ${tip(`${n} · ${b[k]}`)}/>`; x += w; return s; }).join('')}</svg>
      <div class="segl">${keys.map(([k, n, c]) => `<div><i style="background:${c}"></i>${n}<b>${b[k]}</b></div>`).join('')}</div>`;
  }
  function cycle(d) {
    const { p50, p90 } = d.cycle_time_hours, max = Math.max(p90 * 1.15, 1), fmtH = h => h >= 48 ? `${(h / 24).toFixed(1)}<small>d</small>` : `${h.toFixed(h < 10 ? 1 : 0)}<small>h</small>`;
    return `<div class="big"><div><div class="k">${fmtH(p50)}</div><div class="l">p50 · median</div></div><div><div class="k">${fmtH(p90)}</div><div class="l">p90</div></div></div>
      <svg viewBox="0 0 400 34" preserveAspectRatio="none" style="height:34px;margin-top:16px"><rect x="0" y="14" width="400" height="6" fill="var(--bg3)" rx="3"/><rect x="0" y="14" width="${p90 / max * 400}" height="6" fill="#3d4654" rx="3"/><rect x="0" y="14" width="${p50 / max * 400}" height="6" fill="#f2a93b" rx="3"/>
      <line x1="${p50 / max * 400}" x2="${p50 / max * 400}" y1="8" y2="26" stroke="#f2a93b" stroke-width="2"/><line x1="${p90 / max * 400}" x2="${p90 / max * 400}" y1="8" y2="26" stroke="var(--fg2)" stroke-width="2"/></svg>
      <div style="font-size:11px;color:var(--fg3);margin-top:6px">Hours from creation to done, resolved issues in scope.</div>`;
  }
  function prio(d) {
    const max = Math.max(1, ...d.by_priority), W = 300, H = 96;
    return `<svg viewBox="0 0 ${W} ${H}" style="height:${H}px;max-width:300px">${d.by_priority.map((v, i) => { const bh = Math.max(1, v / max * 56), x = i * 60 + 10; return `<rect x="${x}" y="${74 - bh}" width="40" height="${bh}" rx="2" fill="${i === 4 ? '#e5484d' : i === 0 ? '#3d4654' : '#8ea2c9'}" ${tip(`${PL[i]} · ${v}`)}/><text class="ax" x="${x + 20}" y="${88}" text-anchor="middle">${PL[i].split(' ')[0]}</text><text class="ax" x="${x + 20}" y="${69 - bh}" text-anchor="middle" fill="var(--fg)">${v}</text>`; }).join('')}</svg>`;
  }
  let data = null;
  const barsW = () => Math.max(300, ($('#an .c8', root) || $('#an', root)).clientWidth);
  const paintBars = () => { const b = $('#bars', root); if (b && data) b.innerHTML = bars(data, barsW()); };
  const onResize = debounce(paintBars, 150);
  addEventListener('resize', onResize);
  async function load() {
    try {
      const d = await api(`/api/analytics?${projQ(`days=${days}`)}`, { signal });
      data = d;
      const g = (cls, name, body, legend = '') => `<div class="g ${cls}"><h4>${name}${legend ? `<span class="lg">${legend}</span>` : ''}</h4>${body}</div>`;
      $('#an', root).innerHTML =
        g('c8', `Created vs resolved · last ${days} days`, `<div id="bars" style="overflow:hidden">${bars(d, barsW())}</div>`, '<span><i style="background:#5d6778"></i>created</span><span><i style="background:#f2a93b"></i>resolved</span>') +
        g('c4', 'Cycle time', cycle(d)) +
        g('c4', 'By status', donut(d)) +
        g('c5', 'Workload by assignee', assignees(d), '<span><i style="background:#f2a93b"></i>open</span><span><i style="background:#3d4654"></i>done</span>') +
        g('c3', 'By priority', prio(d)) +
        g('c12', 'Age of open issues', age(d));
      $('#ameta', root).textContent = `${d.by_status.done} resolved · ${Object.values(d.by_status).reduce((a, b) => a + b, 0) - d.by_status.done} open`;
    } catch (e) { if (!aborted(e) && e.status !== 401) $('#an', root).innerHTML = `<div class="empty c12"><b>Could not load analytics</b>${esc(e.message)}</div>`; }
  }
  load();
  // hover tooltips for any [data-tip]
  let tipEl = null;
  root.addEventListener('mousemove', e => { const t = e.target.closest && e.target.closest('[data-tip]'); if (!t) { if (tipEl) { tipEl.remove(); tipEl = null; } return; } if (!tipEl) { tipEl = el('<div class="tip"></div>'); document.body.appendChild(tipEl); } const [a, b] = t.dataset.tip.split(' · '); tipEl.innerHTML = `${esc(a)} <span>${esc(b || '')}</span>`; tipEl.style.left = Math.min(innerWidth - tipEl.offsetWidth - 8, e.clientX + 12) + 'px'; tipEl.style.top = e.clientY - 28 + 'px'; });
  root.addEventListener('mouseleave', () => { if (tipEl) { tipEl.remove(); tipEl = null; } });
  const reload = debounce(load, 1500);
  return { onProject() { title(); load(); }, onEvent(t) { if (t === 'issue') reload(); }, unmount() { if (tipEl) tipEl.remove(); removeEventListener('resize', onResize); } };
}

// ---------------------------------------------------------------- activity
function ActivityView(root, r, signal) {
  setCrumb('Activity');
  root.innerHTML = `<div class="vh"><h2>Activity</h2><span class="meta">Latest 100 events across all projects</span></div><div class="vbody"><div class="feed" id="feed">${'<span class="sk"></span>'.repeat(12)}</div></div>`;
  const line = a => {
    const u = userOf(a.actor); let what;
    if (a.op === 'create') what = 'created'; else if (a.op === 'comment') what = 'commented on'; else if (a.op === 'delete') what = 'deleted';
    else if (a.field === 'status') what = `moved ${stPill(a.from)} ${icon('arrow')} ${stPill(a.to)} on`;
    else if (a.field === 'assignee') what = a.to == null ? 'unassigned' : `assigned <b>${esc(userOf(a.to).name)}</b> to`;
    else if (a.field === 'priority') what = `set <b>${PL[a.to] || a.to}</b> priority on`;
    else if (a.field === 'labels') what = 'relabeled'; else if (a.field === 'estimate') what = `estimated <b>${a.to ?? '—'}</b> on`;
    else what = `edited the ${esc(a.field || 'issue')} of`;
    return `<div class="ev">${av(a.actor)}<span><b>${esc(u.name)}</b> ${what} <a class="key" href="#/issue/${esc(a.key)}">${esc(a.key)}</a></span><span class="tm" title="${abs(a.ts)}">${rel(a.ts)}</span></div>`;
  };
  async function load() {
    try {
      const d = await api('/api/activity?limit=100', { signal });
      let day = '', out = '';
      for (const a of d.items) { const dd = new Date(a.ts).toDateString(); if (dd !== day) { day = dd; out += `<div class="day">${dd === new Date(now()).toDateString() ? 'Today' : dd}</div>`; } out += line(a); }
      $('#feed', root).innerHTML = out || '<div class="empty"><b>Nothing yet</b></div>';
    } catch (e) { if (!aborted(e) && e.status !== 401) $('#feed', root).innerHTML = `<div class="empty"><b>Could not load activity</b></div>`; }
  }
  load();
  const reload = debounce(load, 600);
  return { onEvent(t) { if (t === 'issue' || t === 'comment') reload(); } };
}

// ---------------------------------------------------------------- settings
function SettingsView(root, r, signal) {
  if (r.parts[1] === 'infra') return InfraView(root, r, signal);
  setCrumb('Settings<span class="sub">/ Account</span>');
  root.innerHTML = `<div class="vh"><h2>Account</h2></div><div class="vbody settings">
    <div class="sec"><div class="me">${av(S.me.id, 'xl')}<div><div class="nm">${esc(S.me.name)}</div><div class="un">@${esc(S.me.username)}</div></div><button class="btn" id="logout">Sign out</button></div></div>
    <div class="sec"><h4>Keyboard shortcuts</h4><div class="keys">
      ${[['Command palette', '⌘ K  or  /'], ['New issue', 'c'], ['Move selection', 'j / k'], ['Open selected issue', '↵'], ['Close dialog', 'esc'], ['Send comment', '⌘ ↵']].map(([n, k]) => `<div><span>${n}</span><kbd>${k}</kbd></div>`).join('')}</div></div>
    <div class="sec"><h4>Workspace</h4><div class="keys"><div><span>Projects</span><span>${S.boot.projects.length}</span></div><div><span>People</span><span>${S.boot.users.length}</span></div><div><span>Issues</span><span>${fmt(S.boot.counts.total)}</span></div><div><span>Open</span><span>${fmt(S.boot.counts.open)}</span></div></div>
      <p style="margin-top:14px;font-size:12px;color:var(--fg3)">Hosting details are under <a href="#/settings/infra" style="color:var(--acc)">Infrastructure</a>.</p></div></div>`;
  $('#logout', root).onclick = async () => { try { await api('/api/auth/logout', { method: 'POST' }); } catch (e) { /* ignore */ } S.me = null; if (S.es) { S.es.close(); S.es = null; } renderLogin(); };
  return {};
}

// ---------------------------------------------------------------- infrastructure
function InfraView(root, r, signal) {
  setCrumb('Settings<span class="sub">/ Infrastructure</span>');
  const stat = (l, v, cls = '') => `<div><div class="l">${l}</div><div class="v ${cls}">${v}</div></div>`;
  root.innerHTML = `<div class="vh"><h2>Infrastructure</h2><span class="meta" id="imeta"></span></div><div class="vbody infra">
    <p class="lead">Tessera is served from a <b>two-node Kube32 cluster of ESP32-S3 microcontrollers</b>. Node 01 runs the ingress and the display pod; node 02 runs the issue engine as a WebAssembly pod inside 1.5 MB of PSRAM. The two boards talk over ESP-NOW. Everything on this page is live.</p>
    <div class="nodes" id="nodes"></div>
    <div class="sec"><h4>Ingress <span class="n">node 01 · port 80</span></h4><div class="stat" id="ingress"></div></div>
    <div class="sec"><h4>ESP-NOW link</h4><div class="stat" id="link"></div></div>
    <div class="sec"><h4>Issue engine <span class="n">tessera-core · node 02</span></h4><div class="stat" id="core"></div><div id="mem" style="margin-top:14px;max-width:420px"></div></div>
    <div class="sec"><h4>Load test</h4>
      <p style="font-size:12.5px;color:var(--fg2);margin-bottom:12px;max-width:600px">Runs N concurrent clients from this browser for 20 seconds: a loop of list and search requests with random queries against the cluster. Numbers are measured client-side.</p>
      <div class="lt"><div class="ctl"><label for="ltn">Clients</label><input class="inp" id="ltn" type="number" min="1" max="64" value="8"><button class="btn pri" id="ltrun">Run 20 s load test</button><button class="btn hide" id="ltstop">Stop</button><span id="ltlive" style="font-family:var(--mono);color:var(--fg3);font-size:12px"></span></div>
      <div class="prog"><i id="ltp"></i></div><div class="stat" id="ltres"></div><div class="hist" id="lthist"></div><div class="res" id="ltnote"></div></div></div>
  </div>`;
  let timer = null, stats = null, running = false, ltctl = null;
  function spark(arr) {
    if (!arr || arr.length < 2) return '<svg class="spark"></svg>';
    const min = Math.min(...arr), max = Math.max(...arr), rng = Math.max(1, max - min), W = 200, H = 28;
    const pts = arr.map((v, i) => `${(i / (arr.length - 1) * W).toFixed(1)},${(H - 2 - (v - min) / rng * (H - 6)).toFixed(1)}`);
    return `<svg class="spark" viewBox="0 0 ${W} ${H}" preserveAspectRatio="none"><path class="a" d="M0,${H} L${pts.join(' L')} L${W},${H}z"/><path d="M${pts.join(' L')}"/></svg>`;
  }
  function paintCluster(c) {
    if (!c) return;
    $('#nodes', root).innerHTML = c.nodes.map(n => `<div class="node"><div class="nh"><b>${esc(n.name)}</b><span class="role">${esc(n.role)}</span><i class="rd ${n.ready ? '' : 'off'}" title="${n.ready ? 'Ready' : 'Not ready'}"></i></div>
      <div class="stat">${stat('Free heap', bytes(n.free_heap), n.free_heap < 60000 ? 'warn' : '')}${stat('Min free heap', bytes(n.min_free_heap))}${stat('Free PSRAM', n.free_psram ? bytes(n.free_psram) : '<small>none</small>')}${stat('Uptime', dur(n.uptime_ms))}</div>
      ${spark(S.heap[n.name])}<div class="pods">${n.pods.map(p => `<span class="chip"><i style="background:var(--green)"></i>${esc(p)}</span>`).join('')}</div></div>`).join('');
    const g = c.ingress, l = c.link;
    $('#ingress', root).innerHTML = stat('Requests / s', g.rps) + stat('p50', us(g.p50_us)) + stat('p99', us(g.p99_us), g.p99_us > 100000 ? 'warn' : '') + stat('Active SSE', g.active_sse) + stat('Workers', g.workers) + stat('Total requests', fmt(g.requests));
    $('#link', root).innerHTML = stat('Frames rx', fmt(l.espnow_frames_rx)) + stat('Frames tx', fmt(l.espnow_frames_tx)) + stat('Reassembled', fmt(l.reassembled)) + stat('CRC errors', fmt(l.crc_errors), l.crc_errors ? 'warn' : 'ok');
    $('#imeta', root).textContent = `updated ${rel(c.now)}`;
  }
  function paintStats(s) {
    const m = s.memory, pct = m.used / m.limit * 100;
    $('#core', root).innerHTML = stat('Issues', fmt(s.issues)) + stat('Comments', fmt(s.comments)) + stat('Index terms', fmt(s.index.terms)) + stat('Postings', fmt(s.index.postings)) + stat('Sessions', s.sessions) + stat('Requests', fmt(s.requests.total)) + stat('Errors', fmt(s.requests.errors), s.requests.errors ? 'warn' : '') + stat('Avg', us(s.requests.avg_us)) + stat('p99', us(s.requests.p99_us)) + stat('Uptime', dur(s.uptime_ms));
    $('#mem', root).innerHTML = `<div style="display:flex;justify-content:space-between;font-size:11.5px;color:var(--fg3)"><span>Linear memory</span><span class="mono">${bytes(m.used)} / ${bytes(m.limit)} · ${pct.toFixed(0)}%</span></div><div class="meter"><i style="width:${pct}%;background:${pct > 90 ? 'var(--red)' : pct > 75 ? 'var(--acc)' : 'var(--teal)'}"></i></div>`;
  }
  async function poll() {
    try {
      const reqs = [api('/api/stats', { signal })];
      if (S.live !== 'on') reqs.push(api('/api/cluster', { signal }));
      const [s, c] = await Promise.all(reqs);
      paintStats(s); if (c) { onEvent('cluster', c); paintCluster(c); }
    } catch (e) { /* toasted by api() */ }
  }
  paintCluster(S.cluster); poll(); timer = setInterval(poll, 2000);
  // ---- load test
  const QS = ['wifi', 'heap', 'sse', 'board', 'ingress', 'timeout', 'psram', 'key', 'lcd', 'search', 'frame', 'wasm', 'cookie', 'p99', 'comm', 'esp', 'index', 'drag', 'label', 'watchdog'];
  async function runLoad() {
    const n = Math.max(1, Math.min(64, +$('#ltn', root).value || 8)), secs = 20;
    running = true; ltctl = new AbortController(); const sig = ltctl.signal;
    $('#ltrun', root).classList.add('hide'); $('#ltstop', root).classList.remove('hide'); $('#ltnote', root).textContent = ''; $('#ltres', root).innerHTML = ''; $('#lthist', root).innerHTML = '';
    const lat = [], perSec = new Array(secs).fill(0); let errors = 0, inflight = 0;
    const t0 = performance.now(), end = t0 + secs * 1000;
    const worker = async () => {
      while (performance.now() < end && !sig.aborted) {
        const url = Math.random() < 0.5 ? `/api/issues?page=${1 + Math.floor(Math.random() * 12)}&limit=50${Math.random() < 0.3 ? '&status=todo,in_progress' : ''}` : `/api/search?q=${QS[Math.floor(Math.random() * QS.length)]}&limit=20`;
        const s = performance.now(); inflight++;
        try { const res = await fetch(url, { signal: sig, credentials: 'same-origin' }); await res.arrayBuffer(); if (!res.ok) errors++; }
        catch (e) { if (sig.aborted) { inflight--; return; } errors++; }
        inflight--;
        lat.push(performance.now() - s); perSec[Math.min(secs - 1, Math.floor((s - t0) / 1000))]++;
      }
    };
    const tick = setInterval(() => {
      const t = (performance.now() - t0) / 1000; $('#ltp', root).style.width = Math.min(100, t / secs * 100) + '%';
      const sec = Math.floor(t); $('#ltlive', root).textContent = `${sec}s · ${lat.length} req · ${sec > 0 ? perSec[sec - 1] : 0} rps · ${inflight} in flight`;
    }, 250);
    await Promise.all(Array.from({ length: n }, worker));
    clearInterval(tick); running = false;
    $('#ltrun', root).classList.remove('hide'); $('#ltstop', root).classList.add('hide'); $('#ltp', root).style.width = '0'; $('#ltlive', root).textContent = '';
    if (!lat.length) { $('#ltnote', root).textContent = 'No requests completed.'; return; }
    const el2 = (performance.now() - t0) / 1000; lat.sort((a, b) => a - b);
    const p = q => lat[Math.min(lat.length - 1, Math.floor(lat.length * q))];
    $('#ltres', root).innerHTML = stat('Clients', n) + stat('Requests', fmt(lat.length)) + stat('Achieved rps', (lat.length / el2).toFixed(1), 'ok') + stat('p50', ms(p(0.5))) + stat('p99', ms(p(0.99))) + stat('Max', ms(lat[lat.length - 1])) + stat('Errors', errors, errors ? 'bad' : '');
    const mx = Math.max(1, ...perSec);
    $('#lthist', root).innerHTML = perSec.slice(0, Math.ceil(el2)).map((v, i) => `<i class="${v === mx ? 'on' : ''}" style="height:${v / mx * 100}%" title="second ${i + 1}: ${v} rps"></i>`).join('');
    $('#ltnote', root).textContent = `${sig.aborted ? 'Stopped early. ' : ''}Measured over ${el2.toFixed(1)} s from this browser, including network time. The browser caps concurrent HTTP/1.1 connections per host, so the effective concurrency may be lower than ${n}.`;
  }
  $('#ltrun', root).onclick = runLoad;
  $('#ltstop', root).onclick = () => ltctl && ltctl.abort();
  return {
    onEvent(t, d) { if (t === 'cluster') paintCluster(d); },
    unmount() { clearInterval(timer); if (ltctl) ltctl.abort(); },
  };
}

// ---------------------------------------------------------------- overlays
let OVL = null;
function closeOverlay() { if (OVL) { OVL.el.remove(); if (OVL.onclose) OVL.onclose(); OVL = null; } }
function openOverlay(html, cls = '') {
  closeOverlay(); closePop();
  const o = el(`<div class="ovl"><div class="dlg ${cls}" role="dialog">${html}</div></div>`);
  o.addEventListener('mousedown', e => { if (e.target === o) closeOverlay(); });
  document.body.appendChild(o); OVL = { el: o }; return o;
}
function openNewIssue(defaults = {}) {
  const d = { project: S.project || S.boot.projects[0].id, status: 'backlog', priority: 0, assignee: null, labels: [], ...defaults };
  const o = openOverlay(`<div class="dh">${icon('plus')}<span>New issue</span><span class="sp"></span><button class="btn ghost sm" id="nx">${icon('x')}</button></div>
    <div class="db"><textarea class="ttl" id="nt" rows="1" maxlength="120" placeholder="Issue title"></textarea><textarea class="desc" id="nd" maxlength="1000" placeholder="Add a description…"></textarea>
      <div class="ctrls">
        <select class="sel" id="np">${S.boot.projects.map(p => `<option value="${p.id}" ${p.id === d.project ? 'selected' : ''}>${esc(p.name)}</option>`).join('')}</select>
        <select class="sel" id="ns">${S.boot.statuses.map(s => `<option value="${s}" ${s === d.status ? 'selected' : ''}>${SL[s]}</option>`).join('')}</select>
        <select class="sel" id="npr">${PL.map((n, i) => `<option value="${i}" ${i === d.priority ? 'selected' : ''}>${n}</option>`).join('')}</select>
        <select class="sel" id="na"><option value="">Unassigned</option>${S.boot.users.map(u => `<option value="${u.id}" ${u.id === d.assignee ? 'selected' : ''}>${esc(u.name)}</option>`).join('')}</select>
        <input class="inp" id="ne" type="number" min="0" max="999" placeholder="Estimate">
      </div>
      <div class="lbls" id="nl">${S.boot.labels.map(l => `<button type="button" class="chip tog ${d.labels.includes(l.id) ? 'on' : ''}" data-id="${l.id}" style="--c:${l.color}"><i></i>${esc(l.name)}</button>`).join('')}</div>
    </div>
    <div class="df"><span><kbd>⌘</kbd>+<kbd>↵</kbd> to create</span><span class="sp"></span><span id="nerr" style="color:var(--red)"></span><button class="btn" id="ncancel">Cancel</button><button class="btn pri" id="ncreate">Create issue</button></div>`);
  const nt = $('#nt', o), nd = $('#nd', o);
  const grow = ta => { ta.style.height = 'auto'; ta.style.height = ta.scrollHeight + 2 + 'px'; };
  [nt, nd].forEach(t => t.addEventListener('input', () => grow(t)));
  nt.addEventListener('keydown', e => { if (e.key === 'Enter' && !(e.metaKey || e.ctrlKey)) { e.preventDefault(); nd.focus(); } });
  $('#nl', o).onclick = e => { const b = e.target.closest('.tog'); if (b) b.classList.toggle('on'); };
  $('#nx', o).onclick = closeOverlay; $('#ncancel', o).onclick = closeOverlay;
  o.addEventListener('keydown', e => { if (e.key === 'Enter' && (e.metaKey || e.ctrlKey)) { e.preventDefault(); create(); } });
  async function create() {
    const title = nt.value.trim(); const err = $('#nerr', o);
    if (!title) { err.textContent = 'Title is required.'; nt.focus(); return; }
    const body = { title, description: nd.value, project: +$('#np', o).value, status: $('#ns', o).value, priority: +$('#npr', o).value, assignee: $('#na', o).value ? +$('#na', o).value : null, labels: $$('.tog.on', o).map(b => +b.dataset.id), estimate: $('#ne', o).value === '' ? null : +$('#ne', o).value };
    const btn = $('#ncreate', o); btn.disabled = true; err.textContent = '';
    try { const i = await api('/api/issues', { method: 'POST', body }); closeOverlay(); toast(`Created ${i.key}`, { kind: 'ok' }); go('#/issue/' + i.key); }
    catch (e) { err.textContent = e.message || 'Could not create issue'; btn.disabled = false; }
  }
  $('#ncreate', o).onclick = create;
  setTimeout(() => nt.focus(), 30);
}
function openPalette() {
  const cmds = [
    { n: 'Go to board', i: 'board', f: () => go('#/board') }, { n: 'Go to issues', i: 'list', f: () => go('#/issues') },
    { n: 'Go to analytics', i: 'chart', f: () => go('#/analytics') }, { n: 'Go to activity', i: 'pulse', f: () => go('#/activity') },
    { n: 'New issue', i: 'plus', f: () => openNewIssue() }, { n: 'Infrastructure', i: 'cpu', f: () => go('#/settings/infra') },
    { n: 'Account settings', i: 'user', f: () => go('#/settings') },
    { n: 'My open issues', i: 'list', f: () => go(`#/issues?assignee=${S.me.id}&status=todo,in_progress,review`) },
  ];
  const o = openOverlay(`<div class="pin">${icon('search')}<input id="pq" placeholder="Search issues or type a command…" autocomplete="off" spellcheck="false"><kbd>esc</kbd></div>
    <div class="list" id="pl"></div><div class="foot"><span><kbd>↑</kbd><kbd>↓</kbd> navigate</span><span><kbd>↵</kbd> open</span><span id="pmeta" style="margin-left:auto"></span></div>`, 'pal');
  const inp = $('#pq', o), list = $('#pl', o), meta = $('#pmeta', o);
  let rows = [], sel = 0, ctl = null, seq = 0;
  const paint = () => {
    let html = '', last = '';
    rows.forEach((r, i) => { if (r.hd !== last) { html += `<div class="hd">${r.hd}</div>`; last = r.hd; }
      html += r.cmd ? `<div class="it ${i === sel ? 'sel' : ''}" data-i="${i}">${icon(r.cmd.i)}<span class="t">${esc(r.cmd.n)}</span></div>`
        : `<div class="it ${i === sel ? 'sel' : ''}" data-i="${i}">${stIcon(r.it.status)}<span class="key">${esc(r.it.key)}</span><span class="t">${esc(r.it.title)}</span>${av(r.it.assignee)}</div>`; });
    list.innerHTML = html || `<div class="empty"><b>No results</b>Try a different word; the last word matches as a prefix.</div>`;
    const s = $('.it.sel', list); if (s) s.scrollIntoView({ block: 'nearest' });
  };
  async function search() {
    const q = inp.value.trim(), ql = q.toLowerCase();
    const cm = cmds.filter(c => !ql || c.n.toLowerCase().split(' ').some(w => w.startsWith(ql)) || c.n.toLowerCase().startsWith(ql)).map(cmd => ({ hd: 'Commands', cmd }));
    const m = /^([a-z]{2,5})-?(\d+)$/i.exec(q);
    if (ctl) ctl.abort(); sel = 0;
    if (!q) { rows = cm; meta.textContent = ''; paint(); return; }
    ctl = new AbortController(); const my = ++seq;
    try {
      const d = await api(`/api/search?q=${encodeURIComponent(q)}&limit=12`, { signal: ctl.signal });
      if (my !== seq) return;
      let items = d.items.map(it => ({ hd: 'Issues', it }));
      if (m && !items.some(x => x.it.key.toLowerCase() === `${m[1]}-${m[2]}`.toLowerCase())) items.unshift({ hd: 'Issues', it: { key: `${m[1].toUpperCase()}-${m[2]}`, title: `Open ${m[1].toUpperCase()}-${m[2]}`, status: 'todo', assignee: null } });
      rows = [...items, ...cm]; meta.textContent = `${d.total} match${d.total === 1 ? '' : 'es'} · ${us(d.took_us)}`; paint();
    } catch (e) { if (!aborted(e)) { rows = cm; paint(); } }
  }
  const run = i => { const r = rows[i]; if (!r) return; closeOverlay(); if (r.cmd) r.cmd.f(); else go('#/issue/' + r.it.key); };
  inp.oninput = debounce(search, 150);
  inp.onkeydown = e => {
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') { e.preventDefault(); if (rows.length) { sel = (sel + (e.key === 'ArrowDown' ? 1 : rows.length - 1)) % rows.length; paint(); } }
    else if (e.key === 'Enter') { e.preventDefault(); run(sel); }
  };
  list.onmousemove = e => { const it = e.target.closest('.it'); if (it && +it.dataset.i !== sel) { sel = +it.dataset.i; $$('.it', list).forEach(x => x.classList.toggle('sel', +x.dataset.i === sel)); } };
  list.onclick = e => { const it = e.target.closest('.it'); if (it) run(+it.dataset.i); };
  search(); setTimeout(() => inp.focus(), 30);
}

// ---------------------------------------------------------------- global keys & boot
document.addEventListener('keydown', e => {
  if (!S.me) return;
  const t = e.target, typing = t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.tagName === 'SELECT' || t.isContentEditable);
  if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === 'k') { e.preventDefault(); if (OVL && $('#pq', OVL.el)) closeOverlay(); else openPalette(); return; }
  if (e.key === 'Escape') { if (OVL) { closeOverlay(); return; } if (POP) { closePop(); return; } if (typing) { t.blur(); return; } }
  if (typing || OVL || e.metaKey || e.ctrlKey || e.altKey) return;
  if (e.key === '/') { e.preventDefault(); openPalette(); return; }
  if (e.key === 'c') { e.preventDefault(); openNewIssue(); return; }
  if (S.view && S.view.onKey) S.view.onKey(e);
});
window.addEventListener('hashchange', route);
(async function start() {
  try {
    const me = await api('/api/me', { quiet: true }); S.me = me.user;
  } catch (e) { S.me = null; }
  try { await loadBoot(); } catch (e) { $('#app').innerHTML = `<div class="login"><div class="box"><div class="wm">${MARK}<div><h1>Tessera</h1><p>The server is not reachable.</p></div></div><button class="btn" onclick="location.reload()">Retry</button></div></div>`; return; }
  if (S.me) enterApp(); else renderLogin();
})();
