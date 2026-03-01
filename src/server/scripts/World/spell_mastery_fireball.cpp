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

#include "spell_mastery_core.h"

#include "Chat.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Player.h"
#include "PlayerScript.h"
#include "SpellMgr.h"
#include "World.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace SpellMastery
{
std::array<ManagedSpellConfig, 33> const ManagedSpellConfigs =
{ {
    { SPELL_MAGE_FIREBALL_RANK_1, SPELL_MAGE_FIREBALL_RANK_3, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_MAGE_PYROBLAST_RANK_1, SPELL_MAGE_PYROBLAST_RANK_1, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_MAGE_FLAMESTRIKE_RANK_1, SPELL_MAGE_FLAMESTRIKE_RANK_3, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_MAGE_FROSTBOLT_RANK_1, SPELL_MAGE_FROSTBOLT_RANK_1, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_MAGE_ICE_LANCE_RANK_1, SPELL_MAGE_ICE_LANCE_RANK_1, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_MAGE_BLIZZARD_RANK_1, SPELL_MAGE_BLIZZARD_RANK_1, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_MAGE_CONE_OF_COLD_RANK_1, SPELL_MAGE_CONE_OF_COLD_RANK_1, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_MAGE_ARCANE_BLAST_RANK_1, SPELL_MAGE_ARCANE_BLAST_RANK_1, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_MAGE_ARCANE_MISSILES_RANK_1, SPELL_MAGE_ARCANE_MISSILES_RANK_1, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_MAGE_ARCANE_BARRAGE_RANK_1, SPELL_MAGE_ARCANE_BARRAGE_RANK_1, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_MAGE_ARCANE_EXPLOSION_RANK_1, SPELL_MAGE_ARCANE_EXPLOSION_RANK_1, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_SHAMAN_CHAIN_LIGHTNING_RANK_1, SPELL_SHAMAN_CHAIN_LIGHTNING_RANK_1, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_SHAMAN_LAVA_BURST_RANK_1, SPELL_SHAMAN_LAVA_BURST_RANK_1, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_PRIEST_POWER_WORD_SHIELD_RANK_1, SPELL_PRIEST_POWER_WORD_SHIELD_RANK_1, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_PRIEST_PENANCE_RANK_1, SPELL_PRIEST_PENANCE_RANK_1, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_PRIEST_FLASH_HEAL_RANK_1, SPELL_PRIEST_FLASH_HEAL_RANK_1, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_PALADIN_CONSECRATION_RANK_1, SPELL_PALADIN_CONSECRATION_RANK_1, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_WARRIOR_THUNDER_CLAP_RANK_1, SPELL_WARRIOR_THUNDER_CLAP_RANK_9, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_WARRIOR_REVENGE_RANK_1, SPELL_WARRIOR_REVENGE_RANK_1, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_ROGUE_KILLING_SPREE, SPELL_ROGUE_KILLING_SPREE, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_ROGUE_FAN_OF_KNIVES_RANK_1, SPELL_ROGUE_FAN_OF_KNIVES_RANK_1, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_ROGUE_RUPTURE_RANK_1, SPELL_ROGUE_RUPTURE_RANK_1, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_HUNTER_VOLLEY_RANK_1, SPELL_HUNTER_VOLLEY_RANK_1, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_HUNTER_AIMED_SHOT_RANK_1, SPELL_HUNTER_AIMED_SHOT_RANK_1, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_HUNTER_SERPENT_STING_RANK_1, SPELL_HUNTER_SERPENT_STING_RANK_1, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_WARLOCK_HAUNT_RANK_1, SPELL_WARLOCK_HAUNT_RANK_3, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_WARLOCK_SHADOW_BOLT_RANK_1, SPELL_WARLOCK_SHADOW_BOLT_RANK_1, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_WARLOCK_CHAOS_BOLT_RANK_1, SPELL_WARLOCK_CHAOS_BOLT_RANK_1, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_WARLOCK_RAIN_OF_FIRE_RANK_1, SPELL_WARLOCK_RAIN_OF_FIRE_RANK_1, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_DRUID_REJUVENATION_RANK_1, SPELL_DRUID_REJUVENATION_RANK_5, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_DRUID_REGROWTH_RANK_1, SPELL_DRUID_REGROWTH_RANK_3, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_DRUID_RIP_RANK_1, SPELL_DRUID_RIP_RANK_1, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_DRUID_SWIPE_CAT_RANK_1, SPELL_DRUID_SWIPE_CAT_RANK_1, SPELL_MASTERY_TIER_DIAMOND, 10 }
} };

char const SPELL_MASTERY_CHARACTER_TABLE[] = "character_spell_mastery";
char const SPELL_MASTERY_ADDON_PREFIX[] = "SMT";

std::unordered_map<MasteryCacheKey, SpellMasteryProgress, MasteryCacheKeyHash> SpellMasteryCache;
std::unordered_map<MasteryCacheKey, uint32, MasteryCacheKeyHash> SpellMasteryLastXpGrantMs;
std::mutex SpellMasteryCacheMutex;

uint32 constexpr EARLY_ACCESS_LEVEL_OFFSET = 4;
float constexpr EARLY_ACCESS_MIN_SCALE = 0.05f;
uint8 constexpr LOW_RANK_LEVEL_SCALING_MAX_RANK = 3;
float constexpr LOW_RANK_LEVEL_SCALING_PER_LEVEL = 0.08f;
float constexpr LOW_RANK_LEVEL_SCALING_MAX_MULTIPLIER = 8.0f;

ManagedSpellConfig const* GetManagedSpellConfigByBaseSpell(uint32 baseSpellId)
{
    for (ManagedSpellConfig const& config : ManagedSpellConfigs)
        if (config.BaseSpellId == baseSpellId)
            return &config;

    return nullptr;
}

ManagedSpellConfig const* GetManagedSpellConfigForSpell(uint32 spellId)
{
    uint32 firstRank = sSpellMgr->GetFirstSpellInChain(spellId);
    if (!firstRank)
        firstRank = spellId;

    return GetManagedSpellConfigByBaseSpell(firstRank);
}

char const* GetTierName(uint8 tier)
{
    switch (tier)
    {
        case SPELL_MASTERY_TIER_IRON: return "Iron";
        case SPELL_MASTERY_TIER_BRONZE: return "Bronze";
        case SPELL_MASTERY_TIER_SILVER: return "Silver";
        case SPELL_MASTERY_TIER_GOLD: return "Gold";
        case SPELL_MASTERY_TIER_DIAMOND: return "Diamond";
        default: return "Unknown";
    }
}

bool IsSpellMasteryFeedEnabled()
{
    return sWorld->getBoolConfig(CONFIG_CUSTOM_SPELL_MASTERY_FEED);
}

uint64 GetSpellMasteryNextXpThreshold(SpellMasteryProgress const& progress, ManagedSpellConfig const& config)
{
    if (progress.Tier >= config.MaxTier && progress.TierLevel >= config.MaxTierLevel)
        return 0;

    return 100ull * progress.Tier * progress.TierLevel;
}

float ComputeEarlyAccessSpellScale(Player* player, SpellInfo const* spellInfo)
{
    if (!player || !spellInfo)
        return 1.0f;

    uint32 const playerLevel = std::max<uint32>(1, player->GetLevel());
    uint32 const naturalLevel = std::max<uint32>(spellInfo->SpellLevel, spellInfo->BaseLevel);
    if (naturalLevel > 1 && playerLevel < naturalLevel)
    {
        float const adjustedPlayer = float(playerLevel + EARLY_ACCESS_LEVEL_OFFSET);
        float const adjustedNatural = float(naturalLevel + EARLY_ACCESS_LEVEL_OFFSET);
        return std::clamp(adjustedPlayer / adjustedNatural, EARLY_ACCESS_MIN_SCALE, 1.0f);
    }

    // Normalize very low spell ranks so rank-locked mastery baselines stay viable at high player level.
    uint32 const spellRank = sSpellMgr->GetSpellRank(spellInfo->Id);
    bool isLowRankMasterySpell = spellRank >= 1 && spellRank <= LOW_RANK_LEVEL_SCALING_MAX_RANK;
    if (!isLowRankMasterySpell)
    {
        if (ManagedSpellConfig const* config = GetManagedSpellConfigForSpell(spellInfo->Id))
        {
            uint32 const allowedRank = sSpellMgr->GetSpellRank(config->AllowedSpellId);
            isLowRankMasterySpell = allowedRank >= 1 && allowedRank <= LOW_RANK_LEVEL_SCALING_MAX_RANK;
        }
    }

    if (isLowRankMasterySpell && playerLevel > naturalLevel)
    {
        uint32 const levelGap = playerLevel - naturalLevel;
        float const multiplier = 1.0f + (float(levelGap) * LOW_RANK_LEVEL_SCALING_PER_LEVEL);
        return std::clamp(multiplier, 1.0f, LOW_RANK_LEVEL_SCALING_MAX_MULTIPLIER);
    }

    return 1.0f;
}

int32 ApplyEarlyAccessSpellScale(Player* player, SpellInfo const* spellInfo, int32 amount)
{
    if (!amount)
        return amount;

    float const scale = ComputeEarlyAccessSpellScale(player, spellInfo);
    if (scale >= 0.999f)
        return amount;

    int32 scaledAmount = int32(std::lround(float(amount) * scale));
    if (amount > 0)
        return std::max<int32>(1, scaledAmount);

    return std::min<int32>(-1, scaledAmount);
}

void SendMasteryAddonMessage(Player* player, uint32 baseSpellId, uint8 tier, uint8 level, uint64 xp, uint64 next)
{
    if (!player || !player->GetSession() || !IsSpellMasteryFeedEnabled())
        return;

    std::ostringstream payload;
    payload << "SPELL=" << baseSpellId
        << ";TIER=" << uint32(tier)
        << ";LVL=" << uint32(level)
        << ";XP=" << xp
        << ";NEXT=" << next;

    WorldPacket data;
    std::string const message = std::string(SPELL_MASTERY_ADDON_PREFIX) + "\t" + payload.str();
    ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, message);
    player->GetSession()->SendPacket(&data);
}

