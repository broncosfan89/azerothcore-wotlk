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
#include "Cell.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Player.h"
#include "SpellMgr.h"
#include "SpellAuras.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"

#include <algorithm>
#include <cmath>
#include <list>

namespace
{
uint32 constexpr SPELL_PRIEST_FLASH_HEAL_RANK_1 = 2061;

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

uint32 GetHighestKnownSpellInChain(Player* player, uint32 firstRankSpellId)
{
    if (!player || !firstRankSpellId)
        return firstRankSpellId;

    uint32 highestKnownSpellId = firstRankSpellId;
    for (uint32 spellId = firstRankSpellId; spellId; spellId = sSpellMgr->GetNextSpellInChain(spellId))
    {
        if (player->HasSpell(spellId))
            highestKnownSpellId = spellId;
    }

    return highestKnownSpellId;
}

void ApplyBronzeWeakenedSoulReduction(Unit* target, Player* caster, PowerWordShieldMasteryEffects const& effects)
{
    if (!target || !caster || effects.BronzeWeakenedSoulReductionPct <= 0.0f || !target->IsPlayer())
        return;

    Aura* weakenedSoul = target->GetAura(SpellMastery::SPELL_PRIEST_WEAKENED_SOUL, caster->GetGUID());
    if (!weakenedSoul)
        weakenedSoul = target->GetAura(SpellMastery::SPELL_PRIEST_WEAKENED_SOUL);

    if (!weakenedSoul)
        return;

    int32 const baseDuration = weakenedSoul->GetMaxDuration();
    int32 const reducedDuration = int32(std::lround(float(baseDuration) * (1.0f - (effects.BronzeWeakenedSoulReductionPct / 100.0f))));
    int32 const clampedDuration = std::max<int32>(1000, reducedDuration);
    weakenedSoul->SetMaxDuration(clampedDuration);
    if (weakenedSoul->GetDuration() > clampedDuration)
        weakenedSoul->SetDuration(clampedDuration);
}

PowerWordShieldMasteryEffects BuildPowerWordShieldMasteryEffects(SpellMastery::SpellMasteryProgress const& progress, SpellMastery::ManagedSpellConfig const& config)
{
    PowerWordShieldMasteryEffects effects;

    uint8 const ironLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_IRON, config);
    uint8 const bronzeLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_BRONZE, config);
    uint8 const silverLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_SILVER, config);
    uint8 const goldLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_GOLD, config);
    uint8 const diamondLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_DIAMOND, config);
    uint32 const totalMasteryLevels = uint32(ironLevel) + uint32(bronzeLevel) + uint32(silverLevel) + uint32(goldLevel) + uint32(diamondLevel);

    if (totalMasteryLevels > 0)
        effects.IronShieldBonusPct = float(totalMasteryLevels) * 70.0f;

    if (bronzeLevel > 0)
        effects.BronzeWeakenedSoulReductionPct = std::min<float>(60.0f, float(bronzeLevel) * 6.0f);

    if (silverLevel > 0)
        effects.SilverHotPctPerTick = float(silverLevel) * 5.0f;

    if (goldLevel > 0)
        effects.GoldReflectPct = 15.0f + (float(goldLevel) * 6.0f);

    if (diamondLevel > 0)
    {
        effects.DiamondEndHealPct = std::min<float>(100.0f, 10.0f + (float(diamondLevel) * 9.0f));
        effects.DiamondEndHealRadius = 10.0f + float(diamondLevel);
    }

    return effects;
}

