#include <UnrealLib/Assets/Vehicle.hpp>

#include <fmt/format.h>

#include <UnrealLib/Core/Error.hpp>

namespace UnrealLib::Assets
{
    namespace
    {
        const ArrayValue* vehicle_array(const AssetSource& source, std::string_view name)
        {
            const auto* array = source.properties.get_array(name);
            if (array == nullptr && source.properties.find(name) != nullptr)
            {
                throw Error(ErrorCode::InvalidData, fmt::format("{}: could not decode vehicle array {}",
                            source.class_name, name));
            }
            return array;
        }

        VehicleSeat read_seat(const Package& package, const StructValue& value)
        {
            const AssetProperties fields(package, value.fields);
            return {value.struct_name, fields.get_name("CharacterSeatSocket"), fields.get_name("CharacterSeatBone"),
                    fields.get_name("CameraSeatSocket"), fields.get_object("StoragePawn"),
                    fields.get_object("SeatPawn"), fields.get_object("SeatPawnClass")};
        }
    }

    std::optional<VehicleObjectKind> vehicle_object_kind(Linker& linker, ObjectRef object)
    {
        if (!object.is_export() || !object.package->is_valid(object.index))
        {
            return std::nullopt;
        }
        using Kind = VehicleObjectKind;
        constexpr std::pair<std::string_view, Kind> classes[] = {
            {"Engine.Vehicle", Kind::Vehicle},
            {"Engine.SVehicleWheel", Kind::Wheel},
            {"Engine.SVehicleSimBase", Kind::Simulation},
            {"Engine.SkelControlWheel", Kind::WheelControl},
            {"Engine.ActorFactoryVehicle", Kind::Factory},
            {"TdGame.SeqAct_TdHelicopterFactory", Kind::HelicopterFactoryAction},
            {"Engine.SeqAct_SetRigidBodyIgnoreVehicles", Kind::IgnoreVehiclesAction},
        };
        for (const auto& [path, kind] : classes)
        {
            if (linker.is_a(object, path).value_or(false)) return kind;
        }
        return std::nullopt;
    }

    VehicleObject read_vehicle_object(Linker& linker, ObjectRef object, const PropertySchema& schema)
    {
        const auto kind = vehicle_object_kind(linker, object);
        if (!kind)
        {
            throw Error(ErrorCode::InvalidArgument, "expected a vehicle-related export");
        }

        const Package& package = *object.package;
        ObjectProperties properties = linker.read_properties(object, &schema);
        VehicleObject info{*kind, {object, package.class_name(object.index),
                                  AssetProperties(package, std::move(properties.properties)), properties.end_offset},
                           {}, {}};

        const auto add_links = [&](std::initializer_list<std::string_view> names) {
            for (const std::string_view name : names)
            {
                if (const auto index = info.source.properties.get_object(name))
                {
                    info.links.push_back({std::string(name), std::nullopt, *index, linker.resolve(package, *index)});
                }
            }
        };
        const auto add_array_links = [&](std::string_view name) {
            const ArrayValue* array = vehicle_array(info.source, name);
            if (array == nullptr) return;
            for (std::size_t slot = 0; slot < array->elements.size(); ++slot)
            {
                const auto* entry = std::get_if<ObjectValue>(&array->elements[slot].value);
                if (entry == nullptr)
                {
                    throw Error(ErrorCode::InvalidData, fmt::format("{}: {} contains a non-object value",
                                linker.qualified_path(object), name));
                }
                info.links.push_back({std::string(name), slot, entry->value, linker.resolve(package, entry->value)});
            }
        };

        switch (*kind)
        {
        case VehicleObjectKind::Vehicle:
            add_links({"Driver", "Mesh", "SimObj", "RagdollAsset", "HelicopterSkeletalMesh", "myController",
                       "HeliDustEffectEmitter", "StayUprightConstraintSetup", "StayUprightConstraintInstance"});
            add_array_links("Wheels");
            if (const ArrayValue* seats = vehicle_array(info.source, "Seats"))
            {
                info.seats.reserve(seats->elements.size());
                for (const PropertyValue& value : seats->elements)
                {
                    const auto* seat = std::get_if<StructValue>(&value.value);
                    if (seat == nullptr)
                    {
                        throw Error(ErrorCode::InvalidData, fmt::format("{}: Seats contains a non-struct value",
                                    linker.qualified_path(object)));
                    }
                    info.seats.push_back(read_seat(package, *seat));
                }
            }
            break;
        case VehicleObjectKind::Wheel:
            add_links({"WheelControl", "WheelParticleComp", "WheelPSCClass", "WheelParticleSystem"});
            break;
        case VehicleObjectKind::Factory:
            add_links({"VehicleClass", "NewActorClass", "HelicopterSkeletalMesh", "ControllerClass"});
            break;
        case VehicleObjectKind::HelicopterFactoryAction:
            add_links({"Factory", "TdHelicopterFactory"});
            add_array_links("SpawnedChoppers");
            break;
        default:
            break;
        }
        return info;
    }
}
