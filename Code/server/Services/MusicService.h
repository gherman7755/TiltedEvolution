#pragma once

#include <Events/PacketEvent.h>

#include <Messages/NotifyMusicState.h>

struct World;
struct Player;
struct UpdateEvent;
struct PlayerJoinEvent;
struct PlayerLeaveEvent;
struct MusicTimeSyncRequest;
struct MusicCommandRequest;
struct MusicTrackStatusRequest;

/**
 * @brief Shared YouTube music for everyone on the server.
 *
 * The server owns the queue and the playback clock. Every client downloads the current track itself,
 * and once everyone is ready the server picks a start time on its clock, which the clients follow.
 * Everyone can add tracks, only the music host (the first admin online, otherwise the player who has been
 * connected the longest) controls the queue and playback.
 */
class MusicService
{
public:
    MusicService(World& aWorld, entt::dispatcher& aDispatcher);

private:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnPlayerJoin(const PlayerJoinEvent& acEvent) noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept;
    void OnTimeSync(const PacketEvent<MusicTimeSyncRequest>& acMessage) const noexcept;
    void OnCommand(const PacketEvent<MusicCommandRequest>& acMessage) noexcept;
    void OnTrackStatus(const PacketEvent<MusicTrackStatusRequest>& acMessage) noexcept;

    /**
     * @brief The music clock in ms, every playback time the clients get is on this clock.
     */
    [[nodiscard]] double Now() const noexcept;
    [[nodiscard]] uint32_t FindHost() const noexcept;
    [[nodiscard]] uint32_t GetCurrentDuration() const noexcept;
    [[nodiscard]] double GetCurrentPosition() const noexcept;
    [[nodiscard]] NotifyMusicState BuildState() const noexcept;

    /**
     * @brief Makes the first queued track the current one, or stops if the queue is empty.
     */
    void PlayNext() noexcept;
    /**
     * @brief Waits for every player to load the current track.
     */
    void Prepare() noexcept;
    /**
     * @brief Starts playing the current track from the given position, slightly in the future so that everyone starts at once.
     */
    void BeginPlayback(double aPositionMs) noexcept;
    /**
     * @brief Starts playing once every player loaded the current track, or failed to.
     */
    void CheckPrepared() noexcept;
    void Broadcast() const noexcept;

    World& m_world;
    std::chrono::steady_clock::time_point m_epoch;

    // Players that joined, in join order.
    Vector<uint32_t> m_players;
    uint32_t m_hostId{};
    double m_nextHostCheck{};
    uint32_t m_nextTrackId{1};

    NotifyMusicState::PlaybackState m_state{NotifyMusicState::kIdle};
    MusicTrackData m_current{};
    Vector<MusicTrackData> m_queue;
    double m_startServerTimeMs{};
    double m_positionMs{};
    double m_prepareDeadline{};
    // Length of the current track as measured by a client, more precise than the search metadata.
    uint32_t m_measuredDurationMs{};
    TiltedPhoques::Set<uint32_t> m_readyPlayers;
    TiltedPhoques::Set<uint32_t> m_failedPlayers;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_joinConnection;
    entt::scoped_connection m_leaveConnection;
    entt::scoped_connection m_timeSyncConnection;
    entt::scoped_connection m_commandConnection;
    entt::scoped_connection m_trackStatusConnection;
};
