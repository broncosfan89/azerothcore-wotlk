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
#include "GameTime.h"
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
#include <mutex>
#include <unordered_map>

namespace
{
struct RejuvenationMasteryEffects
{
    float IronHealBonusPct = 0.0f;
    int32 BronzeDurationBonusMs = 0;
    uint8 BronzeExtraTicks = 0;
    float SilverSplashHealPct = 0.0f;
    float GoldStackHealPct = 0.0f;
    uint8 GoldMaxStacks = 1;
    float DiamondBonusTickPct = 0.0f;
};

struct RegrowthMasteryEffects
{
    float IronDirectHealBonusPct = 0.0f;
    float IronHotHealBonusPct = 0.0f;
    int32 BronzeDurationBonusMs = 0;
    uint8 BronzeExtraTicks = 0;
    float SilverSplashHealPct = 0.0f;
    float GoldStackHealPct = 0.0f;
    uint8 GoldMaxStacks = 1;
    float DiamondBonusDirectHealPct = 0.0f;
};

struct SwipeCatMasteryEffects
{
    int32 IronEnergyCostReduction = 0;
    float BronzeDamageBonusPct = 0.0f;
    int32 SilverEnergyRefund = 0;
    float GoldBleedPct = 0.0f;
    float DiamondHealPct = 0.0f;
};

struct RipMasteryEffects
{
    float IronDamageBonusPct = 0.0f;
    int32 BronzeTickIntervalMs = 2000;
    float SilverDamageTakenPct = 0.0f;
    int32 GoldDurationBonusMs = 0;
    bool DiamondFullDamageAtOneComboPoint = false;
};

struct RejuvenationStackKey
{
    uint32 CasterGuid;
    uint32 TargetGuid;

    bool operator==(RejuvenationStackKey const& other) const
    {
        return CasterGuid == other.CasterGuid && TargetGuid == other.TargetGuid;
    }
};

struct RejuvenationStackKeyHash
{
    std::size_t operator()(RejuvenationStackKey const& key) const
    {
        return (std::size_t(key.CasterGuid) << 32) ^ key.TargetGuid;
    }
};

struct RejuvenationStackState
{
    uint8 Stacks = 1;
    uint32 ExpiresAtMs = 0;
};

struct RegrowthStackKey
{
    uint32 CasterGuid;
    uint32 TargetGuid;

    bool operator==(RegrowthStackKey const& other) const
    {
        return CasterGuid == other.CasterGuid && TargetGuid == other.TargetGuid;
    }
};

struct RegrowthStackKeyHash
{
    std::size_t operator()(RegrowthStackKey const& key) const
    {
        return (std::size_t(key.CasterGuid) << 32) ^ key.TargetGuid;
    }
};

struct RegrowthStackState
{
    uint8 Stacks = 1;
    uint32 ExpiresAtMs = 0;
};

struct RipSilverDamageTakenKey
{
    uint32 CasterGuid;
    uint32 TargetGuid;

    bool operator==(RipSilverDamageTakenKey const& other) const
    {
        return CasterGuid == other.CasterGuid && TargetGuid == other.TargetGuid;
    }
};

struct RipSilverDamageTakenKeyHash
{
    std::size_t operator()(RipSilverDamageTakenKey const& key) const
    {
        return (std::size_t(key.CasterGuid) << 32) ^ key.TargetGuid;
    }
};

struct RipSilverDamageTakenState
{
    float DamageTakenPct = 0.0f;
    uint32 ExpiresAtMs = 0;
};

float constexpr REJUVENATION_SILVER_SPLASH_RADIUS = 15.0f;
int32 constexpr REJUVENATION_TICK_INTERVAL_MS = 3000;
uint32 constexpr REJUVENATION_STACK_STATE_TTL_MS = 35000;
uint32 constexpr REJUVENATION_XP_GUARD_MS = 350;
float constexpr REGROWTH_SILVER_SPLASH_RADIUS = 15.0f;
int32 constexpr REGROWTH_TICK_INTERVAL_MS = 3000;
uint32 constexpr REGROWTH_STACK_STATE_TTL_MS = 35000;
uint32 constexpr REGROWTH_XP_GUARD_MS = 350;
uint32 constexpr SWIPE_CAT_XP_GUARD_MS = 250;
uint32 constexpr RIP_XP_GUARD_MS = 250;
int32 constexpr RIP_BASE_TICK_INTERVAL_MS = 2000;
int32 constexpr RIP_MIN_TICK_INTERVAL_MS = 500;

std::unordered_map<RejuvenationStackKey, RejuvenationStackState, RejuvenationStackKeyHash> RejuvenationStackStates;
std::unordered_map<RegrowthStackKey, RegrowthStackState, RegrowthStackKeyHash> RegrowthStackStates;
std::unordered_map<RipSilverDamageTakenKey, RipSilverDamageTakenState, RipSilverDamageTakenKeyHash> RipSilverDamageTakenStates;
std::mutex SpellMasteryDruidStateMutex;

RejuvenationMasteryEffects BuildRejuvenationMasteryEffects(SpellMastery::SpellMasteryProgress const& progress, SpellMastery::ManagedSpellConfig const& config)
{
    RejuvenationMasteryEffects effects;

    uint8 ironLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_IRON, config);
    uint8 bronzeLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_BRONZE, config);
    uint8 silverLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_SILVER, config);
    uint8 goldLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_GOLD, config);
    uint8 diamondLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_DIAMOND, config);

    // Global output ramp: every tier level contributes % healing all the way through Diamond.
    effects.IronHealBonusPct += float(ironLevel) * 20.0f;
    effects.IronHealBonusPct += float(bronzeLevel) * 20.0f;
    effects.IronHealBonusPct += float(silverLevel) * 20.0f;
    effects.IronHealBonusPct += float(goldLevel) * 25.0f;
    effects.IronHealBonusPct += float(diamondLevel) * 30.0f;

    if (bronzeLevel > 0)
    {
        effects.BronzeDurationBonusMs = int32(bronzeLevel) * 300;
        if (bronzeLevel >= 5)
            ++effects.BronzeExtraTicks;
        if (bronzeLevel >= 10)
            ++effects.BronzeExtraTicks;
    }

    if (silverLevel > 0)
        effects.SilverSplashHealPct = 15.0f + (float(silverLevel - 1) * (25.0f / 9.0f));

    if (goldLevel > 0)
    {
        effects.GoldStackHealPct = 8.0f;
        effects.GoldMaxStacks = uint8(std::min<int32>(6, 2 + (int32(goldLevel - 1) * 4 + 8) / 9));
    }

    if (diamondLevel > 0)
        effects.DiamondBonusTickPct = 20.0f + (float(diamondLevel - 1) * (60.0f / 9.0f));

    return effects;
}

