# ----------------------------------------------------------------------------------------------------------------------
# Versions.cmake
# ----------------------------------------------------------------------------------------------------------------------
# Stores library versions / git commit hashes
# ----------------------------------------------------------------------------------------------------------------------

# Testing
set(GOOGLETEST_TAG v1.18.0)
set(GOOGLEBENCHMARK_TAG v1.10.0)
set(APPROVALTESTS_TAG v.10.13.0)
set(RAPIDCHECK_TAG 2c3c4365aca21ef4e612768fdc95b1ce8b39a651)

# Debugging / Decompiling

set(SPDLOG_TAG v1.17.0)
set(FMT_TAG 12.2.0)

# Compression
set(ZLIB_TAG v1.4)
set(ZLIBNG_VERSION 2.3.3)                   # Optimized zlib replacement - https://github.com/zlib-ng/zlib-ng
set(LZ4_VERSION v1.11.1)                    #
set(JSON_TAG v3.12.0) # nlohmann/json - TODO: Replace with more optimized library
set(TOML_TAG v4.4.0)  # toml11
set(CLI11_TAG v2.7.2)

# Performance
set(FASTFLOAT_TAG v8.3.3)
set(UNORDEREDDENSE_TAG v5.3.3)
set(MIMALLOC_TAG v3.6.1)
set(MAGICENUM_TAG v0.9.8)
set(TL_EXPECTED_TAG v1.3.1)
set(MIO_TAG 8b6b7d878c89e81614d05edca7936de41ccdd2da)
set(RAPIDJSON_TAG 297637bc0a0f00dc504dbc1a47ebdaa577f8ae19)
set(XXHASH_TAG v0.8.4)
set(SIMDUTF_TAG v9.4.0)
set(SVECTOR_TAG v1.4.0)
set(CONCURRENTQUEUE_TAG v1.0.5)

# Upstream C# UELib (differential testing only; built with dotnet, never linked). Commit pinned for reproducibility.
set(UELIB_TAG 8236c39b79e30887a487ecf9b018204e430a5ebc)
