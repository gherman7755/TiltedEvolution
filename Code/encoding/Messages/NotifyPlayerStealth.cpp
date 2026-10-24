#include <Messages/NotifyPlayerStealth.h>

void NotifyPlayerStealth::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, ServerId);
    Serialization::WriteVarInt(aWriter, MovementFlags);
    Serialization::WriteVarInt(aWriter, Perks.size());
    for (const auto& perk : Perks)
        perk.Serialize(aWriter);
}

void NotifyPlayerStealth::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    ServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    MovementFlags = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;

    const auto cCount = std::min<uint64_t>(Serialization::ReadVarInt(aReader), 64);
    Perks.resize(cCount);
    for (auto& perk : Perks)
        perk.Deserialize(aReader);
}
