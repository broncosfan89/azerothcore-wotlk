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
#include "GameTime.h"
#include "Log.h"
#include "Player.h"
#include "Spell.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace
{
struct ConsecrationMasteryEffects
{
    float IronDamageBonusPct = 0.0f;
    float BronzeRadiusMultiplier = 1.0f;
    float SilverEnemyDamageReductionPct = 0.0f;
    uint8 GoldMaxStacks = 0;
    float GoldDamageBonusPctPerStack = 0.0f;
    float DiamondHealPctOfHitDamage = 0.0f;
};

struct ConsecrationSilverState
{
    float DamageReductionPct = 0.0f;
    uint32 ExpiresAtMs = 0;
};

struct ConsecrationGoldState
{
    uint8 Stacks = 0;
    float DamageBonusPctPerStack = 0.0f;
    uint32 ExpiresAtMs = 0;
};

uint32 constexpr CONSECRATION_XP_GUARD_MS = 8000;
uint32 constexpr CONSECRATION_SILVER_STATE_TTL_MS = 2000;
uint32 constexpr CONSECRATION_GOLD_STATE_TTL_MS = 2000;

// Tracks enemies temporarily weakened while they stand in Consecration.
std::unordered_map<uint32, ConsecrationSilverState> ConsecrationSilverStates;
// Tracks temporary paladin self-buff stacks while Consecration is actively hitting enemies.
std::unordered_map<uint32, ConsecrationGoldState> ConsecrationGoldStates;
std::mutex SpellMasteryPaladinStateMutex;

ConsecrationMasteryEffects BuildConsecrationMasteryEffects(SpellMastery::SpellMasteryProgress const& progress, SpellMastery::ManagedSpellConfig const& config)
{
    ConsecrationMasteryEffects effects;

    uint8 const ironLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_IRON, config);
    uint8 const bronzeLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_BRONZE, config);
    uint8 const silverLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_SILVER, config);
    uint8 const goldLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_GOLD, config);
    uint8 const diamondLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_DIAMOND, config);

    if (ironLevel > 0)
        effects.IronDamageBonusPct = float(ironLevel) * 8.0f;

    if (bronzeLevel > 0)
        effects.BronzeRadiusMultiplier += float(bronzeLevel) * 0.05f;

    if (silverLevel > 0)
        effects.SilverEnemyDamageReductionPct = float(silverLevel) * 1.5f;

    if (goldLevel > 0)
    {
        effects.GoldMaxStacks = goldLevel;
        effects.GoldDamageBonusPctPerStack = 1.0f + (float(goldLevel) * 0.2f);
    }

    if (diamondLevel > 0)
        effects.DiamondHealPctOfHitDamage = 10.0f + (float(diamondLevel) * 5.0f);

    return effects;
}

bool IsDamageEffect(SpellEffectInfo const& effectInfo)
{
    if (!effectInfo.IsEffect())
        return false;

    switch (effectInfo.Effect)
    {
        case SPELL_EFFECT_SCHOOL_DAMAGE:
        case SPELL_EFFECT_WEAPON_DAMAGE:
        case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
        case SPELL_EFFECT_HEALTH_LEECH:
        case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
        case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
            return true;
        default:
            break;
    }

    return effectInfo.IsAura(SPELL_AURA_PERIODIC_DAMAGE) || effectInfo.IsAura(SPELL_AURA_PERIODIC_LEECH);
}

void ApplySpellDamagePercentModifier(Spell* spell, Unit* caster, SpellInfo const* spellInfo, float pct, float silverReductionPct)
{
    if (!spell || !caster || !spellInfo || std::fabs(pct) < 0.01f)
        return;

    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        SpellEffectInfo const& effectInfo = spellInfo->Effects[i];
        if (!IsDamageEffect(effectInfo))
            continue;

        int32 const baseValue = effectInfo.CalcValue(caster);
        if (!baseValue)
            continue;

        float const scaled = float(baseValue) * (1.0f + (pct / 100.0f));
        int32 scaledValue = int32(std::lround(scaled));

        if (baseValue > 0)
            scaledValue = std::max<int32>(1, scaledValue);
        else if (baseValue < 0)
            scaledValue = std::min<int32>(-1, scaledValue);

        spell->SetSpellValue(SpellValueMod(SPELLVALUE_BASE_POINT0 + i), scaledValue);

        if (silverReductionPct > 0.0f)
        {
            LOG_INFO(
                "spells",
                "SpellMastery Consecration Silver: caster={} spell={} effect={} base={} modified={} totalPct={:.2f} silverPct={:.2f}",
                caster->GetName(), spellInfo->Id, uint32(i), baseValue, scaledValue, pct, silverReductionPct);
        }
    }
}

