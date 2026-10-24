#pragma once

#include <Services/MusicTools.h>

#include <Messages/NotifyMusicState.h>
#include <Messages/MusicCommandRequest.h>

struct World;
struct TransportService;

struct UpdateEvent;
struct ConnectedEvent;
struct DisconnectedEvent;
struct MusicTimeSyncResponse;

/**
 * @brief Shared YouTube music, the client side.
 *
 * The server keeps the queue and decides when a track starts on its clock. This service works out how far
 * the local clock is off from the server one, downloads the tracks with yt-dlp and hands everything to the UI,
 * which plays the audio and keeps it in line with the server clock.
 */
struct MusicService
{
    MusicService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~MusicService() noexcept;

    TP_NOCOPYMOVE(MusicService);

    // Calls from the UI.
    void Search(const std::string& acQuery) noexcept;
    void SendCommand(MusicCommandRequest::Action aCommand, uint32_t aTrackId, double aValue, const MusicTrackData& acTrack = {}) noexcept;
    void OnTrackLoaded(uint32_t aTrackId, bool aReady, uint32_t aDurationMs) noexcept;
    void InstallTools() noexcept;
    /**
     * @brief Sends everything to the UI again, after it (re)loaded.
     */
    void SyncUI() noexcept;

private:
    enum class ToolsState
    {
        kMissing,
        kInstalling,
        kReady,
        kFailed,
    };

    enum class FileState
    {
        kQueued,
        kDownloading,
        kReady,
        kFailed,
    };

    struct TrackFile
    {
        FileState State{FileState::kQueued};
        double Progress{};
        std::filesystem::path Path;
        std::string Error;
    };

    struct ClockSample
    {
        double RoundTripMs;
        double OffsetMs;
    };

    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnConnected(const ConnectedEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;
    void OnTimeSyncResponse(const MusicTimeSyncResponse& acMessage) noexcept;
    void OnMusicState(const NotifyMusicState& acMessage) noexcept;

    /**
     * @brief Installs the tools when missing, then runs the job on the download worker.
     */
    void EnsureTools() noexcept;
    void RequestFile(const std::string& acVideoId) noexcept;
    void OnFileChanged(const std::string& acVideoId) noexcept;

    void SendStateToUI() const noexcept;
    void SendClockToUI() const noexcept;
    void SendFileToUI(const std::string& acVideoId) const noexcept;
    void SendToolsToUI() const noexcept;

    World& m_world;
    TransportService& m_transport;

    NotifyMusicState m_state{};
    bool m_hasState{false};

    std::deque<ClockSample> m_clockSamples;
    double m_clockOffsetMs{};
    double m_clockRoundTripMs{};
    double m_nextClockSync{};
    uint32_t m_clockSyncsSent{};

    ToolsState m_toolsState{ToolsState::kMissing};
    std::string m_toolsError;
    // Read by the search worker.
    std::atomic_bool m_installingTools{false};
    bool m_toolsUpdated{false};
    TiltedPhoques::Map<std::string, TrackFile> m_files;
    // The video whose download failure was already reported for the current track.
    uint32_t m_reportedFailureTrackId{};

    uint32_t m_searchId{};

    // Downloads (and tool installs) one at a time, searches separately so they do not wait for downloads.
    TiltedPhoques::UniquePtr<MusicWorker> m_downloadWorker;
    TiltedPhoques::UniquePtr<MusicWorker> m_searchWorker;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_connectedConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_timeSyncConnection;
    entt::scoped_connection m_stateConnection;
};
