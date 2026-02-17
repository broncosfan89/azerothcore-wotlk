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

#include "Player.h"
#include "PlayerScript.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "DatabaseEnv.h"
#include <array>

enum ApprenticeAnglerQuestEnum
{
    QUEST_APPRENTICE_ANGLER = 8194
};

namespace
{
uint32 constexpr SPELL_SCROLL_OF_STRENGTH_RANK_1 = 8118;
uint32 constexpr SPELL_SCROLL_OF_AGILITY_RANK_1 = 8115;
uint32 constexpr SPELL_SCROLL_OF_STAMINA_RANK_1 = 8099;
uint32 constexpr SPELL_SCROLL_OF_INTELLECT_RANK_1 = 8096;
uint32 constexpr SPELL_SCROLL_OF_SPIRIT_RANK_1 = 8112;
char constexpr PERMANENT_PRIMARY_STAT_SCROLL_TABLE[] = "character_permanent_stamina_scroll_bonus";

bool TryGetPrimaryStatFromScrollSpell(SpellInfo const* spellInfo, Stats& stat)
{
    if (!spellInfo || spellInfo->GetSpellSpecific() != SPELL_SPECIFIC_SCROLL)
        return false;

    SpellInfo const* firstRank = spellInfo->GetFirstRankSpell();
    if (!firstRank)
        return false;

    switch (firstRank->Id)
    {
        case SPELL_SCROLL_OF_STRENGTH_RANK_1:
            stat = STAT_STRENGTH;
            return true;
        case SPELL_SCROLL_OF_AGILITY_RANK_1:
            stat = STAT_AGILITY;
            return true;
        case SPELL_SCROLL_OF_STAMINA_RANK_1:
            stat = STAT_STAMINA;
            return true;
        case SPELL_SCROLL_OF_INTELLECT_RANK_1:
            stat = STAT_INTELLECT;
            return true;
        case SPELL_SCROLL_OF_SPIRIT_RANK_1:
            stat = STAT_SPIRIT;
            return true;
        default:
            return false;
    }
}

uint32 GetPrimaryStatBonusFromScrollSpell(SpellInfo const* spellInfo, Player* caster, Stats stat)
{
    if (!spellInfo || !caster)
        return 0;

    uint32 totalBonus = 0;
    for (SpellEffectInfo const& effect : spellInfo->Effects)
    {
        if (!effect.IsAura(SPELL_AURA_MOD_STAT))
            continue;

        if (effect.MiscValue != int32(stat) && effect.MiscValue != -1)
            continue;

        int32 const bonus = effect.CalcValue(caster);
        if (bonus > 0)
            totalBonus += uint32(bonus);
    }

    return totalBonus;
}

void ApplyPermanentPrimaryStatScrollBonus(Player* player, std::array<uint32, MAX_STATS> const& bonuses)
{
    if (!player)
        return;

    for (uint8 stat = STAT_STRENGTH; stat < MAX_STATS; ++stat)
    {
        uint32 const bonus = bonuses[stat];
        if (!bonus)
            continue;

        player->HandleStatFlatModifier(UnitMods(UNIT_MOD_STAT_START + stat), TOTAL_VALUE, float(bonus), true);
    }
}
}

class QuestApprenticeAnglerPlayerScript : public PlayerScript
{
public:
    QuestApprenticeAnglerPlayerScript() : PlayerScript("QuestApprenticeAnglerPlayerScript", {PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST})
    {
    }

