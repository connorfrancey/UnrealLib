# Supported versions

## Summary

| Profile id       | Game                | Platform | File version | Licensee | Engine build | Status |
|------------------|---------------------|----------|--------------|----------|--------------|--------|
| `mirrorsedge-pc` | Mirror's Edge (2008)| PC       | 536          | 43       | 3716         | **Verified** (package header & tables) |
| `dishonored-pc`  | Dishonored (2012)   | PC       | 801          | 30       | 9411         | Packages, headers, properties, reflection, linking checked (see below) |
| `ue3-generic`    | any UE3             | any (LE) | 249 – 868    | any      | any          | Fallback; checked on A Story About My Uncle (UDK 868) |

Not supported (rejected with `UnsupportedError`): UE1/UE2 (file version < 249), file versions above 868 (the last
UE3/UDK version), UE4/UE5 (negative legacy version).

## Other UE3 games (late UE3 / UDK)

Checked with the corpus sweeps pointed at two more Steam installations (`UNREALLIB_MIRRORSEDGE_DIR=<CookedPC>`):

| | A Story About My Uncle (UDK, 868/0, `ue3-generic`) | Dishonored (801/30, `dishonored-pc`) |
|---|---|---|
| Packages load and validate | 30 / 30 | 470 / 470 (5.4 GB) |
| Object headers | 91,504, 0 failures | 1,337,305, 0 failures |
| Tagged properties | 89,060 objects; the 77 failures are objects whose UT base classes are not shipped | 1,334,321 objects, 6,379,669 properties, byte-exact round trip; 334 objects with unshipped classes |
| Reflection + bytecode | 12,801 scripts, 487,178 tokens, 0 failures | 4,785 scripts, 0 failures |
| Import resolution | 6,046 exports, 278 intrinsic; the rest point at packages the game does not ship | 105,707 exports, 3,136 intrinsic (with `index_forced_exports()`); 7 to editor/unshipped packages, 48 objects absent from the forced copies |
| Textures | all 874 decode (430 from `.tfc` texture file caches) | not yet swept |
| Sounds | all 579 SoundNodeWaves read, 579 Ogg payloads extracted | not yet swept |
| Animations | all 358 AnimSequences decode (309 per-track, 46 variable-key); frame 0 of 25,913 tracks matches RawAnimData | not yet swept |
| Meshes | all 85 StaticMeshes (34,608 vertices) and 18 SkeletalMeshes (81,225 vertices) read, native data consumed exactly | not yet swept (801 mesh layouts are not supported) |
| Package rewrite | 30 / 30 round-trip | not yet swept |

What made these work (all gated by file version or profile, so Mirror's Edge is unchanged):

* summary TextureAllocations (≥ 767); supported file versions now 249–868;
* UClass DontSortCategories (≥ 603), AutoCollapseCategories (> 670), bForceScriptOrder (≥ 749), ClassGroups
  (≥ 789), NativeClassName (≥ 813), DLLBindName (≥ 655), and Dishonored's own UClass order (`dishonored-pc`
  profile, `ReflectionRules::dishonored_class_layout`);
* the DominantLightShadowMap array that Dominant{Directional,Spot}LightComponent serialize before UObject data (≥ 617
  / ≥ 682);
* Texture2D mobile mip caches (PVRTC ≥ 674, ATITC + Flash ≥ 857, ETC ≥ 864) and `.tfc` texture file caches
  (`Linker::find_texture_file_cache`);
* imports of members of native-only classes classify as intrinsic;
* seek-free games without the original package files: `Linker::index_forced_exports()` resolves imports through
  forced exports in other packages;
* SoundNodeWave Wii U / iPhone / Flash payloads (≥ 845 / 851 / 854, from upstream);
* AnimSequence RawAnimData (≥ 577) and the AKF_VariableKeyLerp and AKF_PerTrackCompression key formats, with the
  IntervalFixed32NoW, Float32NoW and translation Fixed48NoW / IntervalFixed32NoW codecs (layouts from UE Viewer);
* the 868 StaticMesh layout (compact kDOP tree, section platform flag, color stream, adjacency index buffer and the
  late trailing fields) and SkeletalMesh layout (multi-size index containers with 16- or 32-bit indices, chunk vertex
  colors, the ≥ 592 GPU skin vertex order, vertex colors, adjacency indices and the late trailing fields), worked
  out from the packages themselves: upstream UELib has no mesh serializers. A few trailing fields are kept
  unidentified (see the `StaticMesh` / `SkeletalMesh` API docs).

