"""Far-field training data and robustness tests for the keyword spotter.

A person talking 0.3-2 m from a small MEMS microphone gives a quiet,
reverberant, band-limited signal on top of room noise and other people's
speech. The node's listen pod scales the detected word to a peak of 16,000
before the network sees it. This module simulates that chain on Speech
Commands clips:

  speech -> room reverb -> speaker/microphone response (high-pass, low-pass,
  random EQ, a resonance) -> level -> background noise at a given SNR
  (recorded noise, white, pink, brown, or babble: other talkers saying other
  words) -> int16 -> gain to a 16,000 peak

and builds the training copies (data.py's augmentation for part of the clips,
this chain for the rest, babble-only clips as _unknown_, a few synthesized
voices), the tests below, and the numbers they give for a model.

  farfield.py build [--copies 4]        features of the training copies and the far-field validation
  farfield.py score --model kws.bin [--json FILE]
      Speech Commands test split; the same clips through the chain at 20, 10, 5 dB;
      babble and noise false triggers; held-out synthesized voices
"""
import argparse
import json
import os
import sys
from multiprocessing import Pool

import numpy as np

import data
import frontend
import tts

N = frontend.SAMPLES
NF = 32768
SR = frontend.RATE
FREQS = np.fft.rfftfreq(NF, 1.0 / SR)
FREQS[0] = 1.0
LOGF = np.log(FREQS)
CACHE = os.path.expanduser("~/.cache/kube32-models/data/kws_features")
PEAK = 16000

BEDS = ("sc", "pink", "white", "brown", "babble", "none")
P_BED = (0.25, 0.20, 0.12, 0.08, 0.25, 0.10)


# ---- channel ----------------------------------------------------------------

def eq_response(rng):
    """magnitude response of a small speaker and a MEMS microphone with a random EQ"""
    fc_h = np.exp(rng.uniform(np.log(120), np.log(450)))
    fc_l = rng.uniform(3200, 7200)
    h = 1.0 / np.sqrt(1.0 + (fc_h / FREQS) ** (2 * rng.choice([2, 4])))
    h = h / np.sqrt(1.0 + (FREQS / fc_l) ** (2 * rng.choice([2, 4])))
    k = rng.integers(3, 6)
    cf = np.exp(np.sort(rng.uniform(np.log(150), np.log(7500), k)))
    db = np.interp(LOGF, np.log(cf), np.clip(rng.normal(0, 3, k), -7, 7))
    if rng.random() < 0.5:
        db = db + rng.uniform(0, 8) * np.exp(-0.5 * (np.log(FREQS / rng.uniform(700, 3500)) / 0.35) ** 2)
    return h * 10.0 ** (db / 20.0)


def room_response(rng, rt60=None):
    """spectrum of a synthetic room impulse response: the direct sound and an exponentially decaying tail"""
    rt = rt60 if rt60 is not None else rng.uniform(0.12, 0.7)
    n = int(min(rt * SR * 0.9, 9600))
    t = np.arange(n) / SR
    tail = rng.standard_normal(n) * np.exp(-6.9 * t / rt)
    tail[:80] = 0
    drr = rng.uniform(-3, 14)
    tail *= np.sqrt(10.0 ** (-drr / 10.0) / max(np.sum(tail ** 2), 1e-9))
    tail[0] += 1.0
    return np.fft.rfft(tail, NF)


def shaped_noise(rng, kind):
    w = np.fft.rfft(rng.standard_normal(N))
    f = np.fft.rfftfreq(N, 1.0 / SR)
    f[0] = 1.0
    w = w / (np.sqrt(f / 100.0) if kind == "pink" else (f / 100.0) if kind == "brown" else 1.0)
    return np.fft.irfft(w, N)


def trim(a):
    """a word without the silence around it"""
    a = a.astype(np.float32)
    env = np.convolve(np.abs(a), np.ones(160) / 160, "same")
    loud = np.flatnonzero(env > 0.05 * env.max()) if env.max() > 0 else []
    if len(loud) == 0:
        return a[:0]
    return a[max(0, loud[0] - 640):loud[-1] + 640]


