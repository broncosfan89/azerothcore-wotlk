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

#include "InstanceMapScript.h"
#include "Config.h"
#include "Log.h"
#include "LootMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "utgarde_keep.h"

#include <array>
#include <algorithm>
#include <cmath>

namespace
{
uint32 constexpr ITEM_EMBLEM_OF_CONQUEST = 45624;
uint32 constexpr ITEM_EMBLEM_OF_TRIUMPH = 47241;

bool IsMythicLootBossEntry(uint32 entry)
{
    return entry == NPC_KELESETH || entry == NPC_INGVAR || entry == NPC_SKARVALD || entry == NPC_DALRONN;
}

std::array<uint32, 3> constexpr ITEM_POOL_KELESETH_M0 =
{
    980001,
    980002,
    980003
};

std::array<uint32, 3> constexpr ITEM_POOL_SKARVALD_DALRONN_M0 =
{
    980004,
    980005,
    980006
};

std::array<uint32, 3> constexpr ITEM_POOL_INGVAR_M0 =
{
    980007,
    980008,
    980009
};

uint32 GetRandomMythicUtgardeItemForDataId(uint32 dataId)
{
    std::array<uint32, 3> const* pool = nullptr;
    switch (dataId)
    {
        case DATA_KELESETH:
            pool = &ITEM_POOL_KELESETH_M0;
            break;
        case DATA_DALRONN:
        case DATA_SKARVALD:
            pool = &ITEM_POOL_SKARVALD_DALRONN_M0;
            break;
        case DATA_INGVAR:
            pool = &ITEM_POOL_INGVAR_M0;
            break;
        default:
            break;
    }

    if (!pool)
        return 0;

    return (*pool)[urand(0, uint32(pool->size() - 1))];
}
}

ObjectData const creatureData[] =
{
    { NPC_KELESETH,           DATA_KELESETH           },
    { NPC_DALRONN,            DATA_DALRONN            },
    { NPC_SKARVALD,           DATA_SKARVALD           },
    { NPC_DALRONN_GHOST,      DATA_DALRONN_GHOST      },
    { NPC_SKARVALD_GHOST,     DATA_SKARVALD_GHOST     },
    { NPC_INGVAR,             DATA_INGVAR             },
    { NPC_DARK_RANGER_MARRAH, DATA_DARK_RANGER_MARRAH },
    { 0,                      0                       }
};

class instance_utgarde_keep : public InstanceMapScript
{
public:
    instance_utgarde_keep() : InstanceMapScript("instance_utgarde_keep", MAP_UTGARDE_KEEP) { }

    InstanceScript* GetInstanceScript(InstanceMap* pMap) const override
    {
        return new instance_utgarde_keep_InstanceMapScript(pMap);
    }

    struct instance_utgarde_keep_InstanceMapScript : public InstanceScript
    {
        instance_utgarde_keep_InstanceMapScript(Map* pMap) : InstanceScript(pMap)
        {
            SetHeaders(DataHeader);
            SetBossNumber(EncounterCount);
            LoadObjectData(creatureData, nullptr);
        }

        uint32 m_auiEncounter[MAX_ENCOUNTER];
        uint32 ForgeEventMask;
        std::string str_data;
        bool MythicEnabled;
        uint8 MythicLevel;
        float MythicHealthMultiplier;
        float MythicDamageMultiplier;
        uint8 MythicRewardUpgradeLevel;
        uint32 MythicRewardBaseCount;
        uint32 MythicRewardPerLevelDivisor;
        bool MythicRewardGranted[MAX_ENCOUNTER];

        ObjectGuid GO_ForgeBellowGUID[3];
        ObjectGuid GO_ForgeFireGUID[3];
        ObjectGuid GO_ForgeAnvilGUID[3];
        ObjectGuid GO_PortcullisGUID[2];

        ObjectGuid NPC_SpecialDrakeGUID;
        bool bRocksAchiev;

        void ApplyMythicLootModeToCreature(uint32 dataId)
        {
            if (!MythicEnabled)
                return;

            if (Creature* creature = GetCreature(dataId))
                creature->SetLootMode(LOOT_MODE_HARD_MODE_1);
        }