RegrowthMasteryEffects BuildRegrowthMasteryEffects(SpellMastery::SpellMasteryProgress const& progress, SpellMastery::ManagedSpellConfig const& config)
{
    RegrowthMasteryEffects effects;

    uint8 ironLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_IRON, config);
    uint8 bronzeLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_BRONZE, config);
    uint8 silverLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_SILVER, config);
    uint8 goldLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_GOLD, config);
    uint8 diamondLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_DIAMOND, config);

    // Global output ramp: every tier level contributes % healing all the way through Diamond.
    float const totalHealBonusPct =
        (float(ironLevel) * 20.0f) +
        (float(bronzeLevel) * 20.0f) +
        (float(silverLevel) * 20.0f) +
        (float(goldLevel) * 25.0f) +
        (float(diamondLevel) * 30.0f);
    effects.IronDirectHealBonusPct = totalHealBonusPct;
    effects.IronHotHealBonusPct = totalHealBonusPct;

    if (bronzeLevel > 0)
    {
        effects.BronzeDurationBonusMs = int32(bronzeLevel) * 250;
        if (bronzeLevel >= 5)
            ++effects.BronzeExtraTicks;
        if (bronzeLevel >= 10)
            ++effects.BronzeExtraTicks;
    }

    if (silverLevel > 0)
        effects.SilverSplashHealPct = 12.0f + (float(silverLevel - 1) * (20.0f / 9.0f));

    if (goldLevel > 0)
    {
        effects.GoldStackHealPct = 10.0f;
        effects.GoldMaxStacks = uint8(std::min<int32>(6, 2 + (int32(goldLevel - 1) * 4 + 8) / 9));
    }

    if (diamondLevel > 0)
        effects.DiamondBonusDirectHealPct = 20.0f + (float(diamondLevel - 1) * (60.0f / 9.0f));

    return effects;
}

SwipeCatMasteryEffects BuildSwipeCatMasteryEffects(SpellMastery::SpellMasteryProgress const& progress, SpellMastery::ManagedSpellConfig const& config)
{
    SwipeCatMasteryEffects effects;

    uint8 const ironLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_IRON, config);
    uint8 const bronzeLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_BRONZE, config);
    uint8 const silverLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_SILVER, config);
    uint8 const goldLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_GOLD, config);
    uint8 const diamondLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_DIAMOND, config);
    uint32 const totalMasteryLevels = uint32(ironLevel) + uint32(bronzeLevel) + uint32(silverLevel) + uint32(goldLevel) + uint32(diamondLevel);

    if (ironLevel > 0)
        effects.IronEnergyCostReduction = int32(ironLevel) * 2;

    if (totalMasteryLevels > 0)
        effects.BronzeDamageBonusPct = float(totalMasteryLevels) * 4.0f;

    if (silverLevel > 0)
        effects.SilverEnergyRefund = 1 + int32(silverLevel);

    if (goldLevel > 0)
        effects.GoldBleedPct = 10.0f + (float(goldLevel - 1) * (30.0f / 9.0f));

    if (diamondLevel > 0)
        effects.DiamondHealPct = 8.0f + (float(diamondLevel - 1) * (22.0f / 9.0f));

    return effects;
}

