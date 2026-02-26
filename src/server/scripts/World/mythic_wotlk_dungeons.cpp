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
#include "AllGameObjectScript.h"
#include "AllMapScript.h"
#include "AreaDefines.h"
#include "Config.h"
#include "Creature.h"
#include "GameObject.h"
#include "Log.h"
#include "LootMgr.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "TemporarySummon.h"
#include "UnitScript.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
uint32 constexpr ITEM_EMBLEM_OF_CONQUEST = 45624;
uint32 constexpr ITEM_EMBLEM_OF_TRIUMPH = 47241;
uint32 constexpr NPC_UK_MYTHIC_DIFFICULTY_SELECTOR = 190100;
uint32 constexpr NPC_WOTLK_MYTHIC_DIFFICULTY_SELECTOR = 190101;
uint8 constexpr MYTHIC_LEVEL_CAP = 15;
uint32 constexpr MYTHIC_ITEM_BASE = 70000000;
uint32 constexpr MYTHIC_ITEM_STRIDE = 1000000;
uint32 constexpr MYTHIC_ITEM_SOURCE_CLASS_WEAPON = 2;
uint32 constexpr MYTHIC_ITEM_SOURCE_CLASS_ARMOR = 4;
float constexpr MYTHIC_BERSERK_HEALTH_THRESHOLD_PCT = 30.0f;
float constexpr MYTHIC_BERSERK_DAMAGE_MULTIPLIER = 1.30f;
uint8 constexpr MYTHIC_BERSERK_MIN_LEVEL = 5;
uint8 constexpr MYTHIC_EXTRA_TRASH_MIN_LEVEL = 10;
uint8 constexpr MYTHIC_EXTRA_TRASH_CHANCE_PCT = 30;
uint32 constexpr MYTHIC_GOSSIP_ACTION_SET_LEVEL_BASE = GOSSIP_ACTION_INFO_DEF + 1000;

using AttackDamagePair = std::array<float, 2>;

struct CreatureBaseline
{
    uint32 MaxHealth = 0;
    std::array<AttackDamagePair, MAX_ATTACK> Damage = {};
    std::array<bool, MAX_ATTACK> HasDamage = {};
    uint8 AppliedLevel = 255;
};

struct MythicInstanceState
{
    bool Initialized = false;
    bool Enabled = false;
    bool Locked = false;
    bool PendingRescale = false;
    uint8 Level = 0;
    uint8 MaxLevel = MYTHIC_LEVEL_CAP;

    float HealthM0 = 1.30f;
    float HealthPerLevel = 0.15f;
    float DamageM0 = 1.15f;
    float DamagePerLevel = 0.10f;

    float HealthMultiplier = 1.0f;
    float DamageMultiplier = 1.0f;
    uint32 BerserkCheckTimerMs = 1000;

    uint8 RewardUpgradeLevel = 5;
    uint32 RewardBaseCount = 1;
    uint32 RewardPerLevelDivisor = 2;
    uint32 LootRebuildScanTimerMs = 1000;

    ObjectGuid SelectorGuid = ObjectGuid::Empty;
    std::unordered_set<uint32> BerserkAppliedCreatureGuids;
    std::unordered_set<uint32> ExtraTrashProcessedSpawnIds;
    std::unordered_set<uint32> LootProcessedCreatureGuids;
    std::unordered_set<uint32> RewardProcessedCreatureGuids;
    std::unordered_map<uint32, CreatureBaseline> BaselinesByGuid;
};

std::unordered_map<uint32, MythicInstanceState> MythicStates;

bool IsWotlkMythicDungeonMap(uint32 mapId)
{
    switch (mapId)
    {
        case MAP_UTGARDE_PINNACLE:
        case MAP_THE_NEXUS:
        case MAP_THE_OCULUS:
        case MAP_THE_CULLING_OF_STRATHOLME:
        case MAP_HALLS_OF_STONE:
        case MAP_DRAK_THARON_KEEP:
        case MAP_AZJOL_NERUB:
        case MAP_HALLS_OF_LIGHTNING:
        case MAP_GUNDRAK:
        case MAP_VIOLET_HOLD:
        case MAP_AHN_KAHET_THE_OLD_KINGDOM:
        case MAP_THE_FORGE_OF_SOULS:
        case MAP_TRIAL_OF_THE_CHAMPION:
        case MAP_PIT_OF_SARON:
        case MAP_HALLS_OF_REFLECTION:
            return true;
        default:
            return false;
    }
}

