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