void SendMasteryAddonMessageForProgress(Player* player, ManagedSpellConfig const& config, SpellMasteryProgress const& progress)
{
    SendMasteryAddonMessage(player, config.BaseSpellId, progress.Tier, progress.TierLevel, progress.Xp, GetSpellMasteryNextXpThreshold(progress, config));
}

SpellMasteryProgress SanitizeMasteryProgress(SpellMasteryProgress progress, ManagedSpellConfig const& config)
{
    if (progress.Tier < SPELL_MASTERY_TIER_IRON)
        progress.Tier = SPELL_MASTERY_TIER_IRON;

    if (progress.Tier > config.MaxTier)
        progress.Tier = config.MaxTier;

    if (progress.TierLevel < 1)
        progress.TierLevel = 1;

    if (progress.TierLevel > config.MaxTierLevel)
        progress.TierLevel = config.MaxTierLevel;

    return progress;
}

SpellMasteryProgress LoadSpellMasteryProgressFromDb(uint32 guid, ManagedSpellConfig const& config)
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT `tier`, `tier_level`, `xp` FROM `{}` WHERE `guid` = {} AND `base_spell_id` = {}",
        SPELL_MASTERY_CHARACTER_TABLE, guid, config.BaseSpellId);

    if (result)
    {
        Field* fields = result->Fetch();

        SpellMasteryProgress progress;
        progress.Tier = fields[0].Get<uint8>();
        progress.TierLevel = fields[1].Get<uint8>();
        progress.Xp = fields[2].Get<uint64>();

        return SanitizeMasteryProgress(progress, config);
    }

    CharacterDatabase.Execute(
        "INSERT INTO `{}` (`guid`, `base_spell_id`, `tier`, `tier_level`, `xp`) VALUES ({}, {}, 1, 1, 0)",
        SPELL_MASTERY_CHARACTER_TABLE, guid, config.BaseSpellId);

    return SpellMasteryProgress();
}

