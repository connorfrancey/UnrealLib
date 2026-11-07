# Development guide

## Requirements

* CMake ≥ 3.25, Ninja
* MSVC 2022+ (v143+) or clang-cl; tested with MSVC 14.50 (x64, x86) and LLVM clang-cl (x64)
* Network access on first configure (CPM.cmake downloads dependencies). Set `CPM_SOURCE_CACHE` to share downloads.
* Optional: Doxygen (API docs), ccache (compiler cache).

## Project manager script

`uelib.py` wraps the common tasks. It is a single-file [uv](https://docs.astral.sh/uv/) script (PEP 723
inline metadata, dependencies locked in `uelib.py.lock`): `uv run` installs click, rich and questionary into a
cached, isolated environment — no virtualenv to manage.

Root-level wrappers forward all arguments to it and return its exit code: `build.sh` (POSIX sh: Linux, macOS, Git
Bash), `build.bat` (cmd) and `build.ps1` (PowerShell). They keep the caller's working directory, so relative paths
work, and print installation instructions when uv is missing. The examples below work the same with `build.bat …`.

```powershell
uv run uelib.py                      # interactive menu (questionary)
uv run uelib.py doctor               # check tools, Visual Studio installations, fixtures
uv run uelib.py config set default_preset msvc-x64-debug
uv run uelib.py config set mirrorsedge_dir "C:/…/mirrors edge/TdGame/CookedPC"
uv run uelib.py all                  # configure + build + test (default preset)
uv run uelib.py build msvc-x86-release -t unreallib_package_inspector
uv run uelib.py test clang-cl-x64-asan -R Linker
uv run uelib.py test --sweep         # include the full-corpus sweeps
uv run uelib.py test --no-log        # don't write build/logs/<preset>/ (see below)
uv run uelib.py configure --differential   # enable differential tests vs. upstream UELib
uv run uelib.py docs --open
uv run uelib.py tool inspector Core.u --exports --limit 20
uv run uelib.py clean msvc-x86-release     # or --all; only ever deletes build/<preset>
```

* **No Developer Prompt needed.** The script finds Visual Studio with `vswhere`, runs `vcvarsall.bat` for the preset's
  architecture and caches the resulting environment (`build/.uelib-cache`).
* **Several Visual Studio installations.** A build tree keeps the installation it was configured with (recorded in
  `build/<preset>/.uelib-vs` (older trees: `.manage-vs`), or inferred from the compiler path in `CMakeCache.txt`), so existing trees never switch
  toolsets. New trees use the `vs_installation` setting, or the newest installation.
* **Test logs.** Every `test` (and `all`) run writes `build/logs/<preset>/test-<yyyymmdd-hhmmss>.log`, CTest's
  console output, and a `.xml` JUnit report with every test's own output (the corpus sweeps' statistics included, even
  when they pass). The newest run is also copied to `latest.log`/`latest.xml`; the last 20 runs per preset are kept.
  Logs are written for failed runs too, and CTest runs with `--output-on-failure`. `--no-log` turns this off.
* Settings live in `.uelib.json` (git-ignored): `default_preset`, `mirrorsedge_dir`, `jobs`, `vs_installation`.
* To update the pinned dependencies: edit the script header, then `uv lock --script uelib.py`.

## Building

Presets put each configuration in `build/<preset>`. From a *Developer* command prompt / PowerShell for the matching
architecture (presets use `architecture.strategy = external`, i.e. the environment selects x64/x86):

```powershell
cmake --preset msvc-x64-debug
cmake --build --preset msvc-x64-debug
ctest --preset msvc-x64-debug
```

Presets: `msvc-x64-debug`, `msvc-x64-release`, `msvc-x86-debug`, `msvc-x86-release`, `clang-cl-x64-debug`,
`clang-cl-x64-asan`, `msvc-x64-asan`.

