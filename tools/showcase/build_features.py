"""Places the Milestone 24/25 renderer features in the showcase (Python 3 standard library only).

Usage:
    python tools/showcase/build_features.py --repo <zenith checkout>

Replaces the feature entities in projects/scenes/showcase.zenith-scene (other entities are
left untouched) and merges the feature render settings into projects/showcase.zenith-project:

* the KHR material extension gallery (build_materials.py) in front of the back wall;
* five clustered decals: a floor stencil, a direction arrow, a glossy puddle, a
  normal-mapped metal plate on the back wall and a projector box straddling the
  west wall corner (checks that decals do not leak onto perpendicular faces);
* a campfire of two GPU particle emitters in front of the granite rock: additive unlit
  embers and lit, alpha-blended smoke;
* a second instance of the skinned tentacle (build_coverage.py) standing in the puddle and
  swaying 3.5 times faster, so object motion blur is visible with a static camera;
* an irradiance volume over the room for probe-volume indirect diffuse.

The irradiance volume is written with an empty bake_asset the first time; bake it with

    zenith_editor projects/showcase.zenith-project --bake-irradiance-volumes

and rerunning this script keeps the baked asset of an existing volume entity. Rebake after
moving static geometry. build_coverage.py replaces every tentacle instance, so rerun this script
after it. build_showcase.py imports scene_entities() and RENDER_SETTINGS so a
full Blender rebuild keeps this content. All coordinates are metres, Y up; transforms are
column-major like the scene document.
"""

import argparse
import json
import os

import build_coverage  # sibling script

GALLERY_PATH = "showcase/materials/material_gallery.gltf"
DECAL_TEXTURES = "showcase/decals/"
IRRADIANCE_VOLUME_NAME = "Irradiance Volume"

# The gallery's backdrop sits 0.7 m behind its origin, so z -8.0 keeps it clear of the back
# wall (face at z -9.0). x -0.6 keeps its rightmost spheres out from behind the glass pane
# at x 2.8 when seen from the room's centre line.
GALLERY_ORIGIN = (-0.6, 0.0, -8.0)

# Every setting this script owns; other render settings in the project are left alone.
# Chosen so the default overview stays readable: half-resolution GTAO, a mild depth of
# field focused on the pedestals, Low motion blur (static views stay sharp; only the
# animated tentacle and camera motion blur), and a gentle warm grade.
RENDER_SETTINGS = {
    "ambientOcclusion": 1,
    "indirectDiffuse": 1,
    "decals": 1,
    "particles": 2,
    "motionBlur": 1,
    "motionBlurShutterFraction": 0.5,
    "depthOfField": {
        "quality": 2,
        "focusDistance": 6.0,
        "fStop": 1.4,
        "sensorHeightMillimeters": 36.0,
        "maximumCocRadiusPixels": 12.0,
    },
    "colorGrade": {
        "colorLut": {"enabled": True, "path": "color_grading/warm_cinematic.cube", "strength": 0.7},
        "vignette": {"enabled": True, "intensity": 0.3, "smoothness": 0.6, "roundness": 1.0},
        "filmGrain": {"enabled": True, "intensity": 0.025, "size": 1.5},
    },
}


def translation(offset):
    return [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, offset[0], offset[1], offset[2], 1.0]


def yaw_translation(cosine, sine, offset):
    """Rotation about +Y by the angle with this cosine and sine, then a translation."""
    return [cosine, 0.0, -sine, 0.0, 0.0, 1.0, 0.0, 0.0, sine, 0.0, cosine, 0.0, offset[0], offset[1], offset[2], 1.0]


def wall_translation(offset):
    """Pitches a decal 90 degrees so its local -Y projects along world -Z onto the back wall."""
    return [1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, -1.0, 0.0, 0.0, offset[0], offset[1], offset[2], 1.0]


def decal_entities():
    return [
        ("Decal Floor Stencil", translation((0.0, 0.0, 2.3)), {
            "half_extents": [1.1, 0.3, 1.1], "albedo_asset": DECAL_TEXTURES + "decal_stencil.png",
            "tint": [1.0, 0.45, 0.08], "roughness": 0.6, "roughness_strength": 0.6}),
        ("Decal Arrow", yaw_translation(0.866025, 0.5, (-3.0, 0.0, 0.6)), {
            "half_extents": [0.45, 0.3, 0.75], "albedo_asset": DECAL_TEXTURES + "decal_arrow.png",
            "roughness": 0.7, "roughness_strength": 0.8, "sort_order": 1}),
        # Roughness 0.22 rather than mirror-like: the prepass normals and roughness that
        # screen-space reflections read are not decalled, so a very glossy decal looks grainy.
        ("Decal Puddle", translation((3.0, 0.0, 0.6)), {
            "half_extents": [1.4, 0.3, 1.0], "albedo_asset": DECAL_TEXTURES + "decal_puddle.png",
            "base_color_strength": 0.7, "roughness": 0.22, "roughness_strength": 1.0}),
        # Above the gallery backdrop (top at y 2.0), so the plate stays visible behind it.
        ("Decal Wall Plate", wall_translation((GALLERY_ORIGIN[0], 3.0, -9.0)), {
            "half_extents": [0.8, 0.3, 0.8], "albedo_asset": DECAL_TEXTURES + "decal_plate.png",
            "normal_asset": DECAL_TEXTURES + "decal_plate_normal.png", "roughness": 0.3,
            "roughness_strength": 1.0, "metallic": 1.0, "metallic_strength": 1.0}),
        # The box straddles the floor and the west wall; the angle fade must keep it off the wall.
        ("Decal Corner", translation((-7.4, 0.0, -0.5)), {
            "half_extents": [1.2, 0.8, 1.2], "albedo_asset": DECAL_TEXTURES + "decal_stencil.png",
            "tint": [0.1, 0.8, 1.0]}),
    ]


