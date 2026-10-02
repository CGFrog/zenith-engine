"""Builds the renderer showcase content with Blender.

Usage (Blender 4.2+ / 5.x):
    blender --background --factory-startup --python tools/showcase/build_showcase.py -- \
        --repo <zenith checkout> --textures <texture library> --scans <photoscan library> \
        --hdri <equirectangular .exr or .hdr>

Writes assets/showcase/ (glTF, textures, sky.hdr), projects/showcase.zenith-project
and projects/scenes/showcase.zenith-scene. Source libraries are read only; the
converted files follow the conventions in assets/textures/SOURCE.md (DirectX
normals flipped to glTF convention, roughness/metallic packed into ORM).
"""

import argparse
import json
import math
import os
import random
import sys

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_coverage  # noqa: E402  (sibling script, importable without Blender)

TEXTURE_SIZE = 1024
SCAN_TEXTURE_SIZE = 2048
SCAN_TRIANGLES = 60000
GLTF_NAME = "showcase.gltf"


def parse_arguments():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    parser.add_argument("--textures", required=True)
    parser.add_argument("--scans", required=True)
    parser.add_argument("--hdri", required=True)
    return parser.parse_args(argv)


# ---------------------------------------------------------------- textures


def load_pixels(path, size):
    """Returns raw encoded RGBA values in [0, 1], resized to size x size."""
    image = bpy.data.images.load(path, check_existing=False)
    # Non-Color keeps Blender from linearizing sRGB-tagged data on read.
    image.colorspace_settings.name = "Non-Color"
    if tuple(image.size) != (size, size):
        image.scale(size, size)
    pixels = np.empty(size * size * 4, dtype=np.float32)
    image.pixels.foreach_get(pixels)
    bpy.data.images.remove(image)
    return pixels.reshape(size, size, 4)


def save_png(pixels, path):
    height, width, _ = pixels.shape
    image = bpy.data.images.new(os.path.basename(path), width, height, alpha=True)
    image.colorspace_settings.name = "Non-Color"
    image.pixels.foreach_set(np.clip(pixels, 0.0, 1.0).astype(np.float32).ravel())
    image.filepath_raw = path
    image.file_format = "PNG"
    image.save()
    bpy.data.images.remove(image)


class TextureWriter:
    def __init__(self, library, output_directory):
        self.library = library
        self.output = output_directory
        self.written = []
        os.makedirs(output_directory, exist_ok=True)

    def source(self, relative):
        path = os.path.join(self.library, relative)
        if not os.path.isfile(path):
            raise FileNotFoundError(path)
        return path

    def write(self, name, pixels):
        path = os.path.join(self.output, name + ".png")
        save_png(pixels, path)
        self.written.append(path)
        return path

    def base_color(self, name, albedo, alpha=None):
        pixels = load_pixels(self.source(albedo), TEXTURE_SIZE)
        if alpha:
            pixels[..., 3] = load_pixels(self.source(alpha), TEXTURE_SIZE)[..., 0]
        else:
            pixels[..., 3] = 1.0
        return self.write(name + "_basecolor", pixels)

    def normal(self, name, source):
        pixels = load_pixels(self.source(source), TEXTURE_SIZE)
        pixels[..., 1] = 1.0 - pixels[..., 1]  # DirectX -> glTF (+Y up)
        pixels[..., 3] = 1.0
        return self.write(name + "_normal", pixels)

    def orm(self, name, roughness, roughness_scale=1.0, metallic=None, metallic_value=0.0):
        rough = load_pixels(self.source(roughness), TEXTURE_SIZE)[..., 0] * roughness_scale
        pixels = np.ones((TEXTURE_SIZE, TEXTURE_SIZE, 4), dtype=np.float32)
        pixels[..., 1] = np.clip(rough, 0.02, 1.0)
        pixels[..., 2] = (load_pixels(self.source(metallic), TEXTURE_SIZE)[..., 0]
                          if metallic else metallic_value)
        return self.write(name + "_orm", pixels)


# ---------------------------------------------------------------- materials


def image_node(tree, path, color):
    node = tree.nodes.new("ShaderNodeTexImage")
    node.image = bpy.data.images.load(path, check_existing=True)
    node.image.colorspace_settings.name = "sRGB" if color else "Non-Color"
    return node


