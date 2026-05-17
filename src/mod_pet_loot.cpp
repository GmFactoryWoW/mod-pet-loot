/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "ScriptMgr.h"
#include "Player.h"
#include "Config.h"
#include "Chat.h"
#include "Creature.h"
#include "LootMgr.h"
#include "ObjectAccessor.h"
#include "Group.h"
#include "Map.h"

// Configuration class to store module settings
class PetLootConfig
{
public:
    static PetLootConfig* instance()
    {
        static PetLootConfig instance;
        return &instance;
    }

    bool Enabled;
    uint32 PetId;
    float Radius;

    void Load()
    {
        Enabled = sConfigMgr->GetOption<bool>("PetLoot.Enable", true);
        PetId = sConfigMgr->GetOption<uint32>("PetLoot.PetId", 23234); // Default: Bananas
        Radius = sConfigMgr->GetOption<float>("PetLoot.Radius", 50.0f);
    }
};

// Event to handle the actual looting after the pet reaches the corpse
class PetLootEvent : public BasicEvent
{
public:
    PetLootEvent(ObjectGuid playerGuid, ObjectGuid victimGuid, ObjectGuid petGuid)
        : _playerGuid(playerGuid), _victimGuid(victimGuid), _petGuid(petGuid) { }

    bool Execute(uint64 /*e_time*/, uint32 /*p_time*/) override
    {
        Player* player = ObjectAccessor::FindPlayer(_playerGuid);
        if (!player)
            return true;

        Creature* victim = ObjectAccessor::GetCreature(*player, _victimGuid);
        Creature* pet = ObjectAccessor::GetCreature(*player, _petGuid);

        if (!victim || !pet)
            return true;

        // Ensure they are on the same map
        if (pet->GetMap() != player->GetMap())
            return true;

        // Visual feedback: Pet loots
        pet->HandleEmoteCommand(EMOTE_ONESHOT_LOOT);

        // Fill loot if empty
        if (victim->loot.items.empty() && victim->loot.gold == 0 && victim->loot.quest_items.empty())
        {
            if (auto cTemplate = victim->GetCreatureTemplate())
            {
                bool personal = (player->GetGroup() == nullptr);
                victim->loot.FillLoot(cTemplate->lootid, LootTemplates_Creature, player, personal);
            }
        }

        // Handle Gold
        if (uint32 gold = victim->loot.gold)
        {
            player->ModifyMoney(gold);

            // Send standard loot notification
		    WorldPacket data(SMSG_LOOT_MONEY_NOTIFY, 4 + 1);
            data << uint32(gold);
            data << uint8(1); // "You loot..."
            player->GetSession()->SendPacket(&data);

            victim->loot.gold = 0;
            victim->loot.NotifyMoneyRemoved();
        }

        // Handle Items using core's built-in StoreLootItem
        uint32 items_count = uint32(victim->loot.items.size());
        uint32 max_slot = victim->loot.GetMaxSlotInLootFor(player);

        Group* group = player->GetGroup();
        LootMethod lootMethod = group ? group->GetLootMethod() : FREE_FOR_ALL;

        for (uint32 i = 0; i < max_slot; ++i)
        {
            // Quest item slots: StoreLootItem already enforces AllowedForPlayer
            // and quest requirements — no additional check needed.
            if (i >= items_count)
            {
                InventoryResult msg;
                player->StoreLootItem(uint8(i), &victim->loot, msg);
                continue;
            }

            LootItem& lootItem = victim->loot.items[i];

            if (lootItem.is_looted)
                continue;

            // In a group with a non-FFA loot method, respect group loot rules.
            if (group && lootMethod != FREE_FOR_ALL)
            {
                // is_underthreshold items are always FFA regardless of loot method.
                if (!lootItem.is_underthreshold)
                {
                    // Skip items pending a Need/Greed/Master roll — players must
                    // resolve those manually.
                    if (lootItem.is_blocked)
                        continue;

                    // Skip items allocated to other players (round-robin / master loot).
                    const AllowedLooterSet& allowed = lootItem.GetAllowedLooters();
                    if (!allowed.empty() && !allowed.count(player->GetGUID()))
                        continue;
                }
            }

            InventoryResult msg;
            player->StoreLootItem(uint8(i), &victim->loot, msg);
        }

        // Cleanup corpse visual and decay if empty
        if (victim->loot.isLooted())
        {
            victim->RemoveFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE);
            victim->AllLootRemovedFromCorpse();
        }

        // Pet returns to player
        pet->GetMotionMaster()->MoveFollow(player, 1.0f, 0.0f);

        return true;
    }

private:
    ObjectGuid _playerGuid;
    ObjectGuid _victimGuid;
    ObjectGuid _petGuid;
};

class PetLootWorldScript : public WorldScript
{
public:
    PetLootWorldScript() : WorldScript("PetLootWorldScript") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        PetLootConfig::instance()->Load();
    }
};

class PetLootPlayerScript : public PlayerScript
{
public:
    PetLootPlayerScript() : PlayerScript("PetLootPlayerScript") { }

    void OnPlayerCreatureKill(Player* killer, Creature* victim) override
    {
        ProcessPetLoot(killer, victim);
    }

    void OnPlayerCreatureKilledByPet(Player* petOwner, Creature* victim) override
    {
        ProcessPetLoot(petOwner, victim);
    }

private:
    void ProcessPetLoot(Player* player, Creature* victim)
    {
        if (!PetLootConfig::instance()->Enabled)
            return;

        if (!player || !victim)
            return;

        // Check if the player has the required vanity pet (Bananas) summoned
        ObjectGuid critterGuid = player->GetCritterGUID();
        if (critterGuid.IsEmpty())
            return;

        Creature* critter = ObjectAccessor::GetCreature(*player, critterGuid);
        if (!critter || critter->GetEntry() != PetLootConfig::instance()->PetId)
            return;

        // Distance check between player and victim
        float dist = player->GetDistance(victim);
        if (dist > PetLootConfig::instance()->Radius)
            return;

        // Visual: Pet moves to the corpse
        critter->GetMotionMaster()->MovePoint(1, victim->GetPositionX(), victim->GetPositionY(), victim->GetPositionZ());

        // Schedule the looting event
        uint32 delay = 1000;
        player->m_Events.AddEvent(new PetLootEvent(player->GetGUID(), victim->GetGUID(), critter->GetGUID()), player->m_Events.CalculateTime(delay));
    }
};

void Addmod_pet_lootScripts()
{
    new PetLootWorldScript();
    new PetLootPlayerScript();
}
