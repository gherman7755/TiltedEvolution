#include <Messages/MusicCommandRequest.h>

void MusicCommandRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Command, 8);
    Serialization::WriteVarInt(aWriter, TrackId);
    Serialization::WriteDouble(aWriter, Value);
    Track.Serialize(aWriter);
}

void MusicCommandRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    uint64_t command = 0;
    aReader.ReadBits(command, 8);
    Command = static_cast<Action>(command);
    TrackId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Value = Serialization::ReadDouble(aReader);
    Track.Deserialize(aReader);
}
