"""Builds the irradiance-volume test interior.

Usage:
    python tools/irradiance_room/build_irradiance_room.py --repo <zenith checkout>

Writes assets/irradiance_room/room.gltf, projects/irradiance_room.zenith-project
and projects/scenes/irradiance_room.zenith-scene. The scene's irradiance volume
is left unbaked; bake it with

    zenith_editor projects/irradiance_room.zenith-project --bake-irradiance-volumes

which writes assets/baked/irradiance_room/irradiance_volume_<id>.zvol and saves
the reference into the scene. Only the Python standard library is needed.
"""

import argparse
import base64
import json
import math
import os
import struct

# Room interior: x in [-4, 4], y in [0, 3.5], z in [-5, 5]; walls 0.2 thick.
# The +z wall has a window the sun shines through. The game spawns its player
# at (0, 1.2, 4) looking toward -z, so the default view faces the red (-x),
# green (+x) and white back walls with the sunlit floor in front.
WALL = 0.2
WINDOW_X = 1.6
WINDOW_BOTTOM = 0.9
WINDOW_TOP = 2.7

MATERIALS = {
    "floor": (0.60, 0.45, 0.30),
    "white": (0.80, 0.80, 0.78),
    "red": (0.75, 0.06, 0.05),
    "green": (0.08, 0.55, 0.10),
    "blue": (0.15, 0.25, 0.70),
}

# name -> (material, [(minimum corner, maximum corner), ...])
MESHES = [
    ("Floor", "floor", [((-4.2, -0.2, -5.2), (4.2, 0.0, 5.2))]),
    ("Ceiling", "white", [((-4.2, 3.5, -5.2), (4.2, 3.7, 5.2))]),
    ("WallLeft", "red", [((-4.2, 0.0, -5.0), (-4.0, 3.5, 5.0))]),
    ("WallRight", "green", [((4.0, 0.0, -5.0), (4.2, 3.5, 5.0))]),
    ("WallBack", "white", [((-4.0, 0.0, -5.2), (4.0, 3.5, -5.0))]),
    ("WallFront", "white", [
        ((-4.0, 0.0, 5.0), (-WINDOW_X, 3.5, 5.2)),
        ((WINDOW_X, 0.0, 5.0), (4.0, 3.5, 5.2)),
        ((-WINDOW_X, 0.0, 5.0), (WINDOW_X, WINDOW_BOTTOM, 5.2)),
        ((-WINDOW_X, WINDOW_TOP, 5.0), (WINDOW_X, 3.5, 5.2)),
    ]),
    ("Block", "white", [((-2.4, 0.0, -3.2), (-1.0, 1.0, -1.8))]),
    ("Column", "blue", [((1.6, 0.0, -3.4), (2.4, 2.2, -2.6))]),
]

# (outward normal, two in-face axes ordered so u x v = normal)
FACES = [
    ((1, 0, 0), (0, 0, -1), (0, 1, 0)),
    ((-1, 0, 0), (0, 0, 1), (0, 1, 0)),
    ((0, 1, 0), (1, 0, 0), (0, 0, -1)),
    ((0, -1, 0), (1, 0, 0), (0, 0, 1)),
    ((0, 0, 1), (1, 0, 0), (0, 1, 0)),
    ((0, 0, -1), (-1, 0, 0), (0, 1, 0)),
]


def box_faces(minimum, maximum):
    center = [(a + b) * 0.5 for a, b in zip(minimum, maximum)]
    half = [(b - a) * 0.5 for a, b in zip(minimum, maximum)]
    positions, normals, uvs, indices = [], [], [], []
    for normal, u, v in FACES:
        base = len(positions)
        face_center = [c + n * h for c, n, h in zip(center, normal, half)]
        extent_u = [a * h for a, h in zip(u, half)]
        extent_v = [a * h for a, h in zip(v, half)]
        size_u = 2.0 * sum(abs(a) * h for a, h in zip(u, half))
        size_v = 2.0 * sum(abs(a) * h for a, h in zip(v, half))
        for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            positions.append([c + su * eu + sv * ev for c, eu, ev in zip(face_center, extent_u, extent_v)])
            normals.append(list(normal))
            uvs.append([(su + 1) * 0.5 * size_u, (1 - sv) * 0.5 * size_v])
        indices += [base, base + 1, base + 2, base, base + 2, base + 3]
    return positions, normals, uvs, indices


