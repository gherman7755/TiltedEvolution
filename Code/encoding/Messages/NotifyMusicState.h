#pragma once

#include "Message.h"

#include <Structs/MusicTrackData.h>

using TiltedPhoques::Vector;

/**
 * @brief The whole shared music state, sent whenever anything about it changes.
 */
struct NotifyMusicState final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyMusicState;

    NotifyMusicState()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyMusicState& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && HostPlayerId == acRhs.HostPlayerId && State == acRhs.State && Current == acRhs.Current && StartServerTimeMs == acRhs.StartServerTimeMs &&
               PositionMs == acRhs.PositionMs && ReadyCount == acRhs.ReadyCount && PlayerCount == acRhs.PlayerCount && Queue == acRhs.Queue; }

    enum PlaybackState : uint8_t
    {
        kIdle,
        // Waiting for the players to download the current track.
        kPreparing,
        kPlaying,
        kPaused,
    };

    uint32_t HostPlayerId{};
    PlaybackState State{kIdle};
    // Only meaningful when not idle.
    MusicTrackData Current{};
    // Server time in ms at which the current track was (or will be) at position 0, while playing.
    double StartServerTimeMs{};
    // Position in ms while paused or preparing.
    double PositionMs{};
    uint32_t ReadyCount{};
    uint32_t PlayerCount{};
    Vector<MusicTrackData> Queue{};
};