def pbr_material(name, base=None, normal=None, orm=None, color=(0.8, 0.8, 0.8, 1.0),
                 roughness=0.5, metallic=0.0, emission=None, emission_strength=0.0,
                 alpha=1.0):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    tree = material.node_tree
    bsdf = tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = color
    bsdf.inputs["Roughness"].default_value = roughness
    bsdf.inputs["Metallic"].default_value = metallic
    bsdf.inputs["Alpha"].default_value = alpha
    if base:
        node = image_node(tree, base, True)
        tree.links.new(node.outputs["Color"], bsdf.inputs["Base Color"])
        tree.links.new(node.outputs["Alpha"], bsdf.inputs["Alpha"])
    if normal:
        node = image_node(tree, normal, False)
        normal_map = tree.nodes.new("ShaderNodeNormalMap")
        tree.links.new(node.outputs["Color"], normal_map.inputs["Color"])
        tree.links.new(normal_map.outputs["Normal"], bsdf.inputs["Normal"])
    if orm:
        node = image_node(tree, orm, False)
        split = tree.nodes.new("ShaderNodeSeparateColor")
        tree.links.new(node.outputs["Color"], split.inputs["Color"])
        tree.links.new(split.outputs["Green"], bsdf.inputs["Roughness"])
        tree.links.new(split.outputs["Blue"], bsdf.inputs["Metallic"])
    if emission:
        bsdf.inputs["Emission Color"].default_value = emission
        bsdf.inputs["Emission Strength"].default_value = emission_strength
    return material


# ---------------------------------------------------------------- geometry


def box_map_uvs(mesh, tile):
    """World-scale box mapping so tiling textures keep a constant density."""
    work = bmesh.new()
    work.from_mesh(mesh)
    uv_layer = work.loops.layers.uv.verify()
    for face in work.faces:
        axis = max(range(3), key=lambda index: abs(face.normal[index]))
        for loop in face.loops:
            position = loop.vert.co
            u, v = [position[index] for index in range(3) if index != axis]
            loop[uv_layer].uv = (u / tile, v / tile)
    work.to_mesh(mesh)
    work.free()


def add_box(name, size, location, material, tile=None):
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=location)
    obj = bpy.context.active_object
    obj.name = name
    obj.data.name = name
    obj.scale = size
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    if tile:
        box_map_uvs(obj.data, tile)
    obj.data.materials.append(material)
    return obj


def add_sphere(name, radius, location, material):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=64, ring_count=32, radius=radius, location=location)
    obj = bpy.context.active_object
    obj.name = name
    obj.data.name = name
    bpy.ops.object.shade_smooth()
    obj.data.materials.append(material)
    return obj


def add_card_clump(name, cards, width, height, material):
    """Crossed vertical alpha-tested cards centred on the origin."""
    work = bmesh.new()
    uv_layer = work.loops.layers.uv.new()
    for index in range(cards):
        angle = math.pi * index / cards
        direction = Vector((math.cos(angle), math.sin(angle), 0.0)) * (width * 0.5)
        corners = [-direction, direction, direction + Vector((0, 0, height)), -direction + Vector((0, 0, height))]
        face = work.faces.new([work.verts.new(corner) for corner in corners])
        for loop, uv in zip(face.loops, [(0, 0), (1, 0), (1, 1), (0, 1)]):
            loop[uv_layer].uv = uv
    mesh = bpy.data.meshes.new(name)
    work.to_mesh(mesh)
    work.free()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)
    mesh.materials.append(material)
    return obj


def crop_to_summit(mesh, radius_fraction):
    """Keeps faces near the scan's highest point, trimming the scanned ground."""
    work = bmesh.new()
    work.from_mesh(mesh)
    summit = max(work.verts, key=lambda vertex: vertex.co.z).co.copy()
    xs = [vertex.co.x for vertex in work.verts]
    ys = [vertex.co.y for vertex in work.verts]
    radius = radius_fraction * max(max(xs) - min(xs), max(ys) - min(ys))
    far = [face for face in work.faces
           if (Vector((face.calc_center_median().x, face.calc_center_median().y)) - summit.xy).length > radius]
    bmesh.ops.delete(work, geom=far, context="FACES")
    # Scanned terrain fragments under the surface would set the lowest point, so
    # the ground is a low height percentile and anything beneath it is removed.
    ground = float(np.percentile([vertex.co.z for vertex in work.verts if vertex.link_faces], 5.0))
    buried = [face for face in work.faces if max(vertex.co.z for vertex in face.verts) < ground]
    bmesh.ops.delete(work, geom=buried, context="FACES")
    loose = [vertex for vertex in work.verts if not vertex.link_faces]
    bmesh.ops.delete(work, geom=loose, context="VERTS")
    work.to_mesh(mesh)
    work.free()
    return ground


