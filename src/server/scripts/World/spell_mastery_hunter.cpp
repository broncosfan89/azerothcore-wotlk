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
#include "ObjectAccessor.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"

#include <algorithm>
#include <cmath>
#include <list>
#include <mutex>
#include <unordered_map>

namespace
{
struct VolleyMasteryEffects
{
    float IronDamageBonusPct = 0.0f;
    float BronzeRadiusMultiplier = 1.0f;
    int32 SilverTickIntervalMs = 0;
    uint8 GoldSpreadTargets = 0;
    float DiamondBurstDamagePct = 0.0f;
};

struct SerpentStingMasteryEffects
{
    float IronManaRegenPct = 0.0f;
    float BronzeDamageBonusPct = 0.0f;
    int32 SilverTickIntervalMs = 0;
    uint8 GoldSpreadTargets = 0;
    float GoldSpreadDamagePct = 0.0f;
    float DiamondDetonationDamagePct = 0.0f;
};

struct AimedShotMasteryEffects
{
    float IronDamageBonusPct = 0.0f;
    float BronzeManaCostReductionPct = 0.0f;
    float SilverCritAtHalfChargePct = 0.0f;
    float GoldHighChargeDamagePct = 0.0f;
    float DiamondSplashDamagePct = 0.0f;
};

struct AimedShotChargeState
{
    uint32 StartedAtMs = 0;
    uint32 CastWindowMs = 1;
    uint32 BaseManaCost = 0;
    ObjectGuid TargetGuid = ObjectGuid::Empty;
};

struct PendingAimedShotReleaseState
{
    float ChargeRatio = 0.0f;
    uint32 ManualManaCost = 0;
};

uint32 constexpr VOLLEY_XP_GUARD_MS = 8000;
float constexpr VOLLEY_DIAMOND_BURST_RADIUS = 6.0f;
int32 constexpr VOLLEY_GOLD_BASE_TICK_INTERVAL_MS = 1000;
int32 constexpr VOLLEY_GOLD_MIN_TICK_INTERVAL_MS = 750;
uint32 constexpr SERPENT_STING_XP_GUARD_MS = 250;
int32 constexpr SERPENT_STING_BASE_TICK_INTERVAL_MS = 3000;
int32 constexpr SERPENT_STING_MIN_TICK_INTERVAL_MS = 1000;
float constexpr SERPENT_STING_GOLD_SPREAD_RADIUS = 10.0f;
float constexpr SERPENT_STING_DIAMOND_DETONATION_RADIUS = 10.0f;
float constexpr SERPENT_STING_RANGED_AP_TICK_SCALAR = 0.20f;
uint32 constexpr AIMED_SHOT_XP_GUARD_MS = 750;
uint32 constexpr AIMED_SHOT_CHARGE_WINDOW_MS = 3000;
float constexpr AIMED_SHOT_MIN_CHARGE_RATIO = 0.15f;
float constexpr AIMED_SHOT_DIAMOND_SPLASH_RADIUS = 8.0f;

std::unordered_map<uint32, AimedShotChargeState> AimedShotChargeStates;
std::unordered_map<uint32, PendingAimedShotReleaseState> PendingAimedShotReleaseStates;
std::mutex AimedShotStateMutex;

VolleyMasteryEffects BuildVolleyMasteryEffects(SpellMastery::SpellMasteryProgress const& progress, SpellMastery::ManagedSpellConfig const& config)
{
    VolleyMasteryEffects effects;

    uint8 const ironLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_IRON, config);
    uint8 const bronzeLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_BRONZE, config);
    uint8 const silverLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_SILVER, config);
    uint8 const goldLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_GOLD, config);
    uint8 const diamondLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_DIAMOND, config);
    uint32 const totalMasteryLevels = uint32(ironLevel) + uint32(bronzeLevel) + uint32(silverLevel) + uint32(goldLevel) + uint32(diamondLevel);

    // Damage scaling spans all tiers: +1.5% per mastery level (max +75% at Diamond 10).
    if (totalMasteryLevels > 0)
        effects.IronDamageBonusPct = float(totalMasteryLevels) * 1.5f;

    // Bronze: increase Volley radius.
    if (bronzeLevel > 0)
        effects.BronzeRadiusMultiplier += 0.5f * (float(bronzeLevel) / 10.0f);

    // Silver: increase Volley tick rate (faster periodic trigger).
    if (silverLevel > 0)
    {
        int32 const reductionMs = int32(std::lround(float(VOLLEY_GOLD_BASE_TICK_INTERVAL_MS - VOLLEY_GOLD_MIN_TICK_INTERVAL_MS) * (float(silverLevel) / 10.0f)));
        effects.SilverTickIntervalMs = std::max<int32>(VOLLEY_GOLD_MIN_TICK_INTERVAL_MS, VOLLEY_GOLD_BASE_TICK_INTERVAL_MS - reductionMs);
    }

    // Gold: spread Serpent Sting from a stung target to nearby targets.
    if (goldLevel > 0)
        effects.GoldSpreadTargets = goldLevel;

    // Diamond: add AoE burst damage around each target hit by Volley.
    if (diamondLevel > 0)
        effects.DiamondBurstDamagePct = 5.0f + (float(diamondLevel - 1) * (10.0f / 9.0f)); // 5% -> 15%

    return effects;
}

