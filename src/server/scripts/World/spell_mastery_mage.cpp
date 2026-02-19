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
#include "Chat.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"

#include <algorithm>
#include <cmath>
#include <list>
#include <mutex>
#include <unordered_map>

namespace
{
struct FireballMasteryEffects
{
    float DamageBonusPct = 0.0f;
    float BonusCritChancePct = 0.0f;
    float SplashDamagePct = 0.0f;
    float GoldBurnPct = 0.0f;
    float DiamondCastTimeMultiplier = 1.0f;
    bool HasDiamondCastTime = false;
};

struct FlamestrikeMasteryEffects
{
    float DamageBonusPct = 0.0f;
    float RadiusMultiplier = 1.0f;
    float SilverExtraDamagePct = 0.0f;
    float GoldBurnDamagePct = 0.0f;
    uint8 GoldMaxStacks = 0;
    float DiamondCastTimeMultiplier = 1.0f;
    bool HasDiamondCastTime = false;
};

struct FlamestrikeBurnKey
{
    uint32 CasterGuid;
    uint32 TargetGuid;

    bool operator==(FlamestrikeBurnKey const& other) const
    {
        return CasterGuid == other.CasterGuid && TargetGuid == other.TargetGuid;
    }
};

struct FlamestrikeBurnKeyHash
{
    std::size_t operator()(FlamestrikeBurnKey const& key) const
    {
        return (std::size_t(key.CasterGuid) << 32) ^ key.TargetGuid;
    }
};

struct FlamestrikeBurnState
{
    uint8 Stacks = 0;
    uint32 ExpiresAtMs = 0;
};

struct IgniteCarryKey
{
    uint32 CasterGuid;
    uint32 TargetGuid;

