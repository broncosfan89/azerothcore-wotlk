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

#include "Player.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace
{
struct KillingSpreeMasteryEffects
{
    uint8 IronAttackCount = 5;
    float BronzeDamageBonusPct = 0.0f;
    int32 SilverCooldownReductionMs = 0;
    float GoldBleedPct = 0.0f;
    float DiamondExtraStrikeChancePct = 0.0f;
};

struct FanOfKnivesMasteryEffects
{
    int32 IronEnergyRefund = 0;
    float DamageBonusPct = 0.0f;
    float SilverRadiusMultiplier = 1.0f;
    uint8 GoldComboPoints = 0;
    bool DiamondApplyPoison = false;
};

struct RuptureMasteryEffects
{
    float IronDamageBonusPct = 0.0f;
    int32 BronzeTickIntervalMs = 2000;
    float SilverHealPctOfTickDamage = 0.0f;
    int32 GoldDurationBonusMs = 0;
    bool DiamondFullDamageAtOneComboPoint = false;
};

uint64 constexpr KILLING_SPREE_XP_PER_USE = 500;
uint32 constexpr KILLING_SPREE_XP_GUARD_MS = 250;
uint32 constexpr KILLING_SPREE_BASE_ATTACK_COUNT = 5;
uint32 constexpr KILLING_SPREE_BASE_COOLDOWN_MS = 90000;
uint32 constexpr KILLING_SPREE_SILVER_COOLDOWN_MS = 45000;
uint32 constexpr FAN_OF_KNIVES_XP_GUARD_MS = 250;
uint32 constexpr RUPTURE_XP_GUARD_MS = 250;
int32 constexpr RUPTURE_BASE_TICK_INTERVAL_MS = 2000;
int32 constexpr RUPTURE_MIN_TICK_INTERVAL_MS = 500;

std::unordered_map<uint32, uint8> KillingSpreePendingDiamondExtra;
std::mutex KillingSpreePendingDiamondExtraMutex;

KillingSpreeMasteryEffects BuildKillingSpreeMasteryEffects(SpellMastery::SpellMasteryProgress const& progress, SpellMastery::ManagedSpellConfig const& config)
{
    KillingSpreeMasteryEffects effects;

    uint8 ironLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_IRON, config);
    uint8 bronzeLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_BRONZE, config);
    uint8 silverLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_SILVER, config);
    uint8 goldLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_GOLD, config);
    uint8 diamondLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_DIAMOND, config);
    uint32 const totalMasteryLevels = uint32(ironLevel) + uint32(bronzeLevel) + uint32(silverLevel) + uint32(goldLevel) + uint32(diamondLevel);

    // Iron: increase total attacks from 5 up to 15.
    if (ironLevel > 0)
        effects.IronAttackCount = uint8(std::min<uint32>(15, KILLING_SPREE_BASE_ATTACK_COUNT + ironLevel));

    // Bronze: increase strike damage.
    effects.BronzeDamageBonusPct = float(totalMasteryLevels) * 8.0f;

    // Silver: reduce cooldown to 45 seconds.
    if (silverLevel > 0)
        effects.SilverCooldownReductionMs = int32(KILLING_SPREE_BASE_COOLDOWN_MS - KILLING_SPREE_SILVER_COOLDOWN_MS);

    // Gold: apply a bleed from each hit.
    if (goldLevel > 0)
        effects.GoldBleedPct = 20.0f + (float(goldLevel - 1) * (30.0f / 9.0f)); // 20% -> 50%

    // Diamond: each hit has a chance to trigger an extra Killing Spree strike.
    if (diamondLevel > 0)
        effects.DiamondExtraStrikeChancePct = 5.0f + (float(diamondLevel - 1) * (25.0f / 9.0f)); // 5% -> 30%

    return effects;
}

