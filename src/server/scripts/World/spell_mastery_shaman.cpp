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

#include "AllSpellScript.h"
#include "Cell.h"
#include "CellImpl.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Player.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace
{
struct ChainLightningMasteryEffects
{
    float IronDamageBonusPct = 0.0f;
    float BronzeJumpReductionPct = 30.0f;
    uint8 SilverExtraTargets = 0;
    uint8 GoldMaxStacks = 0;
    float GoldNatureTakenPctPerStack = 0.0f;
    bool HasDiamondInstantReset = false;
};

struct LavaBurstMasteryEffects
{
    int32 IronCooldownReductionMs = 0;
    float BronzeDamageBonusPct = 0.0f;
    float SilverFireDamageTakenPct = 0.0f;
    uint8 GoldSpreadFlameShockTargets = 0;
    bool HasDiamondFlameShockBurst = false;
};

struct ChainLightningGoldDebuffKey
{
    uint32 CasterGuid;
    uint32 TargetGuid;

    bool operator==(ChainLightningGoldDebuffKey const& other) const
    {
        return CasterGuid == other.CasterGuid && TargetGuid == other.TargetGuid;
    }
};

struct ChainLightningGoldDebuffKeyHash
{
    std::size_t operator()(ChainLightningGoldDebuffKey const& key) const
    {
        return (std::size_t(key.CasterGuid) << 32) ^ key.TargetGuid;
    }
};

struct ChainLightningGoldDebuffState
{
    uint8 Stacks = 0;
    uint32 ExpiresAtMs = 0;
};

struct LavaBurstSilverDebuffKey
{
    uint32 CasterGuid;
    uint32 TargetGuid;

    bool operator==(LavaBurstSilverDebuffKey const& other) const
    {
        return CasterGuid == other.CasterGuid && TargetGuid == other.TargetGuid;
    }
};

struct LavaBurstSilverDebuffKeyHash
{
    std::size_t operator()(LavaBurstSilverDebuffKey const& key) const
    {
        return (std::size_t(key.CasterGuid) << 32) ^ key.TargetGuid;
    }
};

struct LavaBurstSilverDebuffState
{
    float FireDamageTakenPct = 0.0f;
    uint32 ExpiresAtMs = 0;
};

uint32 constexpr CHAIN_LIGHTNING_XP_GUARD_MS = 250;
uint32 constexpr LAVA_BURST_XP_GUARD_MS = 250;
uint32 constexpr CHAIN_LIGHTNING_GOLD_DEBUFF_TTL_MS = 15000;
uint32 constexpr LAVA_BURST_SILVER_DEBUFF_TTL_MS = 15000;
int32 constexpr LAVA_BURST_MAX_COOLDOWN_REDUCTION_MS = 6000;
float constexpr LAVA_BURST_GOLD_SPREAD_RADIUS = 20.0f;
float constexpr LAVA_BURST_DIAMOND_SEARCH_RADIUS = 40.0f;
float constexpr CHAIN_LIGHTNING_SILVER_EXTRA_TARGET_RADIUS = 12.5f;
uint32 constexpr CHAIN_LIGHTNING_BASE_TOTAL_TARGETS = 3;
uint32 constexpr CHAIN_LIGHTNING_MAX_TOTAL_TARGETS = 12;
float constexpr CHAIN_LIGHTNING_BASE_JUMP_MULTIPLIER = 0.70f;

std::unordered_map<ChainLightningGoldDebuffKey, ChainLightningGoldDebuffState, ChainLightningGoldDebuffKeyHash> ChainLightningGoldDebuffStates;
std::unordered_map<LavaBurstSilverDebuffKey, LavaBurstSilverDebuffState, LavaBurstSilverDebuffKeyHash> LavaBurstSilverDebuffStates;
std::mutex SpellMasteryShamanStateMutex;

ChainLightningMasteryEffects BuildChainLightningMasteryEffects(SpellMastery::SpellMasteryProgress const& progress, SpellMastery::ManagedSpellConfig const& config)
{
    ChainLightningMasteryEffects effects;

    uint8 ironLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_IRON, config);
    uint8 bronzeLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_BRONZE, config);
    uint8 silverLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_SILVER, config);
    uint8 goldLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_GOLD, config);
    uint8 diamondLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_DIAMOND, config);
    uint32 const totalMasteryLevels = uint32(ironLevel) + uint32(bronzeLevel) + uint32(silverLevel) + uint32(goldLevel) + uint32(diamondLevel);

    if (totalMasteryLevels > 0)
        effects.IronDamageBonusPct = float(totalMasteryLevels) * 4.0f;

    if (bronzeLevel > 0)
        effects.BronzeJumpReductionPct = std::max(0.0f, 30.0f * (1.0f - (float(bronzeLevel) / 10.0f)));

    if (silverLevel > 0)
        effects.SilverExtraTargets = silverLevel;

    if (goldLevel > 0)
    {
        effects.GoldMaxStacks = goldLevel;
        effects.GoldNatureTakenPctPerStack = 2.0f + (float(goldLevel - 1) * (3.0f / 9.0f));
    }

    effects.HasDiamondInstantReset = diamondLevel > 0;
    return effects;
}

