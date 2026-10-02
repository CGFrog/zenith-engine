"""Builds the showcase coverage content without Blender (Python 3 standard library only).

Usage:
    python tools/showcase/build_coverage.py --repo <zenith checkout>

Writes two procedural glTF assets to assets/showcase/coverage/ and replaces their entities in
projects/scenes/showcase.zenith-scene (other entities are left untouched):

* tentacle.gltf - a skinned, tapered tube on a six-joint chain with a looping 4 s
  sway. It exercises skinned motion vectors (TAA), packed skin vertex streams and
  per-frame joint palettes; diagonal stripes make the motion easy to see.
* gold_mirror.gltf - a mirror-smooth gold slab (metallic 1, roughness 0.03) lying on
  the floor in front of the default player start, so screen-space reflections on a
  tinted metal show the tentacle, pedestals and spheres behind it.

build_showcase.py imports scene_entities() from this module so a full Blender
rebuild keeps the coverage entities. All coordinates are glTF (Y-up, metres).
"""

import argparse
import json
import math
import os
import struct
import zlib

TENTACLE_NAME = "tentacle.gltf"
MIRROR_NAME = "gold_mirror.gltf"
TENTACLE_PATH = "showcase/coverage/" + TENTACLE_NAME
MIRROR_PATH = "showcase/coverage/" + MIRROR_NAME
COVERAGE_PATHS = (TENTACLE_PATH, MIRROR_PATH)

JOINT_COUNT = 6
SEGMENT_LENGTH = 0.3
BASE_RADIUS = 0.16
TIP_RADIUS = 0.05
BODY_RINGS = 60
TIP_RINGS = 8
COLUMNS = 32
CLIP_SECONDS = 4.0
CLIP_KEYS = 81

MIRROR_SIZE = (5.0, 0.02, 1.6)
# Mirror spans z in [-1.5, 0.1]; the tentacle stands on its far edge, in front of
# the middle pedestals (z = -2), roughly 5 m ahead of the player start (0, 1.2, 4).
MIRROR_CENTRE = (0.0, MIRROR_SIZE[1] * 0.5, -0.7)
TENTACLE_ORIGIN = (0.0, MIRROR_SIZE[1], -1.15)

FLOAT = 5126
UNSIGNED_BYTE = 5121
UNSIGNED_SHORT = 5123
ARRAY_BUFFER = 34962
ELEMENT_ARRAY_BUFFER = 34963


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
        payload = struct.pack("<" + fmt * (len(values)), *values)
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
        document["asset"] = {"version": "2.0", "generator": "Zenith tools/showcase/build_coverage.py"}
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


# ---------------------------------------------------------------- textures


def write_png(path, width, height, rgb_rows):
    raw = b"".join(b"\x00" + bytes(row) for row in rgb_rows)

    def chunk(kind, payload):
        return (struct.pack(">I", len(payload)) + kind + payload +
                struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    with open(path, "wb") as file:
        file.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 9)) +
                   chunk(b"IEND", b""))


def write_stripe_texture(path, size=256):
    """Diagonal two-tone bands (4x supersampled) that wrap seamlessly in u."""
    teal = (24, 112, 122)
    cream = (236, 214, 170)
    rows = []
    for y in range(size):
        row = []
        for x in range(size):
            cover = 0.0
            for sy in range(2):
                for sx in range(2):
                    u = (x + 0.25 + 0.5 * sx) / size
                    v = (y + 0.25 + 0.5 * sy) / size
                    cover += 0.25 * (int(math.floor(v * 12.0 + u * 2.0)) % 2)
            row.extend(round(a + (b - a) * cover) for a, b in zip(teal, cream))
        rows.append(row)
    write_png(path, size, size, rows)


# ---------------------------------------------------------------- tentacle


def tentacle_profile():
    """(radius, height) samples from the base ring up a tapered body to a rounded tip."""
    body_height = JOINT_COUNT * SEGMENT_LENGTH - TIP_RADIUS
    profile = []
    for ring in range(BODY_RINGS):
        fraction = ring / (BODY_RINGS - 1)
        profile.append((BASE_RADIUS + (TIP_RADIUS - BASE_RADIUS) * fraction, body_height * fraction))
    for ring in range(1, TIP_RINGS + 1):
        angle = 0.5 * math.pi * ring / TIP_RINGS
        profile.append((TIP_RADIUS * math.cos(angle), body_height + TIP_RADIUS * math.sin(angle)))
    return profile


