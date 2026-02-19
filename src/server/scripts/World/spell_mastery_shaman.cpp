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
#include "SpellScript.h"
#include "SpellScriptLoader.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace
{
struct ChainLightningMasteryEffects
{
    float IronDamageBonusPct = 0.0f;
    float BronzeJumpReductionPct = 30.0f;
    uint8 SilverExtraTargets = 0;
    uint8 GoldMaxStacks = 0;
    float GoldNatureTakenPctPerStack = 0.0f;
    bool HasDiamondInstantReset = false;
};

struct ChainLightningGoldDebuffKey
{
    uint32 CasterGuid;
    uint32 TargetGuid;

    bool operator==(ChainLightningGoldDebuffKey const& other) const
    {
        return CasterGuid == other.CasterGuid && TargetGuid == other.TargetGuid;
    }
};

struct ChainLightningGoldDebuffKeyHash
{
    std::size_t operator()(ChainLightningGoldDebuffKey const& key) const
    {
        return (std::size_t(key.CasterGuid) << 32) ^ key.TargetGuid;
    }
};

struct ChainLightningGoldDebuffState
{
    uint8 Stacks = 0;
    uint32 ExpiresAtMs = 0;
};

uint32 constexpr CHAIN_LIGHTNING_XP_GUARD_MS = 250;
uint32 constexpr LAVA_BURST_XP_GUARD_MS = 250;
uint32 constexpr CHAIN_LIGHTNING_GOLD_DEBUFF_TTL_MS = 15000;
uint32 constexpr CHAIN_LIGHTNING_BASE_TOTAL_TARGETS = 3;
uint32 constexpr CHAIN_LIGHTNING_MAX_TOTAL_TARGETS = 12;
float constexpr CHAIN_LIGHTNING_BASE_JUMP_MULTIPLIER = 0.70f;
uint32 constexpr EARLY_ACCESS_LEVEL_OFFSET = 4;
float constexpr EARLY_ACCESS_MIN_SCALE = 0.05f;

std::unordered_map<ChainLightningGoldDebuffKey, ChainLightningGoldDebuffState, ChainLightningGoldDebuffKeyHash> ChainLightningGoldDebuffStates;

ChainLightningMasteryEffects BuildChainLightningMasteryEffects(SpellMastery::SpellMasteryProgress const& progress, SpellMastery::ManagedSpellConfig const& config)
{
    ChainLightningMasteryEffects effects;

    uint8 ironLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_IRON, config);
    uint8 bronzeLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_BRONZE, config);
    uint8 silverLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_SILVER, config);
    uint8 goldLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_GOLD, config);
    uint8 diamondLevel = SpellMastery::GetEffectiveTierLevel(progress, SpellMastery::SPELL_MASTERY_TIER_DIAMOND, config);

    if (ironLevel > 0)
        effects.IronDamageBonusPct = float(ironLevel) * 15.0f;

    if (bronzeLevel > 0)
        effects.BronzeJumpReductionPct = std::max(0.0f, 30.0f * (1.0f - (float(bronzeLevel) / 10.0f)));

    if (silverLevel > 0)
        effects.SilverExtraTargets = silverLevel;

    if (goldLevel > 0)
    {
        effects.GoldMaxStacks = goldLevel;
        effects.GoldNatureTakenPctPerStack = 2.0f + (float(goldLevel - 1) * (3.0f / 9.0f));
    }

    effects.HasDiamondInstantReset = diamondLevel > 0;
    return effects;
}

float ComputeEarlyAccessSpellScale(Player* caster, SpellInfo const* spellInfo)
{
    if (!caster || !spellInfo)
        return 1.0f;

    uint32 const playerLevel = std::max<uint32>(1, caster->GetLevel());
    uint32 const naturalLevel = std::max<uint32>(spellInfo->SpellLevel, spellInfo->BaseLevel);
    if (naturalLevel <= 1 || playerLevel >= naturalLevel)
        return 1.0f;

    float const adjustedPlayer = float(playerLevel + EARLY_ACCESS_LEVEL_OFFSET);
    float const adjustedNatural = float(naturalLevel + EARLY_ACCESS_LEVEL_OFFSET);
    return std::clamp(adjustedPlayer / adjustedNatural, EARLY_ACCESS_MIN_SCALE, 1.0f);
}