LavaBurstMasteryEffects BuildLavaBurstMasteryEffects(SpellMastery::SpellMasteryProgress const& progress, SpellMastery::ManagedSpellConfig const& config)
{
    LavaBurstMasteryEffects effects;

    uint8 const ironLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_IRON, config);
    uint8 const bronzeLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_BRONZE, config);
    uint8 const silverLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_SILVER, config);
    uint8 const goldLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_GOLD, config);
    uint8 const diamondLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_DIAMOND, config);
    uint32 const totalMasteryLevels = uint32(ironLevel) + uint32(bronzeLevel) + uint32(silverLevel) + uint32(goldLevel) + uint32(diamondLevel);

    if (ironLevel > 0)
        effects.IronCooldownReductionMs = int32(std::lround(float(LAVA_BURST_MAX_COOLDOWN_REDUCTION_MS) * (float(ironLevel) / 10.0f)));

    if (totalMasteryLevels > 0)
        effects.BronzeDamageBonusPct = float(totalMasteryLevels) * 1.0f;

    if (silverLevel > 0)
        effects.SilverFireDamageTakenPct = float(silverLevel) * 2.5f;

    if (goldLevel > 0)
        effects.GoldSpreadFlameShockTargets = goldLevel;

    effects.HasDiamondFlameShockBurst = diamondLevel > 0;
    return effects;
}

uint32 GetHighestKnownSpellInChain(Player* player, uint32 firstRankSpellId)
{
    if (!player || !firstRankSpellId)
        return 0;

    uint32 highestKnown = 0;
    for (uint32 spellId = firstRankSpellId; spellId; spellId = sSpellMgr->GetNextSpellInChain(spellId))
        if (player->HasSpell(spellId))
            highestKnown = spellId;

    return highestKnown ? highestKnown : firstRankSpellId;
}

bool HasFlameShockFromCaster(Unit* target, ObjectGuid casterGuid)
{
    if (!target)
        return false;

    for (uint32 spellId = SpellMastery::SPELL_SHAMAN_FLAME_SHOCK_RANK_1; spellId; spellId = sSpellMgr->GetNextSpellInChain(spellId))
        if (target->GetAura(spellId, casterGuid))
            return true;

    return false;
}

void ApplyOrRefreshLavaBurstSilverDebuff(Player* caster, Unit* target, float fireDamageTakenPct)
{
    if (!caster || !target || fireDamageTakenPct <= 0.0f)
        return;

    LavaBurstSilverDebuffKey const key
    {
        uint32(caster->GetGUID().GetCounter()),
        uint32(target->GetGUID().GetCounter())
    };

    uint32 const nowMs = uint32(GameTime::GetGameTimeMS().count());
    std::lock_guard<std::mutex> lock(SpellMasteryShamanStateMutex);
    LavaBurstSilverDebuffState& state = LavaBurstSilverDebuffStates[key];
    state.FireDamageTakenPct = std::max(state.FireDamageTakenPct, fireDamageTakenPct);
    state.ExpiresAtMs = nowMs + LAVA_BURST_SILVER_DEBUFF_TTL_MS;
}

float GetLavaBurstSilverDebuffPct(Player* caster, Unit* target)
{
    if (!caster || !target)
        return 0.0f;

    LavaBurstSilverDebuffKey const key
    {
        uint32(caster->GetGUID().GetCounter()),
        uint32(target->GetGUID().GetCounter())
    };

    std::lock_guard<std::mutex> lock(SpellMasteryShamanStateMutex);
    auto itr = LavaBurstSilverDebuffStates.find(key);
    if (itr == LavaBurstSilverDebuffStates.end())
        return 0.0f;

    uint32 const nowMs = uint32(GameTime::GetGameTimeMS().count());
    if (itr->second.ExpiresAtMs <= nowMs)
    {
        LavaBurstSilverDebuffStates.erase(itr);
        return 0.0f;
    }

    return itr->second.FireDamageTakenPct;
}