def profile_normals(profile):
    normals = []
    for index in range(len(profile)):
        before = profile[max(index - 1, 0)]
        after = profile[min(index + 1, len(profile) - 1)]
        dr, dy = after[0] - before[0], after[1] - before[1]
        length = math.hypot(dr, dy)
        normals.append((dy / length, -dr / length))
    return normals


def joint_influence(height):
    """Blends linearly between neighbouring joint centres, so each ring has at most two joints."""
    position = height / SEGMENT_LENGTH - 0.5
    lower = math.floor(position)
    if lower < 0:
        return (0, 0), (1.0, 0.0)
    if lower >= JOINT_COUNT - 1:
        return (JOINT_COUNT - 1, 0), (1.0, 0.0)
    blend = position - lower
    blend = blend * blend * (3.0 - 2.0 * blend)
    return (lower, lower + 1), (1.0 - blend, blend)


def tentacle_geometry():
    profile = tentacle_profile()
    normals = profile_normals(profile)
    arc = [0.0]
    for index in range(1, len(profile)):
        arc.append(arc[-1] + math.dist(profile[index], profile[index - 1]))
    positions, vertex_normals, tangents, uvs, joints, weights = [], [], [], [], [], []
    for ring, ((radius, height), (normal_r, normal_y)) in enumerate(zip(profile, normals)):
        ring_joints, ring_weights = joint_influence(height)
        for column in range(COLUMNS + 1):
            theta = 2.0 * math.pi * column / COLUMNS
            cosine, sine = math.cos(theta), math.sin(theta)
            positions.append((radius * cosine, height, radius * sine))
            vertex_normals.append((normal_r * cosine, normal_y, normal_r * sine))
            # u follows theta and v decreases upwards, so cross(N, T) points along +v (w = 1).
            tangents.append((-sine, 0.0, cosine, 1.0))
            uvs.append((column / COLUMNS, 1.0 - arc[ring] / arc[-1]))
            joints.append((ring_joints[0], ring_joints[1], 0, 0))
            weights.append((ring_weights[0], ring_weights[1], 0.0, 0.0))
    indices = []
    stride = COLUMNS + 1
    for ring in range(len(profile) - 1):
        apex = ring == len(profile) - 2
        for column in range(COLUMNS):
            a = ring * stride + column
            b = a + 1
            d = a + stride
            e = d + 1
            indices.extend((a, d, b))
            if not apex:
                indices.extend((b, d, e))
    return positions, vertex_normals, tangents, uvs, joints, weights, indices


def axis_quaternion(axis, angle):
    half = 0.5 * angle
    s = math.sin(half)
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(half))


def multiply(first, second):
    x1, y1, z1, w1 = first
    x2, y2, z2, w2 = second
    return (w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2,
            w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2,
            w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2,
            w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2)


def joint_rotation(joint, time):
    """Travelling-wave sway; every term has an integer frequency over the clip, so it loops seamlessly."""
    phase = 2.0 * math.pi * time / CLIP_SECONDS
    # A gentler base bend keeps the base ring close to the surface it stands on.
    amplitude = 0.4 if joint == 0 else 1.0
    bend_z = 0.30 * amplitude * math.sin(phase - 0.7 * joint)
    bend_x = 0.22 * amplitude * math.sin(2.0 * phase - 0.9 * joint + 0.6)
    rotation = multiply(axis_quaternion((0.0, 0.0, 1.0), bend_z), axis_quaternion((1.0, 0.0, 0.0), bend_x))
    if joint == 0:
        rotation = multiply(axis_quaternion((0.0, 1.0, 0.0), 0.6 * math.sin(phase)), rotation)
    return rotation


