#include <TiltedOnlinePCH.h>

#include <Services/StealthService.h>
#include <Services/TransportService.h>

#include <World.h>
#include <Utils.h>
#include <Games/Memory.h>
#include <Forms/TESNPC.h>
#include <PlayerCharacter.h>
#include <Games/ActorExtension.h>

#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>

#include <Messages/PlayerStealthRequest.h>

namespace
{
// ActorState flags1 bits: walking, running, sprinting and sneaking. They decide how much noise the actor makes
// and whether its stealth is used at all.
constexpr uint32_t kMovementFlagsMask = (1u << 6) | (1u << 7) | (1u << 8) | (1u << 9);

// The Sneak tree in Skyrim.esm, every rank of Stealth is its own perk.
constexpr uint32_t kSneakPerks[] = {
    0xBE126, 0xC07C6, 0xC07C7, 0xC07C8, 0xC07C9, // Stealth
    0x58210,                                     // Backstab
    0x1036F0,                                    // Deadly Aim
    0x58211,                                     // Assassin's Blade
    0x58213,                                     // Muffled Movement
    0x5820C,                                     // Light Foot
    0x105F23,                                    // Silent Roll
    0x105F24,                                    // Silence
    0x58214,                                     // Shadow Warrior
};

constexpr auto kMinSendInterval = std::chrono::milliseconds(100);
// Also resent unchanged, so that players who just came into range get it.
constexpr auto kResendInterval = std::chrono::seconds(3);
constexpr auto kPerkScanInterval = std::chrono::seconds(2);

// Actor vtable, same index in CommonLibSSE-NG.
constexpr size_t kApplyPerksFromBaseIndex = 0x101;

bool IsSneakPerk(uint32_t aFormId) noexcept
{
    return std::find(std::begin(kSneakPerks), std::end(kSneakPerks), aFormId) != std::end(kSneakPerks);
}

// Entry of BGSPerkRankArray::perkRanks.
struct PerkRankData
{
    TESForm* pPerk;
    int8_t CurrentRank;
    uint8_t pad9[7];
};
static_assert(sizeof(PerkRankData) == 0x10);
} // namespace

StealthService::StealthService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&StealthService::OnUpdate>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&StealthService::OnDisconnected>(this);
    m_stealthConnection = aDispatcher.sink<NotifyPlayerStealth>().connect<&StealthService::OnNotifyPlayerStealth>(this);
}

void StealthService::OnUpdate(const UpdateEvent&) noexcept
{
    if (!m_transport.IsOnline())
        return;

    SendLocalState();
    ApplyRemoteStates();
}

void StealthService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_remoteStates.clear();
    m_sentMovementFlags = 0;
    m_sentPerks.clear();
    m_lastSend = {};
}

void StealthService::OnNotifyPlayerStealth(const NotifyPlayerStealth& acMessage) noexcept
{
    auto& state = m_remoteStates[acMessage.ServerId];
    state.MovementFlags = acMessage.MovementFlags & kMovementFlagsMask;

    state.Perks.clear();
    for (const auto& perk : acMessage.Perks)
    {
        // Anything else would let a client write any perk into the copy.
        const uint32_t cFormId = m_world.GetModSystem().GetGameId(perk);
        if (cFormId && IsSneakPerk(cFormId))
            state.Perks.push_back(cFormId);
    }
}

void StealthService::SendLocalState() noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    const auto cNow = std::chrono::steady_clock::now();

    if (cNow - m_lastPerkScan >= kPerkScanInterval)
    {
        m_lastPerkScan = cNow;
        m_localPerks.clear();

        for (const uint32_t cPerkId : kSneakPerks)
        {
            GameId perkId{};
            if (pPlayer->GetPerkRank(cPerkId) > 0 && m_world.GetModSystem().GetServerModId(cPerkId, perkId))
                m_localPerks.push_back(perkId);
        }
    }

    const uint32_t cMovementFlags = pPlayer->actorState.flags1 & kMovementFlagsMask;
    const bool cChanged = cMovementFlags != m_sentMovementFlags || m_localPerks != m_sentPerks;

    if (cNow - m_lastSend < (cChanged ? kMinSendInterval : kResendInterval))
        return;

    PlayerStealthRequest request{};
    request.MovementFlags = cMovementFlags;
    request.Perks = m_localPerks;

    if (!m_transport.Send(request))
        return;

    m_lastSend = cNow;
    m_sentMovementFlags = cMovementFlags;
    m_sentPerks = m_localPerks;
}

void StealthService::ApplyRemoteStates() noexcept
{
    for (auto it = m_remoteStates.begin(); it != m_remoteStates.end(); ++it)
    {
        // hopscotch_map only hands out mutable values through value().
        auto& state = it.value();

        Actor* pActor = Utils::GetByServerId<Actor>(it->first);
        if (!pActor || !pActor->GetExtension()->IsRemotePlayer())
            continue;

        // The engine never sets these on the copy, which is moved by the network rather than its AI, so set them every frame.
        pActor->actorState.flags1 = (pActor->actorState.flags1 & ~kMovementFlagsMask) | state.MovementFlags;

        // Base perks are applied to the loaded actor, a copy without 3D gets them once it loads.
        if (!pActor->GetNiNode())
            continue;

        if (state.PerksAppliedTo == pActor->formID && state.AppliedPerks == state.Perks)
            continue;

        state.PerksAppliedTo = pActor->formID;
        state.AppliedPerks = state.Perks;

        if (ApplyPerks(pActor, state.Perks))
            spdlog::info("Applied {} sneak perks to remote player {:X}", state.Perks.size(), pActor->formID);
    }
}

bool StealthService::ApplyPerks(Actor* apActor, const Vector<uint32_t>& acPerkIds) noexcept
{
    // NPCs only get perks from their base, Actor::AddPerk does nothing for them. The copy of a remote player
    // has a base of its own, made from the player's appearance, so it can be changed without touching anyone else.
    auto* pNpc = Cast<TESNPC>(apActor->baseForm);
    if (!pNpc || !pNpc->IsTemporary())
    {
        spdlog::warn("Not giving perks to remote player {:X}, its base is not its own", apActor->formID);
        return false;
    }

    auto& perkRanks = pNpc->perkRanks;
    auto* pOldEntries = static_cast<PerkRankData*>(perkRanks.perkRanks);

    Vector<PerkRankData> entries;
    for (uint32_t i = 0; pOldEntries && i < perkRanks.count; ++i)
    {
        if (pOldEntries[i].pPerk && !IsSneakPerk(pOldEntries[i].pPerk->formID))
            entries.push_back(pOldEntries[i]);
    }

    for (const uint32_t cPerkId : acPerkIds)
    {
        TESForm* pPerk = TESForm::GetById(cPerkId);
        if (pPerk && Cast<BGSPerk>(pPerk))
            entries.push_back(PerkRankData{pPerk, 1, {}});
    }

    PerkRankData* pNewEntries = nullptr;
    if (!entries.empty())
    {
        pNewEntries = static_cast<PerkRankData*>(Memory::Allocate(sizeof(PerkRankData) * entries.size()));
        if (!pNewEntries)
            return false;

        std::copy(entries.begin(), entries.end(), pNewEntries);
    }

    perkRanks.perkRanks = pNewEntries;
    perkRanks.count = static_cast<uint32_t>(entries.size());

    if (pOldEntries)
        Memory::Free(pOldEntries);

    using TApplyPerksFromBase = void(__fastcall*)(Actor*);
    auto* pVtable = *reinterpret_cast<TApplyPerksFromBase**>(apActor);
    pVtable[kApplyPerksFromBaseIndex](apActor);

    return true;
}