SerpentStingMasteryEffects BuildSerpentStingMasteryEffects(SpellMastery::SpellMasteryProgress const& progress, SpellMastery::ManagedSpellConfig const& config)
{
    SerpentStingMasteryEffects effects;

    uint8 const ironLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_IRON, config);
    uint8 const bronzeLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_BRONZE, config);
    uint8 const silverLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_SILVER, config);
    uint8 const goldLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_GOLD, config);
    uint8 const diamondLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_DIAMOND, config);
    uint32 const totalMasteryLevels = uint32(ironLevel) + uint32(bronzeLevel) + uint32(silverLevel) + uint32(goldLevel) + uint32(diamondLevel);

    // Iron: mana regeneration on successful Serpent Sting application.
    if (ironLevel > 0)
        effects.IronManaRegenPct = float(ironLevel) * 2.0f;

    // Damage scaling follows full mastery progression (Iron -> Diamond).
    if (totalMasteryLevels > 0)
        effects.BronzeDamageBonusPct = float(totalMasteryLevels) * 30.0f;

    // Silver: faster tick cadence.
    if (silverLevel > 0)
    {
        int32 const reductionMs = int32(std::lround(float(SERPENT_STING_BASE_TICK_INTERVAL_MS - SERPENT_STING_MIN_TICK_INTERVAL_MS) * (float(silverLevel) / 10.0f)));
        effects.SilverTickIntervalMs = std::max<int32>(SERPENT_STING_MIN_TICK_INTERVAL_MS, SERPENT_STING_BASE_TICK_INTERVAL_MS - reductionMs);
    }

    // Gold: spread a portion of each tick to N nearby targets.
    if (goldLevel > 0)
    {
        effects.GoldSpreadTargets = goldLevel;
        effects.GoldSpreadDamagePct = 20.0f + (float(goldLevel - 1) * (20.0f / 9.0f)); // 20% -> 40%
    }

    // Diamond: AoE detonation when Serpent Sting expires naturally.
    if (diamondLevel > 0)
        effects.DiamondDetonationDamagePct = 60.0f + (float(diamondLevel - 1) * 10.0f); // 60% -> 150%

    return effects;
}

AimedShotMasteryEffects BuildAimedShotMasteryEffects(SpellMastery::SpellMasteryProgress const& progress, SpellMastery::ManagedSpellConfig const& config)
{
    AimedShotMasteryEffects effects;

    uint8 const ironLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_IRON, config);
    uint8 const bronzeLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_BRONZE, config);
    uint8 const silverLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_SILVER, config);
    uint8 const goldLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_GOLD, config);
    uint8 const diamondLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_DIAMOND, config);

    if (ironLevel > 0)
        effects.IronDamageBonusPct = float(ironLevel) * 5.0f;

    if (bronzeLevel > 0)
        effects.BronzeManaCostReductionPct = float(bronzeLevel) * 2.5f;

    if (silverLevel > 0)
        effects.SilverCritAtHalfChargePct = float(silverLevel) * 2.0f;

    if (goldLevel > 0)
        effects.GoldHighChargeDamagePct = 10.0f + (float(goldLevel - 1) * (20.0f / 9.0f)); // 10% -> 30%

    if (diamondLevel > 0)
        effects.DiamondSplashDamagePct = 20.0f + (float(diamondLevel - 1) * (30.0f / 9.0f)); // 20% -> 50%

    return effects;
}

float ComputeAimedShotChargeRatio(uint32 elapsedMs, uint32 windowMs)
{
    if (!windowMs)
        return AIMED_SHOT_MIN_CHARGE_RATIO;

    float const ratio = float(elapsedMs) / float(windowMs);
    return std::clamp(ratio, AIMED_SHOT_MIN_CHARGE_RATIO, 1.0f);
}

uint32 ComputeAimedShotTotalManaCost(uint32 baseManaCost, float chargeRatio, AimedShotMasteryEffects const& effects)
{
    if (!baseManaCost)
        return 0;

    float costMultiplier = 0.25f + (1.75f * std::clamp(chargeRatio, AIMED_SHOT_MIN_CHARGE_RATIO, 1.0f)); // 25% -> 200%
    if (effects.BronzeManaCostReductionPct > 0.0f)
        costMultiplier *= std::max(0.10f, 1.0f - (effects.BronzeManaCostReductionPct / 100.0f));

    return std::max<uint32>(1, uint32(std::lround(float(baseManaCost) * costMultiplier)));
}