def build_gltf():
    binary = bytearray()
    buffer_views, accessors, meshes = [], [], []

    def add_view(data, target):
        while len(binary) % 4:
            binary.append(0)
        buffer_views.append({"buffer": 0, "byteOffset": len(binary), "byteLength": len(data), "target": target})
        binary.extend(data)
        return len(buffer_views) - 1

    def add_accessor(values, kind, component, target, with_bounds=False):
        flat = [x for value in values for x in (value if isinstance(value, list) else [value])]
        pack = "<%d%s" % (len(flat), "f" if component == 5126 else "I")
        accessor = {"bufferView": add_view(struct.pack(pack, *flat), target), "componentType": component,
                    "count": len(values), "type": kind}
        if with_bounds:
            accessor["min"] = [min(v[i] for v in values) for i in range(3)]
            accessor["max"] = [max(v[i] for v in values) for i in range(3)]
        accessors.append(accessor)
        return len(accessors) - 1

    material_names = list(MATERIALS)
    for name, material, boxes in MESHES:
        positions, normals, uvs, indices = [], [], [], []
        for minimum, maximum in boxes:
            p, n, t, i = box_faces(minimum, maximum)
            offset = len(positions)
            positions += p
            normals += n
            uvs += t
            indices += [index + offset for index in i]
        meshes.append({"name": name, "primitives": [{
            "attributes": {
                "POSITION": add_accessor(positions, "VEC3", 5126, 34962, True),
                "NORMAL": add_accessor(normals, "VEC3", 5126, 34962),
                "TEXCOORD_0": add_accessor(uvs, "VEC2", 5126, 34962),
            },
            "indices": add_accessor(indices, "SCALAR", 5125, 34963),
            "material": material_names.index(material),
        }]})

    materials = [{"name": name, "pbrMetallicRoughness": {
        "baseColorFactor": [*color, 1.0], "metallicFactor": 0.0, "roughnessFactor": 0.9}}
        for name, color in MATERIALS.items()]
    return {
        "asset": {"version": "2.0", "generator": "Zenith irradiance room builder"},
        "scene": 0,
        "scenes": [{"nodes": list(range(len(meshes)))}],
        "nodes": [{"name": mesh["name"], "mesh": index} for index, mesh in enumerate(meshes)],
        "meshes": meshes,
        "materials": materials,
        "buffers": [{"byteLength": len(binary),
                     "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(binary)).decode()}],
        "bufferViews": buffer_views,
        "accessors": accessors,
    }


def translation(x, y, z):
    return [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, x, y, z, 1.0]


def normalized(vector):
    length = math.sqrt(sum(v * v for v in vector))
    return [v / length for v in vector]


def build_scene():
    entities = []

    def entity(name, components, transform=None):
        entities.append({"id": len(entities) + 1, "name": name, "local_transform": transform or translation(0, 0, 0),
                         "parent": None, "components": components})

    entity("Sun", {"directional_light": {
        "direction": normalized([0.25, 0.35, 0.9]), "color": [1.0, 0.92, 0.8], "intensity": 6.0, "primary": True,
        "shadows": {"enabled": True, "half_extent": 9.0, "focus_distance": 4.0, "depth_range": 40.0,
                    "constant_bias": 0.5, "slope_bias": 1.5, "normal_bias": 0.02}}})
    # No environment map: the procedural sky lights the window and the bake.
    entity("Environment", {"environment_light": {
        "sky_color": [0.3, 0.42, 0.65], "ground_color": [0.12, 0.1, 0.08], "diffuse_intensity": 1.0,
        "specular_intensity": 0.4, "primary": True, "environment_asset": ""}})
    for index, (name, _, _) in enumerate(MESHES):
        entity(name, {"mesh_renderer": {"path": "irradiance_room/room.gltf", "mesh": index}})
    entity("FloorCollider", {
        "collider": {"type": "box", "half_extents": [4.2, 0.1, 5.2]},
        "rigid_body": {"motion_type": 0, "layer": 0, "mass": 1.0, "friction": 0.8, "restitution": 0.0,
                       "linear_damping": 0.0, "angular_damping": 0.0, "gravity_factor": 1.0, "sensor": False,
                       "allow_sleeping": True, "continuous_collision": False, "lock_rotation": False}},
        translation(0.0, -0.1, 0.0))
    # Covers the room including the wall thickness, so wall surfaces sit inside
    # the edge blend and the volume fades out only beyond them.
    entity("IrradianceVolume", {"irradiance_volume": {
        "half_extents": [4.2, 1.95, 5.2], "probe_counts": [8, 4, 10], "intensity": 1.0, "blend_distance": 0.2,
        "normal_bias": 0.15, "view_bias": 0.15, "bake_asset": ""}}, translation(0.0, 1.75, 0.0))
    # Dense, uniform fog so the sun through the window draws visible shafts
    # against the shadowed interior (the volumetric fog reference view).
    entity("Fog", {"volumetric_fog": {
        "density": 0.06, "height_falloff": 0.0, "base_height": 0.0, "albedo": [0.9, 0.9, 0.88],
        "anisotropy": 0.55, "noise_amount": 0.25, "noise_scale": 2.0, "primary": True}})
    return {"version": 1, "entities": entities}


PROJECT = {
    "version": 3,
    "displayName": "Zenith Irradiance Room",
    "assetRoot": "../assets",
    "sceneRoot": "scenes",
    "startupScene": "irradiance_room.zenith-scene",
    "renderSettings": {
        "version": 1, "antialiasing": 2, "shadows": 3, "lighting": 1, "bloom": 1, "tonemap": 1, "environment": 2,
        "environmentBackground": True, "localLights": 1, "maximumLocalLights": 1024, "reflectionProbes": True,
        "indirectDiffuse": 1, "preferGpuDrivenSubmission": False, "depthPrepass": True,
        "lodErrorThresholdPixels": 1.0, "cascadeSplitLambda": 0.65, "cascadeBlendFraction": 0.1,
        "visualizeShadowCascades": False, "ambientOcclusion": 2, "reflections": 0, "volumetricFog": 2,
    },
}


def write_json(path, document):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as output:
        json.dump(document, output, indent=1)
        output.write("\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    arguments = parser.parse_args()
    write_json(os.path.join(arguments.repo, "assets", "irradiance_room", "room.gltf"), build_gltf())
    write_json(os.path.join(arguments.repo, "projects", "scenes", "irradiance_room.zenith-scene"), build_scene())
    write_json(os.path.join(arguments.repo, "projects", "irradiance_room.zenith-project"), PROJECT)


if __name__ == "__main__":
    main()
