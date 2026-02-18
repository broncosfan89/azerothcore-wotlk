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

float constexpr FIREBALL_SPLASH_RADIUS = 8.0f;
int32 constexpr FIREBALL_GOLD_BURN_BASE_DURATION_MS = 6000;
int32 constexpr FIREBALL_GOLD_BURN_DURATION_EXTEND_MS = 2000;
int32 constexpr FIREBALL_GOLD_BURN_DURATION_CAP_MS = 18000;
int32 constexpr FLAMESTRIKE_GOLD_BURN_BASE_DURATION_MS = 6000;
int32 constexpr FLAMESTRIKE_GOLD_BURN_DURATION_EXTEND_MS = 1000;
int32 constexpr FLAMESTRIKE_GOLD_BURN_DURATION_CAP_MS = 14000;
uint32 constexpr FLAMESTRIKE_BURN_STATE_TTL_MS = 15000;
uint32 constexpr FLAMESTRIKE_XP_GUARD_MS = 800;

std::unordered_map<FlamestrikeBurnKey, FlamestrikeBurnState, FlamestrikeBurnKeyHash> FlamestrikeBurnStates;

FireballMasteryEffects BuildFireballMasteryEffects(SpellMastery::SpellMasteryProgress const& progress, SpellMastery::ManagedSpellConfig const& config)
{
    FireballMasteryEffects effects;

    uint8 ironLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_IRON, config);
    uint8 bronzeLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_BRONZE, config);
    uint8 silverLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_SILVER, config);
    uint8 goldLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_GOLD, config);
    uint8 diamondLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_DIAMOND, config);

    effects.DamageBonusPct += float(ironLevel) * 55.0f;

    if (bronzeLevel > 0)
    {
        effects.BonusCritChancePct = 5.0f + (float(bronzeLevel - 1) * 2.0f);
        effects.DamageBonusPct += float(bronzeLevel) * 35.0f;
    }

    if (silverLevel > 0)
    {
        effects.DamageBonusPct += float(silverLevel) * 25.0f;
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

    effects.DamageBonusPct = float(ironLevel) * 12.0f;

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
    for (auto itr = FlamestrikeBurnStates.begin(); itr != FlamestrikeBurnStates.end();)
    {
        if (itr->first.CasterGuid == guid)
            itr = FlamestrikeBurnStates.erase(itr);
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
        if (!target || !_playerCaster->IsValidAttackTarget(target))
            return;

        if (!_isTriggeredCast)
            SpellMastery::AddSpellMasteryXp(_playerCaster, *_config, SpellMastery::SPELL_MASTERY_XP_PER_HIT);

        if (_isTriggeredCast)
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

        int32 previousTickAmount = 0;
        int32 stackedPerTick = burnPerTick;
        AuraEffect* currentIgniteEffect = nullptr;
        int32 priorMaxDuration = FIREBALL_GOLD_BURN_BASE_DURATION_MS;
        int32 priorDuration = FIREBALL_GOLD_BURN_BASE_DURATION_MS;
        if (Aura* igniteAura = primaryTarget->GetAura(SpellMastery::SPELL_MAGE_IGNITE, _playerCaster->GetGUID()))
        {
            priorMaxDuration = std::max(igniteAura->GetMaxDuration(), FIREBALL_GOLD_BURN_BASE_DURATION_MS);
            priorDuration = std::max(igniteAura->GetDuration(), FIREBALL_GOLD_BURN_BASE_DURATION_MS);
            currentIgniteEffect = igniteAura->GetEffect(EFFECT_0);
            if (currentIgniteEffect)
                previousTickAmount = std::max<int32>(0, currentIgniteEffect->GetAmount());

            stackedPerTick += previousTickAmount;
        }

        _playerCaster->CastCustomSpell(
            SpellMastery::SPELL_MAGE_IGNITE,
            SPELLVALUE_BASE_POINT0,
            stackedPerTick,
            primaryTarget,
            TriggerCastFlags(TRIGGERED_FULL_MASK & ~TRIGGERED_NO_PERIODIC_RESET),
            nullptr,
            currentIgniteEffect,
            _playerCaster->GetGUID());

        int32 appliedTickAmount = 0;
        int32 appliedDuration = 0;
        int32 appliedMaxDuration = 0;

        if (Aura* refreshedIgniteAura = primaryTarget->GetAura(SpellMastery::SPELL_MAGE_IGNITE, _playerCaster->GetGUID()))
        {
            int32 const nextMaxDuration = std::min(
                FIREBALL_GOLD_BURN_DURATION_CAP_MS,
                priorMaxDuration + FIREBALL_GOLD_BURN_DURATION_EXTEND_MS);
            int32 const nextDuration = std::min(
                nextMaxDuration,
                priorDuration + FIREBALL_GOLD_BURN_DURATION_EXTEND_MS);
            refreshedIgniteAura->SetMaxDuration(nextMaxDuration);
            refreshedIgniteAura->SetDuration(nextDuration);

            appliedDuration = refreshedIgniteAura->GetDuration();
            appliedMaxDuration = refreshedIgniteAura->GetMaxDuration();
            if (AuraEffect* refreshedEffect = refreshedIgniteAura->GetEffect(EFFECT_0))
                appliedTickAmount = refreshedEffect->GetAmount();
        }

        if (_playerCaster->GetSession())
        {
            ChatHandler(_playerCaster->GetSession()).PSendSysMessage(
                "[SM DBG] GoldBurn direct={} pct={:.1f} prev={} add={} new={} applied={} dur={}/{}",
                directDamage,
                _effects.GoldBurnPct,
                previousTickAmount,
                burnPerTick,
                stackedPerTick,
                appliedTickAmount,
                appliedDuration,
                appliedMaxDuration);
        }
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
        if (!target || !_playerCaster->IsValidAttackTarget(target))
            return;

        if (!_xpAwarded && !_isTriggeredCast && SpellMastery::ShouldAwardSpellMasteryXp(_playerCaster, *_config, FLAMESTRIKE_XP_GUARD_MS))
        {
            SpellMastery::AddSpellMasteryXp(_playerCaster, *_config, SpellMastery::SPELL_MASTERY_XP_PER_HIT);
            _xpAwarded = true;
        }

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

        FlamestrikeBurnState& burnState = FlamestrikeBurnStates[{ casterGuid, targetGuid }];
        if (burnState.ExpiresAtMs <= nowMs)
            burnState.Stacks = 0;

        burnState.Stacks = std::min<uint8>(_effects.GoldMaxStacks, uint8(burnState.Stacks + 1));
        burnState.ExpiresAtMs = nowMs + FLAMESTRIKE_BURN_STATE_TTL_MS;

        int32 const burnTotal = int32(std::lround((float(baseDamage) * _effects.GoldBurnDamagePct) / 100.0f));
        int32 const perTick = std::max<int32>(1, burnTotal / int32(burnInfo->GetMaxTicks()));
        int32 const stackedPerTick = std::max<int32>(1, perTick * burnState.Stacks);

        _playerCaster->CastCustomSpell(
            SpellMastery::SPELL_MAGE_IGNITE,
            SPELLVALUE_BASE_POINT0,
            stackedPerTick,
            target,
            TriggerCastFlags(TRIGGERED_FULL_MASK & ~TRIGGERED_NO_PERIODIC_RESET),
            nullptr,
            nullptr,
            _playerCaster->GetGUID());

        if (Aura* burnAura = target->GetAura(SpellMastery::SPELL_MAGE_IGNITE, _playerCaster->GetGUID()))
        {
            int32 const nextMaxDuration = std::min(
                FLAMESTRIKE_GOLD_BURN_DURATION_CAP_MS,
                std::max(burnAura->GetMaxDuration(), FLAMESTRIKE_GOLD_BURN_BASE_DURATION_MS) + FLAMESTRIKE_GOLD_BURN_DURATION_EXTEND_MS);
            int32 const nextDuration = std::min(
                nextMaxDuration,
                std::max(burnAura->GetDuration(), FLAMESTRIKE_GOLD_BURN_BASE_DURATION_MS) + FLAMESTRIKE_GOLD_BURN_DURATION_EXTEND_MS);
            burnAura->SetMaxDuration(nextMaxDuration);
            burnAura->SetDuration(nextDuration);
        }
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_mage_flamestrike_mastery::HandleDirectDamage, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
        OnEffectHitTarget += SpellEffectFn(spell_mage_flamestrike_mastery::HandleDirectDamage, EFFECT_1, SPELL_EFFECT_SCHOOL_DAMAGE);
        OnEffectHitTarget += SpellEffectFn(spell_mage_flamestrike_mastery::HandleDirectDamage, EFFECT_2, SPELL_EFFECT_SCHOOL_DAMAGE);
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
}