def babble(rng, pool, talkers):
    """other people talking: each talker is a run of words with short gaps"""
    out = np.zeros(N, np.float32)
    for _ in range(talkers):
        parts, n = [], 0
        gain = rng.uniform(0.3, 1.0)
        while n < N + 16000:
            w = pool[rng.integers(len(pool))]
            w = w / (np.abs(w).max() + 1e-9) * gain * rng.uniform(0.7, 1.0)
            gap = np.zeros(int(rng.uniform(0, 0.15) * SR), np.float32)
            parts += [w, gap]
            n += len(w) + len(gap)
        s = np.concatenate(parts)
        o = rng.integers(0, len(s) - N)
        out += s[o:o + N]
    return out


class Ctx:
    """what the channel draws from: recorded noise, a pool of words for babble"""

    def __init__(self, noise, pool):
        self.noise = noise
        self.pool = pool


def bed(rng, ctx, kind):
    if kind == "sc":
        n = ctx.noise[rng.integers(len(ctx.noise))]
        s = rng.integers(0, len(n) - N)
        b = n[s:s + N].astype(np.float64)
    elif kind == "babble":
        b = babble(rng, ctx.pool, rng.integers(2, 6)).astype(np.float64)
    elif kind == "none":
        return np.zeros(N)
    else:
        b = shaped_noise(rng, kind)
        if rng.random() < 0.5:   # microphone and room color noise too
            b = np.fft.irfft(np.fft.rfft(b, NF) * eq_response(rng), NF)[:N]
    b = b - b.mean()
    return b / (np.sqrt(np.mean(b ** 2)) + 1e-9)