void BeginAimedShotCharge(Player* caster, Spell* spell)
{
    if (!caster || !spell)
        return;

    uint32 const castWindowMs = uint32(std::max<int32>(1, spell->GetCastTime()));
    uint32 const baseManaCost = uint32(std::max<int32>(0, spell->GetPowerCost()));
    AimedShotChargeState state;
    state.StartedAtMs = uint32(GameTime::GetGameTimeMS().count());
    state.CastWindowMs = castWindowMs;
    state.BaseManaCost = baseManaCost;
    state.TargetGuid = spell->m_targets.GetUnitTargetGUID();

    std::lock_guard<std::mutex> lock(AimedShotStateMutex);
    AimedShotChargeStates[uint32(caster->GetGUID().GetCounter())] = state;
}

bool PopAimedShotChargeState(uint32 casterGuid, AimedShotChargeState& outState)
{
    std::lock_guard<std::mutex> lock(AimedShotStateMutex);
    auto itr = AimedShotChargeStates.find(casterGuid);
    if (itr == AimedShotChargeStates.end())
        return false;

    outState = itr->second;
    AimedShotChargeStates.erase(itr);
    return true;
}

void SetPendingAimedShotReleaseState(uint32 casterGuid, PendingAimedShotReleaseState const& state)
{
    std::lock_guard<std::mutex> lock(AimedShotStateMutex);
    PendingAimedShotReleaseStates[casterGuid] = state;
}

bool PopPendingAimedShotReleaseState(uint32 casterGuid, PendingAimedShotReleaseState& outState)
{
    std::lock_guard<std::mutex> lock(AimedShotStateMutex);
    auto itr = PendingAimedShotReleaseStates.find(casterGuid);
    if (itr == PendingAimedShotReleaseStates.end())
        return false;

    outState = itr->second;
    PendingAimedShotReleaseStates.erase(itr);
    return true;
}

bool IsVolleyPeriodicTriggerEffect(AuraEffect const* aurEff)
{
    return aurEff && aurEff->GetAuraType() == SPELL_AURA_PERIODIC_TRIGGER_SPELL;
}
}

void ClearSpellMasteryHunterRuntimeStateForPlayer(uint32 guid)
{
    std::lock_guard<std::mutex> lock(AimedShotStateMutex);
    AimedShotChargeStates.erase(guid);
    PendingAimedShotReleaseStates.erase(guid);
}

class spell_hun_volley_mastery : public SpellScript
{
    PrepareSpellScript(spell_hun_volley_mastery);

    bool Load() override
    {
        if (!GetCaster() || !GetCaster()->IsPlayer())
            return false;

        _playerCaster = GetCaster()->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SpellMastery::SPELL_HUNTER_VOLLEY_RANK_1)
            return false;

        SpellMastery::SpellMasteryProgress const& progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildVolleyMasteryEffects(progress, *_config);

        if (_effects.BronzeRadiusMultiplier > 1.0f)
            GetSpell()->SetSpellValue(SPELLVALUE_RADIUS_MOD, int32(std::lround(_effects.BronzeRadiusMultiplier * 10000.0f)));

        return true;
    }

    void Register() override
    {
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    VolleyMasteryEffects _effects;
};

class spell_hun_volley_mastery_aura : public AuraScript
{
    PrepareAuraScript(spell_hun_volley_mastery_aura);

