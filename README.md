# UnrealLib

A native C++20 library for reading Unreal Engine 3 packages, ported from the behaviour of
[UELib](https://github.com/EliotVU/Unreal-Library) (C#). The first target is **Mirror's Edge (2008), PC**.

> **Status: milestones M0�M7 done, M8 (package writer, T3D) in progress.** Package summaries, compression,
> name/import/export/depends tables, game profiles, structural validation, cross-package import resolution, UObject
> headers, tagged script properties, reflection objects, UnrealScript bytecode decoding and class decompilation, asset
> extraction, package rewriting and T3D level export are implemented and verified against all 2,426 packages of the
> Mirror's Edge PC release; the tables also against upstream UELib (differential tests). See
> [docs/development.md](docs/development.md#milestones).

## Features

* Bounds-checked binary serialization (`BinaryReader`/`BinaryWriter`, bit streams, CompactIndex, FString with
  lossless Windows-1252/UTF-16 handling) with allocation limits.
* UE3 package summary parsing and byte-identical re-serialization (file versions 249–766, little and big endian).
* Fully compressed packages: LZO1X (built-in decoder) and zlib.
* Name, import, export and depends tables; object paths, class names and lookup.
* Game profiles with detection (`mirrorsedge-pc`, `ue3-generic`) and explicit selection.
* Structural validation (offsets, references, outer cycles, overlapping export data).
* `Linker`: package search paths, lazy loading, import resolution across packages (including seek-free forced
  exports), class hierarchy queries, UObject header parsing.
* Script properties: property tags, typed values (structs, arrays, enums, names, objects, delegates), lossless
  rewriting, type information from the game's own script packages (`ReflectionSchema`).
* Reflection objects (`UClass`, `UStruct`, `UFunction`, `UState`, `UEnum`, `UConst`, properties, text buffers) and an
  UnrealScript bytecode decoder with the Mirror's Edge token map.
* Decompiler for whole classes (declaration, structs, enums, consts, variables, replication, `defaultproperties`), functions and states: expressions with operator precedence, if/else, loops (`for`, `while`,
  `do`/`until`, `foreach`), `switch`, break/continue; native function table built from the game itself.
* Differential tests against upstream C# UELib (opt-in, needs the .NET SDK).
* Assets: textures (DXT1/3/5, BC5, ARGB, ... → PNG/TGA/DDS, including mips streamed from other packages), sounds (Ogg
  Vorbis), static meshes (OBJ), skeletal meshes (PSK), animations (PSA), material metadata (JSON).
* Package writer (`PackageModel`, `write_package`): edit names/imports/exports/properties and re-lay out the file,
  optionally zlib/LZO compressed.
* T3D: parser, serializer, validation and level/object export, including brush geometry ([docs/t3d.md](docs/t3d.md)).
* Tools: `UnrealPackageInspector` (text or normalized JSON), `UnrealPackageValidator`, `UnrealScriptDecompiler` and
  `UnrealPackageExtractor`.

## Quick start

With [uv](https://docs.astral.sh/uv/) installed, `./build.sh` (Linux/macOS/Git Bash), `build.bat` (cmd) or
`./build.ps1` (PowerShell) opens an interactive menu (configure, build, test, clean, docs, tools); `build.bat all`
configures, builds and tests the default preset. The wrappers run `uelib.py`, which sets up the Visual C++
environment itself. See
[docs/development.md](docs/development.md#project-manager-script). Plain CMake works as well:

```powershell
cmake --preset msvc-x64-release
cmake --build --preset msvc-x64-release
ctest --preset msvc-x64-release

build/msvc-x64-release/tools/UnrealPackageInspector.exe "<install>/TdGame/CookedPC/Core.u" --exports --limit 20
build/msvc-x64-release/tools/UnrealPackageInspector.exe "<map>.me1" --imports -s "<install>/TdGame/CookedPC"
build/msvc-x64-release/tools/UnrealPackageValidator.exe "<install>/TdGame/CookedPC" -r -q
```

```cpp
#include <UnrealLib/UnrealLib.hpp>

UnrealLib::LoadOptions options;
options.profile = "Mirror's Edge PC";          // optional; auto-detected otherwise

UnrealLib::Package package;
package.load("Core.u", options);                // throws UnrealLib::Error on failure

for (std::size_t i = 0; i < package.exports().size(); ++i) {
    auto index = UnrealLib::ObjectIndex::from_export(i);
    fmt::print("{} {}\n", package.class_name(index), package.object_path(index));
}

if (auto object = package.find_export("Object")) {
    auto bytes = package.export_data(object->export_index()); // raw serialized object data
}

auto diagnostics = UnrealLib::validate_package(package);
```

### Using it from another CMake project (CPM.cmake)

```cmake
CPMAddPackage(NAME UnrealLib GIT_REPOSITORY <url> GIT_TAG <tag>)
target_link_libraries(my_tool PRIVATE UnrealLib::UnrealLib)
```

Tests, tools and examples are off by default for consumers; only fmt, spdlog and zlib are pulled in.

## Documentation

* [Architecture & upstream inventory](docs/architecture.md)
* [Supported versions & Mirror's Edge verification](docs/supported_versions.md)
* [Serialization](docs/serialization.md)
* [Package format](docs/package_format.md)
* [Decompiler](docs/decompiler.md)
* [Assets](docs/assets.md)
* [Development guide (build options, testing, milestones, limitations)](docs/development.md)
* API reference: configure with `-DUNREALLIB_BUILD_DOCS=ON` and build `unreallib_docs` (Doxygen).

## Dependencies

All fetched with [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake), versions pinned in `cmake/Versions.cmake`:
fmt, spdlog, zlib (runtime); nlohmann/json, CLI11 (tools); GoogleTest, RapidCheck, ApprovalTests.cpp (tests); Google Benchmark (optional).

## Legal

UnrealLib contains no game data. Do not commit or redistribute game packages; fixture tests read them from a local,
legally obtained installation (`-DUNREALLIB_MIRRORSEDGE_DIR=...`). Unreal Engine and Mirror's Edge are trademarks of
their respective owners. UnrealLib is not affiliated with Epic Games or Electronic Arts.

Licensed under the MIT license (see [LICENSE](LICENSE)); upstream UELib is also MIT licensed.