bool IsDamageEffect(SpellEffectInfo const& effectInfo)
{
    if (!effectInfo.IsEffect())
        return false;

    switch (effectInfo.Effect)
    {
        case SPELL_EFFECT_SCHOOL_DAMAGE:
        case SPELL_EFFECT_WEAPON_DAMAGE:
        case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
        case SPELL_EFFECT_HEALTH_LEECH:
        case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
        case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
            return true;
        default:
            break;
    }

    return effectInfo.IsAura(SPELL_AURA_PERIODIC_DAMAGE) || effectInfo.IsAura(SPELL_AURA_PERIODIC_LEECH);
}

void ApplySpellDamagePercentModifier(Spell* spell, Unit* caster, SpellInfo const* spellInfo, float pct)
{
    if (!spell || !caster || !spellInfo || std::fabs(pct) < 0.01f)
        return;

    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        SpellEffectInfo const& effectInfo = spellInfo->Effects[i];
        if (!IsDamageEffect(effectInfo))
            continue;

        int32 const baseValue = effectInfo.CalcValue(caster);
        if (!baseValue)
            continue;

        float const scaled = float(baseValue) * (1.0f + (pct / 100.0f));
        int32 scaledValue = int32(std::lround(scaled));

        if (baseValue > 0)
            scaledValue = std::max<int32>(1, scaledValue);
        else if (baseValue < 0)
            scaledValue = std::min<int32>(-1, scaledValue);

        spell->SetSpellValue(SpellValueMod(SPELLVALUE_BASE_POINT0 + i), scaledValue);
    }
}

uint8 GetActiveGoldDebuffStacks(Player* caster, Unit* target)
{
    if (!caster || !target)
        return 0;

    ChainLightningGoldDebuffKey const key
    {
        uint32(caster->GetGUID().GetCounter()),
        uint32(target->GetGUID().GetCounter())
    };

    std::lock_guard<std::mutex> lock(SpellMasteryShamanStateMutex);
    auto itr = ChainLightningGoldDebuffStates.find(key);
    if (itr == ChainLightningGoldDebuffStates.end())
        return 0;

    uint32 const nowMs = uint32(GameTime::GetGameTimeMS().count());
    if (itr->second.ExpiresAtMs <= nowMs)
    {
        ChainLightningGoldDebuffStates.erase(itr);
        return 0;
    }

    return std::max<uint8>(1, itr->second.Stacks);
}
}

void ClearSpellMasteryShamanRuntimeStateForPlayer(uint32 guid)
{
    std::lock_guard<std::mutex> lock(SpellMasteryShamanStateMutex);
    for (auto itr = ChainLightningGoldDebuffStates.begin(); itr != ChainLightningGoldDebuffStates.end();)
    {
        if (itr->first.CasterGuid == guid || itr->first.TargetGuid == guid)
            itr = ChainLightningGoldDebuffStates.erase(itr);
        else
            ++itr;
    }

    for (auto itr = LavaBurstSilverDebuffStates.begin(); itr != LavaBurstSilverDebuffStates.end();)
    {
        if (itr->first.CasterGuid == guid || itr->first.TargetGuid == guid)
            itr = LavaBurstSilverDebuffStates.erase(itr);
        else
            ++itr;
    }
}

class spell_sha_chain_lightning_mastery : public SpellScript
{
    PrepareSpellScript(spell_sha_chain_lightning_mastery);

    bool Load() override
    {
        if (!GetCaster() || !GetCaster()->IsPlayer())
            return false;

        _playerCaster = GetCaster()->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SpellMastery::SPELL_SHAMAN_CHAIN_LIGHTNING_RANK_1)
            return false;

        _progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildChainLightningMasteryEffects(_progress, *_config);
        _isTriggeredCast = GetSpell()->IsTriggered();

        if (_effects.SilverExtraTargets > 0)
        {
            uint32 const totalTargets = std::min<uint32>(CHAIN_LIGHTNING_MAX_TOTAL_TARGETS, CHAIN_LIGHTNING_BASE_TOTAL_TARGETS + _effects.SilverExtraTargets);
            GetSpell()->SetSpellValue(SPELLVALUE_MAX_TARGETS, int32(totalTargets));
        }

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

