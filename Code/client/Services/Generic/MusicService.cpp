#include <Services/MusicService.h>
#include <Services/TransportService.h>
#include <Services/OverlayService.h>
#include <Services/RunnerService.h>

#include <World.h>
#include <OverlayApp.hpp>

#include <Events/UpdateEvent.h>
#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>

#include <Messages/MusicTimeSyncRequest.h>
#include <Messages/MusicTimeSyncResponse.h>
#include <Messages/MusicTrackStatusRequest.h>

namespace
{
// A quick burst right after connecting, then a sample every few seconds to follow the clocks drifting apart.
constexpr uint32_t kClockBurstSamples = 10;
constexpr double kClockBurstIntervalMs = 100.0;
constexpr double kClockIntervalMs = 2000.0;
constexpr size_t kMaxClockSamples = 16;
// Answers that took longer than this say more about the network than about the clock.
constexpr double kMaxClockRoundTripMs = 2000.0;

double WallClockMs() noexcept
{
    // The same clock as Date.now() in the UI.
    return std::chrono::duration<double, std::milli>(std::chrono::system_clock::now().time_since_epoch()).count();
}

CefRefPtr<CefDictionaryValue> ToDictionary(const MusicTrackData& acTrack) noexcept
{
    auto pTrack = CefDictionaryValue::Create();
    pTrack->SetInt("id", static_cast<int>(acTrack.Id));
    pTrack->SetString("videoId", acTrack.VideoId.c_str());
    pTrack->SetString("title", acTrack.Title.c_str());
    pTrack->SetString("author", acTrack.Author.c_str());
    pTrack->SetDouble("durationMs", acTrack.DurationMs);
    pTrack->SetInt("addedById", static_cast<int>(acTrack.AddedByPlayerId));
    pTrack->SetString("addedByName", acTrack.AddedByName.c_str());
    return pTrack;
}

const char* ToString(NotifyMusicState::PlaybackState aState) noexcept
{
    switch (aState)
    {
    case NotifyMusicState::kPreparing: return "preparing";
    case NotifyMusicState::kPlaying: return "playing";
    case NotifyMusicState::kPaused: return "paused";
    default: return "idle";
    }
}
} // namespace

MusicService::MusicService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_transport(aTransport)
    , m_downloadWorker(TiltedPhoques::MakeUnique<MusicWorker>())
    , m_searchWorker(TiltedPhoques::MakeUnique<MusicWorker>())
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&MusicService::OnUpdate>(this);
    m_connectedConnection = aDispatcher.sink<ConnectedEvent>().connect<&MusicService::OnConnected>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&MusicService::OnDisconnected>(this);
    m_timeSyncConnection = aDispatcher.sink<MusicTimeSyncResponse>().connect<&MusicService::OnTimeSyncResponse>(this);
    m_stateConnection = aDispatcher.sink<NotifyMusicState>().connect<&MusicService::OnMusicState>(this);

    // yt-dlp is Python, make it talk UTF-8 to us whatever the system code page is.
    SetEnvironmentVariableW(L"PYTHONUTF8", L"1");
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");

    m_toolsState = MusicTools::AreToolsInstalled() ? ToolsState::kReady : ToolsState::kMissing;
}

MusicService::~MusicService() noexcept
{
    // Stop the workers first, their jobs post back to this service.
    m_searchWorker.reset();
    m_downloadWorker.reset();
}

void MusicService::OnUpdate(const UpdateEvent&) noexcept
{
    if (!m_transport.IsOnline())
        return;

    const double cNow = WallClockMs();
    if (cNow < m_nextClockSync)
        return;

    m_nextClockSync = cNow + (m_clockSyncsSent < kClockBurstSamples ? kClockBurstIntervalMs : kClockIntervalMs);
    ++m_clockSyncsSent;

    MusicTimeSyncRequest request{};
    request.ClientTimeMs = cNow;
    m_transport.Send(request, TiltedPhoques::kUnreliableNoNagle);
}

void MusicService::OnConnected(const ConnectedEvent&) noexcept
{
    m_clockSamples.clear();
    m_clockSyncsSent = 0;
    m_nextClockSync = 0.0;
}

void MusicService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_state = {};
    m_hasState = false;
    m_clockSamples.clear();
    m_reportedFailureTrackId = 0;
    m_downloadWorker->ClearPending();

    // Downloads that did not start yet will be asked for again on the next server.
    for (auto it = m_files.begin(); it != m_files.end();)
    {
        if (it->second.State == FileState::kQueued)
            it = m_files.erase(it);
        else
            ++it;
    }

    SendStateToUI();
}