    bool operator==(IgniteCarryKey const& other) const
    {
        return CasterGuid == other.CasterGuid && TargetGuid == other.TargetGuid;
    }
};

struct IgniteCarryKeyHash
{
    std::size_t operator()(IgniteCarryKey const& key) const
    {
        return (std::size_t(key.CasterGuid) << 32) ^ key.TargetGuid;
    }
};

struct IgniteCarryState
{
    int32 TickAmount = 0;
    int32 DurationMs = 0;
    int32 MaxDurationMs = 0;
    uint32 ExpiresAtMs = 0;
};

float constexpr FIREBALL_SPLASH_RADIUS = 8.0f;
int32 constexpr FIREBALL_GOLD_BURN_BASE_DURATION_MS = 6000;
int32 constexpr FIREBALL_GOLD_BURN_DURATION_EXTEND_MS = 2000;
int32 constexpr FIREBALL_GOLD_BURN_DURATION_CAP_MS = 18000;
int32 constexpr FLAMESTRIKE_GOLD_BURN_BASE_DURATION_MS = 6000;
int32 constexpr FLAMESTRIKE_GOLD_BURN_DURATION_EXTEND_MS = 1000;
int32 constexpr FLAMESTRIKE_GOLD_BURN_DURATION_CAP_MS = 14000;
uint32 constexpr FLAMESTRIKE_BURN_STATE_TTL_MS = 15000;
uint32 constexpr FLAMESTRIKE_XP_GUARD_MS = 800;
uint32 constexpr IGNITE_CARRY_TTL_MS = 750;
int32 constexpr IGNITE_FAST_TICK_INTERVAL_MS = 500;
uint32 constexpr SPELL_MAGE_IGNITE_TALENT_RANK_1 = 11119;
int32 constexpr PYROBLAST_IGNITE_BASE_DURATION_MS = 6000;
int32 constexpr PYROBLAST_IGNITE_DURATION_EXTEND_MS = 1000;
int32 constexpr PYROBLAST_IGNITE_DURATION_CAP_MS = 20000;

std::unordered_map<FlamestrikeBurnKey, FlamestrikeBurnState, FlamestrikeBurnKeyHash> FlamestrikeBurnStates;
std::unordered_map<IgniteCarryKey, IgniteCarryState, IgniteCarryKeyHash> IgniteCarryStates;
std::mutex SpellMasteryMageStateMutex;

void ApplyStackingIgniteDot(Player* caster, Unit* target, int32 addPerTick, int32 baseDurationMs, int32 extendDurationMs, int32 capDurationMs,
    char const* debugTag = nullptr, int32 sourceDamage = 0, float sourcePct = 0.0f)
{
    if (!caster || !target || addPerTick <= 0 || baseDurationMs <= 0 || capDurationMs <= 0)
        return;

    int32 previousTickAmount = 0;
    AuraEffect* currentIgniteEffect = nullptr;
    int32 priorMaxDuration = baseDurationMs;
    int32 priorDuration = baseDurationMs;
    if (Aura* igniteAura = target->GetAura(SpellMastery::SPELL_MAGE_IGNITE, caster->GetGUID()))
    {
        priorMaxDuration = std::max(igniteAura->GetMaxDuration(), baseDurationMs);
        priorDuration = std::max(igniteAura->GetDuration(), baseDurationMs);
        currentIgniteEffect = igniteAura->GetEffect(EFFECT_0);
        if (currentIgniteEffect)
            previousTickAmount = std::max<int32>(0, currentIgniteEffect->GetAmount());
    }

    int32 const stackedPerTick = std::max<int32>(1, previousTickAmount + addPerTick);

    caster->CastCustomSpell(
        SpellMastery::SPELL_MAGE_IGNITE,
        SPELLVALUE_BASE_POINT0,
        stackedPerTick,
        target,
        TriggerCastFlags(TRIGGERED_FULL_MASK & ~TRIGGERED_NO_PERIODIC_RESET),
        nullptr,
        currentIgniteEffect,
        caster->GetGUID());

    int32 appliedTickAmount = 0;
    int32 appliedDuration = 0;
    int32 appliedMaxDuration = 0;

    if (Aura* refreshedIgniteAura = target->GetAura(SpellMastery::SPELL_MAGE_IGNITE, caster->GetGUID()))
    {
        int32 const nextMaxDuration = std::min(capDurationMs, priorMaxDuration + std::max<int32>(0, extendDurationMs));
        int32 const nextDuration = std::min(nextMaxDuration, priorDuration + std::max<int32>(0, extendDurationMs));
        refreshedIgniteAura->SetMaxDuration(nextMaxDuration);
        refreshedIgniteAura->SetDuration(nextDuration);

        appliedDuration = refreshedIgniteAura->GetDuration();
        appliedMaxDuration = refreshedIgniteAura->GetMaxDuration();
        if (AuraEffect* refreshedEffect = refreshedIgniteAura->GetEffect(EFFECT_0))
            appliedTickAmount = refreshedEffect->GetAmount();
    }

    if (debugTag && caster->GetSession())
    {
        ChatHandler(caster->GetSession()).PSendSysMessage(
            "[SM DBG] {} src={} pct={:.1f} prev={} add={} new={} applied={} dur={}/{}",
            debugTag,
            sourceDamage,
            sourcePct,
            previousTickAmount,
            addPerTick,
            stackedPerTick,
            appliedTickAmount,
            appliedDuration,
            appliedMaxDuration);
    }
}

FireballMasteryEffects BuildFireballMasteryEffects(SpellMastery::SpellMasteryProgress const& progress, SpellMastery::ManagedSpellConfig const& config)
{
    FireballMasteryEffects effects;

    uint8 ironLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_IRON, config);
    uint8 bronzeLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_BRONZE, config);
    uint8 silverLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_SILVER, config);
    uint8 goldLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_GOLD, config);
    uint8 diamondLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_DIAMOND, config);

    // Global output ramp: every tier level contributes % damage all the way through Diamond.
    effects.DamageBonusPct += float(ironLevel) * 20.0f;
    effects.DamageBonusPct += float(bronzeLevel) * 20.0f;
    effects.DamageBonusPct += float(silverLevel) * 20.0f;
    effects.DamageBonusPct += float(goldLevel) * 25.0f;
    effects.DamageBonusPct += float(diamondLevel) * 30.0f;

    if (bronzeLevel > 0)
    {
        effects.BonusCritChancePct = 5.0f + (float(bronzeLevel - 1) * 2.0f);
    }

    if (silverLevel > 0)
    {
        effects.SplashDamagePct = 35.0f + (float(silverLevel - 1) * (45.0f / 9.0f));
    }

    if (goldLevel > 0)
        effects.GoldBurnPct = 20.0f + (float(goldLevel - 1) * (30.0f / 9.0f));

    if (diamondLevel > 0)
    {
        effects.HasDiamondCastTime = true;
        effects.DiamondCastTimeMultiplier = 0.05f - (float(diamondLevel - 1) * (0.04f / 9.0f));
    }

    return effects;
}

