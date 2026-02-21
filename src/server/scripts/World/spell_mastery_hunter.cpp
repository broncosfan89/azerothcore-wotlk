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
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"

#include <algorithm>
#include <cmath>
#include <list>

namespace
{
struct VolleyMasteryEffects
{
    float IronDamageBonusPct = 0.0f;
    float BronzeRadiusMultiplier = 1.0f;
    int32 SilverDurationBonusMs = 0;
    int32 GoldTickIntervalMs = 0;
    float DiamondBurstDamagePct = 0.0f;
};

uint32 constexpr VOLLEY_XP_GUARD_MS = 8000;
float constexpr VOLLEY_DIAMOND_BURST_RADIUS = 6.0f;
int32 constexpr VOLLEY_GOLD_BASE_TICK_INTERVAL_MS = 1000;
int32 constexpr VOLLEY_GOLD_MIN_TICK_INTERVAL_MS = 500;

VolleyMasteryEffects BuildVolleyMasteryEffects(SpellMastery::SpellMasteryProgress const& progress, SpellMastery::ManagedSpellConfig const& config)
{
    VolleyMasteryEffects effects;

    uint8 const ironLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_IRON, config);
    uint8 const bronzeLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_BRONZE, config);
    uint8 const silverLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_SILVER, config);
    uint8 const goldLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_GOLD, config);
    uint8 const diamondLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_DIAMOND, config);

    // Iron: increase Volley damage.
    if (ironLevel > 0)
        effects.IronDamageBonusPct = float(ironLevel) * 10.0f;

    // Bronze: increase Volley radius.
    if (bronzeLevel > 0)
        effects.BronzeRadiusMultiplier += 1.5f * (float(bronzeLevel) / 10.0f);

    // Silver: increase Volley duration.
    if (silverLevel > 0)
        effects.SilverDurationBonusMs = int32(silverLevel) * 1000;

    // Gold: increase Volley tick rate (faster periodic trigger).
    if (goldLevel > 0)
    {
        int32 const reductionMs = int32(std::lround(float(VOLLEY_GOLD_BASE_TICK_INTERVAL_MS - VOLLEY_GOLD_MIN_TICK_INTERVAL_MS) * (float(goldLevel) / 10.0f)));
        effects.GoldTickIntervalMs = std::max<int32>(VOLLEY_GOLD_MIN_TICK_INTERVAL_MS, VOLLEY_GOLD_BASE_TICK_INTERVAL_MS - reductionMs);
    }

    // Diamond: add AoE burst damage around each target hit by Volley.
    if (diamondLevel > 0)
        effects.DiamondBurstDamagePct = 20.0f + (float(diamondLevel - 1) * (40.0f / 9.0f)); // 20% -> 60%

    return effects;
}

bool IsVolleyPeriodicTriggerEffect(AuraEffect const* aurEff)
{
    return aurEff && aurEff->GetAuraType() == SPELL_AURA_PERIODIC_TRIGGER_SPELL;
}
}

void ClearSpellMasteryHunterRuntimeStateForPlayer(uint32 /*guid*/)
{
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

        if (_effects.SilverDurationBonusMs > 0)
        {
            int32 const baseDuration = GetSpellInfo()->GetMaxDuration();
            if (baseDuration > 0)
                GetSpell()->SetSpellValue(SPELLVALUE_AURA_DURATION, baseDuration + _effects.SilverDurationBonusMs);
        }

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
        if (_effects.GoldTickIntervalMs <= 0 || !IsVolleyPeriodicTriggerEffect(aurEff))
            return;

        isPeriodic = true;
        amplitude = _effects.GoldTickIntervalMs;
    }

    void HandlePeriodicUpdate(AuraEffect* aurEff)
    {
        if (_effects.GoldTickIntervalMs <= 0 || !aurEff || !IsVolleyPeriodicTriggerEffect(aurEff))
            return;

        if (aurEff->GetPeriodicTimer() > _effects.GoldTickIntervalMs)
            aurEff->SetPeriodicTimer(_effects.GoldTickIntervalMs);
    }

    void HandleEffectApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (_effects.GoldTickIntervalMs <= 0)
            return;

        Aura* aura = GetAura();
        if (!aura)
            return;

        for (uint8 effectIndex = EFFECT_0; effectIndex < MAX_SPELL_EFFECTS; ++effectIndex)
        {
            if (AuraEffect* periodic = aura->GetEffect(effectIndex))
            {
                if (IsVolleyPeriodicTriggerEffect(periodic) && periodic->GetPeriodicTimer() > _effects.GoldTickIntervalMs)
                    periodic->SetPeriodicTimer(_effects.GoldTickIntervalMs);
            }
        }
    }

    void Register() override
    {
        DoEffectCalcPeriodic += AuraEffectCalcPeriodicFn(spell_hun_volley_mastery_aura::CalculatePeriodicTiming, EFFECT_ALL, SPELL_AURA_ANY);
        OnEffectUpdatePeriodic += AuraEffectUpdatePeriodicFn(spell_hun_volley_mastery_aura::HandlePeriodicUpdate, EFFECT_ALL, SPELL_AURA_ANY);
        OnEffectApply += AuraEffectApplyFn(spell_hun_volley_mastery_aura::HandleEffectApply, EFFECT_ALL, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
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

        TryApplyDiamondBurst(target, hitDamage);
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
};

void AddSC_spell_mastery_hunter()
{
    RegisterSpellAndAuraScriptPair(spell_hun_volley_mastery, spell_hun_volley_mastery_aura);
    RegisterSpellScript(spell_hun_volley_trigger_mastery);
}