    bool Load() override
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->IsPlayer())
            return false;

        _playerCaster = caster->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SpellMastery::SPELL_HUNTER_VOLLEY_RANK_1)
            return false;

        SpellMastery::SpellMasteryProgress const& progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildVolleyMasteryEffects(progress, *_config);
        return true;
    }

    void CalculatePeriodicTiming(AuraEffect const* aurEff, bool& isPeriodic, int32& amplitude)
    {
        if (_effects.SilverTickIntervalMs <= 0 || !IsVolleyPeriodicTriggerEffect(aurEff))
            return;

        isPeriodic = true;
        amplitude = _effects.SilverTickIntervalMs;
    }

    void HandlePeriodicUpdate(AuraEffect* aurEff)
    {
        if (_effects.SilverTickIntervalMs <= 0 || !aurEff || !IsVolleyPeriodicTriggerEffect(aurEff))
            return;

        if (aurEff->GetPeriodicTimer() > _effects.SilverTickIntervalMs)
            aurEff->SetPeriodicTimer(_effects.SilverTickIntervalMs);
    }

    void HandleEffectApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (_effects.SilverTickIntervalMs <= 0)
            return;

        Aura* aura = GetAura();
        if (!aura)
            return;

        for (uint8 effectIndex = EFFECT_0; effectIndex < MAX_SPELL_EFFECTS; ++effectIndex)
        {
            if (AuraEffect* periodic = aura->GetEffect(effectIndex))
            {
                if (IsVolleyPeriodicTriggerEffect(periodic) && periodic->GetPeriodicTimer() > _effects.SilverTickIntervalMs)
                    periodic->SetPeriodicTimer(_effects.SilverTickIntervalMs);
            }
        }
    }

    void Register() override
    {
        DoEffectCalcPeriodic += AuraEffectCalcPeriodicFn(spell_hun_volley_mastery_aura::CalculatePeriodicTiming, EFFECT_ALL, SPELL_AURA_PERIODIC_TRIGGER_SPELL);
        OnEffectUpdatePeriodic += AuraEffectUpdatePeriodicFn(spell_hun_volley_mastery_aura::HandlePeriodicUpdate, EFFECT_ALL, SPELL_AURA_PERIODIC_TRIGGER_SPELL);
        OnEffectApply += AuraEffectApplyFn(spell_hun_volley_mastery_aura::HandleEffectApply, EFFECT_ALL, SPELL_AURA_PERIODIC_TRIGGER_SPELL, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    VolleyMasteryEffects _effects;
};

class spell_hun_volley_trigger_mastery : public SpellScript
{
    PrepareSpellScript(spell_hun_volley_trigger_mastery);

    bool Load() override
    {
        if (!GetCaster() || !GetCaster()->IsPlayer())
            return false;

        _playerCaster = GetCaster()->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigByBaseSpell(SpellMastery::SPELL_HUNTER_VOLLEY_RANK_1);
        if (!_config)
            return false;

        SpellMastery::SpellMasteryProgress const& progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildVolleyMasteryEffects(progress, *_config);

        if (_effects.BronzeRadiusMultiplier > 1.0f)
            GetSpell()->SetSpellValue(SPELLVALUE_RADIUS_MOD, int32(std::lround(_effects.BronzeRadiusMultiplier * 10000.0f)));

        return true;
    }

    void HandleOnHit()
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
        if (SpellMastery::ShouldAwardSpellMasteryXp(_playerCaster, *_config, VOLLEY_XP_GUARD_MS))
            SpellMastery::AddSpellMasteryXp(_playerCaster, *_config, SpellMastery::SPELL_MASTERY_XP_PER_HIT);

        if (!_goldSpreadApplied)
            _goldSpreadApplied = TryApplyGoldSerpentSpread(target);

        TryApplyDiamondBurst(target, hitDamage);
    }

    bool TryApplyGoldSerpentSpread(Unit* primaryTarget)
    {
        if (!primaryTarget || _effects.GoldSpreadTargets == 0)
            return false;

        if (!primaryTarget->GetAura(SpellMastery::SPELL_HUNTER_SERPENT_STING_RANK_1, _playerCaster->GetGUID()))
            return false;

        uint8 spreadCount = 0;
        std::list<Unit*> nearbyUnits;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(primaryTarget, _playerCaster, SERPENT_STING_GOLD_SPREAD_RADIUS);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(primaryTarget, nearbyUnits, check);
        Cell::VisitObjects(primaryTarget, searcher, SERPENT_STING_GOLD_SPREAD_RADIUS);

        for (Unit* spreadTarget : nearbyUnits)
        {
            if (!spreadTarget || spreadTarget == primaryTarget || !_playerCaster->IsValidAttackTarget(spreadTarget) || !spreadTarget->IsAlive())
                continue;

            if (spreadTarget->GetAura(SpellMastery::SPELL_HUNTER_SERPENT_STING_RANK_1, _playerCaster->GetGUID()))
                continue;

            _playerCaster->CastSpell(spreadTarget, SpellMastery::SPELL_HUNTER_SERPENT_STING_RANK_1, TRIGGERED_FULL_MASK);

            if (++spreadCount >= _effects.GoldSpreadTargets)
                break;
        }

        return true;
    }

    void TryApplyDiamondBurst(Unit* primaryTarget, int32 hitDamage)
    {
        if (!primaryTarget || hitDamage <= 0 || _effects.DiamondBurstDamagePct <= 0.0f)
            return;

        int32 const burstDamage = std::max<int32>(1, int32(std::lround((float(hitDamage) * _effects.DiamondBurstDamagePct) / 100.0f)));
        std::list<Unit*> nearbyUnits;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(primaryTarget, _playerCaster, VOLLEY_DIAMOND_BURST_RADIUS);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(primaryTarget, nearbyUnits, check);
        Cell::VisitObjects(primaryTarget, searcher, VOLLEY_DIAMOND_BURST_RADIUS);

        for (Unit* burstTarget : nearbyUnits)
        {
            if (!burstTarget || !_playerCaster->IsValidAttackTarget(burstTarget) || !burstTarget->IsAlive())
                continue;

            SpellNonMeleeDamage burstInfo(_playerCaster, burstTarget, GetSpellInfo(), GetSpellInfo()->SchoolMask);
            burstInfo.damage = burstDamage;
            _playerCaster->SendSpellNonMeleeDamageLog(&burstInfo);
            _playerCaster->DealSpellDamage(&burstInfo, false);
        }
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_hun_volley_trigger_mastery::HandleOnHit);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    VolleyMasteryEffects _effects;
    bool _goldSpreadApplied = false;
};