        hitDamage = SpellMastery::ApplyEarlyAccessSpellScale(_playerCaster, GetSpellInfo(), hitDamage);

        uint8 const jumpIndex = _processedTargets++;

        if (jumpIndex > 0 && _effects.BronzeJumpReductionPct < 30.0f)
        {
            float const defaultDrop = std::pow(CHAIN_LIGHTNING_BASE_JUMP_MULTIPLIER, float(jumpIndex));
            if (defaultDrop > 0.0001f)
            {
                float const estimatedBaseDamage = float(hitDamage) / defaultDrop;
                float const customMultiplier = 1.0f - (_effects.BronzeJumpReductionPct / 100.0f);
                float const adjustedDamage = estimatedBaseDamage * std::pow(customMultiplier, float(jumpIndex));
                hitDamage = std::max<int32>(hitDamage, int32(std::lround(adjustedDamage)));
            }
        }

        float totalBonusPct = _effects.IronDamageBonusPct;
        if (_effects.GoldNatureTakenPctPerStack > 0.0f)
        {
            uint8 const stacks = GetActiveGoldDebuffStacks(_playerCaster, target);
            if (stacks > 0)
                totalBonusPct += float(stacks) * _effects.GoldNatureTakenPctPerStack;
        }

        if (totalBonusPct > 0.0f)
        {
            int32 const scaledDamage = int32(std::lround(float(hitDamage) * (1.0f + (totalBonusPct / 100.0f))));
            hitDamage = std::max(hitDamage, scaledDamage);
        }

        SetHitDamage(hitDamage);

        if (!_xpAwarded && !_isTriggeredCast && SpellMastery::ShouldAwardSpellMasteryXp(_playerCaster, *_config, CHAIN_LIGHTNING_XP_GUARD_MS))
        {
            SpellMastery::AddSpellMasteryXp(_playerCaster, *_config, SpellMastery::SPELL_MASTERY_XP_PER_HIT);
            SpellMastery::SpellMasteryProgress const& progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
            SpellMastery::SendMasteryAddonMessageForProgress(_playerCaster, *_config, progress);
            _xpAwarded = true;
        }
    }

    void HandleAfterHit()
    {
        Unit* target = GetHitUnit();
        if (!target)
            return;

        if (!_playerCaster->IsValidAttackTarget(target))
            return;

        ApplyOrRefreshGoldDebuff(target);

        if (_effects.HasDiamondInstantReset && !_isTriggeredCast && !_diamondTriggered)
        {
            _diamondTriggered = true;
            _playerCaster->CastSpell(
                target,
                _config->AllowedSpellId,
                TriggerCastFlags(TRIGGERED_IGNORE_GCD | TRIGGERED_IGNORE_CAST_IN_PROGRESS | TRIGGERED_IGNORE_POWER_AND_REAGENT_COST | TRIGGERED_IGNORE_SPELL_AND_CATEGORY_CD | TRIGGERED_CAST_DIRECTLY));
        }
    }

    void ApplyOrRefreshGoldDebuff(Unit* target)
    {
        if (!_effects.GoldMaxStacks || _effects.GoldNatureTakenPctPerStack <= 0.0f || !target)
            return;

        ChainLightningGoldDebuffKey const key
        {
            uint32(_playerCaster->GetGUID().GetCounter()),
            uint32(target->GetGUID().GetCounter())
        };

        uint32 const nowMs = uint32(GameTime::GetGameTimeMS().count());
        std::lock_guard<std::mutex> lock(SpellMasteryShamanStateMutex);
        ChainLightningGoldDebuffState& state = ChainLightningGoldDebuffStates[key];
        if (state.ExpiresAtMs <= nowMs)
            state.Stacks = 0;

        state.Stacks = std::min<uint8>(_effects.GoldMaxStacks, uint8(state.Stacks + 1));
        state.ExpiresAtMs = nowMs + CHAIN_LIGHTNING_GOLD_DEBUFF_TTL_MS;
    }

    void ExpandSilverChainTargets(std::list<WorldObject*>& targets)
    {
        if (_effects.SilverExtraTargets == 0 || !_playerCaster)
            return;

        Unit* primaryTarget = GetExplTargetUnit();
        if (!primaryTarget)
            return;

        uint32 const totalTargets = std::min<uint32>(CHAIN_LIGHTNING_MAX_TOTAL_TARGETS, CHAIN_LIGHTNING_BASE_TOTAL_TARGETS + _effects.SilverExtraTargets);
        if (totalTargets <= 1)
            return;

        uint32 const desiredAdditionalTargets = totalTargets - 1;
        if (targets.size() >= desiredAdditionalTargets)
            return;

        std::list<Unit*> nearbyUnits;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(primaryTarget, _playerCaster, CHAIN_LIGHTNING_SILVER_EXTRA_TARGET_RADIUS);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(primaryTarget, nearbyUnits, check);
        Cell::VisitObjects(primaryTarget, searcher, CHAIN_LIGHTNING_SILVER_EXTRA_TARGET_RADIUS);

        for (Unit* candidate : nearbyUnits)
        {
            if (!candidate || candidate == primaryTarget || !_playerCaster->IsValidAttackTarget(candidate))
                continue;

            if (!primaryTarget->IsWithinLOSInMap(candidate, VMAP::ModelIgnoreFlags::M2))
                continue;

            bool alreadySelected = false;
            for (WorldObject* existing : targets)
            {
                if (existing && existing->GetGUID() == candidate->GetGUID())
                {
                    alreadySelected = true;
                    break;
                }
            }

            if (alreadySelected)
                continue;

            targets.push_back(candidate);
            if (targets.size() >= desiredAdditionalTargets)
                break;
        }
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_sha_chain_lightning_mastery::HandleDirectDamage, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_sha_chain_lightning_mastery::ExpandSilverChainTargets, EFFECT_0, TARGET_UNIT_TARGET_ENEMY);
        AfterHit += SpellHitFn(spell_sha_chain_lightning_mastery::HandleAfterHit);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    SpellMastery::SpellMasteryProgress _progress;
    ChainLightningMasteryEffects _effects;
    uint8 _processedTargets = 0;
    bool _isTriggeredCast = false;
    bool _xpAwarded = false;
    bool _diamondTriggered = false;
};

