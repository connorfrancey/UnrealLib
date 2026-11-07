# Compatibility report

What UnrealLib supports compared with upstream C# UELib (`EliotVU/Unreal-Library` @ `8236c39`; the upstream column
reflects that commit's source), how each part was
verified on Mirror's Edge (PC, Steam: `TdGame/CookedPC`, 2,426 packages, 6.5 GB), and where behaviour differs on
purpose. The numbers come from the corpus sweeps (`UNREALLIB_FIXTURE_SWEEP=1`) and the differential tests
(`UNREALLIB_BUILD_DIFFERENTIAL=ON`) described in [development.md](development.md).

## Target coverage

| | Status |
|---|---|
| Mirror's Edge PC (file version 536, licensee 43) | Verified on the whole corpus (profile `mirrorsedge-pc`) |
| Other UE3 games, file versions 249–766, little-endian | Accepted by the generic profile (`ue3-generic`); version gates follow upstream's `PackageObjectLegacyVersion`, but nothing outside Mirror's Edge has been verified |
| UE1/UE2 (< 249), late UE3 (≥ 767), UE4/UE5 | Rejected with `UnsupportedError` |
| Big-endian (console) packages | Archive layer and summary round trips only; no profile accepts them |
| LZX compression (Xbox 360) | Not supported |

## Feature matrix

| Area | Upstream UELib | UnrealLib | Verification on Mirror's Edge |
|------|----------------|-----------|-------------------------------|
| Package summary | yes | yes | All 2,426 summaries identical to upstream's; re-serialized summaries byte-identical |
| Compressed packages (LZO/zlib) | chunk records only, no decompression | full decompression (LZO1X, zlib) and LZO/zlib compression | All 1,553 compressed packages decompress to their declared sizes |
| Name / import / export / depends tables | yes | yes | Identical to upstream's for all 873 uncompressed packages (upstream cannot read compressed tables); all 2,426 validate |
| Thumbnails, GUID tables (≥ 584 / ≥ 623) | yes | detected, not parsed | n/a (Mirror's Edge is 536) |
| Cross-package import resolution | no (per-package objects) | `Linker` | 179,461 imports resolve; the 101 unresolved point at content missing from the modded install |
| UObject header (state frame, component template, NetIndex) | yes | yes | 2,525,148 exports |
| Tagged properties | yes (values as display text) | typed values, byte-exact write-back, text format and parse | 9,656,419 properties re-serialize byte-identically; with a `ReflectionSchema` all 1,639,154 arrays decode; tags of 58,845 script-package objects match upstream's; upstream fails on 381 objects whose properties UnrealLib reads |
| MapProperty values | yes | kept as raw bytes | none occur in the corpus |
| Reflection (UField…UClass, UEnum, UConst, UTextBuffer) | yes | yes | Every reflection export consumes its data exactly |
| Bytecode decoding | yes (interleaved with decompiling) | separate decoder | 13,770 scripts / 477,343 tokens end exactly at ScriptSize; 439,779 tokens of 12,097 scripts identical to upstream's (positions, opcodes, native indices) |
| UnrealScript decompiler | yes | yes (`UnrealScriptDecompiler`) | 13,244 functions/states (13,241 fully structured); 2,338 whole classes without placeholders ([decompiler.md](decompiler.md)) |
| Textures | reads Texture2D data; no pixel decoding | Texture2D family, TextureCube; PNG/TGA/DDS | 58,960 parse; 57,835 decode at their largest mip, including mips streamed from other packages ([assets.md](assets.md)) |
| Sounds | yes | SoundNodeWave extraction (Ogg Vorbis) | 78,049 parse, 78,048 extract (1 has no data) |
| Static meshes | no reader | yes (OBJ) | 19,007 parse (35.1 M vertices) |
| Skeletal meshes | no reader | yes (ActorX PSK) | 742 parse |
| Animations | no reader | AnimSequence/AnimSet (ActorX PSA) | 6,938 sequences decode |
| Materials | object data | parameters, textures, expressions (JSON) | 29,392 read |
| Brushes (Polys, Model) | yes | Polys read/write; Model up to its Polys reference | All 9,392 of each; `write_polys` byte-identical |
| BSP tree, terrain, fonts, particles, physics assets | some (UModel, UFont, …) | raw bytes | not decoded |
| T3D export | yes (`Eliot.UELib.Decompiler.T3D`, brushes included) | levels and objects, brushes included | 680 levels, 152,965 actors, 7,958 brushes; exact text round trip |
| T3D import into packages | no | existing objects (properties, brush polygons) | Every level's unmodified export re-imports with 0 changes (338,507 objects); edits round-trip through write → reload → export |
| Package writing | summary/table serializers (`PackageSerializer`) | `PackageModel` + `write_package` (uncompressed, LZO, zlib) | All 873 uncompressed packages rewritten byte-identically; all 1,553 compressed ones reload with identical tables and export data |
| Package validation | no | `validate_package`, `UnrealPackageValidator` | 2,426 packages, 0 errors |
| UE1/UE2/UE4 branches, per-game crypto | yes (many games) | no | out of scope for the initial target |

## Behavioural differences from upstream

The rows of [architecture.md](architecture.md#deviations-from-upstream) still hold (chunk field order, raw object
flags, FName numbers, CompactIndex overflow checks, `GameProfile` instead of `#if GAME`, quiet logging, component
templates, cross-package resolution). Found or confirmed by the M9 differential tests:

| Topic | Upstream | UnrealLib |
|-------|----------|-----------|
| Delegate property tags | the `PropertyType` enum aliases `DelegateProperty` to `StringProperty`, so delegates are reported as strings | stored type name kept (`DelegateProperty`) |
| ScriptStruct properties | `Properties` is replaced by the struct defaults | object properties (`read_properties`) and struct defaults (`read_field`) kept apart |
| Token storage positions | `StoragePosition` one byte late for two-byte native opcodes | exact storage and memory offsets |
| Corrupt or unusual objects | 381 Mirror's Edge objects fail to deserialize (e.g. an enum-parsing exception in `UDefaultProperty.DeserializeTagUE3`) | read |
| Malformed input | exceptions of many types, some swallowed with partial results | only `UnrealLib::Error`, enforced by fuzzing ([development.md](development.md)) |

## Robustness

Seven fuzz targets (package, serialization, properties, bytecode, compression, assets, text) run on every test run
(seeds, regression inputs, 2,000 mutations each) and as libFuzzer executables with AddressSanitizer. A
60-second libFuzzer run per target (about 5.5 million inputs) and 50,000 replay mutations per target are clean. The
one library bug they found — an endless loop on cyclic export outers — is fixed and kept as a regression input.

## Performance

Reference numbers are in [development.md](development.md) (Benchmarks). On a desktop CPU with an MSVC Release
build: the largest Mirror's Edge map loads in about 0.18 s, LZO decompresses at about 4 GiB/s, and tagged properties
are read at about 1.8 million objects per second.