RipMasteryEffects BuildRipMasteryEffects(SpellMastery::SpellMasteryProgress const& progress, SpellMastery::ManagedSpellConfig const& config)
{
    RipMasteryEffects effects;

    uint8 const ironLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_IRON, config);
    uint8 const bronzeLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_BRONZE, config);
    uint8 const silverLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_SILVER, config);
    uint8 const goldLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_GOLD, config);
    uint8 const diamondLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_DIAMOND, config);
    uint32 const totalMasteryLevels = uint32(ironLevel) + uint32(bronzeLevel) + uint32(silverLevel) + uint32(goldLevel) + uint32(diamondLevel);

    if (totalMasteryLevels > 0)
        effects.IronDamageBonusPct = float(totalMasteryLevels) * 25.0f;

    if (bronzeLevel > 0)
        effects.BronzeTickIntervalMs = RIP_MIN_TICK_INTERVAL_MS;

    if (silverLevel > 0)
        effects.SilverDamageTakenPct = float(silverLevel) * 2.0f;

    if (goldLevel > 0)
        effects.GoldDurationBonusMs = int32(goldLevel) * 500;

    effects.DiamondFullDamageAtOneComboPoint = diamondLevel > 0;
    return effects;
}

float GetRipSilverDamageTakenPct(Player* caster, Unit* target)
{
    if (!caster || !target)
        return 0.0f;

    RipSilverDamageTakenKey const key
    {
        uint32(caster->GetGUID().GetCounter()),
        uint32(target->GetGUID().GetCounter())
    };

    std::lock_guard<std::mutex> lock(SpellMasteryDruidStateMutex);
    auto itr = RipSilverDamageTakenStates.find(key);
    if (itr == RipSilverDamageTakenStates.end())
        return 0.0f;

    uint32 const nowMs = uint32(GameTime::GetGameTimeMS().count());
    if (itr->second.ExpiresAtMs <= nowMs)
    {
        RipSilverDamageTakenStates.erase(itr);
        return 0.0f;
    }

    return itr->second.DamageTakenPct;
}
}

void ClearSpellMasteryDruidRuntimeStateForPlayer(uint32 guid)
{
    std::lock_guard<std::mutex> lock(SpellMasteryDruidStateMutex);
    for (auto itr = RejuvenationStackStates.begin(); itr != RejuvenationStackStates.end();)
    {
        if (itr->first.CasterGuid == guid || itr->first.TargetGuid == guid)
            itr = RejuvenationStackStates.erase(itr);
        else
            ++itr;
    }

    for (auto itr = RegrowthStackStates.begin(); itr != RegrowthStackStates.end();)
    {
        if (itr->first.CasterGuid == guid || itr->first.TargetGuid == guid)
            itr = RegrowthStackStates.erase(itr);
        else
            ++itr;
    }

    for (auto itr = RipSilverDamageTakenStates.begin(); itr != RipSilverDamageTakenStates.end();)
    {
        if (itr->first.CasterGuid == guid || itr->first.TargetGuid == guid)
            itr = RipSilverDamageTakenStates.erase(itr);
        else
            ++itr;
    }
}

class spell_dru_rejuvenation_mastery : public SpellScript
{
    PrepareSpellScript(spell_dru_rejuvenation_mastery);

    bool Load() override
    {
        if (!GetCaster() || !GetCaster()->IsPlayer())
            return false;

        _playerCaster = GetCaster()->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SpellMastery::SPELL_DRUID_REJUVENATION_RANK_1)
            return false;

        _progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildRejuvenationMasteryEffects(_progress, *_config);
        _isTriggeredCast = GetSpell()->IsTriggered();

        int32 const baseDuration = GetSpellInfo()->GetMaxDuration();
        if (baseDuration > 0)
        {
            int32 durationBonus = _effects.BronzeDurationBonusMs + (int32(_effects.BronzeExtraTicks) * REJUVENATION_TICK_INTERVAL_MS);
            if (durationBonus > 0)
                GetSpell()->SetSpellValue(SPELLVALUE_AURA_DURATION, baseDuration + durationBonus);
        }

