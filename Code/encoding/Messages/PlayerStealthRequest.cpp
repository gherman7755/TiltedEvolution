#include <Messages/PlayerStealthRequest.h>

void PlayerStealthRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, MovementFlags);
    Serialization::WriteVarInt(aWriter, Perks.size());
    for (const auto& perk : Perks)
        perk.Serialize(aWriter);
}

void PlayerStealthRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    MovementFlags = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;

    // The sneak tree has 13 perks, anything far beyond that is garbage.
    const auto cCount = std::min<uint64_t>(Serialization::ReadVarInt(aReader), 64);
    Perks.resize(cCount);
    for (auto& perk : Perks)
        perk.Deserialize(aReader);
}