Not supported yet: StaticMesh layouts of file versions 770–867 and SkeletalMesh layouts of 592–867 (only 536 and 868
are verified; the readers reject the versions in between with `UnsupportedError`), non-empty skeletal
VertexInfluences, packed (console) skeletal positions.

## Mirror's Edge PC — verification record

### Candidate values vs. evidence

| Value                 | Candidate | Upstream UELib                                   | Real packages (Steam release)                     |
|-----------------------|-----------|--------------------------------------------------|---------------------------------------------------|
| File version          | 536       | `[Build(536, 43)] MirrorsEdge` (UnrealPackage.cs) | 2426 / 2426 packages                               |
| Licensee version      | 43        | same                                             | 2425 / 2426 (the exception is a modded file)       |
| Engine build          | 3716      | not used for detection                           | 2426 / 2426                                         |
| Cooker version        | —         | —                                                | 60 for 2399 packages, 0 for 27 (editor-saved/modded) |
| Compression           | —         | —                                                | LZO (flag `0x2`) for 1553 packages, none for 873   |
| Generations           | —         | —                                                | exactly 1 in every package                          |
| Byte order            | —         | —                                                | little-endian everywhere                            |

Corpus: `TdGame/CookedPC` of a Steam installation — 20 `.u`, 1727 `.upk`, 679 `.me1` files, 6.5 GB. The
installation used during development contains a few modified packages (`Engine.u`, `TdGame.u` are stored uncompressed;
`MirrorsEdgeConsole.u` has licensee 0 and is detected as `ue3-generic`). This is useful: it exercises both the
compressed and uncompressed code paths.

### What is verified

| Feature                                   | Evidence                                                                 |
|-------------------------------------------|--------------------------------------------------------------------------|
| Package summary layout                    | All 54 top-level packages: re-serialized summary is byte-identical (`SummaryRewriteIsByteIdentical`); Core.u header bytes are pinned in a unit test |
| Compressed chunk table order              | Core.u: `UncompressedOffset == NameOffset`, `CompressedOffset == end of summary` |
| LZO1X decompression                       | All 1553 compressed packages decompress to exactly the declared sizes and their tables parse (`FullCorpusSweep`) |
| Name / import / export tables             | All 2426 packages load and validate with zero errors; Core.u tables re-serialize byte-identically |
| Depends table                             | Parsed for all packages; all indices valid                                |
| Object paths / outer chains               | Every export path in all 2426 packages resolves without cycles or invalid references |
| Game profile detection                    | 2425 packages → `mirrorsedge-pc`, 1 → `ue3-generic`                        |

| Import resolution | All 2426 packages: 179,461 imports → exports, 6,547 intrinsic (native-only classes and their members), 8,895 packages; the 90 unresolved imports are all in the three modded packages (references to content that is not installed) (`FullCorpusLinking`) |
| UObject header | 2,525,148 exports: state frame / component template / NetIndex parsed and followed by a well-formed property tag (`FullCorpusLinking`) |
| Agreement with upstream UELib (`8236c39`) | Differential tests over all 2426 packages: every summary identical; names, imports, exports and depends identical for all 873 uncompressed packages (upstream cannot read compressed tables); upstream's build detection (`MirrorsEdge`) agrees with profile detection for every package |

Commands used:

```powershell
UnrealPackageValidator.exe "<install>/TdGame/CookedPC" -r -q
$env:UNREALLIB_FIXTURE_SWEEP=1; ctest --preset msvc-x64-debug -R FullCorpusSweep
cmake --preset msvc-x64-release -DUNREALLIB_BUILD_DIFFERENTIAL=ON -DUNREALLIB_DIFFERENTIAL_RECURSIVE=ON
ctest --preset msvc-x64-release -R Differential
```

### Mirror's Edge specifics taken from upstream

* Bytecode: token `0x4F` is unresolved (`TokenKind::Unresolved`) for Mirror's Edge, as in upstream's
  `DefaultEngineBranch.cs`.
* Bytecode: from version 536 the struct-member context token serializes a 16-bit property type (upstream
  `ContextTokens.cs`: "first attested with Mirrors Edge (536)").

Both are used by the bytecode decoder; the differential tests confirm all 439,779 tokens of the script packages
match upstream's.

### Beyond the package tables

Properties, reflection data, bytecode, the decompiler, assets, T3D and package writing are verified as well; see the
[compatibility report](compatibility.md) for the per-feature results.