uint8 GetActiveGoldDebuffStacks(Player* caster, Unit* target)
{
    if (!caster || !target)
        return 0;

    ChainLightningGoldDebuffKey const key
    {
        uint32(caster->GetGUID().GetCounter()),
        uint32(target->GetGUID().GetCounter())
    };

    auto itr = ChainLightningGoldDebuffStates.find(key);
    if (itr == ChainLightningGoldDebuffStates.end())
        return 0;

    uint32 const nowMs = uint32(GameTime::GetGameTimeMS().count());
    if (itr->second.ExpiresAtMs <= nowMs)
    {
        ChainLightningGoldDebuffStates.erase(itr);
        return 0;
    }

    return std::max<uint8>(1, itr->second.Stacks);
}
}

void ClearSpellMasteryShamanRuntimeStateForPlayer(uint32 guid)
{
    for (auto itr = ChainLightningGoldDebuffStates.begin(); itr != ChainLightningGoldDebuffStates.end();)
    {
        if (itr->first.CasterGuid == guid || itr->first.TargetGuid == guid)
            itr = ChainLightningGoldDebuffStates.erase(itr);
        else
            ++itr;
    }
}

class spell_sha_chain_lightning_mastery : public SpellScript
{
    PrepareSpellScript(spell_sha_chain_lightning_mastery);

    bool Load() override
    {
        if (!GetCaster() || !GetCaster()->IsPlayer())
            return false;

        _playerCaster = GetCaster()->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SpellMastery::SPELL_SHAMAN_CHAIN_LIGHTNING_RANK_1)
            return false;

        _progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
        _effects = BuildChainLightningMasteryEffects(_progress, *_config);
        _isTriggeredCast = GetSpell()->IsTriggered();

        if (_effects.SilverExtraTargets > 0)
        {
            uint32 const totalTargets = std::min<uint32>(CHAIN_LIGHTNING_MAX_TOTAL_TARGETS, CHAIN_LIGHTNING_BASE_TOTAL_TARGETS + _effects.SilverExtraTargets);
            GetSpell()->SetSpellValue(SPELLVALUE_MAX_TARGETS, int32(totalTargets));
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

        float const earlyScale = ComputeEarlyAccessSpellScale(_playerCaster, GetSpellInfo());
        hitDamage = std::max<int32>(1, int32(std::lround(float(hitDamage) * earlyScale)));

        uint8 const jumpIndex = _processedTargets++;

        if (jumpIndex > 0 && _effects.BronzeJumpReductionPct < 30.0f)
        {
            float const defaultDrop = std::pow(CHAIN_LIGHTNING_BASE_JUMP_MULTIPLIER, float(jumpIndex));
            if (defaultDrop > 0.0001f)
            {
                float const estimatedBaseDamage = float(hitDamage) / defaultDrop;
                float const customMultiplier = 1.0f - (_effects.BronzeJumpReductionPct / 100.0f);
                float const adjustedDamage = estimatedBaseDamage * std::pow(customMultiplier, float(jumpIndex));
                hitDamage = std::max<int32>(hitDamage, int32(std::lround(adjustedDamage)));
            }
        }

        float totalBonusPct = _effects.IronDamageBonusPct;
        if (_effects.GoldNatureTakenPctPerStack > 0.0f)
        {
            uint8 const stacks = GetActiveGoldDebuffStacks(_playerCaster, target);
            if (stacks > 0)
                totalBonusPct += float(stacks) * _effects.GoldNatureTakenPctPerStack;
        }

        if (totalBonusPct > 0.0f)
        {
            int32 const scaledDamage = int32(std::lround(float(hitDamage) * (1.0f + (totalBonusPct / 100.0f))));
            hitDamage = std::max(hitDamage, scaledDamage);
        }

        SetHitDamage(hitDamage);

        if (!_xpAwarded && !_isTriggeredCast && SpellMastery::ShouldAwardSpellMasteryXp(_playerCaster, *_config, CHAIN_LIGHTNING_XP_GUARD_MS))
        {
            SpellMastery::AddSpellMasteryXp(_playerCaster, *_config, SpellMastery::SPELL_MASTERY_XP_PER_HIT);
            SpellMastery::SpellMasteryProgress const& progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
            SpellMastery::SendMasteryAddonMessageForProgress(_playerCaster, *_config, progress);
            _xpAwarded = true;
        }
    }