EMBERS = {
    "max_particles": 2000, "spawn_rate": 400.0, "lifetime": [1.0, 2.5], "direction": [0.0, 1.0, 0.0],
    "cone_angle": 0.45, "speed": [0.8, 2.2], "spawn_radius": 0.12, "gravity": [0.0, 0.4, 0.0], "drag": 0.6,
    "start_size": 0.045, "end_size": 0.012, "start_color": [1.0, 0.55, 0.15, 1.0],
    "end_color": [1.0, 0.18, 0.04, 0.0], "emissive_intensity": 6.0, "lit": False, "blend_mode": "additive",
    "space": "world", "soft_distance": 0.05,
}

# Low alpha keeps the overlapping billboards from turning into an opaque column.
SMOKE = {
    "max_particles": 400, "spawn_rate": 60.0, "lifetime": [3.0, 5.0], "direction": [0.0, 1.0, 0.0],
    "cone_angle": 0.35, "speed": [0.25, 0.6], "spawn_radius": 0.2, "gravity": [0.0, 0.12, 0.0], "drag": 0.35,
    "start_size": 0.25, "end_size": 1.1, "angular_speed": 0.6, "start_color": [0.5, 0.5, 0.52, 0.12],
    "end_color": [0.45, 0.45, 0.47, 0.0], "emissive_intensity": 0.0, "lit": True, "blend_mode": "alpha",
    "space": "world", "soft_distance": 0.4,
}

# Covers the room interior (walls at x +-8.0 and z -9.0, floor at y 0, open front at z 2)
# with probes about a metre apart.
IRRADIANCE_VOLUME = {
    "half_extents": [7.9, 2.0, 5.45], "probe_counts": [16, 4, 11], "intensity": 1.0, "blend_distance": 0.5,
    "normal_bias": 0.2, "view_bias": 0.2,
}
IRRADIANCE_VOLUME_CENTRE = (0.0, 2.0, -3.5)

# A small campfire in front of the granite rock: embers rise through a column of lit smoke.
CAMPFIRE = (-5.6, 0.05, -3.4)

# The fast tentacle stands in the puddle decal; its animation plays 3.5 times faster than the
# coverage tentacle so its tip moves several pixels per frame.
FAST_TENTACLE_PREFIX = "Fast "
FAST_TENTACLE_ORIGIN = (3.4, 0.0, 0.4)
FAST_TENTACLE_SPEED = 3.5


def feature_names():
    tentacle = {entity["name"] for entity in fast_tentacle_entities(0)}
    return {name for name, _, _ in decal_entities()} | tentacle | {
        "Material Gallery", "Ember Emitter", "Smoke Emitter", IRRADIANCE_VOLUME_NAME}


def fast_tentacle_entities(first_id):
    return build_coverage.tentacle_entities(first_id, FAST_TENTACLE_PREFIX, FAST_TENTACLE_ORIGIN,
                                            FAST_TENTACLE_SPEED)


def scene_entities(first_id, irradiance_bake_asset=""):
    """Scene-document entities for the feature content, with consecutive ids from first_id."""
    entities = []

    def add(name, matrix, components):
        entities.append({"id": first_id + len(entities), "name": name, "local_transform": matrix, "parent": None,
                         "components": components})

    add("Material Gallery", translation(GALLERY_ORIGIN), {"mesh_renderer": {"path": GALLERY_PATH, "mesh": 0}})
    for name, matrix, decal in decal_entities():
        add(name, matrix, {"decal": decal})
    add("Ember Emitter", translation(CAMPFIRE), {"particle_emitter": EMBERS})
    entities.extend(fast_tentacle_entities(first_id + len(entities)))
    add("Smoke Emitter", translation((CAMPFIRE[0], CAMPFIRE[1] + 0.25, CAMPFIRE[2])), {"particle_emitter": SMOKE})
    volume = dict(IRRADIANCE_VOLUME, bake_asset=irradiance_bake_asset)
    add(IRRADIANCE_VOLUME_NAME, translation(IRRADIANCE_VOLUME_CENTRE), {"irradiance_volume": volume})
    return entities


def existing_bake_asset(entities):
    for entity in entities:
        volume = entity.get("components", {}).get("irradiance_volume")
        if entity.get("name") == IRRADIANCE_VOLUME_NAME and volume:
            return volume.get("bake_asset", "")
    return ""


def update_scene(scene_path):
    with open(scene_path, encoding="utf-8") as file:
        document = json.load(file)
    names = feature_names()
    bake_asset = existing_bake_asset(document["entities"])
    kept = [entity for entity in document["entities"] if entity.get("name") not in names]
    next_id = max((entity["id"] for entity in kept), default=0) + 1
    document["entities"] = kept + scene_entities(next_id, bake_asset)
    with open(scene_path, "w", encoding="utf-8", newline="\n") as file:
        json.dump(document, file, indent=1)
        file.write("\n")


def update_project(project_path):
    with open(project_path, encoding="utf-8") as file:
        project = json.load(file)
    project.setdefault("renderSettings", {}).update(RENDER_SETTINGS)
    with open(project_path, "w", encoding="utf-8", newline="\n") as file:
        json.dump(project, file, indent=2)
        file.write("\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", required=True)
    arguments = parser.parse_args()
    projects = os.path.join(arguments.repo, "projects")
    update_scene(os.path.join(projects, "scenes", "showcase.zenith-scene"))
    update_project(os.path.join(projects, "showcase.zenith-project"))
    print("showcase feature content written to", projects)


if __name__ == "__main__":
    main()