def build_tentacle(directory):
    builder = GltfBuilder()
    positions, normals, tangents, uvs, joints, weights, indices = tentacle_geometry()
    count = len(positions)
    attributes = {
        "POSITION": builder.add("f", flatten(positions), FLOAT, "VEC3", count, ARRAY_BUFFER, bounds=True),
        "NORMAL": builder.add("f", flatten(normals), FLOAT, "VEC3", count, ARRAY_BUFFER),
        "TANGENT": builder.add("f", flatten(tangents), FLOAT, "VEC4", count, ARRAY_BUFFER),
        "TEXCOORD_0": builder.add("f", flatten(uvs), FLOAT, "VEC2", count, ARRAY_BUFFER),
        "JOINTS_0": builder.add("B", flatten(joints), UNSIGNED_BYTE, "VEC4", count, ARRAY_BUFFER),
        "WEIGHTS_0": builder.add("f", flatten(weights), FLOAT, "VEC4", count, ARRAY_BUFFER),
    }
    index_accessor = builder.add("H", indices, UNSIGNED_SHORT, "SCALAR", len(indices), ELEMENT_ARRAY_BUFFER)
    inverse_binds = []
    for joint in range(JOINT_COUNT):
        inverse_binds.extend((1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -joint * SEGMENT_LENGTH, 0, 1))
    inverse_bind_accessor = builder.add("f", inverse_binds, FLOAT, "MAT4", JOINT_COUNT)

    times = [CLIP_SECONDS * key / (CLIP_KEYS - 1) for key in range(CLIP_KEYS)]
    time_accessor = builder.add("f", times, FLOAT, "SCALAR", CLIP_KEYS, bounds=True)
    samplers, channels = [], []
    for joint in range(JOINT_COUNT):
        rotations = [joint_rotation(joint, time) for time in times]
        output = builder.add("f", flatten(rotations), FLOAT, "VEC4", CLIP_KEYS)
        samplers.append({"input": time_accessor, "output": output, "interpolation": "LINEAR"})
        channels.append({"sampler": joint, "target": {"node": joint + 1, "path": "rotation"}})

    nodes = [{"name": "TentacleMesh", "mesh": 0, "skin": 0}]
    for joint in range(JOINT_COUNT):
        node = {"name": "Joint%d" % joint}
        if joint > 0:
            node["translation"] = [0.0, SEGMENT_LENGTH, 0.0]
        if joint + 1 < JOINT_COUNT:
            node["children"] = [joint + 2]
        nodes.append(node)
    document = {
        "scene": 0,
        "scenes": [{"name": "Tentacle", "nodes": [0, 1]}],
        "nodes": nodes,
        "skins": [{"name": "TentacleSkin", "joints": list(range(1, JOINT_COUNT + 1)), "skeleton": 1,
                   "inverseBindMatrices": inverse_bind_accessor}],
        "animations": [{"name": "Sway", "samplers": samplers, "channels": channels}],
        "meshes": [{"name": "Tentacle",
                    "primitives": [{"attributes": attributes, "indices": index_accessor, "material": 0}]}],
        "materials": [{"name": "TentacleStripes", "doubleSided": False,
                       "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicFactor": 0.0,
                                                "roughnessFactor": 0.4}}],
        "textures": [{"source": 0, "sampler": 0}],
        "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}],
        "images": [{"uri": "tentacle_stripes.png", "mimeType": "image/png"}],
    }
    write_stripe_texture(os.path.join(directory, "tentacle_stripes.png"))
    builder.write(directory, TENTACLE_NAME, document)
    return document


# ---------------------------------------------------------------- mirror


def box_faces(half):
    """Per-face (normal, tangent, corner positions) with counter-clockwise outward winding."""
    hx, hy, hz = half
    faces = []
    for axis in range(3):
        for sign in (1.0, -1.0):
            normal = [0.0, 0.0, 0.0]
            normal[axis] = sign
            u_axis = (axis + 1) % 3 if axis != 1 else 0
            v_axis = 3 - axis - u_axis
            u = [0.0, 0.0, 0.0]
            u[u_axis] = 1.0
            v = [0.0, 0.0, 0.0]
            v[v_axis] = 1.0
            # Choose v so that cross(u, v) is the outward normal.
            cross = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
            if sum(c * n for c, n in zip(cross, normal)) < 0.0:
                v = [-c for c in v]
            corners = []
            for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
                corners.append(tuple((normal[i] + su * u[i] + sv * v[i]) * (hx, hy, hz)[i] for i in range(3)))
            faces.append((tuple(normal), tuple(u), corners))
    return faces