void SaveSpellMasteryProgressToDb(uint32 guid, ManagedSpellConfig const& config, SpellMasteryProgress const& progress)
{
    CharacterDatabase.Execute(
        "INSERT INTO `{}` (`guid`, `base_spell_id`, `tier`, `tier_level`, `xp`) VALUES ({}, {}, {}, {}, {}) "
        "ON DUPLICATE KEY UPDATE `tier` = VALUES(`tier`), `tier_level` = VALUES(`tier_level`), `xp` = VALUES(`xp`)",
        SPELL_MASTERY_CHARACTER_TABLE, guid, config.BaseSpellId, progress.Tier, progress.TierLevel, progress.Xp);
}

MasteryCacheKey MakeMasteryCacheKey(Player* player, ManagedSpellConfig const& config)
{
    return
    {
        uint32(player->GetGUID().GetCounter()),
        config.BaseSpellId
    };
}

SpellMasteryProgress GetOrLoadSpellMasteryProgress(Player* player, ManagedSpellConfig const& config)
{
    MasteryCacheKey key = MakeMasteryCacheKey(player, config);
    {
        std::lock_guard<std::mutex> lock(SpellMasteryCacheMutex);
        auto itr = SpellMasteryCache.find(key);
        if (itr != SpellMasteryCache.end())
            return itr->second;
    }

    SpellMasteryProgress const loaded = LoadSpellMasteryProgressFromDb(key.Guid, config);
    {
        std::lock_guard<std::mutex> lock(SpellMasteryCacheMutex);
        auto [itr, inserted] = SpellMasteryCache.emplace(key, loaded);
        if (!inserted)
            return itr->second;
    }

    return loaded;
}