class spell_hun_serpent_sting_mastery : public SpellScript
{
    PrepareSpellScript(spell_hun_serpent_sting_mastery);

    bool Load() override
    {
        if (!GetCaster() || !GetCaster()->IsPlayer())
            return false;

        _playerCaster = GetCaster()->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SpellMastery::SPELL_HUNTER_SERPENT_STING_RANK_1)
            return false;

        SpellMastery::SpellMasteryProgress const& progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildSerpentStingMasteryEffects(progress, *_config);
        _isTriggeredCast = GetSpell()->IsTriggered();
        return true;
    }

    void HandleAfterHit()
    {
        Unit* target = GetHitUnit();
        if (!target || !_playerCaster->IsHostileTo(target))
            return;

        if (!_xpAwarded && !_isTriggeredCast && SpellMastery::ShouldAwardSpellMasteryXp(_playerCaster, *_config, SERPENT_STING_XP_GUARD_MS))
        {
            SpellMastery::AddSpellMasteryXp(_playerCaster, *_config, SpellMastery::SPELL_MASTERY_XP_PER_HIT);
            _xpAwarded = true;
        }

        if (_effects.IronManaRegenPct <= 0.0f || !_playerCaster->HasActivePowerType(POWER_MANA))
            return;

        int32 const maxMana = int32(_playerCaster->GetMaxPower(POWER_MANA));
        if (maxMana <= 0)
            return;

        int32 const manaRegen = std::max<int32>(1, int32(std::lround(float(maxMana) * (_effects.IronManaRegenPct / 100.0f))));
        _playerCaster->ModifyPower(POWER_MANA, manaRegen);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_hun_serpent_sting_mastery::HandleAfterHit);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    SerpentStingMasteryEffects _effects;
    bool _isTriggeredCast = false;
    bool _xpAwarded = false;
};

class spell_hun_serpent_sting_mastery_aura : public AuraScript
{
    PrepareAuraScript(spell_hun_serpent_sting_mastery_aura);