bool IsAnyMythicSelectorEntry(uint32 entry)
{
    return entry == NPC_WOTLK_MYTHIC_DIFFICULTY_SELECTOR || entry == NPC_UK_MYTHIC_DIFFICULTY_SELECTOR;
}

bool CanApplyMythicScalingToCreature(Creature const* creature)
{
    return creature && creature->IsHostileToPlayers() && !creature->IsTrigger() && !creature->IsTotem();
}

uint32 GetMythicScaledItemEntry(uint32 sourceEntry, uint8 mythicLevel)
{
    if (mythicLevel > MYTHIC_LEVEL_CAP || sourceEntry >= MYTHIC_ITEM_STRIDE || sourceEntry >= MYTHIC_ITEM_BASE)
        return 0;

    return MYTHIC_ITEM_BASE + (uint32(mythicLevel) * MYTHIC_ITEM_STRIDE) + sourceEntry;
}

void RemapLootItemsToMythicLevel(LootItemList& lootItems, uint8 mythicLevel)
{
    if (mythicLevel > MYTHIC_LEVEL_CAP)
        return;

    for (LootItem& lootItem : lootItems)
    {
        if (lootItem.itemid >= MYTHIC_ITEM_BASE)
            continue;

        ItemTemplate const* sourceTemplate = sObjectMgr->GetItemTemplate(lootItem.itemid);
        if (!sourceTemplate)
            continue;

        if (sourceTemplate->Class != MYTHIC_ITEM_SOURCE_CLASS_WEAPON && sourceTemplate->Class != MYTHIC_ITEM_SOURCE_CLASS_ARMOR)
            continue;

        uint32 const mappedItemEntry = GetMythicScaledItemEntry(lootItem.itemid, mythicLevel);
        if (!mappedItemEntry)
            continue;

        if (sObjectMgr->GetItemTemplate(mappedItemEntry))
            lootItem.itemid = mappedItemEntry;
    }
}

void RecalculateMythicMultipliers(MythicInstanceState& state)
{
    if (!state.Enabled)
    {
        state.HealthMultiplier = 1.0f;
        state.DamageMultiplier = 1.0f;
        return;
    }

    state.HealthMultiplier = std::max(1.0f, state.HealthM0 + (state.HealthPerLevel * float(state.Level)));
    state.DamageMultiplier = std::max(1.0f, state.DamageM0 + (state.DamagePerLevel * float(state.Level)));
}