        return true;
    }

    void HandleBeforeHit(SpellMissInfo /*missInfo*/)
    {
        Unit* target = GetHitUnit();
        if (!target || !_playerCaster->IsValidAssistTarget(target))
            return;

        uint32 const casterGuid = uint32(_playerCaster->GetGUID().GetCounter());
        uint32 const targetGuid = uint32(target->GetGUID().GetCounter());
        uint32 const nowMs = uint32(GameTime::GetGameTimeMS().count());

        uint8 nextStacks = 1;
        {
            std::lock_guard<std::mutex> lock(SpellMasteryDruidStateMutex);
            if (_effects.GoldMaxStacks > 1 && target->GetAura(_config->BaseSpellId, _playerCaster->GetGUID()))
            {
                auto itr = RejuvenationStackStates.find({ casterGuid, targetGuid });
                uint8 currentStacks = 1;
                if (itr != RejuvenationStackStates.end() && itr->second.ExpiresAtMs > nowMs)
                    currentStacks = std::max<uint8>(1, itr->second.Stacks);

                nextStacks = std::min<uint8>(_effects.GoldMaxStacks, uint8(currentStacks + 1));
            }

            RejuvenationStackState& stackState = RejuvenationStackStates[{ casterGuid, targetGuid }];
            stackState.Stacks = nextStacks;
            stackState.ExpiresAtMs = nowMs + REJUVENATION_STACK_STATE_TTL_MS;
        }
    }

    void HandleOnHit()
    {
        Unit* target = GetHitUnit();
        if (!target || !_playerCaster->IsValidAssistTarget(target))
            return;

        if (!_isTriggeredCast && target->GetHealth() < target->GetMaxHealth() && SpellMastery::ShouldAwardSpellMasteryXp(_playerCaster, *_config, REJUVENATION_XP_GUARD_MS))
            SpellMastery::AddSpellMasteryXp(_playerCaster, *_config, SpellMastery::SPELL_MASTERY_XP_PER_HIT);

        TryApplyDiamondBonusTick(target);
    }

    void TryApplyDiamondBonusTick(Unit* target)
    {
        if (_effects.DiamondBonusTickPct <= 0.0f || !target || _isTriggeredCast)
            return;

        Aura* rejuvenationAura = target->GetAura(_config->BaseSpellId, _playerCaster->GetGUID());
        if (!rejuvenationAura)
            return;

        AuraEffect* periodicEffect = rejuvenationAura->GetEffect(EFFECT_0);
        if (!periodicEffect)
            return;

        int32 const tickAmount = std::max<int32>(1, periodicEffect->GetAmount());
        uint32 bonusHeal = uint32(std::max<int32>(1, int32(std::lround((float(tickAmount) * _effects.DiamondBonusTickPct) / 100.0f))));
        bonusHeal = _playerCaster->SpellHealingBonusDone(target, GetSpellInfo(), bonusHeal, DOT, EFFECT_0);
        bonusHeal = target->SpellHealingBonusTaken(_playerCaster, GetSpellInfo(), bonusHeal, DOT);

        HealInfo healInfo(_playerCaster, target, bonusHeal, GetSpellInfo(), GetSpellInfo()->GetSchoolMask());
        _playerCaster->HealBySpell(healInfo);
    }

    void Register() override
    {
        BeforeHit += BeforeSpellHitFn(spell_dru_rejuvenation_mastery::HandleBeforeHit);
        AfterHit += SpellHitFn(spell_dru_rejuvenation_mastery::HandleOnHit);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    SpellMastery::SpellMasteryProgress _progress;
    RejuvenationMasteryEffects _effects;
    bool _isTriggeredCast = false;
};

class spell_dru_rejuvenation_mastery_aura : public AuraScript
{
    PrepareAuraScript(spell_dru_rejuvenation_mastery_aura);

    bool Load() override
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->IsPlayer() || !GetUnitOwner())
            return false;

        _playerCaster = caster->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SpellMastery::SPELL_DRUID_REJUVENATION_RANK_1)
            return false;

        _progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildRejuvenationMasteryEffects(_progress, *_config);
        return true;
    }

    void CalculatePeriodicHealAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        if (!_playerCaster || amount <= 0)
            return;

        amount = SpellMastery::ApplyEarlyAccessSpellScale(_playerCaster, GetSpellInfo(), amount);

        Unit* target = GetUnitOwner();
        if (!target)
            return;

        float totalBonusPct = _effects.IronHealBonusPct;
        if (_effects.GoldStackHealPct > 0.0f && _effects.GoldMaxStacks > 1)
        {
            uint32 const nowMs = uint32(GameTime::GetGameTimeMS().count());
            RejuvenationStackKey const key
            {
                uint32(_playerCaster->GetGUID().GetCounter()),
                uint32(target->GetGUID().GetCounter())
            };

            std::lock_guard<std::mutex> lock(SpellMasteryDruidStateMutex);
            auto itr = RejuvenationStackStates.find(key);
            if (itr != RejuvenationStackStates.end() && itr->second.ExpiresAtMs > nowMs)
            {
                uint8 stacks = std::min<uint8>(std::max<uint8>(1, itr->second.Stacks), _effects.GoldMaxStacks);
                if (stacks > 1)
                    totalBonusPct += _effects.GoldStackHealPct * float(stacks - 1);
            }
        }

        if (totalBonusPct <= 0.0f)
            return;

        int32 const scaledAmount = int32(std::lround(float(amount) * (1.0f + (totalBonusPct / 100.0f))));
        amount = std::max(amount, scaledAmount);
    }

    void HandlePeriodicTick(AuraEffect const* aurEff)
    {
        if (!_playerCaster || _effects.SilverSplashHealPct <= 0.0f)
            return;

        Unit* primaryTarget = GetUnitOwner();
        if (!primaryTarget || !primaryTarget->IsAlive())
            return;

        int32 const tickHealAmount = std::max<int32>(1, aurEff->GetAmount());
        uint32 splashHeal = uint32(std::max<int32>(1, int32(std::lround((float(tickHealAmount) * _effects.SilverSplashHealPct) / 100.0f))));

        std::list<Unit*> nearbyUnits;
        Acore::AnyFriendlyUnitInObjectRangeCheck check(primaryTarget, _playerCaster, REJUVENATION_SILVER_SPLASH_RADIUS);
        Acore::UnitListSearcher<Acore::AnyFriendlyUnitInObjectRangeCheck> searcher(primaryTarget, nearbyUnits, check);
        Cell::VisitObjects(primaryTarget, searcher, REJUVENATION_SILVER_SPLASH_RADIUS);

        Unit* splashTarget = nullptr;
        uint32 maxMissingHealth = 0;
        for (Unit* candidate : nearbyUnits)
        {
            if (!candidate || candidate == primaryTarget || !candidate->IsAlive() || !_playerCaster->IsValidAssistTarget(candidate))
                continue;

            uint32 const missingHealth = candidate->GetMaxHealth() - candidate->GetHealth();
            if (!missingHealth)
                continue;

            if (missingHealth > maxMissingHealth)
            {
                splashTarget = candidate;
                maxMissingHealth = missingHealth;
            }
        }

        if (!splashTarget)
            return;

        splashHeal = _playerCaster->SpellHealingBonusDone(splashTarget, GetSpellInfo(), splashHeal, DOT, aurEff->GetEffIndex());
        splashHeal = splashTarget->SpellHealingBonusTaken(_playerCaster, GetSpellInfo(), splashHeal, DOT);
        HealInfo healInfo(_playerCaster, splashTarget, splashHeal, GetSpellInfo(), GetSpellInfo()->GetSchoolMask());
        _playerCaster->HealBySpell(healInfo);
    }

    void HandleEffectApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (!_playerCaster)
            return;

        Unit* target = GetUnitOwner();
        if (!target)
            return;

        {
            std::lock_guard<std::mutex> lock(SpellMasteryDruidStateMutex);
            RejuvenationStackState& stackState = RejuvenationStackStates[
                {
                    uint32(_playerCaster->GetGUID().GetCounter()),
                    uint32(target->GetGUID().GetCounter())
                }];

            if (!stackState.Stacks)
                stackState.Stacks = 1;

            stackState.ExpiresAtMs = uint32(GameTime::GetGameTimeMS().count()) + REJUVENATION_STACK_STATE_TTL_MS;
        }
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_dru_rejuvenation_mastery_aura::CalculatePeriodicHealAmount, EFFECT_0, SPELL_AURA_PERIODIC_HEAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_dru_rejuvenation_mastery_aura::HandlePeriodicTick, EFFECT_0, SPELL_AURA_PERIODIC_HEAL);
        OnEffectApply += AuraEffectApplyFn(spell_dru_rejuvenation_mastery_aura::HandleEffectApply, EFFECT_0, SPELL_AURA_PERIODIC_HEAL, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    SpellMastery::SpellMasteryProgress _progress;
    RejuvenationMasteryEffects _effects;
};

