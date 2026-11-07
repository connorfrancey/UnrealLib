#include <UnrealLib/Package/PackageValidator.hpp>

#include <algorithm>
#include <vector>

#include <fmt/format.h>

#include <UnrealLib/Package/Package.hpp>

namespace UnrealLib
{
    namespace
    {
        class Validator
        {
        public:
            explicit Validator(const Package& package) : m_package(package) {}

            Diagnostics run()
            {
                check_summary();
                check_imports();
                check_exports();
                check_outer_chains();
                check_serial_ranges();
                check_depends();
                return std::move(m_diagnostics);
            }

        private:
            void check_offset(std::string_view what, std::int32_t offset, std::int32_t count)
            {
                if (count == 0)
                {
                    return;
                }
                if (offset < 0 || static_cast<std::uint64_t>(offset) >= m_package.data().size())
                {
                    m_diagnostics.error("summary.offset-out-of-bounds",
                        fmt::format("{} offset {} is outside the package ({} bytes)", what, offset,
                                    m_package.data().size()));
                }
            }

            void check_summary()
            {
                const PackageSummary& s = m_package.summary();
                const auto size = m_package.data().size();
                if (s.header_size < 0 || static_cast<std::uint64_t>(s.header_size) > size)
                {
                    m_diagnostics.error("summary.header-size",
                        fmt::format("header size {} is outside the package ({} bytes)", s.header_size, size));
                }
                check_offset("name table", s.name_offset, s.name_count);
                check_offset("import table", s.import_offset, s.import_count);
                check_offset("export table", s.export_offset, s.export_count);

                if (s.generations.empty())
                {
                    m_diagnostics.warning("summary.no-generations", "summary has no generation records");
                }
                else
                {
                    const GenerationInfo& latest = s.generations.back();
                    if (latest.name_count != s.name_count || latest.export_count != s.export_count)
                    {
                        m_diagnostics.warning("summary.generation-mismatch",
                            fmt::format("latest generation ({} names, {} exports) differs from the tables ({} names, "
                                        "{} exports)",
                                        latest.name_count, latest.export_count, s.name_count, s.export_count));
                    }
                }
                if (s.depends_offset != 0 && s.header_size > 0 && s.depends_offset > s.header_size)
                {
                    m_diagnostics.warning("summary.depends-after-header",
                        fmt::format("depends offset {} is after the header end {}", s.depends_offset, s.header_size));
                }
            }

            void check_name(const FName& name, std::string_view where)
            {
                if (!m_package.names().contains_index(name.index))
                {
                    m_diagnostics.error("name.out-of-range",
                        fmt::format("{} refers to name index {} (table has {} entries)", where, name.index,
                                    m_package.names().size()));
                }
                if (name.number < 0)
                {
                    m_diagnostics.warning("name.negative-number",
                        fmt::format("{} has a negative name number {}", where, name.number));
                }
            }

            void check_index(ObjectIndex index, std::string_view where)
            {
                if (!m_package.is_valid(index))
                {
                    m_diagnostics.error("reference.out-of-range",
                        fmt::format("{} refers to object index {} (imports {}, exports {})", where, index.value,
                                    m_package.imports().size(), m_package.exports().size()));
                }
            }

            void check_imports()
            {
                const auto imports = m_package.imports();
                for (std::size_t i = 0; i < imports.size(); ++i)
                {
                    const auto where = fmt::format("import {}", i);
                    check_name(imports[i].class_package, where);
                    check_name(imports[i].class_name, where);
                    check_name(imports[i].object_name, where);
                    check_index(imports[i].outer, where);
                }
            }

            void check_exports()
            {
                const auto exports = m_package.exports();
                for (std::size_t i = 0; i < exports.size(); ++i)
                {
                    const ExportEntry& e = exports[i];
                    const auto where = fmt::format("export {}", i);
                    check_name(e.object_name, where);
                    check_index(e.class_index, where + " class");
                    check_index(e.super_index, where + " super");
                    check_index(e.outer, where + " outer");
                    check_index(e.archetype, where + " archetype");
                    for (const auto& component : e.component_map)
                    {
                        check_name(component.name, where + " component");
                        check_index(component.object, where + " component");
                    }
                    if (e.outer.is_export() && e.outer.export_index() == i)
                    {
                        m_diagnostics.error("reference.self-outer", fmt::format("{} is its own outer", where));
                    }
                }
            }