void MusicService::OnTimeSyncResponse(const MusicTimeSyncResponse& acMessage) noexcept
{
    const double cNow = WallClockMs();
    const double cRoundTrip = cNow - acMessage.ClientTimeMs;
    if (cRoundTrip < 0.0 || cRoundTrip > kMaxClockRoundTripMs)
        return;

    // Assumes the answer took as long as the request, the faster the round trip the less that can be off.
    m_clockSamples.push_back({cRoundTrip, acMessage.ServerTimeMs - (acMessage.ClientTimeMs + cRoundTrip / 2.0)});
    if (m_clockSamples.size() > kMaxClockSamples)
        m_clockSamples.pop_front();

    const auto cBest = std::min_element(m_clockSamples.begin(), m_clockSamples.end(), [](const ClockSample& acLhs, const ClockSample& acRhs) { return acLhs.RoundTripMs < acRhs.RoundTripMs; });

    m_clockOffsetMs = cBest->OffsetMs;
    m_clockRoundTripMs = cBest->RoundTripMs;

    SendClockToUI();
}

void MusicService::OnMusicState(const NotifyMusicState& acMessage) noexcept
{
    const bool cTrackChanged = !m_hasState || m_state.Current.Id != acMessage.Current.Id;

    m_state = acMessage;
    m_hasState = true;

    if (cTrackChanged)
        m_reportedFailureTrackId = 0;

    SendStateToUI();

    if (m_state.State == NotifyMusicState::kIdle)
        return;

    RequestFile(m_state.Current.VideoId.c_str());

    // Get the next one ready while this one plays.
    if (!m_state.Queue.empty())
        RequestFile(m_state.Queue.front().VideoId.c_str());

    // Already failed before, for instance as the next track, tell the server right away.
    OnFileChanged(m_state.Current.VideoId.c_str());
}

void MusicService::EnsureTools() noexcept
{
    if (m_toolsState == ToolsState::kReady || m_toolsState == ToolsState::kInstalling)
    {
        if (m_toolsState == ToolsState::kReady && !m_toolsUpdated)
        {
            m_toolsUpdated = true;
            m_downloadWorker->Post([this] { MusicTools::UpdateTools(*m_downloadWorker); });
        }
        return;
    }

    m_toolsState = ToolsState::kInstalling;
    m_installingTools = true;
    m_toolsError.clear();
    SendToolsToUI();

    // Freshly downloaded tools are the latest version already.
    m_toolsUpdated = true;

    m_downloadWorker->Post([this] {
        std::string error;
        const bool cInstalled = MusicTools::InstallTools(*m_downloadWorker, error);
        m_installingTools = false;

        if (!cInstalled)
            spdlog::error("[Music] Could not install yt-dlp and Deno: {}", error);

        m_world.GetRunner().Queue([this, cInstalled, error] {
            m_toolsState = cInstalled ? ToolsState::kReady : ToolsState::kFailed;
            m_toolsError = error;
            SendToolsToUI();
        });
    });
}

void MusicService::InstallTools() noexcept
{
    if (m_toolsState == ToolsState::kFailed)
        m_toolsState = ToolsState::kMissing;

    EnsureTools();
}

