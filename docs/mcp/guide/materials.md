# Materials and lighting (built-in Lit)

The built-in Lit shader (a material with `shader: ""`, or a fork of it) has extended PBR lobes
on top of metallic-roughness, hex tiling against visible texture repetition, a scene Lit
Quality, and lights in photometric units with colour temperature. glTF models bring the lobes
with their `KHR_materials_*` extensions. Everything here costs nothing when it is off: a
material with no lobe enabled draws with exactly the shader it always did.

## The lobes

A lobe draws only while its `enabled` is true (its factors alone never switch it on, so
animating a factor never recompiles). Set them on a submesh material, a `.material` file or a
preset:

```
update_material path="materials/car.material" values={"pbr": {"clearcoat": {"enabled": true, "factor": 1, "roughness": 0.05}}}
set_properties entity="Car" component="MeshComponent" properties={"submeshes[0].material.pbr.clearcoat.enabled": true}
create_material path="materials/glass.material" values={"pbr": {"transmission": {"enabled": true, "factor": 1}, "volume": {"enabled": true, "thickness": 0.1}, "specular": {"enabled": true, "ior": 1.5}}}
```

A `pbr` object merges into the material's lobes (fields you leave out keep their value); `null`
for a map removes it, `"pbr": null` resets every lobe. A value out of range is an error naming
the field (`pbr.clearcoat.roughness: 2 is out of range (Clear Coat Roughness: 0 to 1)`). The
material read (get_entity, get_asset_info) lists `emissiveStrength` and the lobes that differ
from their defaults.