def build_mirror(directory):
    builder = GltfBuilder()
    half = tuple(0.5 * extent for extent in MIRROR_SIZE)
    positions, normals, tangents, uvs, indices = [], [], [], [], []
    for normal, tangent, corners in box_faces(half):
        base = len(positions)
        positions.extend(corners)
        normals.extend([normal] * 4)
        # v runs against the face's v axis in texture space, so the bitangent sign is -1.
        tangents.extend([tangent + (-1.0,)] * 4)
        uvs.extend([(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)])
        indices.extend((base, base + 1, base + 2, base, base + 2, base + 3))
    count = len(positions)
    attributes = {
        "POSITION": builder.add("f", flatten(positions), FLOAT, "VEC3", count, ARRAY_BUFFER, bounds=True),
        "NORMAL": builder.add("f", flatten(normals), FLOAT, "VEC3", count, ARRAY_BUFFER),
        "TANGENT": builder.add("f", flatten(tangents), FLOAT, "VEC4", count, ARRAY_BUFFER),
        "TEXCOORD_0": builder.add("f", flatten(uvs), FLOAT, "VEC2", count, ARRAY_BUFFER),
    }
    index_accessor = builder.add("H", indices, UNSIGNED_SHORT, "SCALAR", len(indices), ELEMENT_ARRAY_BUFFER)
    document = {
        "scene": 0,
        "scenes": [{"name": "GoldMirror", "nodes": [0]}],
        "nodes": [{"name": "GoldMirror", "mesh": 0}],
        "meshes": [{"name": "GoldMirror",
                    "primitives": [{"attributes": attributes, "indices": index_accessor, "material": 0}]}],
        "materials": [{"name": "MirrorGold", "doubleSided": False,
                       "pbrMetallicRoughness": {"baseColorFactor": [1.0, 0.766, 0.336, 1.0],
                                                "metallicFactor": 1.0, "roughnessFactor": 0.03}}],
    }
    builder.write(directory, MIRROR_NAME, document)
    return document


# ---------------------------------------------------------------- scene document


def translation(offset):
    return [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, offset[0], offset[1], offset[2], 1.0]


def references_coverage(entity):
    return any(isinstance(component, dict) and component.get("path") in COVERAGE_PATHS
               for component in entity.get("components", {}).values())


def tentacle_entities(first_id, name_prefix, origin, speed):
    """Scene-document entities for one tentacle instance, with consecutive ids from first_id.

    The tentacle is serialized as an instantiated model: a root with the model and a
    looping animation, one model_node entity per glTF node, and the skin and mesh on
    the mesh node. Node transforms match the glTF rest pose. name_prefix keeps the
    entity names of several instances distinct.
    """
    entities = []

    def add(name, matrix, parent, components):
        entity_id = first_id + len(entities)
        entities.append({"id": entity_id, "name": name_prefix + name, "local_transform": matrix, "parent": parent,
                         "components": components})
        return entity_id

    root = add("Skinned Tentacle", translation(origin), None, {
        "model": {"path": TENTACLE_PATH},
        "animation": {"clip": 0, "time": 0.0, "speed": speed, "looping": True, "playing": True, "blend_clip": None,
                      "blend_time": 0.0, "blend_duration": 0.0}})
    add("TentacleMesh", translation((0.0, 0.0, 0.0)), root, {
        "model_node": {"path": TENTACLE_PATH, "instance_root": root, "node": 0},
        "skin": {"path": TENTACLE_PATH, "instance_root": root, "skin": 0},
        "mesh_renderer": {"path": TENTACLE_PATH, "mesh": 0}})
    parent = root
    for joint in range(JOINT_COUNT):
        offset = (0.0, SEGMENT_LENGTH if joint > 0 else 0.0, 0.0)
        parent = add("Joint%d" % joint, translation(offset), parent, {
            "model_node": {"path": TENTACLE_PATH, "instance_root": root, "node": joint + 1}})
    return entities


def scene_entities(first_id):
    """Scene-document entities for the coverage assets, with consecutive ids from first_id."""
    entities = tentacle_entities(first_id, "", TENTACLE_ORIGIN, 1.0)
    entities.append({"id": first_id + len(entities), "name": "Gold Mirror", "local_transform": translation(MIRROR_CENTRE),
                     "parent": None, "components": {"mesh_renderer": {"path": MIRROR_PATH, "mesh": 0}}})
    return entities


def update_scene(scene_path):
    with open(scene_path, encoding="utf-8") as file:
        document = json.load(file)
    kept = [entity for entity in document["entities"] if not references_coverage(entity)]
    next_id = max((entity["id"] for entity in kept), default=0) + 1
    document["entities"] = kept + scene_entities(next_id)
    with open(scene_path, "w", encoding="utf-8", newline="\n") as file:
        json.dump(document, file, indent=1)
        file.write("\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", required=True)
    arguments = parser.parse_args()
    output = os.path.join(arguments.repo, "assets", "showcase", "coverage")
    os.makedirs(output, exist_ok=True)
    build_tentacle(output)
    build_mirror(output)
    update_scene(os.path.join(arguments.repo, "projects", "scenes", "showcase.zenith-scene"))
    print("coverage content written to", output)


if __name__ == "__main__":
    main()