MythicInstanceState* GetMythicState(Map* map, bool createIfMissing)
{
    if (!map || !map->IsDungeon() || !IsWotlkMythicDungeonMap(map->GetId()))
        return nullptr;

    InstanceMap* instanceMap = map->ToInstanceMap();
    if (!instanceMap)
        return nullptr;

    uint32 const instanceId = instanceMap->GetInstanceId();
    if (!instanceId)
        return nullptr;

    auto stateItr = MythicStates.find(instanceId);
    if (stateItr == MythicStates.end())
    {
        if (!createIfMissing)
            return nullptr;

        stateItr = MythicStates.emplace(instanceId, MythicInstanceState{}).first;
    }

    MythicInstanceState& state = stateItr->second;
    if (!state.Initialized)
    {
        if (!createIfMissing)
            return nullptr;

        state.Initialized = true;

        bool const enabled = sConfigMgr->GetOption<bool>("Custom.MythicWotlkDungeons.Enable",
            sConfigMgr->GetOption<bool>("Custom.MythicUtgardeKeep.Enable", false));

        if (instanceMap->IsHeroic() && enabled)
        {
            state.Enabled = true;
            uint32 const configuredMaxLevel = std::max<uint32>(0, sConfigMgr->GetOption<uint32>("Custom.MythicWotlkDungeons.MaxLevel",
                sConfigMgr->GetOption<uint32>("Custom.MythicUtgardeKeep.MaxLevel", MYTHIC_LEVEL_CAP)));
            state.MaxLevel = uint8(std::min<uint32>(MYTHIC_LEVEL_CAP, configuredMaxLevel));

            uint32 const configuredLevel = sConfigMgr->GetOption<uint32>("Custom.MythicWotlkDungeons.Level",
                sConfigMgr->GetOption<uint32>("Custom.MythicUtgardeKeep.Level", 0));
            state.Level = uint8(std::min<uint32>(state.MaxLevel, configuredLevel));

            state.HealthM0 = sConfigMgr->GetOption<float>("Custom.MythicWotlkDungeons.HealthMultiplierM0",
                sConfigMgr->GetOption<float>("Custom.MythicUtgardeKeep.HealthMultiplierM0", 1.30f));
            state.HealthPerLevel = sConfigMgr->GetOption<float>("Custom.MythicWotlkDungeons.HealthMultiplierPerLevel",
                sConfigMgr->GetOption<float>("Custom.MythicUtgardeKeep.HealthMultiplierPerLevel", 0.15f));
            state.DamageM0 = sConfigMgr->GetOption<float>("Custom.MythicWotlkDungeons.DamageMultiplierM0",
                sConfigMgr->GetOption<float>("Custom.MythicUtgardeKeep.DamageMultiplierM0", 1.15f));
            state.DamagePerLevel = sConfigMgr->GetOption<float>("Custom.MythicWotlkDungeons.DamageMultiplierPerLevel",
                sConfigMgr->GetOption<float>("Custom.MythicUtgardeKeep.DamageMultiplierPerLevel", 0.10f));

            state.RewardUpgradeLevel = uint8(sConfigMgr->GetOption<uint32>("Custom.MythicWotlkDungeons.RewardUpgradeLevel",
                sConfigMgr->GetOption<uint32>("Custom.MythicUtgardeKeep.RewardUpgradeLevel", 5)));
            state.RewardBaseCount = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("Custom.MythicWotlkDungeons.RewardBaseCount",
                sConfigMgr->GetOption<uint32>("Custom.MythicUtgardeKeep.RewardBaseCount", 1)));
            state.RewardPerLevelDivisor = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("Custom.MythicWotlkDungeons.RewardPerLevelDivisor",
                sConfigMgr->GetOption<uint32>("Custom.MythicUtgardeKeep.RewardPerLevelDivisor", 2)));

            state.PendingRescale = true;
            RecalculateMythicMultipliers(state);
        }
    }

    return &state;
}

void ScaleCreatureAttackDamageFromBaseline(Creature* creature, CreatureBaseline const& baseline, float multiplier)
{
    if (!creature || multiplier <= 0.0f)
        return;

    for (uint8 index = 0; index < MAX_ATTACK; ++index)
    {
        if (!baseline.HasDamage[index])
            continue;

        WeaponAttackType const attackType = WeaponAttackType(index);
        float const minDamage = std::max(1.0f, baseline.Damage[index][0] * multiplier);
        float const maxDamage = std::max(1.0f, baseline.Damage[index][1] * multiplier);
        creature->SetBaseWeaponDamage(attackType, MINDAMAGE, minDamage);
        creature->SetBaseWeaponDamage(attackType, MAXDAMAGE, maxDamage);
        creature->UpdateDamagePhysical(attackType);
    }
}

