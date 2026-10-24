#!/usr/bin/env python3
"""Sound visualizer for the Claude Cube: microphone -> 24 bands -> UDP.

Reads the default input (the Mac's mic), takes an FFT ~30 times a second,
folds it into 24 log-spaced bands with automatic gain, flags kick-drum beats,
and sends 'V' + 24 bytes + flags to the cube's UDP port 4210. The cube shows
the visualizer while packets keep coming and goes back to normal ~1.5 s after
they stop.

    python3 cube_viz.py          # microphone
    python3 cube_viz.py --demo   # synthetic music, no mic needed

Needs numpy and sounddevice (pip3 install --user numpy sounddevice).
Cube address: $CUBE_HOST, else ~/.claude/cube_host, else claudecube.local.
"""
import math
import os
import socket
import sys
import time

import numpy as np

RATE, N, BANDS, PORT = 44100, 2048, 24, 4210
EDGES = np.geomspace(45, 12000, BANDS + 1)
FREQS = np.fft.rfftfreq(N, 1 / RATE)
BINS = [np.where((FREQS >= EDGES[i]) & (FREQS < EDGES[i + 1]))[0] for i in range(BANDS)]


def host():
    h = os.environ.get("CUBE_HOST")
    if h:
        return h
    try:
        with open(os.path.expanduser("~/.claude/cube_host")) as f:
            return f.read().strip() or "claudecube.local"
    except OSError:
        return "claudecube.local"


class Bands:
    def __init__(s):
        s.peak = 1e-6
        s.bass_avg = 1e-6
        s.window = np.hanning(N)

    def __call__(s, block):
        spec = np.abs(np.fft.rfft(block * s.window))
        b = np.array([spec[i].mean() if len(i) else 0.0 for i in BINS])
        b = np.log1p(b * 40)
        s.peak = max(s.peak * 0.995, b.max(), 0.05)  # slow automatic gain
        levels = np.clip(b / s.peak, 0, 1)
        bass = b[:3].mean()
        beat = bass > s.bass_avg * 1.35 and bass > 0.1
        s.bass_avg = s.bass_avg * 0.9 + bass * 0.1
        return (levels * 255).astype(np.uint8), beat


def send(sock, addr, levels, beat):
    sock.sendto(b"V" + bytes(levels) + bytes([1 if beat else 0]), addr)


def demo(sock, addr):
    fold, t0 = Bands(), time.time()
    while True:
        t = time.time() - t0
        x = np.arange(N) / RATE + t
        kick = math.exp(-((t * 2) % 1) * 9)  # 120 bpm
        sig = (kick * np.sin(2 * np.pi * 60 * x) + 0.4 * np.sin(2 * np.pi * 440 * (1 + 0.1 * math.sin(t)) * x)
               + 0.25 * np.sin(2 * np.pi * 2200 * x) * (0.5 + 0.5 * math.sin(t * 3)) + 0.05 * np.random.randn(N))
        send(sock, addr, *fold(sig))
        time.sleep(1 / 30)


def input_device():
    """The mic to listen to: $CUBE_MIC (name substring), else Camo, else the system default."""
    import sounddevice as sd
    for want in [os.environ.get("CUBE_MIC"), "Camo"]:
        if not want:
            continue
        for i, d in enumerate(sd.query_devices()):
            if d["max_input_channels"] > 0 and want.lower() in d["name"].lower():
                return i
    return None


def mic(sock, addr):
    import sounddevice as sd
    fold, buf = Bands(), np.zeros(N)
    dev = input_device()
    sr = int(sd.query_devices(dev, "input")["default_samplerate"]) if dev is not None else RATE
    loud_until, floor = 0.0, 1e-4
    always = os.environ.get("CUBE_VIZ_ALWAYS") == "1"  # CubeSenses runs us only while he dictates
    peak = 0.05
    # only while there's real sound: the cube shows the visualizer as long as packets come
    with sd.InputStream(device=dev, samplerate=sr, channels=1, blocksize=sr // 30) as st:
        while True:
            data, _ = st.read(sr // 30)
            buf = np.roll(buf, -len(data))
            buf[-len(data):] = data[:, 0]
            rms = float(np.sqrt(np.mean(data[:, 0] ** 2)))
            floor = min(floor * 1.0005 + 1e-7, rms) if rms < floor * 4 else floor * 1.0005  # room noise level
            if always or rms > max(0.01, floor * 6):
                loud_until = time.time() + 3
            if always:  # dictation: just how loud he is, for the cube's wave line
                level = min(1.0, rms / max(peak, 1e-4))
                peak = max(peak * 0.995, rms, 0.02)
                sock.sendto(b"L" + bytes([int(level * 255)]), addr)
            elif time.time() < loud_until:
                send(sock, addr, *fold(buf))


if __name__ == "__main__":
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    a = (host(), PORT)
    try:
        demo(s, a) if "--demo" in sys.argv else mic(s, a)
    except KeyboardInterrupt:
        pass
