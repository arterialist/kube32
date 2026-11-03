"""Runs the board models on this computer, with the same integer and float
math the pods use, and serves a page to try them at http://127.0.0.1:8765.

    python models/playground.py

Each model's Python reference matches its pod bit for bit, so a result here is
the result a board gives for the same input, seed and steps. Only the speed
differs.
"""

import argparse
import io
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
for p in ("common", "doodle", "kws", "lm"):
    sys.path.insert(0, os.path.join(HERE, p))
sys.path.insert(0, os.path.join(REPO, "apps", "llm-pipeline", "tools"))

import doodle  # noqa: E402
import frontend  # noqa: E402
import kws  # noqa: E402
import llm  # noqa: E402
import mdlm  # noqa: E402
import spec  # noqa: E402

CACHE = os.path.expanduser("~/.cache/kube32-models")
DEFAULTS = {
    "doodle": os.path.join(HERE, "doodle", "doodle-l.bin"),
    "kws": os.path.join(HERE, "kws", "kws.bin"),
    "mdlm": os.path.join(HERE, "mdlm", "mdlm.bin"),
    "target": os.path.join(CACHE, "ckpt", "target-lm", "target-lm.bin"),
    "draft": os.path.join(CACHE, "data", "tinyllamas", "stories260K.bin"),
    "tokenizer": os.path.join(CACHE, "data", "tinyllamas", "tok512.bin"),
}
CONTEXT = 256


class Models:
    def __init__(self, paths, target_bits):
        t0 = time.time()
        self.tok = llm.Tokenizer(paths["tokenizer"])
        self.doodle = doodle.Model(open(paths["doodle"], "rb").read())
        self.kws = kws.Model(open(paths["kws"], "rb").read())
        self.mdlm = mdlm.Net(open(paths["mdlm"], "rb").read())
        self.target_sl, self.target_bytes = spec.slices(paths["target"], target_bits)
        self.draft_sl, self.draft_bytes = spec.slices(paths["draft"], 8)
        self.target_bits = target_bits
        print("models loaded in %.1f s" % (time.time() - t0))


# ---- inference with progress -------------------------------------------------

def doodle_frames(m, word, seed, steps, guidance):
    """Yields (step, image u8 28x28) after each denoising step: doodle.Model.sample with a hook"""
    cls = doodle.CLASSES.index(word)
    rng = doodle.Rng(seed)
    F32 = doodle.F32
    x = np.array([rng.gauss() for _ in range(784)], F32).reshape(28, 28)
    yield 0, doodle.to_u8(x)
    t = m.sched[steps]
    g = F32(guidance)
    for i in range(steps):
        v = m.forward(x, m.cond(steps, i, cls))
        if guidance != 1.0:
            vu = m.forward(x, m.cond(steps, i, doodle.NONE))
            v = (vu + (g * (v - vu)).astype(F32)).astype(F32)
        a, s, an, sn = t["alpha"][i], t["sigma"][i], t["alpha"][i + 1], t["sigma"][i + 1]
        x0 = np.clip((a * x - s * v).astype(F32), F32(-1.0), F32(1.0))
        eps = (s * x + a * v).astype(F32)
        x = (an * x0 + sn * eps).astype(F32)
        yield i + 1, doodle.to_u8(x)


def mdlm_steps(net, tok, prompt, steps, seed, temperature):
    """Yields (step, tokens) after each refinement step, via mdlm.sample's trace"""
    pt = tok.encode(prompt) if prompt else [llm.BOS]
    length = net.cfg["seq_len"]
    x = list(pt) + [mdlm.MASK] * (length - len(pt))
    yield 0, list(x)
    # mdlm.sample runs every step at once; replay it one step at a time by
    # recording which positions each step reveals
    trace = []
    final = mdlm.sample(net, pt, steps, seed, temperature, trace=trace)
    for rec in trace:
        for p, t in zip(rec["revealed"], rec["tokens"]):
            x[p] = t
        yield rec["step"] + 1, list(x)
    assert x == final


def render_window(tok, toks):
    out = []
    for i, t in enumerate(toks):
        if i == 0 and t == llm.BOS:
            continue
        if t == mdlm.MASK:
            out.append("░")
        elif t == llm.BOS:
            out.append(" ⏎ ")
        else:
            out.append(tok.piece(toks[i - 1] if i else llm.BOS, t).decode("utf-8", "replace"))
    return "".join(out)