void ClearSpellMasteryCacheForPlayer(uint32 guid)
{
    std::lock_guard<std::mutex> lock(SpellMasteryCacheMutex);

    for (auto itr = SpellMasteryCache.begin(); itr != SpellMasteryCache.end();)
    {
        if (itr->first.Guid == guid)
            itr = SpellMasteryCache.erase(itr);
        else
            ++itr;
    }
}

void ClearSpellMasteryRuntimeStateForPlayer(uint32 guid)
{
    ClearSpellMasteryCacheForPlayer(guid);

    {
        std::lock_guard<std::mutex> lock(SpellMasteryCacheMutex);
        for (auto itr = SpellMasteryLastXpGrantMs.begin(); itr != SpellMasteryLastXpGrantMs.end();)
        {
            if (itr->first.Guid == guid)
                itr = SpellMasteryLastXpGrantMs.erase(itr);
            else
                ++itr;
        }
    }

    ClearSpellMasteryMageRuntimeStateForPlayer(guid);
    ClearSpellMasteryDruidRuntimeStateForPlayer(guid);
    ClearSpellMasteryWarriorRuntimeStateForPlayer(guid);
    ClearSpellMasteryWarlockRuntimeStateForPlayer(guid);
    ClearSpellMasteryRogueRuntimeStateForPlayer(guid);
    ClearSpellMasteryHunterRuntimeStateForPlayer(guid);
    ClearSpellMasteryShamanRuntimeStateForPlayer(guid);
    ClearSpellMasteryPaladinRuntimeStateForPlayer(guid);
    ClearSpellMasteryPriestRuntimeStateForPlayer(guid);
}

bool ShouldAwardSpellMasteryXp(Player* player, ManagedSpellConfig const& config, uint32 cooldownMs)
{
    if (!cooldownMs)
        return true;

    MasteryCacheKey key = MakeMasteryCacheKey(player, config);
    uint32 const nowMs = uint32(GameTime::GetGameTimeMS().count());
    std::lock_guard<std::mutex> lock(SpellMasteryCacheMutex);

    auto itr = SpellMasteryLastXpGrantMs.find(key);
    if (itr != SpellMasteryLastXpGrantMs.end() && nowMs >= itr->second && (nowMs - itr->second) < cooldownMs)
        return false;

    SpellMasteryLastXpGrantMs[key] = nowMs;
    return true;
}

uint8 GetEffectiveTierLevel(SpellMasteryProgress const& progress, uint8 targetTier, ManagedSpellConfig const& config)
{
    if (targetTier < SPELL_MASTERY_TIER_IRON || targetTier > config.MaxTier)
        return 0;

    if (progress.Tier > targetTier)
        return config.MaxTierLevel;

    if (progress.Tier == targetTier)
        return progress.TierLevel;

    return 0;
}

