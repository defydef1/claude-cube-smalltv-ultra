#!/usr/bin/env python3
"""Re-encode any GIF (or video frames) into a Claude Cube GIF.

The cube's GIF decoder is built with a 10-bit LZW dictionary (~8 KB of RAM instead
of ~23 KB), so it can only play GIFs whose LZW codes never exceed 10 bits. This
encoder writes exactly that: one global palette (255 colours + 1 transparent),
frames stored as the changed rectangle over the previous frame with unchanged
pixels transparent, LZW reset before the dictionary passes 1024 entries, and a
"CUBE10" comment the firmware checks before playing.

    python3 cube_gif.py in.gif out.gif [--size 240] [--fps N]
"""
import argparse
import sys

from PIL import Image, ImageSequence

MAX_BITS = 10
MARK = b"CUBE10"


def lzw(indices, min_size=8):
    """GIF LZW with codes capped at MAX_BITS (clear code before the table would grow past it)."""
    clear, eoi = 1 << min_size, (1 << min_size) + 1
    out, acc, nbits = bytearray(), 0, 0

    def emit(code, size):
        nonlocal acc, nbits
        acc |= code << nbits
        nbits += size
        while nbits >= 8:
            out.append(acc & 255)
            acc >>= 8
            nbits -= 8

    def reset():
        return {bytes([i]): i for i in range(clear)}, eoi + 1, min_size + 1

    table, nxt, size = reset()
    emit(clear, size)
    w = b""
    for px in indices:
        c = bytes([px])
        wc = w + c
        if wc in table:
            w = wc
            continue
        emit(table[w], size)
        if nxt < (1 << MAX_BITS):
            table[wc] = nxt
            nxt += 1
            if nxt > (1 << size) and size < MAX_BITS:
                size += 1
        if nxt >= (1 << MAX_BITS):  # dictionary full at 10 bits: start over
            emit(clear, size)
            table, nxt, size = reset()
        w = c
    if w:
        emit(table[w], size)
    emit(eoi, size)
    if nbits:
        out.append(acc & 255)
    return bytes(out)


def blocks(data):
    out = bytearray()
    for i in range(0, len(data), 255):
        chunk = data[i:i + 255]
        out += bytes([len(chunk)]) + chunk
    return bytes(out + b"\x00")


def load_frames(path, size, fps):
    im = Image.open(path)
    frames, delays = [], []
    for f in ImageSequence.Iterator(im):
        frames.append(f.convert("RGB"))
        delays.append(f.info.get("duration", 100) or 100)
    if fps:
        # resample to a fixed rate by dropping/duplicating source frames
        total, step, t, out, d_out = sum(delays), 1000.0 / fps, 0.0, [], []
        starts = [sum(delays[:i]) for i in range(len(delays))]
        while t < total:
            i = max(j for j in range(len(starts)) if starts[j] <= t)
            out.append(frames[i]); d_out.append(int(round(step)))
            t += step
        frames, delays = out, d_out
    sq = []
    for f in frames:  # centre-crop to a square, then scale to the panel
        w, h = f.size
        s = min(w, h)
        f = f.crop(((w - s) // 2, (h - s) // 2, (w - s) // 2 + s, (h - s) // 2 + s))
        t = min(size, s)  # never upscale: the cube centres smaller GIFs
        sq.append(f.resize((t, t), Image.LANCZOS) if s != t else f)
    return sq, delays


def encode(frames, delays, out_path):
    size = frames[0].size[0]
    # one palette for the whole loop, from a strip of (up to 24) frames
    pick = frames[:: max(1, len(frames) // 24)]
    strip = Image.new("RGB", (size, size * len(pick)))
    for i, f in enumerate(pick):
        strip.paste(f, (0, i * size))
    pal_img = strip.quantize(255, method=Image.MEDIANCUT)
    pal = pal_img.getpalette()[: 255 * 3]
    pal += [0] * (256 * 3 - len(pal))
    TRANSP = 255
    idx_frames = [list(f.quantize(palette=pal_img, dither=Image.NONE).getdata()) for f in frames]

    g = bytearray(b"GIF89a")
    g += size.to_bytes(2, "little") * 2 + bytes([0xF7, 0, 0]) + bytes(pal)
    g += b"\x21\xFE" + blocks(MARK)                                   # the marker the firmware looks for
    g += b"\x21\xFF\x0BNETSCAPE2.0\x03\x01\x00\x00\x00"               # loop forever
    prev = None
    for n, (idx, d) in enumerate(zip(idx_frames, delays)):
        if prev is None:
            x0, y0, x1, y1, data, transp = 0, 0, size, size, idx, False
        else:
            changed = [i for i in range(len(idx)) if idx[i] != prev[i]]
            if not changed:
                changed = [0]
            ys = [i // size for i in changed]; xs = [i % size for i in changed]
            x0, y0, x1, y1 = min(xs), min(ys), max(xs) + 1, max(ys) + 1
            data = []
            for y in range(y0, y1):
                row, prow = idx[y * size + x0: y * size + x1], prev[y * size + x0: y * size + x1]
                data += [c if c != p else TRANSP for c, p in zip(row, prow)]
            transp = True
        cs = max(2, d // 10)
        g += b"\x21\xF9\x04" + bytes([0x05 if transp else 0x04]) + cs.to_bytes(2, "little") + bytes([TRANSP, 0])
        g += b"\x2C" + x0.to_bytes(2, "little") + y0.to_bytes(2, "little")
        g += (x1 - x0).to_bytes(2, "little") + (y1 - y0).to_bytes(2, "little") + b"\x00"
        g += b"\x08" + blocks(lzw(data))
        prev = idx
    g += b"\x3B"
    open(out_path, "wb").write(g)
    return len(g)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--size", type=int, default=240)
    ap.add_argument("--fps", type=float, default=0)
    a = ap.parse_args()
    frames, delays = load_frames(a.src, a.size, a.fps)
    n = encode(frames, delays, a.dst)
    print("%s: %d frames, %d bytes" % (a.dst, len(frames), n))


if __name__ == "__main__":
    main()