    bool Load() override
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->IsPlayer() || !GetUnitOwner())
            return false;

        _playerCaster = caster->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SpellMastery::SPELL_HUNTER_SERPENT_STING_RANK_1)
            return false;

        SpellMastery::SpellMasteryProgress const& progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildSerpentStingMasteryEffects(progress, *_config);
        return true;
    }

    void CalculatePeriodicDamageAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        if (!_playerCaster || amount <= 0)
            return;

        amount = SpellMastery::ApplyEarlyAccessSpellScale(_playerCaster, GetSpellInfo(), amount);

        if (_effects.BronzeDamageBonusPct > 0.0f)
        {
            int32 const scaledAmount = int32(std::lround(float(amount) * (1.0f + (_effects.BronzeDamageBonusPct / 100.0f))));
            amount = std::max(amount, scaledAmount);
        }

        // Gear/level growth: add a direct ranged attack power tick contribution.
        float const rangedAttackPower = std::max(0.0f, _playerCaster->GetTotalAttackPowerValue(RANGED_ATTACK));
        int32 const rangedApBonus = std::max<int32>(0, int32(std::lround(rangedAttackPower * SERPENT_STING_RANGED_AP_TICK_SCALAR)));
        amount = std::max<int32>(1, amount + rangedApBonus);
    }

    void CalculatePeriodicTiming(AuraEffect const* /*aurEff*/, bool& isPeriodic, int32& amplitude)
    {
        if (_effects.SilverTickIntervalMs <= 0)
            return;

        isPeriodic = true;
        amplitude = _effects.SilverTickIntervalMs;
    }

    void HandlePeriodicUpdate(AuraEffect* aurEff)
    {
        if (!aurEff || _effects.SilverTickIntervalMs <= 0)
            return;

        if (aurEff->GetPeriodicTimer() > _effects.SilverTickIntervalMs)
            aurEff->SetPeriodicTimer(_effects.SilverTickIntervalMs);
    }

    void HandlePeriodicTick(AuraEffect const* aurEff)
    {
        if (!_playerCaster || !aurEff || _effects.GoldSpreadTargets == 0 || _effects.GoldSpreadDamagePct <= 0.0f)
            return;

        Unit* primaryTarget = GetUnitOwner();
        if (!primaryTarget || !primaryTarget->IsAlive())
            return;

        int32 const tickDamage = std::max<int32>(1, aurEff->GetAmount());
        int32 const spreadDamage = std::max<int32>(1, int32(std::lround(float(tickDamage) * (_effects.GoldSpreadDamagePct / 100.0f))));

        uint8 spreadCount = 0;
        std::list<Unit*> nearbyUnits;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(primaryTarget, _playerCaster, SERPENT_STING_GOLD_SPREAD_RADIUS);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(primaryTarget, nearbyUnits, check);
        Cell::VisitObjects(primaryTarget, searcher, SERPENT_STING_GOLD_SPREAD_RADIUS);

        for (Unit* candidate : nearbyUnits)
        {
            if (!candidate || candidate == primaryTarget || !_playerCaster->IsValidAttackTarget(candidate) || !candidate->IsAlive())
                continue;

            SpellNonMeleeDamage spreadInfo(_playerCaster, candidate, GetSpellInfo(), GetSpellInfo()->SchoolMask);
            spreadInfo.damage = spreadDamage;
            _playerCaster->SendSpellNonMeleeDamageLog(&spreadInfo);
            _playerCaster->DealSpellDamage(&spreadInfo, false);

            if (++spreadCount >= _effects.GoldSpreadTargets)
                break;
        }
    }

    void HandleEffectRemove(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        if (!_playerCaster || !aurEff || _effects.DiamondDetonationDamagePct <= 0.0f)
            return;

        AuraApplication const* app = GetTargetApplication();
        if (!app || app->GetRemoveMode() != AURA_REMOVE_BY_EXPIRE)
            return;

        Unit* primaryTarget = GetUnitOwner();
        if (!primaryTarget || !_playerCaster->IsValidAttackTarget(primaryTarget))
            return;

        int32 const tickAmount = std::max<int32>(1, aurEff->GetAmount());
        int32 const detonationDamage = std::max<int32>(1, int32(std::lround(float(tickAmount) * (_effects.DiamondDetonationDamagePct / 100.0f))));

        std::list<Unit*> nearbyUnits;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(primaryTarget, _playerCaster, SERPENT_STING_DIAMOND_DETONATION_RADIUS);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(primaryTarget, nearbyUnits, check);
        Cell::VisitObjects(primaryTarget, searcher, SERPENT_STING_DIAMOND_DETONATION_RADIUS);

        for (Unit* target : nearbyUnits)
        {
            if (!target || !_playerCaster->IsValidAttackTarget(target) || !target->IsAlive())
                continue;

            SpellNonMeleeDamage detonationInfo(_playerCaster, target, GetSpellInfo(), GetSpellInfo()->SchoolMask);
            detonationInfo.damage = detonationDamage;
            _playerCaster->SendSpellNonMeleeDamageLog(&detonationInfo);
            _playerCaster->DealSpellDamage(&detonationInfo, false);
        }
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_hun_serpent_sting_mastery_aura::CalculatePeriodicDamageAmount, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE);
        DoEffectCalcPeriodic += AuraEffectCalcPeriodicFn(spell_hun_serpent_sting_mastery_aura::CalculatePeriodicTiming, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE);
        OnEffectUpdatePeriodic += AuraEffectUpdatePeriodicFn(spell_hun_serpent_sting_mastery_aura::HandlePeriodicUpdate, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_hun_serpent_sting_mastery_aura::HandlePeriodicTick, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE);
        OnEffectRemove += AuraEffectRemoveFn(spell_hun_serpent_sting_mastery_aura::HandleEffectRemove, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    SerpentStingMasteryEffects _effects;
};

class spell_hun_aimed_shot_mastery : public SpellScript
{
    PrepareSpellScript(spell_hun_aimed_shot_mastery);

    bool Load() override
    {
        if (!GetCaster() || !GetCaster()->IsPlayer())
            return false;

        _playerCaster = GetCaster()->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SpellMastery::SPELL_HUNTER_AIMED_SHOT_RANK_1)
            return false;

        SpellMastery::SpellMasteryProgress const& progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildAimedShotMasteryEffects(progress, *_config);
        _isTriggeredCast = GetSpell()->IsTriggered();

        if (_isTriggeredCast)
        {
            PendingAimedShotReleaseState pending;
            if (PopPendingAimedShotReleaseState(uint32(_playerCaster->GetGUID().GetCounter()), pending))
            {
                _chargeRatio = std::clamp(pending.ChargeRatio, AIMED_SHOT_MIN_CHARGE_RATIO, 1.0f);
                _manualManaCost = pending.ManualManaCost;
            }
        }

