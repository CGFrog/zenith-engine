"""Writes the showcase's decal textures.

Small procedural RGBA PNGs for clustered decals (DecalComponent): the albedo
alpha is the decal mask, colors are sRGB and normal maps are tangent space with
+Y up (glTF convention). Pure Python; no dependencies.

    python tools/showcase/build_decal_textures.py assets/showcase/decals

- decal_stencil.png: a spray-painted ring-and-chevron logo (white; tint it).
- decal_arrow.png: a painted direction arrow with a worn edge.
- decal_puddle.png: a dark, soft-edged puddle; pair it with a low roughness
  override to read as wet.
- decal_plate.png / decal_plate_normal.png: a riveted metal plate with beveled
  edges and a seam, for normal and metallic overrides.
"""

import math
import os
import struct
import sys
import zlib

SIZE = 256


def write_png(path, pixels, size=SIZE):
    """pixels: rows of (r, g, b, a) tuples with components in [0, 1]."""
    raw = bytearray()
    for row in pixels:
        raw.append(0)
        for r, g, b, a in row:
            for value in (r, g, b, a):
                raw.append(max(0, min(255, int(round(value * 255.0)))))

    def chunk(kind, data):
        payload = kind + data
        return struct.pack(">I", len(data)) + payload + struct.pack(">I", zlib.crc32(payload) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    with open(path, "wb") as output:
        output.write(b"\x89PNG\r\n\x1a\n")
        output.write(chunk(b"IHDR", header))
        output.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        output.write(chunk(b"IEND", b""))


def hash2(x, y, seed):
    value = math.sin(x * 127.1 + y * 311.7 + seed * 74.7) * 43758.5453
    return value - math.floor(value)


def value_noise(x, y, seed):
    x0, y0 = math.floor(x), math.floor(y)
    fx, fy = x - x0, y - y0
    sx, sy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
    a = hash2(x0, y0, seed)
    b = hash2(x0 + 1, y0, seed)
    c = hash2(x0, y0 + 1, seed)
    d = hash2(x0 + 1, y0 + 1, seed)
    return (a * (1 - sx) + b * sx) * (1 - sy) + (c * (1 - sx) + d * sx) * sy


def fbm(x, y, seed, octaves=4):
    total, amplitude, frequency, norm = 0.0, 0.5, 1.0, 0.0
    for octave in range(octaves):
        total += value_noise(x * frequency, y * frequency, seed + octave * 13) * amplitude
        norm += amplitude
        amplitude *= 0.5
        frequency *= 2.0
    return total / norm


def smoothstep(edge0, edge1, value):
    t = max(0.0, min(1.0, (value - edge0) / (edge1 - edge0)))
    return t * t * (3 - 2 * t)


def coordinates():
    """Yields (row, column, u, v) with u, v in [-1, 1] and v down the image."""
    for row in range(SIZE):
        for column in range(SIZE):
            yield row, column, (column + 0.5) / SIZE * 2 - 1, (row + 0.5) / SIZE * 2 - 1


def image(shader):
    pixels = [[None] * SIZE for _ in range(SIZE)]
    for row, column, u, v in coordinates():
        pixels[row][column] = shader(u, v)
    return pixels


def segment_distance(px, py, ax, ay, bx, by):
    abx, aby = bx - ax, by - ay
    t = max(0.0, min(1.0, ((px - ax) * abx + (py - ay) * aby) / (abx * abx + aby * aby)))
    dx, dy = px - (ax + abx * t), py - (ay + aby * t)
    return math.hypot(dx, dy)


def stencil(u, v):
    radius = math.hypot(u, v)
    ring = 1.0 - smoothstep(0.06, 0.08, abs(radius - 0.74))
    # An upward chevron and a bar: the logo reads upright with v down the image.
    chevron = min(segment_distance(u, v, -0.42, 0.12, 0.0, -0.36), segment_distance(u, v, 0.0, -0.36, 0.42, 0.12))
    bar = segment_distance(u, v, -0.38, 0.38, 0.38, 0.38)
    shape = max(ring, 1.0 - smoothstep(0.08, 0.1, chevron), 1.0 - smoothstep(0.06, 0.08, bar))
    # Spray-paint overspray and speckled wear.
    speckle = smoothstep(0.25, 0.45, fbm(u * 18 + 3, v * 18 + 7, 1))
    alpha = shape * (0.55 + 0.45 * speckle) * (1.0 - smoothstep(0.88, 0.98, radius))
    return (0.92, 0.92, 0.9, alpha * 0.95)


def arrow(u, v):
    # Points toward the top of the image (decal-space -Z).
    shaft = 1.0 - smoothstep(0.0, 0.02, max(abs(u) - 0.16, v - 0.8, -0.12 - v))
    # Triangle head: apex at v = -0.85, base of half width 0.55 at v = -0.1.
    head_side = (abs(u) - (v + 0.85) / 0.75 * 0.55) * 0.8
    head = 1.0 - smoothstep(0.0, 0.02, max(head_side, v + 0.1, -0.85 - v))
    shape = max(shaft, head)
    wear = smoothstep(0.3, 0.5, fbm(u * 10 + 11, v * 10 + 5, 2))
    return (0.95, 0.78, 0.18, shape * (0.65 + 0.35 * wear))


def puddle(u, v):
    distortion = fbm(u * 2.5 + 1, v * 2.5 + 9, 3) - 0.5
    radius = math.hypot(u * 1.0, v * 1.35) + distortion * 0.55
    alpha = 1.0 - smoothstep(0.55, 0.8, radius)
    depth = 1.0 - smoothstep(0.0, 0.7, radius)
    tone = 0.10 + 0.05 * (1.0 - depth)
    return (tone * 0.9, tone * 0.95, tone, alpha * 0.85)


PLATE_HALF = 0.82
BEVEL = 0.08
RIVETS = [(-0.62, -0.62), (0.62, -0.62), (-0.62, 0.62), (0.62, 0.62)]


def rounded_box_distance(u, v, half, corner):
    qx, qy = abs(u) - half + corner, abs(v) - half + corner
    return math.hypot(max(qx, 0.0), max(qy, 0.0)) + min(max(qx, qy), 0.0) - corner


def plate_height(u, v):
    edge = -rounded_box_distance(u, v, PLATE_HALF, 0.12)
    height = smoothstep(0.0, BEVEL, edge)
    seam = abs(u)
    height -= 0.35 * (1.0 - smoothstep(0.0, 0.025, seam)) * smoothstep(0.0, BEVEL, edge)
    for rx, ry in RIVETS:
        distance = math.hypot(u - rx, v - ry)
        if distance < 0.09:
            height += 0.6 * math.sqrt(max(0.0, 1.0 - (distance / 0.09) ** 2))
    return height


def plate(u, v):
    alpha = 1.0 - smoothstep(-0.01, 0.01, rounded_box_distance(u, v, PLATE_HALF, 0.12))
    grain = fbm(u * 30 + 2, v * 3 + 4, 4) * 0.08
    tone = 0.55 + grain
    return (tone, tone * 0.98, tone * 0.95, alpha)


def plate_normal_pixels():
    step = 2.0 / SIZE
    pixels = [[None] * SIZE for _ in range(SIZE)]
    strength = 0.12
    for row, column, u, v in coordinates():
        dhdu = (plate_height(u + step, v) - plate_height(u - step, v)) / (2 * step) * strength
        dhdv = (plate_height(u, v + step) - plate_height(u, v - step)) / (2 * step) * strength
        # Texture +Y is up the image, which is decreasing v.
        nx, ny, nz = -dhdu, dhdv, 1.0
        length = math.sqrt(nx * nx + ny * ny + nz * nz)
        pixels[row][column] = (nx / length * 0.5 + 0.5, ny / length * 0.5 + 0.5, nz / length * 0.5 + 0.5, 1.0)
    return pixels


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 1
    directory = sys.argv[1]
    os.makedirs(directory, exist_ok=True)
    outputs = {
        "decal_stencil.png": image(stencil),
        "decal_arrow.png": image(arrow),
        "decal_puddle.png": image(puddle),
        "decal_plate.png": image(plate),
        "decal_plate_normal.png": plate_normal_pixels(),
    }
    for name, pixels in outputs.items():
        write_png(os.path.join(directory, name), pixels)
        print("wrote", os.path.join(directory, name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
