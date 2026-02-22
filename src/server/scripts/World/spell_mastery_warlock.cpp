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

int32 constexpr HAUNT_DIAMOND_DOT_MAX_DURATION_MS = 60000;
uint32 constexpr HAUNT_XP_GUARD_MS = 250;
uint32 constexpr SHADOW_BOLT_XP_GUARD_MS = 250;
float constexpr SHADOW_BOLT_SILVER_SPLASH_RADIUS = 8.0f;
float constexpr SHADOW_BOLT_DIAMOND_SEARCH_RADIUS = 25.0f;

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

    // Iron: increase Shadow Bolt direct damage.
    if (ironLevel > 0)
        effects.IronDamageBonusPct = float(ironLevel) * 8.0f;

    // Bronze: reduce mana cost by refunding part of cast power cost.
    if (bronzeLevel > 0)
        effects.BronzeManaRefundPct = 6.0f + (float(bronzeLevel - 1) * 2.0f); // 6% -> 24%

    // Silver: splash damage around the primary target.
    if (silverLevel > 0)
        effects.SilverSplashDamagePct = 20.0f + (float(silverLevel - 1) * (30.0f / 9.0f)); // 20% -> 50%

    // Gold: apply a scaling Shadow DoT.
    if (goldLevel > 0)
        effects.GoldDotPerTickPct = 8.0f + (float(goldLevel - 1) * (20.0f / 9.0f)); // 8% -> 28%

    // Diamond: hit additional nearby targets.
    if (diamondLevel > 0)
        effects.DiamondExtraTargets = diamondLevel;

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

        hitDamage = SpellMastery::ApplyEarlyAccessSpellScale(_playerCaster, GetSpellInfo(), hitDamage);

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
        _playerCaster->CastSpell(target, SpellMastery::SPELL_WARLOCK_CORRUPTION_RANK_1, TRIGGERED_FULL_MASK);
        if (Aura* corruption = target->GetAura(SpellMastery::SPELL_WARLOCK_CORRUPTION_RANK_1, _playerCaster->GetGUID()))
        {
            if (AuraEffect* periodic = corruption->GetEffect(EFFECT_0))
                periodic->SetAmount(std::max<int32>(periodic->GetAmount(), dotPerTick));
        }
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

void AddSC_spell_mastery_warlock()
{
    RegisterSpellAndAuraScriptPair(spell_warl_haunt_mastery, spell_warl_haunt_mastery_aura);
    RegisterSpellScript(spell_warl_shadow_bolt_mastery);
}
