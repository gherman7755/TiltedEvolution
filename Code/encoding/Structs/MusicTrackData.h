#pragma once

#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Stl.hpp>

using TiltedPhoques::String;

/**
 * @brief A YouTube track in the shared music queue.
 */
struct MusicTrackData
{
    MusicTrackData() = default;
    ~MusicTrackData() = default;

    bool operator==(const MusicTrackData& acRhs) const noexcept;
    bool operator!=(const MusicTrackData& acRhs) const noexcept;

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

    // Assigned by the server, 0 when a client asks to add the track.
    uint32_t Id{};
    String VideoId{};
    String Title{};
    String Author{};
    // 0 when unknown.
    uint32_t DurationMs{};
    uint32_t AddedByPlayerId{};
    String AddedByName{};
};
