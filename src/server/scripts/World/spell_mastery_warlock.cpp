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

#include "Cell.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Player.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "Unit.h"

#include <algorithm>
#include <cmath>
#include <list>
#include <vector>

namespace
{
struct HauntMasteryEffects
{
    float IronDamageBonusPct = 0.0f;
    float BronzeDotScalingPct = 20.0f;
    float SilverHealPct = 100.0f;
    int32 GoldCooldownReductionMs = 0;
    int32 DiamondDotExtensionMs = 0;
};

struct ShadowBoltMasteryEffects
{
    float IronDamageBonusPct = 0.0f;
    float BronzeManaRefundPct = 0.0f;
    float SilverSplashDamagePct = 0.0f;
    float GoldDotPerTickPct = 0.0f;
    uint8 DiamondExtraTargets = 0;
};

struct ChaosBoltMasteryEffects
{
    float IronDamageBonusPct = 0.0f;
    float BronzeManaRefundPct = 0.0f;
    float SilverCritChancePct = 0.0f;
    float GoldExecuteBonusPct = 0.0f;
    uint8 DiamondExtraTargets = 0;
};

int32 constexpr HAUNT_DIAMOND_DOT_MAX_DURATION_MS = 60000;
uint32 constexpr HAUNT_XP_GUARD_MS = 250;
uint32 constexpr SHADOW_BOLT_XP_GUARD_MS = 250;
uint32 constexpr CHAOS_BOLT_XP_GUARD_MS = 600;
float constexpr SHADOW_BOLT_SILVER_SPLASH_RADIUS = 8.0f;
float constexpr SHADOW_BOLT_DIAMOND_SEARCH_RADIUS = 25.0f;
float constexpr CHAOS_BOLT_DIAMOND_SEARCH_RADIUS = 25.0f;
float constexpr CHAOS_BOLT_GOLD_EXECUTE_HEALTH_PCT = 35.0f;
float constexpr SHADOW_BOLT_LOW_RANK_LEVEL_SCALING_PER_LEVEL = 0.30f;
float constexpr SHADOW_BOLT_LOW_RANK_LEVEL_SCALING_MAX_MULTIPLIER = 25.0f;
int32 constexpr SHADOW_BOLT_GOLD_DOT_BASE_DURATION_MS = 6000;
int32 constexpr SHADOW_BOLT_GOLD_DOT_DURATION_EXTEND_MS = 1000;
int32 constexpr SHADOW_BOLT_GOLD_DOT_DURATION_CAP_MS = 20000;
int32 constexpr SHADOW_BOLT_GOLD_DOT_FAST_TICK_INTERVAL_MS = 500;
float constexpr SHADOW_BOLT_GOLD_DOT_STACK_CARRYOVER_PCT = 50.0f;

HauntMasteryEffects BuildHauntMasteryEffects(SpellMastery::SpellMasteryProgress const& progress, SpellMastery::ManagedSpellConfig const& config)
{
    HauntMasteryEffects effects;

    uint8 ironLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_IRON, config);
    uint8 bronzeLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_BRONZE, config);
    uint8 silverLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_SILVER, config);
    uint8 goldLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_GOLD, config);
    uint8 diamondLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_DIAMOND, config);
    uint32 const totalMasteryLevels = uint32(ironLevel) + uint32(bronzeLevel) + uint32(silverLevel) + uint32(goldLevel) + uint32(diamondLevel);

    // Iron: increase direct Haunt impact damage.
    effects.IronDamageBonusPct = float(totalMasteryLevels) * 10.0f;

    // Bronze: increase Haunt periodic damage amplification from 20% to 100%.
    if (bronzeLevel > 0)
        effects.BronzeDotScalingPct = 20.0f + (float(bronzeLevel - 1) * (80.0f / 9.0f));

    // Silver: increase return heal from 100% to 200%.
    if (silverLevel > 0)
        effects.SilverHealPct = 100.0f + (float(silverLevel - 1) * (100.0f / 9.0f));

    // Gold: reduce effective cooldown from 8s to 3s.
    if (goldLevel > 0)
        effects.GoldCooldownReductionMs = int32(std::lround(5000.0f * (float(goldLevel) / 10.0f)));

    // Diamond: refresh all your Warlock DoTs and extend their duration.
    if (diamondLevel > 0)
        effects.DiamondDotExtensionMs = int32(std::lround(1000.0f + (float(diamondLevel - 1) * (4000.0f / 9.0f))));

    return effects;
}