        return true;
    }

    void HandleBeforeHit(SpellMissInfo /*missInfo*/)
    {
        Unit* target = GetHitUnit();
        if (!target || !_playerCaster->IsValidAttackTarget(target))
            return;

        EnsureChargeData();
        if (_chargeRatio < 0.5f || _effects.SilverCritAtHalfChargePct <= 0.0f)
            return;

        if (roll_chance_f(_effects.SilverCritAtHalfChargePct))
            GetSpell()->SetSpellValue(SPELLVALUE_FORCED_CRIT_RESULT, 1);
    }

    void HandleOnHit()
    {
        Unit* target = GetHitUnit();
        if (!target || !_playerCaster->IsValidAttackTarget(target))
            return;

        int32 hitDamage = GetHitDamage();
        if (hitDamage <= 0)
            return;

        EnsureChargeData();

        hitDamage = SpellMastery::ApplyEarlyAccessSpellScale(_playerCaster, GetSpellInfo(), hitDamage);

        float const chargeDamagePct = 20.0f + (180.0f * _chargeRatio); // 20% -> 200%
        int32 const chargeScaled = int32(std::lround(float(hitDamage) * (chargeDamagePct / 100.0f)));
        hitDamage = std::max<int32>(1, chargeScaled);

        if (_effects.IronDamageBonusPct > 0.0f)
        {
            int32 const scaled = int32(std::lround(float(hitDamage) * (1.0f + (_effects.IronDamageBonusPct / 100.0f))));
            hitDamage = std::max(hitDamage, scaled);
        }

        if (_chargeRatio >= 0.7f && _effects.GoldHighChargeDamagePct > 0.0f)
        {
            int32 const scaled = int32(std::lround(float(hitDamage) * (1.0f + (_effects.GoldHighChargeDamagePct / 100.0f))));
            hitDamage = std::max(hitDamage, scaled);
        }

        SetHitDamage(hitDamage);
        _finalHitDamage = hitDamage;

        ApplyManaCost();

        if (!_xpAwarded && SpellMastery::ShouldAwardSpellMasteryXp(_playerCaster, *_config, AIMED_SHOT_XP_GUARD_MS))
        {
            SpellMastery::AddSpellMasteryXp(_playerCaster, *_config, SpellMastery::SPELL_MASTERY_XP_PER_HIT);
            _xpAwarded = true;
        }

        TryApplyDiamondSplash(target, hitDamage);
    }

    void EnsureChargeData()
    {
        if (_chargeDataInitialized)
            return;

        _chargeDataInitialized = true;

        if (_chargeRatio > 0.0f)
            return;

        AimedShotChargeState chargeState;
        if (PopAimedShotChargeState(uint32(_playerCaster->GetGUID().GetCounter()), chargeState))
        {
            uint32 const nowMs = uint32(GameTime::GetGameTimeMS().count());
            uint32 const elapsedMs = (nowMs >= chargeState.StartedAtMs) ? (nowMs - chargeState.StartedAtMs) : 0;
            _chargeRatio = ComputeAimedShotChargeRatio(elapsedMs, chargeState.CastWindowMs);
            _baseManaCost = chargeState.BaseManaCost;
            return;
        }

        _chargeRatio = 1.0f;
        _baseManaCost = uint32(std::max<int32>(0, GetSpell()->GetPowerCost()));
    }

    void ApplyManaCost()
    {
        if (_manaApplied || !_playerCaster || !_playerCaster->HasActivePowerType(POWER_MANA))
            return;

        EnsureChargeData();

        uint32 totalCost = _manualManaCost;
        if (!totalCost)
            totalCost = ComputeAimedShotTotalManaCost(_baseManaCost, _chargeRatio, _effects);

        int32 manaToSpend = 0;
        if (_manualManaCost > 0)
            manaToSpend = int32(totalCost);
        else
            manaToSpend = int32((totalCost > _baseManaCost) ? (totalCost - _baseManaCost) : 0);

        if (manaToSpend > 0)
            _playerCaster->ModifyPower(POWER_MANA, -manaToSpend);

        _manaApplied = true;
    }

    void TryApplyDiamondSplash(Unit* primaryTarget, int32 hitDamage)
    {
        if (!primaryTarget || hitDamage <= 0 || _effects.DiamondSplashDamagePct <= 0.0f || _chargeRatio < 0.9f)
            return;

        int32 const splashDamage = std::max<int32>(1, int32(std::lround((float(hitDamage) * _effects.DiamondSplashDamagePct) / 100.0f)));
        std::list<Unit*> nearbyUnits;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(primaryTarget, _playerCaster, AIMED_SHOT_DIAMOND_SPLASH_RADIUS);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(primaryTarget, nearbyUnits, check);
        Cell::VisitObjects(primaryTarget, searcher, AIMED_SHOT_DIAMOND_SPLASH_RADIUS);

        for (Unit* splashTarget : nearbyUnits)
        {
            if (!splashTarget || splashTarget == primaryTarget || !_playerCaster->IsValidAttackTarget(splashTarget) || !splashTarget->IsAlive())
                continue;

            SpellNonMeleeDamage splashInfo(_playerCaster, splashTarget, GetSpellInfo(), GetSpellInfo()->SchoolMask);
            splashInfo.damage = splashDamage;
            _playerCaster->SendSpellNonMeleeDamageLog(&splashInfo);
            _playerCaster->DealSpellDamage(&splashInfo, false);
        }
    }

    void Register() override
    {
        BeforeHit += BeforeSpellHitFn(spell_hun_aimed_shot_mastery::HandleBeforeHit);
        OnHit += SpellHitFn(spell_hun_aimed_shot_mastery::HandleOnHit);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    AimedShotMasteryEffects _effects;
    bool _isTriggeredCast = false;
    bool _chargeDataInitialized = false;
    bool _manaApplied = false;
    bool _xpAwarded = false;
    float _chargeRatio = 0.0f;
    uint32 _baseManaCost = 0;
    uint32 _manualManaCost = 0;
    int32 _finalHitDamage = 0;
};

