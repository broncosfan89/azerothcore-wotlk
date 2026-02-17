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
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Player.h"
#include "PlayerScript.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "World.h"
#include "WorldSession.h"
#include <array>
#include <cstdint>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace
{
enum SpellMasteryConstants : uint32
{
    SPELL_MAGE_FIREBALL_RANK_1 = 133
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
    int32 FlatDamageBonus = 0;
    float BonusCritChancePct = 0.0f;
    float SplashDamagePct = 0.0f;
    float DuplicateChancePct = 0.0f;
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

std::array<ManagedSpellConfig, 1> const ManagedSpellConfigs =
{ {
    { SPELL_MAGE_FIREBALL_RANK_1, 50, SPELL_MASTERY_TIER_DIAMOND, 10 }
} };

char constexpr SPELL_MASTERY_CHARACTER_TABLE[] = "character_spell_mastery";
char constexpr SPELL_MASTERY_ADDON_PREFIX[] = "SMT";
float constexpr FIREBALL_SPLASH_RADIUS = 8.0f;
float constexpr FIREBALL_DUPLICATE_SCAN_RADIUS = 45.0f;
uint32 constexpr FIREBALL_DUPLICATE_TARGET_CAP = 24;

std::unordered_map<MasteryCacheKey, SpellMasteryProgress, MasteryCacheKeyHash> SpellMasteryCache;

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

SpellMasteryProgress& GetOrLoadSpellMasteryProgress(Player* player, ManagedSpellConfig const& config)
{
    MasteryCacheKey key =
    {
        uint32(player->GetGUID().GetCounter()),
        config.BaseSpellId
    };

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

    // Iron: flat damage by level
    effects.FlatDamageBonus += int32(ironLevel) * 2;

    // Bronze: crit chance starts at 5%, +1% each level, plus additional flat damage
    if (bronzeLevel > 0)
    {
        effects.BonusCritChancePct = 5.0f + float(bronzeLevel - 1);
        effects.FlatDamageBonus += int32(bronzeLevel);
    }

    // Silver: splash 10% to 30%
    if (silverLevel > 0)
        effects.SplashDamagePct = 10.0f + (float(silverLevel - 1) * (20.0f / 9.0f));

    // Gold: duplicate chance 5% to 20%
    if (goldLevel > 0)
        effects.DuplicateChancePct = 5.0f + (float(goldLevel - 1) * (15.0f / 9.0f));

    // Diamond: near-instant cast, scales further by level
    if (diamondLevel > 0)
    {
        effects.HasDiamondCastTime = true;
        effects.DiamondCastTimeMultiplier = 0.05f - (float(diamondLevel - 1) * (0.04f / 9.0f)); // 5% -> 1%
    }

    return effects;
}

bool IsManagedFireballRankSpell(uint32 spellId)
{
    ManagedSpellConfig const* config = GetManagedSpellConfigForSpell(spellId);
    return config && config->BaseSpellId == SPELL_MAGE_FIREBALL_RANK_1;
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
        player->GetSession()->SendAreaTriggerMessage("Fireball Mastery advanced to {} Tier Level {}.", GetTierName(progress.Tier), progress.TierLevel);
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
        if (!player || !IsManagedFireballRankSpell(spellID) || spellID == SPELL_MAGE_FIREBALL_RANK_1)
            return;

        player->removeSpell(spellID, SPEC_MASK_ALL, false);
        if (player->GetSession())
            player->GetSession()->SendAreaTriggerMessage("Fireball ranks are disabled by Spell Mastery. Use Rank 1 Fireball.");
    }

    void OnPlayerLogout(Player* player) override
    {
        if (player)
            ClearSpellMasteryCacheForPlayer(uint32(player->GetGUID().GetCounter()));
    }

    void OnPlayerDelete(ObjectGuid guid, uint32 /*accountId*/) override
    {
        ClearSpellMasteryCacheForPlayer(uint32(guid.GetCounter()));
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

        SetHitDamage(GetHitDamage() + _effects.FlatDamageBonus);
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
        TryApplyGoldDuplicateCast(target);
    }

    void TryApplySilverSplashDamage(Unit* primaryTarget)
    {
        if (_effects.SplashDamagePct <= 0.0f || !primaryTarget)
            return;

        int32 const splashDamage = int32((float(GetHitDamage()) * _effects.SplashDamagePct) / 100.0f);
        if (splashDamage <= 0)
            return;

        std::list<Unit*> nearbyUnits;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(_playerCaster, primaryTarget, FIREBALL_SPLASH_RADIUS);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(primaryTarget, nearbyUnits, check);
        Cell::VisitObjects(primaryTarget, searcher, FIREBALL_SPLASH_RADIUS);

        for (Unit* nearbyTarget : nearbyUnits)
        {
            if (!nearbyTarget || nearbyTarget == primaryTarget || !_playerCaster->IsValidAttackTarget(nearbyTarget))
                continue;

            Unit::DealDamage(_playerCaster, nearbyTarget, splashDamage, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_FIRE);
        }
    }

    void TryApplyGoldDuplicateCast(Unit* primaryTarget)
    {
        if (_effects.DuplicateChancePct <= 0.0f)
            return;

        if (!roll_chance_f(_effects.DuplicateChancePct))
            return;

        std::list<Unit*> nearbyUnits;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(_playerCaster, _playerCaster, FIREBALL_DUPLICATE_SCAN_RADIUS);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(_playerCaster, nearbyUnits, check);
        Cell::VisitObjects(_playerCaster, searcher, FIREBALL_DUPLICATE_SCAN_RADIUS);

        uint32 castsDone = 0;
        for (Unit* nearbyTarget : nearbyUnits)
        {
            if (castsDone >= FIREBALL_DUPLICATE_TARGET_CAP)
                break;

            if (!nearbyTarget || nearbyTarget == primaryTarget || !_playerCaster->IsValidAttackTarget(nearbyTarget))
                continue;

            if (!_playerCaster->IsInCombatWith(nearbyTarget))
                continue;

            _playerCaster->CastSpell(nearbyTarget, _config->BaseSpellId, TRIGGERED_FULL_MASK);
            ++castsDone;
        }
    }

    void Register() override
    {
        BeforeHit += BeforeSpellHitFn(spell_mage_fireball_mastery::HandleBeforeHit);
        OnEffectHitTarget += SpellEffectFn(spell_mage_fireball_mastery::HandleDirectDamage, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
        OnHit += SpellHitFn(spell_mage_fireball_mastery::HandleOnHit);
    }

private:
    Player* _playerCaster = nullptr;
    ManagedSpellConfig const* _config = nullptr;
    SpellMasteryProgress _progress;
    SpellMasteryEffects _effects;
    bool _isTriggeredCast = false;
};

class fireball_mastery_prepare_all_spell_script : public AllSpellScript
{
public:
    fireball_mastery_prepare_all_spell_script() : AllSpellScript("fireball_mastery_prepare_all_spell_script", { ALLSPELLHOOK_ON_PREPARE })
    {
    }

    void OnSpellPrepare(Spell* spell, Unit* caster, SpellInfo const* spellInfo) override
    {
        if (!spell || !caster || !caster->IsPlayer() || !spellInfo)
            return;

        ManagedSpellConfig const* config = GetManagedSpellConfigForSpell(spellInfo->Id);
        if (!config || config->BaseSpellId != SPELL_MAGE_FIREBALL_RANK_1)
            return;

        Player* player = caster->ToPlayer();
        SpellMasteryProgress const& progress = GetOrLoadSpellMasteryProgress(player, *config);
        SpellMasteryEffects const effects = BuildFireballMasteryEffects(progress, *config);
        if (!effects.HasDiamondCastTime)
            return;

        int32 const currentCastTime = spell->GetCastTime();
        if (currentCastTime <= 0)
            return;

        int32 const reducedCastTime = std::max<int32>(1, int32(float(currentCastTime) * effects.DiamondCastTimeMultiplier));
        if (reducedCastTime < currentCastTime)
            spell->SetSpellMasteryCastTime(reducedCastTime);
    }
};

void AddSC_spell_mastery_fireball()
{
    new fireball_mastery_player_script();
    new fireball_mastery_prepare_all_spell_script();
    RegisterSpellScript(spell_mage_fireball_mastery);
}
