# Architecture

UnrealLib is a native C++20 re-implementation of the package-handling parts of
[UELib](https://github.com/EliotVU/Unreal-Library) (Eliot.UELib, C#), targeting Mirror's Edge PC (UE3, 536/43) first.
Upstream is the behavioural reference; where real packages disagree with upstream, the packages win and the deviation
is documented (see "Deviations from upstream" below).

## Module layering

```
                        UnrealLib::UnrealLib  (INTERFACE umbrella)
                                   │
   ┌───────────────────────────────┼─────────────────────────────── planned ────────────────────────┐
   │                               │                                                                  │
 Linker ──► Objects ──► Package ──► Names ──► Serialization ──► Utilities ──► Core
                                                               planned: Properties, Reflection,
   │                        │                            ▲          Bytecode, Decompiler, Assets, T3D, Writer
   └────────────────────────┴────────────────────────────┘
```

| Module | Target | Status | Contents |
|--------|--------|--------|----------|
| Core | `UnrealLib::Core` | implemented | `Error` hierarchy, `Diagnostics`, logging facade (spdlog backend, pluggable sink), `Guid`, version |
| Utilities | `UnrealLib::Utilities` | implemented | Windows-1252 / UTF-16 / UTF-8 conversion, case-insensitive hashing |
| Serialization | `UnrealLib::Serialization` | implemented | `BinaryReader`/`Writer`, `BitReader`/`Writer`, `Archive`s, CompactIndex, FString, compression (zlib, LZO1X), contexts, limits |
| Names | `UnrealLib::Names` | implemented | `FName`, `NameEntry`, `NameTable` |
| Package | `UnrealLib::Package` | implemented | summary, compressed chunks, import/export/depends tables, `GameProfile` registry, `Package`, validator |
| Objects | `UnrealLib::Objects` | implemented (header) | UE3 object flags, UObject header (state frame, component template, NetIndex) |
| Linker | `UnrealLib::Linker` | implemented | package search paths, lazy loading, import resolution (incl. forced exports), class hierarchy (`is_a`), header input |
| Properties / Reflection | — | M4–M5 | property tags/values, UField/UStruct/UClass/UFunction/UState/UEnum/UConst |
| Bytecode / Decompiler | — | M5–M6 | token decoding → IR → control flow → UnrealScript |
| Assets | — | M7 | textures, sounds, meshes, materials, animations |
| T3D / Writer | — | M8 | T3D, package writing |

Dependencies are strictly downward; every module is a static library with an explicit source list
(`src/CMakeLists.txt`) created by `unreallib_add_module()`.

Third-party runtime dependencies: fmt (public, used in `Log.hpp`), spdlog (private to Core), zlib (private to
Serialization). nlohmann/json and CLI11 are used only by the tools; GoogleTest only by the tests.

## Package loading pipeline

Implemented stages (`Package::load`):

1. Read the file into memory (`read_file`, bounded by `max_file_size`).
2. Parse `PackageSummary` (version gates from the file version).
3. Select a `GameProfile` — explicit (`LoadOptions::profile`) or `GameProfileRegistry::detect()` scoring version
   range, licensee, engine build, cooker version and byte order.
4. Decompress compressed chunks into the decompressed image (`decompress_package`).
5. Parse name, import, export and depends tables.
6. (Separately) `validate_package()` checks structural consistency.

### Linking (M3)

`Linker` owns a set of packages, located through search paths (file stem → path, case-insensitive) and loaded lazily
when an import first needs them. Loading never recurses into imports, so cyclic package dependencies (Engine ↔ game
packages) and cyclic object references need no special handling; every resolution is cached.

Import resolution (UE3 semantics, verified on the corpus):

1. Walk the import's outer chain to the outermost entry, which names the package. Outers are imports or **forced
   exports** — `Package` exports with `ExportFlag::ForcedExport` that seek-free cooking places in maps as stand-ins
   for the original packages and groups (45,897 such imports in Mirror's Edge maps).
2. Walk back down, matching each element against the target package's exports by outer, name (case-insensitive,
   including the instance number) **and class**. Name alone is not enough: Core.u exports a MetaData *instance*
   called `MetaData` while the class `Core.MetaData` is intrinsic.
3. Outcomes: `Export`, `Package`, `Intrinsic` (an unexported `Core.Class` import: native-only class),
   `PackageNotFound`, `ObjectNotFound`, `ClassMismatch`, `Invalid`.

Class hierarchy queries (`Linker::is_a`) follow class → super chains across packages. They decide whether an export
is a UComponent, which changes its serialized header.

### UObject header (M3)

| Object | Serialized before the script properties |
|--------|------------------------------------------|
| class default object (`ObjectFlag::ClassDefaultObject`) | NetIndex |
| component (class derives from `Core.Component`) | [StateFrame] TemplateOwnerClass [TemplateName if inside a CDO] NetIndex |
| other | [StateFrame if `HasStack`] NetIndex |

The CDO rule comes from upstream's `UObject.Load` → `DeserializeClassDefault` and was confirmed on real data
(`Default__ActorComponent` = NetIndex then a `TickGroup` ByteProperty tag). In cooked Mirror's Edge packages NetIndex
equals the export's 0-based index. Verified on all 2,525,148 exports of the corpus: after the header, every export
starts with a well-formed property tag (valid name and a real property type, or `None`).

Planned stages (M4+): typed objects, property deserialization, UStruct/UClass data, lazy object graph.

## Design decisions

* **Exceptions for failures, diagnostics for findings.** Malformed input that prevents a result throws an
  `UnrealLib::Error` subclass carrying an offset. Suspicious-but-loadable conditions are `Diagnostics`.
* **Value-type tables, index-based references.** Imports/exports are plain structs; references are `ObjectIndex`
  values. This keeps tables trivially copyable/serializable and gives cycle-safe navigation (`object_path` detects
  cycles). The object graph (M3) will use stable handles into these tables.
* **Game specifics in profiles.** `GameProfile` currently carries identification data and compression methods.
  Serialization rules for higher layers (bytecode token maps, property quirks, asset formats) will be added to the
  profile as those layers land, rather than as `if (game == X)` checks in serializers (which is how upstream does it
  with `#if GAME` blocks).
* **Explicit registries, no static self-registration.** `GameProfileRegistry::builtin()` is a function-local static;
  future class/opcode/asset registries will follow the same pattern.
* **Lossless round trips.** Readers keep on-disk details (string encodings, raw name numbers, raw object flags) so the
  writer (M8) can reproduce original bytes. Byte-identical rewrite of Mirror's Edge headers and tables is tested.

## Upstream UELib inventory

Source: `EliotVU/Unreal-Library` @ `8236c39` (334 C# files). Projects: `src` (Eliot.UELib), `Eliot.UELib.Decompiler`,
`Eliot.UELib.Decompiler.T3D`, `CLI`, `Benchmark`, `Test`.

| Upstream area | Key types | Port status / plan |
|---------------|-----------|--------------------|
| Package | `UnrealPackage`, `PackageFileSummary`, `GameBuild` (build table with `[Build(ver, lic)]`), `UnrealPackage.Deserialize` | **Ported** (summary, tables). `GameBuild`/`EngineBranch` → `GameProfile` |
| Streams | `UnrealStream.cs`: `UnrealReader`, `UnrealWriter`, `IUnrealStream`, `UPackageStream` | **Ported** as `BinaryReader`/`BinaryWriter` + context |
| Tables | `Core/Tables`: `UNameTableItem`, `UImportTableItem`, `UExportTableItem`, `UGenerationTableItem`, `UPackageIndex`, `UObjectThumbnailTableItem` | **Ported** except thumbnails |
| Compression | `UnrealPackageCompression.cs` (chunk structs only; upstream does not decompress whole packages) | **Ported and extended** (LZO1X + zlib decompression) |
| Version gates | `Branch/PackageObjectLegacyVersion.cs` | Ported subset → `LegacyVersion` |
| Branches | `Branch/EngineBranch`, `DefaultEngineBranch` (token maps), `UE2/*`, `UE3/*` per-game | Mirror's Edge branch data captured in docs; token map → M5 |
| Objects | `Core/Classes`: `UObject`, `UField`, `UStruct`, `UClass`, `UState`, `UFunction`, `UEnum`, `UConst`, `UScriptStruct`, `UTextBuffer`, `UMetaData`, `UComponent`, `UPackage` | M3–M5 |
| Properties | `Core/Classes/Props`: 19 `U*Property` types; `UDefaultProperty` (tag parsing) | M4 |
| Types | `Core/Types`: `UBulkData`, `UArray`, `UMap`, `UName`, math structs | M4/M7 |
| Bytecode | `Core/Tokens/*`, `UnrealTokens.cs`, `ByteCodeDecompiler.cs` (decoding and decompiling interleaved) | M5 (decoder) and M6 (decompiler), deliberately split |
| Decompiler | `*Decompiler.cs` partial classes, `Eliot.UELib.Decompiler*` | M6, M8 (T3D) |
| Engine assets | `Engine/Classes`: 44 classes (`UTexture2D`, `USoundNodeWave`, `UFont`, `UModel`, `UPolys`, …) | M7, Mirror's Edge formats first |
| Encoding | `UnrealEncoding` (Windows-1252), `Decoding/` (crypto for some games) | 1252 ported; crypto not needed for Mirror's Edge |
| Services | `LibServices` logging | → `Log.hpp` |

Not needed for the initial target: UE1/UE2 branches, UE4 summary handling, console-specific branches, per-game
crypto decoders.

## Deviations from upstream

| Topic | Upstream | UnrealLib | Reason |
|-------|----------|-----------|--------|
| FCompressedChunk field order | `Deserialize` reads UncompOffset, CompOffset, UncompSize, CompSize | UncompOffset, UncompSize, CompOffset, CompSize | Matches upstream `Serialize` and verified Mirror's Edge data |
| Fully compressed packages | not decompressed | decompressed (LZO1X, zlib) | needed to read cooked Mirror's Edge packages |
| Export ObjectFlags | 32-bit halves swapped | raw on-disk value | raw value matches UE3 `RF_*` constants; lossless |
| FName number | stored as `number - 1` | stored as on disk | lossless |
| CompactIndex overflow | silently overflows | `SerializationError` | robustness |
| Game-specific logic | `#if GAME` blocks inside serializers | `GameProfile` | maintainability |
| Logging | `Console.WriteLine` in deserializers | `Log.hpp`, default level Warning | library hygiene |
| FName display | `Name_N` only when on-disk number > 1 (number 1 shows as `Name`) | `Name_<number-1>` whenever number > 0 (UE3 `FName::ToString` behaviour) | correctness; differential tests compare raw numbers |

| Component template data | read for every UComponent incl. CDOs when the class is registered; class-name heuristic otherwise | CDOs never; other components by class hierarchy | data-verified; avoids upstream's `EndsWith("Component")` heuristic |
| Import resolution | not cross-package (objects are created per package) | `Linker` resolves across packages, incl. forced exports | feature |

All rows except "FName display", "Logging", "Component template data" and "Import resolution" are asserted or
observable in `tests/Differential`.

## Testing architecture

See [development.md](development.md#testing).
