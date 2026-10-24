#include <Messages/MusicTimeSyncResponse.h>

void MusicTimeSyncResponse::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteDouble(aWriter, ClientTimeMs);
    Serialization::WriteDouble(aWriter, ServerTimeMs);
}

void MusicTimeSyncResponse::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    ClientTimeMs = Serialization::ReadDouble(aReader);
    ServerTimeMs = Serialization::ReadDouble(aReader);
}