def import_scan(name, folder, textures, material_name):
    texturing = os.path.join(folder, "MeshroomCache", "Texturing")
    run = next(entry.path for entry in os.scandir(texturing) if entry.is_dir())
    bpy.ops.wm.obj_import(filepath=os.path.join(run, "texturedMesh.obj"))
    # Photogrammetry output carries loose debris from the surroundings; keep the
    # largest connected piece.
    bpy.context.view_layer.objects.active = bpy.context.selected_objects[0]
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.separate(type="LOOSE")
    bpy.ops.object.mode_set(mode="OBJECT")
    pieces = list(bpy.context.selected_objects)
    obj = max(pieces, key=lambda piece: len(piece.data.polygons))
    for piece in pieces:
        if piece != obj:
            bpy.data.objects.remove(piece)
    obj.select_set(True)
    obj.name = name
    obj.data.name = name
    bpy.context.view_layer.objects.active = obj

    triangles = sum(len(polygon.vertices) - 2 for polygon in obj.data.polygons)
    if triangles > SCAN_TRIANGLES:
        decimate = obj.modifiers.new("decimate", "DECIMATE")
        decimate.ratio = SCAN_TRIANGLES / triangles
        bpy.ops.object.modifier_apply(modifier=decimate.name)
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    obj.data.validate(clean_customdata=False)

    # Lay the scan flat: its smallest principal axis becomes up.
    points = np.array([vertex.co[:] for vertex in obj.data.vertices])
    centred = points - points.mean(axis=0)
    _, vectors = np.linalg.eigh(np.cov(centred.T))
    up = Vector(vectors[:, 0])
    if up.z < 0.0:
        up = -up
    obj.data.transform(up.rotation_difference(Vector((0.0, 0.0, 1.0))).to_matrix().to_4x4())
    ground = crop_to_summit(obj.data, radius_fraction=0.2)
    low = Vector([min(v.co[i] for v in obj.data.vertices) for i in range(3)])
    high = Vector([max(v.co[i] for v in obj.data.vertices) for i in range(3)])
    # Sink the ground slightly so the scan's ragged rim sits just under the floor.
    obj.data.transform(Matrix.Translation(Vector(((low.x + high.x) * -0.5, (low.y + high.y) * -0.5, -ground))))
    extent = high - low
    obj.data.transform(Matrix.Scale(1.8 / max(extent.x, extent.y), 4))
    obj.data.transform(Matrix.Translation(Vector((0.0, 0.0, -0.03))))
    bpy.ops.object.shade_smooth()

    atlas = os.path.join(run, "texture_1001.png")
    pixels = load_pixels(atlas, SCAN_TEXTURE_SIZE)
    pixels[..., 3] = 1.0
    base = textures.write(name.lower() + "_basecolor", pixels)
    obj.data.materials.clear()
    obj.data.materials.append(pbr_material(material_name, base=base, roughness=0.85))
    return obj


# ---------------------------------------------------------------- scene


