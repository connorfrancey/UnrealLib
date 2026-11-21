# Libraries
Third-party libraries and dependencies used by UnrealLib.
Libraries are fetched using CPM.cmake

## Header-only dependencies

| Dependency | Used by |
|---|---|
| `unordered_dense` | Linker export lookup |
| `magic_enum` | Game profile enum names |
| `mio` | Windows package file reads, with a stream fallback if mapping is unavailable |
| `fast_float` | Floating-point property text parsing |
| `tl::expected` | Package validator results and load errors |
| RapidJSON | Package validator JSON reports |

Versions are pinned in [Versions.cmake](../cmake/Versions.cmake). These headers are private to their build targets.
The validator dependencies are fetched only when the command-line tools are enabled.