def story_passes(models, prompt, steps, mode, k):
    """Yields dicts per target pass: text so far, the pass's drafts and how many were accepted.
    mode: target | draft | spec | ngram"""
    tok = models.tok
    toks = tok.encode(prompt)
    n_prompt = len(toks)
    if mode == "draft":
        target = spec.Model(models.draft_sl, CONTEXT)
    else:
        target = spec.Model(models.target_sl, CONTEXT)
    draft = spec.Model(models.draft_sl, CONTEXT) if mode == "spec" else None
    kk = k if mode in ("spec", "ngram") else 0
    while len(toks) - n_prompt < steps and len(toks) < CONTEXT:
        room = min(kk, steps - (len(toks) - n_prompt) - 1, CONTEXT - len(toks) - 1)
        d = []
        if room > 0 and mode == "ngram":
            d = spec.ngram_draft(toks, room)
        elif room > 0 and draft is not None:
            seq = list(toks)
            draft.valid = min(draft.valid, len(seq) - 1)
            d += draft.run(seq, len(seq), len(seq) - 1)
            for _ in range(room - 1):
                seq.append(d[-1])
                d += draft.run(seq, len(seq), len(seq) - 1)
        seq = toks + d
        target.valid = min(target.valid, len(toks) - 1)
        outs = target.run(seq, len(seq), len(toks) - 1)
        n = 0
        while n < len(d) and d[n] == outs[n]:
            n += 1
        new = d[:n] + [outs[n]]
        target.valid = min(target.valid, len(toks) + n)
        if draft is not None:
            draft.valid = min(draft.valid, len(toks) + n)
        stop = llm.BOS in new
        if stop:
            new = new[:new.index(llm.BOS)]
        before = len(toks)
        toks += new
        # the drafts the target turned down, as text after the accepted ones
        rejected = ""
        if n < len(d):
            ctx = toks[:before + n]
            rejected = tok.decode(ctx + d[n:])[len(tok.decode(ctx)):]
        yield {"text": tok.decode(toks), "drafted": len(d), "accepted": n, "added": len(new), "rejected": rejected}
        if stop:
            break


def spot(models, audio):
    feats = frontend.features(audio)
    logits = models.kws.forward(feats)
    order = np.argsort(-logits)
    return {"word": kws.LABELS[int(order[0])],
            "scores": [{"label": kws.LABELS[int(i)], "logit": round(float(logits[i]), 3)} for i in order]}


def to_clip(pcm, normalize):
    """1 s of 16 kHz int16, centered on the loudest 1 s and optionally scaled to a fixed peak"""
    a = np.asarray(pcm, np.int16)
    n = frontend.SAMPLES
    if len(a) > n:
        e = np.convolve(a.astype(np.float64) ** 2, np.ones(n), "valid")
        s = int(np.argmax(e))
        a = a[s:s + n]
    out = np.zeros(n, np.int16)
    out[:len(a)] = a
    peak = int(np.max(np.abs(out.astype(np.int32)))) if len(out) else 0
    if normalize and peak > 0:
        out = np.clip(np.rint(out.astype(np.float64) * (16000.0 / peak)), -32768, 32767).astype(np.int16)
    return out, peak


def say_clip(word, voice):
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "w.wav")
        subprocess.run(["say", "-v", voice, "-o", path, "--file-format=WAVE", "--data-format=LEI16@16000", word],
                       check=True, timeout=20)
        with wave.open(path) as w:
            return np.frombuffer(w.readframes(w.getnframes()), "<i2")


