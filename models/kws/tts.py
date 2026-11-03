"""Spoken words from the host's speech synthesizer (macOS `say`), used as a
held-out test of the keyword spotter and for a small share of its training
words.

  tts.py [--out FILE]     every English voice at three rates, the keywords
                          and a list of other words: int16 16 kHz, silence
                          trimmed, at most 1 s
"""
import argparse
import os
import re
import subprocess
import tempfile
import wave
from concurrent.futures import ThreadPoolExecutor

import numpy as np

import data

CACHE = os.path.expanduser("~/.cache/kube32-models/data/kws_tts.npz")
RATES = (120, 175, 230)
# voices that sing or buzz are left out
SKIP = {"Bad News", "Bahh", "Bells", "Boing", "Bubbles", "Cellos", "Good News", "Jester", "Organ",
        "Superstar", "Trinoids", "Whisper", "Wobble", "Zarvox"}
HELD_OUT = ("Samantha", "Daniel", "Karen")
OTHER = ("yes", "no", "up", "down", "left", "right", "on", "off", "stop", "go", "zero", "one", "two",
         "three", "four", "five", "six", "seven", "eight", "nine", "happy", "wow", "follow", "learn",
         "bad", "bat", "hat", "dig", "fog", "mouse", "table", "music")


def voices():
    out = subprocess.run(["say", "-v", "?"], capture_output=True, text=True, check=True).stdout
    names = []
    for line in out.splitlines():
        m = re.match(r"^(.*?)\s+en_\w+\s+#", line)
        if not m:
            continue
        name = m.group(1).strip()
        base = name.split(" (")[0]
        if base not in SKIP and name not in names:
            names.append(name)
    return names


def say(args):
    voice, rate, word = args
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "w.wav")
        subprocess.run(["say", "-v", voice, "-r", str(rate), "-o", path, "--file-format=WAVE",
                        "--data-format=LEI16@16000", word], check=True, timeout=30)
        with wave.open(path) as w:
            a = np.frombuffer(w.readframes(w.getnframes()), "<i2")
    if len(a) == 0:
        return np.zeros(0, np.int16)
    loud = np.flatnonzero(np.abs(a.astype(np.int32)) > max(100, int(np.abs(a).max()) // 30))
    if len(loud) == 0:
        return np.zeros(0, np.int16)
    a = a[max(0, loud[0] - 800):loud[-1] + 800]
    return a[:16000].astype(np.int16)


def build(out=CACHE):
    vs = voices()
    words = data.KEYWORDS + OTHER
    jobs = [(v, r, w) for v in vs for r in RATES for w in words]
    with ThreadPoolExecutor(4) as ex:
        clips = list(ex.map(say, jobs))
    keep = [i for i, c in enumerate(clips) if len(c) > 1600]
    audio = np.zeros((len(keep), 16000), np.int16)
    length = np.zeros(len(keep), np.int32)
    for j, i in enumerate(keep):
        audio[j, :len(clips[i])] = clips[i]
        length[j] = len(clips[i])
    label = np.array([data.KEYWORDS.index(jobs[i][2]) if jobs[i][2] in data.KEYWORDS else data.UNKNOWN for i in keep], np.int8)
    voice = np.array([jobs[i][0] for i in keep])
    word = np.array([jobs[i][2] for i in keep])
    rate = np.array([jobs[i][1] for i in keep], np.int32)
    np.savez_compressed(out, audio=audio, length=length, label=label, voice=voice, word=word, rate=rate)
    return len(keep), len(vs)


def load(path=CACHE):
    if not os.path.exists(path):
        build(path)
    z = np.load(path)
    return {k: z[k] for k in z.files}


def is_held(voice):
    return voice.split(" (")[0] in HELD_OUT


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--out", default=CACHE)
    a = p.parse_args()
    n, v = build(a.out)
    print("%d clips from %d voices" % (n, v))
