// UnrealLib benchmarks (Google Benchmark). Build with -DUNREALLIB_BUILD_BENCHMARKS=ON (Release for meaningful numbers).
//
// Synthetic benchmarks always run. With UNREALLIB_MIRRORSEDGE_DIR set, fixture benchmarks also run against the
// largest map package of the game (override with UNREALLIB_BENCH_PACKAGE=<path relative to CookedPC>).

#include <benchmark/benchmark.h>

#include <algorithm>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include <UnrealLib/UnrealLib.hpp>

#include "Support/FixtureSupport.hpp"
#include "Support/SyntheticPackage.hpp"

using namespace UnrealLib;
namespace fs = std::filesystem;

namespace
{
    // ------------------------------------------------------------------------------------------------------------------
    // Data
    // ------------------------------------------------------------------------------------------------------------------

    /// 1 MiB that compresses like package data: runs of structured records mixed with noise.
    const std::vector<std::byte>& sample_data()
    {
        static const std::vector<std::byte> data = [] {
            std::vector<std::byte> out(1u << 20);
            std::mt19937 rng(42);
            for (std::size_t i = 0; i < out.size(); ++i)
            {
                const bool noise = (i / 4096) % 4 == 3;
                out[i] = static_cast<std::byte>(noise ? rng() : (i * 31 / 7) % 61);
            }
            return out;
        }();
        return data;
    }

    /// A cooked package with @p exports objects (each with tagged properties), as file bytes.
    std::vector<std::byte> synthetic_package(std::size_t exports, CompressionMethod compression)
    {
        using namespace Testing;
        SyntheticPackageSpec spec;
        spec.names = {"None", "Core", "Engine", "Package", "Class", "Object", "IntProperty", "FloatProperty",
                      "Health", "Speed"};
        spec.compression = compression;
        // The objects' class is defined in the package itself (a UClass export), so its hierarchy resolves without
        // other packages.
        const ObjectIndex actor = add_export(spec, ObjectIndex{}, ObjectIndex{}, ObjectIndex{}, "BenchActor");
        // NetIndex, Health=100 (IntProperty), Speed=1.5 (FloatProperty), None.
        BinaryWriter w;
        w.write_i32(0);
        for (const auto& [name, type, value] : {std::tuple{8, 6, 100}, std::tuple{9, 7, 0x3FC00000}})
        {
            w.write_i32(name);
            w.write_i32(0);
            w.write_i32(type);
            w.write_i32(0);
            w.write_i32(4);
            w.write_i32(0);
            w.write_i32(value);
        }
        w.write_i32(0);
        w.write_i32(0);
        const std::vector<std::byte> payload = w.take();
        for (std::size_t i = 0; i < exports; ++i)
        {
            add_export(spec, actor, ObjectIndex{}, ObjectIndex{}, "Actor_" + std::to_string(i), 0x0007000400000000ull,
                       payload);
        }
        return build_synthetic_package(spec).file;
    }

