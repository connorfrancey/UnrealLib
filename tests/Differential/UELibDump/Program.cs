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

using System.Text;
using System.Text.Json;
using UELib;
using UELib.Core;

namespace UnrealLib.Differential;

internal static class Program
{
    private static readonly string[] PackageExtensions = [".u", ".upk", ".me1", ".umap", ".ut3", ".udk"];

    private static int Main(string[] args)
    {
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
            }

            w.WriteEndObject();
        }
        return Encoding.UTF8.GetString(stream.ToArray());
    }
}
