#pragma once

#include "Message.h"

/**
 * @brief Tells the server whether the client managed to download and load the current track.
 */
struct MusicTrackStatusRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kMusicTrackStatusRequest;

    MusicTrackStatusRequest()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const MusicTrackStatusRequest& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && TrackId == acRhs.TrackId && Ready == acRhs.Ready && DurationMs == acRhs.DurationMs; }

    uint32_t TrackId{};
    bool Ready{};
    // Exact length of the loaded file, 0 when unknown.
    uint32_t DurationMs{};
};