void ApplyMythicScaleToCreature(MythicInstanceState& state, Creature* creature, bool forceReapply)
{
    if (!state.Enabled || !CanApplyMythicScalingToCreature(creature) || IsAnyMythicSelectorEntry(creature->GetEntry()))
        return;

    if (creature->IsDungeonBoss())
        creature->SetLootMode(LOOT_MODE_DEFAULT);
    else
        creature->SetLootMode(0);

    uint32 const guidLow = uint32(creature->GetGUID().GetCounter());
    CreatureBaseline& baseline = state.BaselinesByGuid[guidLow];
    if (baseline.MaxHealth == 0)
    {
        baseline.MaxHealth = std::max<uint32>(1, creature->GetMaxHealth());

        for (uint8 index = 0; index < MAX_ATTACK; ++index)
        {
            WeaponAttackType const attackType = WeaponAttackType(index);
            float const minDamage = creature->GetWeaponDamageRange(attackType, MINDAMAGE);
            float const maxDamage = creature->GetWeaponDamageRange(attackType, MAXDAMAGE);
            baseline.Damage[index][0] = minDamage;
            baseline.Damage[index][1] = maxDamage;
            baseline.HasDamage[index] = (minDamage > 0.0f || maxDamage > 0.0f);
        }
    }

    if (!forceReapply && baseline.AppliedLevel == state.Level)
        return;

    float const healthPct = creature->GetMaxHealth() > 0 ? float(creature->GetHealth()) / float(creature->GetMaxHealth()) : 1.0f;
    uint32 const scaledMaxHealth = std::max<uint32>(1, uint32(std::lround(float(baseline.MaxHealth) * state.HealthMultiplier)));
    creature->SetMaxHealth(scaledMaxHealth);
    uint32 const scaledCurrentHealth = std::max<uint32>(1, uint32(std::lround(float(scaledMaxHealth) * std::clamp(healthPct, 0.0f, 1.0f))));
    creature->SetHealth(std::min<uint32>(scaledMaxHealth, scaledCurrentHealth));

    ScaleCreatureAttackDamageFromBaseline(creature, baseline, state.DamageMultiplier);
    baseline.AppliedLevel = state.Level;
}

bool IsExtraTrashCandidate(MythicInstanceState const& state, Creature* creature)
{
    if (!state.Enabled || state.Level < MYTHIC_EXTRA_TRASH_MIN_LEVEL || !creature)
        return false;

    if (!CanApplyMythicScalingToCreature(creature) || creature->IsDungeonBoss())
        return false;

    return creature->GetSpawnId() != 0;
}

void TrySpawnMythicExtraTrash(MythicInstanceState& state, Creature* creature)
{
    if (!IsExtraTrashCandidate(state, creature))
        return;

    uint32 const spawnId = creature->GetSpawnId();
    if (!state.ExtraTrashProcessedSpawnIds.insert(spawnId).second)
        return;

    if (urand(1, 100) > MYTHIC_EXTRA_TRASH_CHANCE_PCT)
        return;

    float const offsetX = frand(-2.5f, 2.5f);
    float const offsetY = frand(-2.5f, 2.5f);
    if (TempSummon* extraTrash = creature->SummonCreature(creature->GetEntry(), creature->GetPositionX() + offsetX, creature->GetPositionY() + offsetY,
        creature->GetPositionZ(), creature->GetOrientation(), TEMPSUMMON_DEAD_DESPAWN))
    {
        ApplyMythicScaleToCreature(state, extraTrash, true);
    }
}

void RescaleMythicCreaturesInMap(Map* map, MythicInstanceState& state)
{
    if (!map || !state.Enabled)
        return;

    for (auto const& spawnPair : map->GetCreatureBySpawnIdStore())
    {
        Creature* creature = spawnPair.second;
        if (!creature)
            continue;

        ApplyMythicScaleToCreature(state, creature, true);
        TrySpawnMythicExtraTrash(state, creature);
    }
}

Player* GetFallbackLootRecipient(Map* map)
{
    if (!map)
        return nullptr;

    Map::PlayerList const& players = map->GetPlayers();
    for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
    {
        if (Player* player = itr->GetSource())
            return player;
    }

    return nullptr;
}