class spell_dru_regrowth_mastery : public SpellScript
{
    PrepareSpellScript(spell_dru_regrowth_mastery);

    bool Load() override
    {
        if (!GetCaster() || !GetCaster()->IsPlayer())
            return false;

        _playerCaster = GetCaster()->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SpellMastery::SPELL_DRUID_REGROWTH_RANK_1)
            return false;

        _progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildRegrowthMasteryEffects(_progress, *_config);
        _isTriggeredCast = GetSpell()->IsTriggered();

        int32 const baseDuration = GetSpellInfo()->GetMaxDuration();
        if (baseDuration > 0)
        {
            int32 durationBonus = _effects.BronzeDurationBonusMs + (int32(_effects.BronzeExtraTicks) * REGROWTH_TICK_INTERVAL_MS);
            if (durationBonus > 0)
                GetSpell()->SetSpellValue(SPELLVALUE_AURA_DURATION, baseDuration + durationBonus);
        }

        return true;
    }

    void HandleBeforeHit(SpellMissInfo /*missInfo*/)
    {
        Unit* target = GetHitUnit();
        if (!target || !_playerCaster->IsValidAssistTarget(target))
            return;

        uint32 const casterGuid = uint32(_playerCaster->GetGUID().GetCounter());
        uint32 const targetGuid = uint32(target->GetGUID().GetCounter());
        uint32 const nowMs = uint32(GameTime::GetGameTimeMS().count());

        uint8 nextStacks = 1;
        {
            std::lock_guard<std::mutex> lock(SpellMasteryDruidStateMutex);
            if (_effects.GoldMaxStacks > 1 && target->GetAura(_config->BaseSpellId, _playerCaster->GetGUID()))
            {
                auto itr = RegrowthStackStates.find({ casterGuid, targetGuid });
                uint8 currentStacks = 1;
                if (itr != RegrowthStackStates.end() && itr->second.ExpiresAtMs > nowMs)
                    currentStacks = std::max<uint8>(1, itr->second.Stacks);

                nextStacks = std::min<uint8>(_effects.GoldMaxStacks, uint8(currentStacks + 1));
            }

            RegrowthStackState& stackState = RegrowthStackStates[{ casterGuid, targetGuid }];
            stackState.Stacks = nextStacks;
            stackState.ExpiresAtMs = nowMs + REGROWTH_STACK_STATE_TTL_MS;
        }
    }

    void HandleDirectHeal(SpellEffIndex /*effIndex*/)
    {
        Unit* target = GetHitUnit();
        if (!target || !_playerCaster->IsValidAssistTarget(target))
            return;

        int32 hitHeal = GetHitHeal();
        if (hitHeal <= 0)
            return;

        hitHeal = SpellMastery::ApplyEarlyAccessSpellScale(_playerCaster, GetSpellInfo(), hitHeal);

        if (_effects.IronDirectHealBonusPct > 0.0f)
        {
            int32 const scaledHeal = int32(std::lround(float(hitHeal) * (1.0f + (_effects.IronDirectHealBonusPct / 100.0f))));
            hitHeal = std::max(hitHeal, scaledHeal);
        }

        SetHitHeal(hitHeal);
        _finalDirectHeal = hitHeal;
    }

    void HandleAfterHit()
    {
        Unit* target = GetHitUnit();
        if (!target || !_playerCaster->IsValidAssistTarget(target))
            return;

        if (!_xpAwarded && !_isTriggeredCast && target->GetHealth() < target->GetMaxHealth() && SpellMastery::ShouldAwardSpellMasteryXp(_playerCaster, *_config, REGROWTH_XP_GUARD_MS))
        {
            SpellMastery::AddSpellMasteryXp(_playerCaster, *_config, SpellMastery::SPELL_MASTERY_XP_PER_HIT);
            _xpAwarded = true;
        }

        TryApplyDiamondBonusDirectHeal(target);
    }

    void TryApplyDiamondBonusDirectHeal(Unit* target)
    {
        if (_effects.DiamondBonusDirectHealPct <= 0.0f || !target || _isTriggeredCast)
            return;

        int32 const baseDirectHeal = std::max<int32>(0, _finalDirectHeal);
        if (baseDirectHeal <= 0)
            return;

        uint32 bonusHeal = uint32(std::max<int32>(1, int32(std::lround((float(baseDirectHeal) * _effects.DiamondBonusDirectHealPct) / 100.0f))));
        bonusHeal = _playerCaster->SpellHealingBonusDone(target, GetSpellInfo(), bonusHeal, HEAL, EFFECT_0);
        bonusHeal = target->SpellHealingBonusTaken(_playerCaster, GetSpellInfo(), bonusHeal, HEAL);

        HealInfo healInfo(_playerCaster, target, bonusHeal, GetSpellInfo(), GetSpellInfo()->GetSchoolMask());
        _playerCaster->HealBySpell(healInfo);
    }

    void Register() override
    {
        BeforeHit += BeforeSpellHitFn(spell_dru_regrowth_mastery::HandleBeforeHit);
        OnEffectHitTarget += SpellEffectFn(spell_dru_regrowth_mastery::HandleDirectHeal, EFFECT_0, SPELL_EFFECT_HEAL);
        AfterHit += SpellHitFn(spell_dru_regrowth_mastery::HandleAfterHit);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    SpellMastery::SpellMasteryProgress _progress;
    RegrowthMasteryEffects _effects;
    bool _isTriggeredCast = false;
    bool _xpAwarded = false;
    int32 _finalDirectHeal = 0;
};