class spell_sha_lava_burst_mastery : public SpellScript
{
    PrepareSpellScript(spell_sha_lava_burst_mastery);

    bool Load() override
    {
        if (!GetCaster() || !GetCaster()->IsPlayer())
            return false;

        _playerCaster = GetCaster()->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SpellMastery::SPELL_SHAMAN_LAVA_BURST_RANK_1)
            return false;

        _progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildLavaBurstMasteryEffects(_progress, *_config);
        _flameShockSpellId = GetHighestKnownSpellInChain(_playerCaster, SpellMastery::SPELL_SHAMAN_FLAME_SHOCK_RANK_1);
        _isTriggeredCast = GetSpell()->IsTriggered();
        return true;
    }

    void HandleOnHitDamage()
    {
        Unit* target = GetHitUnit();
        if (!target || _playerCaster->IsFriendlyTo(target))
            return;

        int32 hitDamage = GetHitDamage();
        if (hitDamage <= 0)
            return;

        hitDamage = SpellMastery::ApplyEarlyAccessSpellScale(_playerCaster, GetSpellInfo(), hitDamage);

        if (_effects.BronzeDamageBonusPct > 0.0f)
        {
            int32 const scaledDamage = int32(std::lround(float(hitDamage) * (1.0f + (_effects.BronzeDamageBonusPct / 100.0f))));
            hitDamage = std::max(hitDamage, scaledDamage);
        }

        SetHitDamage(hitDamage);

        if (!_xpAwarded && !_isTriggeredCast && SpellMastery::ShouldAwardSpellMasteryXp(_playerCaster, *_config, LAVA_BURST_XP_GUARD_MS))
        {
            SpellMastery::AddSpellMasteryXp(_playerCaster, *_config, SpellMastery::SPELL_MASTERY_XP_PER_HIT);
            SpellMastery::SpellMasteryProgress const& progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
            SpellMastery::SendMasteryAddonMessageForProgress(_playerCaster, *_config, progress);
            _xpAwarded = true;
        }

        if (_effects.SilverFireDamageTakenPct > 0.0f)
            ApplyOrRefreshLavaBurstSilverDebuff(_playerCaster, target, _effects.SilverFireDamageTakenPct);

        TrySpreadFlameShock(target);
        TryDiamondFlameShockBurst(target);

        if (!_cooldownAdjusted && !_isTriggeredCast && _effects.IronCooldownReductionMs > 0)
        {
            _playerCaster->ModifySpellCooldown(_config->AllowedSpellId, -_effects.IronCooldownReductionMs);
            _cooldownAdjusted = true;
        }
    }

    void TrySpreadFlameShock(Unit* primaryTarget)
    {
        if (!primaryTarget || !_effects.GoldSpreadFlameShockTargets || !_flameShockSpellId)
            return;

        uint8 applied = 0;
        std::list<Unit*> nearbyUnits;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(primaryTarget, _playerCaster, LAVA_BURST_GOLD_SPREAD_RADIUS);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(primaryTarget, nearbyUnits, check);
        Cell::VisitObjects(primaryTarget, searcher, LAVA_BURST_GOLD_SPREAD_RADIUS);

        for (Unit* candidate : nearbyUnits)
        {
            if (!candidate || candidate == primaryTarget || !_playerCaster->IsValidAttackTarget(candidate))
                continue;

            if (HasFlameShockFromCaster(candidate, _playerCaster->GetGUID()))
                continue;

            _playerCaster->CastSpell(candidate, _flameShockSpellId, TRIGGERED_FULL_MASK);
            if (++applied >= _effects.GoldSpreadFlameShockTargets)
                break;
        }
    }

    void TryDiamondFlameShockBurst(Unit* primaryTarget)
    {
        if (!primaryTarget || !_effects.HasDiamondFlameShockBurst || _isTriggeredCast || _diamondTriggered)
            return;

        _diamondTriggered = true;

        std::list<Unit*> nearbyUnits;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(primaryTarget, _playerCaster, LAVA_BURST_DIAMOND_SEARCH_RADIUS);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(primaryTarget, nearbyUnits, check);
        Cell::VisitObjects(primaryTarget, searcher, LAVA_BURST_DIAMOND_SEARCH_RADIUS);

        for (Unit* candidate : nearbyUnits)
        {
            if (!candidate || !_playerCaster->IsValidAttackTarget(candidate))
                continue;

            if (!HasFlameShockFromCaster(candidate, _playerCaster->GetGUID()))
                continue;

            _playerCaster->CastSpell(
                candidate,
                _config->AllowedSpellId,
                TriggerCastFlags(TRIGGERED_IGNORE_GCD | TRIGGERED_IGNORE_CAST_IN_PROGRESS | TRIGGERED_IGNORE_POWER_AND_REAGENT_COST | TRIGGERED_IGNORE_SPELL_AND_CATEGORY_CD | TRIGGERED_CAST_DIRECTLY));
        }
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_sha_lava_burst_mastery::HandleOnHitDamage);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    SpellMastery::SpellMasteryProgress _progress;
    LavaBurstMasteryEffects _effects;
    uint32 _flameShockSpellId = 0;
    bool _isTriggeredCast = false;
    bool _xpAwarded = false;
    bool _cooldownAdjusted = false;
    bool _diamondTriggered = false;
};

