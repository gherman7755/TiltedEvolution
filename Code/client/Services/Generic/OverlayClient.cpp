#include <TiltedOnlinePCH.h>

#include <OverlayRenderHandler.hpp>
#include <DInputHook.hpp>

#include <Services/OverlayClient.h>
#include <Services/TransportService.h>
#include <Services/MusicService.h>

#include <Messages/SendChatMessageRequest.h>
#include <Messages/TeleportRequest.h>

#include <Events/SetTimeCommandEvent.h>

#include <World.h>

OverlayClient::OverlayClient(TransportService& aTransport, TiltedPhoques::OverlayRenderHandler* apHandler)
    : TiltedPhoques::OverlayClient(apHandler)
    , m_transport(aTransport)
{
}

OverlayClient::~OverlayClient() noexcept
{
}

bool OverlayClient::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source_process, CefRefPtr<CefProcessMessage> message)
{
    if (message->GetName() == "ui-event")
    {
        auto pArguments = message->GetArgumentList();

        auto eventName = pArguments->GetString(0).ToString();
        auto eventArgs = pArguments->GetList(1);

        spdlog::info(eventName);
        spdlog::info(eventArgs->GetString(0).ToString());
        spdlog::info(std::to_string(eventArgs->GetInt(1)));
        spdlog::info(eventArgs->GetString(2).ToString());

#ifndef PUBLIC_BUILD
        LOG(INFO) << "event=ui_event name=" << eventName;
#endif

        if (eventName == "connect")
            ProcessConnectMessage(eventArgs);
        else if (eventName == "disconnect")
            ProcessDisconnectMessage();
        else if (eventName == "revealPlayers")
            ProcessRevealPlayersMessage();
        else if (eventName == "sendMessage")
            ProcessChatMessage(eventArgs);
        else if (eventName == "setTime")
            ProcessSetTimeCommand(eventArgs);
        else if (eventName == "launchParty")
            World::Get().GetPartyService().CreateParty();
        else if (eventName == "leaveParty")
            World::Get().GetPartyService().LeaveParty();
        else if (eventName == "createPartyInvite")
        {
            uint32_t aPlayerId = eventArgs->GetInt(0);
            World::Get().GetPartyService().CreateInvite(aPlayerId);
        }
        else if (eventName == "acceptPartyInvite")
        {
            uint32_t aInviterId = eventArgs->GetInt(0);
            // push to main thread because the party service has to check validity of invite thread safely
            World::Get().GetRunner().Queue([aInviterId]() { World::Get().GetPartyService().AcceptInvite(aInviterId); });
        }
        else if (eventName == "kickPartyMember")
        {
            uint32_t aPlayerId = eventArgs->GetInt(0);
            World::Get().GetPartyService().KickPartyMember(aPlayerId);
        }
        else if (eventName == "changePartyLeader")
        {
            uint32_t aPlayerId = eventArgs->GetInt(0);
            World::Get().GetPartyService().ChangePartyLeader(aPlayerId);
        }
        else if (eventName == "teleportToPlayer")
            ProcessTeleportMessage(eventArgs);
        else if (eventName == "toggleDebugUI")
            ProcessToggleDebugUI();
        else if (eventName == "openPlayGuide")
            ProcessOpenPlayGuide();
        else if (eventName.starts_with("music"))
            ProcessMusicMessage(eventName, eventArgs);

        return true;
    }

    return false;
}

void OverlayClient::ProcessConnectMessage(CefRefPtr<CefListValue> aEventArgs)
{
    std::string baseIp = aEventArgs->GetString(0);
    if (baseIp == "localhost")
    {
        baseIp = "127.0.0.1";
    }

    uint16_t port = aEventArgs->GetInt(1) ? aEventArgs->GetInt(1) : 10578;
    World::Get().GetTransport().SetServerPassword(aEventArgs->GetString(2));

    std::string endpoint = baseIp + ":" + std::to_string(port);

    World::Get().GetRunner().Queue([endpoint] { World::Get().GetTransport().Connect(endpoint); });
}

void OverlayClient::ProcessDisconnectMessage()
{
    World::Get().GetRunner().Queue([]() { World::Get().GetTransport().Close(); });
}

void OverlayClient::ProcessRevealPlayersMessage()
{
    SetUIVisible(false);
    World::Get().GetMagicService().StartRevealingOtherPlayers();
}

void OverlayClient::ProcessChatMessage(CefRefPtr<CefListValue> aEventArgs)
{
    std::string contents = aEventArgs->GetString(1).ToString();
    if (!contents.empty())
    {
        SendChatMessageRequest messageRequest;
        messageRequest.MessageType = static_cast<ChatMessageType>(aEventArgs->GetInt(0));
        messageRequest.ChatMessage = contents;

        spdlog::info(L"Send chat message of type {}: '{}' ", messageRequest.MessageType, aEventArgs->GetString(1).ToWString());

        m_transport.Send(messageRequest);
    }
}