ShadowBoltMasteryEffects BuildShadowBoltMasteryEffects(SpellMastery::SpellMasteryProgress const& progress, SpellMastery::ManagedSpellConfig const& config)
{
    ShadowBoltMasteryEffects effects;

    uint8 const ironLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_IRON, config);
    uint8 const bronzeLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_BRONZE, config);
    uint8 const silverLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_SILVER, config);
    uint8 const goldLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_GOLD, config);
    uint8 const diamondLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_DIAMOND, config);
    uint32 const totalMasteryLevels = uint32(ironLevel) + uint32(bronzeLevel) + uint32(silverLevel) + uint32(goldLevel) + uint32(diamondLevel);

    // Global output ramp: each mastery level contributes to Shadow Bolt direct damage.
    if (totalMasteryLevels > 0)
        effects.IronDamageBonusPct = float(totalMasteryLevels) * 8.0f;

    // Bronze: reduce mana cost by refunding part of cast power cost.
    if (bronzeLevel > 0)
        effects.BronzeManaRefundPct = 6.0f + (float(bronzeLevel - 1) * 2.0f); // 6% -> 24%

    // Silver: splash damage around the primary target.
    if (silverLevel > 0)
        effects.SilverSplashDamagePct = 20.0f + (float(silverLevel - 1) * (30.0f / 9.0f)); // 20% -> 50%

    // Gold: apply a scaling Shadow DoT.
    if (goldLevel > 0)
        effects.GoldDotPerTickPct = 0.5f + (float(goldLevel - 1) * 0.25f); // 0.5% -> 2.75% (50% reduction)

    // Diamond: hit additional nearby targets.
    if (diamondLevel > 0)
        effects.DiamondExtraTargets = diamondLevel;

    return effects;
}

ChaosBoltMasteryEffects BuildChaosBoltMasteryEffects(SpellMastery::SpellMasteryProgress const& progress, SpellMastery::ManagedSpellConfig const& config)
{
    ChaosBoltMasteryEffects effects;

    uint8 const ironLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_IRON, config);
    uint8 const bronzeLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_BRONZE, config);
    uint8 const silverLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_SILVER, config);
    uint8 const goldLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_GOLD, config);
    uint8 const diamondLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_DIAMOND, config);
    uint32 const totalMasteryLevels = uint32(ironLevel) + uint32(bronzeLevel) + uint32(silverLevel) + uint32(goldLevel) + uint32(diamondLevel);

    // Global output ramp: each mastery level contributes to Chaos Bolt direct damage.
    if (totalMasteryLevels > 0)
        effects.IronDamageBonusPct = float(totalMasteryLevels) * 6.0f;

    // Bronze: partial mana refund.
    if (bronzeLevel > 0)
        effects.BronzeManaRefundPct = 5.0f + (float(bronzeLevel - 1) * (15.0f / 9.0f)); // 5% -> 20%

    // Silver: bonus crit chance.
    if (silverLevel > 0)
        effects.SilverCritChancePct = 5.0f + (float(silverLevel - 1) * (25.0f / 9.0f)); // 5% -> 30%

    // Gold: execute damage bonus against low-health targets.
    if (goldLevel > 0)
        effects.GoldExecuteBonusPct = 10.0f + (float(goldLevel - 1) * (30.0f / 9.0f)); // 10% -> 40%

    // Diamond: fire extra Chaos Bolts at nearby enemies.
    if (diamondLevel > 0)
        effects.DiamondExtraTargets = uint8((diamondLevel + 2) / 3); // 1..4

    return effects;
}

bool IsWarlockPeriodicDotAura(Aura const* aura, ObjectGuid casterGuid)
{
    if (!aura || aura->GetCasterGUID() != casterGuid)
        return false;

    SpellInfo const* spellInfo = aura->GetSpellInfo();
    if (!spellInfo || spellInfo->SpellFamilyName != SPELLFAMILY_WARLOCK)
        return false;

    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        AuraEffect const* auraEffect = aura->GetEffect(i);
        if (!auraEffect)
            continue;

        AuraType const auraType = auraEffect->GetAuraType();
        if (auraType == SPELL_AURA_PERIODIC_DAMAGE || auraType == SPELL_AURA_PERIODIC_DAMAGE_PERCENT || auraType == SPELL_AURA_PERIODIC_LEECH)
            return true;
    }

    return false;
}