    std::string synthetic_t3d(std::size_t actors)
    {
        std::string text = "Begin Map\n   Begin Level NAME=PersistentLevel\n";
        for (std::size_t i = 0; i < actors; ++i)
        {
            text += "      Begin Actor Class=StaticMeshActor Name=StaticMeshActor_" + std::to_string(i) + "\n"
                    "         Begin Object Class=StaticMeshComponent Name=StaticMeshComponent0\n"
                    "            StaticMesh=StaticMesh'Pkg.Group.Mesh'\n"
                    "         End Object\n"
                    "         Components(0)=StaticMeshComponent0\n"
                    "         Location=(X=1024.5,Y=-512.25,Z=96.0)\n"
                    "         Tag=\"Some text\"\n"
                    "      End Actor\n";
        }
        return text + "   End Level\nEnd Map\n";
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Synthetic benchmarks
    // ------------------------------------------------------------------------------------------------------------------

    void BM_Compress(benchmark::State& state, CompressionMethod method)
    {
        const auto& data = sample_data();
        for (auto _ : state) benchmark::DoNotOptimize(compress(method, data));
        state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * data.size()));
    }

    void BM_Decompress(benchmark::State& state, CompressionMethod method)
    {
        const auto& data = sample_data();
        const std::vector<std::byte> packed = compress(method, data);
        std::vector<std::byte> out(data.size());
        for (auto _ : state)
        {
            decompress(method, packed, out);
            benchmark::DoNotOptimize(out.data());
        }
        state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * data.size()));
        state.counters["ratio"] = static_cast<double>(packed.size()) / static_cast<double>(data.size());
    }

    void BM_LoadPackage(benchmark::State& state, CompressionMethod method)
    {
        const std::vector<std::byte> file = synthetic_package(static_cast<std::size_t>(state.range(0)), method);
        for (auto _ : state)
        {
            Package package;
            package.load_from_memory(file, "Synthetic");
            benchmark::DoNotOptimize(package.exports().size());
        }
        state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * file.size()));
        state.SetItemsProcessed(state.iterations() * state.range(0));
    }

    void BM_ReadProperties(benchmark::State& state)
    {
        Package package;
        package.load_from_memory(synthetic_package(static_cast<std::size_t>(state.range(0)), CompressionMethod::None),
                                 "Synthetic");
        Linker linker;
        const Package& p = linker.add(std::move(package));
        for (auto _ : state)
        {
            std::size_t n = 0;
            try
            {
                for (std::size_t i = 1; i < p.exports().size(); ++i) // export 0 is the class
                {
                    n += linker.read_properties(ObjectRef{&p, ObjectIndex::from_export(i)}).properties.size();
                }
            }
            catch (const Error& e)
            {
                state.SkipWithError(e.what());
                break;
            }
            if (n != 2 * static_cast<std::size_t>(state.range(0)))
            {
                state.SkipWithError("unexpected property count");
                break;
            }
            benchmark::DoNotOptimize(n);
        }
        state.SetItemsProcessed(state.iterations() * state.range(0));
    }

    void BM_T3DParse(benchmark::State& state)
    {
        const std::string text = synthetic_t3d(static_cast<std::size_t>(state.range(0)));
        for (auto _ : state) benchmark::DoNotOptimize(T3D::parse(text));
        state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * text.size()));
    }

    void BM_T3DSerialize(benchmark::State& state)
    {
        const T3D::T3DDocument doc = T3D::parse(synthetic_t3d(static_cast<std::size_t>(state.range(0))));
        for (auto _ : state) benchmark::DoNotOptimize(T3D::serialize(doc));
        state.SetItemsProcessed(state.iterations() * state.range(0));
    }

    void BM_PropertyTextRoundTrip(benchmark::State& state)
    {
        NameTable names;
        names.add("None");
        StructValue vector{"Vector", StructEncoding::Binary, {}};
        for (const char* axis : {"X", "Y", "Z"})
        {
            Property p;
            p.name = axis;
            p.value.value = 1234.5678f;
            vector.fields.push_back(p);
        }
        ArrayValue array;
        array.elements.assign(64, PropertyValue{vector});
        array.count = 64;
        const PropertyValue value{array};
        PropertyFormatOptions format;
        format.names = &names;
        PropertyParseOptions parse;
        parse.make_name = [&](std::string_view t) { return FName{names.add(t), 0}; };
        for (auto _ : state)
        {
            const std::string text = format_property_value(value, format);
            benchmark::DoNotOptimize(parse_property_value(text, value, parse));
        }
        state.SetItemsProcessed(state.iterations() * 64);
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Fixture benchmarks (Mirror's Edge)
    // ------------------------------------------------------------------------------------------------------------------

    fs::path bench_package()
    {
        const std::string chosen = TestSupport::env("UNREALLIB_BENCH_PACKAGE");
        if (!chosen.empty()) return TestSupport::fixture_dir() / chosen;
        fs::path best;
        std::uintmax_t best_size = 0;
        for (const auto& e : fs::recursive_directory_iterator(TestSupport::fixture_dir() / "Maps"))
        {
            if (e.is_regular_file() && Text::iequals(e.path().extension().string(), ".me1") &&
                e.file_size() > best_size)
            {
                best = e.path();
                best_size = e.file_size();
            }
        }
        return best;
    }

    void BM_FixtureLoad(benchmark::State& state, fs::path file)
    {
        for (auto _ : state)
        {
            const Package package = Package::open(file);
            benchmark::DoNotOptimize(package.exports().size());
        }
        state.SetLabel(file.filename().string());
        state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * fs::file_size(file)));
    }

    void BM_FixtureResolveImports(benchmark::State& state, fs::path file)
    {
        // Fresh linker each time: includes lazily loading the packages the imports live in.
        std::size_t imports = 0;
        for (auto _ : state)
        {
            Linker linker;
            linker.add_search_path(TestSupport::fixture_dir());
            const Package& package = linker.load(file);
            imports = package.imports().size();
            for (std::size_t i = 0; i < imports; ++i)
            {
                ResolveStatus status = linker.resolve(package, ObjectIndex::from_import(i)).status;
                benchmark::DoNotOptimize(status);
            }
        }
        state.SetLabel(file.filename().string());
        state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * imports));
    }

    struct FixtureWorld
    {
        Linker linker;
        std::unique_ptr<ReflectionSchema> schema;
        const Package* package = nullptr;

        explicit FixtureWorld(const fs::path& file)
        {
            linker.add_search_path(TestSupport::fixture_dir());
            schema = std::make_unique<ReflectionSchema>(linker);
            for (const Package* script : TestSupport::load_script_packages(linker)) schema->add_package(*script);
            package = &linker.load(file);
        }
    };

    void BM_FixtureReadAllProperties(benchmark::State& state, fs::path file)
    {
        FixtureWorld world(file);
        std::size_t exports = 0;
        for (auto _ : state)
        {
            std::size_t n = 0;
            exports = 0;
            for (std::size_t i = 0; i < world.package->exports().size(); ++i)
            {
                const ObjectRef ref{world.package, ObjectIndex::from_export(i)};
                try
                {
                    n += world.linker.read_properties(ref, world.schema.get()).properties.size();
                    ++exports;
                }
                catch (const Error&)
                {
                }
            }
            benchmark::DoNotOptimize(n);
        }
        state.SetLabel(file.filename().string());
        state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * exports));
    }

    void BM_FixtureExportLevel(benchmark::State& state, fs::path file)
    {
        FixtureWorld world(file);
        T3D::ExportOptions options;
        options.schema = world.schema.get();
        for (auto _ : state)
        {
            benchmark::DoNotOptimize(T3D::serialize(T3D::export_level(world.linker, *world.package, options)));
        }
        state.SetLabel(file.filename().string());
    }
}