bool IsManagedRankSpell(uint32 spellId, ManagedSpellConfig const** outConfig)
{
    ManagedSpellConfig const* config = GetManagedSpellConfigForSpell(spellId);
    if (!config)
        return false;

    if (outConfig)
        *outConfig = config;

    return true;
}

void EnforceManagedSpellBaseRankOnly(Player* player, ManagedSpellConfig const& config)
{
    std::vector<uint32> spellsToRemove;
    PlayerSpellMap const& spellMap = player->GetSpellMap();
    bool hasAnyRankInChain = false;

    for (auto const& itr : spellMap)
    {
        uint32 spellId = itr.first;
        uint32 firstRank = sSpellMgr->GetFirstSpellInChain(spellId);
        if (!firstRank)
            continue;

        if (firstRank == config.BaseSpellId)
        {
            hasAnyRankInChain = true;
            if (spellId != config.AllowedSpellId)
                spellsToRemove.push_back(spellId);
        }
    }

    if (!hasAnyRankInChain && config.BaseSpellId == SPELL_WARRIOR_THUNDER_CLAP_RANK_1 && player->getClass() == CLASS_WARRIOR)
    {
        SpellInfo const* baseInfo = sSpellMgr->GetSpellInfo(config.BaseSpellId);
        if (!baseInfo || player->GetLevel() >= baseInfo->SpellLevel)
            hasAnyRankInChain = true;
    }

    if (!hasAnyRankInChain && config.BaseSpellId == SPELL_SHAMAN_CHAIN_LIGHTNING_RANK_1 && player->getClass() == CLASS_SHAMAN)
        hasAnyRankInChain = true;

    if (!hasAnyRankInChain && config.BaseSpellId == SPELL_PALADIN_CONSECRATION_RANK_1 && player->getClass() == CLASS_PALADIN)
        hasAnyRankInChain = true;

    if (!hasAnyRankInChain && config.BaseSpellId == SPELL_PRIEST_POWER_WORD_SHIELD_RANK_1 && player->getClass() == CLASS_PRIEST)
        hasAnyRankInChain = true;

    if (hasAnyRankInChain && !player->HasSpell(config.AllowedSpellId))
    {
        // Upgrade managed spells to the configured baseline rank before pruning other chain ranks.
        player->learnSpell(config.AllowedSpellId);
        spellsToRemove.erase(std::remove(spellsToRemove.begin(), spellsToRemove.end(), config.AllowedSpellId), spellsToRemove.end());
    }

    for (uint32 spellId : spellsToRemove)
        player->removeSpell(spellId, SPEC_MASK_ALL, false);
}

void EnforceAllManagedSpellBaseRanks(Player* player)
{
    for (ManagedSpellConfig const& config : ManagedSpellConfigs)
        EnforceManagedSpellBaseRankOnly(player, config);
}

void EnsureShamanInstantLevelOneSpells(Player* player)
{
    if (!player || player->getClass() != CLASS_SHAMAN || player->GetLevel() < 1)
        return;

    if (!player->HasSpell(SPELL_SHAMAN_CHAIN_LIGHTNING_RANK_1))
        player->learnSpell(SPELL_SHAMAN_CHAIN_LIGHTNING_RANK_1);

    if (!player->HasSpell(SPELL_SHAMAN_LAVA_BURST_RANK_1))
        player->learnSpell(SPELL_SHAMAN_LAVA_BURST_RANK_1);

    if (!player->HasSpell(SPELL_SHAMAN_VOLCANIC_ERUPTION))
        player->learnSpell(SPELL_SHAMAN_VOLCANIC_ERUPTION);

    if (SPELL_SHAMAN_VOLCANIC_ERUPTION != 66690 && player->HasSpell(66690))
        player->removeSpell(66690, SPEC_MASK_ALL, false);
}