int32 ApplyShadowBoltLowRankLevelFloor(Player* player, SpellInfo const* spellInfo, int32 amount)
{
    if (!player || !spellInfo || amount <= 0)
        return amount;

    uint32 const rank = sSpellMgr->GetSpellRank(spellInfo->Id);
    if (rank < 1 || rank > 3)
        return amount;

    uint32 const playerLevel = std::max<uint32>(1, player->GetLevel());
    uint32 const naturalLevel = std::max<uint32>(1, std::max<uint32>(spellInfo->SpellLevel, spellInfo->BaseLevel));
    if (playerLevel <= naturalLevel)
        return amount;

    uint32 const levelGap = playerLevel - naturalLevel;
    float const multiplier = std::clamp(1.0f + (float(levelGap) * SHADOW_BOLT_LOW_RANK_LEVEL_SCALING_PER_LEVEL), 1.0f, SHADOW_BOLT_LOW_RANK_LEVEL_SCALING_MAX_MULTIPLIER);
    int32 const scaledAmount = int32(std::lround(float(amount) * multiplier));
    return std::max(amount, scaledAmount);
}

void ApplyStackingShadowBoltGoldDot(Player* caster, Unit* target, int32 addPerTick)
{
    if (!caster || !target || addPerTick <= 0)
        return;

    Aura* currentCorruptionAura = target->GetAuraOfRankedSpell(SpellMastery::SPELL_WARLOCK_CORRUPTION_RANK_1, caster->GetGUID());
    AuraEffect* currentCorruptionEffect = currentCorruptionAura ? currentCorruptionAura->GetEffect(EFFECT_0) : nullptr;

    int32 previousTickAmount = currentCorruptionEffect ? std::max<int32>(0, currentCorruptionEffect->GetAmount()) : 0;
    int32 priorMaxDuration = currentCorruptionAura ? std::max(currentCorruptionAura->GetMaxDuration(), SHADOW_BOLT_GOLD_DOT_BASE_DURATION_MS) : SHADOW_BOLT_GOLD_DOT_BASE_DURATION_MS;
    int32 priorDuration = currentCorruptionAura ? std::max(currentCorruptionAura->GetDuration(), SHADOW_BOLT_GOLD_DOT_BASE_DURATION_MS) : SHADOW_BOLT_GOLD_DOT_BASE_DURATION_MS;
    int32 const perTickFromHit = std::max<int32>(1, addPerTick);
    int32 const carryOverTick = std::max<int32>(0, int32(std::lround(float(previousTickAmount) * (SHADOW_BOLT_GOLD_DOT_STACK_CARRYOVER_PCT / 100.0f))));
    int32 const stackedPerTick = std::max<int32>(1, carryOverTick + perTickFromHit);

    uint32 corruptionSpellId = currentCorruptionAura ? currentCorruptionAura->GetId() : SpellMastery::SPELL_WARLOCK_CORRUPTION_RANK_1;
    caster->CastCustomSpell(
        corruptionSpellId,
        SPELLVALUE_BASE_POINT0,
        stackedPerTick,
        target,
        TriggerCastFlags(TRIGGERED_FULL_MASK & ~TRIGGERED_NO_PERIODIC_RESET),
        nullptr,
        currentCorruptionEffect,
        caster->GetGUID());

    Aura* refreshedCorruptionAura = target->GetAuraOfRankedSpell(SpellMastery::SPELL_WARLOCK_CORRUPTION_RANK_1, caster->GetGUID());
    if (!refreshedCorruptionAura)
        return;

    int32 const nextMaxDuration = std::min<int32>(SHADOW_BOLT_GOLD_DOT_DURATION_CAP_MS, priorMaxDuration + SHADOW_BOLT_GOLD_DOT_DURATION_EXTEND_MS);
    int32 const nextDuration = std::min<int32>(nextMaxDuration, priorDuration + SHADOW_BOLT_GOLD_DOT_DURATION_EXTEND_MS);
    refreshedCorruptionAura->SetMaxDuration(nextMaxDuration);
    refreshedCorruptionAura->SetDuration(nextDuration);

    if (AuraEffect* refreshedEffect = refreshedCorruptionAura->GetEffect(EFFECT_0))
    {
        refreshedEffect->SetAmount(std::max<int32>(refreshedEffect->GetAmount(), stackedPerTick));
        if (refreshedEffect->GetPeriodicTimer() > SHADOW_BOLT_GOLD_DOT_FAST_TICK_INTERVAL_MS)
            refreshedEffect->SetPeriodicTimer(SHADOW_BOLT_GOLD_DOT_FAST_TICK_INTERVAL_MS);
    }
}
}