        Player* GetFallbackLootRecipient() const
        {
            if (!instance)
                return nullptr;

            Map::PlayerList const& players = instance->GetPlayers();
            for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
                if (Player* player = itr->GetSource())
                    return player;

            return nullptr;
        }

        void RebuildMythicBossLoot(uint32 dataId)
        {
            if (!MythicEnabled)
            {
                LOG_INFO("scripts", "UK Mythic loot rebuild skipped: dataId {} (mythic disabled)", dataId);
                return;
            }

            Creature* creature = GetCreature(dataId);
            if (!creature || creature->IsAlive())
            {
                LOG_INFO("scripts", "UK Mythic loot rebuild skipped: dataId {} creature {} alive {}", dataId, creature ? creature->GetEntry() : 0, creature ? uint32(creature->IsAlive()) : 0);
                return;
            }

            creature->SetLootMode(LOOT_MODE_HARD_MODE_1);

            Player* lootRecipient = creature->GetLootRecipient();
            if (!lootRecipient || !lootRecipient->IsInMap(creature))
            {
                lootRecipient = GetFallbackLootRecipient();
                if (lootRecipient)
                    creature->SetLootRecipient(lootRecipient);
            }

            creature->loot.clear();
            if (uint32 lootid = creature->GetCreatureTemplate()->lootid)
                creature->loot.FillLoot(lootid, LootTemplates_Creature, lootRecipient, false, false, creature->GetLootMode(), creature);
            else
                LOG_INFO("scripts", "UK Mythic loot rebuild warning: boss {} has no lootid", creature->GetEntry());

            if (creature->GetLootMode())
                creature->loot.generateMoneyLoot(creature->GetCreatureTemplate()->mingold, creature->GetCreatureTemplate()->maxgold);

            bool hasValidLootItem = false;
            for (LootItem const& lootItem : creature->loot.items)
            {
                if (sObjectMgr->GetItemTemplate(lootItem.itemid))
                {
                    hasValidLootItem = true;
                    break;
                }
            }

            if (!hasValidLootItem)
            {
                if (uint32 forcedItemId = GetRandomMythicUtgardeItemForDataId(dataId))
                {
                    LootStoreItem forcedItem(forcedItemId, 0, 100.0f, false, LOOT_MODE_DEFAULT, 0, 1, 1);
                    creature->loot.AddItem(forcedItem);
                    LOG_INFO("scripts", "UK Mythic loot rebuild fallback injected item {} for bossEntry {} dataId {}", forcedItemId, creature->GetEntry(), dataId);
                }
            }

            std::string const recipientGuid = lootRecipient ? lootRecipient->GetGUID().ToString() : "none";
            LOG_INFO("scripts", "UK Mythic loot rebuild result: bossEntry {} dataId {} lootMode {} recipient {} items {} gold {} empty {}",
                creature->GetEntry(), dataId, creature->GetLootMode(), recipientGuid, creature->loot.items.size(), creature->loot.gold, uint32(creature->loot.empty()));

            if (!creature->loot.empty())
            {
                creature->SetDynamicFlag(UNIT_DYNFLAG_LOOTABLE | UNIT_DYNFLAG_TAPPED | UNIT_DYNFLAG_TAPPED_BY_PLAYER);
                creature->DestroyForVisiblePlayers();
                creature->SetVisible(true);
            }
        }

        void Initialize() override
        {
            memset(&m_auiEncounter, 0, sizeof(m_auiEncounter));
            ForgeEventMask = 0;
            memset(&MythicRewardGranted, 0, sizeof(MythicRewardGranted));

            bRocksAchiev = true;

            MythicEnabled = false;
            MythicLevel = 0;
            MythicHealthMultiplier = 1.0f;
            MythicDamageMultiplier = 1.0f;
            MythicRewardUpgradeLevel = uint8(sConfigMgr->GetOption<uint32>("Custom.MythicUtgardeKeep.RewardUpgradeLevel", 5));
            MythicRewardBaseCount = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("Custom.MythicUtgardeKeep.RewardBaseCount", 1));
            MythicRewardPerLevelDivisor = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("Custom.MythicUtgardeKeep.RewardPerLevelDivisor", 2));