void MusicService::RequestFile(const std::string& acVideoId) noexcept
{
    if (acVideoId.empty())
        return;

    if (auto it = m_files.find(acVideoId); it != m_files.end())
    {
        // Retry failed downloads only once the track comes up again as the current one.
        if (it->second.State != FileState::kFailed || acVideoId != m_state.Current.VideoId.c_str() || m_reportedFailureTrackId != 0)
            return;
    }

    TrackFile& file = m_files[acVideoId];
    file = {};

    if (const auto cCached = MusicTools::FindCached(acVideoId); !cCached.empty())
    {
        file.State = FileState::kReady;
        file.Path = cCached;

        // Keeps recently played tracks from being trimmed first.
        std::error_code error;
        std::filesystem::last_write_time(cCached, std::filesystem::file_time_type::clock::now(), error);

        OnFileChanged(acVideoId);
        return;
    }

    EnsureTools();
    SendFileToUI(acVideoId);

    m_downloadWorker->Post([this, acVideoId] {
        if (!MusicTools::AreToolsInstalled())
        {
            m_world.GetRunner().Queue([this, acVideoId] {
                TrackFile& file = m_files[acVideoId];
                file.State = FileState::kFailed;
                file.Error = m_toolsError.empty() ? "yt-dlp is not installed" : m_toolsError;
                OnFileChanged(acVideoId);
            });
            return;
        }

        m_world.GetRunner().Queue([this, acVideoId] {
            m_files[acVideoId].State = FileState::kDownloading;
            SendFileToUI(acVideoId);
        });

        double lastProgress = -100.0;
        std::filesystem::path path;
        std::string error;

        const bool cDownloaded = MusicTools::Download(
            *m_downloadWorker, acVideoId,
            [&](double aProgress) {
                if (aProgress - lastProgress < 2.0 && aProgress < 100.0)
                    return;
                lastProgress = aProgress;

                m_world.GetRunner().Queue([this, acVideoId, aProgress] {
                    m_files[acVideoId].Progress = aProgress;
                    SendFileToUI(acVideoId);
                });
            },
            path, error);

        if (cDownloaded)
            spdlog::info("[Music] Downloaded {}", acVideoId);
        else
            spdlog::error("[Music] Could not download {}: {}", acVideoId, error);

        m_world.GetRunner().Queue([this, acVideoId, cDownloaded, path, error] {
            TrackFile& file = m_files[acVideoId];
            file.State = cDownloaded ? FileState::kReady : FileState::kFailed;
            file.Path = path;
            file.Error = error;
            file.Progress = cDownloaded ? 100.0 : 0.0;

            OnFileChanged(acVideoId);

            std::vector<std::string> keep{m_state.Current.VideoId.c_str()};
            if (!m_state.Queue.empty())
                keep.emplace_back(m_state.Queue.front().VideoId.c_str());

            m_downloadWorker->Post([keep] { MusicTools::TrimCache(keep); });
        });
    });
}

void MusicService::OnFileChanged(const std::string& acVideoId) noexcept
{
    SendFileToUI(acVideoId);

    const auto it = m_files.find(acVideoId);
    if (it == m_files.end() || it->second.State != FileState::kFailed)
        return;

    // A loaded track is reported by the UI once the audio is ready to play, a failed download right here.
    if (m_state.State != NotifyMusicState::kIdle && acVideoId == m_state.Current.VideoId.c_str() && m_reportedFailureTrackId != m_state.Current.Id)
    {
        m_reportedFailureTrackId = m_state.Current.Id;

        MusicTrackStatusRequest request{};
        request.TrackId = m_state.Current.Id;
        request.Ready = false;
        m_transport.Send(request);
    }
}

void MusicService::Search(const std::string& acQuery) noexcept
{
    const uint32_t cSearchId = ++m_searchId;

    EnsureTools();

    // Only the latest search matters.
    m_searchWorker->ClearPending();
    m_searchWorker->Post([this, acQuery, cSearchId] {
        // The download worker installs the tools, wait for it.
        while (m_installingTools && !m_searchWorker->IsStopping())
            std::this_thread::sleep_for(std::chrono::milliseconds(250));

        std::vector<MusicTools::SearchResult> results;
        std::string error;

        if (!MusicTools::AreToolsInstalled())
            error = "yt-dlp is not installed";
        else
            MusicTools::Search(*m_searchWorker, acQuery, results, error);

        m_world.GetRunner().Queue([this, acQuery, cSearchId, results = std::move(results), error] {
            if (cSearchId != m_searchId)
                return;

            auto* pOverlay = m_world.GetOverlayService().GetOverlayApp();
            if (!pOverlay)
                return;

            auto pList = CefListValue::Create();
            for (size_t i = 0; i < results.size(); ++i)
            {
                auto pResult = CefDictionaryValue::Create();
                pResult->SetString("videoId", results[i].VideoId);
                pResult->SetString("title", results[i].Title);
                pResult->SetString("author", results[i].Author);
                pResult->SetDouble("durationMs", results[i].DurationMs);
                pList->SetDictionary(i, pResult);
            }

            auto pArguments = CefListValue::Create();
            pArguments->SetString(0, acQuery);
            pArguments->SetList(1, pList);
            pArguments->SetString(2, error);
            pOverlay->ExecuteAsync("musicSearchResults", pArguments);
        });
    });
}

void MusicService::SendCommand(MusicCommandRequest::Action aCommand, uint32_t aTrackId, double aValue, const MusicTrackData& acTrack) noexcept
{
    if (!m_transport.IsOnline())
        return;

    MusicCommandRequest request{};
    request.Command = aCommand;
    request.TrackId = aTrackId;
    request.Value = aValue;
    request.Track = acTrack;
    m_transport.Send(request);
}