| Lobe | Fields (range) | Map (channels) | glTF |
|---|---|---|---|
| `clearcoat` | `factor` (0-1), `roughness` (0-1), `normalScale` | `map` (R factor, G roughness), `normalMap` (the coat's own normal map) | `KHR_materials_clearcoat` |
| `sheen` | `color` (linear, 0-1), `roughness` (0-1) | `map` (RGB colour, sRGB; A roughness) | `KHR_materials_sheen` |
| `anisotropy` | `strength` (0-1), `rotation` (radians from the tangent) | `map` (RG direction, B strength) | `KHR_materials_anisotropy` |
| `iridescence` | `factor` (0-1), `ior` (>= 1), `thicknessNm` ([min, max] nm) | `map` (R factor, G thickness between min and max) | `KHR_materials_iridescence` |
| `transmission` | `factor` (0-1) | `map` (R factor, G the volume's thickness) | `KHR_materials_transmission` |
| `volume` | `thickness` (>= 0), `attenuationColor`, `attenuationDistance` (> 0, null: none) | (transmission's G) | `KHR_materials_volume` |
| `specular` | `factor` (0-1), `color` (>= 0), `ior` (>= 1, or 0) | `map` (RGB colour, sRGB; A factor) | `KHR_materials_specular`, `KHR_materials_ior` |
| `hex` | `rotation` (0-1), `contrast` (0-1) | none (tiles every texture of UV set 0) | (engine) |

Every map has a `<map>TexCoord` (`mapTexCoord`, `normalTexCoord`): the UV set it samples (0 or 1).
`emissiveStrength` (>= 0, `KHR_materials_emissive_strength`) multiplies the emissive factor.
The volume needs transmission; Vertex lighting draws no lobe (hex tiling still applies), and an
unlit variant (no lights or IBL in the scene) only hex tiling.

## Maps and the 16-texture budget

GLES3 and WebGL2 have 16 fragment textures, and the busiest built-in Lit variant already binds
13. A material's variant binds at most **3** extension maps, kept in this priority order:
clear coat, specular, transmission, sheen, anisotropy, iridescence, clear coat normal. A map
past the third is dropped: its lobe draws with its factors alone. The rule is the same on
every platform. A variant in a scene with 2D light shadows has one slot less. The Inspector's
Advanced Surface footer says "Extension maps n / 3" and names the dropped maps;
`get_material_variant` returns them as `extensionMaps.dropped`.

When glTF gives a lobe's values in two textures (clear coat factor and roughness, sheen colour
and roughness, specular strength and colour, iridescence factor and thickness, transmission and
volume thickness) the import packs them into one map on a background job.

## Variants

The lobes are compile-time variants of the built-in Lit, named in its reference
`builtin:lit#<defines>` (a fork's `<fork>#<defines>`): `PBR_CLEARCOAT`, `PBR_SHEEN`,
`PBR_ANISOTROPY`, `PBR_IRIDESCENCE`, `PBR_TRANSMISSION`, `PBR_VOLUME`, `PBR_SPECULAR`,
`PBR_HEX_TILING`, `TERRAIN_HEX_TILING`, one `PBR_<LOBE>_MAP` per bound map, and
`USE_LIT_QUALITY`. Compiled shaders are stored as `lit_` plus two letters per define. While a
new variant compiles, the object keeps drawing with the one it had (no frame stall);
`get_material_variant {entity, submesh}` says which it draws (`ready`), its `defines`, the
`customId`, the maps, what the mobile tier strips, and how many builtin:lit variants the open
scenes draw (each one is a compile and an exported shader per backend: past 64 it warns).
Exports precompile them.

## Lit Quality

Scene setting `lit_quality` (Inspector: Lighting > Lights > Lit Quality, Lua `scene.litQuality`):

- `standard`: the shader as it always was.
- `high`: geometric specular anti-aliasing (no shimmering highlights on dense normal maps and
  fine curves) and multiscatter energy compensation of the direct lights (rough metals keep
  their brightness). A few percent per lit pixel.

New scenes are High; a scene saved before Lit Quality existed loads as Standard and saves
unchanged.

## Photometric lights and colour temperature

A Light's `intensityUnit` is `engine` (the engine's own number, drawn as it is: every light of
before), `lux` (directional), `candela` or `lumen` (point, spot). Physical units are drawn
through the scene's `physical_light_ev100` (Physical Light Exposure: Sunny 15, Overcast 12,
Interior 7, Night 2): at EV100 15 a 100000 lx sun is about 2.5 engine units. A spot's lumens
spread over the whole sphere, so its cone does not change its brightness.
`set_light_unit {entity, unit, preserveBrightness=true}` changes the unit and converts the
intensity so the light stays as bright (one undo step); setting `intensityUnit` alone keeps the
number. A unit the type cannot take draws as its neighbour (lux and candela, the same number).
`useColorTemperature` with `colorTemperature` (1000-20000 K, 6500 white) colours the light as
a black body; its `color` then filters that (the Inspector labels it Filter). The conversion is
on the CPU, only when a light or the EV100 changes.

## Hex tiling

- On a material (`pbr.hex.enabled`): every texture of UV set 0 is sampled on three randomly
  rotated and shifted copies on a hex grid, blended by the base colour's luminance
  (`contrast`); the normal map is turned back with its tile. Three taps per texture.
- On a Terrain's painted layer (`surfaceLayers[i].hexTiling`, `hexRotation`): the layer's flat
  projection instead of its near and far tiling; the side planes stay as they are.

## Transmission

A transmissive material draws after the opaque objects, opaque over a copy of them which it
refracts (through its volume) and blurs by its roughness. The copy holds the opaque scene and
the sky: transparent objects are not seen through it, and highlights behind it are clipped to
the display range. A transmissive material takes no SSAO and no 2D lights (its variant has no
room left for them). It counts as reading the scene colour (Scene Color Scale, one copy per camera
pass).

## Mobile PBR Tier

Project Settings > Graphics > Mobile PBR Tier (`Reduced` by default): Android, iOS and HTML5
exports leave out iridescence, anisotropy direction maps and clear coat normal maps; `Full`
keeps them. Desktop exports and the editor draw everything. The Inspector footer and
`get_material_variant` (`mobileStripped`) say what a material loses.

## glTF import

`KHR_materials_clearcoat`, `_sheen`, `_anisotropy`, `_iridescence`, `_transmission`, `_volume`,
`_specular`, `_ior` and `_emissive_strength` load into the lobes, clamped to the
specifications' ranges (a non-finite value takes its default, with one warning per model).
`KHR_materials_unlit` loads as the library Unlit shader (Lighting None). An extension the
engine does not read is named in one warning (`KHR_texture_transform`: the textures draw
untransformed); a required one is an error, and the model still loads. An edited lobe of a
model's submesh survives a reload of the model.

## Presets

`list_material_presets` / `apply_material_preset` (and the Inspector's Presets menu): Car
Paint (clear coat), Velvet (sheen), Brushed Metal (anisotropy), Soap Bubble (iridescence), Clear
Glass (transmission and volume), Tiled Floor (hex tiling). They set the built-in Lit on a
material that had a library shader.

## Lua

Flat `Material` properties on the copy `getMaterial` returns, then `setMaterial`:
`clearcoatEnabled`, `clearcoatFactor`, `clearcoatRoughness`, `clearcoatNormalScale`,
`sheenEnabled`, `sheenColor`, `sheenRoughness`, `anisotropyEnabled`, `anisotropyStrength`,
`anisotropyRotation`, `iridescenceEnabled`, `iridescenceFactor`, `iridescenceIor`,
`iridescenceThicknessRange`, `transmissionEnabled`, `transmissionFactor`, `volumeEnabled`,
`thicknessFactor`, `attenuationColor`, `attenuationDistance`, `specularEnabled`,
`specularFactor`, `specularColor`, `ior`, `emissiveStrength`, `hexTilingEnabled`,
`hexTilingRotation`, `hexTilingContrast` (maps read only). `Light`: `intensityUnit`
(`LightUnit.*`), `useColorTemperature`, `colorTemperature`, `setIntensityUnit(unit,
preserveBrightness)`. `scene.litQuality` (`LitQuality.*`), `scene.physicalLightEV100`. Terrain
layers: `hexTiling`, `hexRotation`. Details: `jasper://docs/lua-api`.

## Limits

- The lobes are the built-in Lit's (and its forks' that keep the hooks, `jasper://docs/shaders`):
  library shaders and the SSR G-buffer do not draw them.
- The sheen's environment light and the transmission blur are approximations next to the
  Khronos viewer (no Charlie cube, no mip chain of the scene copy).
- The pipeline is 8-bit: physical units are pre-exposed by the EV100, there is no automatic
  exposure.
