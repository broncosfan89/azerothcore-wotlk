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

#include "GameTime.h"
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
struct ThunderClapMasteryEffects
{
    float DamageBonusPct = 0.0f;
    float BronzeRadiusMultiplier = 1.0f;
    int32 BronzeCooldownReductionMs = 0;
    uint8 SilverRendTargets = 0;
    float GoldEchoDamagePct = 0.0f;
    float DiamondRendTickPct = 0.0f;
};

struct ThunderClapEchoToken
{
    float DamagePct = 0.0f;
    uint8 PendingCasts = 0;
    uint32 ExpiresAtMs = 0;
};

uint32 constexpr THUNDER_CLAP_XP_GUARD_MS = 350;
uint32 constexpr THUNDER_CLAP_ECHO_TOKEN_TTL_MS = 2000;

std::unordered_map<uint32, ThunderClapEchoToken> ThunderClapEchoTokens;

ThunderClapMasteryEffects BuildThunderClapMasteryEffects(SpellMastery::SpellMasteryProgress const& progress, SpellMastery::ManagedSpellConfig const& config)
{
    ThunderClapMasteryEffects effects;

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

    // Bronze: every other level grants radius and cooldown gains.
    uint8 bronzeStepLevel = bronzeLevel / 2;
    if (bronzeStepLevel > 0)
    {
        effects.BronzeRadiusMultiplier += float(bronzeStepLevel) * 0.06f; // +6% at 2/4/6/8/10 -> max +30%
        effects.BronzeCooldownReductionMs = int32(bronzeStepLevel) * 400;  // -0.4s at 2/4/6/8/10 -> max -2.0s
    }

    // Silver: apply Rend to N targets where N is silver tier level.
    effects.SilverRendTargets = silverLevel;

    // Gold: trigger a secondary Thunder Clap pulse.
    if (goldLevel > 0)
        effects.GoldEchoDamagePct = 20.0f + (float(goldLevel - 1) * (40.0f / 9.0f)); // 20% -> 60%

    // Diamond: trigger immediate Rend tick damage on targets with your Rend.
    if (diamondLevel > 0)
        effects.DiamondRendTickPct = 20.0f + (float(diamondLevel - 1) * (80.0f / 9.0f)); // 20% -> 100%

    return effects;
}

uint32 GetHighestKnownSpellInChain(Player* player, uint32 firstRankSpellId)
{
    if (!player || !firstRankSpellId)
        return 0;

    uint32 highestKnown = 0;
    for (uint32 spellId = firstRankSpellId; spellId; spellId = sSpellMgr->GetNextSpellInChain(spellId))
        if (player->HasSpell(spellId))
            highestKnown = spellId;

    return highestKnown ? highestKnown : firstRankSpellId;
}

Aura* FindRendAuraByCaster(Unit* target, ObjectGuid casterGuid)
{
    if (!target)
        return nullptr;

    for (uint32 spellId = SpellMastery::SPELL_WARRIOR_REND_RANK_1; spellId; spellId = sSpellMgr->GetNextSpellInChain(spellId))
        if (Aura* aura = target->GetAura(spellId, casterGuid))
            return aura;

    return nullptr;
}
}

void ClearSpellMasteryWarriorRuntimeStateForPlayer(uint32 guid)
{
    ThunderClapEchoTokens.erase(guid);
}

class spell_war_thunder_clap_mastery : public SpellScript
{
    PrepareSpellScript(spell_war_thunder_clap_mastery);

    bool Load() override
    {
        if (!GetCaster() || !GetCaster()->IsPlayer())
            return false;

        _playerCaster = GetCaster()->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SpellMastery::SPELL_WARRIOR_THUNDER_CLAP_RANK_1)
            return false;

        _progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildThunderClapMasteryEffects(_progress, *_config);
        _isTriggeredCast = GetSpell()->IsTriggered();
        _casterGuidLow = uint32(_playerCaster->GetGUID().GetCounter());

        _rendSpellId = GetHighestKnownSpellInChain(_playerCaster, SpellMastery::SPELL_WARRIOR_REND_RANK_1);

        if (_effects.BronzeRadiusMultiplier > 1.0f)
            GetSpell()->SetSpellValue(SPELLVALUE_RADIUS_MOD, int32(std::lround(_effects.BronzeRadiusMultiplier * 10000.0f)));