            if (instance && instance->IsHeroic() && sConfigMgr->GetOption<bool>("Custom.MythicUtgardeKeep.Enable", false))
            {
                uint32 const maxLevel = std::max<uint32>(0, sConfigMgr->GetOption<uint32>("Custom.MythicUtgardeKeep.MaxLevel", 20));
                uint32 const configuredLevel = sConfigMgr->GetOption<uint32>("Custom.MythicUtgardeKeep.Level", 0);
                MythicLevel = uint8(std::min<uint32>(maxLevel, configuredLevel));

                float const healthM0 = sConfigMgr->GetOption<float>("Custom.MythicUtgardeKeep.HealthMultiplierM0", 1.30f);
                float const healthPerLevel = sConfigMgr->GetOption<float>("Custom.MythicUtgardeKeep.HealthMultiplierPerLevel", 0.15f);
                float const damageM0 = sConfigMgr->GetOption<float>("Custom.MythicUtgardeKeep.DamageMultiplierM0", 1.15f);
                float const damagePerLevel = sConfigMgr->GetOption<float>("Custom.MythicUtgardeKeep.DamageMultiplierPerLevel", 0.10f);

                MythicHealthMultiplier = std::max(1.0f, healthM0 + (healthPerLevel * float(MythicLevel)));
                MythicDamageMultiplier = std::max(1.0f, damageM0 + (damagePerLevel * float(MythicLevel)));
                MythicEnabled = true;
            }
        }

        bool IsEncounterInProgress() const override
        {
            if (InstanceScript::IsEncounterInProgress())
                return true;

            for (uint8 i = 0; i < MAX_ENCOUNTER; ++i)
                if (m_auiEncounter[i] == IN_PROGRESS) return true;

            return false;
        }

        bool SetBossState(uint32 type, EncounterState state) override
        {
            if (!InstanceScript::SetBossState(type, state))
                return false;

            if (type == DATA_KELESETH && state == NOT_STARTED)
                bRocksAchiev = true;

            if (MythicEnabled)
            {
                switch (type)
                {
                    case DATA_KELESETH:
                        ApplyMythicLootModeToCreature(DATA_KELESETH);
                        if (state == DONE)
                            RebuildMythicBossLoot(DATA_KELESETH);
                        break;
                    case DATA_DALRONN_AND_SKARVALD:
                        ApplyMythicLootModeToCreature(DATA_SKARVALD);
                        ApplyMythicLootModeToCreature(DATA_DALRONN);
                        break;
                    case DATA_INGVAR:
                        ApplyMythicLootModeToCreature(DATA_INGVAR);
                        if (state == DONE)
                            RebuildMythicBossLoot(DATA_INGVAR);
                        break;
                    default:
                        break;
                }
            }

            if (MythicEnabled && state == DONE && type < MAX_ENCOUNTER && !MythicRewardGranted[type])
            {
                uint32 rewardItemId = (MythicLevel >= MythicRewardUpgradeLevel) ? ITEM_EMBLEM_OF_TRIUMPH : ITEM_EMBLEM_OF_CONQUEST;
                uint32 rewardCount = MythicRewardBaseCount + (uint32(MythicLevel) / MythicRewardPerLevelDivisor);
                if (type == DATA_INGVAR)
                    ++rewardCount;

                Map::PlayerList const& players = instance->GetPlayers();
                for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
                {
                    Player* player = itr->GetSource();
                    if (!player)
                        continue;

                    player->AddItem(rewardItemId, rewardCount);
                    if (player->GetSession())
                        player->GetSession()->SendAreaTriggerMessage("Mythic {} reward: item {} x{}.", uint32(MythicLevel), rewardItemId, rewardCount);
                }

                MythicRewardGranted[type] = true;
            }

            return true;
        }

        void OnPlayerEnter(Player* plr) override
        {
            if (Creature* c = GetCreature(DATA_DARK_RANGER_MARRAH))
            {
                c->SetReactState(REACT_PASSIVE);
                c->SetUnitFlag(UNIT_FLAG_NON_ATTACKABLE);
                if (plr && plr->GetTeamId() == TEAM_HORDE)
                {
                    if (!c->IsVisible())
                        c->SetVisible(true);
                }
                else if (c->IsVisible())
                {
                    c->SetVisible(false);
                }
            }

            if (MythicEnabled && plr && plr->GetSession())
                plr->GetSession()->SendAreaTriggerMessage("Utgarde Keep Mythic {} active (HP x{:.2f}, Damage x{:.2f}).", uint32(MythicLevel), MythicHealthMultiplier, MythicDamageMultiplier);
        }

        void OnCreatureCreate(Creature* creature) override
        {
            if (MythicEnabled && creature && creature->IsHostileToPlayers() && !creature->IsTrigger() && !creature->IsTotem())
            {
                if (IsMythicLootBossEntry(creature->GetEntry()))
                    creature->SetLootMode(LOOT_MODE_HARD_MODE_1);
                else
                    creature->SetLootMode(0);

                uint32 const currentMaxHealth = creature->GetMaxHealth();
                if (currentMaxHealth > 0)
                {
                    uint32 const scaledMaxHealth = std::max<uint32>(1, uint32(std::lround(float(currentMaxHealth) * MythicHealthMultiplier)));
                    creature->SetMaxHealth(scaledMaxHealth);
                    if (creature->IsAlive())
                        creature->SetHealth(scaledMaxHealth);
                }

                auto scaleAttackDamage = [this, creature](WeaponAttackType attackType)
                {
                    float minDamage = creature->GetWeaponDamageRange(attackType, MINDAMAGE);
                    float maxDamage = creature->GetWeaponDamageRange(attackType, MAXDAMAGE);
                    if (minDamage <= 0.0f && maxDamage <= 0.0f)
                        return;

                    creature->SetBaseWeaponDamage(attackType, MINDAMAGE, std::max(1.0f, minDamage * MythicDamageMultiplier));
                    creature->SetBaseWeaponDamage(attackType, MAXDAMAGE, std::max(1.0f, maxDamage * MythicDamageMultiplier));
                    creature->UpdateDamagePhysical(attackType);
                };

                scaleAttackDamage(BASE_ATTACK);
                scaleAttackDamage(OFF_ATTACK);
                scaleAttackDamage(RANGED_ATTACK);
            }

            switch (creature->GetEntry())
            {
                case NPC_ENSLAVED_PROTO_DRAKE:
                    if (creature->GetPositionX() < 250.0f) NPC_SpecialDrakeGUID = creature->GetGUID();
                    break;
            }

            InstanceScript::OnCreatureCreate(creature);
        }

        void OnGameObjectCreate(GameObject* go) override
        {
            switch (go->GetEntry())
            {
                case GO_BELLOW_1:
                    GO_ForgeBellowGUID[0] = go->GetGUID();
                    if (ForgeEventMask & 1) HandleGameObject(ObjectGuid::Empty, true, go);
                    break;
                case GO_BELLOW_2:
                    GO_ForgeBellowGUID[1] = go->GetGUID();
                    if (ForgeEventMask & 2) HandleGameObject(ObjectGuid::Empty, true, go);
                    break;
                case GO_BELLOW_3:
                    GO_ForgeBellowGUID[2] = go->GetGUID();
                    if (ForgeEventMask & 4) HandleGameObject(ObjectGuid::Empty, true, go);
                    break;
                case GO_FORGEFIRE_1:
                    GO_ForgeFireGUID[0] = go->GetGUID();
                    if (ForgeEventMask & 1) HandleGameObject(ObjectGuid::Empty, true, go);
                    break;
                case GO_FORGEFIRE_2:
                    GO_ForgeFireGUID[1] = go->GetGUID();
                    if (ForgeEventMask & 2) HandleGameObject(ObjectGuid::Empty, true, go);
                    break;
                case GO_FORGEFIRE_3:
                    GO_ForgeFireGUID[2] = go->GetGUID();
                    if (ForgeEventMask & 4) HandleGameObject(ObjectGuid::Empty, true, go);
                    break;
                case GO_GLOWING_ANVIL_1:
                    GO_ForgeAnvilGUID[0] = go->GetGUID();
                    if (ForgeEventMask & 1) HandleGameObject(ObjectGuid::Empty, true, go);
                    break;
                case GO_GLOWING_ANVIL_2:
                    GO_ForgeAnvilGUID[1] = go->GetGUID();
                    if (ForgeEventMask & 2) HandleGameObject(ObjectGuid::Empty, true, go);
                    break;
                case GO_GLOWING_ANVIL_3:
                    GO_ForgeAnvilGUID[2] = go->GetGUID();
                    if (ForgeEventMask & 4) HandleGameObject(ObjectGuid::Empty, true, go);
                    break;
                case GO_GIANT_PORTCULLIS_1:
                    GO_PortcullisGUID[0] = go->GetGUID();
                    if (m_auiEncounter[2] == DONE) HandleGameObject(ObjectGuid::Empty, true, go);
                    break;
                case GO_GIANT_PORTCULLIS_2:
                    GO_PortcullisGUID[1] = go->GetGUID();
                    if (m_auiEncounter[2] == DONE) HandleGameObject(ObjectGuid::Empty, true, go);
                    break;
            }
        }

        void SetData(uint32 type, uint32 data) override
        {
            switch (type)
            {
                case DATA_ON_THE_ROCKS_ACHIEV:
                    bRocksAchiev = false;
                    break;
                case DATA_DALRONN_AND_SKARVALD:
                    if (data == NOT_STARTED)
                    {
                        if (Creature* c = GetCreature(DATA_DALRONN))
                            if (c->isDead())
                            {
                                c->AI()->DoAction(-1);
                                c->Respawn();
                            }

                        if (Creature* c = GetCreature(DATA_SKARVALD))
                            if (c->isDead())
                                c->Respawn();

                        if (Creature* c = GetCreature(DATA_DALRONN_GHOST))
                        {
                            c->AI()->DoAction(-1);
                            c->DespawnOrUnsummon();
                        }

                        if (Creature* c = GetCreature(DATA_SKARVALD_GHOST))
                            c->DespawnOrUnsummon();

                    }

                    if (data == DONE)
                    {
                        if (Creature* c = GetCreature(DATA_DALRONN_GHOST))
                        {
                            c->AI()->DoAction(-1);
                            c->DespawnOrUnsummon();
                        }

                        if (Creature* c = GetCreature(DATA_SKARVALD_GHOST))
                            c->DespawnOrUnsummon();

                        RebuildMythicBossLoot(DATA_SKARVALD);
                        RebuildMythicBossLoot(DATA_DALRONN);
                    }

                    m_auiEncounter[1] = data;
                    ApplyMythicLootModeToCreature(DATA_SKARVALD);
                    ApplyMythicLootModeToCreature(DATA_DALRONN);
                    break;
                case DATA_UNLOCK_SKARVALD_LOOT:
                    if (Creature* c = GetCreature(DATA_SKARVALD))
                    {
                        if (MythicEnabled)
                        {
                            RebuildMythicBossLoot(DATA_SKARVALD);
                            break;
                        }

                        c->SetDynamicFlag(UNIT_DYNFLAG_LOOTABLE | UNIT_DYNFLAG_TAPPED | UNIT_DYNFLAG_TAPPED_BY_PLAYER);
                        c->SetLootMode(MythicEnabled ? LOOT_MODE_HARD_MODE_1 : LOOT_MODE_DEFAULT);
                        c->loot.clear();
                        if (uint32 lootid = c->GetCreatureTemplate()->lootid)
                            c->loot.FillLoot(lootid, LootTemplates_Creature, c->GetLootRecipient(), false, false, c->GetLootMode(), c);
                        if (c->GetLootMode())
                            c->loot.generateMoneyLoot(c->GetCreatureTemplate()->mingold, c->GetCreatureTemplate()->maxgold);
                        c->DestroyForVisiblePlayers();
                        c->SetVisible(true);
                    }
                    break;
                case DATA_UNLOCK_DALRONN_LOOT:
                    if (Creature* c = GetCreature(DATA_DALRONN))
                    {
                        if (MythicEnabled)
                        {
                            RebuildMythicBossLoot(DATA_DALRONN);
                            break;
                        }

                        c->AI()->DoAction(-1);
                        c->SetDynamicFlag(UNIT_DYNFLAG_LOOTABLE | UNIT_DYNFLAG_TAPPED | UNIT_DYNFLAG_TAPPED_BY_PLAYER);
                        c->SetLootMode(MythicEnabled ? LOOT_MODE_HARD_MODE_1 : LOOT_MODE_DEFAULT);
                        c->loot.clear();
                        if (uint32 lootid = c->GetCreatureTemplate()->lootid)
                            c->loot.FillLoot(lootid, LootTemplates_Creature, c->GetLootRecipient(), false, false, c->GetLootMode(), c);
                        if (c->GetLootMode())
                            c->loot.generateMoneyLoot(c->GetCreatureTemplate()->mingold, c->GetCreatureTemplate()->maxgold);
                        c->DestroyForVisiblePlayers();
                        c->SetVisible(true);
                    }
                    break;
                case DATA_INGVAR:
                    if (data == DONE)
                    {
                        HandleGameObject(GO_PortcullisGUID[0], true);
                        HandleGameObject(GO_PortcullisGUID[1], true);
                        RebuildMythicBossLoot(DATA_INGVAR);
                    }
                    m_auiEncounter[2] = data;
                    break;
                case DATA_FORGE_1:
                case DATA_FORGE_2:
                case DATA_FORGE_3:
                    if (data == NOT_STARTED)
                    {
                        HandleGameObject(GO_ForgeBellowGUID[type - 100], false);
                        HandleGameObject(GO_ForgeFireGUID[type - 100], false);
                        HandleGameObject(GO_ForgeAnvilGUID[type - 100], false);
                        ForgeEventMask &= ~((uint32)(1 << (type - 100)));
                    }
                    else
                    {
                        HandleGameObject(GO_ForgeBellowGUID[type - 100], true);
                        HandleGameObject(GO_ForgeFireGUID[type - 100], true);
                        HandleGameObject(GO_ForgeAnvilGUID[type - 100], true);
                        ForgeEventMask |= (uint32)(1 << (type - 100));
                    }
                    break;
                case DATA_SPECIAL_DRAKE:
                    if (Creature* c = instance->GetCreature(NPC_SpecialDrakeGUID))
                        c->AI()->SetData(28, 6);
                    break;
            }

            if (data == DONE)
            {
                SaveToDB();
            }
        }

        uint32 GetData(uint32 id) const override
        {
            switch (id)
            {
                case DATA_KELESETH:
                case DATA_DALRONN_AND_SKARVALD:
                case DATA_INGVAR:
                    return m_auiEncounter[id];
                case DATA_FORGE_1:
                case DATA_FORGE_2:
                case DATA_FORGE_3:
                    return ForgeEventMask & (uint32)(1 << (id - 100));
            }

            return 0;
        }

        void ReadSaveDataMore(std::istringstream& data) override
        {
            data >> m_auiEncounter[0];
            data >> m_auiEncounter[1];
            data >> m_auiEncounter[2];
            data >> ForgeEventMask;
        }

        void WriteSaveDataMore(std::ostringstream& data) override
        {
            data << m_auiEncounter[0] << ' ' << m_auiEncounter[1] << ' ' << m_auiEncounter[2] << ' ' << ForgeEventMask;
        }

        bool CheckAchievementCriteriaMeet(uint32 criteria_id, Player const*  /*source*/, Unit const*  /*target*/, uint32  /*miscvalue1*/) override
        {
            switch (criteria_id)
            {
                case 7231: // On The Rocks
                    return bRocksAchiev;
            }
            return false;
        }
    };
};

void AddSC_instance_utgarde_keep()
{
    new instance_utgarde_keep();
}