    void HandleAfterHit()
    {
        Unit* target = GetHitUnit();
        if (!target)
            return;

        if (!_playerCaster->IsValidAttackTarget(target))
            return;

        ApplyOrRefreshGoldDebuff(target);

        if (_effects.HasDiamondInstantReset && !_isTriggeredCast && !_diamondTriggered)
        {
            _diamondTriggered = true;
            _playerCaster->RemoveSpellCooldown(_config->AllowedSpellId, true);
            _playerCaster->CastSpell(
                target,
                _config->AllowedSpellId,
                TriggerCastFlags(TRIGGERED_IGNORE_GCD | TRIGGERED_IGNORE_CAST_IN_PROGRESS | TRIGGERED_IGNORE_POWER_AND_REAGENT_COST | TRIGGERED_IGNORE_SPELL_AND_CATEGORY_CD | TRIGGERED_CAST_DIRECTLY));
        }
    }

    void ApplyOrRefreshGoldDebuff(Unit* target)
    {
        if (!_effects.GoldMaxStacks || _effects.GoldNatureTakenPctPerStack <= 0.0f || !target)
            return;

        ChainLightningGoldDebuffKey const key
        {
            uint32(_playerCaster->GetGUID().GetCounter()),
            uint32(target->GetGUID().GetCounter())
        };

        uint32 const nowMs = uint32(GameTime::GetGameTimeMS().count());
        ChainLightningGoldDebuffState& state = ChainLightningGoldDebuffStates[key];
        if (state.ExpiresAtMs <= nowMs)
            state.Stacks = 0;

        state.Stacks = std::min<uint8>(_effects.GoldMaxStacks, uint8(state.Stacks + 1));
        state.ExpiresAtMs = nowMs + CHAIN_LIGHTNING_GOLD_DEBUFF_TTL_MS;
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_sha_chain_lightning_mastery::HandleDirectDamage, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
        AfterHit += SpellHitFn(spell_sha_chain_lightning_mastery::HandleAfterHit);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    SpellMastery::SpellMasteryProgress _progress;
    ChainLightningMasteryEffects _effects;
    uint8 _processedTargets = 0;
    bool _isTriggeredCast = false;
    bool _xpAwarded = false;
    bool _diamondTriggered = false;
};

class spell_sha_lava_burst_mastery : public SpellScript
{
    PrepareSpellScript(spell_sha_lava_burst_mastery);

    bool Load() override
    {
        if (!GetCaster() || !GetCaster()->IsPlayer())
            return false;

        _playerCaster = GetCaster()->ToPlayer();
        _config = SpellMastery::GetManagedSpellConfigForSpell(GetSpellInfo()->Id);
        if (!_config || _config->BaseSpellId != SpellMastery::SPELL_SHAMAN_LAVA_BURST_RANK_1)
            return false;

        _isTriggeredCast = GetSpell()->IsTriggered();
        return true;
    }

    void HandleOnHitDamage()
    {
        Unit* target = GetHitUnit();
        if (!target || _playerCaster->IsFriendlyTo(target))
            return;

        int32 hitDamage = GetHitDamage();
        if (hitDamage <= 0)
            return;

        float const earlyScale = ComputeEarlyAccessSpellScale(_playerCaster, GetSpellInfo());
        hitDamage = std::max<int32>(1, int32(std::lround(float(hitDamage) * earlyScale)));
        SetHitDamage(hitDamage);

        if (!_xpAwarded && !_isTriggeredCast && SpellMastery::ShouldAwardSpellMasteryXp(_playerCaster, *_config, LAVA_BURST_XP_GUARD_MS))
        {
            SpellMastery::AddSpellMasteryXp(_playerCaster, *_config, SpellMastery::SPELL_MASTERY_XP_PER_HIT);
            SpellMastery::SpellMasteryProgress const& progress = SpellMastery::GetOrLoadSpellMasteryProgress(_playerCaster, *_config);
            SpellMastery::SendMasteryAddonMessageForProgress(_playerCaster, *_config, progress);
            _xpAwarded = true;
        }
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_sha_lava_burst_mastery::HandleOnHitDamage);
    }

private:
    Player* _playerCaster = nullptr;
    SpellMastery::ManagedSpellConfig const* _config = nullptr;
    bool _isTriggeredCast = false;
    bool _xpAwarded = false;
};

void AddSC_spell_mastery_shaman()
{
    RegisterSpellScript(spell_sha_chain_lightning_mastery);
    RegisterSpellScript(spell_sha_lava_burst_mastery);
}
