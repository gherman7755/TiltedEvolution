#include <Messages/MusicTrackStatusRequest.h>

void MusicTrackStatusRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, TrackId);
    Serialization::WriteBool(aWriter, Ready);
    Serialization::WriteVarInt(aWriter, DurationMs);
}

void MusicTrackStatusRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    TrackId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Ready = Serialization::ReadBool(aReader);
    DurationMs = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
}
