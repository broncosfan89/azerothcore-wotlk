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

#include "AllSpellScript.h"
#include "Cell.h"
#include "CellImpl.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Player.h"
#include "PlayerScript.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "World.h"
#include "WorldSession.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace
{
enum SpellMasteryConstants : uint32
{
    SPELL_MAGE_FIREBALL_RANK_1 = 133,
    SPELL_MAGE_FLAMESTRIKE_RANK_1 = 2120,
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
    uint64 HitXpGain;
    uint8 MaxTier;
    uint8 MaxTierLevel;
};

struct SpellMasteryProgress
{
    uint8 Tier = SPELL_MASTERY_TIER_IRON;
    uint8 TierLevel = 1;
    uint64 Xp = 0;
};

struct SpellMasteryEffects
{
    float DamageBonusPct = 0.0f;
    float BonusCritChancePct = 0.0f;
    float SplashDamagePct = 0.0f;
    float DuplicateChancePct = 0.0f;
    float DiamondCastTimeMultiplier = 1.0f;
    bool HasDiamondCastTime = false;
};

struct FlamestrikeMasteryEffects
{
    float DamageBonusPct = 0.0f;
    float RadiusMultiplier = 1.0f;
    float SilverExtraDamagePct = 0.0f;
    float GoldBurnDamagePct = 0.0f;
    uint8 GoldMaxStacks = 0;
    float DiamondCastTimeMultiplier = 1.0f;
    bool HasDiamondCastTime = false;
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

struct FlamestrikeBurnKey
{
    uint32 CasterGuid;
    uint32 TargetGuid;

    bool operator==(FlamestrikeBurnKey const& other) const
    {
        return CasterGuid == other.CasterGuid && TargetGuid == other.TargetGuid;
    }
};

struct FlamestrikeBurnKeyHash
{
    std::size_t operator()(FlamestrikeBurnKey const& key) const
    {
        return (std::size_t(key.CasterGuid) << 32) ^ key.TargetGuid;
    }
};

struct FlamestrikeBurnState
{
    uint8 Stacks = 0;
    uint32 ExpiresAtMs = 0;
};

std::array<ManagedSpellConfig, 2> const ManagedSpellConfigs =
{ {
    { SPELL_MAGE_FIREBALL_RANK_1, 50, SPELL_MASTERY_TIER_DIAMOND, 10 },
    { SPELL_MAGE_FLAMESTRIKE_RANK_1, 50, SPELL_MASTERY_TIER_DIAMOND, 10 }
} };

char constexpr SPELL_MASTERY_CHARACTER_TABLE[] = "character_spell_mastery";
char constexpr SPELL_MASTERY_ADDON_PREFIX[] = "SMT";
float constexpr FIREBALL_SPLASH_RADIUS = 8.0f;
int32 constexpr FIREBALL_GOLD_BURN_BASE_DURATION_MS = 6000;
int32 constexpr FIREBALL_GOLD_BURN_DURATION_EXTEND_MS = 2000;
int32 constexpr FIREBALL_GOLD_BURN_DURATION_CAP_MS = 18000;
int32 constexpr FLAMESTRIKE_GOLD_BURN_BASE_DURATION_MS = 6000;
int32 constexpr FLAMESTRIKE_GOLD_BURN_DURATION_EXTEND_MS = 1000;
int32 constexpr FLAMESTRIKE_GOLD_BURN_DURATION_CAP_MS = 14000;
uint32 constexpr FLAMESTRIKE_BURN_STATE_TTL_MS = 15000;
uint32 constexpr FLAMESTRIKE_XP_GUARD_MS = 800;

std::unordered_map<MasteryCacheKey, SpellMasteryProgress, MasteryCacheKeyHash> SpellMasteryCache;
std::unordered_map<MasteryCacheKey, uint32, MasteryCacheKeyHash> SpellMasteryLastXpGrantMs;
std::unordered_map<FlamestrikeBurnKey, FlamestrikeBurnState, FlamestrikeBurnKeyHash> FlamestrikeBurnStates;

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

    for (auto itr = FlamestrikeBurnStates.begin(); itr != FlamestrikeBurnStates.end();)
    {
        if (itr->first.CasterGuid == guid)
            itr = FlamestrikeBurnStates.erase(itr);
        else
            ++itr;
    }
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

SpellMasteryEffects BuildFireballMasteryEffects(SpellMasteryProgress const& progress, ManagedSpellConfig const& config)
{
    SpellMasteryEffects effects;

    uint8 ironLevel = GetEffectiveTierLevel(progress, SPELL_MASTERY_TIER_IRON, config);
    uint8 bronzeLevel = GetEffectiveTierLevel(progress, SPELL_MASTERY_TIER_BRONZE, config);
    uint8 silverLevel = GetEffectiveTierLevel(progress, SPELL_MASTERY_TIER_SILVER, config);
    uint8 goldLevel = GetEffectiveTierLevel(progress, SPELL_MASTERY_TIER_GOLD, config);
    uint8 diamondLevel = GetEffectiveTierLevel(progress, SPELL_MASTERY_TIER_DIAMOND, config);

    // Iron: heavy percent damage ramp so rank-1 Fireball can stay competitive.
    effects.DamageBonusPct += float(ironLevel) * 55.0f;

    // Bronze: crit chance starts at 5%, +2% each level, plus additional percent damage
    if (bronzeLevel > 0)
    {
        effects.BonusCritChancePct = 5.0f + (float(bronzeLevel - 1) * 2.0f);
        effects.DamageBonusPct += float(bronzeLevel) * 35.0f;
    }

    // Silver: stronger splash and additional direct-damage scaling.
    if (silverLevel > 0)
    {
        effects.DamageBonusPct += float(silverLevel) * 25.0f;
        effects.SplashDamagePct = 35.0f + (float(silverLevel - 1) * (45.0f / 9.0f)); // 35% -> 80%
    }

    // Gold: convert 20% to 50% of direct hit into stacking burn.
    if (goldLevel > 0)
        effects.DuplicateChancePct = 20.0f + (float(goldLevel - 1) * (30.0f / 9.0f));

    // Diamond: near-instant cast, scales further by level
    if (diamondLevel > 0)
    {
        effects.HasDiamondCastTime = true;
        effects.DiamondCastTimeMultiplier = 0.05f - (float(diamondLevel - 1) * (0.04f / 9.0f)); // 5% -> 1%
    }

    return effects;
}

FlamestrikeMasteryEffects BuildFlamestrikeMasteryEffects(SpellMasteryProgress const& progress, ManagedSpellConfig const& config)
{
    FlamestrikeMasteryEffects effects;

    uint8 ironLevel = GetEffectiveTierLevel(progress, SPELL_MASTERY_TIER_IRON, config);
    uint8 bronzeLevel = GetEffectiveTierLevel(progress, SPELL_MASTERY_TIER_BRONZE, config);
    uint8 silverLevel = GetEffectiveTierLevel(progress, SPELL_MASTERY_TIER_SILVER, config);
    uint8 goldLevel = GetEffectiveTierLevel(progress, SPELL_MASTERY_TIER_GOLD, config);
    uint8 diamondLevel = GetEffectiveTierLevel(progress, SPELL_MASTERY_TIER_DIAMOND, config);

    // Iron: percent damage to impact and periodic damage ticks.
    effects.DamageBonusPct = float(ironLevel) * 12.0f;

    // Bronze: increase AoE size.
    if (bronzeLevel > 0)
        effects.RadiusMultiplier += float(bronzeLevel) * 0.05f;

    // Silver: secondary extra damage pass.
    if (silverLevel > 0)
        effects.SilverExtraDamagePct = 8.0f + float(silverLevel - 1) * (14.0f / 9.0f); // 8% -> 22%

    // Gold: stacking burn percentage and stack cap.
    if (goldLevel > 0)
    {
        effects.GoldBurnDamagePct = 10.0f + float(goldLevel - 1) * (15.0f / 9.0f); // 10% -> 25%
        effects.GoldMaxStacks = uint8(std::min<int32>(10, 3 + goldLevel / 2)); // 3 -> 8
    }

    // Diamond: cast time reduction.
    if (diamondLevel > 0)
    {
        effects.HasDiamondCastTime = true;
        effects.DiamondCastTimeMultiplier = 0.80f - (float(diamondLevel - 1) * (0.70f / 9.0f)); // 80% -> 10%
    }

    return effects;
}

bool IsManagedRankSpell(uint32 spellId, ManagedSpellConfig const** outConfig = nullptr)
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

    for (auto const& itr : spellMap)
    {
        uint32 spellId = itr.first;
        if (spellId == config.BaseSpellId)
            continue;

        uint32 firstRank = sSpellMgr->GetFirstSpellInChain(spellId);
        if (!firstRank)
            continue;

        if (firstRank == config.BaseSpellId)
            spellsToRemove.push_back(spellId);
    }

    for (uint32 spellId : spellsToRemove)
        player->removeSpell(spellId, SPEC_MASK_ALL, false);
}

void EnforceAllManagedSpellBaseRanks(Player* player)
{
    for (ManagedSpellConfig const& config : ManagedSpellConfigs)
        EnforceManagedSpellBaseRankOnly(player, config);
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

class fireball_mastery_player_script : public PlayerScript
{
public:
    fireball_mastery_player_script() : PlayerScript("fireball_mastery_player_script", { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LEVEL_CHANGED, PLAYERHOOK_ON_LEARN_SPELL, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_DELETE })
    {
    }

    void OnPlayerLogin(Player* player) override
    {
        if (!player)
            return;

        EnforceAllManagedSpellBaseRanks(player);

        for (ManagedSpellConfig const& config : ManagedSpellConfigs)
        {
            SpellMasteryProgress const& progress = GetOrLoadSpellMasteryProgress(player, config);
            SendMasteryAddonMessageForProgress(player, config, progress);
        }
    }

    void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override
    {
        if (!player)
            return;

        EnforceAllManagedSpellBaseRanks(player);
    }

    void OnPlayerLearnSpell(Player* player, uint32 spellID) override
    {
        ManagedSpellConfig const* config = nullptr;
        if (!player || !IsManagedRankSpell(spellID, &config) || spellID == config->BaseSpellId)
            return;

        player->removeSpell(spellID, SPEC_MASK_ALL, false);
        if (player->GetSession())
        {
            SpellInfo const* baseSpellInfo = sSpellMgr->GetSpellInfo(config->BaseSpellId);
            char const* spellName = baseSpellInfo ? baseSpellInfo->SpellName[DEFAULT_LOCALE] : "This spell";
            player->GetSession()->SendAreaTriggerMessage("{} ranks are disabled by Spell Mastery. Use Rank 1.", spellName);
        }
    }

    void OnPlayerLogout(Player* player) override
    {
        if (player)
            ClearSpellMasteryRuntimeStateForPlayer(uint32(player->GetGUID().GetCounter()));
    }

    void OnPlayerDelete(ObjectGuid guid, uint32 /*accountId*/) override
    {
        ClearSpellMasteryRuntimeStateForPlayer(uint32(guid.GetCounter()));
    }
};

class spell_mage_fireball_mastery : public SpellScript
{
    PrepareSpellScript(spell_mage_fireball_mastery);

    bool Load() override
    {
        if (!GetCaster() || !GetCaster()->IsPlayer())
            return false;

        _playerCaster = GetCaster()->ToPlayer();
        _config = GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SPELL_MAGE_FIREBALL_RANK_1)
            return false;

        _progress = GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildFireballMasteryEffects(_progress, *_config);
        _isTriggeredCast = GetSpell()->IsTriggered();
        return true;
    }

    void HandleBeforeHit(SpellMissInfo /*missInfo*/)
    {
        if (_effects.BonusCritChancePct <= 0.0f)
            return;

        Unit* target = GetHitUnit();
        if (!target || !_playerCaster->IsValidAttackTarget(target))
            return;

        if (roll_chance_f(_effects.BonusCritChancePct))
            GetSpell()->SetSpellValue(SPELLVALUE_FORCED_CRIT_RESULT, 1);
    }

    void HandleDirectDamage(SpellEffIndex /*effIndex*/)
    {
        Unit* target = GetHitUnit();
        if (!target || !_playerCaster->IsValidAttackTarget(target))
            return;

        int32 hitDamage = GetHitDamage();
        if (hitDamage <= 0)
            return;

        if (_effects.DamageBonusPct > 0.0f)
        {
            int32 const scaledDamage = int32(std::lround(float(hitDamage) * (1.0f + (_effects.DamageBonusPct / 100.0f))));
            hitDamage = std::max(hitDamage, scaledDamage);
        }

        SetHitDamage(hitDamage);
        _finalDirectDamage = hitDamage;
    }

    void HandleOnHit()
    {
        Unit* target = GetHitUnit();
        if (!target || !_playerCaster->IsValidAttackTarget(target))
            return;

        if (!_isTriggeredCast)
            AddSpellMasteryXp(_playerCaster, *_config, _config->HitXpGain);

        if (_isTriggeredCast)
            return;

        TryApplySilverSplashDamage(target);
        TryApplyGoldStackingBurn(target);
    }

    void TryApplySilverSplashDamage(Unit* primaryTarget)
    {
        if (_effects.SplashDamagePct <= 0.0f || !primaryTarget)
            return;

        int32 const directDamage = std::max<int32>(GetHitDamage(), _finalDirectDamage);
        if (directDamage <= 0)
            return;

        int32 const splashDamage = int32(std::lround((float(directDamage) * _effects.SplashDamagePct) / 100.0f));
        if (splashDamage <= 0)
            return;

        std::list<Unit*> nearbyUnits;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(primaryTarget, _playerCaster, FIREBALL_SPLASH_RADIUS);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(primaryTarget, nearbyUnits, check);
        Cell::VisitObjects(primaryTarget, searcher, FIREBALL_SPLASH_RADIUS);

        for (Unit* nearbyTarget : nearbyUnits)
        {
            if (!nearbyTarget || !_playerCaster->IsValidAttackTarget(nearbyTarget))
                continue;

            SpellNonMeleeDamage splashInfo(_playerCaster, nearbyTarget, GetSpellInfo(), GetSpellInfo()->SchoolMask);
            splashInfo.damage = splashDamage;
            _playerCaster->SendSpellNonMeleeDamageLog(&splashInfo);
            _playerCaster->DealSpellDamage(&splashInfo, false);
        }
    }

    void TryApplyGoldStackingBurn(Unit* primaryTarget)
    {
        if (_effects.DuplicateChancePct <= 0.0f || !primaryTarget)
            return;

        int32 const directDamage = std::max<int32>(GetHitDamage(), _finalDirectDamage);
        if (directDamage <= 0)
            return;

        SpellInfo const* igniteInfo = sSpellMgr->GetSpellInfo(SPELL_MAGE_IGNITE);
        if (!igniteInfo || !igniteInfo->GetMaxTicks())
            return;

        int32 const burnTotal = int32(std::lround((float(directDamage) * _effects.DuplicateChancePct) / 100.0f));
        int32 const burnPerTick = std::max<int32>(1, burnTotal / int32(igniteInfo->GetMaxTicks()));

        int32 previousTickAmount = 0;
        int32 stackedPerTick = burnPerTick;
        AuraEffect* currentIgniteEffect = nullptr;
        int32 priorMaxDuration = FIREBALL_GOLD_BURN_BASE_DURATION_MS;
        int32 priorDuration = FIREBALL_GOLD_BURN_BASE_DURATION_MS;
        if (Aura* igniteAura = primaryTarget->GetAura(SPELL_MAGE_IGNITE, _playerCaster->GetGUID()))
        {
            priorMaxDuration = std::max(igniteAura->GetMaxDuration(), FIREBALL_GOLD_BURN_BASE_DURATION_MS);
            priorDuration = std::max(igniteAura->GetDuration(), FIREBALL_GOLD_BURN_BASE_DURATION_MS);
            currentIgniteEffect = igniteAura->GetEffect(EFFECT_0);
            if (currentIgniteEffect)
                previousTickAmount = std::max<int32>(0, currentIgniteEffect->GetAmount());

            stackedPerTick += previousTickAmount;
        }

        _playerCaster->CastCustomSpell(
            SPELL_MAGE_IGNITE,
            SPELLVALUE_BASE_POINT0,
            stackedPerTick,
            primaryTarget,
            TriggerCastFlags(TRIGGERED_FULL_MASK & ~TRIGGERED_NO_PERIODIC_RESET),
            nullptr,
            currentIgniteEffect,
            _playerCaster->GetGUID());

        int32 appliedTickAmount = 0;
        int32 appliedDuration = 0;
        int32 appliedMaxDuration = 0;

        if (Aura* refreshedIgniteAura = primaryTarget->GetAura(SPELL_MAGE_IGNITE, _playerCaster->GetGUID()))
        {
            int32 const nextMaxDuration = std::min(
                FIREBALL_GOLD_BURN_DURATION_CAP_MS,
                priorMaxDuration + FIREBALL_GOLD_BURN_DURATION_EXTEND_MS);
            int32 const nextDuration = std::min(
                nextMaxDuration,
                priorDuration + FIREBALL_GOLD_BURN_DURATION_EXTEND_MS);
            refreshedIgniteAura->SetMaxDuration(nextMaxDuration);
            refreshedIgniteAura->SetDuration(nextDuration);

            appliedDuration = refreshedIgniteAura->GetDuration();
            appliedMaxDuration = refreshedIgniteAura->GetMaxDuration();
            if (AuraEffect* refreshedEffect = refreshedIgniteAura->GetEffect(EFFECT_0))
                appliedTickAmount = refreshedEffect->GetAmount();
        }

        if (_playerCaster->GetSession())
        {
            ChatHandler(_playerCaster->GetSession()).PSendSysMessage(
                "[SM DBG] GoldBurn direct={} pct={:.1f} prev={} add={} new={} applied={} dur={}/{}",
                directDamage,
                _effects.DuplicateChancePct,
                previousTickAmount,
                burnPerTick,
                stackedPerTick,
                appliedTickAmount,
                appliedDuration,
                appliedMaxDuration);
        }
    }

    void Register() override
    {
        BeforeHit += BeforeSpellHitFn(spell_mage_fireball_mastery::HandleBeforeHit);
        OnEffectHitTarget += SpellEffectFn(spell_mage_fireball_mastery::HandleDirectDamage, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
        AfterHit += SpellHitFn(spell_mage_fireball_mastery::HandleOnHit);
    }

private:
    Player* _playerCaster = nullptr;
    ManagedSpellConfig const* _config = nullptr;
    SpellMasteryProgress _progress;
    SpellMasteryEffects _effects;
    bool _isTriggeredCast = false;
    int32 _finalDirectDamage = 0;
};

class spell_mage_flamestrike_mastery : public SpellScript
{
    PrepareSpellScript(spell_mage_flamestrike_mastery);

    bool Load() override
    {
        if (!GetCaster() || !GetCaster()->IsPlayer())
            return false;

        _playerCaster = GetCaster()->ToPlayer();
        _config = GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SPELL_MAGE_FLAMESTRIKE_RANK_1)
            return false;

        _progress = GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildFlamestrikeMasteryEffects(_progress, *_config);
        _isTriggeredCast = GetSpell()->IsTriggered();

        if (_effects.RadiusMultiplier > 1.0f)
            GetSpell()->SetSpellValue(SPELLVALUE_RADIUS_MOD, int32(std::lround(_effects.RadiusMultiplier * 10000.0f)));

        return true;
    }

    void HandleDirectDamage(SpellEffIndex /*effIndex*/)
    {
        Unit* target = GetHitUnit();
        if (!target || !_playerCaster->IsValidAttackTarget(target))
            return;

        int32 hitDamage = GetHitDamage();
        if (hitDamage <= 0)
            return;

        if (_effects.DamageBonusPct > 0.0f)
        {
            int32 const scaledDamage = int32(std::lround(float(hitDamage) * (1.0f + (_effects.DamageBonusPct / 100.0f))));
            hitDamage = std::max(hitDamage, scaledDamage);
        }

        SetHitDamage(hitDamage);
        _finalHitDamage = hitDamage;
    }

    void HandleOnHit()
    {
        Unit* target = GetHitUnit();
        if (!target || !_playerCaster->IsValidAttackTarget(target))
            return;

        if (!_xpAwarded && !_isTriggeredCast && ShouldAwardSpellMasteryXp(_playerCaster, *_config, FLAMESTRIKE_XP_GUARD_MS))
        {
            AddSpellMasteryXp(_playerCaster, *_config, _config->HitXpGain);
            _xpAwarded = true;
        }

        TryApplySilverExtraDamage(target);
        TryApplyGoldStackingBurn(target);
    }

    void TryApplySilverExtraDamage(Unit* target)
    {
        if (_effects.SilverExtraDamagePct <= 0.0f || !target || _isApplyingSilverExtra)
            return;

        int32 const baseDamage = std::max<int32>(GetHitDamage(), _finalHitDamage);
        if (baseDamage <= 0)
            return;

        int32 const extraDamage = int32(std::lround((float(baseDamage) * _effects.SilverExtraDamagePct) / 100.0f));
        if (extraDamage <= 0)
            return;

        _isApplyingSilverExtra = true;
        SpellNonMeleeDamage extraInfo(_playerCaster, target, GetSpellInfo(), GetSpellInfo()->SchoolMask);
        extraInfo.damage = extraDamage;
        _playerCaster->SendSpellNonMeleeDamageLog(&extraInfo);
        _playerCaster->DealSpellDamage(&extraInfo, false);
        _isApplyingSilverExtra = false;
    }

    void TryApplyGoldStackingBurn(Unit* target)
    {
        if (_effects.GoldBurnDamagePct <= 0.0f || _effects.GoldMaxStacks == 0 || !target)
            return;

        SpellInfo const* burnInfo = sSpellMgr->GetSpellInfo(SPELL_MAGE_IGNITE);
        if (!burnInfo || !burnInfo->GetMaxTicks())
            return;

        int32 const baseDamage = std::max<int32>(GetHitDamage(), _finalHitDamage);
        if (baseDamage <= 0)
            return;

        uint32 const casterGuid = uint32(_playerCaster->GetGUID().GetCounter());
        uint32 const targetGuid = uint32(target->GetGUID().GetCounter());
        uint32 const nowMs = uint32(GameTime::GetGameTimeMS().count());

        FlamestrikeBurnState& burnState = FlamestrikeBurnStates[{ casterGuid, targetGuid }];
        if (burnState.ExpiresAtMs <= nowMs)
            burnState.Stacks = 0;

        burnState.Stacks = std::min<uint8>(_effects.GoldMaxStacks, uint8(burnState.Stacks + 1));
        burnState.ExpiresAtMs = nowMs + FLAMESTRIKE_BURN_STATE_TTL_MS;

        int32 const burnTotal = int32(std::lround((float(baseDamage) * _effects.GoldBurnDamagePct) / 100.0f));
        int32 const perTick = std::max<int32>(1, burnTotal / int32(burnInfo->GetMaxTicks()));
        int32 const stackedPerTick = std::max<int32>(1, perTick * burnState.Stacks);

        _playerCaster->CastCustomSpell(
            SPELL_MAGE_IGNITE,
            SPELLVALUE_BASE_POINT0,
            stackedPerTick,
            target,
            TriggerCastFlags(TRIGGERED_FULL_MASK & ~TRIGGERED_NO_PERIODIC_RESET),
            nullptr,
            nullptr,
            _playerCaster->GetGUID());

        if (Aura* burnAura = target->GetAura(SPELL_MAGE_IGNITE, _playerCaster->GetGUID()))
        {
            int32 const nextMaxDuration = std::min(
                FLAMESTRIKE_GOLD_BURN_DURATION_CAP_MS,
                std::max(burnAura->GetMaxDuration(), FLAMESTRIKE_GOLD_BURN_BASE_DURATION_MS) + FLAMESTRIKE_GOLD_BURN_DURATION_EXTEND_MS);
            int32 const nextDuration = std::min(
                nextMaxDuration,
                std::max(burnAura->GetDuration(), FLAMESTRIKE_GOLD_BURN_BASE_DURATION_MS) + FLAMESTRIKE_GOLD_BURN_DURATION_EXTEND_MS);
            burnAura->SetMaxDuration(nextMaxDuration);
            burnAura->SetDuration(nextDuration);
        }
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_mage_flamestrike_mastery::HandleDirectDamage, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
        OnEffectHitTarget += SpellEffectFn(spell_mage_flamestrike_mastery::HandleDirectDamage, EFFECT_1, SPELL_EFFECT_SCHOOL_DAMAGE);
        OnEffectHitTarget += SpellEffectFn(spell_mage_flamestrike_mastery::HandleDirectDamage, EFFECT_2, SPELL_EFFECT_SCHOOL_DAMAGE);
        AfterHit += SpellHitFn(spell_mage_flamestrike_mastery::HandleOnHit);
    }

private:
    Player* _playerCaster = nullptr;
    ManagedSpellConfig const* _config = nullptr;
    SpellMasteryProgress _progress;
    FlamestrikeMasteryEffects _effects;
    bool _isTriggeredCast = false;
    bool _xpAwarded = false;
    bool _isApplyingSilverExtra = false;
    int32 _finalHitDamage = 0;
};

class spell_mastery_prepare_all_spell_script : public AllSpellScript
{
public:
    spell_mastery_prepare_all_spell_script() : AllSpellScript("spell_mastery_prepare_all_spell_script", { ALLSPELLHOOK_ON_PREPARE })
    {
    }

    void OnSpellPrepare(Spell* spell, Unit* caster, SpellInfo const* spellInfo) override
    {
        if (!spell || !caster || !caster->IsPlayer() || !spellInfo)
            return;

        ManagedSpellConfig const* config = GetManagedSpellConfigForSpell(spellInfo->Id);
        if (!config)
            return;

        Player* player = caster->ToPlayer();
        SpellMasteryProgress const& progress = GetOrLoadSpellMasteryProgress(player, *config);

        if (config->BaseSpellId == SPELL_MAGE_FIREBALL_RANK_1)
        {
            SpellMasteryEffects const effects = BuildFireballMasteryEffects(progress, *config);
            if (!effects.HasDiamondCastTime)
                return;

            int32 const currentCastTime = spell->GetCastTime();
            if (currentCastTime <= 0)
                return;

            int32 const reducedCastTime = std::max<int32>(1, int32(float(currentCastTime) * effects.DiamondCastTimeMultiplier));
            if (reducedCastTime < currentCastTime)
                spell->SetSpellMasteryCastTime(reducedCastTime);
            return;
        }

        if (config->BaseSpellId == SPELL_MAGE_FLAMESTRIKE_RANK_1)
        {
            FlamestrikeMasteryEffects const effects = BuildFlamestrikeMasteryEffects(progress, *config);
            if (effects.RadiusMultiplier > 1.0f)
                spell->SetSpellValue(SPELLVALUE_RADIUS_MOD, int32(std::lround(effects.RadiusMultiplier * 10000.0f)));

            if (!effects.HasDiamondCastTime)
                return;

            int32 const currentCastTime = spell->GetCastTime();
            if (currentCastTime <= 0)
                return;

            int32 const reducedCastTime = std::max<int32>(1, int32(float(currentCastTime) * effects.DiamondCastTimeMultiplier));
            if (reducedCastTime < currentCastTime)
                spell->SetSpellMasteryCastTime(reducedCastTime);
        }
    }
};

void AddSC_spell_mastery_fireball()
{
    new fireball_mastery_player_script();
    new spell_mastery_prepare_all_spell_script();
    RegisterSpellScript(spell_mage_fireball_mastery);
    RegisterSpellScript(spell_mage_flamestrike_mastery);
}
