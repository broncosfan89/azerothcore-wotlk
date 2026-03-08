/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "AllCreatureScript.h"
#include "AllMapScript.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Creature.h"
#include "Group.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "TemporarySummon.h"
#include "UnitScript.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
constexpr uint32 MAP_EASTERN_KINGDOMS = 0;
constexpr uint32 MAP_DEADMINES = 36;
constexpr uint32 ZONE_WESTFALL = 40;

constexpr uint32 QUEST_BREAK_THE_UPRISING = 900000;
constexpr uint32 QUEST_SEAL_THE_DEADMINES = 900001;

constexpr uint32 NPC_DEFIAS_SMUGGLER = 95;
constexpr uint32 NPC_DEFIAS_TRAPPER = 504;
constexpr uint32 NPC_DEFIAS_PILLAGER = 589;
constexpr uint32 NPC_DEFIAS_HIGHWAYMAN = 122;
constexpr uint32 NPC_DEFIAS_KNUCKLEDUSTER = 449;
constexpr uint32 NPC_RHAHK_ZOR = 644;
constexpr uint32 NPC_EDWIN_VANCLEEF = 639;
constexpr uint32 SPELL_BREAKOUT_VISUAL_AURA = 37800;

constexpr uint32 OUTDOOR_QUALIFY_SCORE = 3;
constexpr uint32 OUTDOOR_SILVER_SCORE = 7;
constexpr uint32 OUTDOOR_GOLD_SCORE = 12;

constexpr float DUNGEON_HEALTH_MULTIPLIER = 1.85f;
constexpr float DUNGEON_DAMAGE_MULTIPLIER = 1.35f;
constexpr float DUNGEON_BOSS_HEALTH_MULTIPLIER = 2.20f;
constexpr float DUNGEON_BOSS_DAMAGE_MULTIPLIER = 1.50f;
constexpr float CONTRIBUTION_SHARE_DISTANCE = 120.0f;
constexpr float BREAKOUT_MOB_SCALE = 1.12f;
constexpr float BREAKOUT_COMMANDER_SCALE = 1.25f;

struct BreakoutSpawn
{
    uint32 Entry;
    Position Pos;
    uint32 Score;
};

struct PlayerContribution
{
    uint32 Score = 0;
    uint32 Kills = 0;
    bool OutdoorRewarded = false;
    bool DungeonQuestGranted = false;
    bool DungeonRewarded = false;
};

struct CreatureBaseline
{
    uint32 MaxHealth = 0;
    std::array<std::array<float, 2>, MAX_ATTACK> Damage{};
    std::array<bool, MAX_ATTACK> HasDamage{};
    bool Applied = false;
};

enum class BreakoutPhase : uint8
{
    Inactive,
    Outdoor,
    Dungeon
};

std::array<BreakoutSpawn, 20> const OutdoorSpawns =
{{
    { NPC_DEFIAS_SMUGGLER,     Position(-9856.29f, 1386.08f, 38.2633f, 0.593412f), 1 },
    { NPC_DEFIAS_SMUGGLER,     Position(-9998.19f, 1451.22f, 41.3837f, 2.80998f), 1 },
    { NPC_DEFIAS_TRAPPER,      Position(-9987.65f, 1478.71f, 43.5477f, 2.67035f), 1 },
    { NPC_DEFIAS_TRAPPER,      Position(-9875.14f, 1308.65f, 43.1871f, 2.40855f), 1 },

    { NPC_DEFIAS_SMUGGLER,     Position(-10347.1f, 1567.01f, 41.3426f, 2.16421f), 1 },
    { NPC_DEFIAS_TRAPPER,      Position(-10357.9f, 1562.52f, 42.0098f, 5.93412f), 1 },
    { NPC_DEFIAS_SMUGGLER,     Position(-10324.6f, 1413.14f, 40.4493f, 3.32955f), 1 },
    { NPC_DEFIAS_TRAPPER,      Position(-10311.5f, 1402.89f, 41.4070f, 0.017453f), 1 },

    { NPC_DEFIAS_PILLAGER,     Position(-10971.5f, 1461.66f, 43.4908f, 2.89725f), 1 },
    { NPC_DEFIAS_PILLAGER,     Position(-11001.2f, 1466.05f, 43.2363f, 3.56047f), 1 },
    { NPC_DEFIAS_PILLAGER,     Position(-11023.6f, 1500.94f, 43.2850f, 4.27606f), 1 },
    { NPC_DEFIAS_PILLAGER,     Position(-11062.2f, 1563.69f, 45.2017f, 1.58828f), 1 },

    { NPC_DEFIAS_PILLAGER,     Position(-10424.3f, 1905.80f, 7.17894f, 5.26966f), 1 },
    { NPC_DEFIAS_PILLAGER,     Position(-10447.0f, 1910.94f, 9.48856f, 0.314159f), 1 },
    { NPC_DEFIAS_TRAPPER,      Position(-10186.8f, 1920.14f, 37.4566f, 1.11701f), 1 },
    { NPC_DEFIAS_TRAPPER,      Position(-10172.2f, 1934.17f, 37.3743f, 2.71352f), 1 },

    { NPC_DEFIAS_HIGHWAYMAN,   Position(-11121.0f, 1177.36f, 62.4712f, 6.01474f), 1 },
    { NPC_DEFIAS_KNUCKLEDUSTER,Position(-11224.2f, 1210.91f, 89.2226f, 4.27719f), 1 },
    { NPC_DEFIAS_HIGHWAYMAN,   Position(-11202.4f, 1331.91f, 89.3128f, 4.37965f), 1 },
    { NPC_DEFIAS_KNUCKLEDUSTER,Position(-11237.6f, 1249.72f, 89.7604f, 0.261799f), 1 }
}};