void ClearSpellMasteryWarlockRuntimeStateForPlayer(uint32 /*guid*/)
{
}

class spell_warl_haunt_mastery : public SpellScript
{
    PrepareSpellScript(spell_warl_haunt_mastery);

    bool Load() override
    {
        if (!GetCaster() || !GetCaster()->IsPlayer())
            return false;

        _playerCaster = GetCaster()->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SpellMastery::SPELL_WARLOCK_HAUNT_RANK_1)
            return false;

        _progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildHauntMasteryEffects(_progress, *_config);
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

        if (_effects.IronDamageBonusPct > 0.0f)
        {
            int32 const scaledDamage = int32(std::lround(float(hitDamage) * (1.0f + (_effects.IronDamageBonusPct / 100.0f))));
            hitDamage = std::max(hitDamage, scaledDamage);
        }

        SetHitDamage(hitDamage);
    }

    void HandleAfterHit()
    {
        Unit* target = GetHitUnit();
        if (!target || !_playerCaster->IsHostileTo(target))
            return;

        if (!_xpAwarded && SpellMastery::ShouldAwardSpellMasteryXp(_playerCaster, *_config, HAUNT_XP_GUARD_MS))
        {
            SpellMastery::AddSpellMasteryXp(_playerCaster, *_config, SpellMastery::SPELL_MASTERY_XP_PER_HIT);
            _xpAwarded = true;
        }

        if (Aura* hauntAura = GetHitAura())
            TryApplyBronzeDotScaling(hauntAura);

        if (!_cooldownAdjusted && _effects.GoldCooldownReductionMs > 0)
        {
            _playerCaster->ModifySpellCooldown(_config->AllowedSpellId, -_effects.GoldCooldownReductionMs);
            _cooldownAdjusted = true;
        }

        if (!_playerCaster->IsValidAttackTarget(target))
            return;

        TryRefreshDiamondDots(target);
    }

    void TryApplyBronzeDotScaling(Aura* hauntAura)
    {
        if (!hauntAura || _effects.BronzeDotScalingPct <= 0.0f)
            return;

        if (AuraEffect* periodicBonusEffect = hauntAura->GetEffect(EFFECT_0))
            periodicBonusEffect->SetAmount(int32(std::lround(_effects.BronzeDotScalingPct)));
    }

    void TryRefreshDiamondDots(Unit* target)
    {
        if (!target || _effects.DiamondDotExtensionMs <= 0)
            return;

        std::vector<Aura*> dotsToRefresh;
        Unit::AuraApplicationMap const& auras = target->GetAppliedAuras();
        for (Unit::AuraApplicationMap::const_iterator itr = auras.begin(); itr != auras.end(); ++itr)
        {
            Aura* aura = itr->second->GetBase();
            if (!aura || aura->GetId() == GetSpellInfo()->Id || !IsWarlockPeriodicDotAura(aura, _playerCaster->GetGUID()))
                continue;

            if (std::find(dotsToRefresh.begin(), dotsToRefresh.end(), aura) == dotsToRefresh.end())
                dotsToRefresh.push_back(aura);
        }

        for (Aura* aura : dotsToRefresh)
        {
            int32 const currentMaxDuration = aura->GetMaxDuration();
            if (currentMaxDuration <= 0)
                continue;

            aura->RefreshTimersWithMods();

            int32 const extendedMaxDuration = std::min<int32>(HAUNT_DIAMOND_DOT_MAX_DURATION_MS, currentMaxDuration + _effects.DiamondDotExtensionMs);
            aura->SetMaxDuration(extendedMaxDuration);
            aura->SetDuration(extendedMaxDuration);
        }
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_warl_haunt_mastery::HandleDirectDamage, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
        AfterHit += SpellHitFn(spell_warl_haunt_mastery::HandleAfterHit);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    SpellMastery::SpellMasteryProgress _progress;
    HauntMasteryEffects _effects;
    bool _xpAwarded = false;
    bool _cooldownAdjusted = false;
};