void MusicService::OnTrackLoaded(uint32_t aTrackId, bool aReady, uint32_t aDurationMs) noexcept
{
    if (!m_transport.IsOnline())
        return;

    if (!aReady)
        spdlog::error("[Music] The UI could not play track {}", aTrackId);

    MusicTrackStatusRequest request{};
    request.TrackId = aTrackId;
    request.Ready = aReady;
    request.DurationMs = aDurationMs;
    m_transport.Send(request);
}

void MusicService::SyncUI() noexcept
{
    SendToolsToUI();
    SendClockToUI();

    for (const auto& [videoId, file] : m_files)
        SendFileToUI(videoId);

    SendStateToUI();
}

void MusicService::SendStateToUI() const noexcept
{
    auto* pOverlay = m_world.GetOverlayService().GetOverlayApp();
    if (!pOverlay)
        return;

    auto pState = CefDictionaryValue::Create();
    pState->SetBool("connected", m_hasState && m_transport.IsOnline());
    pState->SetString("state", ToString(m_hasState ? m_state.State : NotifyMusicState::kIdle));
    pState->SetInt("hostPlayerId", static_cast<int>(m_state.HostPlayerId));
    pState->SetInt("localPlayerId", static_cast<int>(m_transport.GetLocalPlayerId()));
    pState->SetBool("isHost", m_hasState && m_state.HostPlayerId == m_transport.GetLocalPlayerId());
    pState->SetDouble("startServerTimeMs", m_state.StartServerTimeMs);
    pState->SetDouble("positionMs", m_state.PositionMs);
    pState->SetInt("readyCount", static_cast<int>(m_state.ReadyCount));
    pState->SetInt("playerCount", static_cast<int>(m_state.PlayerCount));

    if (m_hasState && m_state.State != NotifyMusicState::kIdle)
        pState->SetDictionary("current", ToDictionary(m_state.Current));
    else
        pState->SetNull("current");

    auto pQueue = CefListValue::Create();
    for (size_t i = 0; i < m_state.Queue.size(); ++i)
        pQueue->SetDictionary(i, ToDictionary(m_state.Queue[i]));
    pState->SetList("queue", pQueue);

    auto pArguments = CefListValue::Create();
    pArguments->SetDictionary(0, pState);
    pOverlay->ExecuteAsync("musicState", pArguments);
}

void MusicService::SendClockToUI() const noexcept
{
    auto* pOverlay = m_world.GetOverlayService().GetOverlayApp();
    if (!pOverlay || m_clockSamples.empty())
        return;

    auto pArguments = CefListValue::Create();
    pArguments->SetDouble(0, m_clockOffsetMs);
    pArguments->SetDouble(1, m_clockRoundTripMs);
    pOverlay->ExecuteAsync("musicClock", pArguments);
}

void MusicService::SendFileToUI(const std::string& acVideoId) const noexcept
{
    auto* pOverlay = m_world.GetOverlayService().GetOverlayApp();
    if (!pOverlay)
        return;

    const auto it = m_files.find(acVideoId);
    if (it == m_files.end())
        return;

    const TrackFile& file = it->second;

    const char* pState = "queued";
    switch (file.State)
    {
    case FileState::kDownloading: pState = "downloading"; break;
    case FileState::kReady: pState = "ready"; break;
    case FileState::kFailed: pState = "failed"; break;
    default: break;
    }

    auto pArguments = CefListValue::Create();
    pArguments->SetString(0, acVideoId);
    pArguments->SetString(1, pState);
    pArguments->SetDouble(2, file.Progress);
    pArguments->SetString(3, file.Path.wstring());
    pArguments->SetString(4, file.Error);
    pOverlay->ExecuteAsync("musicFile", pArguments);
}

void MusicService::SendToolsToUI() const noexcept
{
    auto* pOverlay = m_world.GetOverlayService().GetOverlayApp();
    if (!pOverlay)
        return;

    const char* pState = "missing";
    switch (m_toolsState)
    {
    case ToolsState::kInstalling: pState = "installing"; break;
    case ToolsState::kReady: pState = "ready"; break;
    case ToolsState::kFailed: pState = "failed"; break;
    default: break;
    }

    auto pArguments = CefListValue::Create();
    pArguments->SetString(0, pState);
    pArguments->SetString(1, m_toolsError);
    pOverlay->ExecuteAsync("musicTools", pArguments);
}