FanOfKnivesMasteryEffects BuildFanOfKnivesMasteryEffects(SpellMastery::SpellMasteryProgress const& progress, SpellMastery::ManagedSpellConfig const& config)
{
    FanOfKnivesMasteryEffects effects;

    uint8 const ironLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_IRON, config);
    uint8 const bronzeLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_BRONZE, config);
    uint8 const silverLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_SILVER, config);
    uint8 const goldLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_GOLD, config);
    uint8 const diamondLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_DIAMOND, config);
    uint32 const totalMasteryLevels = uint32(ironLevel) + uint32(bronzeLevel) + uint32(silverLevel) + uint32(goldLevel) + uint32(diamondLevel);

    // Iron: reduce effective energy cost from 50 down to 20 (refund up to 30 energy).
    if (ironLevel > 0)
        effects.IronEnergyRefund = int32(ironLevel) * 3;

    // Bronze through Diamond: +4% damage per mastery level.
    if (totalMasteryLevels > 0)
        effects.DamageBonusPct = float(totalMasteryLevels) * 4.0f;

    // Silver: increase range from 8 yards to 20 yards at Silver 10.
    if (silverLevel > 0)
        effects.SilverRadiusMultiplier += 1.5f * (float(silverLevel) / 10.0f);

    // Gold: generate combo points on first valid hit each cast.
    if (goldLevel > 0)
        effects.GoldComboPoints = uint8(std::min<uint32>(5, goldLevel));

    // Diamond: apply poison to every hit target.
    effects.DiamondApplyPoison = diamondLevel > 0;

    return effects;
}

RuptureMasteryEffects BuildRuptureMasteryEffects(SpellMastery::SpellMasteryProgress const& progress, SpellMastery::ManagedSpellConfig const& config)
{
    RuptureMasteryEffects effects;

    uint8 const ironLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_IRON, config);
    uint8 const bronzeLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_BRONZE, config);
    uint8 const silverLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_SILVER, config);
    uint8 const goldLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_GOLD, config);
    uint8 const diamondLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_DIAMOND, config);
    uint32 const totalMasteryLevels = uint32(ironLevel) + uint32(bronzeLevel) + uint32(silverLevel) + uint32(goldLevel) + uint32(diamondLevel);

    if (totalMasteryLevels > 0)
        effects.IronDamageBonusPct = float(totalMasteryLevels) * 5.0f;

    if (bronzeLevel > 0)
    {
        int32 const intervalRange = RUPTURE_BASE_TICK_INTERVAL_MS - RUPTURE_MIN_TICK_INTERVAL_MS;
        int32 const reduction = int32((int64(intervalRange) * int64(bronzeLevel)) / 10);
        effects.BronzeTickIntervalMs = std::max<int32>(RUPTURE_MIN_TICK_INTERVAL_MS, RUPTURE_BASE_TICK_INTERVAL_MS - reduction);
    }

    if (silverLevel > 0)
        effects.SilverHealPctOfTickDamage = float(silverLevel) * 2.0f;

    if (goldLevel > 0)
        effects.GoldDurationBonusMs = int32(goldLevel) * 500;

    effects.DiamondFullDamageAtOneComboPoint = diamondLevel > 0;
    return effects;
}
}

void ClearSpellMasteryRogueRuntimeStateForPlayer(uint32 guid)
{
    std::lock_guard<std::mutex> lock(KillingSpreePendingDiamondExtraMutex);
    KillingSpreePendingDiamondExtra.erase(guid);
}

class spell_rog_killing_spree_mastery : public SpellScript
{
    PrepareSpellScript(spell_rog_killing_spree_mastery);

    bool Load() override
    {
        if (!GetCaster() || !GetCaster()->IsPlayer())
            return false;

        _playerCaster = GetCaster()->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SpellMastery::SPELL_ROGUE_KILLING_SPREE)
            return false;

        SpellMastery::SpellMasteryProgress const& progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildKillingSpreeMasteryEffects(progress, *_config);
        return true;
    }

    void HandleAfterCast()
    {
        if (!_xpAwarded && SpellMastery::ShouldAwardSpellMasteryXp(_playerCaster, *_config, KILLING_SPREE_XP_GUARD_MS))
        {
            SpellMastery::AddSpellMasteryXp(_playerCaster, *_config, KILLING_SPREE_XP_PER_USE);
            _xpAwarded = true;
        }

        if (Aura* spreeAura = _playerCaster->GetAura(_config->AllowedSpellId))
        {
            int32 const baseDuration = spreeAura->GetMaxDuration();
            if (baseDuration > 0 && _effects.IronAttackCount > KILLING_SPREE_BASE_ATTACK_COUNT)
            {
                int32 const scaledDuration = int32(std::lround(
                    (float(baseDuration) * float(_effects.IronAttackCount)) / float(KILLING_SPREE_BASE_ATTACK_COUNT)));
                int32 const newDuration = std::max(baseDuration, scaledDuration);
                spreeAura->SetMaxDuration(newDuration);
                spreeAura->SetDuration(newDuration);
            }
        }

        if (_effects.SilverCooldownReductionMs > 0)
            _playerCaster->ModifySpellCooldown(_config->AllowedSpellId, -_effects.SilverCooldownReductionMs);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_rog_killing_spree_mastery::HandleAfterCast);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    KillingSpreeMasteryEffects _effects;
    bool _xpAwarded = false;
};