Position const CommanderSpawn(-11024.2f, 1660.14f, 42.8221f, 4.10152f);

bool IsOutdoorBreakoutEntry(uint32 entry)
{
    switch (entry)
    {
        case NPC_DEFIAS_SMUGGLER:
        case NPC_DEFIAS_TRAPPER:
        case NPC_DEFIAS_PILLAGER:
        case NPC_DEFIAS_HIGHWAYMAN:
        case NPC_DEFIAS_KNUCKLEDUSTER:
        case NPC_RHAHK_ZOR:
            return true;
        default:
            return false;
    }
}

bool IsBreakoutDungeonBoss(uint32 entry)
{
    return entry == NPC_RHAHK_ZOR || entry == NPC_EDWIN_VANCLEEF;
}

std::string GetContributionTier(PlayerContribution const& contribution)
{
    if (contribution.Score >= OUTDOOR_GOLD_SCORE)
        return "Gold";

    if (contribution.Score >= OUTDOOR_SILVER_SCORE)
        return "Silver";

    return "Bronze";
}

void MarkBreakoutCreature(Creature* creature, bool commander = false)
{
    if (!creature)
        return;

    creature->SetName(commander
        ? Acore::StringFormat("Breakout Commander {}", creature->GetName())
        : Acore::StringFormat("Breakout {}", creature->GetName()));
    creature->SetObjectScale(commander ? BREAKOUT_COMMANDER_SCALE : BREAKOUT_MOB_SCALE);

    if (!creature->HasAura(SPELL_BREAKOUT_VISUAL_AURA))
        creature->AddAura(SPELL_BREAKOUT_VISUAL_AURA, creature);
}

class DeadminesBreakoutMgr
{
public:
    static DeadminesBreakoutMgr& Instance()
    {
        static DeadminesBreakoutMgr instance;
        return instance;
    }

    bool Start(ChatHandler* handler)
    {
        if (_phase != BreakoutPhase::Inactive)
        {
            if (handler)
                handler->SendErrorMessage("Deadmines Breakout is already active.");
            return false;
        }

        Map* map = sMapMgr->CreateBaseMap(MAP_EASTERN_KINGDOMS);
        if (!map)
        {
            if (handler)
                handler->SendErrorMessage("Unable to load the Eastern Kingdoms world map.");
            return false;
        }

        ResetState();
        _phase = BreakoutPhase::Outdoor;

        for (BreakoutSpawn const& spawn : OutdoorSpawns)
        {
            if (TempSummon* summon = map->SummonCreature(spawn.Entry, spawn.Pos, nullptr, 0))
            {
                MarkBreakoutCreature(summon);
                ObjectGuid::LowType const guidLow = summon->GetGUID().GetCounter();
                _activeOutdoorCreatures.insert(summon->GetGUID());
                _outdoorScoresByGuid.emplace(guidLow, spawn.Score);
                _outdoorThreatRemaining += spawn.Score;
            }
        }

        GrantOutdoorQuestToWestfallPlayers();
        BroadcastToWestfall("Westfall Alert: The Defias have broken out of the Deadmines. Defend the zone and drive them back.");

        if (handler)
            handler->PSendSysMessage("Deadmines Breakout started with {} outdoor mobs and {} total threat.", uint32(_activeOutdoorCreatures.size()), _outdoorThreatRemaining);

        return true;
    }

