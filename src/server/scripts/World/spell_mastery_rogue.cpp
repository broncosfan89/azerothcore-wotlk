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

uint64 constexpr KILLING_SPREE_XP_PER_USE = 500;
uint32 constexpr KILLING_SPREE_XP_GUARD_MS = 250;
uint32 constexpr KILLING_SPREE_BASE_ATTACK_COUNT = 5;
uint32 constexpr KILLING_SPREE_BASE_COOLDOWN_MS = 90000;
uint32 constexpr KILLING_SPREE_SILVER_COOLDOWN_MS = 45000;

std::unordered_map<uint32, uint8> KillingSpreePendingDiamondExtra;

KillingSpreeMasteryEffects BuildKillingSpreeMasteryEffects(SpellMastery::SpellMasteryProgress const& progress, SpellMastery::ManagedSpellConfig const& config)
{
    KillingSpreeMasteryEffects effects;

    uint8 ironLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_IRON, config);
    uint8 bronzeLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_BRONZE, config);
    uint8 silverLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_SILVER, config);
    uint8 goldLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_GOLD, config);
    uint8 diamondLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_DIAMOND, config);

    // Iron: increase total attacks from 5 up to 15.
    if (ironLevel > 0)
        effects.IronAttackCount = uint8(std::min<uint32>(15, KILLING_SPREE_BASE_ATTACK_COUNT + ironLevel));

    // Bronze: increase strike damage.
    effects.BronzeDamageBonusPct = float(bronzeLevel) * 8.0f;

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
}

void ClearSpellMasteryRogueRuntimeStateForPlayer(uint32 guid)
{
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

        auto itr = KillingSpreePendingDiamondExtra.find(_casterGuidLow);
        if (itr != KillingSpreePendingDiamondExtra.end() && itr->second > 0)
        {
            _isDiamondExtraCast = true;
            if (--itr->second == 0)
                KillingSpreePendingDiamondExtra.erase(itr);
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

        if (_effects.BronzeDamageBonusPct > 0.0f)
        {
            int32 const scaledDamage = int32(std::lround(float(hitDamage) * (1.0f + (_effects.BronzeDamageBonusPct / 100.0f))));
            hitDamage = std::max(hitDamage, scaledDamage);
            SetHitDamage(hitDamage);
        }

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

        ++KillingSpreePendingDiamondExtra[_casterGuidLow];
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

void AddSC_spell_mastery_rogue()
{
    RegisterSpellScript(spell_rog_killing_spree_mastery);
    RegisterSpellScript(spell_rog_killing_spree_weapon_mastery);
}
