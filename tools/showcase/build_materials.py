"""Builds the KHR material extension gallery without Blender (Python 3 standard library only).

Usage:
    python tools/showcase/build_materials.py --repo <zenith checkout>

Writes assets/showcase/materials/material_gallery.gltf (+ .bin and a checker PNG): one
mesh whose primitives are spheres, each with its own material, standing in front of a
checkered backdrop that makes transmission and refraction readable.

Front row (y = 1.3 m), left to right:
  * red and blue car paint: metallic flake base under a smooth KHR_materials_clearcoat
  * velvet and linen: rough dark bases under KHR_materials_sheen
  * clear, frosted and tinted glass: KHR_materials_transmission (+ ior); the tinted
    sphere adds KHR_materials_volume thickness and attenuation
Back row (y = 0.5 m):
  * KHR_materials_specular: a copper-tinted dielectric and a sphere with no specular
  * KHR_materials_ior ladder on a black dielectric: 1.0, 1.25, 1.5, 1.8, 2.4, 3.0

The model is not placed in any scene; add an entity with a mesh_renderer component for
path "showcase/materials/material_gallery.gltf", mesh 0. All coordinates are glTF
(Y-up, metres) with the origin on the floor at the gallery's front centre.
"""

import argparse
import json
import math
import os
import struct
import zlib

GALLERY_NAME = "material_gallery.gltf"
GALLERY_PATH = "showcase/materials/" + GALLERY_NAME
CHECKER_NAME = "gallery_checker.png"

SPHERE_RADIUS = 0.32
SPHERE_SPACING = 0.8
FRONT_ROW_HEIGHT = 1.3
BACK_ROW_HEIGHT = 0.5
SPHERE_RINGS = 20
SPHERE_COLUMNS = 40
# Backdrop: a thin panel behind both rows.
BACKDROP_SIZE = (6.8, 2.0, 0.04)
BACKDROP_CENTRE = (0.0, 1.0, -0.7)

FLOAT = 5126
UNSIGNED_SHORT = 5123
ARRAY_BUFFER = 34962
ELEMENT_ARRAY_BUFFER = 34963

EXTENSIONS = ["KHR_materials_clearcoat", "KHR_materials_sheen", "KHR_materials_transmission",
              "KHR_materials_volume", "KHR_materials_ior", "KHR_materials_specular"]


def pbr(base_color, metallic, roughness):
    return {"baseColorFactor": list(base_color) + [1.0], "metallicFactor": metallic, "roughnessFactor": roughness}


def material(name, base_color, metallic, roughness, **extensions):
    entry = {"name": name, "pbrMetallicRoughness": pbr(base_color, metallic, roughness)}
    if extensions:
        entry["extensions"] = {"KHR_materials_" + key: value for key, value in extensions.items()}
    return entry


FRONT_ROW = [
    material("CarPaintRed", (0.55, 0.02, 0.03), 0.8, 0.45,
             clearcoat={"clearcoatFactor": 1.0, "clearcoatRoughnessFactor": 0.03}),
    material("CarPaintBlue", (0.02, 0.12, 0.45), 0.7, 0.55,
             clearcoat={"clearcoatFactor": 1.0, "clearcoatRoughnessFactor": 0.12}),
    material("Velvet", (0.02, 0.004, 0.04), 0.0, 0.9,
             sheen={"sheenColorFactor": [1.0, 0.4, 0.85], "sheenRoughnessFactor": 0.3}),
    material("Linen", (0.35, 0.30, 0.20), 0.0, 0.85,
             sheen={"sheenColorFactor": [0.7, 0.7, 0.65], "sheenRoughnessFactor": 0.75}),
    material("ClearGlass", (1.0, 1.0, 1.0), 0.0, 0.0,
             transmission={"transmissionFactor": 1.0}, ior={"ior": 1.5}),
    material("FrostedGlass", (0.95, 0.97, 1.0), 0.0, 0.3,
             transmission={"transmissionFactor": 1.0}, ior={"ior": 1.5}),
    material("TintedGlass", (1.0, 1.0, 1.0), 0.0, 0.02,
             transmission={"transmissionFactor": 1.0}, ior={"ior": 1.5},
             volume={"thicknessFactor": 2.0 * SPHERE_RADIUS, "attenuationColor": [0.25, 0.75, 0.45],
                     "attenuationDistance": 0.6}),
]