void EnsureMageInstantLevelOneSpells(Player* player)
{
    if (!player || player->getClass() != CLASS_MAGE || player->GetLevel() < 1)
        return;

    if (!player->HasSpell(SPELL_MAGE_FROSTBOLT_RANK_1))
        player->learnSpell(SPELL_MAGE_FROSTBOLT_RANK_1);

    if (!player->HasSpell(SPELL_MAGE_ICE_LANCE_RANK_1))
        player->learnSpell(SPELL_MAGE_ICE_LANCE_RANK_1);

    if (!player->HasSpell(SPELL_MAGE_BLIZZARD_RANK_1))
        player->learnSpell(SPELL_MAGE_BLIZZARD_RANK_1);

    if (!player->HasSpell(SPELL_MAGE_CONE_OF_COLD_RANK_1))
        player->learnSpell(SPELL_MAGE_CONE_OF_COLD_RANK_1);

    if (!player->HasSpell(SPELL_MAGE_ARCANE_BLAST_RANK_1))
        player->learnSpell(SPELL_MAGE_ARCANE_BLAST_RANK_1);

    if (!player->HasSpell(SPELL_MAGE_ARCANE_MISSILES_RANK_1))
        player->learnSpell(SPELL_MAGE_ARCANE_MISSILES_RANK_1);

    if (!player->HasSpell(SPELL_MAGE_ARCANE_BARRAGE_RANK_1))
        player->learnSpell(SPELL_MAGE_ARCANE_BARRAGE_RANK_1);

    if (!player->HasSpell(SPELL_MAGE_ARCANE_EXPLOSION_RANK_1))
        player->learnSpell(SPELL_MAGE_ARCANE_EXPLOSION_RANK_1);

    if (!player->HasSpell(SPELL_MAGE_PYROBLAST_RANK_1))
        player->learnSpell(SPELL_MAGE_PYROBLAST_RANK_1);
}

void EnsurePaladinInstantLevelOneSpells(Player* player)
{
    if (!player || player->getClass() != CLASS_PALADIN || player->GetLevel() < 1)
        return;

    if (!player->HasSpell(SPELL_PALADIN_CONSECRATION_RANK_1))
        player->learnSpell(SPELL_PALADIN_CONSECRATION_RANK_1);
}

void EnsureWarriorInstantLevelOneSpells(Player* player)
{
    if (!player || player->getClass() != CLASS_WARRIOR || player->GetLevel() < 1)
        return;

    if (!player->HasSpell(SPELL_WARRIOR_REVENGE_RANK_1))
        player->learnSpell(SPELL_WARRIOR_REVENGE_RANK_1);
}

void EnsurePriestInstantLevelOneSpells(Player* player)
{
    if (!player || player->getClass() != CLASS_PRIEST || player->GetLevel() < 1)
        return;

    if (!player->HasSpell(SPELL_PRIEST_POWER_WORD_SHIELD_RANK_1))
        player->learnSpell(SPELL_PRIEST_POWER_WORD_SHIELD_RANK_1);

    if (!player->HasSpell(SPELL_PRIEST_PENANCE_RANK_1))
        player->learnSpell(SPELL_PRIEST_PENANCE_RANK_1);

    if (!player->HasSpell(SPELL_PRIEST_FLASH_HEAL_RANK_1))
        player->learnSpell(SPELL_PRIEST_FLASH_HEAL_RANK_1);
}

void EnsureDruidInstantLevelOneSpells(Player* player)
{
    if (!player || player->getClass() != CLASS_DRUID || player->GetLevel() < 1)
        return;

    if (!player->HasSpell(SPELL_DRUID_CAT_FORM_RANK_1))
        player->learnSpell(SPELL_DRUID_CAT_FORM_RANK_1);

    if (!player->HasSpell(SPELL_DRUID_CLAW_RANK_1))
        player->learnSpell(SPELL_DRUID_CLAW_RANK_1);

    if (!player->HasSpell(SPELL_DRUID_RIP_RANK_1))
        player->learnSpell(SPELL_DRUID_RIP_RANK_1);

    if (!player->HasSpell(SPELL_DRUID_SWIPE_CAT_RANK_1))
        player->learnSpell(SPELL_DRUID_SWIPE_CAT_RANK_1);
}

