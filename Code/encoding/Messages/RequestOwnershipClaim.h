#pragma once

#include "Message.h"

struct RequestOwnershipClaim final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestOwnershipClaim;

    RequestOwnershipClaim()
        : ClientMessage(Opcode)
    {
    }

    enum Kind : uint8_t
    {
        // The party leader takes over actors around them.
        kLeader,
        // A quest keeps placing the actor next to the local player, so it can only be simulated there.
        kQuestPull,
    };

    virtual ~RequestOwnershipClaim() = default;

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestOwnershipClaim& achRhs) const noexcept { return ServerId == achRhs.ServerId && ExpectedOwnershipEpoch == achRhs.ExpectedOwnershipEpoch && Reason == achRhs.Reason && GetOpcode() == achRhs.GetOpcode(); }

    uint32_t ServerId{};
    uint32_t ExpectedOwnershipEpoch{};
    Kind Reason{kLeader};
};
