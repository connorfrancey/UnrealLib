#pragma once

/// @file
/// @brief Serialized UE3 vehicle metadata, including Mirror's Edge seat layouts and helicopter factories.

#include <UnrealLib/Assets/AssetObject.hpp>

namespace UnrealLib::Assets
{
    enum class VehicleObjectKind : std::uint8_t
    {
        Vehicle,
        Wheel,
        Simulation,
        WheelControl,
        Factory,
        HelicopterFactoryAction,
        IgnoreVehiclesAction,
    };

    /// @brief A reference from a vehicle property, including its serialized array slot when applicable.
    struct VehicleLink
    {
        std::string property;
        std::optional<std::size_t> array_index;
        ObjectIndex index;      ///< Index in the package owning the vehicle object.
        Resolution resolution;
    };

    /// @brief A TdVehicle.VehicleSeat or TdVehicle_Helicopter.GunnerSeat.
    /// @details Absent fields inherit script defaults. Remaining seat fields are available in the Seats property.
    struct VehicleSeat
    {
        std::string struct_name;
        std::optional<std::string> character_socket;
        std::optional<std::string> character_bone;
        std::optional<std::string> camera_socket;
        std::optional<ObjectIndex> storage_pawn; ///< Index in the vehicle package.
        std::optional<ObjectIndex> seat_pawn;
        std::optional<ObjectIndex> seat_pawn_class;
    };

    /// @brief Serialized vehicle data, including tagged physics, flight and camera settings where present.
    struct VehicleObject
    {
        VehicleObjectKind kind;
        AssetSource source;
        std::vector<VehicleLink> links;
        std::vector<VehicleSeat> seats;
    };

    /// @brief Classifies a vehicle-related export using its reflected class hierarchy.
    [[nodiscard]] std::optional<VehicleObjectKind> vehicle_object_kind(Linker& linker, ObjectRef object);

    /// @brief Reads an instance or class default object. Use a ReflectionSchema with Engine and TdGame registered
    ///        to decode Wheels, Seats and other vehicle arrays. Properties represent the serialized overrides.
    [[nodiscard]] VehicleObject read_vehicle_object(Linker& linker, ObjectRef object, const PropertySchema& schema);
}
