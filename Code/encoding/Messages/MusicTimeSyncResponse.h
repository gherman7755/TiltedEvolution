#pragma once

#include "Message.h"

/**
 * @brief The server music clock, answering a MusicTimeSyncRequest.
 */
struct MusicTimeSyncResponse final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kMusicTimeSyncResponse;

    MusicTimeSyncResponse()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const MusicTimeSyncResponse& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && ClientTimeMs == acRhs.ClientTimeMs && ServerTimeMs == acRhs.ServerTimeMs; }

    double ClientTimeMs{};
    double ServerTimeMs{};
};