class spell_rog_killing_spree_weapon_mastery : public SpellScript
{
    PrepareSpellScript(spell_rog_killing_spree_weapon_mastery);

    bool Load() override
    {
        if (!GetCaster() || !GetCaster()->IsPlayer())
            return false;

        _playerCaster = GetCaster()->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigByBaseSpell(SpellMastery::SPELL_ROGUE_KILLING_SPREE);
        if (!_config)
            return false;

        SpellMastery::SpellMasteryProgress const& progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildKillingSpreeMasteryEffects(progress, *_config);
        _casterGuidLow = uint32(_playerCaster->GetGUID().GetCounter());

        {
            std::lock_guard<std::mutex> lock(KillingSpreePendingDiamondExtraMutex);
            auto itr = KillingSpreePendingDiamondExtra.find(_casterGuidLow);
            if (itr != KillingSpreePendingDiamondExtra.end() && itr->second > 0)
            {
                _isDiamondExtraCast = true;
                if (--itr->second == 0)
                    KillingSpreePendingDiamondExtra.erase(itr);
            }
        }

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

        if (_effects.BronzeDamageBonusPct > 0.0f)
        {
            int32 const scaledDamage = int32(std::lround(float(hitDamage) * (1.0f + (_effects.BronzeDamageBonusPct / 100.0f))));
            hitDamage = std::max(hitDamage, scaledDamage);
        }

        SetHitDamage(hitDamage);
        TryApplyGoldBleed(target, hitDamage);
        TryTriggerDiamondExtraHit(target);
    }

    void TryApplyGoldBleed(Unit* target, int32 hitDamage)
    {
        if (!target || _effects.GoldBleedPct <= 0.0f || hitDamage <= 0)
            return;

        SpellInfo const* bleedInfo = sSpellMgr->GetSpellInfo(SpellMastery::SPELL_WARRIOR_REND_RANK_1);
        if (!bleedInfo || !bleedInfo->GetMaxTicks())
            return;

        int32 const bleedTotal = int32(std::lround((float(hitDamage) * _effects.GoldBleedPct) / 100.0f));
        int32 const bleedPerTick = std::max<int32>(1, bleedTotal / int32(bleedInfo->GetMaxTicks()));

        _playerCaster->CastCustomSpell(
            SpellMastery::SPELL_WARRIOR_REND_RANK_1,
            SPELLVALUE_BASE_POINT0,
            bleedPerTick,
            target,
            TRIGGERED_FULL_MASK,
            nullptr,
            nullptr,
            _playerCaster->GetGUID());
    }

    void TryTriggerDiamondExtraHit(Unit* target)
    {
        if (!target || _isDiamondExtraCast || _effects.DiamondExtraStrikeChancePct <= 0.0f)
            return;

        if (!roll_chance_f(_effects.DiamondExtraStrikeChancePct))
            return;

        {
            std::lock_guard<std::mutex> lock(KillingSpreePendingDiamondExtraMutex);
            ++KillingSpreePendingDiamondExtra[_casterGuidLow];
        }
        _playerCaster->CastSpell(target, SpellMastery::SPELL_ROGUE_KILLING_SPREE_WEAPON_DMG, TRIGGERED_FULL_MASK);
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_rog_killing_spree_weapon_mastery::HandleOnHit);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    KillingSpreeMasteryEffects _effects;
    uint32 _casterGuidLow = 0;
    bool _isDiamondExtraCast = false;
};

class spell_rog_fan_of_knives_mastery : public SpellScript
{
    PrepareSpellScript(spell_rog_fan_of_knives_mastery);

    bool Load() override
    {
        if (!GetCaster() || !GetCaster()->IsPlayer())
            return false;

        _playerCaster = GetCaster()->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SpellMastery::SPELL_ROGUE_FAN_OF_KNIVES_RANK_1)
            return false;