    bool Stop(ChatHandler* handler)
    {
        if (_phase == BreakoutPhase::Inactive)
        {
            if (handler)
                handler->SendErrorMessage("Deadmines Breakout is not active.");
            return false;
        }

        BreakoutPhase const oldPhase = _phase;
        DespawnOutdoorCreatures();
        ResetState();

        if (oldPhase == BreakoutPhase::Outdoor)
            BroadcastToWestfall("Westfall Alert: The Deadmines breakout has been stood down.");
        else
            BroadcastToDeadmines("The Deadmines breach has been stood down.");

        if (handler)
            handler->SendSysMessage("Deadmines Breakout stopped.");

        return true;
    }

    bool UnlockDungeon(ChatHandler* handler)
    {
        if (_phase != BreakoutPhase::Outdoor)
        {
            if (handler)
                handler->SendErrorMessage("The breakout must be in the outdoor phase before it can be unlocked.");
            return false;
        }

        DespawnOutdoorCreatures();
        BeginDungeonPhase(true);

        if (handler)
            handler->SendSysMessage("Outdoor phase skipped. Deadmines hard mode is now active.");

        return true;
    }

    std::string GetStatusSummary() const
    {
        switch (_phase)
        {
            case BreakoutPhase::Inactive:
                return "inactive";
            case BreakoutPhase::Outdoor:
                return Acore::StringFormat("outdoor active, mobs={}, threat={}, contributors={}", GetAliveOutdoorCount(), _outdoorThreatRemaining, GetContributorCount());
            case BreakoutPhase::Dungeon:
                return Acore::StringFormat("dungeon active, qualified={}, scaled creatures={}", GetQualifiedContributorCount(), _dungeonBaselines.size());
            default:
                return "unknown";
        }
    }

    void HandlePlayerZoneState(Player* player)
    {
        if (!player || !player->IsInWorld())
            return;

        if (_phase == BreakoutPhase::Outdoor && player->GetMapId() == MAP_EASTERN_KINGDOMS && player->GetZoneId() == ZONE_WESTFALL)
        {
            TryGrantQuest(player, QUEST_BREAK_THE_UPRISING);
            if (player->GetSession())
                player->GetSession()->SendAreaTriggerMessage("Westfall is under attack. Quest added: Break the Defias Uprising.");
            return;
        }

        if (_phase == BreakoutPhase::Dungeon && player->GetMapId() == MAP_DEADMINES)
        {
            if (IsQualifiedContributor(player))
                TryGrantDungeonQuest(player);

            if (player->GetSession())
                player->GetSession()->SendAreaTriggerMessage("Deadmines Breakout hard mode is active. Defeat Edwin VanCleef to seal the breach.");
        }
    }

    void HandleCreatureKill(Player* killer, Creature* killed)
    {
        if (_phase != BreakoutPhase::Outdoor || !killer || !killed)
            return;

        ObjectGuid::LowType const guidLow = killed->GetGUID().GetCounter();
        auto itr = _outdoorScoresByGuid.find(guidLow);
        if (itr == _outdoorScoresByGuid.end())
            return;

        AwardContribution(killer, itr->second);
    }

    void HandleOutdoorCreatureDeath(Creature* creature)
    {
        if (_phase != BreakoutPhase::Outdoor || !creature)
            return;

        ObjectGuid::LowType const guidLow = creature->GetGUID().GetCounter();
        if (!_processedOutdoorDeaths.insert(guidLow).second)
            return;

        if (_commanderGuid == creature->GetGUID())
        {
            CompleteOutdoorPhase();
            return;
        }

        auto itr = _outdoorScoresByGuid.find(guidLow);
        if (itr == _outdoorScoresByGuid.end())
            return;

        if (_outdoorThreatRemaining > itr->second)
            _outdoorThreatRemaining -= itr->second;
        else
            _outdoorThreatRemaining = 0;

        if (_outdoorThreatRemaining == 0 && _commanderGuid.IsEmpty())
            SpawnCommander();
    }