| Option | Default | Meaning |
|--------|---------|---------|
| `UNREALLIB_BUILD_TESTS` | top-level | GoogleTest suite |
| `UNREALLIB_BUILD_TOOLS` | top-level | `UnrealPackageInspector`, `UnrealPackageValidator`, `UnrealScriptDecompiler`, `UnrealPackageExtractor`, `UnrealT3DImporter` |
| `UNREALLIB_BUILD_EXAMPLES` | top-level | examples |
| `UNREALLIB_BUILD_BENCHMARKS` | OFF | `UnrealLibBenchmarks` (Google Benchmark; see *Benchmarks* below) |
| `UNREALLIB_BUILD_FUZZERS` | OFF | libFuzzer executables (clang/clang-cl; use preset `clang-cl-x64-fuzz`) |
| `UNREALLIB_BUILD_DOCS` | OFF | `unreallib_docs` Doxygen target |
| `UNREALLIB_USE_CCACHE` | top-level | compile through ccache if found |
| `UNREALLIB_ENABLE_ASAN` | OFF | AddressSanitizer |
| `UNREALLIB_WARNINGS_AS_ERRORS` | OFF | `/WX` / `-Werror` |
| `UNREALLIB_INSTALL` | top-level | install rules and the `UnrealLib` CMake package (also installs fmt, spdlog, zlib) |
| `UNREALLIB_MIRRORSEDGE_DIR` | empty | CookedPC directory for fixture tests |

### Using UnrealLib from another project (CPM.cmake)

```cmake
CPMAddPackage(
    NAME UnrealLib
    GIT_REPOSITORY <url-of-this-repository>
    GIT_TAG <commit-or-tag>)
target_link_libraries(my_app PRIVATE UnrealLib::UnrealLib)
```

As a subproject, tests/tools/examples/ccache default to OFF and only fmt, spdlog and zlib are requested. Dependencies
are added with CPM by name, so a parent that already added `fmt`, `spdlog` or `zlib` through CPM shares its copy
(make sure the versions are compatible: see `cmake/Versions.cmake`). `add_subdirectory()` works the same way.

### Installing and `find_package(UnrealLib)`

```sh
cmake --build --preset msvc-x64-release
cmake --install build/msvc-x64-release --prefix C:/sdk/UnrealLib
cmake --install build/msvc-x64-debug   --prefix C:/sdk/UnrealLib   # optional: Debug libraries get a "d" suffix
```

```cmake
find_package(UnrealLib 0.1 CONFIG REQUIRED)            # CMAKE_PREFIX_PATH=C:/sdk/UnrealLib
target_link_libraries(my_app PRIVATE UnrealLib::UnrealLib) # or single modules: UnrealLib::Package, UnrealLib::T3D, ...
```

The prefix receives `include/UnrealLib` (with the generated `Version.hpp`), one static library per module
(`UnrealLib<Module>[d].lib`), `lib/cmake/UnrealLib` (config, version file — `SameMinorVersion` while the version is
0.x — and exported targets `UnrealLib::<Module>` / `UnrealLib::UnrealLib`), the command-line tools in `bin`, and
fmt, spdlog and zlib with their own CMake packages (UnrealLib's static libraries link them; the config finds them
with `find_dependency`). Test, tool and benchmark dependencies are never installed. Install rules are on by default
only for top-level builds (`UNREALLIB_INSTALL`); a parent project that provides its own fmt/spdlog/zlib without
their install rules should leave it off. The `Install.Consumer` CTest test installs the build into
`build/<preset>/install-test`, then configures, builds and runs `tests/Install/Consumer` against it.

### ccache

`cmake/Ccache.cmake` sets `CMAKE_<LANG>_COMPILER_LAUNCHER` when ccache is installed (and nothing else set a launcher).
For MSVC it switches debug info to embedded `/Z7` (`CMAKE_MSVC_DEBUG_INFORMATION_FORMAT`), because ccache cannot cache
compilations that write a shared PDB. Verified: 100 % cache hits on a clean rebuild with MSVC, and Ninja header
dependency tracking intact. **ccache is not used with clang-cl**: CMake passes `-clang:-MD/-MT/-MF` dependency flags
that ccache 4.14 rejects, and forcing `/showIncludes` instead breaks Ninja's header dependency tracking.

### API documentation

```powershell
cmake --preset msvc-x64-debug -DUNREALLIB_BUILD_DOCS=ON
cmake --build --preset msvc-x64-debug --target unreallib_docs   # → build/msvc-x64-debug/docs/html
```

All public headers use Doxygen comments (`///`, `@brief`, `@param`, `@throws`).

## Testing

GoogleTest is the test framework (with GoogleMock available); tests are discovered by CTest via
`gtest_discover_tests`. Two GoogleTest add-ons, both test-only dependencies:

* **RapidCheck** (property-based testing): `RC_GTEST_PROP` tests run as ordinary GoogleTest tests on generated inputs
  and shrink a failure to a minimal counterexample. 100 cases per property by default; `RC_PARAMS="max_success=5000"`
  runs more, and `RC_PARAMS="seed=<n>"` (printed on failure) reproduces a failing run.
* **ApprovalTests.cpp** (golden master): `ApprovalTests::Approvals::verify(text)` compares output with a committed
  `approved/<File>.<Suite>.<Test>.approved.txt`. A mismatch prints a text diff (no GUI diff tool is launched) and
  writes a `.received.txt` file next to it (git-ignored); if the change is intended, rename it to `.approved.txt`
  and commit it. The test executable's `main()` is `tests/Support/TestMain.cpp`, which registers the approval
  listener with GoogleTest.

| Directory | Contents |
|-----------|----------|
| `tests/Unit/*` | per-module unit tests, parameterized where values are tabular (CompactIndex encodings, SerializeInt maxima, version gates) |
| `tests/Support` | `SyntheticPackage`: builds valid UE3 packages in memory (optionally zlib-compressed, any block size) |
| `tests/Unit/Serialization/PropertyBasedTests.cpp` | RapidCheck properties: round trips (integers, float bit patterns, both byte orders, CompactIndex, FString, bit streams, property tags) and totality on arbitrary bytes |
| `tests/Regression` | approval tests: token maps, synthetic package bytes and listing, property formatting (`approved/`) |
| `tests/Fuzz` | fuzz targets, seeds and regression inputs; `FuzzReplayTests` (every run) and libFuzzer executables (see *Fuzzing* below); `MutationTests` for the loader |
| `tests/Integration` | real Mirror's Edge fixtures (skipped unless `UNREALLIB_MIRRORSEDGE_DIR` is set) |
| `tests/Differential` | UnrealLib vs. upstream C# UELib (opt-in, needs the .NET SDK) |
| `tests/Install` | `Install.Consumer`: install + `find_package(UnrealLib)` from a separate project |

Fixture tests:

```powershell
cmake --preset msvc-x64-debug -DUNREALLIB_MIRRORSEDGE_DIR="C:/…/mirrors edge/TdGame/CookedPC"
ctest --preset msvc-x64-debug
# full corpus (~70 s in Debug):
$env:UNREALLIB_FIXTURE_SWEEP = "1"; ctest --preset msvc-x64-debug -R FullCorpusSweep
```

Game packages are copyrighted: never commit them. Header *metadata* transcribed into unit tests (Core.u's 125-byte
summary: counts, offsets, flags) is the only fixture-derived data in the repository.

### Differential tests (vs. upstream UELib)

```powershell
cmake --preset msvc-x64-release -DUNREALLIB_BUILD_DIFFERENTIAL=ON -DUNREALLIB_MIRRORSEDGE_DIR="C:/…/CookedPC"
# add -DUNREALLIB_DIFFERENTIAL_RECURSIVE=ON for the whole corpus
cmake --build --preset msvc-x64-release
ctest --preset msvc-x64-release -R Differential
```

How it works:

1. CPM downloads upstream `EliotVU/Unreal-Library` at the commit pinned in `cmake/Versions.cmake` (`UELIB_TAG`,
   `DOWNLOAD_ONLY`).
2. `dotnet build` compiles `tests/Differential/UELibDump` (C#) against it; all build output goes to the build tree
   (`--artifacts-path`). Upstream is never linked into UnrealLib.
3. CTest fixtures `Differential.UELibDump.Clean` / `.Run` write one JSON document per package in a *raw* schema
   (indices, on-disk name numbers, raw flags — no display strings, so neither side's formatting conventions can hide or
   fake a difference).
4. `UnrealLibDifferentialTests` (GoogleTest) produces the same schema with UnrealLib and compares semantically
   (`nlohmann::json` equality; mismatches are reported as JSON-patch style paths with both values).

Upstream does not decompress fully compressed packages, so for those only the summary is compared. The compressed
chunk record is compared against upstream's *known* field-order bug, so the test fails if upstream ever changes it.

For uncompressed script packages (`.u`) the dump also deserializes every object with upstream and records, per
export, its property tags (name, stored type, size, array index, struct name, bool value) and, for structs with
bytecode, the script's storage size and every token as (memory position, opcode — or `0x10000 +` native index for
native calls). `ObjectsMatch` compares them with `Linker::read_properties` / `read_field` + `decode_script`:

* upstream's `UScriptStruct` replaces its (empty) object property list with the struct defaults, so for a
  ScriptStruct the struct defaults are compared;
* known upstream deviation: its `PropertyType` enum aliases `DelegateProperty` to `StringProperty`, so delegate tags
  are reported as `StringProperty`;
* memory positions are compared rather than storage positions, because upstream's `StoragePosition` is one byte late
  for two-byte (extended native) opcodes.

Result on Mirror's Edge (the 12 uncompressed script packages: Engine.u, TdGame.u, MirrorsEdgeTweaksScripts.u,
MirrorsEdgeConsole.u, Td*Content.u, Fp.u): property tags of all 58,845 objects upstream loads agree, and so do all
439,779 tokens of 12,097 scripts and every script storage size. Upstream fails to deserialize 381 further objects
(e.g. an enum-parsing exception in `UDefaultProperty.DeserializeTagUE3` for Engine UI components); UnrealLib reads the
properties of all of them.

### AddressSanitizer

```powershell
cmake --preset clang-cl-x64-asan -DUNREALLIB_MIRRORSEDGE_DIR="C:/…/CookedPC"
cmake --build --preset clang-cl-x64-asan
ctest --preset clang-cl-x64-asan
```

* `clang-cl-x64-asan` uses LLVM's ASan runtime. CMake links with `lld-link` directly, so `CompilerOptions.cmake` adds
  the runtime directory, the runtime libraries, and copies `clang_rt.asan_dynamic-*.dll` next to every executable.
  clang-cl's ASan does not support the debug CRT, so this preset uses the release DLL CRT.
* `msvc-x64-asan` requires the optional *C++ AddressSanitizer* Visual Studio component (not installed in the
  development environment, so not verified).
* `_DISABLE_STL_ANNOTATION` is defined for every target (dependencies included) in ASan builds: STL container
  annotations need `stl_asan.lib` and must match in all objects. Heap/stack/global checks are unaffected.
* Known toolchain limitation: throwing a new exception from *inside* a `catch` block crashed clang-cl ASan builds
  (access violation in a temporary's destructor inside the catch funclet; reproduced in
  `decompress_chunk`). The code now records the failure in the handler and throws after it. Avoid throwing from catch
  blocks in UnrealLib code.

Verified: all 177 tests pass under `clang-cl-x64-asan`, including mutation tests and real-fixture tests.

### Fuzzing

Seven fuzz targets (`tests/Fuzz/Targets`) cover the inputs the library parses: `package` (summary, compressed
chunks, name/import/export/depends tables, validation, object headers and properties of every export),
`serialization` (CompactIndex, FString, FName, … in every version/byte-order context, with round trips),
`properties` (tags and values; `write(read(x))` must be a fixed point and every value must survive
`format_property_value` → `parse_property_value`), `bytecode`, `compression` (zlib/LZO decoding and
compress → decompress round trips), `assets` (every asset reader on a fuzzed package) and `text` (T3D parser with its
serialize/parse invariant, property text parser). Contract: any input is accepted or rejected with
`UnrealLib::Error`; any other exception, crash, hang, sanitizer report or violated invariant is a bug.

* **Every test run** (`FuzzReplayTests`, any compiler): each target runs its seeds, the inputs in
  `tests/Fuzz/Regressions/<target>/` (crash/hang files from libFuzzer, kept forever) and 2,000 deterministic mutations
  (`UNREALLIB_FUZZ_ITERATIONS` to change). `UNREALLIB_FUZZ_TRACE=<file>` writes each input to `<file>` before running
  it, which identifies hangs.
* **libFuzzer** (preset `clang-cl-x64-fuzz`: ASan + coverage, tests/tools off):

  ```powershell
  uv run uelib.py build clang-cl-x64-fuzz
  build/clang-cl-x64-fuzz/tests/Fuzz/UnrealLibFuzzSeeds.exe corpus
  build/clang-cl-x64-fuzz/tests/Fuzz/UnrealLibFuzz_package.exe corpus/package -max_total_time=600 -timeout=10
  ```

  Copy any `crash-*`/`timeout-*` file into `tests/Fuzz/Regressions/<target>/` with a descriptive name.

