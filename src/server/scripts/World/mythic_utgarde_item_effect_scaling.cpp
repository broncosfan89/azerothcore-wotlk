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

#include "Item.h"
#include "Player.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"

#include <cmath>

namespace
{
uint32 constexpr MYTHIC_ITEM_LEGACY_MIN_ENTRY = 59001;
uint32 constexpr MYTHIC_ITEM_LEGACY_MAX_ENTRY = 60509;
uint32 constexpr MYTHIC_ITEM_LEGACY_BASE_OFFSET = 59000;
uint32 constexpr MYTHIC_ITEM_LEGACY_LEVEL_STRIDE = 100;
uint32 constexpr MYTHIC_ITEM_WOTLK_BASE_OFFSET = 70000000;
uint32 constexpr MYTHIC_ITEM_WOTLK_LEVEL_STRIDE = 1000000;
uint8 constexpr MYTHIC_ITEM_LEVEL_CAP = 15;
float constexpr MYTHIC_ITEM_EFFECT_BASE_MULTIPLIER = 1.35f;
float constexpr MYTHIC_ITEM_EFFECT_PER_LEVEL_MULTIPLIER = 0.05f;

bool TryGetMythicItemLevelFromEntry(uint32 entry, uint8& outLevel)
{
    if (entry >= MYTHIC_ITEM_LEGACY_MIN_ENTRY && entry <= MYTHIC_ITEM_LEGACY_MAX_ENTRY)
    {
        uint32 const slotSuffix = entry % 100;
        if (slotSuffix >= 1 && slotSuffix <= 9)
        {
            outLevel = uint8((entry - MYTHIC_ITEM_LEGACY_BASE_OFFSET) / MYTHIC_ITEM_LEGACY_LEVEL_STRIDE);
            return true;
        }
    }

    if (entry < MYTHIC_ITEM_WOTLK_BASE_OFFSET)
        return false;

    uint32 const relative = entry - MYTHIC_ITEM_WOTLK_BASE_OFFSET;
    uint32 const level = relative / MYTHIC_ITEM_WOTLK_LEVEL_STRIDE;
    uint32 const sourceEntry = relative % MYTHIC_ITEM_WOTLK_LEVEL_STRIDE;

    if (level > MYTHIC_ITEM_LEVEL_CAP || sourceEntry == 0)
        return false;

    outLevel = uint8(level);
    return true;
}

uint8 GetHighestMatchingMythicItemLevelForSpell(Player* player, uint32 spellId)
{
    if (!player || !spellId)
        return 0;

    uint8 bestLevel = 0;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
            continue;

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto)
            continue;

        uint8 mythicLevel = 0;
        if (!TryGetMythicItemLevelFromEntry(proto->ItemId, mythicLevel))
            continue;

        bool hasMatchingEquipSpell = false;
        for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
        {
            if (proto->Spells[i].SpellId != spellId)
                continue;

            uint8 const trigger = proto->Spells[i].SpellTrigger;
            if (trigger == ITEM_SPELLTRIGGER_ON_EQUIP ||
                trigger == ITEM_SPELLTRIGGER_ON_USE ||
                trigger == ITEM_SPELLTRIGGER_CHANCE_ON_HIT ||
                trigger == ITEM_SPELLTRIGGER_ON_NO_DELAY_USE)
            {
                hasMatchingEquipSpell = true;
                break;
            }
        }

        if (!hasMatchingEquipSpell)
            continue;

        bestLevel = std::max<uint8>(bestLevel, mythicLevel);
    }

    return bestLevel;
}
}

class spell_item_mythic_utgarde_effect_scaling : public AuraScript
{
    PrepareAuraScript(spell_item_mythic_utgarde_effect_scaling);

    void HandleCalcAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        // Scale only obvious power gains; avoid amplifying negative auras/debuff-style effects.
        if (amount <= 0)
            return;

        Player* owner = GetUnitOwner() ? GetUnitOwner()->ToPlayer() : nullptr;
        if (!owner)
            return;

        uint8 const mythicLevel = GetHighestMatchingMythicItemLevelForSpell(owner, GetSpellInfo()->Id);
        float const multiplier = MYTHIC_ITEM_EFFECT_BASE_MULTIPLIER + (MYTHIC_ITEM_EFFECT_PER_LEVEL_MULTIPLIER * float(mythicLevel));
        if (multiplier <= 1.0f)
            return;

        amount = int32(std::lround(float(amount) * multiplier));
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_item_mythic_utgarde_effect_scaling::HandleCalcAmount, EFFECT_ALL, SPELL_AURA_ANY);
    }
};

class spell_item_mythic_utgarde_effect_scaling_loader : public SpellScriptLoader
{
public:
    spell_item_mythic_utgarde_effect_scaling_loader() : SpellScriptLoader("spell_item_mythic_utgarde_effect_scaling") { }

    AuraScript* GetAuraScript() const override
    {
        return new spell_item_mythic_utgarde_effect_scaling();
    }
};

void AddSC_mythic_utgarde_item_effect_scaling()
{
    new spell_item_mythic_utgarde_effect_scaling_loader();
}
