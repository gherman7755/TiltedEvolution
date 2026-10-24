#include <Messages/MusicTimeSyncRequest.h>

void MusicTimeSyncRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteDouble(aWriter, ClientTimeMs);
}

void MusicTimeSyncRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    ClientTimeMs = Serialization::ReadDouble(aReader);
}
