"""Speech Commands v2 as the keyword spotter's classes, features precomputed.

Classes: the six keywords, _unknown_ (every other word) and _silence_
(background noise at a random volume, or nothing). The split follows the
dataset's validation_list.txt and testing_list.txt. Training copies are
augmented: shifted up to 100 ms, mixed with background noise (p 0.8, volume
up to 0.1), and each copy draws a new set of unknown words.
"""
import glob
import os
import wave
from multiprocessing import Pool

import numpy as np

import frontend

ROOT = os.path.expanduser("~/.cache/kube32-models/data/speech_commands")
CACHE = os.path.expanduser("~/.cache/kube32-models/data/kws_features")
KEYWORDS = ("cat", "dog", "bird", "house", "tree", "bed")
LABELS = KEYWORDS + ("_unknown_", "_silence_")
UNKNOWN, SILENCE = 6, 7


def read_wav(path):
    with wave.open(path) as w:
        a = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
    out = np.zeros(frontend.SAMPLES, np.int16)
    out[: min(len(a), frontend.SAMPLES)] = a[: frontend.SAMPLES]
    return out


def split_lists():
    val = set(open(os.path.join(ROOT, "validation_list.txt")).read().split())
    test = set(open(os.path.join(ROOT, "testing_list.txt")).read().split())
    files = {"train": [], "val": [], "test": []}
    for path in sorted(glob.glob(os.path.join(ROOT, "*", "*.wav"))):
        rel = os.path.relpath(path, ROOT)
        word = rel.split("/")[0]
        if word == "_background_noise_":
            continue
        label = KEYWORDS.index(word) if word in KEYWORDS else UNKNOWN
        s = "val" if rel in val else "test" if rel in test else "train"
        files[s].append((path, label))
    return files


def noises():
    out = []
    for p in sorted(glob.glob(os.path.join(ROOT, "_background_noise_", "*.wav"))):
        with wave.open(p) as w:
            out.append(np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float32))
    return out


def noise_clip(rng, noise):
    n = noise[rng.integers(len(noise))]
    s = rng.integers(0, len(n) - frontend.SAMPLES)
    return n[s:s + frontend.SAMPLES]


def silence(rng, noise):
    if rng.random() < 0.1:
        return np.zeros(frontend.SAMPLES, np.int16)
    return np.clip(noise_clip(rng, noise) * rng.uniform(0, 1), -32768, 32767).astype(np.int16)


def augment(rng, a, noise):
    x = a.astype(np.float32)
    sh = rng.integers(-1600, 1601)
    x = np.roll(x, sh)
    if sh > 0:
        x[:sh] = 0
    elif sh < 0:
        x[sh:] = 0
    if rng.random() < 0.8:
        x = x + noise_clip(rng, noise) * rng.uniform(0, 0.1)
    return np.clip(x, -32768, 32767).astype(np.int16)


def build_split(args):
    """features and labels of one split (or one augmented training copy)"""
    split, copy, items, n_unknown, n_silence = args
    rng = np.random.default_rng(1000 * copy + {"train": 0, "val": 1, "test": 2}[split])
    noise = noises()
    kw = [(p, l) for p, l in items if l != UNKNOWN]
    unk = [(p, l) for p, l in items if l == UNKNOWN]
    pick = rng.choice(len(unk), min(n_unknown, len(unk)), replace=False)
    chosen = kw + [unk[i] for i in sorted(pick)]
    audio, labels = [], []
    for p, l in chosen:
        a = read_wav(p)
        audio.append(augment(rng, a, noise) if split == "train" else a)
        labels.append(l)
    for _ in range(n_silence):
        audio.append(silence(rng, noise))
        labels.append(SILENCE)
    audio = np.stack(audio)
    feats = np.concatenate([frontend.features(audio[i:i + 256]) for i in range(0, len(audio), 256)])
    return audio if split != "train" else None, feats, np.array(labels, np.int8)


def load(copies=4, workers=2):
    """{"train": (feats, labels) over the copies, "val"/"test": (audio, feats, labels)}"""
    os.makedirs(CACHE, exist_ok=True)
    path = os.path.join(CACHE, "features-%d.npz" % copies)
    if os.path.exists(path):
        z = np.load(path)
        return {"train": (z["train_x"], z["train_y"]), "val": (z["val_a"], z["val_x"], z["val_y"]),
                "test": (z["test_a"], z["test_x"], z["test_y"])}
    files = split_lists()
    per_kw = {s: len([1 for _, l in files[s] if l != UNKNOWN]) // len(KEYWORDS) for s in files}
    jobs = [("train", c, files["train"], 2 * per_kw["train"], per_kw["train"]) for c in range(copies)]
    jobs += [(s, 0, files[s], per_kw[s], per_kw[s]) for s in ("val", "test")]
    with Pool(workers) as pool:
        res = pool.map(build_split, jobs)
    tx = np.concatenate([r[1] for r in res[:copies]])
    ty = np.concatenate([r[2] for r in res[:copies]])
    (va, vx, vy), (ta, tx2, ty2) = res[copies], res[copies + 1]
    np.savez(path, train_x=tx, train_y=ty, val_a=va, val_x=vx, val_y=vy, test_a=ta, test_x=tx2, test_y=ty2)
    return {"train": (tx, ty), "val": (va, vx, vy), "test": (ta, tx2, ty2)}


if __name__ == "__main__":
    d = load()
    for k, v in d.items():
        y = v[-1]
        print(k, len(y), np.bincount(y, minlength=len(LABELS)))