Toolchain notes (clang-cl 22 / LLVM libFuzzer on Windows): libFuzzer's runtime is built for the static CRT, so the
preset uses `MultiThreaded`. Two instrumentation problems break C++ exception unwinding (a caught `UnrealLib::Error`
ends the process with `STATUS_BREAKPOINT` or an access violation inside the throw), so the preset avoids both:
`-fsanitize-coverage=indirect-calls` (part of `-fsanitize=fuzzer-no-link`; the modules are instrumented with
`inline-8bit-counters,pc-table,trace-cmp` instead) and optimization under ASan + libFuzzer (the preset builds
unoptimized). Each was confirmed by bisection; the affected inputs are in `Regressions/` and pass in MSVC Debug/Release,
clang-cl ASan and optimized clang-cl builds.

Results: a 60-second libFuzzer run per target (about 5.5 million inputs) and 50,000 replay mutations per target find
nothing further. Bug found and fixed: a package whose export outers form a cycle (A → B → A) made
`Linker::header_input()` loop forever (it only guarded against an object being its own outer); every outer/super chain
walk in the library is now bounded or cycle-checked (`Regressions/package/cyclic-outer-hang.bin`).

### Benchmarks

```powershell
uv run uelib.py configure msvc-x64-release -D UNREALLIB_BUILD_BENCHMARKS=ON
uv run uelib.py build msvc-x64-release -t unreallib_benchmarks
build/msvc-x64-release/benchmarks/UnrealLibBenchmarks.exe [--benchmark_filter=<regex>]
```

`benchmarks/Benchmarks.cpp` (Google Benchmark) measures compression (1 MiB of package-like data), loading synthetic
packages (1,000/10,000 exports; uncompressed, LZO and zlib), reading tagged properties, T3D parse/serialize and the
property text round trip. With `UNREALLIB_MIRRORSEDGE_DIR` set it adds fixture benchmarks on the largest map package
(or `UNREALLIB_BENCH_PACKAGE=<path below CookedPC>`): loading, resolving every import with a fresh `Linker` (includes
lazily loading the packages they live in), reading every export's properties with a `ReflectionSchema`, and T3D level
export.

Reference numbers (MSVC 19.51 x64 Release, 24-thread 4.4 GHz desktop):