def build_scene(arguments, textures):
    tex = textures
    concrete = pbr_material(
        "PolishedConcrete",
        base=tex.base_color("concrete", r"Rock\Concrete2\T_Concrete_WW2_2x2_2K_albedo.tif"),
        normal=tex.normal("concrete", r"Rock\Concrete2\T_Concrete_WW2_2x2_1K_normal.tif"),
        orm=tex.orm("concrete", r"Rock\Concrete2\T_Concrete_WW2_2x2_1K_roughness.tif", roughness_scale=0.45))
    brick = pbr_material(
        "Brick",
        base=tex.base_color("brick", r"Industrial\Bricks1\T_Wall_BrickSloppy_2.5x2.5_1K_albedo.tif"),
        normal=tex.normal("brick", r"Industrial\Bricks1\T_Wall_BrickSloppy_2.5x2.5_1K_normal.tif"),
        orm=tex.orm("brick", r"Industrial\Bricks1\T_Wall_BrickSloppy_2.5x2.5_1K_roughness.tif"))
    stone = pbr_material(
        "Stone",
        base=tex.base_color("stone", r"Rock\Stone2\T_Stone_BaseSurface3_1x1_1K_albedo.tif"),
        normal=tex.normal("stone", r"Rock\Stone2\T_Stone_BaseSurface3_1x1_1K_normal.tif"),
        orm=tex.orm("stone", r"Rock\Stone2\T_Stone_BaseSurface3_1x1_1K_roughness.tif"))
    textured_metals = [
        ("Galvanized", r"Metal\Galvanized Metal 1\T_Metal_Galvanized", None, 1.0),
        ("Aluminium", r"Metal\Aluminium 1\T_OldAluminium_Worn", None, 1.0),
        ("MetalSheet", r"Metal\Metal Sheet 1\T_MetalSheet_Aged", "metallic", 0.0),
        ("Rust", r"Metal\Rust 1\T_Rust_Heavy_MediumRust", None, 0.0),
    ]
    metal_materials = []
    for name, prefix, metallic_map, metallic_value in textured_metals:
        metallic = prefix + "_1K_metallic.tif" if metallic_map else None
        metal_materials.append(pbr_material(
            name,
            base=tex.base_color(name.lower(), prefix + "_2K_albedo.tif"),
            normal=tex.normal(name.lower(), prefix + "_1K_normal.tif"),
            orm=tex.orm(name.lower(), prefix + "_1K_roughness.tif", metallic=metallic,
                        metallic_value=metallic_value)))
    fern = pbr_material(
        "Fern",
        base=tex.base_color("fern", r"Plants\Fern1 (House)\T_Plant_HouseFern_2K_albedo.tif",
                            alpha=r"Plants\Fern1 (House)\T_Plant_HouseFern_1K_alpha.tif"),
        normal=tex.normal("fern", r"Plants\Fern1 (House)\T_Plant_HouseFern_1K_normal.tif"),
        orm=tex.orm("fern", r"Plants\Fern1 (House)\T_Plant_HouseFern_1K_roughness.tif"))
    grass = pbr_material(
        "Grass",
        base=tex.base_color("grass", r"Plants\Grass1\T_Grass13_2K_albedo.tif",
                            alpha=r"Plants\Grass1\T_Grass13_2K_alpha.tif"),
        normal=tex.normal("grass", r"Plants\Grass1\T_Grass13_2K_normal.tif"),
        orm=tex.orm("grass", r"Plants\Grass1\T_Grass13_2K_roughness.tif"))

    # Blender is Z-up; the glTF exporter converts to Y-up (Blender +Y becomes -Z).
    add_box("Floor", (20.0, 18.0, 0.2), (0.0, 3.0, -0.1), concrete, tile=2.0)
    add_box("WallBack", (16.6, 0.3, 4.0), (0.0, 9.15, 2.0), brick, tile=2.5)
    add_box("WallLeft", (0.3, 11.0, 4.0), (-8.15, 3.5, 2.0), brick, tile=2.5)
    add_box("WallRight", (0.3, 11.0, 4.0), (8.15, 3.5, 2.0), brick, tile=2.5)

    for index, material in enumerate(metal_materials):
        x = -3.0 + index * 2.0
        add_box("Pedestal%d" % index, (0.8, 0.8, 0.6), (x, 2.0, 0.3), stone, tile=1.0)
        add_sphere("TexturedSphere%d" % index, 0.4, (x, 2.0, 1.0), material)

    ladder = [("Chrome", (0.95, 0.95, 0.95, 1.0), 1.0, 0.04), ("Gold", (1.0, 0.77, 0.34, 1.0), 1.0, 0.2),
              ("Copper", (0.95, 0.64, 0.54, 1.0), 1.0, 0.4), ("RedPlastic", (0.7, 0.04, 0.03, 1.0), 0.0, 0.08),
              ("WhitePlastic", (0.85, 0.85, 0.85, 1.0), 0.0, 0.5), ("Rubber", (0.04, 0.04, 0.04, 1.0), 0.0, 0.9)]
    for index, (name, color, metallic, roughness) in enumerate(ladder):
        material = pbr_material(name, color=color, metallic=metallic, roughness=roughness)
        add_sphere("Sphere" + name, 0.45, (-3.75 + index * 1.5, 5.0, 0.45), material)

    for index, (name, color) in enumerate([("EmissiveCyan", (0.1, 0.8, 1.0, 1.0)),
                                           ("EmissiveOrange", (1.0, 0.45, 0.08, 1.0))]):
        material = pbr_material(name, color=(0.02, 0.02, 0.02, 1.0), roughness=0.4, emission=color,
                                emission_strength=12.0)
        add_box("Strip" + name, (7.0, 0.06, 0.06), (-4.0 + index * 8.0, 8.9, 0.25), material)
        add_box("Pillar" + name, (0.12, 0.12, 2.4), (-6.5 + index * 13.0, 7.5, 1.2), material)

    glass = pbr_material("Glass", color=(0.65, 0.85, 0.95, 1.0), roughness=0.05, alpha=0.25)
    add_box("GlassPane", (2.4, 0.04, 1.8), (2.8, 6.6, 0.9), glass)

    import_scan("GraniteRock", os.path.join(arguments.scans, "Granite Pond Rock 2"), textures, "GraniteScan")
    rock = bpy.data.objects["GraniteRock"]
    rock.location = (-5.5, 5.5, 0.0)

    fern_clump = add_card_clump("TemplateFern", 3, 0.9, 0.8, fern)
    grass_clump = add_card_clump("TemplateGrass", 4, 0.7, 0.55, grass)
    for material in (fern, grass, glass):
        material.use_backface_culling = False
    return [fern_clump.name, grass_clump.name]