uint32 HealNearbyFriendlyPlayers(Player* caster, Unit* center, int32 healAmount, float radius, SpellInfo const* spellInfo)
{
    if (!center || healAmount <= 0 || radius <= 0.0f)
        return 0;

    std::list<Unit*> units;
    Acore::AnyFriendlyUnitInObjectRangeCheck unitCheck(center, center, radius);
    Acore::UnitListSearcher<Acore::AnyFriendlyUnitInObjectRangeCheck> searcher(center, units, unitCheck);
    Cell::VisitObjects(center, searcher, radius);

    uint32 healedTargets = 0;
    for (Unit* unit : units)
    {
        if (!unit || !unit->IsAlive() || !unit->IsPlayer())
            continue;

        if (!center->IsFriendlyTo(unit))
            continue;

        if (caster && spellInfo)
        {
            HealInfo healInfo(caster, unit, uint32(std::max<int32>(1, healAmount)), spellInfo, spellInfo->GetSchoolMask());
            caster->HealBySpell(healInfo);
        }
        else
            unit->ModifyHealth(healAmount);

        ++healedTargets;
    }

    return healedTargets;
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
        _renewSpellId = GetHighestKnownSpellInChain(_playerCaster, SpellMastery::SPELL_PRIEST_RENEW_RANK_1);
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

        ApplyBronzeWeakenedSoulReduction(target, _playerCaster, _effects);

        if (_effects.SilverHotPctPerTick > 0.0f)
        {
            if (Aura* shieldAura = target->GetAura(GetSpellInfo()->Id, _playerCaster->GetGUID()))
            {
                if (AuraEffect const* absorbEff = shieldAura->GetEffect(EFFECT_0))
                {
                    int32 const shieldAmount = std::max<int32>(1, absorbEff->GetAmount());
                    int32 const hotPerTick = std::max<int32>(1, int32(std::lround((float(shieldAmount) * _effects.SilverHotPctPerTick) / 100.0f)));
                    _playerCaster->CastCustomSpell(target, _renewSpellId, &hotPerTick, nullptr, nullptr, true);
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
    uint32 _renewSpellId = SpellMastery::SPELL_PRIEST_RENEW_RANK_1;
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
        _endHealSpellId = GetHighestKnownSpellInChain(_playerCaster, SPELL_PRIEST_FLASH_HEAL_RANK_1);
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
        ApplyBronzeWeakenedSoulReduction(GetTarget(), _playerCaster, _effects);

        if (_playerCaster && _playerCaster->GetSession() && SpellMastery::IsSpellMasteryFeedEnabled())
        {
            ChatHandler(_playerCaster->GetSession()).PSendSysMessage(
                "[SM PWS] absorb={} ironBonus={:.1f}% silverHotPerTick={:.1f}%",
                _initialShieldAmount,
                _effects.IronShieldBonusPct,
                _effects.SilverHotPctPerTick);
        }
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
        if (!target || _effects.DiamondEndHealPct <= 0.0f || _effects.DiamondEndHealRadius <= 0.0f)
            return;

        int32 const shieldPool = std::max<int32>(_initialShieldAmount, int32(_absorbedTotal));
        if (_playerCaster && _playerCaster->GetSession() && SpellMastery::IsSpellMasteryFeedEnabled())
        {
            ChatHandler(_playerCaster->GetSession()).PSendSysMessage(
                "[SM PWS] remove pool={} absorbed={} initial={} alive={}",
                shieldPool,
                _absorbedTotal,
                _initialShieldAmount,
                target->IsAlive() ? 1 : 0);
        }

        if (!target->IsAlive() || shieldPool <= 0)
            return;

        int32 const healAmount = std::max<int32>(1, int32(std::lround((float(shieldPool) * _effects.DiamondEndHealPct) / 100.0f)));
        SpellInfo const* endHealSpellInfo = sSpellMgr->GetSpellInfo(_endHealSpellId);
        if (!endHealSpellInfo)
            endHealSpellInfo = GetSpellInfo();
        uint32 const healedTargets = HealNearbyFriendlyPlayers(_playerCaster, target, healAmount, _effects.DiamondEndHealRadius, endHealSpellInfo);

        if (_playerCaster && _playerCaster->GetSession() && SpellMastery::IsSpellMasteryFeedEnabled())
        {
            ChatHandler(_playerCaster->GetSession()).PSendSysMessage(
                "[SM PWS] end-heal pool={} heal={} radius={:.1f} targets={}",
                shieldPool,
                healAmount,
                _effects.DiamondEndHealRadius,
                healedTargets);
        }
    }

    void Register() override
    {
        OnEffectApply += AuraEffectApplyFn(spell_pri_power_word_shield_mastery_aura::HandleApply, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_pri_power_word_shield_mastery_aura::HandleCalculateAmount, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
        AfterEffectAbsorb += AuraEffectAbsorbFn(spell_pri_power_word_shield_mastery_aura::HandleAfterAbsorb, EFFECT_0);
        OnEffectRemove += AuraEffectRemoveFn(spell_pri_power_word_shield_mastery_aura::HandleRemove, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    SpellMastery::SpellMasteryProgress _progress;
    PowerWordShieldMasteryEffects _effects;
    uint32 _endHealSpellId = SPELL_PRIEST_FLASH_HEAL_RANK_1;
    int32 _initialShieldAmount = 0;
    uint32 _absorbedTotal = 0;
};

class spell_pri_weakened_soul_mastery : public AuraScript
{
    PrepareAuraScript(spell_pri_weakened_soul_mastery);

    bool Load() override
    {
        if (!GetCaster() || !GetCaster()->IsPlayer())
            return false;

        Unit* owner = GetUnitOwner();
        if (!owner || !owner->IsPlayer())
            return false;

        _playerCaster = GetCaster()->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigByBaseSpell(SpellMastery::SPELL_PRIEST_POWER_WORD_SHIELD_RANK_1);
        if (!_config)
            return false;

        _progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildPowerWordShieldMasteryEffects(_progress, *_config);
        return true;
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        ApplyBronzeWeakenedSoulReduction(GetUnitOwner(), _playerCaster, _effects);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_pri_weakened_soul_mastery::HandleApply, EFFECT_ALL, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    SpellMastery::SpellMasteryProgress _progress;
    PowerWordShieldMasteryEffects _effects;
};

void AddSC_spell_mastery_priest()
{
    RegisterSpellScript(spell_pri_power_word_shield_mastery);
    RegisterSpellScript(spell_pri_power_word_shield_mastery_aura);
    RegisterSpellScript(spell_pri_weakened_soul_mastery);
}
