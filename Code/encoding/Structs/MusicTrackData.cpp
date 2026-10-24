#include <Structs/MusicTrackData.h>
#include <TiltedCore/Serialization.hpp>

using TiltedPhoques::Serialization;

bool MusicTrackData::operator==(const MusicTrackData& acRhs) const noexcept
{
    return Id == acRhs.Id && VideoId == acRhs.VideoId && Title == acRhs.Title && Author == acRhs.Author && DurationMs == acRhs.DurationMs && AddedByPlayerId == acRhs.AddedByPlayerId &&
           AddedByName == acRhs.AddedByName;
}

bool MusicTrackData::operator!=(const MusicTrackData& acRhs) const noexcept
{
    return !this->operator==(acRhs);
}

void MusicTrackData::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Id);
    Serialization::WriteString(aWriter, VideoId);
    Serialization::WriteString(aWriter, Title);
    Serialization::WriteString(aWriter, Author);
    Serialization::WriteVarInt(aWriter, DurationMs);
    Serialization::WriteVarInt(aWriter, AddedByPlayerId);
    Serialization::WriteString(aWriter, AddedByName);
}

void MusicTrackData::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    Id = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    VideoId = Serialization::ReadString(aReader);
    Title = Serialization::ReadString(aReader);
    Author = Serialization::ReadString(aReader);
    DurationMs = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    AddedByPlayerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    AddedByName = Serialization::ReadString(aReader);
}
