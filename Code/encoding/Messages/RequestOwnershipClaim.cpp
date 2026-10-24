#include <Messages/RequestOwnershipClaim.h>

void RequestOwnershipClaim::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, ServerId);
    Serialization::WriteVarInt(aWriter, ExpectedOwnershipEpoch);
    Serialization::WriteVarInt(aWriter, Reason);
}

void RequestOwnershipClaim::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    ServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    ExpectedOwnershipEpoch = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Reason = Serialization::ReadVarInt(aReader) == kQuestPull ? kQuestPull : kLeader;
}