void RebuildMythicBossLoot(MythicInstanceState& state, Creature* creature)
{
    if (!state.Enabled || !creature || creature->IsAlive() || !creature->IsDungeonBoss())
        return;

    uint32 const guidLow = uint32(creature->GetGUID().GetCounter());
    if (!state.LootProcessedCreatureGuids.insert(guidLow).second)
        return;

    creature->SetLootMode(LOOT_MODE_DEFAULT);

    Player* lootRecipient = creature->GetLootRecipient();
    if (!lootRecipient || !lootRecipient->IsInMap(creature))
    {
        lootRecipient = GetFallbackLootRecipient(creature->GetMap());
        if (lootRecipient)
            creature->SetLootRecipient(lootRecipient);
    }

    creature->loot.clear();
    if (uint32 lootId = creature->GetCreatureTemplate()->lootid)
        creature->loot.FillLoot(lootId, LootTemplates_Creature, lootRecipient, false, false, creature->GetLootMode(), creature);

    if (creature->GetLootMode())
        creature->loot.generateMoneyLoot(creature->GetCreatureTemplate()->mingold, creature->GetCreatureTemplate()->maxgold);

    uint32 remappedCount = 0;
    for (LootItem& lootItem : creature->loot.items)
    {
        uint32 const oldItemId = lootItem.itemid;
        if (oldItemId >= MYTHIC_ITEM_BASE)
            continue;

        ItemTemplate const* sourceTemplate = sObjectMgr->GetItemTemplate(oldItemId);
        if (!sourceTemplate)
            continue;

        if (sourceTemplate->Class != MYTHIC_ITEM_SOURCE_CLASS_WEAPON && sourceTemplate->Class != MYTHIC_ITEM_SOURCE_CLASS_ARMOR)
            continue;

        uint32 const mappedItemEntry = GetMythicScaledItemEntry(oldItemId, state.Level);
        if (!mappedItemEntry || !sObjectMgr->GetItemTemplate(mappedItemEntry))
            continue;

        lootItem.itemid = mappedItemEntry;
        ++remappedCount;
    }

    LOG_INFO("scripts", "MYTHIC_WOTLK_LOOT boss {} ({}) level {} remapped_items {} total_items {}",
        creature->GetName(), creature->GetEntry(), uint32(state.Level), remappedCount, creature->loot.items.size());

    if (!creature->loot.empty())
    {
        creature->SetDynamicFlag(UNIT_DYNFLAG_LOOTABLE | UNIT_DYNFLAG_TAPPED | UNIT_DYNFLAG_TAPPED_BY_PLAYER);
        creature->DestroyForVisiblePlayers();
        creature->SetVisible(true);
    }
}

void GrantMythicBossReward(MythicInstanceState& state, Creature* creature)
{
    if (!state.Enabled || !creature || !creature->IsDungeonBoss())
        return;

    uint32 const guidLow = uint32(creature->GetGUID().GetCounter());
    if (!state.RewardProcessedCreatureGuids.insert(guidLow).second)
        return;

    uint32 const rewardItemId = (state.Level >= state.RewardUpgradeLevel) ? ITEM_EMBLEM_OF_TRIUMPH : ITEM_EMBLEM_OF_CONQUEST;
    uint32 const rewardCount = state.RewardBaseCount + (uint32(state.Level) / state.RewardPerLevelDivisor);

    Map* map = creature->GetMap();
    if (!map)
        return;

    for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin(); itr != map->GetPlayers().end(); ++itr)
    {
        Player* player = itr->GetSource();
        if (!player)
            continue;

        player->AddItem(rewardItemId, rewardCount);
        if (player->GetSession())
            player->GetSession()->SendAreaTriggerMessage("Mythic {} reward: item {} x{}.", uint32(state.Level), rewardItemId, rewardCount);
    }
}

void SetMythicLevelForMap(Map* map, MythicInstanceState& state, uint8 newLevel)
{
    if (!state.Enabled)
        return;

    newLevel = std::min<uint8>(newLevel, state.MaxLevel);
    if (newLevel == state.Level)
        return;

    state.Level = newLevel;
    state.BerserkAppliedCreatureGuids.clear();
    RecalculateMythicMultipliers(state);
    state.PendingRescale = true;

    if (map)
        RescaleMythicCreaturesInMap(map, state);

    state.PendingRescale = false;
}

Creature* FindExistingSelector(Map* map)
{
    if (!map)
        return nullptr;

    for (auto const& spawnPair : map->GetCreatureBySpawnIdStore())
    {
        if (Creature* creature = spawnPair.second)
            if (IsAnyMythicSelectorEntry(creature->GetEntry()))
                return creature;
    }

    return nullptr;
}

