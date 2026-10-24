#pragma once

#include <Messages/NotifyPlayerStealth.h>

struct World;
struct TransportService;
struct Actor;

struct UpdateEvent;
struct DisconnectedEvent;

/**
 * @brief Lets NPCs simulated on this client see remote players the way their own game sees them.
 *
 * NPCs of the other clients detect this player through the copy of the player in their game. That copy is
 * an NPC moved by the network, so on its own the engine never treats it as sneaking, gives it none of the
 * player's sneak perks and thinks it is walking or running however its last movement packet looked. This
 * service sends the movement mode and the sneak perks of the local player, and applies the ones of the
 * remote players to their copies.
 */
struct StealthService
{
    StealthService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~StealthService() noexcept = default;

    TP_NOCOPYMOVE(StealthService);

private:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;
    void OnNotifyPlayerStealth(const NotifyPlayerStealth& acMessage) noexcept;

    void SendLocalState() noexcept;
    void ApplyRemoteStates() noexcept;
    static bool ApplyPerks(Actor* apActor, const Vector<uint32_t>& acPerkIds) noexcept;

    struct RemoteState
    {
        uint32_t MovementFlags{};
        // Game form ids.
        Vector<uint32_t> Perks{};
        // The copy the perks were last written to, a respawned copy has a new base.
        uint32_t PerksAppliedTo{};
        Vector<uint32_t> AppliedPerks{};
    };

    World& m_world;
    TransportService& m_transport;

    uint32_t m_sentMovementFlags{};
    Vector<GameId> m_sentPerks{};
    std::chrono::steady_clock::time_point m_lastSend{};
    std::chrono::steady_clock::time_point m_lastPerkScan{};
    Vector<GameId> m_localPerks{};

    // Server id of the remote player's character -> what they sent.
    Map<uint32_t, RemoteState> m_remoteStates{};

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_stealthConnection;
};