        SpellMastery::SpellMasteryProgress const& progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildFanOfKnivesMasteryEffects(progress, *_config);
        return true;
    }

    void HandleBeforeCast()
    {
        if (_effects.SilverRadiusMultiplier > 1.0f)
            GetSpell()->SetSpellValue(SPELLVALUE_RADIUS_MOD, int32(std::lround(_effects.SilverRadiusMultiplier * 10000.0f)));
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

        if (_effects.DamageBonusPct > 0.0f)
        {
            int32 const scaledDamage = int32(std::lround(float(hitDamage) * (1.0f + (_effects.DamageBonusPct / 100.0f))));
            hitDamage = std::max(hitDamage, scaledDamage);
        }

        SetHitDamage(hitDamage);

        if (!_xpAwarded && SpellMastery::ShouldAwardSpellMasteryXp(_playerCaster, *_config, FAN_OF_KNIVES_XP_GUARD_MS))
        {
            SpellMastery::AddSpellMasteryXp(_playerCaster, *_config, SpellMastery::SPELL_MASTERY_XP_PER_HIT);
            _xpAwarded = true;
        }

        if (!_comboPointsGranted && _effects.GoldComboPoints > 0)
        {
            Unit* comboTarget = _playerCaster->GetSelectedUnit();
            if (!comboTarget || !_playerCaster->IsValidAttackTarget(comboTarget))
                comboTarget = target;

            _playerCaster->AddComboPoints(comboTarget, int8(_effects.GoldComboPoints));
            _comboPointsGranted = true;
        }

        if (_effects.DiamondApplyPoison)
            _playerCaster->CastSpell(target, SpellMastery::SPELL_ROGUE_DEADLY_POISON, TRIGGERED_FULL_MASK);
    }

    void HandleAfterCast()
    {
        if (_effects.IronEnergyRefund > 0)
            _playerCaster->ModifyPower(POWER_ENERGY, _effects.IronEnergyRefund);
    }

    void Register() override
    {
        BeforeCast += SpellCastFn(spell_rog_fan_of_knives_mastery::HandleBeforeCast);
        OnHit += SpellHitFn(spell_rog_fan_of_knives_mastery::HandleOnHit);
        AfterCast += SpellCastFn(spell_rog_fan_of_knives_mastery::HandleAfterCast);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    FanOfKnivesMasteryEffects _effects;
    bool _xpAwarded = false;
    bool _comboPointsGranted = false;
};

class spell_rog_rupture_mastery : public SpellScript
{
    PrepareSpellScript(spell_rog_rupture_mastery);

    bool Load() override
    {
        if (!GetCaster() || !GetCaster()->IsPlayer())
            return false;

        _playerCaster = GetCaster()->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SpellMastery::SPELL_ROGUE_RUPTURE_RANK_1)
            return false;

        SpellMastery::SpellMasteryProgress const& progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildRuptureMasteryEffects(progress, *_config);
        _isTriggeredCast = GetSpell()->IsTriggered();

        if (_effects.GoldDurationBonusMs > 0)
        {
            int32 const baseDuration = GetSpellInfo()->GetMaxDuration();
            if (baseDuration > 0)
                GetSpell()->SetSpellValue(SPELLVALUE_AURA_DURATION, baseDuration + _effects.GoldDurationBonusMs);
        }

        return true;
    }

    void HandleAfterHit()
    {
        Unit* target = GetHitUnit();
        if (!target || !_playerCaster->IsHostileTo(target))
            return;

        if (!_xpAwarded && !_isTriggeredCast && SpellMastery::ShouldAwardSpellMasteryXp(_playerCaster, *_config, RUPTURE_XP_GUARD_MS))
        {
            SpellMastery::AddSpellMasteryXp(_playerCaster, *_config, SpellMastery::SPELL_MASTERY_XP_PER_HIT);
            _xpAwarded = true;
        }
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_rog_rupture_mastery::HandleAfterHit);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    RuptureMasteryEffects _effects;
    bool _isTriggeredCast = false;
    bool _xpAwarded = false;
};

class spell_rog_rupture_mastery_aura : public AuraScript
{
    PrepareAuraScript(spell_rog_rupture_mastery_aura);

