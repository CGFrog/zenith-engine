# Renderer showcase content

`projects/showcase.zenith-project` is a test scene that exercises the renderer
features the stock demo cannot: alpha-tested double-sided foliage, a blended
glass pane, textured and untextured metals across a roughness ladder, emissive
strips for bloom, a dense photoscan for mesh LODs, 52 clustered local lights, a
glossy floor for screen-space reflections, an HDR sky for image-based
lighting, an animated skinned tentacle (skinned motion vectors and packed skin
streams) and a mirror-smooth gold slab (screen-space reflections on a tinted
metal).

Run it with:

```powershell
build\Release\zenith.exe --project projects\showcase.zenith-project
```

It also works for benchmarking (`ZENITH_BENCHMARK_FRAMES`, etc.).

## Rebuilding the content

The generated files under `assets/showcase/`, `projects/showcase.zenith-project`
and `projects/scenes/showcase.zenith-scene` are committed, so rebuilding is only
needed to change the scene. It needs Blender 4.2 or newer, Python with Pillow,
and the source libraries listed in `assets/showcase/SOURCE.md`:

```powershell
blender --background --factory-startup --python tools/showcase/build_showcase.py -- `
    --repo . --textures E:\Textures --scans "X:\The Vault\Projects\Photoscans" `
    --hdri "<blender>\datafiles\studiolights\world\courtyard.exr"
python tools/showcase/compress_textures.py assets/showcase/showcase.gltf
```

The build's `zenith_cooked_textures` target then cooks the new textures to
BC7/BC5 automatically.

The skinned tentacle and gold mirror under `assets/showcase/coverage/` are
procedural and need only Python 3 (no Blender or source libraries). This
regenerates them and replaces just their entities in the scene document:

```powershell
python tools/showcase/build_coverage.py --repo .
```

`build_showcase.py` imports the same entity layout, so a full Blender rebuild
keeps them.

The KHR material extension gallery under `assets/showcase/materials/` (car
paint clearcoat, sheen fabrics, clear/frosted/tinted glass, a tinted specular
dielectric and an ior ladder in front of a checkered backdrop) is also
procedural Python 3. It only writes the asset; place it with a
`mesh_renderer` entity for `showcase/materials/material_gallery.gltf`, mesh 0:

```powershell
python tools/showcase/build_materials.py --repo .
```

`build_features.py` places the Milestone 24/25 features in the scene and merges
their render settings into the project (Python 3 only). It replaces just its own
entities: the material gallery (in front of the back wall), five clustered
decals (`assets/showcase/decals/`, from `build_decal_textures.py`), an ember and
smoke campfire of GPU particle emitters, a second tentacle instance swaying 3.5x
faster for object motion blur, and an irradiance volume over the room. The
render settings it owns turn on half-resolution GTAO, probe-volume indirect
diffuse, decals, High particles, Low motion blur, a mild High depth of field
(focus 6 m, f/1.4 on a 36 mm sensor) and the warm LUT grade with a vignette and
film grain. Rerun it after `build_coverage.py`, which replaces every tentacle
instance, then bake the irradiance volume:

```powershell
python tools/showcase/build_features.py --repo .
build\Debug\zenith_editor.exe projects\showcase.zenith-project --bake-irradiance-volumes
```

The bake writes `assets/baked/showcase/irradiance_volume_<id>_<hash>.zvol` and
saves the scene through the editor's serializer; `build_features.py` keeps the
volume's `bake_asset` when it runs again. Rebake after moving static geometry,
delete the previous `.zvol`, and regenerate the showcase reference captures
(`docs/RENDERER_VALIDATION.md`).
