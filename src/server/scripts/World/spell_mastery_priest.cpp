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
#include "SpellScript.h"
#include "SpellScriptLoader.h"

#include <algorithm>
#include <cmath>
#include <list>

namespace
{
struct PowerWordShieldMasteryEffects
{
    float IronShieldBonusPct = 0.0f;
    float BronzeWeakenedSoulReductionPct = 0.0f;
    float SilverHotPctPerTick = 0.0f;
    float GoldReflectPct = 0.0f;
    float DiamondEndHealPct = 0.0f;
    float DiamondEndHealRadius = 0.0f;
};

uint32 constexpr PWS_XP_GUARD_MS = 250;

PowerWordShieldMasteryEffects BuildPowerWordShieldMasteryEffects(SpellMastery::SpellMasteryProgress const& progress, SpellMastery::ManagedSpellConfig const& config)
{
    PowerWordShieldMasteryEffects effects;

    uint8 const ironLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_IRON, config);
    uint8 const bronzeLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_BRONZE, config);
    uint8 const silverLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_SILVER, config);
    uint8 const goldLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_GOLD, config);
    uint8 const diamondLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_DIAMOND, config);

    if (ironLevel > 0)
        effects.IronShieldBonusPct = float(ironLevel) * 8.0f;

    if (bronzeLevel > 0)
        effects.BronzeWeakenedSoulReductionPct = std::min<float>(60.0f, float(bronzeLevel) * 6.0f);

    if (silverLevel > 0)
        effects.SilverHotPctPerTick = float(silverLevel) * 1.5f;

    if (goldLevel > 0)
        effects.GoldReflectPct = 5.0f + (float(goldLevel) * 3.0f);

    if (diamondLevel > 0)
    {
        effects.DiamondEndHealPct = std::min<float>(100.0f, 10.0f + (float(diamondLevel) * 9.0f));
        effects.DiamondEndHealRadius = 10.0f + float(diamondLevel);
    }

    return effects;
}

void HealNearbyFriendlyPlayers(Unit* center, int32 healAmount, float radius)
{
    if (!center || healAmount <= 0 || radius <= 0.0f)
        return;

    std::list<Unit*> units;
    Acore::AnyFriendlyUnitInObjectRangeCheck unitCheck(center, center, radius);
    Acore::UnitListSearcher<Acore::AnyFriendlyUnitInObjectRangeCheck> searcher(center, units, unitCheck);
    Cell::VisitObjects(center, searcher, radius);

    for (Unit* unit : units)
    {
        if (!unit || !unit->IsAlive() || !unit->IsPlayer())
            continue;

        if (!center->IsFriendlyTo(unit))
            continue;

        unit->ModifyHealth(healAmount);
    }
}
}

void ClearSpellMasteryPriestRuntimeStateForPlayer(uint32 /*guid*/)
{
}

class spell_pri_power_word_shield_mastery : public SpellScript
{
    PrepareSpellScript(spell_pri_power_word_shield_mastery);

    bool Load() override
    {
        if (!GetCaster() || !GetCaster()->IsPlayer())
            return false;

        _playerCaster = GetCaster()->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SpellMastery::SPELL_PRIEST_POWER_WORD_SHIELD_RANK_1)
            return false;

        _progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildPowerWordShieldMasteryEffects(_progress, *_config);
        _isTriggeredCast = GetSpell()->IsTriggered();
        return true;
    }

    void HandleOnHit()
    {
        Unit* target = GetHitUnit();
        if (!target || !_playerCaster->IsFriendlyTo(target))
            return;

        if (!_xpAwarded && !_isTriggeredCast && SpellMastery::ShouldAwardSpellMasteryXp(_playerCaster, *_config, PWS_XP_GUARD_MS))
        {
            SpellMastery::AddSpellMasteryXp(_playerCaster, *_config, SpellMastery::SPELL_MASTERY_XP_PER_HIT);
            _xpAwarded = true;
        }

        if (_effects.BronzeWeakenedSoulReductionPct > 0.0f && target->IsPlayer())
        {
            if (Aura* weakenedSoul = target->GetAura(SpellMastery::SPELL_PRIEST_WEAKENED_SOUL, _playerCaster->GetGUID()))
            {
                int32 const baseDuration = weakenedSoul->GetMaxDuration();
                int32 const reducedDuration = int32(std::lround(float(baseDuration) * (1.0f - (_effects.BronzeWeakenedSoulReductionPct / 100.0f))));
                int32 const clampedDuration = std::max<int32>(1000, reducedDuration);
                weakenedSoul->SetMaxDuration(clampedDuration);
                if (weakenedSoul->GetDuration() > clampedDuration)
                    weakenedSoul->SetDuration(clampedDuration);
            }
        }

        if (_effects.SilverHotPctPerTick > 0.0f)
        {
            if (Aura* shieldAura = target->GetAura(GetSpellInfo()->Id, _playerCaster->GetGUID()))
            {
                if (AuraEffect const* absorbEff = shieldAura->GetEffect(EFFECT_0))
                {
                    int32 const shieldAmount = std::max<int32>(1, absorbEff->GetAmount());
                    int32 const hotPerTick = std::max<int32>(1, int32(std::lround((float(shieldAmount) * _effects.SilverHotPctPerTick) / 100.0f)));
                    _playerCaster->CastCustomSpell(target, SpellMastery::SPELL_PRIEST_RENEW_RANK_1, &hotPerTick, nullptr, nullptr, true);
                }
            }
        }
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_pri_power_word_shield_mastery::HandleOnHit);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    SpellMastery::SpellMasteryProgress _progress;
    PowerWordShieldMasteryEffects _effects;
    bool _isTriggeredCast = false;
    bool _xpAwarded = false;
};

