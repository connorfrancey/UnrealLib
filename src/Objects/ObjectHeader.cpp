#include <UnrealLib/Objects/ObjectHeader.hpp>

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Objects/ObjectFlags.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>

namespace UnrealLib
{
    namespace
    {
        StateFrame read_state_frame(BinaryReader& reader)
        {
            const SerializationContext& ctx = reader.context();
            StateFrame frame;
            frame.node = ObjectIndex{reader.read_index()};
            frame.state_node = ObjectIndex{reader.read_index()};
            frame.probe_mask = ctx.at_least(LegacyVersion::ProbeMaskReducedAndIgnoreMaskRemoved) ? reader.read_u32()
                                                                                                : reader.read_u64();
            frame.latent_action = ctx.at_least(LegacyVersion::StateFrameLatentActionReduced) ? reader.read_u16()
                                                                                            : reader.read_u32();
            if (ctx.at_least(LegacyVersion::AddedStateStackToUStateFrame))
            {
                const std::uint32_t count = reader.read_array_count(4 * 3);
                frame.state_stack.reserve(count);
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    PushedState pushed;
                    pushed.state = ObjectIndex{reader.read_index()};
                    pushed.node = ObjectIndex{reader.read_index()};
                    pushed.offset = reader.read_index();
                    frame.state_stack.push_back(pushed);
                }
            }
            if (!frame.node.is_null())
            {
                frame.offset = reader.read_index();
            }
            return frame;
        }

        void write_state_frame(BinaryWriter& writer, const StateFrame& frame)
        {
            const SerializationContext& ctx = writer.context();
            writer.write_index(frame.node.value);
            writer.write_index(frame.state_node.value);
            if (ctx.at_least(LegacyVersion::ProbeMaskReducedAndIgnoreMaskRemoved))
            {
                writer.write_u32(static_cast<std::uint32_t>(frame.probe_mask));
            }
            else
            {
                writer.write_u64(frame.probe_mask);
            }
            if (ctx.at_least(LegacyVersion::StateFrameLatentActionReduced))
            {
                writer.write_u16(static_cast<std::uint16_t>(frame.latent_action));
            }
            else
            {
                writer.write_u32(frame.latent_action);
            }
            if (ctx.at_least(LegacyVersion::AddedStateStackToUStateFrame))
            {
                writer.write_i32(static_cast<std::int32_t>(frame.state_stack.size()));
                for (const auto& pushed : frame.state_stack)
                {
                    writer.write_index(pushed.state.value);
                    writer.write_index(pushed.node.value);
                    writer.write_index(pushed.offset);
                }
            }
            if (!frame.node.is_null())
            {
                writer.write_index(frame.offset.value_or(0));
            }
        }
    }

    bool component_template_has_name(std::uint16_t file_version, const ObjectHeaderInput& input) noexcept
    {
        return file_version < to_value(LegacyVersion::ClassDefaultCheckAddedToTemplateName) ||
               input.is_class_default_template;
    }

    ObjectHeader read_object_header(BinaryReader& reader, const ObjectHeaderInput& input)
    {
        const SerializationContext& ctx = reader.context();
        const std::size_t start = reader.position();
        ObjectHeader header;

        // Class default objects take a separate route (upstream UObject.Load -> DeserializeClassDefault): only the
        // NetIndex precedes the properties, even for component CDOs. Verified on Mirror's Edge Engine.u, e.g.
        // Default__ActorComponent = [NetIndex][TickGroup ByteProperty ...].
        if ((input.object_flags & ObjectFlag::ClassDefaultObject) != 0)
        {
            if (ctx.at_least(LegacyVersion::NetObjectCountAdded))
            {
                header.net_index = reader.read_i32();
            }
            header.properties_offset = reader.position() - start;
            return header;
        }

        if ((input.object_flags & ObjectFlag::HasStack) != 0)
        {
            header.state_frame = read_state_frame(reader);
        }

        if (input.is_component && ctx.at_least(LegacyVersion::TemplateDataAddedToUComponent))
        {
            ComponentTemplate component;
            component.owner_class = ObjectIndex{reader.read_index()};
            if (component_template_has_name(ctx.file_version, input))
            {
                component.template_name = read_fname(reader);
            }
            header.component = component;
        }

        if (ctx.at_least(LegacyVersion::NetObjectCountAdded))
        {
            header.net_index = reader.read_i32();
        }

        header.properties_offset = reader.position() - start;
        return header;
    }

    void write_object_header(BinaryWriter& writer, const ObjectHeader& header, const ObjectHeaderInput& input)
    {
        const SerializationContext& ctx = writer.context();
        if ((input.object_flags & ObjectFlag::ClassDefaultObject) != 0)
        {
            if (header.state_frame || header.component)
            {
                throw SerializationError(ErrorCode::InvalidArgument,
                    "class default objects serialize neither a state frame nor component template data");
            }
            if (ctx.at_least(LegacyVersion::NetObjectCountAdded))
            {
                writer.write_i32(header.net_index.value_or(-1));
            }
            return;
        }
        const bool has_stack = (input.object_flags & ObjectFlag::HasStack) != 0;
        if (has_stack != header.state_frame.has_value())
        {
            throw SerializationError(ErrorCode::InvalidArgument, "state frame presence does not match HasStack");
        }
        if (header.state_frame)
        {
            write_state_frame(writer, *header.state_frame);
        }

        const bool wants_component = input.is_component && ctx.at_least(LegacyVersion::TemplateDataAddedToUComponent);
        if (wants_component != header.component.has_value())
        {
            throw SerializationError(ErrorCode::InvalidArgument, "component template presence does not match input");
        }
        if (header.component)
        {
            writer.write_index(header.component->owner_class.value);
            if (component_template_has_name(ctx.file_version, input))
            {
                write_fname(writer, header.component->template_name.value_or(FName{}));
            }
        }

        if (ctx.at_least(LegacyVersion::NetObjectCountAdded))
        {
            writer.write_i32(header.net_index.value_or(-1));
        }
    }
}