    bool Load() override
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->IsPlayer() || !GetUnitOwner())
            return false;

        _playerCaster = caster->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SpellMastery::SPELL_ROGUE_RUPTURE_RANK_1)
            return false;

        SpellMastery::SpellMasteryProgress const& progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildRuptureMasteryEffects(progress, *_config);
        return true;
    }

    void CalculatePeriodicDamageAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        if (!_playerCaster || amount <= 0)
            return;

        amount = SpellMastery::ApplyEarlyAccessSpellScale(_playerCaster, GetSpellInfo(), amount);

        float totalBonusPct = _effects.IronDamageBonusPct;
        if (totalBonusPct > 0.0f)
        {
            int32 const scaledAmount = int32(std::lround(float(amount) * (1.0f + (totalBonusPct / 100.0f))));
            amount = std::max(amount, scaledAmount);
        }

        if (_effects.DiamondFullDamageAtOneComboPoint)
        {
            uint8 comboPoints = std::max<uint8>(1, _playerCaster->GetComboPoints());
            if (comboPoints < 5)
            {
                int32 const scaledForComboPoints = int32(std::lround(float(amount) * (5.0f / float(comboPoints))));
                amount = std::max(amount, scaledForComboPoints);
            }
        }
    }

    void CalculatePeriodicTiming(AuraEffect const* /*aurEff*/, bool& isPeriodic, int32& amplitude)
    {
        if (_effects.BronzeTickIntervalMs <= 0)
            return;

        isPeriodic = true;
        amplitude = _effects.BronzeTickIntervalMs;
    }

    void HandlePeriodicUpdate(AuraEffect* aurEff)
    {
        if (!aurEff || _effects.BronzeTickIntervalMs <= 0)
            return;

        if (aurEff->GetPeriodicTimer() > _effects.BronzeTickIntervalMs)
            aurEff->SetPeriodicTimer(_effects.BronzeTickIntervalMs);
    }

    void HandlePeriodicTick(AuraEffect const* aurEff)
    {
        if (!_playerCaster || !aurEff || _effects.SilverHealPctOfTickDamage <= 0.0f || !_playerCaster->IsAlive())
            return;

        Unit* target = GetTarget();
        if (!target || !_playerCaster->IsHostileTo(target))
            return;

        int32 const tickDamage = std::max<int32>(1, aurEff->GetAmount());
        int32 const healAmount = std::max<int32>(1, int32(std::lround((float(tickDamage) * _effects.SilverHealPctOfTickDamage) / 100.0f)));
        HealInfo healInfo(_playerCaster, _playerCaster, uint32(healAmount), GetSpellInfo(), GetSpellInfo()->GetSchoolMask());
        _playerCaster->HealBySpell(healInfo);
    }

    void HandleEffectApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (_playerCaster && _config)
        {
            Unit* target = GetUnitOwner();
            if (target && _playerCaster->IsHostileTo(target) && SpellMastery::ShouldAwardSpellMasteryXp(_playerCaster, *_config, RUPTURE_XP_GUARD_MS))
                SpellMastery::AddSpellMasteryXp(_playerCaster, *_config, SpellMastery::SPELL_MASTERY_XP_PER_HIT);
        }

        if (_effects.BronzeTickIntervalMs > 0)
        {
            if (Aura* aura = GetAura())
            {
                if (AuraEffect* periodic = aura->GetEffect(EFFECT_0))
                {
                    if (periodic->GetPeriodicTimer() > _effects.BronzeTickIntervalMs)
                        periodic->SetPeriodicTimer(_effects.BronzeTickIntervalMs);
                }
            }
        }
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_rog_rupture_mastery_aura::CalculatePeriodicDamageAmount, EFFECT_ALL, SPELL_AURA_PERIODIC_DAMAGE);
        DoEffectCalcPeriodic += AuraEffectCalcPeriodicFn(spell_rog_rupture_mastery_aura::CalculatePeriodicTiming, EFFECT_ALL, SPELL_AURA_PERIODIC_DAMAGE);
        OnEffectUpdatePeriodic += AuraEffectUpdatePeriodicFn(spell_rog_rupture_mastery_aura::HandlePeriodicUpdate, EFFECT_ALL, SPELL_AURA_PERIODIC_DAMAGE);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_rog_rupture_mastery_aura::HandlePeriodicTick, EFFECT_ALL, SPELL_AURA_PERIODIC_DAMAGE);
        OnEffectApply += AuraEffectApplyFn(spell_rog_rupture_mastery_aura::HandleEffectApply, EFFECT_ALL, SPELL_AURA_PERIODIC_DAMAGE, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    RuptureMasteryEffects _effects;
};

void AddSC_spell_mastery_rogue()
{
    RegisterSpellScript(spell_rog_killing_spree_mastery);
    RegisterSpellScript(spell_rog_killing_spree_weapon_mastery);
    RegisterSpellScript(spell_rog_fan_of_knives_mastery);
    RegisterSpellAndAuraScriptPair(spell_rog_rupture_mastery, spell_rog_rupture_mastery_aura);
}