    void HandleVanCleefDeath(Creature* creature)
    {
        if (_phase != BreakoutPhase::Dungeon || !creature || creature->GetEntry() != NPC_EDWIN_VANCLEEF)
            return;

        RewardDungeonParticipants(creature->GetMap());
        BroadcastToDeadmines("The breach is sealed. Edwin VanCleef has fallen.");
        ResetState();
    }

    void ScaleDungeonCreature(Creature* creature)
    {
        if (_phase != BreakoutPhase::Dungeon || !creature || creature->GetMapId() != MAP_DEADMINES)
            return;

        if (!creature->IsHostileToPlayers() || creature->IsTrigger() || creature->IsTotem())
            return;

        ObjectGuid::LowType const guidLow = creature->GetGUID().GetCounter();
        CreatureBaseline& baseline = _dungeonBaselines[guidLow];
        if (baseline.MaxHealth == 0)
        {
            baseline.MaxHealth = std::max<uint32>(1, creature->GetMaxHealth());
            for (uint8 attackIndex = 0; attackIndex < MAX_ATTACK; ++attackIndex)
            {
                WeaponAttackType const attackType = WeaponAttackType(attackIndex);
                float const minDamage = creature->GetWeaponDamageRange(attackType, MINDAMAGE);
                float const maxDamage = creature->GetWeaponDamageRange(attackType, MAXDAMAGE);
                baseline.Damage[attackIndex][0] = minDamage;
                baseline.Damage[attackIndex][1] = maxDamage;
                baseline.HasDamage[attackIndex] = (minDamage > 0.0f || maxDamage > 0.0f);
            }
        }

        if (baseline.Applied)
            return;

        float const healthMultiplier = IsBreakoutDungeonBoss(creature->GetEntry()) ? DUNGEON_BOSS_HEALTH_MULTIPLIER : DUNGEON_HEALTH_MULTIPLIER;
        float const damageMultiplier = IsBreakoutDungeonBoss(creature->GetEntry()) ? DUNGEON_BOSS_DAMAGE_MULTIPLIER : DUNGEON_DAMAGE_MULTIPLIER;
        float const healthPct = creature->GetMaxHealth() > 0 ? float(creature->GetHealth()) / float(creature->GetMaxHealth()) : 1.0f;
        uint32 const scaledHealth = std::max<uint32>(1, uint32(std::lround(float(baseline.MaxHealth) * healthMultiplier)));

        creature->SetMaxHealth(scaledHealth);
        creature->SetHealth(std::min<uint32>(scaledHealth, std::max<uint32>(1, uint32(std::lround(float(scaledHealth) * healthPct)))));

        for (uint8 attackIndex = 0; attackIndex < MAX_ATTACK; ++attackIndex)
        {
            if (!baseline.HasDamage[attackIndex])
                continue;

            WeaponAttackType const attackType = WeaponAttackType(attackIndex);
            creature->SetBaseWeaponDamage(attackType, MINDAMAGE, std::max(1.0f, baseline.Damage[attackIndex][0] * damageMultiplier));
            creature->SetBaseWeaponDamage(attackType, MAXDAMAGE, std::max(1.0f, baseline.Damage[attackIndex][1] * damageMultiplier));
            creature->UpdateDamagePhysical(attackType);
        }

        baseline.Applied = true;
    }

private:
    void ResetState()
    {
        _phase = BreakoutPhase::Inactive;
        _outdoorThreatRemaining = 0;
        _commanderGuid.Clear();
        _activeOutdoorCreatures.clear();
        _outdoorScoresByGuid.clear();
        _processedOutdoorDeaths.clear();
        _contributions.clear();
        _dungeonBaselines.clear();
    }

    void DespawnOutdoorCreatures()
    {
        Map* map = sMapMgr->CreateBaseMap(MAP_EASTERN_KINGDOMS);
        if (!map)
            return;

        for (ObjectGuid const& guid : _activeOutdoorCreatures)
        {
            if (Creature* creature = map->GetCreature(guid))
            {
                if (TempSummon* summon = creature->ToTempSummon())
                    summon->UnSummon();
                else
                    creature->DespawnOrUnsummon();
            }
        }
    }

    void BroadcastToWestfall(std::string const& text) const
    {
        if (Map* map = sMapMgr->CreateBaseMap(MAP_EASTERN_KINGDOMS))
            map->SendZoneText(ZONE_WESTFALL, text.c_str());
    }