class spell_dru_regrowth_mastery_aura : public AuraScript
{
    PrepareAuraScript(spell_dru_regrowth_mastery_aura);

    bool Load() override
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->IsPlayer() || !GetUnitOwner())
            return false;

        _playerCaster = caster->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SpellMastery::SPELL_DRUID_REGROWTH_RANK_1)
            return false;

        _progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildRegrowthMasteryEffects(_progress, *_config);
        return true;
    }

    void CalculatePeriodicHealAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        if (!_playerCaster || amount <= 0)
            return;

        amount = SpellMastery::ApplyEarlyAccessSpellScale(_playerCaster, GetSpellInfo(), amount);

        Unit* target = GetUnitOwner();
        if (!target)
            return;

        float totalBonusPct = _effects.IronHotHealBonusPct;
        if (_effects.GoldStackHealPct > 0.0f && _effects.GoldMaxStacks > 1)
        {
            uint32 const nowMs = uint32(GameTime::GetGameTimeMS().count());
            RegrowthStackKey const key
            {
                uint32(_playerCaster->GetGUID().GetCounter()),
                uint32(target->GetGUID().GetCounter())
            };

            std::lock_guard<std::mutex> lock(SpellMasteryDruidStateMutex);
            auto itr = RegrowthStackStates.find(key);
            if (itr != RegrowthStackStates.end() && itr->second.ExpiresAtMs > nowMs)
            {
                uint8 stacks = std::min<uint8>(std::max<uint8>(1, itr->second.Stacks), _effects.GoldMaxStacks);
                if (stacks > 1)
                    totalBonusPct += _effects.GoldStackHealPct * float(stacks - 1);
            }
        }

        if (totalBonusPct <= 0.0f)
            return;

        int32 const scaledAmount = int32(std::lround(float(amount) * (1.0f + (totalBonusPct / 100.0f))));
        amount = std::max(amount, scaledAmount);
    }

    void HandlePeriodicTick(AuraEffect const* aurEff)
    {
        if (!_playerCaster || _effects.SilverSplashHealPct <= 0.0f)
            return;

        Unit* primaryTarget = GetUnitOwner();
        if (!primaryTarget || !primaryTarget->IsAlive())
            return;

        int32 const tickHealAmount = std::max<int32>(1, aurEff->GetAmount());
        uint32 splashHeal = uint32(std::max<int32>(1, int32(std::lround((float(tickHealAmount) * _effects.SilverSplashHealPct) / 100.0f))));

        std::list<Unit*> nearbyUnits;
        Acore::AnyFriendlyUnitInObjectRangeCheck check(primaryTarget, _playerCaster, REGROWTH_SILVER_SPLASH_RADIUS);
        Acore::UnitListSearcher<Acore::AnyFriendlyUnitInObjectRangeCheck> searcher(primaryTarget, nearbyUnits, check);
        Cell::VisitObjects(primaryTarget, searcher, REGROWTH_SILVER_SPLASH_RADIUS);

        Unit* splashTarget = nullptr;
        uint32 maxMissingHealth = 0;
        for (Unit* candidate : nearbyUnits)
        {
            if (!candidate || candidate == primaryTarget || !candidate->IsAlive() || !_playerCaster->IsValidAssistTarget(candidate))
                continue;

            uint32 const missingHealth = candidate->GetMaxHealth() - candidate->GetHealth();
            if (!missingHealth)
                continue;

            if (missingHealth > maxMissingHealth)
            {
                splashTarget = candidate;
                maxMissingHealth = missingHealth;
            }
        }

        if (!splashTarget)
            return;

        splashHeal = _playerCaster->SpellHealingBonusDone(splashTarget, GetSpellInfo(), splashHeal, DOT, aurEff->GetEffIndex());
        splashHeal = splashTarget->SpellHealingBonusTaken(_playerCaster, GetSpellInfo(), splashHeal, DOT);
        HealInfo healInfo(_playerCaster, splashTarget, splashHeal, GetSpellInfo(), GetSpellInfo()->GetSchoolMask());
        _playerCaster->HealBySpell(healInfo);
    }

    void HandleEffectApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (!_playerCaster)
            return;

        Unit* target = GetUnitOwner();
        if (!target)
            return;

        {
            std::lock_guard<std::mutex> lock(SpellMasteryDruidStateMutex);
            RegrowthStackState& stackState = RegrowthStackStates[
                {
                    uint32(_playerCaster->GetGUID().GetCounter()),
                    uint32(target->GetGUID().GetCounter())
                }];

            if (!stackState.Stacks)
                stackState.Stacks = 1;

            stackState.ExpiresAtMs = uint32(GameTime::GetGameTimeMS().count()) + REGROWTH_STACK_STATE_TTL_MS;
        }
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_dru_regrowth_mastery_aura::CalculatePeriodicHealAmount, EFFECT_1, SPELL_AURA_PERIODIC_HEAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_dru_regrowth_mastery_aura::HandlePeriodicTick, EFFECT_1, SPELL_AURA_PERIODIC_HEAL);
        OnEffectApply += AuraEffectApplyFn(spell_dru_regrowth_mastery_aura::HandleEffectApply, EFFECT_1, SPELL_AURA_PERIODIC_HEAL, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    SpellMastery::SpellMasteryProgress _progress;
    RegrowthMasteryEffects _effects;
};