FireballMasteryEffects BuildPyroblastMasteryEffects(SpellMastery::SpellMasteryProgress const& progress, SpellMastery::ManagedSpellConfig const& config)
{
    FireballMasteryEffects effects;

    uint8 ironLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_IRON, config);
    uint8 bronzeLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_BRONZE, config);
    uint8 silverLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_SILVER, config);
    uint8 goldLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_GOLD, config);
    uint8 diamondLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_DIAMOND, config);

    uint32 const totalMasteryLevels = uint32(ironLevel) + uint32(bronzeLevel) + uint32(silverLevel) + uint32(goldLevel) + uint32(diamondLevel);
    effects.DamageBonusPct = float(totalMasteryLevels) * 2.0f;

    if (bronzeLevel > 0)
    {
        effects.BonusCritChancePct = 5.0f + (float(bronzeLevel - 1) * 2.0f);
    }

    if (silverLevel > 0)
    {
        effects.SplashDamagePct = 35.0f + (float(silverLevel - 1) * (45.0f / 9.0f));
    }

    if (goldLevel > 0)
        effects.GoldBurnPct = 20.0f + (float(goldLevel - 1) * (30.0f / 9.0f));

    if (diamondLevel > 0)
    {
        effects.HasDiamondCastTime = true;
        effects.DiamondCastTimeMultiplier = 0.05f - (float(diamondLevel - 1) * (0.04f / 9.0f));
    }

    return effects;
}

FlamestrikeMasteryEffects BuildFlamestrikeMasteryEffects(SpellMastery::SpellMasteryProgress const& progress, SpellMastery::ManagedSpellConfig const& config)
{
    FlamestrikeMasteryEffects effects;

    uint8 ironLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_IRON, config);
    uint8 bronzeLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_BRONZE, config);
    uint8 silverLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_SILVER, config);
    uint8 goldLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_GOLD, config);
    uint8 diamondLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_DIAMOND, config);

    // Global output ramp: every tier level contributes % damage all the way through Diamond.
    effects.DamageBonusPct += float(ironLevel) * 20.0f;
    effects.DamageBonusPct += float(bronzeLevel) * 20.0f;
    effects.DamageBonusPct += float(silverLevel) * 20.0f;
    effects.DamageBonusPct += float(goldLevel) * 25.0f;
    effects.DamageBonusPct += float(diamondLevel) * 30.0f;

    if (bronzeLevel > 0)
        effects.RadiusMultiplier += float(bronzeLevel) * 0.05f;

    if (silverLevel > 0)
        effects.SilverExtraDamagePct = 8.0f + float(silverLevel - 1) * (14.0f / 9.0f);

    if (goldLevel > 0)
    {
        effects.GoldBurnDamagePct = 10.0f + float(goldLevel - 1) * (15.0f / 9.0f);
        effects.GoldMaxStacks = uint8(std::min<int32>(10, 3 + goldLevel / 2));
    }

    if (diamondLevel > 0)
    {
        effects.HasDiamondCastTime = true;
        effects.DiamondCastTimeMultiplier = 0.80f - (float(diamondLevel - 1) * (0.70f / 9.0f));
    }

    return effects;
}
}

void ClearSpellMasteryMageRuntimeStateForPlayer(uint32 guid)
{
    std::lock_guard<std::mutex> lock(SpellMasteryMageStateMutex);

    for (auto itr = FlamestrikeBurnStates.begin(); itr != FlamestrikeBurnStates.end();)
    {
        if (itr->first.CasterGuid == guid)
            itr = FlamestrikeBurnStates.erase(itr);
        else
            ++itr;
    }

    for (auto itr = IgniteCarryStates.begin(); itr != IgniteCarryStates.end();)
    {
        if (itr->first.CasterGuid == guid)
            itr = IgniteCarryStates.erase(itr);
        else
            ++itr;
    }
}

class spell_mage_fireball_mastery : public SpellScript
{
    PrepareSpellScript(spell_mage_fireball_mastery);

    bool Load() override
    {
        if (!GetCaster() || !GetCaster()->IsPlayer())
            return false;

        _playerCaster = GetCaster()->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SpellMastery::SPELL_MAGE_FIREBALL_RANK_1)
            return false;

