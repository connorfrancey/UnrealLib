// UELibDump — writes the raw differential schema for packages loaded with upstream UELib.
//
// Usage: UELibDump <output-dir> <package-or-directory>... [--recursive]
//
// One JSON file is written per package; its "file" key is the package path relative to the input directory (or the
// file name for file arguments), with '/' separators, and the output file name is that path with '/' replaced by "__". Packages that upstream fails to load produce
// {"file": ..., "error": ...}. Compressed packages: upstream stops after the summary, so only "summary" is written
// and "tables_loaded" is false.
//
// Schema (all values raw, as stored on disk, so display conventions cannot hide or fake differences):
//   names:   [{ "text", "flags": "0x%016X" }]
//   name references: [index, number] where number is the on-disk value (upstream's UName.Number + 1)
//   object references: raw int32 package index
//   object flags: raw on-disk uint64 (upstream swaps the 32-bit halves; undone here)
//
// Benchmark mode: UELibDump --bench <output.json> <directory> [--iterations N]
// Times upstream on the packages it reads completely (uncompressed; see Bench) for the speed comparison in
// tests/Differential/DifferentialTests.cpp (SpeedComparison).

using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using UELib;
using UELib.Core;
using UELib.Services;
using UELib.Types;

namespace UnrealLib.Differential;

internal static class Program
{
    private static readonly string[] PackageExtensions = [".u", ".upk", ".me1", ".umap", ".ut3", ".udk"];

    private static int Main(string[] args)
    {
        if (args.Length > 0 && args[0] == "--bench")
        {
            return Bench(args);
        }

        bool recursive = args.Contains("--recursive");
        string[] positional = args.Where(a => !a.StartsWith("--", StringComparison.Ordinal)).ToArray();
        if (positional.Length < 2)
        {
            Console.Error.WriteLine("usage: UELibDump <output-dir> <package-or-directory>... [--recursive]");
            return 3;
        }

        string outputDir = positional[0];
        Directory.CreateDirectory(outputDir);

        // Upstream prints diagnostics with Console.WriteLine while deserializing; keep stdout clean.
        TextWriter originalOut = Console.Out;
        Console.SetOut(TextWriter.Null);

        int written = 0, failed = 0;
        foreach (var (file, relative) in Collect(positional.Skip(1), recursive))
        {
            string target = Path.Combine(outputDir, relative.Replace("/", "__") + ".json");
            try
            {
                File.WriteAllText(target, Dump(file, relative), new UTF8Encoding(false));
                ++written;
            }
            catch (Exception e)
            {
                ++failed;
                File.WriteAllText(target, ErrorJson(relative, e), new UTF8Encoding(false));
            }
        }

        Console.SetOut(originalOut);
        Console.WriteLine($"UELibDump: {written} dumped, {failed} failed to load");
        return 0;
    }