void EnsureHunterInstantLevelOneSpells(Player* player)
{
    if (!player || player->getClass() != CLASS_HUNTER || player->GetLevel() < 1)
        return;

    if (!player->HasSpell(SPELL_HUNTER_VOLLEY_RANK_1))
        player->learnSpell(SPELL_HUNTER_VOLLEY_RANK_1);

    if (!player->HasSpell(SPELL_HUNTER_AIMED_SHOT_RANK_1))
        player->learnSpell(SPELL_HUNTER_AIMED_SHOT_RANK_1);

    if (!player->HasSpell(SPELL_HUNTER_SERPENT_STING_RANK_1))
        player->learnSpell(SPELL_HUNTER_SERPENT_STING_RANK_1);
}

void EnsureWarlockInstantLevelOneSpells(Player* player)
{
    if (!player || player->getClass() != CLASS_WARLOCK || player->GetLevel() < 1)
        return;

    if (!player->HasSpell(SPELL_WARLOCK_HAUNT_RANK_3))
        player->learnSpell(SPELL_WARLOCK_HAUNT_RANK_3);

    if (!player->HasSpell(SPELL_WARLOCK_SHADOW_BOLT_RANK_1))
        player->learnSpell(SPELL_WARLOCK_SHADOW_BOLT_RANK_1);

    if (!player->HasSpell(SPELL_WARLOCK_CHAOS_BOLT_RANK_1))
        player->learnSpell(SPELL_WARLOCK_CHAOS_BOLT_RANK_1);

    if (!player->HasSpell(SPELL_WARLOCK_RAIN_OF_FIRE_RANK_1))
        player->learnSpell(SPELL_WARLOCK_RAIN_OF_FIRE_RANK_1);
}

void EnsureRogueInstantLevelOneSpells(Player* player)
{
    if (!player || player->getClass() != CLASS_ROGUE || player->GetLevel() < 1)
        return;

    if (!player->HasSpell(SPELL_ROGUE_FAN_OF_KNIVES_RANK_1))
        player->learnSpell(SPELL_ROGUE_FAN_OF_KNIVES_RANK_1);

    if (!player->HasSpell(SPELL_ROGUE_RUPTURE_RANK_1))
        player->learnSpell(SPELL_ROGUE_RUPTURE_RANK_1);
}

void AddSpellMasteryXp(Player* player, ManagedSpellConfig const& config, uint64 xpGain)
{
    if (!xpGain)
        return;

    xpGain *= SPELL_MASTERY_XP_MULTIPLIER;

    SpellMasteryProgress progress = GetOrLoadSpellMasteryProgress(player, config);
    progress.Xp += xpGain;

    bool leveled = false;
    while (progress.Tier < config.MaxTier || progress.TierLevel < config.MaxTierLevel)
    {
        uint64 const threshold = 100ull * progress.Tier * progress.TierLevel;
        if (progress.Xp < threshold)
            break;

        progress.Xp -= threshold;
        leveled = true;

        if (progress.TierLevel < config.MaxTierLevel)
            ++progress.TierLevel;
        else if (progress.Tier < config.MaxTier)
        {
            ++progress.Tier;
            progress.TierLevel = 1;
        }
        else
            break;
    }

    {
        std::lock_guard<std::mutex> lock(SpellMasteryCacheMutex);
        SpellMasteryCache[MakeMasteryCacheKey(player, config)] = progress;
    }

    SaveSpellMasteryProgressToDb(uint32(player->GetGUID().GetCounter()), config, progress);
    SendMasteryAddonMessageForProgress(player, config, progress);

    if (leveled && player->GetSession())
    {
        SpellInfo const* baseSpellInfo = sSpellMgr->GetSpellInfo(config.BaseSpellId);
        char const* spellName = baseSpellInfo ? baseSpellInfo->SpellName[DEFAULT_LOCALE] : "Spell";
        player->GetSession()->SendAreaTriggerMessage("{} Mastery advanced to {} Tier Level {}.", spellName, GetTierName(progress.Tier), progress.TierLevel);
    }
}
}