        _progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
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
        _finalDirectDamage = hitDamage;
    }

    void HandleOnHit()
    {
        Unit* target = GetHitUnit();
        if (!target || !_playerCaster->IsHostileTo(target))
            return;

        if (!_isTriggeredCast)
            SpellMastery::AddSpellMasteryXp(_playerCaster, *_config, SpellMastery::SPELL_MASTERY_XP_PER_HIT);

        if (_isTriggeredCast || !_playerCaster->IsValidAttackTarget(target))
            return;

        TryApplySilverSplashDamage(target);
        TryApplyGoldStackingBurn(target);
    }

    void TryApplySilverSplashDamage(Unit* primaryTarget)
    {
        if (_effects.SplashDamagePct <= 0.0f || !primaryTarget)
            return;

        int32 const directDamage = std::max<int32>(GetHitDamage(), _finalDirectDamage);
        if (directDamage <= 0)
            return;

        int32 const splashDamage = int32(std::lround((float(directDamage) * _effects.SplashDamagePct) / 100.0f));
        if (splashDamage <= 0)
            return;

        std::list<Unit*> nearbyUnits;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(primaryTarget, _playerCaster, FIREBALL_SPLASH_RADIUS);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(primaryTarget, nearbyUnits, check);
        Cell::VisitObjects(primaryTarget, searcher, FIREBALL_SPLASH_RADIUS);

        for (Unit* nearbyTarget : nearbyUnits)
        {
            if (!nearbyTarget || !_playerCaster->IsValidAttackTarget(nearbyTarget))
                continue;

            SpellNonMeleeDamage splashInfo(_playerCaster, nearbyTarget, GetSpellInfo(), GetSpellInfo()->SchoolMask);
            splashInfo.damage = splashDamage;
            _playerCaster->SendSpellNonMeleeDamageLog(&splashInfo);
            _playerCaster->DealSpellDamage(&splashInfo, false);
        }
    }

    void TryApplyGoldStackingBurn(Unit* primaryTarget)
    {
        if (_effects.GoldBurnPct <= 0.0f || !primaryTarget)
            return;

        int32 const directDamage = std::max<int32>(GetHitDamage(), _finalDirectDamage);
        if (directDamage <= 0)
            return;

        SpellInfo const* igniteInfo = sSpellMgr->GetSpellInfo(SpellMastery::SPELL_MAGE_IGNITE);
        if (!igniteInfo || !igniteInfo->GetMaxTicks())
            return;

        int32 const burnTotal = int32(std::lround((float(directDamage) * _effects.GoldBurnPct) / 100.0f));
        int32 const burnPerTick = std::max<int32>(1, burnTotal / int32(igniteInfo->GetMaxTicks()));
        ApplyStackingIgniteDot(
            _playerCaster,
            primaryTarget,
            burnPerTick,
            FIREBALL_GOLD_BURN_BASE_DURATION_MS,
            FIREBALL_GOLD_BURN_DURATION_EXTEND_MS,
            FIREBALL_GOLD_BURN_DURATION_CAP_MS,
            "GoldBurn",
            directDamage,
            _effects.GoldBurnPct);
    }

    void Register() override
    {
        BeforeHit += BeforeSpellHitFn(spell_mage_fireball_mastery::HandleBeforeHit);
        OnEffectHitTarget += SpellEffectFn(spell_mage_fireball_mastery::HandleDirectDamage, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
        AfterHit += SpellHitFn(spell_mage_fireball_mastery::HandleOnHit);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    SpellMastery::SpellMasteryProgress _progress;
    FireballMasteryEffects _effects;
    bool _isTriggeredCast = false;
    int32 _finalDirectDamage = 0;
};

class spell_mage_flamestrike_mastery : public SpellScript
{
    PrepareSpellScript(spell_mage_flamestrike_mastery);

    bool Load() override
    {
        if (!GetCaster() || !GetCaster()->IsPlayer())
            return false;

        _playerCaster = GetCaster()->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SpellMastery::SPELL_MAGE_FLAMESTRIKE_RANK_1)
            return false;

        _progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildFlamestrikeMasteryEffects(_progress, *_config);
        _isTriggeredCast = GetSpell()->IsTriggered();

        if (_effects.RadiusMultiplier > 1.0f)
            GetSpell()->SetSpellValue(SPELLVALUE_RADIUS_MOD, int32(std::lround(_effects.RadiusMultiplier * 10000.0f)));

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

        if (_effects.DamageBonusPct > 0.0f)
        {
            int32 const scaledDamage = int32(std::lround(float(hitDamage) * (1.0f + (_effects.DamageBonusPct / 100.0f))));
            hitDamage = std::max(hitDamage, scaledDamage);
        }

        SetHitDamage(hitDamage);
        _finalHitDamage = hitDamage;
    }

    void HandleOnHit()
    {
        Unit* target = GetHitUnit();
        if (!target || !_playerCaster->IsHostileTo(target))
            return;

        if (!_xpAwarded && !_isTriggeredCast && SpellMastery::ShouldAwardSpellMasteryXp(_playerCaster, *_config, FLAMESTRIKE_XP_GUARD_MS))
        {
            SpellMastery::AddSpellMasteryXp(_playerCaster, *_config, SpellMastery::SPELL_MASTERY_XP_PER_HIT);
            _xpAwarded = true;
        }

        if (!_playerCaster->IsValidAttackTarget(target))
            return;

        TryApplySilverExtraDamage(target);
        TryApplyGoldStackingBurn(target);
    }

    void TryApplySilverExtraDamage(Unit* target)
    {
        if (_effects.SilverExtraDamagePct <= 0.0f || !target || _isApplyingSilverExtra)
            return;

        int32 const baseDamage = std::max<int32>(GetHitDamage(), _finalHitDamage);
        if (baseDamage <= 0)
            return;

        int32 const extraDamage = int32(std::lround((float(baseDamage) * _effects.SilverExtraDamagePct) / 100.0f));
        if (extraDamage <= 0)
            return;

        _isApplyingSilverExtra = true;
        SpellNonMeleeDamage extraInfo(_playerCaster, target, GetSpellInfo(), GetSpellInfo()->SchoolMask);
        extraInfo.damage = extraDamage;
        _playerCaster->SendSpellNonMeleeDamageLog(&extraInfo);
        _playerCaster->DealSpellDamage(&extraInfo, false);
        _isApplyingSilverExtra = false;
    }

    void TryApplyGoldStackingBurn(Unit* target)
    {
        if (_effects.GoldBurnDamagePct <= 0.0f || _effects.GoldMaxStacks == 0 || !target)
            return;

        SpellInfo const* burnInfo = sSpellMgr->GetSpellInfo(SpellMastery::SPELL_MAGE_IGNITE);
        if (!burnInfo || !burnInfo->GetMaxTicks())
            return;

        int32 const baseDamage = std::max<int32>(GetHitDamage(), _finalHitDamage);
        if (baseDamage <= 0)
            return;

        uint32 const casterGuid = uint32(_playerCaster->GetGUID().GetCounter());
        uint32 const targetGuid = uint32(target->GetGUID().GetCounter());
        uint32 const nowMs = uint32(GameTime::GetGameTimeMS().count());

        uint8 burnStacks = 0;
        {
            std::lock_guard<std::mutex> lock(SpellMasteryMageStateMutex);
            FlamestrikeBurnState& burnState = FlamestrikeBurnStates[{ casterGuid, targetGuid }];
            if (burnState.ExpiresAtMs <= nowMs)
                burnState.Stacks = 0;

            burnState.Stacks = std::min<uint8>(_effects.GoldMaxStacks, uint8(burnState.Stacks + 1));
            burnState.ExpiresAtMs = nowMs + FLAMESTRIKE_BURN_STATE_TTL_MS;
            burnStacks = burnState.Stacks;
        }

        int32 const burnTotal = int32(std::lround((float(baseDamage) * _effects.GoldBurnDamagePct) / 100.0f));
        int32 const perTick = std::max<int32>(1, burnTotal / int32(burnInfo->GetMaxTicks()));
        int32 const stackedPerTick = std::max<int32>(1, perTick * burnStacks);

        ApplyStackingIgniteDot(
            _playerCaster,
            target,
            stackedPerTick,
            FLAMESTRIKE_GOLD_BURN_BASE_DURATION_MS,
            FLAMESTRIKE_GOLD_BURN_DURATION_EXTEND_MS,
            FLAMESTRIKE_GOLD_BURN_DURATION_CAP_MS);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_mage_flamestrike_mastery::HandleDirectDamage, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
        AfterHit += SpellHitFn(spell_mage_flamestrike_mastery::HandleOnHit);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    SpellMastery::SpellMasteryProgress _progress;
    FlamestrikeMasteryEffects _effects;
    bool _isTriggeredCast = false;
    bool _xpAwarded = false;
    bool _isApplyingSilverExtra = false;
    int32 _finalHitDamage = 0;
};

class spell_mage_pyroblast_ignite_pool : public SpellScript
{
    PrepareSpellScript(spell_mage_pyroblast_ignite_pool);

    bool Load() override
    {
        if (!GetCaster() || !GetCaster()->IsPlayer())
            return false;

        _playerCaster = GetCaster()->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SpellMastery::SPELL_MAGE_PYROBLAST_RANK_1)
            return false;

        _progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildPyroblastMasteryEffects(_progress, *_config);
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
        _finalHitDamage = hitDamage;
    }

    void HandleAfterHit()
    {
        Unit* target = GetHitUnit();
        if (!target || !_playerCaster->IsHostileTo(target))
            return;

        int32 const hitDamage = std::max<int32>(GetHitDamage(), _finalHitDamage);
        if (hitDamage <= 0)
            return;

        if (!_isTriggeredCast)
            SpellMastery::AddSpellMasteryXp(_playerCaster, *_config, SpellMastery::SPELL_MASTERY_XP_PER_HIT);

        if (!_playerCaster->IsValidAttackTarget(target))
            return;

        TryApplySilverSplashDamage(target, hitDamage);
        TryApplyGoldStackingBurn(target, hitDamage);
        TryApplyIgniteTalentBurn(target, hitDamage);
    }

    void TryApplySilverSplashDamage(Unit* primaryTarget, int32 hitDamage)
    {
        if (_effects.SplashDamagePct <= 0.0f || !primaryTarget || hitDamage <= 0)
            return;

        int32 const splashDamage = int32(std::lround((float(hitDamage) * _effects.SplashDamagePct) / 100.0f));
        if (splashDamage <= 0)
            return;

        std::list<Unit*> nearbyUnits;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(primaryTarget, _playerCaster, FIREBALL_SPLASH_RADIUS);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(primaryTarget, nearbyUnits, check);
        Cell::VisitObjects(primaryTarget, searcher, FIREBALL_SPLASH_RADIUS);

        for (Unit* nearbyTarget : nearbyUnits)
        {
            if (!nearbyTarget || !_playerCaster->IsValidAttackTarget(nearbyTarget))
                continue;

            SpellNonMeleeDamage splashInfo(_playerCaster, nearbyTarget, GetSpellInfo(), GetSpellInfo()->SchoolMask);
            splashInfo.damage = splashDamage;
            _playerCaster->SendSpellNonMeleeDamageLog(&splashInfo);
            _playerCaster->DealSpellDamage(&splashInfo, false);
        }
    }

    void TryApplyGoldStackingBurn(Unit* target, int32 hitDamage)
    {
        if (_effects.GoldBurnPct <= 0.0f || !target || hitDamage <= 0)
            return;

        SpellInfo const* igniteInfo = sSpellMgr->GetSpellInfo(SpellMastery::SPELL_MAGE_IGNITE);
        if (!igniteInfo || !igniteInfo->GetMaxTicks())
            return;

        int32 const burnTotal = int32(std::lround((float(hitDamage) * _effects.GoldBurnPct) / 100.0f));
        int32 const burnPerTick = std::max<int32>(1, burnTotal / int32(igniteInfo->GetMaxTicks()));
        ApplyStackingIgniteDot(
            _playerCaster,
            target,
            burnPerTick,
            FIREBALL_GOLD_BURN_BASE_DURATION_MS,
            FIREBALL_GOLD_BURN_DURATION_EXTEND_MS,
            FIREBALL_GOLD_BURN_DURATION_CAP_MS,
            "PyroGoldBurn",
            hitDamage,
            _effects.GoldBurnPct);
    }

    void TryApplyIgniteTalentBurn(Unit* target, int32 hitDamage)
    {
        AuraEffect const* igniteTalent = _playerCaster->GetAuraEffectOfRankedSpell(SPELL_MAGE_IGNITE_TALENT_RANK_1, EFFECT_0);
        if (!igniteTalent)
            return;

        SpellInfo const* igniteInfo = sSpellMgr->GetSpellInfo(SpellMastery::SPELL_MAGE_IGNITE);
        if (!igniteInfo || !igniteInfo->GetMaxTicks())
            return;

        int32 const ignitePct = 8 * int32(igniteTalent->GetSpellInfo()->GetRank());
        if (ignitePct <= 0)
            return;

        int32 const burnTotal = CalculatePct(hitDamage, ignitePct);
        int32 const burnPerTick = std::max<int32>(1, burnTotal / int32(igniteInfo->GetMaxTicks()));

        ApplyStackingIgniteDot(
            _playerCaster,
            target,
            burnPerTick,
            PYROBLAST_IGNITE_BASE_DURATION_MS,
            PYROBLAST_IGNITE_DURATION_EXTEND_MS,
            PYROBLAST_IGNITE_DURATION_CAP_MS);
    }

    void Register() override
    {
        BeforeHit += BeforeSpellHitFn(spell_mage_pyroblast_ignite_pool::HandleBeforeHit);
        OnEffectHitTarget += SpellEffectFn(spell_mage_pyroblast_ignite_pool::HandleDirectDamage, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
        AfterHit += SpellHitFn(spell_mage_pyroblast_ignite_pool::HandleAfterHit);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    SpellMastery::SpellMasteryProgress _progress;
    FireballMasteryEffects _effects;
    bool _isTriggeredCast = false;
    int32 _finalHitDamage = 0;
};

class spell_mage_ignite_mastery_pool : public AuraScript
{
    PrepareAuraScript(spell_mage_ignite_mastery_pool);

    bool Load() override
    {
        return GetCaster() && GetCaster()->IsPlayer() && GetUnitOwner();
    }

    void HandleEffectApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetUnitOwner();
        if (!caster || !target)
            return;

        uint32 const nowMs = uint32(GameTime::GetGameTimeMS().count());
        IgniteCarryKey const key{ uint32(caster->GetGUID().GetCounter()), uint32(target->GetGUID().GetCounter()) };
        IgniteCarryState carryState;
        {
            std::lock_guard<std::mutex> lock(SpellMasteryMageStateMutex);
            auto itr = IgniteCarryStates.find(key);
            if (itr == IgniteCarryStates.end() || itr->second.ExpiresAtMs <= nowMs)
                return;

            carryState = itr->second;
            IgniteCarryStates.erase(itr);
        }

        if (Aura* igniteAura = GetAura())
        {
            if (AuraEffect* igniteEffect = igniteAura->GetEffect(EFFECT_0))
            {
                igniteEffect->SetAmount(std::max<int32>(igniteEffect->GetAmount(), carryState.TickAmount));
                igniteEffect->SetPeriodicTimer(IGNITE_FAST_TICK_INTERVAL_MS);
            }

            int32 const mergedMaxDuration = std::max<int32>(igniteAura->GetMaxDuration(), carryState.MaxDurationMs);
            int32 const mergedDuration = std::max<int32>(igniteAura->GetDuration(), std::min<int32>(carryState.DurationMs, mergedMaxDuration));
            igniteAura->SetMaxDuration(mergedMaxDuration);
            igniteAura->SetDuration(mergedDuration);
        }
    }

    void HandleEffectRemove(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        AuraApplication const* app = GetTargetApplication();
        if (!app || app->GetRemoveMode() == AURA_REMOVE_BY_EXPIRE || app->GetRemoveMode() == AURA_REMOVE_BY_DEATH)
            return;

        Unit* caster = GetCaster();
        Unit* target = GetUnitOwner();
        Aura const* aura = GetAura();
        if (!caster || !target || !aura)
            return;

        uint32 const nowMs = uint32(GameTime::GetGameTimeMS().count());
        IgniteCarryState state;
        state.TickAmount = std::max<int32>(0, aurEff->GetAmount());
        state.DurationMs = std::max<int32>(0, aura->GetDuration());
        state.MaxDurationMs = std::max<int32>(0, aura->GetMaxDuration());
        state.ExpiresAtMs = nowMs + IGNITE_CARRY_TTL_MS;
        {
            std::lock_guard<std::mutex> lock(SpellMasteryMageStateMutex);
            IgniteCarryStates[{ uint32(caster->GetGUID().GetCounter()), uint32(target->GetGUID().GetCounter()) }] = state;
        }
    }

    void HandlePeriodicUpdate(AuraEffect* aurEff)
    {
        if (!aurEff)
            return;

        // Keep Ignite ticking quickly even while frequent reapplications are happening.
        if (aurEff->GetPeriodicTimer() > IGNITE_FAST_TICK_INTERVAL_MS)
            aurEff->SetPeriodicTimer(IGNITE_FAST_TICK_INTERVAL_MS);
    }

    void Register() override
    {
        OnEffectApply += AuraEffectApplyFn(spell_mage_ignite_mastery_pool::HandleEffectApply, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
        OnEffectRemove += AuraEffectRemoveFn(spell_mage_ignite_mastery_pool::HandleEffectRemove, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
        OnEffectUpdatePeriodic += AuraEffectUpdatePeriodicFn(spell_mage_ignite_mastery_pool::HandlePeriodicUpdate, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE);
    }
};

class spell_mastery_prepare_mage_spell_script : public AllSpellScript
{
public:
    spell_mastery_prepare_mage_spell_script() : AllSpellScript("spell_mastery_prepare_mage_spell_script", { ALLSPELLHOOK_ON_PREPARE })
    {
    }

    void OnSpellPrepare(Spell* spell, Unit* caster, SpellInfo const* spellInfo) override
    {
        if (!spell || !caster || !caster->IsPlayer() || !spellInfo)
            return;

        SpellMastery::ManagedSpellConfig const* config = SpellMastery::GetManagedSpellConfigForSpell(spellInfo->Id);
        if (!config)
            return;

        Player* player = caster->ToPlayer();
        SpellMastery::SpellMasteryProgress const& progress = SpellMastery::GetOrLoadSpellMasteryProgress(player, *config);

        if (config->BaseSpellId == SpellMastery::SPELL_MAGE_FIREBALL_RANK_1)
        {
            FireballMasteryEffects const effects = BuildFireballMasteryEffects(progress, *config);
            if (!effects.HasDiamondCastTime)
                return;

            int32 const currentCastTime = spell->GetCastTime();
            if (currentCastTime <= 0)
                return;

            int32 const reducedCastTime = std::max<int32>(1, int32(float(currentCastTime) * effects.DiamondCastTimeMultiplier));
            if (reducedCastTime < currentCastTime)
                spell->SetSpellMasteryCastTime(reducedCastTime);
            return;
        }

        if (config->BaseSpellId == SpellMastery::SPELL_MAGE_PYROBLAST_RANK_1)
        {
            FireballMasteryEffects const effects = BuildPyroblastMasteryEffects(progress, *config);
            if (!effects.HasDiamondCastTime)
                return;

            int32 const currentCastTime = spell->GetCastTime();
            if (currentCastTime <= 0)
                return;

            int32 const reducedCastTime = std::max<int32>(1, int32(float(currentCastTime) * effects.DiamondCastTimeMultiplier));
            if (reducedCastTime < currentCastTime)
                spell->SetSpellMasteryCastTime(reducedCastTime);
            return;
        }

        if (config->BaseSpellId == SpellMastery::SPELL_MAGE_FLAMESTRIKE_RANK_1)
        {
            FlamestrikeMasteryEffects const effects = BuildFlamestrikeMasteryEffects(progress, *config);
            if (effects.RadiusMultiplier > 1.0f)
                spell->SetSpellValue(SPELLVALUE_RADIUS_MOD, int32(std::lround(effects.RadiusMultiplier * 10000.0f)));

            if (!effects.HasDiamondCastTime)
                return;

            int32 const currentCastTime = spell->GetCastTime();
            if (currentCastTime <= 0)
                return;

            int32 const reducedCastTime = std::max<int32>(1, int32(float(currentCastTime) * effects.DiamondCastTimeMultiplier));
            if (reducedCastTime < currentCastTime)
                spell->SetSpellMasteryCastTime(reducedCastTime);
        }
    }
};

void AddSC_spell_mastery_mage()
{
    new spell_mastery_prepare_mage_spell_script();
    RegisterSpellScript(spell_mage_fireball_mastery);
    RegisterSpellScript(spell_mage_flamestrike_mastery);
    RegisterSpellScript(spell_mage_pyroblast_ignite_pool);
    RegisterSpellScript(spell_mage_ignite_mastery_pool);
}
