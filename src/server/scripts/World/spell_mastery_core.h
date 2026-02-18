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

#ifndef AC_SPELL_MASTERY_CORE_H
#define AC_SPELL_MASTERY_CORE_H

#include "Define.h"
#include <cstddef>

class Player;

namespace SpellMastery
{
enum SpellMasteryConstants : uint32
{
    SPELL_MAGE_FIREBALL_RANK_1 = 133,
    SPELL_MAGE_FIREBALL_RANK_3 = 145,
    SPELL_MAGE_FLAMESTRIKE_RANK_1 = 2120,
    SPELL_MAGE_FLAMESTRIKE_RANK_3 = 8422,
    SPELL_WARRIOR_THUNDER_CLAP_RANK_1 = 6343,
    SPELL_WARRIOR_THUNDER_CLAP_RANK_9 = 47502,
    SPELL_WARRIOR_REND_RANK_1 = 772,
    SPELL_ROGUE_KILLING_SPREE = 51690,
    SPELL_ROGUE_KILLING_SPREE_WEAPON_DMG = 57841,
    SPELL_WARLOCK_HAUNT_RANK_1 = 48181,
    SPELL_WARLOCK_HAUNT_RANK_3 = 59163,
    SPELL_WARLOCK_HAUNT_HEAL = 48210,
    SPELL_DRUID_REJUVENATION_RANK_1 = 774,
    SPELL_DRUID_REJUVENATION_RANK_3 = 1430,
    SPELL_DRUID_REJUVENATION_RANK_5 = 2091,
    SPELL_DRUID_REGROWTH_RANK_1 = 8936,
    SPELL_DRUID_REGROWTH_RANK_3 = 8939,
    SPELL_MAGE_IGNITE = 12654
};

enum SpellMasteryTier : uint8
{
    SPELL_MASTERY_TIER_IRON = 1,
    SPELL_MASTERY_TIER_BRONZE = 2,
    SPELL_MASTERY_TIER_SILVER = 3,
    SPELL_MASTERY_TIER_GOLD = 4,
    SPELL_MASTERY_TIER_DIAMOND = 5
};

struct ManagedSpellConfig
{
    uint32 BaseSpellId;
    uint32 AllowedSpellId;
    uint8 MaxTier;
    uint8 MaxTierLevel;
};

inline constexpr uint64 SPELL_MASTERY_XP_PER_HIT = 100;

struct SpellMasteryProgress
{
    uint8 Tier = SPELL_MASTERY_TIER_IRON;
    uint8 TierLevel = 1;
    uint64 Xp = 0;
};

struct MasteryCacheKey
{
    uint32 Guid;
    uint32 BaseSpellId;

    bool operator==(MasteryCacheKey const& other) const
    {
        return Guid == other.Guid && BaseSpellId == other.BaseSpellId;
    }
};

struct MasteryCacheKeyHash
{
    std::size_t operator()(MasteryCacheKey const& key) const
    {
        return (std::size_t(key.Guid) << 32) ^ key.BaseSpellId;
    }
};

extern char const SPELL_MASTERY_CHARACTER_TABLE[];
extern char const SPELL_MASTERY_ADDON_PREFIX[];

ManagedSpellConfig const* GetManagedSpellConfigByBaseSpell(uint32 baseSpellId);
ManagedSpellConfig const* GetManagedSpellConfigForSpell(uint32 spellId);
char const* GetTierName(uint8 tier);
bool IsSpellMasteryFeedEnabled();
uint64 GetSpellMasteryNextXpThreshold(SpellMasteryProgress const& progress, ManagedSpellConfig const& config);
void SendMasteryAddonMessage(Player* player, uint32 baseSpellId, uint8 tier, uint8 level, uint64 xp, uint64 next);
void SendMasteryAddonMessageForProgress(Player* player, ManagedSpellConfig const& config, SpellMasteryProgress const& progress);
SpellMasteryProgress SanitizeMasteryProgress(SpellMasteryProgress progress, ManagedSpellConfig const& config);
SpellMasteryProgress LoadSpellMasteryProgressFromDb(uint32 guid, ManagedSpellConfig const& config);
void SaveSpellMasteryProgressToDb(uint32 guid, ManagedSpellConfig const& config, SpellMasteryProgress const& progress);
MasteryCacheKey MakeMasteryCacheKey(Player* player, ManagedSpellConfig const& config);
SpellMasteryProgress& GetOrLoadSpellMasteryProgress(Player* player, ManagedSpellConfig const& config);
void ClearSpellMasteryCacheForPlayer(uint32 guid);
void ClearSpellMasteryRuntimeStateForPlayer(uint32 guid);
bool ShouldAwardSpellMasteryXp(Player* player, ManagedSpellConfig const& config, uint32 cooldownMs);
uint8 GetEffectiveTierLevel(SpellMasteryProgress const& progress, uint8 targetTier, ManagedSpellConfig const& config);
bool IsManagedRankSpell(uint32 spellId, ManagedSpellConfig const** outConfig = nullptr);
void EnforceManagedSpellBaseRankOnly(Player* player, ManagedSpellConfig const& config);
void EnforceAllManagedSpellBaseRanks(Player* player);
void AddSpellMasteryXp(Player* player, ManagedSpellConfig const& config, uint64 xpGain);
}

void ClearSpellMasteryMageRuntimeStateForPlayer(uint32 guid);
void ClearSpellMasteryDruidRuntimeStateForPlayer(uint32 guid);
void ClearSpellMasteryWarriorRuntimeStateForPlayer(uint32 guid);
void ClearSpellMasteryWarlockRuntimeStateForPlayer(uint32 guid);
void ClearSpellMasteryRogueRuntimeStateForPlayer(uint32 guid);
void AddSC_spell_mastery_mage();
void AddSC_spell_mastery_druid();
void AddSC_spell_mastery_warrior();
void AddSC_spell_mastery_warlock();
void AddSC_spell_mastery_rogue();

#endif
