#pragma once

#include "Message.h"

/**
 * @brief Asks the server for its music clock, to work out how far the local clock is off.
 */
struct MusicTimeSyncRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kMusicTimeSyncRequest;

    MusicTimeSyncRequest()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const MusicTimeSyncRequest& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && ClientTimeMs == acRhs.ClientTimeMs; }

    // Local wall clock in ms when the request was sent, echoed back by the server.
    double ClientTimeMs{};
};
