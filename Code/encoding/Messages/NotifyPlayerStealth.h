#pragma once

#include "Message.h"
#include <Structs/GameId.h>

using TiltedPhoques::Vector;

/**
 * @brief Stealth state of a remote player, see PlayerStealthRequest.
 */
struct NotifyPlayerStealth final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyPlayerStealth;

    NotifyPlayerStealth()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyPlayerStealth& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && ServerId == acRhs.ServerId && MovementFlags == acRhs.MovementFlags && Perks == acRhs.Perks;
    }

    // Server id of the player's character.
    uint32_t ServerId{};
    uint32_t MovementFlags{};
    Vector<GameId> Perks{};
};
