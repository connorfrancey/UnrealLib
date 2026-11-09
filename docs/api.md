# API guide and review

This page states the conventions every public header follows and records the M9 API review: what was checked, what
was changed, and what is intentionally left as it is. The reference documentation is generated with Doxygen
(`uv run uelib.py docs`).

## Layout

| Header group | Namespace | Purpose |
|--------------|-----------|---------|
| `Core/` | `UnrealLib` | `Error` hierarchy, `Diagnostics`, `Guid`, logging facade, library version |
| `Utilities/` | `UnrealLib::Text` | case-insensitive comparison and hashing, Windows-1252/UTF-8 helpers |
| `Serialization/` | `UnrealLib` | `BinaryReader`/`BinaryWriter`, `SerializationContext` and limits, CompactIndex, FString, compression |
| `Names/`, `Package/`, `Objects/` | `UnrealLib` | name table, summary, import/export/depends tables, `Package`, game profiles, validation, UObject header |
| `Properties/` | `UnrealLib` | property tags, typed values, `PropertySchema`, reading/writing/formatting/parsing values |
| `Bytecode/`, `Reflection/`, `Decompiler/` | `UnrealLib` | token decoding, UField/UStruct/UClass data, UnrealScript decompiler |
| `Linker/` | `UnrealLib` | cross-package resolution (`Linker`, `ObjectRef`), `ReflectionSchema` |
| `Assets/` | `UnrealLib::Assets` | textures, sounds, meshes, animations, materials, brushes |
| `Writer/` | `UnrealLib` | `PackageModel`, `write_package`, `replace_properties` |
| `T3D/` | `UnrealLib::T3D` | T3D document model, parser, serializer, validation, export and import |

`<UnrealLib/UnrealLib.hpp>` includes everything. Each module is also a CMake target (`UnrealLib::Package`,
`UnrealLib::T3D`, …) that links only what it depends on (see [architecture.md](architecture.md)).

## Conventions

* **Errors.** Every failure is an exception derived from `UnrealLib::Error`, with an `ErrorCode` and, where it
  applies, the byte offset of the problem. No other exception type escapes for any input, except `std::bad_alloc`
  when memory really runs out within the configured limits. The fuzz targets check this on every test run. `@throws`
  documents specific subclasses and conditions; functions without a `@throws` line follow the general contract (see
  `Core/Error.hpp`). Findings that do not stop the operation (raw-kept values, validation warnings, import skips) are
  reported as `Diagnostics`.
* **Untrusted input.** All readers are bounds-checked. Allocations driven by the input are capped by
  `SerializationLimits` (`LoadOptions::limits`), and loops over input-controlled chains (outer, super, class
  hierarchy) are bounded or cycle-checked.
* **Indices and references.** `ObjectIndex` uses the on-disk encoding: 0 is null, positive values are exports
  (`from_export(i)`), negative values are imports. `ObjectRef` is a (package, index) pair; it does not own the
  package.
* **Ownership and lifetime.** A `Linker` owns the packages it loads or is given (`load`, `add`). Returned
  `const Package&` references and `ObjectRef`s stay valid as long as the linker exists. `Package` owns its bytes, and
  `export_data()` spans point into them. `PackageModel` is an independent, editable copy.
* **Values.** Readers return plain structs (`Texture2D`, `Polys`, `FieldObject`, `ObjectProperties`, …) by value.
  Everything is serializable again: property lists, Polys and package models write back byte-identically when
  unmodified.
* **`[[nodiscard]]`.** Used on functions whose result is the reason for calling them. Exceptions by design:
  `BinaryReader::read_*` (often called only to skip data), and `Linker::load`/`add`, `PackageModel::add_import`/
  `add_export` and `T3D::import_document`, which are called for their side effects.
* **Names in text.** `FName`s are shown as `Base` or `Base_<number-1>` (UE3 `FName::ToString`). `PackageModel::make_name`
  and the text parsers accept that form back.
* **Thread safety.** Nothing is internally synchronized. A loaded `Package` is immutable, so concurrent `const`
  access to it is safe. `Linker` and `ReflectionSchema` keep caches, even behind `const` member functions, so each
  thread needs its own (or external locking). `set_log_level` / `set_log_sink` change global state.
* **Versioning.** 0.x: minor versions may change the API (the CMake package uses `SameMinorVersion` compatibility).
  `library_version` / `library_version_string` (`Core/Version.hpp`) report the version.

## Review (M9)

Checked:

| Check | Result |
|-------|--------|
| Every public header compiles on its own and twice (include guards) | All 49 headers; enforced by the `unreallib_header_check` target in the test build |
| Public headers include only standard headers, UnrealLib headers and fmt (the only public dependency) | Yes; spdlog and zlib stay private |
| Internal helpers out of public headers | Yes; `detail` code lives in `src/` (e.g. `MeshReading.hpp`, `Lzo1x.hpp`) |
| `[[nodiscard]]` on value-returning functions | Added to `Image::create` and `encode_compact_index`; the remaining omissions are the intentional ones listed above |
| Error documentation | The contract is stated once in `Core/Error.hpp` instead of repeating `@throws Error` on about 70 read/write functions |
| Installed package usable from another project | `Install.Consumer` test (`find_package(UnrealLib)`, single module and umbrella target) |
| Doxygen | 0 warnings |

Intentionally unchanged:

* **No shared library build.** Static libraries only. The API is not yet ABI-stable: it exposes `std::` types and
  aggregate structs.
* **Aggregates with public fields** (table entries, decoded assets, T3D blocks). They are data records meant to be
  read and edited directly, as the writer and T3D import do. Invariants are checked when the data is used:
  `PackageModel::validate`, `write_package`, and the readers.
* **Exceptions rather than `expected`-style results.** Corrupt input is exceptional for a package library, and a
  single contract checked by fuzzing is simpler for callers than mixed error channels.