# ---- HTTP ---------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    models = None

    def log_message(self, fmt, *args):
        pass

    def send_json(self, obj, code=200):
        b = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def sse(self, gen):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        t0 = time.time()
        try:
            for ev in gen:
                ev["ms"] = int((time.time() - t0) * 1000)
                self.wfile.write(b"data: " + json.dumps(ev).encode() + b"\n\n")
                self.wfile.flush()
            self.wfile.write(b"event: done\ndata: {\"ms\": %d}\n\n" % int((time.time() - t0) * 1000))
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        m = self.models
        if u.path == "/":
            b = PAGE.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)
        elif u.path == "/api/info":
            self.send_json({"doodle_weights": m.doodle.weights, "kws_weights": m.kws.weights,
                            "mdlm_bytes": m.mdlm.bytes, "target_bytes": m.target_bytes,
                            "target_bits": m.target_bits, "draft_bytes": m.draft_bytes,
                            "words": list(doodle.CLASSES)})
        elif u.path == "/api/doodle":
            word = q.get("word", "cat")
            if word not in doodle.CLASSES:
                return self.send_json({"error": "word must be one of " + ", ".join(doodle.CLASSES)}, 400)
            gen = doodle_frames(m.doodle, word, int(q.get("seed", 1)), int(q.get("steps", 16)),
                                float(q.get("guidance", 2.0)))
            self.sse({"step": s, "img": img.flatten().tolist()} for s, img in gen)
        elif u.path == "/api/mdlm":
            gen = mdlm_steps(m.mdlm, m.tok, q.get("prompt", ""), int(q.get("steps", 32)), int(q.get("seed", 1)),
                             float(q.get("temperature", 0.0)))
            self.sse({"step": s, "text": render_window(m.tok, x),
                      "masked": sum(1 for t in x if t == mdlm.MASK)} for s, x in gen)
        elif u.path == "/api/story":
            self.sse(story_passes(m, q.get("prompt", "Once upon a time"), int(q.get("steps", 60)),
                                  q.get("mode", "target"), int(q.get("k", 4))))
        elif u.path == "/api/say":
            word = q.get("word", "cat")
            if not word.replace(" ", "").isalpha() or len(word) > 20:
                return self.send_json({"error": "letters only"}, 400)
            clip, peak = to_clip(say_clip(word, q.get("voice", "Samantha")), q.get("normalize", "1") == "1")
            r = spot(m, clip)
            r["peak"] = peak
            self.send_json(r)
        else:
            self.send_json({"error": "not found"}, 404)

    def do_POST(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path != "/api/kws":
            return self.send_json({"error": "not found"}, 404)
        n = int(self.headers.get("Content-Length", 0))
        if n > 16000 * 2 * 10:
            return self.send_json({"error": "at most 10 s of audio"}, 413)
        pcm = np.frombuffer(self.rfile.read(n), "<i2")
        clip, peak = to_clip(pcm, q.get("normalize", "1") == "1")
        r = spot(self.models, clip)
        r["peak"] = peak
        self.send_json(r)


PAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Board models</title>
<style>
:root{--bg:#f5f4f0;--paper:#fffdf9;--ink:#1d1c1a;--mute:#6d6960;--line:#e0dbd0;--acc:#c2410c;--ok:#15803d;--bad:#b91c1c;--code:#eeeae1}
@media (prefers-color-scheme:dark){:root{--bg:#141311;--paper:#1c1b18;--ink:#ebe7de;--mute:#a19c91;--line:#34312b;--acc:#fb923c;--ok:#4ade80;--bad:#f87171;--code:#26241f}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.55 -apple-system,BlinkMacSystemFont,"Inter",sans-serif}
main{max-width:1100px;margin:0 auto;padding:24px 18px 60px}
h1{font:600 26px/1.2 Georgia,serif;margin:0 0 4px}.sub{color:var(--mute);margin:0 0 20px}
.grid{display:grid;gap:14px;grid-template-columns:repeat(auto-fit,minmax(330px,1fr))}
.card{background:var(--paper);border:1px solid var(--line);border-radius:10px;padding:16px}
.card h2{font-size:16px;margin:0 0 2px}.card .note{color:var(--mute);font-size:13px;margin:0 0 12px}
label{font-size:13px;color:var(--mute)}input,select{font:14px inherit;padding:6px 8px;border:1px solid var(--line);border-radius:6px;background:var(--bg);color:var(--ink)}
input[type=text]{width:100%}.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin:6px 0}
button{font:600 14px inherit;padding:7px 12px;border-radius:6px;border:1px solid var(--ink);background:var(--ink);color:var(--bg);cursor:pointer}
button.alt{background:transparent;color:var(--ink)}button:disabled{opacity:.5;cursor:default}
.out{font:14px/1.6 ui-monospace,Menlo,monospace;background:var(--code);border-radius:8px;padding:12px;min-height:90px;white-space:pre-wrap;word-break:break-word;margin-top:10px}
.stat{font:12px ui-monospace,Menlo,monospace;color:var(--mute);margin-top:6px}
canvas{image-rendering:pixelated;width:224px;height:224px;border-radius:8px;border:1px solid var(--line);background:#fff}
.acc{background:color-mix(in srgb,var(--ok) 25%,transparent)}.rej{color:var(--bad);text-decoration:line-through}
.word{font:600 28px Georgia,serif}.bars div{display:grid;grid-template-columns:80px 1fr 50px;gap:8px;font-size:12px;align-items:center}
.bar{height:6px;background:var(--acc);border-radius:3px}
</style></head><body><main>
<h1>Board models on this Mac</h1>
<p class="sub">The same integer and float math as the pods on the ESP32-S3 boards, bit for bit. Same input, seed and steps give the same output as a board; only the time differs.</p>
<div class="grid">

<section class="card" id="voice"><h2>Say a word</h2><p class="note">Keyword spotter (cat, dog, bird, house, tree, bed). Then the drawing and the story start at the same time.</p>
<div class="row"><button id="rec">Record 1.5 s</button><span id="recst" class="stat"></span></div>
<div class="row"><select id="sayw"><option>cat</option><option>dog</option><option>bird</option><option>house</option><option>tree</option><option>bed</option></select>
<button class="alt" id="say">Mac voice says it</button><label><input type="checkbox" id="norm" checked> normalize level</label></div>
<div class="row"><span class="word" id="heard">&nbsp;</span></div><div class="bars" id="bars"></div></section>

<section class="card"><h2>Doodle diffusion</h2><p class="note" id="dnote">doodle-l, the devkit's drawing model.</p>
<div class="row"><select id="dw"></select><label>steps <select id="ds"><option>8</option><option selected>16</option></select></label>
<label>seed <input id="dseed" type="number" value="1" style="width:70px"></label><button id="dgo">Draw</button></div>
<canvas id="cv" width="28" height="28"></canvas><div class="stat" id="dst"></div></section>

<section class="card"><h2>Story</h2><p class="note" id="snote"></p>
<input type="text" id="sp" value="Once upon a time, there was a cat">
<div class="row"><select id="sm"><option value="spec">speculative: target + stories260K draft</option><option value="target">target alone</option><option value="draft">stories260K alone</option><option value="ngram">speculative: prompt lookup draft</option></select>
<label>k <select id="sk"><option>2</option><option selected>4</option><option>6</option></select></label>
<label>tokens <input id="sn" type="number" value="80" style="width:70px"></label><label><input type="checkbox" id="showrej"> show rejected drafts</label><button id="sgo">Write</button></div>
<div class="out" id="so"></div><div class="stat" id="sst"></div></section>

<section class="card"><h2>Diffusion text</h2><p class="note">The masked diffusion model fills a 64-token window. Grey blocks are masks.</p>
<input type="text" id="mp" value="" placeholder="optional prefix, e.g. Tom had a red ball">
<div class="row"><label>steps <select id="ms"><option>8</option><option>16</option><option selected>32</option></select></label>
<label>temperature <input id="mt" type="number" value="0" step="0.1" style="width:70px"></label><label>seed <input id="mseed" type="number" value="1" style="width:70px"></label><button id="mgo">Generate</button></div>
<div class="out" id="mo"></div><div class="stat" id="mst"></div></section>
</div>
<script>
const $=id=>document.getElementById(id);
let info={};
fetch('/api/info').then(r=>r.json()).then(i=>{info=i;
 $('dw').innerHTML=i.words.map(w=>`<option>${w}</option>`).join('');
 $('dnote').textContent=`doodle-l, the devkit's drawing model: ${i.doodle_weights.toLocaleString()} int8 weights.`;
 $('snote').textContent=`Target: 3.57M parameters, int${i.target_bits} layers, ${(i.target_bytes/1e6).toFixed(2)} MB (streamed from flash on the devkit). Draft: stories260K, ${(i.draft_bytes/1e3).toFixed(0)} KB.`;});
function stream(url,on,done){const es=new EventSource(url);es.onmessage=e=>on(JSON.parse(e.data));
 es.addEventListener('done',e=>{es.close();done&&done(JSON.parse(e.data))});es.onerror=()=>es.close();return es;}
let des,ses,mes;
function draw(word){if(des)des.close();const ctx=$('cv').getContext('2d'),im=ctx.createImageData(28,28);
 const steps=$('ds').value;$('dst').textContent='drawing '+word+'...';
 des=stream(`/api/doodle?word=${word}&steps=${steps}&seed=${$('dseed').value}`,e=>{
  for(let i=0;i<784;i++){const v=e.img[i];im.data[i*4]=im.data[i*4+1]=im.data[i*4+2]=v;im.data[i*4+3]=255}
  ctx.putImageData(im,0,0);$('dst').textContent=`${word}: step ${e.step}/${steps}, ${e.ms} ms`;});}
$('dgo').onclick=()=>draw($('dw').value);
function story(prompt){if(ses)ses.close();const mode=$('sm').value,k=$('sk').value;let passes=0,acc=0,dr=0,last='';$('so').innerHTML='';
 ses=stream(`/api/story?prompt=${encodeURIComponent(prompt)}&mode=${mode}&k=${k}&steps=${$('sn').value}`,e=>{
  passes++;acc+=e.accepted;dr+=e.drafted;
  const add=e.text.slice(last.length);last=e.text;
  const span=document.createElement('span');span.textContent=add;if(e.accepted>0)span.className='acc';$('so').appendChild(span);
  if(e.rejected&&$('showrej').checked){const r=document.createElement('span');r.className='rej';r.textContent=e.rejected;r.title='drafts the target rejected';$('so').appendChild(r);}
  $('sst').textContent=`${passes} target passes, ${dr?`acceptance ${(acc/dr).toFixed(2)}, `:''}${e.ms} ms on this Mac`;
 },d=>{$('sst').textContent+=' (done)';});}
$('sgo').onclick=()=>story($('sp').value);
$('mgo').onclick=()=>{if(mes)mes.close();const st=$('ms').value;
 mes=stream(`/api/mdlm?prompt=${encodeURIComponent($('mp').value)}&steps=${st}&temperature=${$('mt').value}&seed=${$('mseed').value}`,e=>{
  $('mo').textContent=e.text;$('mst').textContent=`step ${e.step}/${st}, ${e.masked} masks left, ${e.ms} ms`;});};
function heard(r){$('heard').textContent=r.word;const mx=Math.max(...r.scores.map(s=>s.logit)),mn=Math.min(...r.scores.map(s=>s.logit));
 $('bars').innerHTML=r.scores.map(s=>`<div><span>${s.label}</span><span class="bar" style="width:${((s.logit-mn)/(mx-mn+1e-6)*100).toFixed(0)}%"></span><span>${s.logit.toFixed(1)}</span></div>`).join('')+`<div class="stat">input peak ${r.peak}</div>`;
 if(info.words&&info.words.includes(r.word)){$('dw').value=r.word;draw(r.word);const p=`Once upon a time, there was a ${r.word}`;$('sp').value=p;story(p);}}
$('say').onclick=()=>fetch(`/api/say?word=${$('sayw').value}&normalize=${$('norm').checked?1:0}`).then(r=>r.json()).then(heard);
$('rec').onclick=async()=>{const b=$('rec');b.disabled=true;
 try{const stream=await navigator.mediaDevices.getUserMedia({audio:{echoCancellation:false,noiseSuppression:false,autoGainControl:false}});
  const ac=new AudioContext({sampleRate:16000}),src=ac.createMediaStreamSource(stream),sp=ac.createScriptProcessor(4096,1,1),buf=[];
  sp.onaudioprocess=e=>buf.push(new Float32Array(e.inputBuffer.getChannelData(0)));src.connect(sp);sp.connect(ac.destination);
  $('recst').textContent='speak now';await new Promise(r=>setTimeout(r,1500));
  sp.disconnect();src.disconnect();stream.getTracks().forEach(t=>t.stop());await ac.close();
  const n=buf.reduce((a,x)=>a+x.length,0),pcm=new Int16Array(n);let o=0;for(const x of buf)for(const v of x)pcm[o++]=Math.max(-32768,Math.min(32767,Math.round(v*32767)));
  $('recst').textContent='';const r=await fetch(`/api/kws?normalize=${$('norm').checked?1:0}`,{method:'POST',body:pcm.buffer}).then(r=>r.json());heard(r);
 }catch(err){$('recst').textContent='microphone: '+err.message}finally{b.disabled=false}};
</script></main></body></html>"""


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--port", type=int, default=8765)
    p.add_argument("--target-bits", type=int, default=4, help="the target's layer bits, 4 as on the devkit or 8")
    for k, v in DEFAULTS.items():
        p.add_argument("--" + k, default=v)
    a = p.parse_args()
    Handler.models = Models({k: getattr(a, k) for k in DEFAULTS}, a.target_bits)
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    srv.daemon_threads = True
    print("http://127.0.0.1:%d" % a.port)
    srv.serve_forever()


if __name__ == "__main__":
    main()
