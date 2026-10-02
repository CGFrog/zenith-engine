"""Writes the showcase's reflection-probe environments.

Two stylized interiors stand in for baked captures of the showcase room's west
and east halves: a cool room lit by a cyan strip and a warm room lit by an
orange one. They are deliberately distinct so per-pixel probe blending and box
projection are easy to see. Pure Python; no dependencies.

    python tools/showcase/build_probe_environments.py assets/showcase
"""

import math
import os
import sys

WIDTH = 256
HEIGHT = 128

ROOMS = {
    "probe_cool.hdr": {
        "wall": (0.10, 0.22, 0.34),
        "floor": (0.06, 0.07, 0.09),
        "ceiling": (0.16, 0.20, 0.26),
        "strip": (0.6, 4.5, 6.0),
        "panel": (3.0, 3.6, 4.2),
    },
    "probe_warm.hdr": {
        "wall": (0.36, 0.17, 0.07),
        "floor": (0.10, 0.07, 0.05),
        "ceiling": (0.28, 0.20, 0.14),
        "strip": (7.0, 2.6, 0.5),
        "panel": (4.4, 3.4, 2.4),
    },
}


def radiance(room, direction):
    """Radiance of a box room seen from its center along a unit direction (Y up)."""
    x, y, z = direction
    if y < -0.25:
        # Floor checker so reflections show parallax.
        distance = 1.0 / max(-y, 1e-3)
        u, v = x * distance, z * distance
        checker = 0.75 + 0.5 * ((int(math.floor(u)) + int(math.floor(v))) & 1)
        return tuple(channel * checker for channel in room["floor"])
    if y > 0.55:
        distance = 1.0 / y
        u, v = x * distance, z * distance
        if abs(u) < 0.45 and abs(v) < 0.45:
            return room["panel"]
        return room["ceiling"]
    # Walls with a bright horizontal strip and vertical panel seams.
    azimuth = math.atan2(z, x)
    seam = 0.6 if math.cos(azimuth * 8.0) > 0.97 else 1.0
    if 0.05 < y < 0.14:
        return room["strip"]
    return tuple(channel * seam for channel in room["wall"])


def encode_rgbe(color):
    value = max(color)
    if value < 1e-32:
        return (0, 0, 0, 0)
    mantissa, exponent = math.frexp(value)
    scale = mantissa * 256.0 / value
    return tuple(min(255, int(channel * scale)) for channel in color) + (exponent + 128,)


def run_length_scanline(scanline):
    """New-style Radiance scanline: four channel planes of literal runs."""
    encoded = bytearray((2, 2, WIDTH >> 8, WIDTH & 0xFF))
    for channel in range(4):
        plane = [pixel[channel] for pixel in scanline]
        for start in range(0, WIDTH, 128):
            chunk = plane[start:start + 128]
            encoded.append(len(chunk))
            encoded.extend(chunk)
    return encoded


def write_room(path, room):
    data = bytearray(b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n")
    data.extend(("-Y %d +X %d\n" % (HEIGHT, WIDTH)).encode("ascii"))
    for row in range(HEIGHT):
        # Matches env_equirect_to_cube.comp: u = atan(z, x) / 2pi + 0.5, v = acos(y) / pi.
        polar = (row + 0.5) / HEIGHT * math.pi
        scanline = []
        for column in range(WIDTH):
            azimuth = ((column + 0.5) / WIDTH - 0.5) * 2.0 * math.pi
            direction = (math.sin(polar) * math.cos(azimuth), math.cos(polar), math.sin(polar) * math.sin(azimuth))
            scanline.append(encode_rgbe(radiance(room, direction)))
        data.extend(run_length_scanline(scanline))
    with open(path, "wb") as output:
        output.write(data)


def main():
    output = sys.argv[1] if len(sys.argv) > 1 else os.path.join("assets", "showcase")
    for name, room in ROOMS.items():
        write_room(os.path.join(output, name), room)


if __name__ == "__main__":
    main()
