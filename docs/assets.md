# Assets

`UnrealLib::Assets` (CMake target `UnrealLib::Assets`) deserializes the native data of asset objects and exports it
to common formats. Deserialization (`read_*`) is separate from decoding (`decode_*`) and export (`to_*`).

Every reader takes the object's metadata from its tagged script properties and requires its native data to be
consumed **exactly**: a reader that stops early or runs past the export is an error, never silently accepted.

| Class | Reader | Export | Mirror's Edge verification |
|-------|--------|--------|----------------------------|
| Texture2D, LightMapTexture2D, ShadowMapTexture2D, TerrainWeightMapTexture, TextureFlipBook | `read_texture2d` | PNG, TGA, DDS (all mips, lossless) | 58,960 textures parse; 57,835 decode at their **largest** mip (34,806 of those from mips streamed in other packages), 0 failures |
| TextureCube | `read_texture_cube` | (faces are Texture2D) | 1,143 parse |
| SoundNodeWave | `read_sound_wave` | stored file (Ogg Vorbis) | 78,049 parse; 78,048 extract (1 has no data), all Ogg; sample of 320 checked as valid Vorbis streams |
| StaticMesh | `read_static_mesh` | Wavefront OBJ | 19,007 parse; 35.1M vertices, 12.5M triangles; all indices in range; every LOD 0 vertex inside the stored bounds |
| SkeletalMesh | `read_skeletal_mesh` | ActorX PSK | 742 parse; 19,540 bones, 1.8M vertices; every vertex's weights sum to 255; all bone/index references in range |
| AnimSequence | `read_anim_sequence` | ActorX PSA (per AnimSet) | 6,938 decode; decoded tracks cover each compressed byte stream exactly; all rotations unit length |
| AnimSet, TdAnimSet | `read_anim_set` | — | 679 parse; for all 560 populated sets each sequence has one track per bone |
| Polys | `read_polys`, `write_polys` | T3D `Begin PolyList` (via T3D export/import) | 9,392 parse exactly (65,005 polygons) and `write_polys` reproduces every one byte for byte; every polygon has ≥ 3 vertices and a unit normal; ElementOwner is the Polys itself or its outer Model |
| Model | `read_model` (up to the Polys reference) | — | 9,392 parse (41,850 BSP surfaces, zones included); every surface indexes the decoded points/vectors; the Polys reference is a Polys |
| Material, MaterialInstanceConstant, MaterialInstanceTimeVarying | `read_material` | JSON metadata | 29,392 read with a ReflectionSchema; every parameter array decodes (21,821 parameters, 84,137 texture references, 391,680 expressions) |

The checks run in `MirrorsEdgeAssetTests.FullCorpusAssets`, `.FullCorpusMaterials` and `.FullCorpusBrushes` (`UNREALLIB_FIXTURE_SWEEP=1`,
about two minutes in Release). `UNREALLIB_ASSET_DUMP=<dir>` writes a sample of decoded textures, meshes and skeletal
meshes for manual review (game-derived output: never commit it).

## Sources

* Texture, bulk data and sound layouts: upstream UELib (`UTexture`, `UTexture2D`, `UBulkData`, `USoundNodeWave`,
  version gates from `PackageObjectLegacyVersion`).
* Static/skeletal mesh and animation layouts and the Mirror's Edge deviations: UE Viewer (umodel) by Konstantin Nosov,
  MIT license — `UnMesh3.cpp` (`#if MEDGE` branches), `UnAnim3.cpp`, `UnMeshTypes.h`, `ExportPsk.cpp` (PSK/PSA
  conventions). UnrealLib's code is an independent implementation; upstream UELib has no UE3 mesh or animation code.
* Everything was then checked against the game's files (exact consumption plus the semantic checks above). Two facts
  came from the fixtures alone and are recorded in the Mirror's Edge `GameProfile::AssetRules`:
  every static mesh ends with one extra float of unknown meaning (`static_mesh_trailing_float`), and the per-bone
  collision data of skeletal meshes (PerPolyKDOPs) is non-empty in exactly one mesh.

## Bulk data and streamed mips

`BulkData` is FUntypedBulkData: flags, element count, size on disk, offset. Inline payloads follow their record; in 30
packages the stored offset is stale by a constant 138 or 216 bytes (1,254 records), so UnrealLib — like UE3 — reads
inline payloads at their actual position and reports `offset_mismatch()`.

Mirror's Edge has no texture file caches (`.tfc`). Mips flagged `StoreInSeparateFile` are stored in the file of the
texture's **outermost package** (e.g. `B_C_01.upk` for `B_C_01.Facade.T_X` copied into a map), at `offset_in_file` of the
file *as stored on disk*, LZO-compressed. `load_mip(package, texture, mip, &linker)` finds that file through the
Linker's search paths.

## Pixel formats

Formats in the game: DXT1 (48,172), DXT5 (7,495), A8R8G8B8 (1,829), G8 (265), DXT3 (74). `decode_pixels` also supports
BC5, G16, V8U8, A1 and the float formats. Unit tests check every BC block mode against hand-computed texels.

## Export conventions

* **OBJ** (static meshes): Y and Z are swapped (Unreal is Z-up, left-handed), which also makes clockwise front faces
  counter-clockwise as OBJ expects; V is flipped. One group per section, `usemtl` = material object name.
* **PSK/PSA**: the conventions of UE Viewer's exporter, so files import like umodel's: positions mirrored on Y,
  quaternion Y and W negated (non-root bone orientations W-negated first), the first two corners of each triangle
  swapped. One point per vertex (no welding). PSA bone names come from the AnimSet; keys are sampled once per frame.

## Limitations

* Material native data (compiled shader maps) is not decoded; only its size is reported. Material expression graphs
  are available as their objects' properties, not as a node graph.
* Animation key formats other than `AKF_ConstantKeyLerp`, and rotation codecs other than None/Float96NoW/Fixed48NoW/
  Fixed32NoW, throw `UnsupportedError` (none occur in Mirror's Edge). Version-gated layouts beyond 536 are rejected
  where they were not implemented (e.g. static mesh color streams ≥ 615, skeletal index buffers ≥ 806).
* Skeletal chunk CPU vertices (rigid/soft) are skipped; the GPU-skin vertex buffer is decoded.
* `read_model` stops after the Polys reference: BSP nodes and verts are skipped by size, and leaf hulls, leaves,
  portal nodes, the vertex buffer and lighting data after Polys are not decoded. File versions other than 536 are
  rejected for Model, and FPoly layouts < 78 or ≥ 600 for Polys.
* Fonts, terrain, physics assets, particle systems and level data are not deserialized beyond their
  properties. Audio is extracted, not decoded (Ogg Vorbis is a standard format).
* LZX-compressed bulk data (Xbox 360) is not supported.