    void BroadcastToDeadmines(std::string const& text) const
    {
        if (Map* map = sMapMgr->CreateBaseMap(MAP_DEADMINES))
            map->SendZoneText(0, text.c_str());
    }

    void GrantOutdoorQuestToWestfallPlayers()
    {
        Map* map = sMapMgr->CreateBaseMap(MAP_EASTERN_KINGDOMS);
        if (!map)
            return;

        Map::PlayerList const& players = map->GetPlayers();
        for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
            if (Player* player = itr->GetSource())
                if (player->GetZoneId() == ZONE_WESTFALL)
                    TryGrantQuest(player, QUEST_BREAK_THE_UPRISING);
    }

    void TryGrantQuest(Player* player, uint32 questId)
    {
        if (!player)
            return;

        if (player->GetQuestStatus(questId) != QUEST_STATUS_NONE || player->GetQuestRewardStatus(questId))
            return;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            return;

        if (player->CanTakeQuest(quest, true) && player->CanAddQuest(quest, true))
            player->AddQuestAndCheckCompletion(quest, nullptr);
    }

    void TryGrantDungeonQuest(Player* player)
    {
        if (!player)
            return;

        PlayerContribution& contribution = _contributions[player->GetGUID().GetCounter()];
        if (contribution.DungeonQuestGranted)
            return;

        TryGrantQuest(player, QUEST_SEAL_THE_DEADMINES);
        contribution.DungeonQuestGranted = true;
    }

    bool IsQualifiedContributor(Player* player) const
    {
        if (!player)
            return false;

        auto itr = _contributions.find(player->GetGUID().GetCounter());
        return itr != _contributions.end() && itr->second.Score >= OUTDOOR_QUALIFY_SCORE;
    }

    void AwardContribution(Player* killer, uint32 score)
    {
        if (!killer)
            return;

        std::vector<Player*> recipients;
        recipients.reserve(5);

        auto considerRecipient = [&](Player* candidate)
        {
            if (!candidate || !candidate->IsInWorld() || candidate->GetMapId() != MAP_EASTERN_KINGDOMS || candidate->GetZoneId() != ZONE_WESTFALL)
                return;

            if (!candidate->IsWithinDistInMap(killer, CONTRIBUTION_SHARE_DISTANCE))
                return;

            if (std::find(recipients.begin(), recipients.end(), candidate) == recipients.end())
                recipients.push_back(candidate);
        };

        considerRecipient(killer);

        if (Group* group = killer->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                considerRecipient(ref->GetSource());

        for (Player* recipient : recipients)
        {
            PlayerContribution& contribution = _contributions[recipient->GetGUID().GetCounter()];
            contribution.Score += score;
            contribution.Kills += 1;
            TryGrantQuest(recipient, QUEST_BREAK_THE_UPRISING);
        }
    }

    void SpawnCommander()
    {
        Map* map = sMapMgr->CreateBaseMap(MAP_EASTERN_KINGDOMS);
        if (!map)
            return;

        if (TempSummon* summon = map->SummonCreature(NPC_RHAHK_ZOR, CommanderSpawn, nullptr, 0))
        {
            MarkBreakoutCreature(summon, true);
            _commanderGuid = summon->GetGUID();
            _activeOutdoorCreatures.insert(summon->GetGUID());
            _outdoorScoresByGuid.emplace(summon->GetGUID().GetCounter(), 5);
            BroadcastToWestfall("Westfall Alert: Rhahk'Zor has rallied the breakout. Hunt him down near Moonbrook.");
        }
    }

    void CompleteOutdoorPhase()
    {
        RewardOutdoorParticipants();
        BeginDungeonPhase(false);
    }

    void RewardOutdoorParticipants()
    {
        for (auto& [guidLow, contribution] : _contributions)
        {
            if (contribution.Score < OUTDOOR_QUALIFY_SCORE || contribution.OutdoorRewarded)
                continue;

            if (Player* player = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(guidLow)))
            {
                TryGrantQuest(player, QUEST_BREAK_THE_UPRISING);
                if (player->GetQuestStatus(QUEST_BREAK_THE_UPRISING) == QUEST_STATUS_INCOMPLETE)
                    player->AreaExploredOrEventHappens(QUEST_BREAK_THE_UPRISING);

                uint32 xpReward = 1500;
                int64 moneyReward = 10000;
                if (contribution.Score >= OUTDOOR_GOLD_SCORE)
                {
                    xpReward = 5000;
                    moneyReward = 60000;
                }
                else if (contribution.Score >= OUTDOOR_SILVER_SCORE)
                {
                    xpReward = 3000;
                    moneyReward = 30000;
                }

                player->GiveXP(xpReward, nullptr);
                player->ModifyMoney(moneyReward);
                TryGrantDungeonQuest(player);

                if (player->GetSession())
                    player->GetSession()->SendAreaTriggerMessage("Westfall secured. {} contribution reward granted ({} score).", GetContributionTier(contribution), contribution.Score);
            }

            contribution.OutdoorRewarded = true;
        }
    }