BACK_ROW = [
    material("CopperSpecular", (0.04, 0.04, 0.04), 0.0, 0.25,
             specular={"specularFactor": 1.0, "specularColorFactor": [4.0, 2.2, 1.2]}),
    material("NoSpecular", (0.5, 0.5, 0.5), 0.0, 0.25, specular={"specularFactor": 0.0}),
] + [
    material("Ior%s" % str(ior).replace(".", "_"), (0.02, 0.02, 0.02), 0.0, 0.12, ior={"ior": ior})
    for ior in (1.0, 1.25, 1.5, 1.8, 2.4, 3.0)
]

BACKDROP_MATERIAL = {"name": "GalleryBackdrop", "pbrMetallicRoughness": {
    "baseColorTexture": {"index": 0}, "metallicFactor": 0.0, "roughnessFactor": 0.8}}


# ---------------------------------------------------------------- glTF writer


class GltfBuilder:
    """Accumulates little-endian buffer views and accessors for one .bin file."""

    def __init__(self):
        self.binary = bytearray()
        self.views = []
        self.accessors = []

    def add(self, fmt, values, component_type, accessor_type, count, target=None, bounds=False):
        while len(self.binary) % 4 != 0:
            self.binary.append(0)
        payload = struct.pack("<" + fmt * len(values), *values)
        view = {"buffer": 0, "byteOffset": len(self.binary), "byteLength": len(payload)}
        if target is not None:
            view["target"] = target
        self.views.append(view)
        self.binary.extend(payload)
        accessor = {"bufferView": len(self.views) - 1, "componentType": component_type, "count": count,
                    "type": accessor_type}
        if bounds:
            width = len(values) // count
            accessor["min"] = [min(values[i::width]) for i in range(width)]
            accessor["max"] = [max(values[i::width]) for i in range(width)]
        self.accessors.append(accessor)
        return len(self.accessors) - 1

    def write(self, directory, name, document):
        bin_name = os.path.splitext(name)[0] + ".bin"
        document["asset"] = {"version": "2.0", "generator": "Zenith tools/showcase/build_materials.py"}
        document["buffers"] = [{"uri": bin_name, "byteLength": len(self.binary)}]
        document["bufferViews"] = self.views
        document["accessors"] = self.accessors
        with open(os.path.join(directory, bin_name), "wb") as file:
            file.write(self.binary)
        with open(os.path.join(directory, name), "w", encoding="utf-8", newline="\n") as file:
            json.dump(document, file, indent=1)
            file.write("\n")


def flatten(rows):
    return [value for row in rows for value in row]


def write_png(path, width, height, rgb_rows):
    raw = b"".join(b"\x00" + bytes(row) for row in rgb_rows)

    def chunk(kind, payload):
        return (struct.pack(">I", len(payload)) + kind + payload +
                struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    with open(path, "wb") as file:
        file.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 9)) +
                   chunk(b"IEND", b""))