void EnsureSelectorInMap(Map* map, MythicInstanceState& state, Player* preferredAnchor)
{
    if (!map || !state.Enabled)
        return;

    if (!state.SelectorGuid.IsEmpty())
    {
        if (Creature* existingByGuid = map->GetCreature(state.SelectorGuid))
            return;

        state.SelectorGuid.Clear();
    }

    if (Creature* existingSelector = FindExistingSelector(map))
    {
        state.SelectorGuid = existingSelector->GetGUID();
        return;
    }

    if (!sObjectMgr->GetCreatureTemplate(NPC_WOTLK_MYTHIC_DIFFICULTY_SELECTOR))
        return;

    Player* anchor = preferredAnchor;
    if (!anchor)
    {
        for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin(); itr != map->GetPlayers().end(); ++itr)
        {
            if (Player* candidate = itr->GetSource())
            {
                anchor = candidate;
                break;
            }
        }
    }

    if (!anchor)
        return;

    float const spawnDistance = 2.0f;
    float const spawnX = anchor->GetPositionX() + (std::cos(anchor->GetOrientation()) * spawnDistance);
    float const spawnY = anchor->GetPositionY() + (std::sin(anchor->GetOrientation()) * spawnDistance);
    float const spawnZ = anchor->GetPositionZ();

    if (TempSummon* selector = anchor->SummonCreature(NPC_WOTLK_MYTHIC_DIFFICULTY_SELECTOR, spawnX, spawnY, spawnZ,
        anchor->GetOrientation(), TEMPSUMMON_MANUAL_DESPAWN))
    {
        selector->SetHomePosition(spawnX, spawnY, spawnZ, anchor->GetOrientation());
        state.SelectorGuid = selector->GetGUID();
    }
}

void TryApplyMythicBossBerserk(MythicInstanceState& state, Creature* creature)
{
    if (!state.Enabled || state.Level < MYTHIC_BERSERK_MIN_LEVEL || !creature || !creature->IsDungeonBoss() || !creature->IsAlive())
        return;

    if (!creature->IsInCombat() || creature->GetHealthPct() > MYTHIC_BERSERK_HEALTH_THRESHOLD_PCT)
        return;

    uint32 const guidLow = uint32(creature->GetGUID().GetCounter());
    if (!state.BerserkAppliedCreatureGuids.insert(guidLow).second)
        return;

    for (uint8 index = 0; index < MAX_ATTACK; ++index)
    {
        WeaponAttackType const attackType = WeaponAttackType(index);
        float const minDamage = creature->GetWeaponDamageRange(attackType, MINDAMAGE);
        float const maxDamage = creature->GetWeaponDamageRange(attackType, MAXDAMAGE);
        if (minDamage <= 0.0f && maxDamage <= 0.0f)
            continue;

        creature->SetBaseWeaponDamage(attackType, MINDAMAGE, std::max(1.0f, minDamage * MYTHIC_BERSERK_DAMAGE_MULTIPLIER));
        creature->SetBaseWeaponDamage(attackType, MAXDAMAGE, std::max(1.0f, maxDamage * MYTHIC_BERSERK_DAMAGE_MULTIPLIER));
        creature->UpdateDamagePhysical(attackType);
    }

    if (Map* map = creature->GetMap())
    {
        for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin(); itr != map->GetPlayers().end(); ++itr)
            if (Player* player = itr->GetSource())
                if (player->GetSession())
                    player->GetSession()->SendAreaTriggerMessage("{} goes Berserk! (+30% damage)", creature->GetName());
    }
}
}

class mythic_wotlk_dungeons_all_map : public AllMapScript
{
public:
    mythic_wotlk_dungeons_all_map()
        : AllMapScript("mythic_wotlk_dungeons_all_map", { ALLMAPHOOK_ON_PLAYER_ENTER_ALL, ALLMAPHOOK_ON_DESTROY_MAP, ALLMAPHOOK_ON_MAP_UPDATE }) { }