    void BeginDungeonPhase(bool forced)
    {
        _phase = BreakoutPhase::Dungeon;
        _activeOutdoorCreatures.clear();
        _outdoorScoresByGuid.clear();
        _processedOutdoorDeaths.clear();
        _outdoorThreatRemaining = 0;
        _commanderGuid.Clear();

        for (auto& [guidLow, contribution] : _contributions)
        {
            if (contribution.Score < OUTDOOR_QUALIFY_SCORE)
                continue;

            if (Player* player = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(guidLow)))
                TryGrantDungeonQuest(player);
        }

        BroadcastToWestfall(forced
            ? "Westfall Alert: The breach has been forced open. Qualified defenders may now storm the Deadmines."
            : "Westfall Alert: The breakout is contained. Qualified defenders may now enter the Deadmines breach.");
    }

    void RewardDungeonParticipants(Map* map)
    {
        if (!map)
            return;

        Map::PlayerList const& players = map->GetPlayers();
        for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
        {
            Player* player = itr->GetSource();
            if (!player || !IsQualifiedContributor(player))
                continue;

            PlayerContribution& contribution = _contributions[player->GetGUID().GetCounter()];
            if (contribution.DungeonRewarded)
                continue;

            TryGrantDungeonQuest(player);
            if (player->GetQuestStatus(QUEST_SEAL_THE_DEADMINES) == QUEST_STATUS_INCOMPLETE)
                player->AreaExploredOrEventHappens(QUEST_SEAL_THE_DEADMINES);

            player->GiveXP(7000, nullptr);
            player->ModifyMoney(120000);

            if (player->GetSession())
                player->GetSession()->SendAreaTriggerMessage("Deadmines sealed. Breakout completion reward granted.");

            contribution.DungeonRewarded = true;
        }
    }

    uint32 GetAliveOutdoorCount() const
    {
        Map* map = sMapMgr->CreateBaseMap(MAP_EASTERN_KINGDOMS);
        if (!map)
            return 0;

        uint32 count = 0;
        for (ObjectGuid const& guid : _activeOutdoorCreatures)
            if (Creature* creature = map->GetCreature(guid))
                if (creature->IsAlive())
                    ++count;

        return count;
    }

    uint32 GetContributorCount() const
    {
        return uint32(std::count_if(_contributions.begin(), _contributions.end(), [](auto const& pair)
        {
            return pair.second.Score > 0;
        }));
    }

    uint32 GetQualifiedContributorCount() const
    {
        return uint32(std::count_if(_contributions.begin(), _contributions.end(), [](auto const& pair)
        {
            return pair.second.Score >= OUTDOOR_QUALIFY_SCORE;
        }));
    }

    BreakoutPhase _phase = BreakoutPhase::Inactive;
    uint32 _outdoorThreatRemaining = 0;
    ObjectGuid _commanderGuid;
    std::unordered_set<ObjectGuid> _activeOutdoorCreatures;
    std::unordered_map<ObjectGuid::LowType, uint32> _outdoorScoresByGuid;
    std::unordered_set<ObjectGuid::LowType> _processedOutdoorDeaths;
    std::unordered_map<ObjectGuid::LowType, PlayerContribution> _contributions;
    std::unordered_map<ObjectGuid::LowType, CreatureBaseline> _dungeonBaselines;
};

class deadmines_breakout_player_script : public PlayerScript
{
public:
    deadmines_breakout_player_script()
        : PlayerScript("deadmines_breakout_player_script", { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_UPDATE_ZONE, PLAYERHOOK_ON_MAP_CHANGED, PLAYERHOOK_ON_CREATURE_KILL, PLAYERHOOK_ON_CREATURE_KILLED_BY_PET }) { }