class spell_dru_swipe_cat_mastery : public SpellScript
{
    PrepareSpellScript(spell_dru_swipe_cat_mastery);

    bool Load() override
    {
        if (!GetCaster() || !GetCaster()->IsPlayer())
            return false;

        _playerCaster = GetCaster()->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SpellMastery::SPELL_DRUID_SWIPE_CAT_RANK_1)
            return false;

        _progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildSwipeCatMasteryEffects(_progress, *_config);
        _isTriggeredCast = GetSpell()->IsTriggered();
        return true;
    }

    void HandleDirectDamage()
    {
        Unit* target = GetHitUnit();
        if (!target || !_playerCaster->IsHostileTo(target))
            return;

        int32 hitDamage = GetHitDamage();
        if (hitDamage <= 0)
            return;

        hitDamage = SpellMastery::ApplyEarlyAccessSpellScale(_playerCaster, GetSpellInfo(), hitDamage);

        float totalDamageBonusPct = _effects.BronzeDamageBonusPct;
        totalDamageBonusPct += GetRipSilverDamageTakenPct(_playerCaster, target);
        if (totalDamageBonusPct > 0.0f)
        {
            int32 const scaledDamage = int32(std::lround(float(hitDamage) * (1.0f + (totalDamageBonusPct / 100.0f))));
            hitDamage = std::max(hitDamage, scaledDamage);
        }
        SetHitDamage(hitDamage);

        if (!_xpAwarded && !_isTriggeredCast && SpellMastery::ShouldAwardSpellMasteryXp(_playerCaster, *_config, SWIPE_CAT_XP_GUARD_MS))
        {
            SpellMastery::AddSpellMasteryXp(_playerCaster, *_config, SpellMastery::SPELL_MASTERY_XP_PER_HIT);
            _xpAwarded = true;
        }

        if (!_silverRefunded && !_isTriggeredCast && _effects.SilverEnergyRefund > 0)
        {
            _playerCaster->ModifyPower(POWER_ENERGY, _effects.SilverEnergyRefund);
            _silverRefunded = true;
        }

        if (_effects.GoldBleedPct > 0.0f)
        {
            int32 bleedAmount = std::max<int32>(1, int32(std::lround((float(hitDamage) * _effects.GoldBleedPct) / 100.0f)));
            _playerCaster->CastCustomSpell(target, SpellMastery::SPELL_WARRIOR_REND_RANK_1, &bleedAmount, nullptr, nullptr, true);
        }

        if (_effects.DiamondHealPct > 0.0f && _playerCaster->IsAlive())
        {
            int32 const healAmount = std::max<int32>(1, int32(std::lround((float(hitDamage) * _effects.DiamondHealPct) / 100.0f)));
            _playerCaster->ModifyHealth(healAmount);
        }
    }

    void HandleAfterCast()
    {
        if (_isTriggeredCast || _effects.IronEnergyCostReduction <= 0)
            return;

        _playerCaster->ModifyPower(POWER_ENERGY, _effects.IronEnergyCostReduction);
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_dru_swipe_cat_mastery::HandleDirectDamage);
        AfterCast += SpellCastFn(spell_dru_swipe_cat_mastery::HandleAfterCast);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    SpellMastery::SpellMasteryProgress _progress;
    SwipeCatMasteryEffects _effects;
    bool _isTriggeredCast = false;
    bool _xpAwarded = false;
    bool _silverRefunded = false;
};