class spell_warl_haunt_mastery_aura : public AuraScript
{
    PrepareAuraScript(spell_warl_haunt_mastery_aura);

    bool Load() override
    {
        if (!GetCaster() || !GetCaster()->IsPlayer())
            return false;

        _playerCaster = GetCaster()->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SpellMastery::SPELL_WARLOCK_HAUNT_RANK_1)
            return false;

        SpellMastery::SpellMasteryProgress const& progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildHauntMasteryEffects(progress, *_config);
        return true;
    }

    void HandleRemove(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        if (!_playerCaster || _effects.SilverHealPct <= 100.0f)
            return;

        int32 const baseHeal = std::max<int32>(0, aurEff->GetAmount());
        if (baseHeal <= 0)
            return;

        int32 const bonusPct = int32(std::lround(_effects.SilverHealPct - 100.0f));
        if (bonusPct <= 0)
            return;

        int32 const bonusHeal = CalculatePct(baseHeal, bonusPct);
        if (bonusHeal <= 0)
            return;

        GetTarget()->CastCustomSpell(_playerCaster, SpellMastery::SPELL_WARLOCK_HAUNT_HEAL, &bonusHeal, nullptr, nullptr, true, nullptr, aurEff, GetCasterGUID());
    }

    void Register() override
    {
        OnEffectRemove += AuraEffectRemoveFn(spell_warl_haunt_mastery_aura::HandleRemove, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    HauntMasteryEffects _effects;
};

class spell_warl_shadow_bolt_mastery : public SpellScript
{
    PrepareSpellScript(spell_warl_shadow_bolt_mastery);

    bool Load() override
    {
        if (!GetCaster() || !GetCaster()->IsPlayer())
            return false;

        _playerCaster = GetCaster()->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SpellMastery::SPELL_WARLOCK_SHADOW_BOLT_RANK_1)
            return false;

        SpellMastery::SpellMasteryProgress const& progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildShadowBoltMasteryEffects(progress, *_config);
        _isTriggeredCast = GetSpell()->IsTriggered();
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

        int32 const rawHitDamage = hitDamage;
        hitDamage = SpellMastery::ApplyEarlyAccessSpellScale(_playerCaster, GetSpellInfo(), hitDamage);
        hitDamage = std::max(hitDamage, ApplyShadowBoltLowRankLevelFloor(_playerCaster, GetSpellInfo(), rawHitDamage));

        if (_effects.IronDamageBonusPct > 0.0f)
        {
            int32 const scaledDamage = int32(std::lround(float(hitDamage) * (1.0f + (_effects.IronDamageBonusPct / 100.0f))));
            hitDamage = std::max(hitDamage, scaledDamage);
        }

        SetHitDamage(hitDamage);
        _finalHitDamage = hitDamage;
    }

    void HandleAfterHit()
    {
        Unit* target = GetHitUnit();
        if (!target || !_playerCaster->IsValidAttackTarget(target))
            return;

        int32 const hitDamage = std::max<int32>(GetHitDamage(), _finalHitDamage);
        if (hitDamage <= 0)
            return;

        if (!_xpAwarded && !_isTriggeredCast && SpellMastery::ShouldAwardSpellMasteryXp(_playerCaster, *_config, SHADOW_BOLT_XP_GUARD_MS))
        {
            SpellMastery::AddSpellMasteryXp(_playerCaster, *_config, SpellMastery::SPELL_MASTERY_XP_PER_HIT);
            _xpAwarded = true;
        }

        TryApplyBronzeManaRefund();
        TryApplySilverSplash(target, hitDamage);
        TryApplyGoldDot(target, hitDamage);
        TryApplyDiamondExtraBolts(target);
    }

    void TryApplyBronzeManaRefund()
    {
        if (_manaRefundApplied || _effects.BronzeManaRefundPct <= 0.0f || !_playerCaster->HasActivePowerType(POWER_MANA))
            return;

        int32 const castCost = std::max<int32>(0, GetSpell()->GetPowerCost());
        if (castCost <= 0)
            return;

        int32 const refund = std::max<int32>(1, int32(std::lround(float(castCost) * (_effects.BronzeManaRefundPct / 100.0f))));
        _playerCaster->ModifyPower(POWER_MANA, refund);
        _manaRefundApplied = true;
    }

    void TryApplySilverSplash(Unit* primaryTarget, int32 hitDamage)
    {
        if (!primaryTarget || hitDamage <= 0 || _effects.SilverSplashDamagePct <= 0.0f)
            return;

        int32 const splashDamage = std::max<int32>(1, int32(std::lround(float(hitDamage) * (_effects.SilverSplashDamagePct / 100.0f))));
        std::list<Unit*> nearbyUnits;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(primaryTarget, _playerCaster, SHADOW_BOLT_SILVER_SPLASH_RADIUS);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(primaryTarget, nearbyUnits, check);
        Cell::VisitObjects(primaryTarget, searcher, SHADOW_BOLT_SILVER_SPLASH_RADIUS);

        for (Unit* candidate : nearbyUnits)
        {
            if (!candidate || candidate == primaryTarget || !_playerCaster->IsValidAttackTarget(candidate) || !candidate->IsAlive())
                continue;

            SpellNonMeleeDamage splashInfo(_playerCaster, candidate, GetSpellInfo(), GetSpellInfo()->SchoolMask);
            splashInfo.damage = splashDamage;
            _playerCaster->SendSpellNonMeleeDamageLog(&splashInfo);
            _playerCaster->DealSpellDamage(&splashInfo, false);
        }
    }

    void TryApplyGoldDot(Unit* target, int32 hitDamage)
    {
        if (!target || hitDamage <= 0 || _effects.GoldDotPerTickPct <= 0.0f)
            return;

        int32 const dotPerTick = std::max<int32>(1, int32(std::lround(float(hitDamage) * (_effects.GoldDotPerTickPct / 100.0f))));
        ApplyStackingShadowBoltGoldDot(_playerCaster, target, dotPerTick);
    }

    void TryApplyDiamondExtraBolts(Unit* primaryTarget)
    {
        if (!primaryTarget || _effects.DiamondExtraTargets == 0 || _isTriggeredCast)
            return;

        uint8 launched = 0;
        std::list<Unit*> nearbyUnits;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(primaryTarget, _playerCaster, SHADOW_BOLT_DIAMOND_SEARCH_RADIUS);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(primaryTarget, nearbyUnits, check);
        Cell::VisitObjects(primaryTarget, searcher, SHADOW_BOLT_DIAMOND_SEARCH_RADIUS);

        for (Unit* candidate : nearbyUnits)
        {
            if (!candidate || candidate == primaryTarget || !_playerCaster->IsValidAttackTarget(candidate) || !candidate->IsAlive())
                continue;

            _playerCaster->CastSpell(
                candidate,
                _config->AllowedSpellId,
                TriggerCastFlags(TRIGGERED_IGNORE_GCD | TRIGGERED_IGNORE_CAST_IN_PROGRESS | TRIGGERED_IGNORE_POWER_AND_REAGENT_COST | TRIGGERED_IGNORE_SPELL_AND_CATEGORY_CD | TRIGGERED_CAST_DIRECTLY));

            if (++launched >= _effects.DiamondExtraTargets)
                break;
        }
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_warl_shadow_bolt_mastery::HandleDirectDamage, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
        AfterHit += SpellHitFn(spell_warl_shadow_bolt_mastery::HandleAfterHit);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    ShadowBoltMasteryEffects _effects;
    bool _isTriggeredCast = false;
    bool _xpAwarded = false;
    bool _manaRefundApplied = false;
    int32 _finalHitDamage = 0;
};

class spell_warl_chaos_bolt_mastery : public SpellScript
{
    PrepareSpellScript(spell_warl_chaos_bolt_mastery);

    bool Load() override
    {
        if (!GetCaster() || !GetCaster()->IsPlayer())
            return false;

        _playerCaster = GetCaster()->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SpellMastery::SPELL_WARLOCK_CHAOS_BOLT_RANK_1)
            return false;

        SpellMastery::SpellMasteryProgress const& progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildChaosBoltMasteryEffects(progress, *_config);
        _isTriggeredCast = GetSpell()->IsTriggered();
        return true;
    }

    void HandleBeforeHit(SpellMissInfo /*missInfo*/)
    {
        if (_effects.SilverCritChancePct <= 0.0f)
            return;

        Unit* target = GetHitUnit();
        if (!target || !_playerCaster->IsValidAttackTarget(target))
            return;

        if (roll_chance_f(_effects.SilverCritChancePct))
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

        hitDamage = SpellMastery::ApplyEarlyAccessSpellScale(_playerCaster, GetSpellInfo(), hitDamage);

        if (_effects.IronDamageBonusPct > 0.0f)
        {
            int32 const scaledDamage = int32(std::lround(float(hitDamage) * (1.0f + (_effects.IronDamageBonusPct / 100.0f))));
            hitDamage = std::max(hitDamage, scaledDamage);
        }

        if (_effects.GoldExecuteBonusPct > 0.0f && target->GetHealthPct() <= CHAOS_BOLT_GOLD_EXECUTE_HEALTH_PCT)
        {
            int32 const executeScaled = int32(std::lround(float(hitDamage) * (1.0f + (_effects.GoldExecuteBonusPct / 100.0f))));
            hitDamage = std::max(hitDamage, executeScaled);
        }

        SetHitDamage(hitDamage);
    }

    void HandleAfterHit()
    {
        Unit* target = GetHitUnit();
        if (!target || !_playerCaster->IsValidAttackTarget(target))
            return;

        if (!_xpAwarded && !_isTriggeredCast && SpellMastery::ShouldAwardSpellMasteryXp(_playerCaster, *_config, CHAOS_BOLT_XP_GUARD_MS))
        {
            SpellMastery::AddSpellMasteryXp(_playerCaster, *_config, SpellMastery::SPELL_MASTERY_XP_PER_HIT);
            _xpAwarded = true;
        }

        TryApplyBronzeManaRefund();
        TryApplyDiamondExtraBolts(target);
    }

    void TryApplyBronzeManaRefund()
    {
        if (_manaRefundApplied || _effects.BronzeManaRefundPct <= 0.0f || !_playerCaster->HasActivePowerType(POWER_MANA))
            return;

        int32 const castCost = std::max<int32>(0, GetSpell()->GetPowerCost());
        if (castCost <= 0)
            return;

        int32 const refund = std::max<int32>(1, int32(std::lround(float(castCost) * (_effects.BronzeManaRefundPct / 100.0f))));
        _playerCaster->ModifyPower(POWER_MANA, refund);
        _manaRefundApplied = true;
    }

    void TryApplyDiamondExtraBolts(Unit* primaryTarget)
    {
        if (!primaryTarget || _effects.DiamondExtraTargets == 0 || _isTriggeredCast)
            return;

        uint8 launched = 0;
        std::list<Unit*> nearbyUnits;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(primaryTarget, _playerCaster, CHAOS_BOLT_DIAMOND_SEARCH_RADIUS);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(primaryTarget, nearbyUnits, check);
        Cell::VisitObjects(primaryTarget, searcher, CHAOS_BOLT_DIAMOND_SEARCH_RADIUS);

        for (Unit* candidate : nearbyUnits)
        {
            if (!candidate || candidate == primaryTarget || !_playerCaster->IsValidAttackTarget(candidate) || !candidate->IsAlive())
                continue;

            _playerCaster->CastSpell(
                candidate,
                _config->AllowedSpellId,
                TriggerCastFlags(TRIGGERED_IGNORE_GCD | TRIGGERED_IGNORE_CAST_IN_PROGRESS | TRIGGERED_IGNORE_POWER_AND_REAGENT_COST | TRIGGERED_IGNORE_SPELL_AND_CATEGORY_CD | TRIGGERED_CAST_DIRECTLY));

            if (++launched >= _effects.DiamondExtraTargets)
                break;
        }
    }

    void Register() override
    {
        BeforeHit += BeforeSpellHitFn(spell_warl_chaos_bolt_mastery::HandleBeforeHit);
        OnEffectHitTarget += SpellEffectFn(spell_warl_chaos_bolt_mastery::HandleDirectDamage, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
        AfterHit += SpellHitFn(spell_warl_chaos_bolt_mastery::HandleAfterHit);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    ChaosBoltMasteryEffects _effects;
    bool _isTriggeredCast = false;
    bool _xpAwarded = false;
    bool _manaRefundApplied = false;
};

void AddSC_spell_mastery_warlock()
{
    RegisterSpellAndAuraScriptPair(spell_warl_haunt_mastery, spell_warl_haunt_mastery_aura);
    RegisterSpellScript(spell_warl_shadow_bolt_mastery);
    RegisterSpellScript(spell_warl_chaos_bolt_mastery);
}