        if (_isTriggeredCast)
        {
            uint32 const nowMs = uint32(GameTime::GetGameTimeMS().count());
            auto itr = ThunderClapEchoTokens.find(_casterGuidLow);
            if (itr != ThunderClapEchoTokens.end() && itr->second.PendingCasts > 0 && itr->second.ExpiresAtMs > nowMs)
            {
                _isGoldEchoCast = true;
                _goldEchoDamagePct = itr->second.DamagePct;
                if (--itr->second.PendingCasts == 0)
                    ThunderClapEchoTokens.erase(itr);
            }
        }

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

        if (_isGoldEchoCast && _goldEchoDamagePct > 0.0f)
            hitDamage = std::max<int32>(1, int32(std::lround(float(hitDamage) * (_goldEchoDamagePct / 100.0f))));

        SetHitDamage(hitDamage);
        _finalHitDamage = hitDamage;
    }

    void HandleAfterHit()
    {
        Unit* target = GetHitUnit();
        if (!target || !_playerCaster->IsValidAttackTarget(target))
            return;

        if (!_xpAwarded && !_isTriggeredCast && SpellMastery::ShouldAwardSpellMasteryXp(_playerCaster, *_config, THUNDER_CLAP_XP_GUARD_MS))
        {
            SpellMastery::AddSpellMasteryXp(_playerCaster, *_config, SpellMastery::SPELL_MASTERY_XP_PER_HIT);
            _xpAwarded = true;
        }

        if (_isTriggeredCast)
            return;

        TryApplySilverRend(target);
        TryApplyDiamondRendTick(target);

        if (!_cooldownAdjusted && _effects.BronzeCooldownReductionMs > 0)
        {
            _playerCaster->ModifySpellCooldown(_config->AllowedSpellId, -_effects.BronzeCooldownReductionMs);
            _cooldownAdjusted = true;
        }

        TryTriggerGoldEcho();
    }

    void TryApplySilverRend(Unit* target)
    {
        if (!target || !_effects.SilverRendTargets || _silverRendApplied >= _effects.SilverRendTargets || !_rendSpellId)
            return;

        _playerCaster->CastSpell(target, _rendSpellId, TRIGGERED_FULL_MASK);
        ++_silverRendApplied;
    }

    void TryApplyDiamondRendTick(Unit* target)
    {
        if (!target || _effects.DiamondRendTickPct <= 0.0f)
            return;

        Aura* rendAura = FindRendAuraByCaster(target, _playerCaster->GetGUID());
        if (!rendAura)
            return;

        AuraEffect* rendEffect = rendAura->GetEffect(EFFECT_0);
        if (!rendEffect)
            return;

        int32 const rendTick = std::max<int32>(1, rendEffect->GetAmount());
        int32 const bonusTick = int32(std::lround(float(rendTick) * (_effects.DiamondRendTickPct / 100.0f)));
        if (bonusTick <= 0)
            return;

        SpellInfo const* rendSpellInfo = rendAura->GetSpellInfo();
        SpellNonMeleeDamage damageInfo(_playerCaster, target, rendSpellInfo, rendSpellInfo->SchoolMask);
        damageInfo.damage = bonusTick;
        _playerCaster->SendSpellNonMeleeDamageLog(&damageInfo);
        _playerCaster->DealSpellDamage(&damageInfo, false);
    }

    void TryTriggerGoldEcho()
    {
        if (_goldEchoTriggered || _effects.GoldEchoDamagePct <= 0.0f)
            return;

        ThunderClapEchoToken token;
        token.DamagePct = _effects.GoldEchoDamagePct;
        token.PendingCasts = 1;
        token.ExpiresAtMs = uint32(GameTime::GetGameTimeMS().count()) + THUNDER_CLAP_ECHO_TOKEN_TTL_MS;
        ThunderClapEchoTokens[_casterGuidLow] = token;

        _goldEchoTriggered = true;
        _playerCaster->CastSpell(_playerCaster, _config->AllowedSpellId, TRIGGERED_FULL_MASK);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_war_thunder_clap_mastery::HandleDirectDamage, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
        AfterHit += SpellHitFn(spell_war_thunder_clap_mastery::HandleAfterHit);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    SpellMastery::SpellMasteryProgress _progress;
    ThunderClapMasteryEffects _effects;

    bool _isTriggeredCast = false;
    bool _isGoldEchoCast = false;
    bool _xpAwarded = false;
    bool _cooldownAdjusted = false;
    bool _goldEchoTriggered = false;

    float _goldEchoDamagePct = 0.0f;
    int32 _finalHitDamage = 0;
    uint32 _casterGuidLow = 0;
    uint8 _silverRendApplied = 0;
    uint32 _rendSpellId = 0;
};

void AddSC_spell_mastery_warrior()
{
    RegisterSpellScript(spell_war_thunder_clap_mastery);
}
