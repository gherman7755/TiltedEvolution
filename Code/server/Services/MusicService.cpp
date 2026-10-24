#include <Services/MusicService.h>

#include <GameServer.h>
#include <World.h>

#include <Events/UpdateEvent.h>
#include <Events/PlayerJoinEvent.h>
#include <Events/PlayerLeaveEvent.h>

#include <Messages/MusicTimeSyncRequest.h>
#include <Messages/MusicTimeSyncResponse.h>
#include <Messages/MusicCommandRequest.h>
#include <Messages/MusicTrackStatusRequest.h>

#include <Setting.h>
namespace
{
Console::Setting bEnableMusic{"Music:bEnableMusic", "Lets players listen to YouTube tracks together, in sync", true};
Console::Setting uMaxQueuedTracks{"Music:uMaxQueuedTracks", "How many tracks can wait in the music queue", 100u};
Console::Setting uPrepareTimeoutMs{"Music:uPrepareTimeoutMs", "How long to wait for every player to download a track before starting it without the slow ones", 20000u};

// How far in the future playback starts, so that every client gets the message in time and starts at once.
constexpr double kStartLeadMs = 800.0;
// Tracks are considered over a little after their length, the clients stop on their own at the end of the file.
constexpr double kEndGraceMs = 500.0;
constexpr double kHostCheckIntervalMs = 1000.0;
constexpr size_t kMaxTextLength = 200;

bool IsValidVideoId(const String& acVideoId) noexcept
{
    if (acVideoId.size() != 11)
        return false;

    return std::all_of(acVideoId.begin(), acVideoId.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'; });
}

String Truncate(const String& acText) noexcept
{
    if (acText.size() <= kMaxTextLength)
        return acText;

    // Do not cut a UTF-8 sequence in half.
    size_t size = kMaxTextLength;
    while (size > 0 && (static_cast<unsigned char>(acText[size]) & 0xC0) == 0x80)
        --size;

    return acText.substr(0, size);
}
} // namespace

MusicService::MusicService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_epoch(std::chrono::steady_clock::now())
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&MusicService::OnUpdate>(this);
    m_joinConnection = aDispatcher.sink<PlayerJoinEvent>().connect<&MusicService::OnPlayerJoin>(this);
    m_leaveConnection = aDispatcher.sink<PlayerLeaveEvent>().connect<&MusicService::OnPlayerLeave>(this);
    m_timeSyncConnection = aDispatcher.sink<PacketEvent<MusicTimeSyncRequest>>().connect<&MusicService::OnTimeSync>(this);
    m_commandConnection = aDispatcher.sink<PacketEvent<MusicCommandRequest>>().connect<&MusicService::OnCommand>(this);
    m_trackStatusConnection = aDispatcher.sink<PacketEvent<MusicTrackStatusRequest>>().connect<&MusicService::OnTrackStatus>(this);
}

double MusicService::Now() const noexcept
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_epoch).count();
}

uint32_t MusicService::FindHost() const noexcept
{
    const auto& adminSessions = GameServer::Get()->GetAdminSessions();

    for (const uint32_t cPlayerId : m_players)
    {
        const Player* pPlayer = m_world.GetPlayerManager().GetById(cPlayerId);
        if (pPlayer && adminSessions.contains(pPlayer->GetConnectionId()))
            return cPlayerId;
    }

    return m_players.empty() ? 0 : m_players.front();
}

uint32_t MusicService::GetCurrentDuration() const noexcept
{
    return m_measuredDurationMs ? m_measuredDurationMs : m_current.DurationMs;
}

double MusicService::GetCurrentPosition() const noexcept
{
    if (m_state != NotifyMusicState::kPlaying)
        return m_positionMs;

    double position = std::max(Now() - m_startServerTimeMs, 0.0);
    if (const uint32_t cDuration = GetCurrentDuration())
        position = std::min(position, static_cast<double>(cDuration));

    return position;
}

NotifyMusicState MusicService::BuildState() const noexcept
{
    NotifyMusicState state{};
    state.HostPlayerId = m_hostId;
    state.State = m_state;
    state.Current = m_current;
    state.Current.DurationMs = GetCurrentDuration();
    state.StartServerTimeMs = m_startServerTimeMs;
    state.PositionMs = m_positionMs;
    state.ReadyCount = static_cast<uint32_t>(m_readyPlayers.size());
    state.PlayerCount = static_cast<uint32_t>(m_players.size());
    state.Queue = m_queue;

    return state;
}

void MusicService::Broadcast() const noexcept
{
    const NotifyMusicState cState = BuildState();

    for (const uint32_t cPlayerId : m_players)
    {
        if (Player* pPlayer = m_world.GetPlayerManager().GetById(cPlayerId))
            pPlayer->Send(cState);
    }
}

