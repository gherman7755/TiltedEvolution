#pragma once

#include "Message.h"
#include <Structs/GameId.h>

using TiltedPhoques::Vector;

/**
 * @brief What other clients need to judge how well the local player is hidden.
 */
struct PlayerStealthRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kPlayerStealthRequest;

    PlayerStealthRequest()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const PlayerStealthRequest& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && MovementFlags == acRhs.MovementFlags && Perks == acRhs.Perks; }

    // Walking, running, sprinting and sneaking bits of the actor state.
    uint32_t MovementFlags{};
    Vector<GameId> Perks{};
};