class spell_mastery_prepare_shaman_spell_script : public AllSpellScript
{
public:
    spell_mastery_prepare_shaman_spell_script() : AllSpellScript("spell_mastery_prepare_shaman_spell_script", { ALLSPELLHOOK_ON_PREPARE })
    {
    }

    void OnSpellPrepare(Spell* spell, Unit* caster, SpellInfo const* spellInfo) override
    {
        if (!spell || !caster || !spellInfo || !caster->IsPlayer())
            return;

        if (!(spellInfo->SchoolMask & SPELL_SCHOOL_MASK_FIRE))
            return;

        Player* player = caster->ToPlayer();
        if (!player || player->getClass() != CLASS_SHAMAN)
            return;

        Unit* target = spell->m_targets.GetUnitTarget();
        if (!target || !player->IsValidAttackTarget(target))
            return;

        float const silverPct = GetLavaBurstSilverDebuffPct(player, target);
        if (silverPct <= 0.0f)
            return;

        ApplySpellDamagePercentModifier(spell, caster, spellInfo, silverPct);
    }
};

void AddSC_spell_mastery_shaman()
{
    new spell_mastery_prepare_shaman_spell_script();
    RegisterSpellScript(spell_sha_chain_lightning_mastery);
    RegisterSpellScript(spell_sha_lava_burst_mastery);
}