class spell_mastery_player_script : public PlayerScript
{
public:
    spell_mastery_player_script() : PlayerScript("spell_mastery_player_script", { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LEVEL_CHANGED, PLAYERHOOK_ON_LEARN_SPELL, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_DELETE })
    {
    }

    void OnPlayerLogin(Player* player) override
    {
        if (!player)
            return;

        SpellMastery::EnsureShamanInstantLevelOneSpells(player);
        SpellMastery::EnsureMageInstantLevelOneSpells(player);
        SpellMastery::EnsurePaladinInstantLevelOneSpells(player);
        SpellMastery::EnsureWarriorInstantLevelOneSpells(player);
        SpellMastery::EnsurePriestInstantLevelOneSpells(player);
        SpellMastery::EnsureDruidInstantLevelOneSpells(player);
        SpellMastery::EnsureHunterInstantLevelOneSpells(player);
        SpellMastery::EnsureWarlockInstantLevelOneSpells(player);
        SpellMastery::EnsureRogueInstantLevelOneSpells(player);
        SpellMastery::EnforceAllManagedSpellBaseRanks(player);

        for (SpellMastery::ManagedSpellConfig const& config : SpellMastery::ManagedSpellConfigs)
        {
            SpellMastery::SpellMasteryProgress const& progress = SpellMastery::GetOrLoadSpellMasteryProgress(player, config);
            SpellMastery::SendMasteryAddonMessageForProgress(player, config, progress);
        }
    }

    void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override
    {
        if (!player)
            return;

        SpellMastery::EnsureShamanInstantLevelOneSpells(player);
        SpellMastery::EnsureMageInstantLevelOneSpells(player);
        SpellMastery::EnsurePaladinInstantLevelOneSpells(player);
        SpellMastery::EnsureWarriorInstantLevelOneSpells(player);
        SpellMastery::EnsurePriestInstantLevelOneSpells(player);
        SpellMastery::EnsureDruidInstantLevelOneSpells(player);
        SpellMastery::EnsureHunterInstantLevelOneSpells(player);
        SpellMastery::EnsureWarlockInstantLevelOneSpells(player);
        SpellMastery::EnsureRogueInstantLevelOneSpells(player);
        SpellMastery::EnforceAllManagedSpellBaseRanks(player);
    }

    void OnPlayerLearnSpell(Player* player, uint32 spellID) override
    {
        SpellMastery::ManagedSpellConfig const* config = nullptr;
        if (!player || !SpellMastery::IsManagedRankSpell(spellID, &config) || spellID == config->AllowedSpellId)
            return;

        if (!player->HasSpell(config->AllowedSpellId))
            player->learnSpell(config->AllowedSpellId);

        player->removeSpell(spellID, SPEC_MASK_ALL, false);
        if (player->GetSession())
        {
            SpellInfo const* baseSpellInfo = sSpellMgr->GetSpellInfo(config->AllowedSpellId);
            char const* spellName = baseSpellInfo ? baseSpellInfo->SpellName[DEFAULT_LOCALE] : "This spell";
            uint32 const rankToUse = std::max<uint32>(1, sSpellMgr->GetSpellRank(config->AllowedSpellId));
            player->GetSession()->SendAreaTriggerMessage("{} ranks are disabled by Spell Mastery. Use Rank {}.", spellName, rankToUse);
        }
    }

    void OnPlayerLogout(Player* player) override
    {
        if (player)
            SpellMastery::ClearSpellMasteryRuntimeStateForPlayer(uint32(player->GetGUID().GetCounter()));
    }

    void OnPlayerDelete(ObjectGuid guid, uint32 /*accountId*/) override
    {
        SpellMastery::ClearSpellMasteryRuntimeStateForPlayer(uint32(guid.GetCounter()));
    }
};

void AddSC_spell_mastery_fireball()
{
    new spell_mastery_player_script();
    AddSC_spell_mastery_mage();
    AddSC_spell_mastery_druid();
    AddSC_spell_mastery_warrior();
    AddSC_spell_mastery_warlock();
    AddSC_spell_mastery_rogue();
    AddSC_spell_mastery_hunter();
    AddSC_spell_mastery_shaman();
    AddSC_spell_mastery_paladin();
    AddSC_spell_mastery_priest();
}