void MusicService::OnUpdate(const UpdateEvent&) noexcept
{
    const double cNow = Now();

    if (cNow >= m_nextHostCheck)
    {
        m_nextHostCheck = cNow + kHostCheckIntervalMs;

        // Admin rights can change at any time through the console.
        if (const uint32_t cHostId = FindHost(); cHostId != m_hostId)
        {
            m_hostId = cHostId;
            Broadcast();
        }
    }

    if (m_state == NotifyMusicState::kPreparing && cNow >= m_prepareDeadline)
    {
        spdlog::info("[Music] Not every player loaded '{}' in time, starting without them", m_current.Title.c_str());
        m_prepareDeadline = std::numeric_limits<double>::max();

        if (m_readyPlayers.empty())
            PlayNext();
        else
            BeginPlayback(m_positionMs);
    }
    else if (m_state == NotifyMusicState::kPlaying)
    {
        const uint32_t cDuration = GetCurrentDuration();
        if (cDuration && cNow >= m_startServerTimeMs + cDuration + kEndGraceMs)
            PlayNext();
    }
}

void MusicService::OnPlayerJoin(const PlayerJoinEvent& acEvent) noexcept
{
    const uint32_t cPlayerId = acEvent.pPlayer->GetId();
    if (std::find(m_players.begin(), m_players.end(), cPlayerId) == m_players.end())
        m_players.push_back(cPlayerId);

    m_hostId = FindHost();

    // Everyone sees the new player count, and the host may have changed.
    Broadcast();
}

void MusicService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    const uint32_t cPlayerId = acEvent.pPlayer->GetId();

    std::erase(m_players, cPlayerId);
    m_readyPlayers.erase(cPlayerId);
    m_failedPlayers.erase(cPlayerId);

    m_hostId = FindHost();

    // Nobody is listening anymore, keep the spot for whoever comes back.
    if (m_players.empty() && m_state == NotifyMusicState::kPlaying)
    {
        m_positionMs = GetCurrentPosition();
        m_state = NotifyMusicState::kPaused;
    }

    if (m_state == NotifyMusicState::kPreparing)
        CheckPrepared();

    Broadcast();
}

void MusicService::OnTimeSync(const PacketEvent<MusicTimeSyncRequest>& acMessage) const noexcept
{
    MusicTimeSyncResponse response{};
    response.ClientTimeMs = acMessage.Packet.ClientTimeMs;
    response.ServerTimeMs = Now();

    // Any delay on the way back makes the clock estimate worse, so skip Nagle's algorithm.
    GameServer::Get()->Send(acMessage.pPlayer->GetConnectionId(), response, TiltedPhoques::kUnreliableNoNagle);
}

void MusicService::OnCommand(const PacketEvent<MusicCommandRequest>& acMessage) noexcept
{
    if (!bEnableMusic)
        return;

    const auto& message = acMessage.Packet;
    Player* pPlayer = acMessage.pPlayer;
    const bool cIsHost = pPlayer->GetId() == m_hostId;

    auto queueIt = std::find_if(m_queue.begin(), m_queue.end(), [&message](const MusicTrackData& acTrack) { return acTrack.Id == message.TrackId; });
    const bool cIsQueued = message.TrackId != 0 && queueIt != m_queue.end();

    switch (message.Command)
    {
    case MusicCommandRequest::kAdd:
    {
        if (!IsValidVideoId(message.Track.VideoId))
        {
            spdlog::warn("[Music] Player {:X} tried to add an invalid video id", pPlayer->GetId());
            return;
        }

        if (m_queue.size() >= std::max(uMaxQueuedTracks.value_as<uint32_t>(), 1u))
            return;

        MusicTrackData track = message.Track;
        track.Id = m_nextTrackId++;
        track.Title = Truncate(track.Title);
        track.Author = Truncate(track.Author);
        track.AddedByPlayerId = pPlayer->GetId();
        track.AddedByName = pPlayer->GetUsername();

        spdlog::info("[Music] {} added '{}' ({})", pPlayer->GetUsername().c_str(), track.Title.c_str(), track.VideoId.c_str());

        m_queue.push_back(std::move(track));

        if (m_state == NotifyMusicState::kIdle)
        {
            PlayNext();
            return;
        }
        break;
    }
    case MusicCommandRequest::kRemove:
    {
        if (cIsQueued && (cIsHost || queueIt->AddedByPlayerId == pPlayer->GetId()))
        {
            m_queue.erase(queueIt);
            break;
        }

        if (cIsHost && m_state != NotifyMusicState::kIdle && message.TrackId == m_current.Id)
        {
            PlayNext();
            return;
        }
        return;
    }
    case MusicCommandRequest::kPlay:
    {
        if (!cIsHost || !cIsQueued)
            return;

        MusicTrackData track = std::move(*queueIt);
        m_queue.erase(queueIt);
        m_queue.insert(m_queue.begin(), std::move(track));

        PlayNext();
        return;
    }
    case MusicCommandRequest::kNext:
    {
        if (!cIsHost || m_state == NotifyMusicState::kIdle)
            return;

        PlayNext();
        return;
    }
    case MusicCommandRequest::kPause:
    {
        if (!cIsHost || m_state != NotifyMusicState::kPlaying)
            return;

        m_positionMs = GetCurrentPosition();
        m_state = NotifyMusicState::kPaused;
        break;
    }
    case MusicCommandRequest::kResume:
    {
        if (!cIsHost || m_state != NotifyMusicState::kPaused)
            return;

        BeginPlayback(m_positionMs);
        return;
    }
    case MusicCommandRequest::kSeek:
    {
        if (!cIsHost || m_state == NotifyMusicState::kIdle || !std::isfinite(message.Value))
            return;

        double position = std::max(message.Value, 0.0);
        if (const uint32_t cDuration = GetCurrentDuration())
            position = std::min(position, static_cast<double>(cDuration));

        if (m_state == NotifyMusicState::kPlaying)
        {
            BeginPlayback(position);
            return;
        }

        m_positionMs = position;
        break;
    }
    case MusicCommandRequest::kMove:
    {
        if (!cIsHost || !cIsQueued || !std::isfinite(message.Value))
            return;

        MusicTrackData track = std::move(*queueIt);
        m_queue.erase(queueIt);

        const size_t cIndex = static_cast<size_t>(std::clamp(message.Value, 0.0, static_cast<double>(m_queue.size())));
        m_queue.insert(m_queue.begin() + cIndex, std::move(track));
        break;
    }
    default: return;
    }

    Broadcast();
}