int main(int argc, char** argv)
{
    using M = CompressionMethod;
    benchmark::RegisterBenchmark("Compress/zlib", BM_Compress, M::Zlib)->Unit(benchmark::kMillisecond);
    benchmark::RegisterBenchmark("Compress/lzo", BM_Compress, M::Lzo)->Unit(benchmark::kMillisecond);
    benchmark::RegisterBenchmark("Decompress/zlib", BM_Decompress, M::Zlib)->Unit(benchmark::kMillisecond);
    benchmark::RegisterBenchmark("Decompress/lzo", BM_Decompress, M::Lzo)->Unit(benchmark::kMillisecond);
    benchmark::RegisterBenchmark("LoadPackage/uncompressed", BM_LoadPackage, M::None)->Arg(1000)->Arg(10000);
    benchmark::RegisterBenchmark("LoadPackage/lzo", BM_LoadPackage, M::Lzo)->Arg(10000);
    benchmark::RegisterBenchmark("LoadPackage/zlib", BM_LoadPackage, M::Zlib)->Arg(10000);
    benchmark::RegisterBenchmark("ReadProperties", BM_ReadProperties)->Arg(10000);
    benchmark::RegisterBenchmark("T3D/parse", BM_T3DParse)->Arg(1000);
    benchmark::RegisterBenchmark("T3D/serialize", BM_T3DSerialize)->Arg(1000);
    benchmark::RegisterBenchmark("PropertyText/format+parse", BM_PropertyTextRoundTrip);

    if (TestSupport::fixtures_available())
    {
        const fs::path file = bench_package();
        if (!file.empty() && fs::is_regular_file(file))
        {
            benchmark::RegisterBenchmark("Fixture/LoadPackage", BM_FixtureLoad, file)->Unit(benchmark::kMillisecond);
            benchmark::RegisterBenchmark("Fixture/ResolveImports", BM_FixtureResolveImports, file)
                ->Unit(benchmark::kMillisecond);
            benchmark::RegisterBenchmark("Fixture/ReadAllProperties", BM_FixtureReadAllProperties, file)
                ->Unit(benchmark::kMillisecond);
            benchmark::RegisterBenchmark("Fixture/ExportLevelT3D", BM_FixtureExportLevel, file)
                ->Unit(benchmark::kMillisecond);
        }
    }

    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
