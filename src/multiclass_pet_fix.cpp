#include "ScriptMgr.h"
#include "Player.h"
#include "Pet.h"
#include "ObjectMgr.h"
#include "SpellScript.h"
#include "SpellInfo.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Map.h"
#include "TemporarySummon.h"

class MulticlassPetFixPlayerScript : public PlayerScript
{
public:
    MulticlassPetFixPlayerScript() : PlayerScript("MulticlassPetFixPlayerScript") { }

    void OnPlayerBeforeLoadPetFromDB(Player* player, uint32& /*petentry*/, uint32& petnumber, bool& current, bool& forceLoadFromDB) override
    {
        PetStable* petStable = player->GetPetStable();
        if (!petStable)
            return;

        PetStable::PetInfo const* petInfo = nullptr;
        if (petnumber)
        {
            if (petStable->CurrentPet && petStable->CurrentPet->PetNumber == petnumber)
                petInfo = &petStable->CurrentPet.value();
            else
            {
                for (auto const& info : petStable->UnslottedPets)
                {
                    if (info.PetNumber == petnumber)
                    {
                        petInfo = &info;
                        break;
                    }
                }
            }
        }
        else if (current)
        {
            if (petStable->CurrentPet)
                petInfo = &petStable->CurrentPet.value();
        }

        if (petInfo)
        {
            CreatureTemplate const* creatureInfo = sObjectMgr->GetCreatureTemplate(petInfo->CreatureId);
            if (creatureInfo && creatureInfo->type != CREATURE_TYPE_UNDEAD)
            {
                // Force load from DB to bypass the DK pet exception check for all non-DK pets (demons, elementals, beasts)
                forceLoadFromDB = true;
            }
        }
    }

    void OnPlayerBeforeGuardianInitStatsForLevel(Player* /*player*/, Guardian* guardian, CreatureTemplate const* /*cinfo*/, PetType& petType) override
    {
        if (guardian->IsPet())
        {
            if (petType == MAX_PET_TYPE)
            {
                petType = guardian->ToPet()->getPetType();
            }
        }
    }

    void OnPlayerBeforeTempSummonInitStats(Player* player, TempSummon* tempSummon, uint32& /*duration*/) override
    {
        if (tempSummon->IsGuardian())
        {
            Guardian* guardian = (Guardian*)tempSummon;
            uint32 creatorSpellId = guardian->GetUInt32Value(UNIT_CREATED_BY_SPELL);
            if (creatorSpellId == 688 || creatorSpellId == 697 || creatorSpellId == 712 || creatorSpellId == 691 || creatorSpellId == 30146 || creatorSpellId == 31687 || creatorSpellId == 52150)
            {
                // If it is summoned as a minion/guardian (not the player's primary pet)
                if (player->GetPetGUID() != guardian->GetGUID())
                {
                    // Make it controllable so it uses PetAI and auto-casts spells
                    guardian->AddUnitTypeMask(UNIT_MASK_CONTROLLABLE_GUARDIAN);
                    guardian->InitCharmInfo();
                }
            }
        }
    }

    Optional<bool> OnPlayerIsClass(Player const* player, Classes playerClass, ClassContext context) override
    {
        if (context == CLASS_CONTEXT_PET)
        {
            // For pet permanent checks and spell handling, check if the player has the corresponding class pet summon spells.
            if (playerClass == CLASS_WARLOCK)
            {
                // Imp (688), Voidwalker (697), Succubus (712), Felhunter (691), Felguard (30146)
                if (player->HasSpell(688) || player->HasSpell(697) || player->HasSpell(712) || player->HasSpell(691) || player->HasSpell(30146))
                {
                    return true;
                }
            }
            else if (playerClass == CLASS_MAGE)
            {
                // Summon Water Elemental (31687)
                if (player->HasSpell(31687))
                {
                    return true;
                }
            }
            else if (playerClass == CLASS_DEATH_KNIGHT)
            {
                // Raise Dead (46584)
                if (player->HasSpell(46584))
                {
                    return true;
                }
            }
            else if (playerClass == CLASS_HUNTER)
            {
                // Call Pet (883)
                if (player->HasSpell(883))
                {
                    return true;
                }
            }
        }
        return std::nullopt;
    }
};

class SpellSummonPetOverrideScript : public SpellScript
{
    PrepareSpellScript(SpellSummonPetOverrideScript);

    void HandleSummon(SpellEffIndex effIndex)
    {
        Player* owner = GetCaster()->ToPlayer();
        if (!owner)
            return;

        // If the player already has a pet active
        if (owner->GetPet())
        {
            // Prevent the default EffectSummonPet from running
            PreventHitDefaultEffect(effIndex);

            // Summon it as a Guardian/Minion instead!
            uint32 petEntry = GetSpellInfo()->Effects[effIndex].MiscValue;
            if (!petEntry)
                return;

            float x, y, z;
            owner->GetClosePoint(x, y, z, owner->GetObjectSize());

            // Summon properties Entry 67 is SUMMON_TYPE_MINION
            SummonPropertiesEntry const* properties = sSummonPropertiesStore.LookupEntry(67);
            int32 duration = GetSpellInfo()->GetDuration();
            if (Player* modOwner = owner->GetSpellModOwner())
                modOwner->ApplySpellMod(GetSpellInfo()->Id, SPELLMOD_DURATION, duration);

            TempSummon* summon = owner->GetMap()->SummonCreature(petEntry, Position(x, y, z, owner->GetOrientation()), properties, duration, owner, GetSpellInfo()->Id);
            if (!summon)
                return;

            // Generate name
            std::string newName = sObjectMgr->GeneratePetName(petEntry);
            if (!newName.empty())
                summon->SetName(newName);
        }
    }

    void Register() override
    {
        OnEffectHit += SpellEffectFn(SpellSummonPetOverrideScript::HandleSummon, EFFECT_0, SPELL_EFFECT_SUMMON_PET);
        OnEffectHit += SpellEffectFn(SpellSummonPetOverrideScript::HandleSummon, EFFECT_1, SPELL_EFFECT_SUMMON_PET);
        OnEffectHit += SpellEffectFn(SpellSummonPetOverrideScript::HandleSummon, EFFECT_2, SPELL_EFFECT_SUMMON_PET);
    }
};

class SpellSummonPetOverrideLoader : public SpellScriptLoader
{
public:
    SpellSummonPetOverrideLoader() : SpellScriptLoader("spell_summon_pet_override") { }

    SpellScript* GetSpellScript() const override
    {
        return new SpellSummonPetOverrideScript();
    }
};

void AddMulticlassPetFixScripts()
{
    new MulticlassPetFixPlayerScript();
    new SpellSummonPetOverrideLoader();

    // Dynamically register the spell script to the DB during server startup to make it zero-config
    WorldDatabase.Execute("DELETE FROM spell_scripts WHERE ScriptName = 'spell_summon_pet_override'");
    WorldDatabase.Execute("INSERT INTO spell_scripts (spell_id, ScriptName) VALUES "
                          "(688, 'spell_summon_pet_override'), "     // Summon Imp
                          "(697, 'spell_summon_pet_override'), "     // Summon Voidwalker
                          "(712, 'spell_summon_pet_override'), "     // Summon Succubus
                          "(691, 'spell_summon_pet_override'), "     // Summon Felhunter
                          "(30146, 'spell_summon_pet_override'), "   // Summon Felguard
                          "(31687, 'spell_summon_pet_override'), "   // Summon Water Elemental
                          "(52150, 'spell_summon_pet_override')");   // Summon Ghoul (DK pet spell)
}