void MusicService::OnTrackStatus(const PacketEvent<MusicTrackStatusRequest>& acMessage) noexcept
{
    const auto& message = acMessage.Packet;
    if (m_state == NotifyMusicState::kIdle || message.TrackId != m_current.Id)
        return;

    const uint32_t cPlayerId = acMessage.pPlayer->GetId();

    if (message.Ready)
    {
        m_readyPlayers.insert(cPlayerId);
        m_failedPlayers.erase(cPlayerId);

        if (!m_measuredDurationMs && message.DurationMs)
            m_measuredDurationMs = message.DurationMs;
    }
    else
    {
        spdlog::info("[Music] {} could not load '{}'", acMessage.pPlayer->GetUsername().c_str(), m_current.Title.c_str());
        m_failedPlayers.insert(cPlayerId);
        m_readyPlayers.erase(cPlayerId);
    }

    if (m_state == NotifyMusicState::kPreparing)
        CheckPrepared();
    else
        Broadcast();
}

void MusicService::PlayNext() noexcept
{
    if (m_queue.empty())
    {
        m_state = NotifyMusicState::kIdle;
        m_current = {};
        m_positionMs = 0.0;
        m_startServerTimeMs = 0.0;
        m_measuredDurationMs = 0;
        m_readyPlayers.clear();
        m_failedPlayers.clear();

        Broadcast();
        return;
    }

    m_current = std::move(m_queue.front());
    m_queue.erase(m_queue.begin());

    spdlog::info("[Music] Now playing '{}' ({})", m_current.Title.c_str(), m_current.VideoId.c_str());

    Prepare();
}

void MusicService::Prepare() noexcept
{
    m_state = NotifyMusicState::kPreparing;
    m_positionMs = 0.0;
    m_startServerTimeMs = 0.0;
    m_measuredDurationMs = 0;
    m_readyPlayers.clear();
    m_failedPlayers.clear();
    m_prepareDeadline = Now() + uPrepareTimeoutMs.value_as<uint32_t>();

    Broadcast();
}

void MusicService::BeginPlayback(double aPositionMs) noexcept
{
    m_state = NotifyMusicState::kPlaying;
    m_positionMs = 0.0;
    m_startServerTimeMs = Now() + kStartLeadMs - aPositionMs;

    Broadcast();
}

void MusicService::CheckPrepared() noexcept
{
    for (const uint32_t cPlayerId : m_players)
    {
        if (!m_readyPlayers.contains(cPlayerId) && !m_failedPlayers.contains(cPlayerId))
        {
            Broadcast();
            return;
        }
    }

    if (m_players.empty())
    {
        Broadcast();
        return;
    }

    if (m_readyPlayers.empty())
    {
        spdlog::info("[Music] Nobody could load '{}', skipping it", m_current.Title.c_str());
        PlayNext();
        return;
    }

    BeginPlayback(m_positionMs);
}