class spell_dru_rip_mastery : public SpellScript
{
    PrepareSpellScript(spell_dru_rip_mastery);

    bool Load() override
    {
        if (!GetCaster() || !GetCaster()->IsPlayer())
            return false;

        _playerCaster = GetCaster()->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SpellMastery::SPELL_DRUID_RIP_RANK_1)
            return false;

        _progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildRipMasteryEffects(_progress, *_config);
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

        if (!_xpAwarded && !_isTriggeredCast && SpellMastery::ShouldAwardSpellMasteryXp(_playerCaster, *_config, RIP_XP_GUARD_MS))
        {
            SpellMastery::AddSpellMasteryXp(_playerCaster, *_config, SpellMastery::SPELL_MASTERY_XP_PER_HIT);
            _xpAwarded = true;
        }
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_dru_rip_mastery::HandleAfterHit);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    SpellMastery::SpellMasteryProgress _progress;
    RipMasteryEffects _effects;
    bool _isTriggeredCast = false;
    bool _xpAwarded = false;
};

class spell_dru_rip_mastery_aura : public AuraScript
{
    PrepareAuraScript(spell_dru_rip_mastery_aura);

    bool Load() override
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->IsPlayer() || !GetUnitOwner())
            return false;

        _playerCaster = caster->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SpellMastery::SPELL_DRUID_RIP_RANK_1)
            return false;

        _progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildRipMasteryEffects(_progress, *_config);
        return true;
    }

    void CalculatePeriodicDamageAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        if (!_playerCaster || amount <= 0)
            return;

        amount = SpellMastery::ApplyEarlyAccessSpellScale(_playerCaster, GetSpellInfo(), amount);

        float totalBonusPct = _effects.IronDamageBonusPct + _effects.SilverDamageTakenPct;
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

    void HandleEffectApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
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

        if (!_playerCaster || _effects.SilverDamageTakenPct <= 0.0f)
            return;

        Unit* target = GetUnitOwner();
        if (!target)
            return;

        int32 const durationMs = GetAura() ? GetAura()->GetDuration() : 0;
        uint32 const nowMs = uint32(GameTime::GetGameTimeMS().count());
        {
            std::lock_guard<std::mutex> lock(SpellMasteryDruidStateMutex);
            RipSilverDamageTakenStates[{ uint32(_playerCaster->GetGUID().GetCounter()), uint32(target->GetGUID().GetCounter()) }] =
            {
                _effects.SilverDamageTakenPct,
                nowMs + uint32(std::max<int32>(1000, durationMs))
            };
        }
    }

    void HandleEffectRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (!_playerCaster)
            return;

        Unit* target = GetUnitOwner();
        if (!target)
            return;

        {
            std::lock_guard<std::mutex> lock(SpellMasteryDruidStateMutex);
            RipSilverDamageTakenStates.erase({ uint32(_playerCaster->GetGUID().GetCounter()), uint32(target->GetGUID().GetCounter()) });
        }
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_dru_rip_mastery_aura::CalculatePeriodicDamageAmount, EFFECT_ALL, SPELL_AURA_PERIODIC_DAMAGE);
        DoEffectCalcPeriodic += AuraEffectCalcPeriodicFn(spell_dru_rip_mastery_aura::CalculatePeriodicTiming, EFFECT_ALL, SPELL_AURA_PERIODIC_DAMAGE);
        OnEffectUpdatePeriodic += AuraEffectUpdatePeriodicFn(spell_dru_rip_mastery_aura::HandlePeriodicUpdate, EFFECT_ALL, SPELL_AURA_PERIODIC_DAMAGE);
        OnEffectApply += AuraEffectApplyFn(spell_dru_rip_mastery_aura::HandleEffectApply, EFFECT_ALL, SPELL_AURA_PERIODIC_DAMAGE, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
        OnEffectRemove += AuraEffectRemoveFn(spell_dru_rip_mastery_aura::HandleEffectRemove, EFFECT_ALL, SPELL_AURA_PERIODIC_DAMAGE, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    SpellMastery::SpellMasteryProgress _progress;
    RipMasteryEffects _effects;
};

void AddSC_spell_mastery_druid()
{
    RegisterSpellAndAuraScriptPair(spell_dru_rejuvenation_mastery, spell_dru_rejuvenation_mastery_aura);
    RegisterSpellAndAuraScriptPair(spell_dru_regrowth_mastery, spell_dru_regrowth_mastery_aura);
    RegisterSpellScript(spell_dru_swipe_cat_mastery);
    RegisterSpellAndAuraScriptPair(spell_dru_rip_mastery, spell_dru_rip_mastery_aura);
}
