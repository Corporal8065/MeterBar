#!/usr/bin/env python3
"""Synthesize MeterBar's Anime-mode UI click sounds (v0.16).

No external samples: every sound is a pure sine gesture rendered here.
Run once; the WAVs are build inputs, embedded into the app via JUCE
BinaryData (see app/CMakeLists.txt). Parameters (44.1 kHz, 16-bit mono):

  pop.wav     70 ms — sine gliding 880 -> 320 Hz (exponential), fast
              decay envelope, peak 0.50.
  bloop.wav   90 ms — sine gliding 300 -> 720 Hz (exponential), soft
              attack/decay envelope, peak 0.45.
  sparkle.wav 160 ms — two high sine pings (1568 Hz then 2093 Hz,
              60 ms apart), each decaying, peak 0.40.
"""
import math
import struct
import wave
import os

SR = 44100
OUT = os.path.dirname(os.path.abspath(__file__))


def render(seconds, fn):
    n = int(SR * seconds)
    return [fn(i / SR, i, n) for i in range(n)]


def save(name, samples):
    path = os.path.join(OUT, name)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        frames = b"".join(
            struct.pack("<h", int(max(-1.0, min(1.0, s)) * 32767)) for s in samples
        )
        w.writeframes(frames)
    print("wrote", path, len(samples), "samples")


def glide(f0, f1, t, dur):
    # phase of an exponential frequency glide f0 -> f1 over dur
    k = math.log(f1 / f0)
    frac = t / dur
    return 2 * math.pi * f0 * dur * (math.exp(k * frac) - 1) / k


def pop(t, i, n):
    dur = 0.070
    env = math.exp(-t / 0.018)
    return 0.50 * env * math.sin(glide(880.0, 320.0, t, dur))


def bloop(t, i, n):
    dur = 0.090
    env = math.sin(math.pi * min(1.0, t / dur)) ** 1.5
    return 0.45 * env * math.sin(glide(300.0, 720.0, t, dur))


def sparkle(t, i, n):
    out = 0.0
    for start, f in ((0.0, 1568.0), (0.060, 2093.0)):
        lt = t - start
        if lt >= 0:
            out += 0.40 * math.exp(-lt / 0.030) * math.sin(2 * math.pi * f * lt)
    return out


save("click-pop.wav", render(0.070, pop))
save("click-bloop.wav", render(0.090, bloop))
save("click-sparkle.wav", render(0.160, sparkle))