def export_gltf(output_directory):
    path = os.path.join(output_directory, GLTF_NAME)
    bpy.ops.export_scene.gltf(filepath=path, export_format="GLTF_SEPARATE", export_texture_dir="textures",
                              export_tangents=True, export_yup=True, export_apply=True,
                              export_cameras=False, export_lights=False, export_extras=False,
                              export_animations=False)
    return path


# Material flags are set explicitly rather than inferred by the exporter.
MATERIAL_FLAGS = {
    "Fern": {"alphaMode": "MASK", "alphaCutoff": 0.5, "doubleSided": True},
    "Grass": {"alphaMode": "MASK", "alphaCutoff": 0.5, "doubleSided": True},
    "Glass": {"alphaMode": "BLEND", "doubleSided": True},
}


def patch_gltf(path):
    with open(path, encoding="utf-8") as file:
        document = json.load(file)
    for material in document.get("materials", []):
        flags = MATERIAL_FLAGS.get(material["name"], {"alphaMode": "OPAQUE", "doubleSided": False})
        material.pop("alphaCutoff", None)
        material.update(flags)
        if material["name"].startswith("Emissive"):
            material.setdefault("extensions", {})["KHR_materials_emissive_strength"] = {"emissiveStrength": 12.0}
            used = document.setdefault("extensionsUsed", [])
            if "KHR_materials_emissive_strength" not in used:
                used.append("KHR_materials_emissive_strength")
    with open(path, "w", encoding="utf-8", newline="\n") as file:
        json.dump(document, file, indent=1)
    return document


# ---------------------------------------------------------------- scene document


def column_major(matrix):
    return [matrix[row][column] for column in range(4) for row in range(4)]


def node_matrix(node):
    translation = Matrix.Translation(Vector(node.get("translation", (0.0, 0.0, 0.0))))
    w = node.get("rotation", (0.0, 0.0, 0.0, 1.0))
    from mathutils import Quaternion
    rotation = Quaternion((w[3], w[0], w[1], w[2])).to_matrix().to_4x4()
    scale = node.get("scale", (1.0, 1.0, 1.0))
    return translation @ rotation @ Matrix.Diagonal((scale[0], scale[1], scale[2], 1.0))


class SceneWriter:
    def __init__(self):
        self.entities = []

    def add(self, name, matrix=None, components=None, parent=None):
        entity_id = len(self.entities) + 1
        self.entities.append({"id": entity_id, "name": name,
                              "local_transform": column_major(matrix or Matrix.Identity(4)),
                              "parent": parent, "components": components or {}})
        return entity_id