class spell_pri_power_word_shield_mastery_aura : public AuraScript
{
    PrepareAuraScript(spell_pri_power_word_shield_mastery_aura);

    bool Load() override
    {
        if (!GetCaster() || !GetCaster()->IsPlayer())
            return false;

        _playerCaster = GetCaster()->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SpellMastery::SPELL_PRIEST_POWER_WORD_SHIELD_RANK_1)
            return false;

        _progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildPowerWordShieldMasteryEffects(_progress, *_config);
        return true;
    }

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SpellMastery::SPELL_PRIEST_REFLECTIVE_SHIELD_TRIGGERED });
    }

    void HandleApply(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        if (!aurEff)
            return;

        _initialShieldAmount = std::max<int32>(_initialShieldAmount, aurEff->GetAmount());
    }

    void HandleCalculateAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        amount = SpellMastery::ApplyEarlyAccessSpellScale(_playerCaster, GetSpellInfo(), amount);

        if (_effects.IronShieldBonusPct <= 0.0f)
            return;

        int32 const scaledAmount = int32(std::lround(float(amount) * (1.0f + (_effects.IronShieldBonusPct / 100.0f))));
        amount = std::max(amount, scaledAmount);
    }

    void HandleAfterAbsorb(AuraEffect* aurEff, DamageInfo& damageInfo, uint32& absorbAmount)
    {
        _absorbedTotal += absorbAmount;

        if (_effects.GoldReflectPct <= 0.0f)
            return;

        Unit* target = GetTarget();
        Unit* attacker = damageInfo.GetAttacker();
        if (!target || !attacker || attacker == target || absorbAmount == 0)
            return;

        if (SpellInfo const* incomingSpell = damageInfo.GetSpellInfo())
            if (incomingSpell->Id == SpellMastery::SPELL_PRIEST_REFLECTIVE_SHIELD_TRIGGERED)
                return;

        int32 const reflectDamage = std::max<int32>(1, int32(std::lround((float(absorbAmount) * _effects.GoldReflectPct) / 100.0f)));
        target->CastCustomSpell(attacker, SpellMastery::SPELL_PRIEST_REFLECTIVE_SHIELD_TRIGGERED, &reflectDamage, nullptr, nullptr, true, nullptr, aurEff);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        if (!target || !target->IsAlive() || _effects.DiamondEndHealPct <= 0.0f || _effects.DiamondEndHealRadius <= 0.0f)
            return;

        int32 const shieldPool = std::max<int32>(_initialShieldAmount, int32(_absorbedTotal));
        if (shieldPool <= 0)
            return;

        int32 const healAmount = std::max<int32>(1, int32(std::lround((float(shieldPool) * _effects.DiamondEndHealPct) / 100.0f)));
        HealNearbyFriendlyPlayers(target, healAmount, _effects.DiamondEndHealRadius);
    }

    void Register() override
    {
        OnEffectApply += AuraEffectApplyFn(spell_pri_power_word_shield_mastery_aura::HandleApply, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_pri_power_word_shield_mastery_aura::HandleCalculateAmount, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
        AfterEffectAbsorb += AuraEffectAbsorbFn(spell_pri_power_word_shield_mastery_aura::HandleAfterAbsorb, EFFECT_0);
        AfterEffectRemove += AuraEffectRemoveFn(spell_pri_power_word_shield_mastery_aura::HandleRemove, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    SpellMastery::SpellMasteryProgress _progress;
    PowerWordShieldMasteryEffects _effects;
    int32 _initialShieldAmount = 0;
    uint32 _absorbedTotal = 0;
};

void AddSC_spell_mastery_priest()
{
    RegisterSpellScript(spell_pri_power_word_shield_mastery);
    RegisterSpellScript(spell_pri_power_word_shield_mastery_aura);
}