    void OnPlayerLogin(Player* player) override
    {
        DeadminesBreakoutMgr::Instance().HandlePlayerZoneState(player);
    }

    void OnPlayerUpdateZone(Player* player, uint32 /*newZone*/, uint32 /*newArea*/) override
    {
        DeadminesBreakoutMgr::Instance().HandlePlayerZoneState(player);
    }

    void OnPlayerMapChanged(Player* player) override
    {
        DeadminesBreakoutMgr::Instance().HandlePlayerZoneState(player);
    }

    void OnPlayerCreatureKill(Player* killer, Creature* killed) override
    {
        DeadminesBreakoutMgr::Instance().HandleCreatureKill(killer, killed);
    }

    void OnPlayerCreatureKilledByPet(Player* owner, Creature* killed) override
    {
        DeadminesBreakoutMgr::Instance().HandleCreatureKill(owner, killed);
    }
};

class deadmines_breakout_all_creature : public AllCreatureScript
{
public:
    deadmines_breakout_all_creature() : AllCreatureScript("deadmines_breakout_all_creature") { }

    void OnCreatureAddWorld(Creature* creature) override
    {
        DeadminesBreakoutMgr::Instance().ScaleDungeonCreature(creature);
    }
};

class deadmines_breakout_unit_script : public UnitScript
{
public:
    deadmines_breakout_unit_script()
        : UnitScript("deadmines_breakout_unit_script", true, { UNITHOOK_ON_UNIT_DEATH }) { }

    void OnUnitDeath(Unit* unit, Unit* /*killer*/) override
    {
        Creature* creature = unit ? unit->ToCreature() : nullptr;
        if (!creature)
            return;

        if (IsOutdoorBreakoutEntry(creature->GetEntry()) && creature->GetMapId() == MAP_EASTERN_KINGDOMS)
            DeadminesBreakoutMgr::Instance().HandleOutdoorCreatureDeath(creature);

        if (creature->GetEntry() == NPC_EDWIN_VANCLEEF && creature->GetMapId() == MAP_DEADMINES)
            DeadminesBreakoutMgr::Instance().HandleVanCleefDeath(creature);
    }
};

class deadmines_breakout_all_map : public AllMapScript
{
public:
    deadmines_breakout_all_map() : AllMapScript("deadmines_breakout_all_map", { ALLMAPHOOK_ON_PLAYER_ENTER_ALL }) { }

    void OnPlayerEnterAll(Map* /*map*/, Player* player) override
    {
        DeadminesBreakoutMgr::Instance().HandlePlayerZoneState(player);
    }
};

class deadmines_breakout_commandscript : public CommandScript
{
public:
    deadmines_breakout_commandscript() : CommandScript("deadmines_breakout_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable breakoutCommandTable =
        {
            { "start",  HandleStartCommand,  SEC_GAMEMASTER, Console::Yes },
            { "stop",   HandleStopCommand,   SEC_GAMEMASTER, Console::Yes },
            { "status", HandleStatusCommand, SEC_GAMEMASTER, Console::Yes },
            { "unlock", HandleUnlockCommand, SEC_GAMEMASTER, Console::Yes }
        };

        static ChatCommandTable commandTable =
        {
            { "breakout", breakoutCommandTable }
        };

        return commandTable;
    }

    static bool HandleStartCommand(ChatHandler* handler)
    {
        return DeadminesBreakoutMgr::Instance().Start(handler);
    }

    static bool HandleStopCommand(ChatHandler* handler)
    {
        return DeadminesBreakoutMgr::Instance().Stop(handler);
    }

    static bool HandleStatusCommand(ChatHandler* handler)
    {
        handler->PSendSysMessage("Deadmines Breakout status: {}", DeadminesBreakoutMgr::Instance().GetStatusSummary());
        return true;
    }

    static bool HandleUnlockCommand(ChatHandler* handler)
    {
        return DeadminesBreakoutMgr::Instance().UnlockDungeon(handler);
    }
};
}

void AddSC_deadmines_breakout()
{
    new deadmines_breakout_player_script();
    new deadmines_breakout_all_creature();
    new deadmines_breakout_unit_script();
    new deadmines_breakout_all_map();
    new deadmines_breakout_commandscript();
}
