#pragma once

#include "Message.h"

#include <Structs/MusicTrackData.h>

/**
 * @brief A change to the shared music queue or playback. Everyone can add tracks, the rest is up to the music host.
 */
struct MusicCommandRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kMusicCommandRequest;

    MusicCommandRequest()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const MusicCommandRequest& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Command == acRhs.Command && TrackId == acRhs.TrackId && Value == acRhs.Value && Track == acRhs.Track; }

    enum Action : uint8_t
    {
        kAdd,
        kRemove,
        kPlay,
        kNext,
        kPause,
        kResume,
        kSeek,
        kMove,
    };

    Action Command{kAdd};
    // The track to remove, play or move.
    uint32_t TrackId{};
    // Position in ms for kSeek, new queue index for kMove.
    double Value{};
    // The track to add for kAdd.
    MusicTrackData Track{};
};
