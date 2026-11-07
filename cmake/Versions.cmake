# ----------------------------------------------------------------------------------------------------------------------
# Versions.cmake
# ----------------------------------------------------------------------------------------------------------------------
# Stores library versions / git commit hashes
# ----------------------------------------------------------------------------------------------------------------------

set(GOOGLETEST_TAG v1.18.0)
set(GOOGLEBENCHMARK_TAG v1.10.0)
set(SPDLOG_TAG v1.17.0)
set(FMT_TAG 12.2.0)
set(ZLIB_TAG v1.4)
set(JSON_TAG v3.12.0)
set(CLI11_TAG v2.7.2)

# Upstream C# UELib (differential testing only; built with dotnet, never linked). Commit pinned for reproducibility.
set(UELIB_TAG 8236c39b79e30887a487ecf9b018204e430a5ebc)