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
#include <sstream>
#include <unordered_map>
#include <vector>

namespace SpellMastery
{
std::array<ManagedSpellConfig, 10> const ManagedSpellConfigs =
{ {
    { SPELL_MAGE_FIREBALL_RANK_1, SPELL_MAGE_FIREBALL_RANK_3, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_MAGE_PYROBLAST_RANK_1, SPELL_MAGE_PYROBLAST_RANK_10, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_MAGE_FLAMESTRIKE_RANK_1, SPELL_MAGE_FLAMESTRIKE_RANK_3, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_SHAMAN_CHAIN_LIGHTNING_RANK_1, SPELL_SHAMAN_CHAIN_LIGHTNING_RANK_1, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_SHAMAN_LAVA_BURST_RANK_1, SPELL_SHAMAN_LAVA_BURST_RANK_1, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_WARRIOR_THUNDER_CLAP_RANK_1, SPELL_WARRIOR_THUNDER_CLAP_RANK_9, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_ROGUE_KILLING_SPREE, SPELL_ROGUE_KILLING_SPREE, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_WARLOCK_HAUNT_RANK_1, SPELL_WARLOCK_HAUNT_RANK_3, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_DRUID_REJUVENATION_RANK_1, SPELL_DRUID_REJUVENATION_RANK_5, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_DRUID_REGROWTH_RANK_1, SPELL_DRUID_REGROWTH_RANK_3, SPELL_MASTERY_TIER_DIAMOND, 10 }
} };

char const SPELL_MASTERY_CHARACTER_TABLE[] = "character_spell_mastery";
char const SPELL_MASTERY_ADDON_PREFIX[] = "SMT";

std::unordered_map<MasteryCacheKey, SpellMasteryProgress, MasteryCacheKeyHash> SpellMasteryCache;
std::unordered_map<MasteryCacheKey, uint32, MasteryCacheKeyHash> SpellMasteryLastXpGrantMs;

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

SpellMasteryProgress& GetOrLoadSpellMasteryProgress(Player* player, ManagedSpellConfig const& config)
{
    MasteryCacheKey key = MakeMasteryCacheKey(player, config);

    auto itr = SpellMasteryCache.find(key);
    if (itr != SpellMasteryCache.end())
        return itr->second;

    SpellMasteryProgress const loaded = LoadSpellMasteryProgressFromDb(key.Guid, config);
    auto [newItr, _] = SpellMasteryCache.emplace(key, loaded);
    return newItr->second;
}

void ClearSpellMasteryCacheForPlayer(uint32 guid)
{
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

    for (auto itr = SpellMasteryLastXpGrantMs.begin(); itr != SpellMasteryLastXpGrantMs.end();)
    {
        if (itr->first.Guid == guid)
            itr = SpellMasteryLastXpGrantMs.erase(itr);
        else
            ++itr;
    }

    ClearSpellMasteryMageRuntimeStateForPlayer(guid);
    ClearSpellMasteryDruidRuntimeStateForPlayer(guid);
    ClearSpellMasteryWarriorRuntimeStateForPlayer(guid);
    ClearSpellMasteryWarlockRuntimeStateForPlayer(guid);
    ClearSpellMasteryRogueRuntimeStateForPlayer(guid);
    ClearSpellMasteryShamanRuntimeStateForPlayer(guid);
}

bool ShouldAwardSpellMasteryXp(Player* player, ManagedSpellConfig const& config, uint32 cooldownMs)
{
    if (!cooldownMs)
        return true;

    MasteryCacheKey key = MakeMasteryCacheKey(player, config);
    uint32 const nowMs = uint32(GameTime::GetGameTimeMS().count());

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
}

void AddSpellMasteryXp(Player* player, ManagedSpellConfig const& config, uint64 xpGain)
{
    if (!xpGain)
        return;

    SpellMasteryProgress& progress = GetOrLoadSpellMasteryProgress(player, config);
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
    AddSC_spell_mastery_shaman();
}