            void check_outer_chains()
            {
                const std::size_t total = m_package.imports().size() + m_package.exports().size();
                auto walk = [&](ObjectIndex start, std::string_view where) {
                    std::size_t steps = 0;
                    for (ObjectIndex current = start; !current.is_null(); ++steps)
                    {
                        if (!m_package.is_valid(current))
                        {
                            return; // reported by check_index
                        }
                        if (steps > total)
                        {
                            m_diagnostics.error("reference.outer-cycle",
                                fmt::format("{} has a cyclic outer chain", where));
                            return;
                        }
                        current = m_package.outer(current);
                    }
                };
                for (std::size_t i = 0; i < m_package.imports().size(); ++i)
                {
                    walk(ObjectIndex::from_import(i), fmt::format("import {}", i));
                }
                for (std::size_t i = 0; i < m_package.exports().size(); ++i)
                {
                    walk(ObjectIndex::from_export(i), fmt::format("export {}", i));
                }
            }

            void check_serial_ranges()
            {
                struct Range
                {
                    std::int64_t begin;
                    std::int64_t end;
                    std::size_t index;
                };
                std::vector<Range> ranges;
                const auto exports = m_package.exports();
                const auto size = static_cast<std::int64_t>(m_package.data().size());
                const std::int64_t header_end = m_package.summary().header_size;

                for (std::size_t i = 0; i < exports.size(); ++i)
                {
                    const ExportEntry& e = exports[i];
                    if (e.serial_size < 0 || e.serial_offset < 0)
                    {
                        m_diagnostics.error("export.serial-negative",
                            fmt::format("export {} has negative serial size/offset ({}, {})", i, e.serial_size,
                                        e.serial_offset));
                        continue;
                    }
                    if (e.serial_size == 0)
                    {
                        continue;
                    }
                    const std::int64_t begin = e.serial_offset;
                    const std::int64_t end = begin + e.serial_size;
                    if (end > size)
                    {
                        m_diagnostics.error("export.serial-out-of-bounds",
                            fmt::format("export {} data [{}, {}) exceeds the package ({} bytes)", i, begin, end, size));
                        continue;
                    }
                    if (begin < header_end)
                    {
                        m_diagnostics.warning("export.serial-in-header",
                            fmt::format("export {} data starts at {} inside the header (ends at {})", i, begin,
                                        header_end));
                    }
                    ranges.push_back({begin, end, i});
                }

                std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) { return a.begin < b.begin; });
                for (std::size_t k = 1; k < ranges.size(); ++k)
                {
                    if (ranges[k].begin < ranges[k - 1].end)
                    {
                        m_diagnostics.error("export.serial-overlap",
                            fmt::format("export {} data [{}, {}) overlaps export {} data [{}, {})", ranges[k].index,
                                        ranges[k].begin, ranges[k].end, ranges[k - 1].index, ranges[k - 1].begin,
                                        ranges[k - 1].end));
                    }
                }
            }

            void check_depends()
            {
                const auto& depends = m_package.depends();
                if (depends.empty())
                {
                    return;
                }
                if (depends.size() != m_package.exports().size())
                {
                    m_diagnostics.error("depends.count",
                        fmt::format("depends table has {} lists for {} exports", depends.size(),
                                    m_package.exports().size()));
                }
                for (std::size_t i = 0; i < depends.size(); ++i)
                {
                    for (const ObjectIndex index : depends[i])
                    {
                        check_index(index, fmt::format("depends list of export {}", i));
                    }
                }
            }

            const Package& m_package;
            Diagnostics m_diagnostics;
        };
    }

    Diagnostics validate_package(const Package& package)
    {
        return Validator(package).run();
    }
}