| Benchmark | Result |
|-----------|--------|
| zlib compress / decompress (1 MiB, ratio 0.26) | 207 MiB/s / 1.33 GiB/s |
| LZO1X compress / decompress (1 MiB, ratio 0.26) | 297 MiB/s / 4.0 GiB/s |
| Load synthetic package, 10,000 exports: uncompressed / LZO / zlib | 2.4 ms / 3.1 ms / 3.2 ms |
| Read tagged properties, 10,000 objects | 5.5 ms (1.8 M objects/s) |
| T3D parse / serialize, 1,000 actors | 1.2 ms / 0.6 ms |
| Property text format + parse, 64 vectors | 49 µs |
| Factory_Roof.me1 (Mirror's Edge, largest map): load | 182 ms (274 MiB/s) |
| … resolve all imports (fresh linker, loads Engine etc.) | 293 ms |
| … read all properties (ReflectionSchema) | 43 ms |
| … export level as T3D | 28 ms |

## Coding rules (summary)

* C++20, no compiler extensions, `/W4 /permissive-` clean.
* No raw owning pointers, no C-style casts, no struct-overlay deserialization, no unchecked offsets.
* Every count/length read from input is validated before allocation.
* New version-dependent behaviour: add a `LegacyVersion` gate (upstream name/value). New game-dependent behaviour:
  add it to `GameProfile`.
* Every feature lands with GoogleTest coverage; formats are not claimed as supported without fixture verification.
* Add source files to the explicit lists in `src/CMakeLists.txt` / `tests/CMakeLists.txt`.

## Milestones

| Milestone | Scope | Status |
|-----------|-------|--------|
| M0 | Upstream research, Mirror's Edge verification, architecture, docs | **done** |
| M1 | Core, Serialization (reader/writer, bits, CompactIndex, FString, archives, limits, compression) | **done** |
| M2 | Summary, names, imports, exports, depends, profiles, decompression, validation, tools | **done** (verified on 2426 packages) |
| M3 | Linker (cross-package import resolution, search paths, cycles), class hierarchy (`is_a`), UObject header | **done** (verified on 2426 packages); typed object model continues in M4/M5 |
| M4 | Property tags, typed property values, struct/array/map, unknown-property preservation | **done** (verified on 2426 packages: 9,656,419 properties, byte-exact round trip; with `ReflectionSchema` every one of 1,639,154 arrays decodes). `MapProperty` values are kept as raw bytes |
| M5 | Reflection (`UField`/`UStruct`/`UClass`/`UFunction`/`UState`/`UEnum`/`UConst`), bytecode decoder + IR | **done** (verified on 2426 packages: every reflection export consumes its data exactly; 13,770 scripts / 477,343 tokens end exactly at ScriptSize) |
| M6 | Decompiler (expressions, control flow, UnrealScript output), `UnrealScriptDecompiler` tool | **done** (13,244 functions/states, 13,241 fully structured; 2,338 whole classes with 0 placeholders; see [decompiler.md](decompiler.md)) |
| M7 | Assets (textures, sound, meshes, materials, animation) + PackageExtractor | **done** (every texture, sound, static/skeletal mesh, animation and material of the game; see [assets.md](assets.md)) |
| M8 | T3D, package writer, LZO compression | **done** — writer: all 873 uncompressed packages rewritten byte-identically, all 1,553 compressed ones reload with identical tables and export bytes, LZO encoder; T3D: parser/serializer/validation, level export verified on all 680 levels (152,965 actors, 7,958 brushes / 48,103 polygons, 2,880,354 properties, 0 raw values, text round trip exact; see [t3d.md](t3d.md)); T3D import (`import_document`, `parse_property_value`, `UnrealT3DImporter`): every level's unmodified export imports back with 0 changes (338,507 objects), edits round-trip through write → reload → export; brush geometry: `read_polys`/`write_polys` (byte-exact) and `read_model` on all 9,392 Polys/Model exports |
| M9 | Differential tests for later milestones, libFuzzer, benchmarks, install/export, API review | **done** — install rules + exported `UnrealLib` CMake package (`Install.Consumer` test, Debug and Release); fuzzing: 7 targets replayed on every test run + libFuzzer preset (one cyclic-outer hang found and fixed); benchmarks (synthetic + Mirror's Edge fixture); differential tests extended to property tags and bytecode (58,845 objects, 439,779 tokens agree with upstream); API review ([api.md](api.md), header self-containment check) and [compatibility report](compatibility.md) |

### Next concrete tasks

1. Decode `MapProperty` values (kept raw today) once a fixture shows their layout.
2. Further games: profiles and verification beyond Mirror's Edge (see [compatibility.md](compatibility.md)).

## Known limitations

* Objects are parsed up to and including their tagged script properties; reflection objects completely (bytecode
  included); textures, sounds, static/skeletal meshes and animations completely (see [assets.md](assets.md)). Native
  data of other classes (levels, BSP, terrain, fonts, particle systems, material shader caches) is available as the
  export's remaining raw bytes.
* Without a `ReflectionSchema`, only the built-in Core math structs are known: arrays and script-declared structs
  are kept as raw bytes (losslessly). Pass a `ReflectionSchema` over the game's script packages to decode them all.
* `MapProperty` values are kept as raw bytes.
* Decompiler: comments, `cpptext` and `#exec` directives are not stored in cooked packages. Labels in function code are generated (`J0x…`): only state code stores label names.
* Intrinsic (native-only) classes are recognised by rule — an unexported `Core.Class` import — not from a list; their
  super chain is unknown (`Linker::is_a` returns std::nullopt for them, except "is not a Core.Component").
* File versions < 249 and ≥ 767, UE4/UE5, and LZX compression are rejected.
* Thumbnail and import/export GUID tables (≥ 584 / ≥ 623) are detected but not parsed.
* Big-endian (console) packages are handled by the archive layer and summary round-trip tests but not verified
  against real console packages, and no built-in profile accepts them.
* T3D export covers tagged properties, subobjects and brush geometry (not the level BSP tree); T3D import modifies
  existing objects only (no new actors/imports) and does not rebuild BSP (see [t3d.md](t3d.md)).
* Differential comparison against upstream UELib covers the package summary and the name/import/export/depends
  tables only; property values, reflection data and bytecode are verified by UnrealLib's own corpus sweeps. Upstream cannot read compressed packages' tables, so for the 1553
  compressed packages the tables are verified only by UnrealLib's own fixture tests.
