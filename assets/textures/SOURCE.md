# Wood siding texture provenance

These runtime PNGs were converted from the user-supplied 1K TIFF texture set
`Wood/Siding/T_Wood_SidingOutdoor4_1x1_1K_*`. The originals are intentionally
not modified or copied into the repository.

- `wood_siding_basecolor.png`: albedo, sampled as sRGB.
- `wood_siding_normal.png`: tangent-space normal with the source green channel
  flipped from DirectX to glTF/OpenGL convention.
- `wood_siding_orm.png`: packed occlusion/roughness/metallic. R is 1.0, G comes
  from the supplied roughness map, and B is 0.0 for non-metal wood.

The supplied height map is not used by the initial renderer.