    private static IEnumerable<(string File, string Relative)> Collect(IEnumerable<string> inputs, bool recursive)
    {
        foreach (string input in inputs)
        {
            if (Directory.Exists(input))
            {
                var option = recursive ? SearchOption.AllDirectories : SearchOption.TopDirectoryOnly;
                foreach (string f in Directory.EnumerateFiles(input, "*", option)
                             .Where(f => PackageExtensions.Contains(Path.GetExtension(f).ToLowerInvariant()))
                             .OrderBy(f => f, StringComparer.OrdinalIgnoreCase))
                {
                    yield return (f, Path.GetRelativePath(input, f).Replace(Path.DirectorySeparatorChar, '/'));
                }
            }
            else
            {
                yield return (input, Path.GetFileName(input));
            }
        }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Benchmark mode
    //
    // Workloads (the same work the dump above does, which the differential tests verify UnrealLib reproduces):
    //   load:    UnrealLoader.LoadPackage (summary, name/import/export/depends tables), for every uncompressed package
    //            (upstream stops after the summary for compressed ones, so they would not be comparable);
    //   objects: LoadPackage + InitializePackage(All) + Load of every remaining export (property tags, reflection data
    //            and bytecode tokens), for every uncompressed script package (.u).
    // One warm-up pass over all packages (JIT and tiered compilation), then per package the minimum of N runs, each
    // after a full GC so that garbage from earlier runs is not collected inside a timed run.
    //
    // Output: { "runtime", "iterations", "load": { "<relative path>": ms }, "objects": { ... } }
    // ---------------------------------------------------------------------------------------------------------------

    private static int Bench(string[] args)
    {
        int iterations = 5;
        var positional = new List<string>();
        for (int i = 1; i < args.Length; ++i)
        {
            if (args[i] == "--iterations" && i + 1 < args.Length)
            {
                iterations = Math.Max(1, int.Parse(args[++i], CultureInfo.InvariantCulture));
            }
            else
            {
                positional.Add(args[i]);
            }
        }
        if (positional.Count != 2)
        {
            Console.Error.WriteLine("usage: UELibDump --bench <output.json> <directory> [--iterations N]");
            return 3;
        }

        TextWriter originalOut = Console.Out;
        Console.SetOut(TextWriter.Null);
        // Upstream reports trailing data and swallowed exceptions on stderr; UnrealLib's logging is quiet by default,
        // so neither side spends timed work on console output.
        LibServices.LogService = new NullLogService();

        var load = new List<(string File, string Relative)>();
        var objects = new List<(string File, string Relative)>();
        foreach (var (file, relative) in Collect([positional[1]], false))
        {
            try
            {
                using UnrealPackage package = UnrealLoader.LoadPackage(file);
                var s = package.Summary;
                if (s.CompressedChunks != null && s.CompressedChunks.Count != 0 && s.CompressionFlags != 0)
                {
                    continue;
                }
                load.Add((file, relative));
                if (Path.GetExtension(file).Equals(".u", StringComparison.OrdinalIgnoreCase))
                {
                    objects.Add((file, relative));
                }
            }
            catch (Exception)
            {
                // Not loadable by upstream: nothing to compare.
            }
        }

        var loadTimes = Measure(load, iterations, LoadOnly);
        var objectTimes = Measure(objects, iterations, LoadObjects);

        using (var stream = File.Create(positional[0]))
        using (var w = new Utf8JsonWriter(stream, new JsonWriterOptions { Indented = true }))
        {
            w.WriteStartObject();
            w.WriteString("runtime", RuntimeInformation.FrameworkDescription);
            w.WriteNumber("iterations", iterations);
            foreach (var (name, times) in new[] { ("load", loadTimes), ("objects", objectTimes) })
            {
                w.WriteStartObject(name);
                foreach (var (relative, ms) in times)
                {
                    w.WriteNumber(relative, ms);
                }
                w.WriteEndObject();
            }
            w.WriteEndObject();
        }

        Console.SetOut(originalOut);
        Console.WriteLine($"UELibDump: benchmarked {load.Count} packages (load) and {objects.Count} script packages " +
                          $"(objects), {iterations} runs each");
        return 0;
    }

    private static List<(string Relative, double Milliseconds)> Measure(List<(string File, string Relative)> packages,
                                                                      int iterations, Action<string> work)
    {
        foreach (var (file, _) in packages)
        {
            work(file); // warm-up
        }
        var results = new List<(string, double)>();
        foreach (var (file, relative) in packages)
        {
            double best = double.MaxValue;
            for (int i = 0; i < iterations; ++i)
            {
                GC.Collect();
                GC.WaitForPendingFinalizers();
                GC.Collect();
                long start = Stopwatch.GetTimestamp();
                work(file);
                best = Math.Min(best, Stopwatch.GetElapsedTime(start).TotalMilliseconds);
            }
            results.Add((relative, best));
        }
        return results;
    }

    private sealed class NullLogService : ILogService
    {
        public void Log(string text) { }
        public void Log(string format, params object?[] arg) { }
        public void SilentException(Exception exception) { }
        public void SilentAssert(bool assert, string message) { }
    }

    private static void LoadOnly(string file)
    {
        using UnrealPackage package = UnrealLoader.LoadPackage(file);
    }

    private static void LoadObjects(string file)
    {
        using UnrealPackage package = UnrealLoader.LoadPackage(file);
        package.InitializePackage(UnrealPackage.InitFlags.All);
        foreach (var export in package.Exports)
        {
            UObject? obj = export.Object;
            if (obj == null || obj.DeserializationState != 0)
            {
                continue;
            }
            try
            {
                obj.Load();
            }
            catch (Exception)
            {
                // Same as the dump: failures are part of the work, not of the result.
            }
        }
    }

    private static string ErrorJson(string relative, Exception e)
    {
        using var stream = new MemoryStream();
        using (var w = new Utf8JsonWriter(stream, new JsonWriterOptions { Indented = true }))
        {
            w.WriteStartObject();
            w.WriteString("file", relative);
            w.WriteString("error", $"{e.GetType().Name}: {e.Message}");
            w.WriteEndObject();
        }
        return Encoding.UTF8.GetString(stream.ToArray());
    }

    private static string Hex32(uint v) => $"0x{v:X8}";
    private static string Hex64(ulong v) => $"0x{v:X16}";
    private static string Guid(UGuid g) => $"{g.A:X8}{g.B:X8}{g.C:X8}{g.D:X8}";

    private static void WriteName(Utf8JsonWriter w, UName name)
    {
        w.WriteStartArray();
        w.WriteNumberValue((int)name);
        w.WriteNumberValue(name.Number + 1); // upstream stores (on-disk number - 1)
        w.WriteEndArray();
    }

    // Script packages only (objects of every export are deserialized, which is slow for maps):
    //   objects: [{ "export": i, "ok": bool, "properties": [{ "name", "type", "size", "index", "struct"?, "bool"? }],
    //               "script"?: { "storage_size", "tokens": [[memory position, memory size, code]] } }]
    // code = opcode for expression tokens, 0x10000 + native index for native calls (upstream does not keep the raw
    // opcode of natives, and its StoragePosition is off by one for two-byte native opcodes, so memory positions are
    // compared). Tokens are sorted by position.
    private static void WriteObjects(Utf8JsonWriter w, UnrealPackage package)
    {
        package.InitializePackage(UnrealPackage.InitFlags.All);
        w.WriteStartArray("objects");
        for (int i = 0; i < package.Exports.Count; ++i)
        {
            UObject? obj = package.Exports[i].Object;
            if (obj == null)
            {
                continue;
            }
            if (obj.DeserializationState == 0)
            {
                try
                {
                    obj.Load();
                }
                catch (Exception)
                {
                    // Reported through DeserializationState below.
                }
            }
            bool ok = !obj.DeserializationState.HasFlag(UObject.ObjectState.Errorlized);
            w.WriteStartObject();
            w.WriteNumber("export", i);
            w.WriteBoolean("ok", ok);
            w.WriteStartArray("properties");
            foreach (var p in obj.Properties ?? [])
            {
                w.WriteStartObject();
                w.WritePropertyName("name"); WriteName(w, p.Name);
                w.WriteString("type", UDefaultProperty.ResolvePropertyTypeName(p.Type));
                w.WriteNumber("size", p.Size);
                w.WriteNumber("index", p.ArrayIndex);
                // StructName shares storage with EnumName/InnerTypeName upstream, so it is only meaningful for structs.
                if (p.Type == PropertyType.StructProperty && p.StructName is { } structName)
                {
                    w.WritePropertyName("struct"); WriteName(w, structName);
                }
                if (p.BoolValue is { } boolValue)
                {
                    w.WriteBoolean("bool", boolValue);
                }
                w.WriteEndObject();
            }
            w.WriteEndArray();
            if (obj is UStruct { ByteCodeManager: { } code } s)
            {
                w.WriteStartObject("script");
                w.WriteNumber("storage_size", s.ScriptSize);
                w.WriteStartArray("tokens");
                foreach (var t in (code.DeserializedTokens ?? []).OrderBy(t => t.Position))
                {
                    w.WriteStartArray();
                    w.WriteNumberValue(t.Position);
                    w.WriteNumberValue(t.Size);
                    w.WriteNumberValue(t is UStruct.UByteCodeDecompiler.NativeFunctionToken n
                                           ? 0x10000 + n.NativeItem.ByteToken
                                           : t.OpCode);
                    w.WriteEndArray();
                }
                w.WriteEndArray();
                w.WriteEndObject();
            }
            w.WriteEndObject();
        }
        w.WriteEndArray();
    }

    private static string Dump(string file, string relative)
    {
        using UnrealPackage package = UnrealLoader.LoadPackage(file);
        UnrealPackage.PackageFileSummary s = package.Summary;
        bool tablesLoaded = s.CompressedChunks == null || s.CompressedChunks.Count == 0 || s.CompressionFlags == 0;

        using var stream = new MemoryStream();
        using (var w = new Utf8JsonWriter(stream, new JsonWriterOptions { Indented = true }))
        {
            w.WriteStartObject();
            w.WriteString("file", relative);
            w.WriteString("upstream_build", package.Build?.Name.ToString() ?? "null");
            w.WriteBoolean("tables_loaded", tablesLoaded);

            w.WriteStartObject("summary");
            w.WriteNumber("file_version", s.Version);
            w.WriteNumber("licensee_version", s.LicenseeVersion);
            w.WriteNumber("header_size", s.HeaderSize);
            w.WriteString("folder_name", s.FolderName ?? "");
            w.WriteString("package_flags", Hex32((uint)s.PackageFlags));
            w.WriteNumber("name_count", s.NameCount);
            w.WriteNumber("name_offset", s.NameOffset);
            w.WriteNumber("export_count", s.ExportCount);
            w.WriteNumber("export_offset", s.ExportOffset);
            w.WriteNumber("import_count", s.ImportCount);
            w.WriteNumber("import_offset", s.ImportOffset);
            w.WriteNumber("depends_offset", s.DependsOffset);
            w.WriteString("guid", Guid(s.Guid));
            w.WriteStartArray("generations");
            foreach (var g in s.Generations ?? [])
            {
                w.WriteStartArray();
                w.WriteNumberValue(g.ExportCount);
                w.WriteNumberValue(g.NameCount);
                w.WriteNumberValue(g.NetObjectCount);
                w.WriteEndArray();
            }
            w.WriteEndArray();
            w.WriteNumber("engine_version", s.EngineVersion);
            w.WriteNumber("cooker_version", s.CookerVersion);
            w.WriteString("compression_flags", Hex32(s.CompressionFlags));
            // Field values exactly as upstream exposes them (its Deserialize reads the record in its own order).
            w.WriteStartArray("compressed_chunks");
            foreach (var c in s.CompressedChunks ?? [])
            {
                w.WriteStartObject();
                w.WriteNumber("uncompressed_offset", c.UncompressedOffset);
                w.WriteNumber("uncompressed_size", c.UncompressedSize);
                w.WriteNumber("compressed_offset", c.CompressedOffset);
                w.WriteNumber("compressed_size", c.CompressedSize);
                w.WriteEndObject();
            }
            w.WriteEndArray();
            w.WriteString("package_source", Hex32(s.PackageSource));
            w.WriteStartArray("additional_packages_to_cook");
            foreach (string p in s.AdditionalPackagesToCook ?? [])
            {
                w.WriteStringValue(p);
            }
            w.WriteEndArray();
            w.WriteEndObject();

            if (tablesLoaded)
            {
                w.WriteStartArray("names");
                foreach (var n in package.Names)
                {
                    w.WriteStartObject();
                    w.WriteString("text", n.Name);
                    w.WriteString("flags", Hex64(n.Flags));
                    w.WriteEndObject();
                }
                w.WriteEndArray();

                w.WriteStartArray("imports");
                foreach (var i in package.Imports)
                {
                    w.WriteStartObject();
                    w.WritePropertyName("class_package"); WriteName(w, i.ClassPackageName);
                    w.WritePropertyName("class"); WriteName(w, i.ClassName);
                    w.WriteNumber("outer", i.OuterIndex.Index);
                    w.WritePropertyName("name"); WriteName(w, i.ObjectName);
                    w.WriteEndObject();
                }
                w.WriteEndArray();

                w.WriteStartArray("exports");
                foreach (var e in package.Exports)
                {
                    w.WriteStartObject();
                    w.WriteNumber("class", e.ClassIndex.Index);
                    w.WriteNumber("super", e.SuperIndex.Index);
                    w.WriteNumber("outer", e.OuterIndex.Index);
                    w.WritePropertyName("name"); WriteName(w, e.ObjectName);
                    w.WriteNumber("archetype", e.ArchetypeIndex.Index);
                    // Undo upstream's half swap to obtain the on-disk value (little-endian packages).
                    ulong flags = e.ObjectFlags;
                    w.WriteString("object_flags", Hex64((flags >> 32) | (flags << 32)));
                    w.WriteNumber("serial_size", e.SerialSize);
                    w.WriteNumber("serial_offset", e.SerialOffset);
                    w.WriteString("export_flags", Hex32(e.ExportFlags));
                    w.WriteStartArray("component_map");
                    if (e.ComponentMap != null)
                    {
                        foreach (var kv in e.ComponentMap)
                        {
                            w.WriteStartArray();
                            WriteName(w, kv.Key);
                            w.WriteNumberValue(kv.Value.Index);
                            w.WriteEndArray();
                        }
                    }
                    w.WriteEndArray();
                    w.WriteStartArray("net_object_counts");
                    foreach (int n in e.GenerationNetObjectCount ?? [])
                    {
                        w.WriteNumberValue(n);
                    }
                    w.WriteEndArray();
                    w.WriteString("package_guid", Guid(e.PackageGuid));
                    w.WriteString("package_flags", Hex32(e.PackageFlags));
                    w.WriteEndObject();
                }
                w.WriteEndArray();

                w.WriteStartArray("depends");
                foreach (var list in package.Dependencies)
                {
                    w.WriteStartArray();
                    foreach (var d in list)
                    {
                        w.WriteNumberValue(d.Index);
                    }
                    w.WriteEndArray();
                }
                w.WriteEndArray();

                if (Path.GetExtension(file).Equals(".u", StringComparison.OrdinalIgnoreCase))
                {
                    WriteObjects(w, package);
                }
            }

            w.WriteEndObject();
        }
        return Encoding.UTF8.GetString(stream.ToArray());
    }
}