class spell_mastery_hunter_aimed_charge_script : public AllSpellScript
{
public:
    spell_mastery_hunter_aimed_charge_script() : AllSpellScript("spell_mastery_hunter_aimed_charge_script", { ALLSPELLHOOK_ON_PREPARE, ALLSPELLHOOK_ON_CAST_CANCEL })
    {
    }

    void OnSpellPrepare(Spell* spell, Unit* caster, SpellInfo const* spellInfo) override
    {
        if (!spell || !caster || !spellInfo || !caster->IsPlayer())
            return;

        Player* player = caster->ToPlayer();
        if (!player || player->getClass() != CLASS_HUNTER || spell->IsTriggered())
            return;

        SpellMastery::ManagedSpellConfig const* config = SpellMastery::GetManagedSpellConfigForSpell(spellInfo->Id);
        if (!config || config->BaseSpellId != SpellMastery::SPELL_HUNTER_AIMED_SHOT_RANK_1)
            return;

        int32 const currentCastTime = spell->GetCastTime();
        if (currentCastTime <= 0 || currentCastTime < int32(AIMED_SHOT_CHARGE_WINDOW_MS))
            spell->SetSpellMasteryCastTime(int32(AIMED_SHOT_CHARGE_WINDOW_MS));

        BeginAimedShotCharge(player, spell);
    }

    void OnSpellCastCancel(Spell* spell, Unit* caster, SpellInfo const* spellInfo, bool bySelf) override
    {
        if (!spell || !caster || !spellInfo || !caster->IsPlayer() || spell->IsTriggered())
            return;

        Player* player = caster->ToPlayer();
        if (!player || player->getClass() != CLASS_HUNTER)
            return;

        SpellMastery::ManagedSpellConfig const* config = SpellMastery::GetManagedSpellConfigForSpell(spellInfo->Id);
        if (!config || config->BaseSpellId != SpellMastery::SPELL_HUNTER_AIMED_SHOT_RANK_1)
            return;

        AimedShotChargeState chargeState;
        if (!PopAimedShotChargeState(uint32(player->GetGUID().GetCounter()), chargeState))
            return;

        if (!bySelf)
            return;

        Unit* target = ObjectAccessor::GetUnit(*player, chargeState.TargetGuid);
        if (!target)
            target = spell->m_targets.GetUnitTarget();

        if (!target || !player->IsValidAttackTarget(target))
            return;

        SpellMastery::SpellMasteryProgress const& progress = SpellMastery::GetOrLoadSpellMasteryProgress(player, *config);
        AimedShotMasteryEffects const effects = BuildAimedShotMasteryEffects(progress, *config);

        uint32 const nowMs = uint32(GameTime::GetGameTimeMS().count());
        uint32 const elapsedMs = (nowMs >= chargeState.StartedAtMs) ? (nowMs - chargeState.StartedAtMs) : 0;
        float const chargeRatio = ComputeAimedShotChargeRatio(elapsedMs, chargeState.CastWindowMs);
        uint32 const manualManaCost = ComputeAimedShotTotalManaCost(chargeState.BaseManaCost, chargeRatio, effects);

        SetPendingAimedShotReleaseState(uint32(player->GetGUID().GetCounter()), PendingAimedShotReleaseState{ chargeRatio, manualManaCost });
        player->CastSpell(
            target,
            spellInfo->Id,
            TriggerCastFlags(TRIGGERED_IGNORE_GCD | TRIGGERED_IGNORE_CAST_IN_PROGRESS | TRIGGERED_IGNORE_POWER_AND_REAGENT_COST | TRIGGERED_IGNORE_SPELL_AND_CATEGORY_CD | TRIGGERED_CAST_DIRECTLY));
    }
};

void AddSC_spell_mastery_hunter()
{
    new spell_mastery_hunter_aimed_charge_script();
    RegisterSpellAndAuraScriptPair(spell_hun_volley_mastery, spell_hun_volley_mastery_aura);
    RegisterSpellScript(spell_hun_volley_trigger_mastery);
    RegisterSpellScript(spell_hun_aimed_shot_mastery);
    RegisterSpellAndAuraScriptPair(spell_hun_serpent_sting_mastery, spell_hun_serpent_sting_mastery_aura);
}