    void OnPlayerEnterAll(Map* map, Player* player) override
    {
        MythicInstanceState* state = GetMythicState(map, true);
        if (!state || !state->Enabled)
            return;

        EnsureSelectorInMap(map, *state, player);

        if (player && player->GetSession())
            player->GetSession()->SendAreaTriggerMessage("Mythic {} active (HP x{:.2f}, Damage x{:.2f}). Talk to the Mythic Difficulty Selector to set 0-{}.",
                uint32(state->Level), state->HealthMultiplier, state->DamageMultiplier, uint32(state->MaxLevel));
    }

    void OnDestroyMap(Map* map) override
    {
        if (!map || !map->IsDungeon())
            return;

        InstanceMap* instanceMap = map->ToInstanceMap();
        if (!instanceMap)
            return;

        MythicStates.erase(instanceMap->GetInstanceId());
    }

    void OnMapUpdate(Map* map, uint32 diff) override
    {
        MythicInstanceState* state = GetMythicState(map, true);
        if (!state || !state->Enabled)
            return;

        EnsureSelectorInMap(map, *state, nullptr);

        if (state->PendingRescale)
        {
            RescaleMythicCreaturesInMap(map, *state);
            state->PendingRescale = false;
        }

        if (state->Level < MYTHIC_BERSERK_MIN_LEVEL)
        {
            if (state->LootRebuildScanTimerMs > diff)
            {
                state->LootRebuildScanTimerMs -= diff;
            }
            else
            {
                state->LootRebuildScanTimerMs = 1000;
                for (auto const& spawnPair : map->GetCreatureBySpawnIdStore())
                {
                    Creature* creature = spawnPair.second;
                    if (!creature || !creature->IsDungeonBoss() || creature->IsAlive())
                        continue;

                    RebuildMythicBossLoot(*state, creature);
                    GrantMythicBossReward(*state, creature);
                }
            }
            return;
        }

        if (state->BerserkCheckTimerMs > diff)
        {
            state->BerserkCheckTimerMs -= diff;
            return;
        }

        state->BerserkCheckTimerMs = 500;
        for (auto const& spawnPair : map->GetCreatureBySpawnIdStore())
        {
            if (Creature* creature = spawnPair.second)
            {
                TryApplyMythicBossBerserk(*state, creature);
            }
        }

        if (state->LootRebuildScanTimerMs > diff)
        {
            state->LootRebuildScanTimerMs -= diff;
        }
        else
        {
            state->LootRebuildScanTimerMs = 1000;
            for (auto const& spawnPair : map->GetCreatureBySpawnIdStore())
            {
                Creature* creature = spawnPair.second;
                if (!creature || !creature->IsDungeonBoss() || creature->IsAlive())
                    continue;

                RebuildMythicBossLoot(*state, creature);
                GrantMythicBossReward(*state, creature);
            }
        }
    }
};

class mythic_wotlk_dungeons_all_creature : public AllCreatureScript
{
public:
    mythic_wotlk_dungeons_all_creature() : AllCreatureScript("mythic_wotlk_dungeons_all_creature") { }

    void OnCreatureAddWorld(Creature* creature) override
    {
        MythicInstanceState* state = GetMythicState(creature ? creature->GetMap() : nullptr, true);
        if (!state || !state->Enabled || !creature)
            return;

        if (IsAnyMythicSelectorEntry(creature->GetEntry()))
        {
            state->SelectorGuid = creature->GetGUID();
            return;
        }

        ApplyMythicScaleToCreature(*state, creature, false);
        TrySpawnMythicExtraTrash(*state, creature);
    }
};

class mythic_wotlk_dungeons_all_gameobject : public AllGameObjectScript
{
public:
    mythic_wotlk_dungeons_all_gameobject() : AllGameObjectScript("mythic_wotlk_dungeons_all_gameobject") { }

    void OnGameObjectLootStateChanged(GameObject* go, uint32 state, Unit* /*unit*/) override
    {
        if (!go || state != GO_ACTIVATED)
            return;

        MythicInstanceState* mythicState = GetMythicState(go->GetMap(), true);
        if (!mythicState || !mythicState->Enabled)
            return;

        RemapLootItemsToMythicLevel(go->loot.items, mythicState->Level);
    }
};

