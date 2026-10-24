#include <Messages/NotifyMusicState.h>

void NotifyMusicState::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, HostPlayerId);
    aWriter.WriteBits(State, 8);
    Current.Serialize(aWriter);
    Serialization::WriteDouble(aWriter, StartServerTimeMs);
    Serialization::WriteDouble(aWriter, PositionMs);
    Serialization::WriteVarInt(aWriter, ReadyCount);
    Serialization::WriteVarInt(aWriter, PlayerCount);
    Serialization::WriteVarInt(aWriter, Queue.size());
    for (const auto& track : Queue)
        track.Serialize(aWriter);
}

void NotifyMusicState::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    HostPlayerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    uint64_t state = 0;
    aReader.ReadBits(state, 8);
    State = static_cast<PlaybackState>(state);
    Current.Deserialize(aReader);
    StartServerTimeMs = Serialization::ReadDouble(aReader);
    PositionMs = Serialization::ReadDouble(aReader);
    ReadyCount = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    PlayerCount = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    const uint64_t count = Serialization::ReadVarInt(aReader);
    Queue.resize(count);
    for (auto& track : Queue)
        track.Deserialize(aReader);
}