void OverlayClient::ProcessSetTimeCommand(CefRefPtr<CefListValue> aEventArgs)
{
    const uint8_t hours = static_cast<uint8_t>(aEventArgs->GetInt(0));
    const uint8_t minutes = static_cast<uint8_t>(aEventArgs->GetInt(1));
    const uint32_t senderId = m_transport.GetLocalPlayerId();
    World::Get().GetDispatcher().trigger(SetTimeCommandEvent(hours, minutes, senderId));
}

void OverlayClient::ProcessTeleportMessage(CefRefPtr<CefListValue> aEventArgs)
{
    TeleportRequest request{};
    request.PlayerId = aEventArgs->GetInt(0);

    m_transport.Send(request);
}

namespace
{
// JavaScript numbers arrive as ints or doubles depending on their value.
double GetNumber(const CefRefPtr<CefListValue>& acArguments, size_t aIndex) noexcept
{
    switch (acArguments->GetType(aIndex))
    {
    case VTYPE_INT: return acArguments->GetInt(aIndex);
    case VTYPE_DOUBLE: return acArguments->GetDouble(aIndex);
    default: return 0.0;
    }
}
} // namespace

void OverlayClient::ProcessMusicMessage(const std::string& acEventName, CefRefPtr<CefListValue> aEventArgs)
{
    // The music service lives on the main thread.
    if (acEventName == "musicSearch")
    {
        std::string query = aEventArgs->GetString(0).ToString();
        World::Get().GetRunner().Queue([query] { World::Get().ctx().at<MusicService>().Search(query); });
    }
    else if (acEventName == "musicAdd")
    {
        MusicTrackData track{};
        track.VideoId = aEventArgs->GetString(0).ToString().c_str();
        track.Title = aEventArgs->GetString(1).ToString().c_str();
        track.Author = aEventArgs->GetString(2).ToString().c_str();
        track.DurationMs = static_cast<uint32_t>(std::max(GetNumber(aEventArgs, 3), 0.0));

        World::Get().GetRunner().Queue([track] { World::Get().ctx().at<MusicService>().SendCommand(MusicCommandRequest::kAdd, 0, 0.0, track); });
    }
    else if (acEventName == "musicCommand")
    {
        const auto cCommand = static_cast<MusicCommandRequest::Action>(static_cast<int>(GetNumber(aEventArgs, 0)));
        const auto cTrackId = static_cast<uint32_t>(GetNumber(aEventArgs, 1));
        const double cValue = GetNumber(aEventArgs, 2);

        if (cCommand == MusicCommandRequest::kAdd || cCommand > MusicCommandRequest::kMove)
            return;

        World::Get().GetRunner().Queue([cCommand, cTrackId, cValue] { World::Get().ctx().at<MusicService>().SendCommand(cCommand, cTrackId, cValue); });
    }
    else if (acEventName == "musicTrackLoaded")
    {
        const auto cTrackId = static_cast<uint32_t>(GetNumber(aEventArgs, 0));
        const bool cReady = aEventArgs->GetBool(1);
        const auto cDurationMs = static_cast<uint32_t>(std::max(GetNumber(aEventArgs, 2), 0.0));

        World::Get().GetRunner().Queue([cTrackId, cReady, cDurationMs] { World::Get().ctx().at<MusicService>().OnTrackLoaded(cTrackId, cReady, cDurationMs); });
    }
    else if (acEventName == "musicInstallTools")
        World::Get().GetRunner().Queue([] { World::Get().ctx().at<MusicService>().InstallTools(); });
    else if (acEventName == "musicSync")
        World::Get().GetRunner().Queue([] { World::Get().ctx().at<MusicService>().SyncUI(); });
}

void OverlayClient::ProcessToggleDebugUI()
{
    World::Get().GetDebugService().m_showDebugStuff = !World::Get().GetDebugService().m_showDebugStuff;
}

void OverlayClient::ProcessOpenPlayGuide()
{
    ShellExecuteW(nullptr, L"open", LR"(https://wiki.tiltedphoques.com/tilted-online/general-information/playguide)", nullptr, nullptr, SW_SHOWNORMAL);
}

void OverlayClient::SetUIVisible(bool aVisible) noexcept
{
    auto pRenderer = GetOverlayRenderHandler();
    if (!pRenderer)
        return;

    TiltedPhoques::DInputHook::Get().SetEnabled(aVisible);
    World::Get().GetOverlayService().SetActive(aVisible);
    pRenderer->SetCursorVisible(aVisible);
}