def active_rms(x):
    fr = (x[:N // 320 * 320].reshape(-1, 320) ** 2).mean(1)
    top = np.sort(fr)[-max(1, len(fr) // 4):]
    return float(np.sqrt(top.mean())) + 1e-9


def far_field(rng, ctx, speech, kind=None, snr=None, peak=None, normalize=None, p_reverb=0.6, rt60=None):
    """one clip as the pod would see it. speech: float samples or None (noise only)"""
    if kind is None:
        kind = BEDS[rng.choice(len(BEDS), p=P_BED)]
    if speech is not None:
        X = np.fft.rfft(speech.astype(np.float64), NF)
        if rt60 is not None or rng.random() < p_reverb:
            X = X * room_response(rng, rt60)
        x = np.fft.irfft(X * eq_response(rng), NF)[:N]
        if snr is None:
            snr = rng.choice([rng.uniform(2, 8), rng.uniform(8, 16), rng.uniform(16, 30)], p=[0.25, 0.35, 0.40]) \
                if kind != "babble" else rng.uniform(3, 18)
        x = x + bed(rng, ctx, kind) * active_rms(x) / 10.0 ** (snr / 20.0)
    else:
        x = bed(rng, ctx, kind)
        if kind in ("sc", "babble"):
            x = np.fft.irfft(np.fft.rfft(x, NF) * eq_response(rng), NF)[:N]
    m = np.abs(x).max()
    if peak is None:
        peak = np.exp(rng.uniform(np.log(150), np.log(9000)))
    x = np.rint(x * (peak / m)) if m > 0 else np.zeros(N)
    if normalize is None:
        normalize = rng.random() < 0.85
    if normalize:
        p = max(np.abs(x).max(), 64.0)
        x = np.rint(x * (PEAK / p))
    return np.clip(x, -32768, 32767).astype(np.int16)


def centered(a, rng=None, jitter=0.0):
    """a trimmed word in a 1 s buffer, near the middle"""
    a = a[:N].astype(np.float32)
    o = (N - len(a)) // 2
    if rng is not None and jitter:
        o = int(np.clip(o + rng.uniform(-jitter, jitter) * SR, 0, N - len(a)))
    out = np.zeros(N, np.float32)
    out[o:o + len(a)] = a
    return out


def shift(rng, a, ms=100):
    x = a.astype(np.float32)
    sh = int(rng.integers(-ms * 16, ms * 16 + 1))
    x = np.roll(x, sh)
    if sh > 0:
        x[:sh] = 0
    elif sh < 0:
        x[sh:] = 0
    return x


def pod_scale(a):
    """the pod's gain on a clip"""
    p = max(int(np.abs(a.astype(np.int32)).max()), 64)
    return np.clip(np.rint(a.astype(np.float64) * (PEAK / p)), -32768, 32767).astype(np.int16)


# ---- training copies --------------------------------------------------------

_CTX = {}


def pool_words(items, n, rng):
    idx = rng.choice(len(items), min(n, len(items)), replace=False)
    out = [trim(data.read_wav(items[i][0])) for i in sorted(idx)]
    return [w for w in out if len(w) > 2400]


def get_ctx(files):
    if "ctx" not in _CTX:
        rng = np.random.default_rng(5)
        unk = [(p, l) for p, l in files["train"] if l == data.UNKNOWN]
        tz = tts.load()
        words = pool_words(unk, 3000, rng)
        # a few synthesized words (not the held-out voices') among the people talking
        ok = [i for i in rng.permutation(len(tz["label"])) if tz["label"][i] == data.UNKNOWN and not tts.is_held(tz["voice"][i])][:300]
        words += [trim(tz["audio"][i, :tz["length"][i]]) for i in ok]
        _CTX["ctx"] = Ctx(data.noises(), words)
        _CTX["tts"] = tz
    return _CTX["ctx"], _CTX["tts"]


def feats_of(audio):
    return np.concatenate([frontend.features(np.stack(audio[i:i + 256])) for i in range(0, len(audio), 256)]).astype(np.int16)


def build_train(args):
    """features of one training copy: (int16 features, labels)"""
    copy, files, per_kw, n_tts = args
    rng = np.random.default_rng(7000 + copy)
    ctx, tz = get_ctx(files)
    kw = [(p, l) for p, l in files["train"] if l != data.UNKNOWN]
    unk = [(p, l) for p, l in files["train"] if l == data.UNKNOWN]
    pick = rng.choice(len(unk), min(2 * per_kw, len(unk)), replace=False)
    chosen = kw + [unk[i] for i in sorted(pick)]
    p_far = 0.65
    audio, labels = [], []
    for p, l in chosen:
        a = data.read_wav(p)
        if rng.random() < p_far:
            audio.append(far_field(rng, ctx, shift(rng, a)))
        else:
            x = data.augment(rng, a, ctx.noise)
            audio.append(pod_scale(x) if rng.random() < 0.5 else x)
        labels.append(l)
    # other people talking, no keyword: _unknown_
    for _ in range(int(1.5 * per_kw)):
        t = int(rng.choice([1, 2, 3, 4, 5], p=[0.15, 0.25, 0.25, 0.2, 0.15]))
        audio.append(far_field(rng, ctx, babble(rng, ctx.pool, t), kind=BEDS[rng.choice(4)] if rng.random() < 0.5 else "none"))
        labels.append(data.UNKNOWN)
    # silence: recorded and synthetic noise
    for _ in range(per_kw):
        if rng.random() < 0.5:
            audio.append(data.silence(rng, ctx.noise))
        elif rng.random() < 0.1:
            audio.append(np.zeros(N, np.int16))
        else:
            audio.append(far_field(rng, ctx, None, kind=BEDS[rng.choice(4)]))
        labels.append(data.SILENCE)
    # a few synthesized voices (the held-out ones stay out)
    ok = np.flatnonzero([not tts.is_held(v) for v in tz["voice"]])
    kwi = ok[tz["label"][ok] != data.UNKNOWN]
    unki = ok[tz["label"][ok] == data.UNKNOWN]
    for i in list(rng.choice(kwi, n_tts)) + list(rng.choice(unki, n_tts)):
        w = tz["audio"][i, :tz["length"][i]].astype(np.float32)
        audio.append(far_field(rng, ctx, centered(w, rng, 0.25)))
        labels.append(int(tz["label"][i]))
    order = rng.permutation(len(audio))
    audio = [audio[i] for i in order]
    labels = np.array(labels, np.int8)[order]
    return feats_of(audio), labels


def far_split(args):
    """a split's clips through the chain: (int16 features, labels)"""
    split, audio, labels, seed = args
    rng = np.random.default_rng(seed)
    files = data.split_lists()
    ctx = Ctx(data.noises(), pool_words([(p, l) for p, l in files["val" if split == "val" else "test"] if l == data.UNKNOWN], 1500, rng))
    out = []
    for a, l in zip(audio, labels):
        if l == data.SILENCE:
            out.append(far_field(rng, ctx, None, kind=BEDS[rng.choice(4)]))
        else:
            out.append(far_field(rng, ctx, shift(rng, a)))
    return feats_of(out), labels


def load_train(copies=4, workers=2):
    path = os.path.join(CACHE, "far2-%d.npz" % copies)
    if os.path.exists(path):
        z = np.load(path)
        return z["train_x"], z["train_y"], z["val_x"], z["val_y"]
    files = data.split_lists()
    per_kw = len([1 for _, l in files["train"] if l != data.UNKNOWN]) // len(data.KEYWORDS)
    z0 = np.load(os.path.join(CACHE, "features-4.npz"))
    jobs = [(c, files, per_kw, 400) for c in range(copies)]
    with Pool(workers) as pool:
        res = pool.map(build_train, jobs)
        vx, vy = pool.map(far_split, [("val", z0["val_a"], z0["val_y"], 31)])[0]
    tx = np.concatenate([r[0] for r in res])
    ty = np.concatenate([r[1] for r in res])
    np.savez(path, train_x=tx, train_y=ty, val_x=vx, val_y=vy)
    return tx, ty, vx, vy


# ---- tests ------------------------------------------------------------------

def classify(model, audio):
    f = frontend.features(np.stack(audio))
    return np.array([int(np.argmax(model.forward(x))) for x in f])


def confusion(y, p, k=len(data.LABELS)):
    cm = np.zeros((k, k), np.int64)
    for t, q in zip(y, p):
        cm[t, q] += 1
    return cm


def test_sets():
    """the clips every test uses, fixed seeds"""
    z = np.load(os.path.join(CACHE, "features-4.npz"))
    files = data.split_lists()
    rng = np.random.default_rng(99)
    pool = pool_words([(p, l) for p, l in files["test"] if l == data.UNKNOWN], 1500, rng)
    tz = tts.load()
    held = np.flatnonzero([tts.is_held(v) for v in tz["voice"]])
    # babble from the held-out voices' other words too
    other = [trim(tz["audio"][i, :tz["length"][i]]) for i in held if tz["label"][i] == data.UNKNOWN]
    return z["test_a"], z["test_y"], pool, other, tz, held


def run(model):
    ta, ty, pool, other, tz, held = test_sets()
    noise = data.noises()
    res = {}
    kwunk = np.flatnonzero(ty != data.SILENCE)
    # standard
    p = classify(model, list(ta))
    res["speech_commands"] = {"accuracy": float(np.mean(p == ty)), "confusion": confusion(ty, p).tolist()}
    # far field: quiet, reverberant, colored, noisy, then the pod's gain
    for snr in (20, 10, 5):
        rng = np.random.default_rng(1000 + snr)
        ctx = Ctx(noise, pool)
        aud = [far_field(rng, ctx, shift(rng, ta[i]), kind=("pink", "white", "sc", "babble")[j % 4], snr=snr,
                         peak=rng.uniform(300, 900), normalize=True, p_reverb=1.0, rt60=rng.uniform(0.15, 0.5))
               for j, i in enumerate(kwunk)]
        p = classify(model, aud)
        res["far_snr%d" % snr] = {"accuracy": float(np.mean(p == ty[kwunk])), "confusion": confusion(ty[kwunk], p).tolist()}
    # false triggers: babble of 1-5 talkers and noise alone, scaled by the pod's gain
    for name, mk in (("babble_1", lambda r: babble(r, pool, 1)),
                     ("babble_3_5", lambda r: babble(r, pool, int(r.integers(3, 6)))),
                     ("babble_synth_1", lambda r: babble(r, other, 1)),
                     ("babble_synth_3_5", lambda r: babble(r, other, int(r.integers(3, 6))))):
        rng = np.random.default_rng(2000 + len(name))
        ctx = Ctx(noise, pool)
        aud = [far_field(rng, ctx, mk(rng), kind="none", peak=rng.uniform(300, 900), normalize=True,
                         p_reverb=1.0, rt60=rng.uniform(0.15, 0.5)) for _ in range(500)]
        p = classify(model, aud)
        res[name] = {"keyword_rate": float(np.mean(p < data.UNKNOWN)), "unknown_rate": float(np.mean(p == data.UNKNOWN)),
                     "clips": len(aud)}
    rng = np.random.default_rng(3000)
    ctx = Ctx(noise, pool)
    aud = [far_field(rng, ctx, None, kind=("pink", "white", "sc", "brown")[j % 4], peak=rng.uniform(100, 900), normalize=True)
           for j in range(400)]
    p = classify(model, aud)
    res["noise_only"] = {"keyword_rate": float(np.mean(p < data.UNKNOWN)), "silence_rate": float(np.mean(p == data.SILENCE)), "clips": len(aud)}
    # held-out synthesized voices: as spoken (centered, the pod's gain), and through the chain
    tl = tz["label"][held]
    clean = [pod_scale(centered(tz["audio"][i, :tz["length"][i]]).astype(np.int16)) for i in held]
    p = classify(model, clean)
    kwm = tl != data.UNKNOWN
    res["tts"] = {"accuracy": float(np.mean(p == tl)), "keyword_recall": float(np.mean(p[kwm] == tl[kwm])),
                  "confusion": confusion(tl, p).tolist(), "clips": int(len(tl)),
                  "voices": sorted({v.split(" (")[0] for v in tz["voice"][held]}),
                  "by_voice_keyword_recall": {}}
    for v in res["tts"]["voices"]:
        m = np.array([tz["voice"][i].split(" (")[0] == v for i in held]) & kwm
        res["tts"]["by_voice_keyword_recall"][v] = float(np.mean(p[m] == tl[m]))
    for snr in (20, 10):
        rng = np.random.default_rng(4000 + snr)
        ctx = Ctx(noise, pool)
        aud = [far_field(rng, ctx, centered(tz["audio"][i, :tz["length"][i]], rng, 0.2), kind=("pink", "white", "sc", "babble")[j % 4],
                         snr=snr, peak=rng.uniform(300, 900), normalize=True, p_reverb=1.0, rt60=rng.uniform(0.15, 0.5))
               for j, i in enumerate(held)]
        p = classify(model, aud)
        res["tts_far_snr%d" % snr] = {"accuracy": float(np.mean(p == tl)), "keyword_recall": float(np.mean(p[kwm] == tl[kwm]))}
    return res


def report(res):
    L = [l[:6] for l in data.LABELS]
    print("Speech Commands test      %.4f" % res["speech_commands"]["accuracy"])
    for snr in (20, 10, 5):
        print("far field, SNR %2d dB      %.4f" % (snr, res["far_snr%d" % snr]["accuracy"]))
    for k in ("babble_1", "babble_3_5", "babble_synth_1", "babble_synth_3_5"):
        print("%-12s keyword false triggers %.3f, called unknown %.3f" % (k, res[k]["keyword_rate"], res[k]["unknown_rate"]))
    print("noise only   keyword false triggers %.3f, called silence %.3f" % (res["noise_only"]["keyword_rate"], res["noise_only"]["silence_rate"]))
    t = res["tts"]
    print("held-out voices %s (%d clips): accuracy %.4f, keyword recall %.4f %s" % (t["voices"], t["clips"], t["accuracy"], t["keyword_recall"],
                                                                              {k: round(v, 3) for k, v in t["by_voice_keyword_recall"].items()}))
    for snr in (20, 10):
        r = res["tts_far_snr%d" % snr]
        print("held-out voices, chain SNR %2d dB: accuracy %.4f, keyword recall %.4f" % (snr, r["accuracy"], r["keyword_recall"]))
    for name in ("speech_commands", "far_snr10", "tts"):
        cm = np.array(res[name]["confusion"])
        print("\n%s (rows true, columns predicted)" % name)
        print("%-7s" % "" + "".join("%7s" % l for l in L))
        for i, l in enumerate(L):
            print("%-7s" % l + "".join("%7d" % v for v in cm[i]))


def main():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--copies", type=int, default=4)
    e = sub.add_parser("score")
    e.add_argument("--model", required=True)
    e.add_argument("--json")
    a = p.parse_args()
    if a.cmd == "build":
        tx, ty, vx, vy = load_train(a.copies)
        print("train", len(ty), np.bincount(ty, minlength=8), "val", len(vy))
    else:
        import kws
        res = run(kws.Model(open(a.model, "rb").read()))
        report(res)
        if a.json:
            json.dump(res, open(a.json, "w"), indent=1)


if __name__ == "__main__":
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    main()