void ApplyOrRefreshSilverState(Unit* target, float reductionPct)
{
    if (!target || reductionPct <= 0.0f)
        return;

    uint32 const nowMs = uint32(GameTime::GetGameTimeMS().count());
    std::lock_guard<std::mutex> lock(SpellMasteryPaladinStateMutex);
    ConsecrationSilverState& state = ConsecrationSilverStates[uint32(target->GetGUID().GetCounter())];
    state.DamageReductionPct = std::max(state.DamageReductionPct, reductionPct);
    state.ExpiresAtMs = nowMs + CONSECRATION_SILVER_STATE_TTL_MS;
}

float GetSilverReductionPct(Unit* unitCaster)
{
    if (!unitCaster)
        return 0.0f;

    uint32 const casterGuid = uint32(unitCaster->GetGUID().GetCounter());
    std::lock_guard<std::mutex> lock(SpellMasteryPaladinStateMutex);
    auto itr = ConsecrationSilverStates.find(casterGuid);
    if (itr == ConsecrationSilverStates.end())
        return 0.0f;

    uint32 const nowMs = uint32(GameTime::GetGameTimeMS().count());
    if (itr->second.ExpiresAtMs <= nowMs)
    {
        ConsecrationSilverStates.erase(itr);
        return 0.0f;
    }

    return itr->second.DamageReductionPct;
}

void ApplyOrRefreshGoldState(Player* paladin, uint8 maxStacks, float bonusPctPerStack)
{
    if (!paladin || maxStacks == 0 || bonusPctPerStack <= 0.0f)
        return;

    uint32 const nowMs = uint32(GameTime::GetGameTimeMS().count());
    std::lock_guard<std::mutex> lock(SpellMasteryPaladinStateMutex);
    ConsecrationGoldState& state = ConsecrationGoldStates[uint32(paladin->GetGUID().GetCounter())];
    if (state.ExpiresAtMs <= nowMs)
        state.Stacks = 0;

    state.Stacks = std::min<uint8>(maxStacks, uint8(state.Stacks + 1));
    state.DamageBonusPctPerStack = bonusPctPerStack;
    state.ExpiresAtMs = nowMs + CONSECRATION_GOLD_STATE_TTL_MS;
}

float GetGoldDamageBonusPct(Player* paladin)
{
    if (!paladin)
        return 0.0f;

    uint32 const paladinGuid = uint32(paladin->GetGUID().GetCounter());
    std::lock_guard<std::mutex> lock(SpellMasteryPaladinStateMutex);
    auto itr = ConsecrationGoldStates.find(paladinGuid);
    if (itr == ConsecrationGoldStates.end())
        return 0.0f;

    uint32 const nowMs = uint32(GameTime::GetGameTimeMS().count());
    if (itr->second.ExpiresAtMs <= nowMs || itr->second.Stacks == 0)
    {
        ConsecrationGoldStates.erase(itr);
        return 0.0f;
    }

    return float(itr->second.Stacks) * itr->second.DamageBonusPctPerStack;
}
}

void ClearSpellMasteryPaladinRuntimeStateForPlayer(uint32 guid)
{
    std::lock_guard<std::mutex> lock(SpellMasteryPaladinStateMutex);
    ConsecrationGoldStates.erase(guid);
    ConsecrationSilverStates.erase(guid);
}

class spell_pal_consecration_mastery : public SpellScript
{
    PrepareSpellScript(spell_pal_consecration_mastery);

    bool Load() override
    {
        if (!GetCaster() || !GetCaster()->IsPlayer())
            return false;

        _playerCaster = GetCaster()->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SpellMastery::SPELL_PALADIN_CONSECRATION_RANK_1)
            return false;

        _progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildConsecrationMasteryEffects(_progress, *_config);
        _isTriggeredCast = GetSpell()->IsTriggered();

        if (_effects.BronzeRadiusMultiplier > 1.0f)
            GetSpell()->SetSpellValue(SPELLVALUE_RADIUS_MOD, int32(std::lround(_effects.BronzeRadiusMultiplier * 10000.0f)));