def static_body():
    return {"motion_type": 0, "layer": 0, "mass": 1.0, "friction": 0.6, "restitution": 0.0,
            "linear_damping": 0.05, "angular_damping": 0.05, "gravity_factor": 1.0, "sensor": False,
            "allow_sleeping": True, "continuous_collision": False, "lock_rotation": False}


def box_collider(writer, name, centre, half_extents):
    writer.add(name, Matrix.Translation(Vector(centre)),
               {"collider": {"type": "box", "half_extents": list(half_extents)}, "rigid_body": static_body()})


def build_scene_document(document, templates):
    writer = SceneWriter()
    model_path = "showcase/" + GLTF_NAME
    writer.add("Sun", components={"directional_light": {
        "direction": list(Vector((0.45, 0.85, 0.3)).normalized()), "color": [1.0, 0.92, 0.8],
        "intensity": 6.0, "primary": True,
        "shadows": {"enabled": True, "half_extent": 14.0, "focus_distance": 6.0, "depth_range": 60.0,
                    "constant_bias": 0.5, "slope_bias": 1.5, "normal_bias": 0.02}}})
    writer.add("Environment", components={"environment_light": {
        "sky_color": [0.2, 0.24, 0.34], "ground_color": [0.08, 0.065, 0.05], "diffuse_intensity": 1.0,
        "specular_intensity": 1.0, "primary": True, "environment_asset": "showcase/sky.hdr"}})

    mesh_index = {}
    for node in document["nodes"]:
        if "mesh" in node:
            mesh_index[node["name"]] = node["mesh"]
    for node in document["nodes"]:
        if "mesh" not in node or node["name"] in templates:
            continue
        components = {"mesh_renderer": {"path": model_path, "mesh": node["mesh"]}}
        if node["name"].startswith(("TexturedSphere", "Sphere")):
            radius = 0.4 if node["name"].startswith("Textured") else 0.45
            components["collider"] = {"type": "sphere", "radius": radius}
            components["rigid_body"] = static_body()
        writer.add(node["name"], node_matrix(node), components)

    # glTF coordinates (Y-up): Blender (x, y, z) -> (x, z, -y).
    box_collider(writer, "Floor Collider", (0.0, -0.1, -3.0), (10.0, 0.1, 9.0))
    box_collider(writer, "Back Wall Collider", (0.0, 2.0, -9.15), (8.3, 2.0, 0.15))
    box_collider(writer, "Left Wall Collider", (-8.15, 2.0, -3.5), (0.15, 2.0, 5.5))
    box_collider(writer, "Right Wall Collider", (8.15, 2.0, -3.5), (0.15, 2.0, 5.5))
    for index in range(4):
        box_collider(writer, "Pedestal Collider %d" % index, (-3.0 + index * 2.0, 0.3, -2.0), (0.4, 0.3, 0.4))
    box_collider(writer, "Rock Collider", (-5.5, 0.4, -5.5), (0.9, 0.4, 0.9))

    generator = random.Random(1923)
    fern_mesh, grass_mesh = mesh_index[templates[0]], mesh_index[templates[1]]
    for index in range(36):
        x = generator.uniform(4.2, 7.6)
        z = generator.uniform(-8.5, -0.5)
        scale = generator.uniform(0.7, 1.4)
        matrix = (Matrix.Translation((x, 0.0, z)) @ Matrix.Rotation(generator.uniform(0, math.tau), 4, "Y") @
                  Matrix.Scale(scale, 4))
        mesh = fern_mesh if index % 3 == 0 else grass_mesh
        writer.add("Foliage %d" % (index + 1), matrix, {"mesh_renderer": {"path": model_path, "mesh": mesh}})

    colors = [(1.0, 0.3, 0.12), (0.2, 0.85, 0.35), (0.2, 0.4, 1.0), (1.0, 0.85, 0.4)]
    for index in range(48):
        x = -6.5 + (index % 12) * (13.0 / 11.0)
        z = 1.5 - (index // 12) * 2.6
        writer.add("Floor Light %d" % (index + 1), Matrix.Translation((x, 0.35, z)),
                   {"point_light": {"color": list(colors[index % 4]), "intensity": 3.0, "range": 2.5}})
    for index in range(4):
        x = -3.0 + index * 2.0
        writer.add("Sphere Spot %d" % (index + 1), Matrix.Translation((x, 3.6, -2.0)),
                   {"spot_light": {"direction": [0.0, -1.0, 0.0], "color": [1.0, 0.95, 0.9], "intensity": 25.0,
                                   "range": 6.0, "inner_cone": 0.25, "outer_cone": 0.45}})
    # The skinned tentacle and gold mirror come from build_coverage.py (assets/showcase/coverage/).
    writer.entities.extend(build_coverage.scene_entities(len(writer.entities) + 1))
    # Two overlapping probes with distinct environments (build_probe_environments.py)
    # split the room into west and east halves for per-pixel probe blending.
    for name, x, asset in (("Reflection Probe West", -4.0, "showcase/probe_cool.hdr"),
                           ("Reflection Probe East", 4.0, "showcase/probe_warm.hdr")):
        writer.add(name, Matrix.Translation((x, 2.0, -3.0)),
                   {"reflection_probe": {"half_extents": [4.0, 2.0, 6.0], "blend_distance": 1.5, "priority": 0,
                                         "environment_asset": asset}})
    return {"version": 1, "entities": writer.entities}


# ---------------------------------------------------------------- project


PROJECT = {
    "version": 3,
    "displayName": "Zenith Renderer Showcase",
    "assetRoot": "../assets",
    "sceneRoot": "scenes",
    "startupScene": "showcase.zenith-scene",
    "renderSettings": {
        "version": 1, "antialiasing": 2, "shadows": 3, "lighting": 1, "bloom": 1, "tonemap": 1,
        "environment": 2, "environmentBackground": True, "localLights": 1, "maximumLocalLights": 1024,
        "reflectionProbes": True, "reflectionProbeBlending": 1, "preferGpuDrivenSubmission": False, "depthPrepass": True,
        "lodErrorThresholdPixels": 1.0, "cascadeSplitLambda": 0.65, "cascadeBlendFraction": 0.1,
        "visualizeShadowCascades": False, "ambientOcclusion": 2, "reflections": 2,
    },
}


def convert_hdri(source, destination):
    # A file-backed image has no buffer to save until its pixels are read, so the
    # linear float data is copied into a new image written as Radiance HDR.
    source_image = bpy.data.images.load(source, check_existing=False)
    width, height = source_image.size
    pixels = np.empty(width * height * 4, dtype=np.float32)
    source_image.pixels.foreach_get(pixels)
    bpy.data.images.remove(source_image)
    image = bpy.data.images.new("sky", width, height, alpha=False, float_buffer=True)
    image.pixels.foreach_set(pixels)
    image.filepath_raw = destination
    image.file_format = "HDR"
    image.save()
    bpy.data.images.remove(image)


def main():
    arguments = parse_arguments()
    bpy.ops.wm.read_factory_settings(use_empty=True)
    output = os.path.join(arguments.repo, "assets", "showcase")
    os.makedirs(output, exist_ok=True)
    # Start from an empty output so renamed or dropped textures do not linger.
    texture_output = os.path.join(output, "textures")
    if os.path.isdir(texture_output):
        for entry in os.listdir(texture_output):
            os.remove(os.path.join(texture_output, entry))
    staging = os.path.join(output, "_staging")
    textures = TextureWriter(arguments.textures, staging)
    templates = build_scene(arguments, textures)
    gltf_path = export_gltf(output)
    document = patch_gltf(gltf_path)
    for path in textures.written:
        os.remove(path)
    os.rmdir(staging)
    convert_hdri(arguments.hdri, os.path.join(output, "sky.hdr"))

    scenes = os.path.join(arguments.repo, "projects", "scenes")
    with open(os.path.join(scenes, "showcase.zenith-scene"), "w", encoding="utf-8", newline="\n") as file:
        json.dump(build_scene_document(document, templates), file, indent=1)
        file.write("\n")
    with open(os.path.join(arguments.repo, "projects", "showcase.zenith-project"), "w", encoding="utf-8",
              newline="\n") as file:
        json.dump(PROJECT, file, indent=2)
        file.write("\n")
    print("showcase written to", output)


main()