    void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
    {
        if (quest->GetQuestId() == QUEST_APPRENTICE_ANGLER)
        {
            uint32 level = player->GetLevel();
            int32 moneyRew = 0;
            if (level <= 10)
                moneyRew = 85;
            else if (level <= 60)
                moneyRew = 2300;
            else if (level <= 69)
                moneyRew = 9000;
            else if (level <= 70)
                moneyRew = 11200;
            else if (level <= 79)
                moneyRew = 12000;
            else
                moneyRew = 19000;

            player->ModifyMoney(moneyRew);
            player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_MONEY_FROM_QUEST_REWARD, uint32(moneyRew));
            player->SaveToDB(false, false);

            // Send packet with money
            WorldPacket data(SMSG_QUESTGIVER_QUEST_COMPLETE, (4 + 4 + 4 + 4 + 4));
            data << uint32(quest->GetQuestId());
            data << uint32(0);
            data << uint32(moneyRew);
            data << uint32(0);
            data << uint32(0);
            data << uint32(0);
            player->SendDirectMessage(&data);
        }
    }
};

class PermanentPrimaryStatScrollBonusPlayerScript : public PlayerScript
{
public:
    PermanentPrimaryStatScrollBonusPlayerScript() : PlayerScript("PermanentPrimaryStatScrollBonusPlayerScript", { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_SPELL_CAST })
    {
    }

    void OnPlayerLogin(Player* player) override
    {
        if (!player)
            return;

        QueryResult result = CharacterDatabase.Query(
            "SELECT `strength_bonus`, `agility_bonus`, `stamina_bonus`, `intellect_bonus`, `spirit_bonus` FROM `{}` WHERE `guid` = {}",
            PERMANENT_PRIMARY_STAT_SCROLL_TABLE, player->GetGUID().GetCounter());

        if (!result)
            return;

        std::array<uint32, MAX_STATS> bonuses = {};
        Field* fields = result->Fetch();
        bonuses[STAT_STRENGTH] = fields[0].Get<uint32>();
        bonuses[STAT_AGILITY] = fields[1].Get<uint32>();
        bonuses[STAT_STAMINA] = fields[2].Get<uint32>();
        bonuses[STAT_INTELLECT] = fields[3].Get<uint32>();
        bonuses[STAT_SPIRIT] = fields[4].Get<uint32>();

        ApplyPermanentPrimaryStatScrollBonus(player, bonuses);
    }

    void OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
    {
        if (!player || !spell || !spell->m_CastItem)
            return;

        Stats stat = STAT_STRENGTH;
        if (!TryGetPrimaryStatFromScrollSpell(spell->GetSpellInfo(), stat))
            return;

        uint32 const statBonus = GetPrimaryStatBonusFromScrollSpell(spell->GetSpellInfo(), player, stat);
        if (!statBonus)
            return;

        std::array<uint32, MAX_STATS> bonuses = {};
        bonuses[stat] = statBonus;

        CharacterDatabase.Execute(
            "INSERT INTO `{}` (`guid`, `strength_bonus`, `agility_bonus`, `stamina_bonus`, `intellect_bonus`, `spirit_bonus`) "
            "VALUES ({}, {}, {}, {}, {}, {}) "
            "ON DUPLICATE KEY UPDATE "
            "`strength_bonus` = `strength_bonus` + VALUES(`strength_bonus`), "
            "`agility_bonus` = `agility_bonus` + VALUES(`agility_bonus`), "
            "`stamina_bonus` = `stamina_bonus` + VALUES(`stamina_bonus`), "
            "`intellect_bonus` = `intellect_bonus` + VALUES(`intellect_bonus`), "
            "`spirit_bonus` = `spirit_bonus` + VALUES(`spirit_bonus`)",
            PERMANENT_PRIMARY_STAT_SCROLL_TABLE, player->GetGUID().GetCounter(),
            bonuses[STAT_STRENGTH], bonuses[STAT_AGILITY], bonuses[STAT_STAMINA], bonuses[STAT_INTELLECT], bonuses[STAT_SPIRIT]);

        ApplyPermanentPrimaryStatScrollBonus(player, bonuses);
    }
};

void AddSC_player_scripts()
{
    new QuestApprenticeAnglerPlayerScript();
    new PermanentPrimaryStatScrollBonusPlayerScript();
}