        return true;
    }

    void HandleOnHit()
    {
        Unit* target = GetHitUnit();
        if (!target || _playerCaster->IsFriendlyTo(target))
            return;

        int32 hitDamage = GetHitDamage();
        if (hitDamage <= 0)
            return;

        hitDamage = SpellMastery::ApplyEarlyAccessSpellScale(_playerCaster, GetSpellInfo(), hitDamage);

        if (_effects.IronDamageBonusPct > 0.0f)
        {
            int32 const scaledDamage = int32(std::lround(float(hitDamage) * (1.0f + (_effects.IronDamageBonusPct / 100.0f))));
            hitDamage = std::max(hitDamage, scaledDamage);
            SetHitDamage(hitDamage);
        }

        if (!_xpAwarded && !_isTriggeredCast && SpellMastery::ShouldAwardSpellMasteryXp(_playerCaster, *_config, CONSECRATION_XP_GUARD_MS))
        {
            SpellMastery::AddSpellMasteryXp(_playerCaster, *_config, SpellMastery::SPELL_MASTERY_XP_PER_HIT);
            _xpAwarded = true;
        }

        if (_effects.SilverEnemyDamageReductionPct > 0.0f)
            ApplyOrRefreshSilverState(target, _effects.SilverEnemyDamageReductionPct);

        if (_effects.GoldMaxStacks > 0 && _effects.GoldDamageBonusPctPerStack > 0.0f)
            ApplyOrRefreshGoldState(_playerCaster, _effects.GoldMaxStacks, _effects.GoldDamageBonusPctPerStack);

        if (_effects.DiamondHealPctOfHitDamage > 0.0f && _playerCaster->IsAlive())
        {
            int32 const healAmount = std::max<int32>(1, int32(std::lround((float(hitDamage) * _effects.DiamondHealPctOfHitDamage) / 100.0f)));
            _playerCaster->ModifyHealth(healAmount);
        }
    }

    void HandleAfterCast()
    {
        if (_xpAwarded || _isTriggeredCast)
            return;

        if (SpellMastery::ShouldAwardSpellMasteryXp(_playerCaster, *_config, CONSECRATION_XP_GUARD_MS))
        {
            SpellMastery::AddSpellMasteryXp(_playerCaster, *_config, SpellMastery::SPELL_MASTERY_XP_PER_HIT);
            _xpAwarded = true;
        }
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_pal_consecration_mastery::HandleOnHit);
        AfterCast += SpellCastFn(spell_pal_consecration_mastery::HandleAfterCast);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    SpellMastery::SpellMasteryProgress _progress;
    ConsecrationMasteryEffects _effects;
    bool _isTriggeredCast = false;
    bool _xpAwarded = false;
};

class spell_mastery_prepare_paladin_spell_script : public AllSpellScript
{
public:
    spell_mastery_prepare_paladin_spell_script() : AllSpellScript("spell_mastery_prepare_paladin_spell_script", { ALLSPELLHOOK_ON_PREPARE, ALLSPELLHOOK_ON_CAST })
    {
    }

    void OnSpellPrepare(Spell* spell, Unit* caster, SpellInfo const* spellInfo) override
    {
        if (!spell || !caster || !spellInfo)
            return;

        float outgoingDamagePct = 0.0f;
        float silverReductionPct = 0.0f;

        if (caster->IsPlayer() && caster->ToPlayer()->getClass() == CLASS_PALADIN)
        {
            Player* paladin = caster->ToPlayer();
            SpellMastery::ManagedSpellConfig const* consecrationConfig =
                SpellMastery::GetManagedSpellConfigByBaseSpell(SpellMastery::SPELL_PALADIN_CONSECRATION_RANK_1);

            if (consecrationConfig)
            {
                SpellMastery::SpellMasteryProgress const& progress = SpellMastery::GetOrLoadSpellMasteryProgress(paladin, *consecrationConfig);
                ConsecrationMasteryEffects const effects = BuildConsecrationMasteryEffects(progress, *consecrationConfig);

                if (effects.BronzeRadiusMultiplier > 1.0f && SpellMastery::GetManagedSpellConfigForSpell(spellInfo->Id) == consecrationConfig)
                    spell->SetSpellValue(SPELLVALUE_RADIUS_MOD, int32(std::lround(effects.BronzeRadiusMultiplier * 10000.0f)));

                outgoingDamagePct += GetGoldDamageBonusPct(paladin);
            }
        }

        silverReductionPct = GetSilverReductionPct(caster);
        outgoingDamagePct -= silverReductionPct;
        ApplySpellDamagePercentModifier(spell, caster, spellInfo, outgoingDamagePct, silverReductionPct);
    }

    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* spellInfo, bool /*skipCheck*/) override
    {
        if (!spell || !caster || !spellInfo || !caster->IsPlayer() || spell->IsTriggered())
            return;

        SpellMastery::ManagedSpellConfig const* config = SpellMastery::GetManagedSpellConfigForSpell(spellInfo->Id);
        if (!config || config->BaseSpellId != SpellMastery::SPELL_PALADIN_CONSECRATION_RANK_1)
            return;

        Player* paladin = caster->ToPlayer();
        if (!paladin || paladin->getClass() != CLASS_PALADIN)
            return;

        if (SpellMastery::ShouldAwardSpellMasteryXp(paladin, *config, CONSECRATION_XP_GUARD_MS))
            SpellMastery::AddSpellMasteryXp(paladin, *config, SpellMastery::SPELL_MASTERY_XP_PER_HIT);
    }
};

void AddSC_spell_mastery_paladin()
{
    new spell_mastery_prepare_paladin_spell_script();
    RegisterSpellScript(spell_pal_consecration_mastery);
}