def write_checker_texture(path, size=256, cells=8):
    """Warm/cool checkerboard with thin dark grout lines, readable through refraction."""
    warm = (214, 168, 96)
    cool = (60, 92, 140)
    grout = (24, 24, 28)
    cell = size // cells
    rows = []
    for y in range(size):
        row = []
        for x in range(size):
            if x % cell < 2 or y % cell < 2:
                row.extend(grout)
            else:
                row.extend(warm if (x // cell + y // cell) % 2 == 0 else cool)
        rows.append(row)
    write_png(path, size, size, rows)


# ---------------------------------------------------------------- geometry


class Primitives:
    """Gathers vertex streams and per-primitive index ranges for one mesh."""

    def __init__(self):
        self.positions, self.normals, self.tangents, self.uvs = [], [], [], []
        self.primitives = []

    def add(self, positions, normals, tangents, uvs, indices, material_index):
        base = len(self.positions)
        self.positions.extend(positions)
        self.normals.extend(normals)
        self.tangents.extend(tangents)
        self.uvs.extend(uvs)
        self.primitives.append(([base + index for index in indices], material_index))


def sphere(centre):
    positions, normals, tangents, uvs, indices = [], [], [], [], []
    for ring in range(SPHERE_RINGS + 1):
        polar = math.pi * ring / SPHERE_RINGS
        for column in range(SPHERE_COLUMNS + 1):
            azimuth = 2.0 * math.pi * column / SPHERE_COLUMNS
            normal = (math.sin(polar) * math.cos(azimuth), math.cos(polar), -math.sin(polar) * math.sin(azimuth))
            positions.append(tuple(c + SPHERE_RADIUS * n for c, n in zip(centre, normal)))
            normals.append(normal)
            # Tangent follows increasing u (azimuth); v decreases upwards, the
            # direction cross(N, T) points, as build_coverage.py's tentacle (w = 1).
            tangents.append((-math.sin(azimuth), 0.0, -math.cos(azimuth), 1.0))
            uvs.append((column / SPHERE_COLUMNS, ring / SPHERE_RINGS))
    stride = SPHERE_COLUMNS + 1
    for ring in range(SPHERE_RINGS):
        for column in range(SPHERE_COLUMNS):
            a = ring * stride + column
            b = a + stride
            if ring > 0:
                indices.extend((a, b, a + 1))
            if ring < SPHERE_RINGS - 1:
                indices.extend((a + 1, b, b + 1))
    return positions, normals, tangents, uvs, indices


def backdrop():
    """Front face of the backdrop panel only; it is viewed from +z."""
    hx, hy, hz = (0.5 * extent for extent in BACKDROP_SIZE)
    cx, cy, cz = BACKDROP_CENTRE
    z = cz + hz
    positions = [(cx - hx, cy - hy, z), (cx + hx, cy - hy, z), (cx + hx, cy + hy, z), (cx - hx, cy + hy, z)]
    normals = [(0.0, 0.0, 1.0)] * 4
    tangents = [(1.0, 0.0, 0.0, 1.0)] * 4
    # Square checker cells: u spans the width in 0.35 m cells, v the height.
    repeat_u = BACKDROP_SIZE[0] / (8 * 0.35)
    repeat_v = BACKDROP_SIZE[1] / (8 * 0.35)
    uvs = [(0.0, repeat_v), (repeat_u, repeat_v), (repeat_u, 0.0), (0.0, 0.0)]
    return positions, normals, tangents, uvs, [0, 1, 2, 0, 2, 3]


def row_centres(count, height):
    first = -0.5 * SPHERE_SPACING * (count - 1)
    return [(first + SPHERE_SPACING * index, height, 0.0) for index in range(count)]


def build_gallery(directory):
    materials = FRONT_ROW + BACK_ROW + [BACKDROP_MATERIAL]
    geometry = Primitives()
    for index, centre in enumerate(row_centres(len(FRONT_ROW), FRONT_ROW_HEIGHT)):
        geometry.add(*sphere(centre), index)
    for index, centre in enumerate(row_centres(len(BACK_ROW), BACK_ROW_HEIGHT)):
        geometry.add(*sphere(centre), len(FRONT_ROW) + index)
    geometry.add(*backdrop(), len(materials) - 1)

    builder = GltfBuilder()
    count = len(geometry.positions)
    attributes = {
        "POSITION": builder.add("f", flatten(geometry.positions), FLOAT, "VEC3", count, ARRAY_BUFFER, bounds=True),
        "NORMAL": builder.add("f", flatten(geometry.normals), FLOAT, "VEC3", count, ARRAY_BUFFER),
        "TANGENT": builder.add("f", flatten(geometry.tangents), FLOAT, "VEC4", count, ARRAY_BUFFER),
        "TEXCOORD_0": builder.add("f", flatten(geometry.uvs), FLOAT, "VEC2", count, ARRAY_BUFFER),
    }
    primitives = []
    for indices, material_index in geometry.primitives:
        accessor = builder.add("H", indices, UNSIGNED_SHORT, "SCALAR", len(indices), ELEMENT_ARRAY_BUFFER)
        primitives.append({"attributes": attributes, "indices": accessor, "material": material_index})
    document = {
        "extensionsUsed": EXTENSIONS,
        "scene": 0,
        "scenes": [{"name": "MaterialGallery", "nodes": [0]}],
        "nodes": [{"name": "MaterialGallery", "mesh": 0}],
        "meshes": [{"name": "MaterialGallery", "primitives": primitives}],
        "materials": materials,
        "textures": [{"source": 0, "sampler": 0}],
        "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}],
        "images": [{"uri": CHECKER_NAME, "mimeType": "image/png"}],
    }
    write_checker_texture(os.path.join(directory, CHECKER_NAME))
    builder.write(directory, GALLERY_NAME, document)
    return document


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", default=os.path.join(os.path.dirname(__file__), "..", ".."),
                        help="Zenith checkout root")
    arguments = parser.parse_args()
    directory = os.path.join(arguments.repo, "assets", "showcase", "materials")
    os.makedirs(directory, exist_ok=True)
    document = build_gallery(directory)
    print("Wrote %s with %d materials" % (os.path.join(directory, GALLERY_NAME), len(document["materials"])))


if __name__ == "__main__":
    main()
