#!/usr/bin/env python3
"""The puddle formula, off the GPU, so its shape can be iterated on without the game.

    python Tools/vcspuddles.py out.png [drainPatchSize] [puddleScale]


The shader's puddle term is a pure function of (world x, world y, wetness). Rendering it here at
several wetness levels is a far better instrument than a screenshot: the game decides where the
camera is, it takes ninety seconds to dry, and a still frame cannot show a sequence anyway.

Keep in step with WriteShadeFS in GPU/Common/VCSWater.cpp.
"""
import math, struct, sys, zlib

def hash21(x, y):
    v = math.sin(x * 127.1 + y * 311.7) * 43758.5453
    return v - math.floor(v)

def vnoise(x, y):
    ix, iy = math.floor(x), math.floor(y)
    fx, fy = x - ix, y - iy
    wx = fx * fx * (3 - 2 * fx)
    wy = fy * fy * (3 - 2 * fy)
    a, b = hash21(ix, iy), hash21(ix + 1, iy)
    c, d = hash21(ix, iy + 1), hash21(ix + 1, iy + 1)
    top = a + (b - a) * wx
    bot = c + (d - c) * wx
    return top + (bot - top) * wy

def puddle_noise(x, y, scale):
    # Two octaves. One is too smooth to break a wet road into anything you would call a puddle.
    return vnoise(x * scale, y * scale) * 0.65 + vnoise(x * scale * 2.7 + 11.3,
                                                        y * scale * 2.7 + 5.1) * 0.35

def smoothstep(e0, e1, x):
    if e1 <= e0:
        return 1.0 if x >= e1 else 0.0
    t = max(0.0, min(1.0, (x - e0) / (e1 - e0)))
    return t * t * (3 - 2 * t)

HALF, FEATHER_RATIO = 4.0, 0.6
FEATHER = HALF * FEATHER_RATIO
REACH = HALF + FEATHER

def puddle_at(wx, wy, wetness, drain_inv, puddle_scale):
    d = abs(wy)
    if d >= REACH:
        return 0.0
    ramp = 1.0 - d / REACH
    kerb = smoothstep(0.0, FEATHER / REACH, ramp)
    # Over the DRIVABLE half width, not the whole ramp - see the note in the shader.
    across = min(1.0, (1.0 - ramp) / (1.0 - FEATHER / REACH))
    drain = smoothstep(0.35, 0.65, vnoise(wx * drain_inv, wy * drain_inv))
    band = 1.0 - smoothstep(wetness - 0.18, wetness + 0.06, abs(across - drain))
    n = puddle_noise(wx, wy, puddle_scale)
    patch = smoothstep(1.02 - wetness - 0.28, 1.02 - wetness + 0.04, n)
    return max(0.0, min(1.0, max(band * patch, wetness ** 3))) * kerb

def render(out, drain_inv, puddle_scale, levels=(1.00, 0.75, 0.50, 0.30, 0.15, 0.05)):
    W, H, GAP = 900, 110, 10
    metres_long, metres_across = 90.0, 13.0
    rows = []
    print("wetness | wet area | mean | runs of wet along the road (median length, m)")
    for wetness in levels:
        band = bytearray()
        total, count, runs = 0.0, 0, []
        for py in range(H):
            wy = (py - H / 2.0) * (metres_across / H)
            run = 0
            for px in range(W):
                wx = px * (metres_long / W)
                v = puddle_at(wx, wy, wetness, drain_inv, puddle_scale)
                band.append(int(v * 255))
                if abs(wy) < REACH:
                    total += v
                    count += 1
                    if v > 0.4:
                        run += 1
                    elif run:
                        runs.append(run * metres_long / W)
                        run = 0
            if run:
                runs.append(run * metres_long / W)
        rows.append(band)
        runs.sort()
        med = runs[len(runs) // 2] if runs else 0.0
        print("  %.2f  |  %5.1f%%  | %.3f | %5.1f  (%d runs)"
              % (wetness, 100.0 * sum(1 for b in band if b > 100) / (W * H),
                 total / max(count, 1), med, len(runs)))

    sheet_h = (H + GAP) * len(rows) + GAP
    canvas = bytearray(b"\x20" * (W * GAP))
    for band in rows:
        canvas += band + bytearray(b"\x20" * (W * GAP))
    raw = b"".join(b"\x00" + bytes(canvas[y * W:(y + 1) * W]) for y in range(sheet_h))
    def chunk(t, d):
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    open(out, "wb").write(b"\x89PNG\r\n\x1a\n" +
        chunk(b"IHDR", struct.pack(">IIBBBBB", W, sheet_h, 8, 0, 0, 0, 0)) +
        chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))
    print("wrote", out)

if __name__ == "__main__":
    out = sys.argv[1]
    drain_patch = float(sys.argv[2]) if len(sys.argv) > 2 else 24.0
    puddle_scale = float(sys.argv[3]) if len(sys.argv) > 3 else 0.09
    print("drainPatchSize %.0f, puddleScale %.2f (features about %.1f m)"
          % (drain_patch, puddle_scale, 1.0 / puddle_scale))
    render(out, 1.0 / drain_patch, puddle_scale)