class mythic_wotlk_dungeons_unit_script : public UnitScript
{
public:
    mythic_wotlk_dungeons_unit_script()
        : UnitScript("mythic_wotlk_dungeons_unit_script", true, { UNITHOOK_ON_UNIT_ENTER_COMBAT, UNITHOOK_ON_UNIT_DEATH }) { }

    void OnUnitEnterCombat(Unit* unit, Unit* /*victim*/) override
    {
        Creature* creature = unit ? unit->ToCreature() : nullptr;
        if (!creature || !creature->IsDungeonBoss())
            return;

        MythicInstanceState* state = GetMythicState(creature->GetMap(), true);
        if (!state || !state->Enabled)
            return;

        state->Locked = true;
    }

    void OnUnitDeath(Unit* unit, Unit* /*killer*/) override
    {
        Creature* creature = unit ? unit->ToCreature() : nullptr;
        if (!creature || !creature->IsDungeonBoss())
            return;

        MythicInstanceState* state = GetMythicState(creature->GetMap(), true);
        if (!state || !state->Enabled)
            return;

        state->Locked = true;
        RebuildMythicBossLoot(*state, creature);
        GrantMythicBossReward(*state, creature);
    }
};

class npc_wotlk_mythic_selector : public CreatureScript
{
public:
    npc_wotlk_mythic_selector() : CreatureScript("npc_wotlk_mythic_selector") { }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        if (!player || !creature)
            return true;

        MythicInstanceState* state = GetMythicState(player->GetMap(), true);
        if (!state || !state->Enabled)
        {
            if (player->GetSession())
                player->GetSession()->SendAreaTriggerMessage("Mythic mode is only available in Heroic WotLK dungeons.");
            return true;
        }

        ClearGossipMenuFor(player);

        for (uint8 level = 0; level <= state->MaxLevel; ++level)
        {
            std::string label = "Set Mythic " + std::to_string(level);
            if (level == state->Level)
                label += " (current)";
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, label, GOSSIP_SENDER_MAIN, MYTHIC_GOSSIP_ACTION_SET_LEVEL_BASE + level);
        }

        SendGossipMenuFor(player, player->GetGossipTextId(creature), creature->GetGUID());
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* /*creature*/, uint32 sender, uint32 action) override
    {
        if (!player || sender != GOSSIP_SENDER_MAIN)
            return true;

        ClearGossipMenuFor(player);
        CloseGossipMenuFor(player);

        if (action < MYTHIC_GOSSIP_ACTION_SET_LEVEL_BASE)
            return true;

        uint32 const requestedLevel = action - MYTHIC_GOSSIP_ACTION_SET_LEVEL_BASE;
        Map* map = player->GetMap();
        MythicInstanceState* state = GetMythicState(map, true);
        if (!state || !state->Enabled)
            return true;

        if (requestedLevel > state->MaxLevel)
        {
            if (player->GetSession())
                player->GetSession()->SendAreaTriggerMessage("Invalid Mythic level {}. Max is {}.", requestedLevel, uint32(state->MaxLevel));
            return true;
        }

        if (state->Locked && requestedLevel != state->Level)
        {
            if (player->GetSession())
                player->GetSession()->SendAreaTriggerMessage("Mythic level is locked for this run (active level remains {}). Reset the instance to change it.", uint32(state->Level));
            return true;
        }

        SetMythicLevelForMap(map, *state, uint8(requestedLevel));
        if (player->GetSession())
            player->GetSession()->SendAreaTriggerMessage("Mythic level set to {}.", uint32(state->Level));
        return true;
    }
};

void AddSC_mythic_wotlk_dungeons()
{
    new mythic_wotlk_dungeons_all_map();
    new mythic_wotlk_dungeons_all_creature();
    new mythic_wotlk_dungeons_all_gameobject();
    new mythic_wotlk_dungeons_unit_script();
    new npc_wotlk_mythic_selector();
}
