#include "ScriptMgr.h"
#include "Player.h"
#include "Pet.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "SpellScript.h"
#include "SpellInfo.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Map.h"
#include "TemporarySummon.h"

namespace
{
    // Summon spells this module intercepts. A second cast while a pet is already
    // active is diverted to a controllable side guardian instead of replacing the
    // primary pet. Keep this list in sync with data/sql/db-world/base/multiclass_summons.sql.
    bool IsMulticlassSummonSpell(uint32 spellId)
    {
        switch (spellId)
        {
            case 688:   // Summon Imp
            case 697:   // Summon Voidwalker
            case 712:   // Summon Succubus
            case 691:   // Summon Felhunter
            case 30146: // Summon Felguard
            case 70907: // Summon Water Elemental (Temp)
            case 70908: // Summon Water Elemental (Perm)
            case 46584: // Raise Dead (Temp Ghoul)
            case 52150: // Raise Dead (Perm Ghoul)
                return true;
            default:
                return false;
        }
    }
}

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
            if (IsMulticlassSummonSpell(creatorSpellId))
            {
                // If it is summoned as a minion/guardian (not the player's primary pet)
                if (player->GetPetGUID() != guardian->GetGUID())
                {
                    // Make it controllable so it uses PetAI and auto-casts spells.
                    // NOTE: Do NOT call AIM_Initialize() here. This hook fires during
                    // TempSummon::InitStats(), before the creature is added to the world.
                    // AddToWorld() will call AIM_Initialize() itself, and at that point
                    // UNIT_MASK_CONTROLLABLE_GUARDIAN ensures it picks up PetAI correctly.
                    // Calling it here causes a double-init that can reset AI to NullCreatureAI.
                    guardian->AddUnitTypeMask(UNIT_MASK_CONTROLLABLE_GUARDIAN);
                    guardian->InitCharmInfo();

                    // Diagnostic: confirms a secondary summon was upgraded to a
                    // controllable guardian. Demote to LOG_DEBUG once verified.
                    LOG_INFO("module.multiclass_pet_fix", "TempSummonInitStats: flagged controllable guardian entry {} (spell {}) for {}",
                        guardian->GetEntry(), creatorSpellId, player->GetName());
                }
            }
        }
    }

    // NOTE: This module intentionally does NOT override OnPlayerIsClass.
    //
    // On Dad's MMO Lab / Unbound the dedicated multiclass module owns class identity
    // (it implements OnPlayerIsClass for every ClassContext). A previous version here
    // returned "true" for CLASS_CONTEXT_PET whenever player->HasSpell(<summon spell>),
    // but multiclass servers grant every class's spells into every spellbook, so that
    // check was true for ALL characters - which made the core treat everyone as a
    // warlock in pet context and let any character summon/control demons. Leave class
    // identity to the multiclass module; this module only handles the summon mechanics.
};

class SpellSummonPetOverrideScript : public SpellScript
{
    PrepareSpellScript(SpellSummonPetOverrideScript);

    void HandleSummon(SpellEffIndex effIndex)
    {
        Player* owner = GetCaster()->ToPlayer();
        if (!owner)
            return;

        uint32 const petEntry = GetSpellInfo()->Effects[effIndex].MiscValue;
        if (!petEntry)
            return;

        // GetPetGUID() (the SUMMON_SLOT_PET slot) covers both real Pet objects and
        // slot-claiming guardian pets, whereas GetPet() only matches true Pets and
        // would miss guardian-style primaries (e.g. permanent Water Elemental / Ghoul).
        ObjectGuid const petGuid = owner->GetPetGUID();

        // Diagnostic: confirm the override fires and on which spell/effect. Grep the
        // worldserver log for "module.multiclass_pet_fix". Demote to LOG_DEBUG once the
        // secondary-summon behaviour is verified for every pet type.
        LOG_INFO("module.multiclass_pet_fix", "HandleSummon fired: spell {} effIdx {} entry {} caster {} GetPetGUID {}",
            GetSpellInfo()->Id, uint32(effIndex), petEntry, owner->GetName(), petGuid.ToString());

        // Recasting the entry that is ALREADY the primary pet: let the default effect
        // refresh it in place rather than spawning a duplicate (avoids an "army" of
        // the same pet when the player spams their primary's summon spell).
        if (petGuid)
            if (Creature* primary = ObjectAccessor::GetCreatureOrPetOrVehicle(*owner, petGuid))
                if (primary->GetEntry() == petEntry)
                    return;

        // Enforce one active summon per creature entry: remove any existing instance of
        // this creature (a leftover side guardian of the same type) before continuing.
        // This both prevents duplicate clones and clears stale state that otherwise
        // breaks a later re-summon of the same pet.
        owner->RemoveAllMinionsByEntry(petEntry);

        // No primary pet present: let the default effect make this summon the primary
        // pet (full client control via the pet action bar / pet frame).
        if (!petGuid)
            return;

        // A different pet is already primary: spawn this one as a controllable side
        // guardian, and prevent the default effect that would dismiss the active pet.
        PreventHitDefaultEffect(effIndex);

        // Summon a controllable guardian that does NOT claim the pet slot:
        //   - Category ALLY keeps Minion::IsGuardianPet() false, so Unit::SetMinion
        //     never displaces (dismisses) the existing primary pet.
        //   - Type GUARDIAN makes Map::SummonCreature instantiate a Guardian (an
        //     ALLY category resolves the unit mask from Type; PET would instead fall
        //     through to a plain, AI-less TempSummon).
        //   - Slot 0 avoids evicting other active side guardians via m_SummonSlot.
        // These are set explicitly rather than cloned from SummonProperties entry 67
        // so behaviour does not depend on that entry's (DBC-defined) Type/Slot values.
        // MulticlassPetFixPlayerScript::OnPlayerBeforeTempSummonInitStats then flags
        // the guardian controllable during InitStats, so AIM_Initialize picks PetAI
        // (auto-follow + autocast) for it.
        static SummonPropertiesEntry const guardianProperties = []
        {
            SummonPropertiesEntry props{};
            props.Id = 67;
            props.Category = SUMMON_CATEGORY_ALLY;
            props.Faction = 0;
            props.Type = SUMMON_TYPE_GUARDIAN;
            props.Slot = 0;
            props.Flags = 0;
            return props;
        }();

        float x, y, z;
        owner->GetClosePoint(x, y, z, owner->GetObjectSize());

        int32 duration = GetSpellInfo()->GetDuration();
        if (Player* modOwner = owner->GetSpellModOwner())
            modOwner->ApplySpellMod(GetSpellInfo()->Id, SPELLMOD_DURATION, duration);

        TempSummon* summon = owner->GetMap()->SummonCreature(petEntry, Position(x, y, z, owner->GetOrientation()), &guardianProperties, duration, owner, GetSpellInfo()->Id);
        if (!summon)
        {
            LOG_INFO("module.multiclass_pet_fix", "HandleSummon: SummonCreature returned null for entry {} (spell {})", petEntry, GetSpellInfo()->Id);
            return;
        }

        LOG_INFO("module.multiclass_pet_fix", "HandleSummon: diverted spell {} entry {} to side guardian {} (mask guardian={})",
            GetSpellInfo()->Id, petEntry, summon->GetGUID().ToString(), summon->IsGuardian());

        std::string newName = sObjectMgr->GeneratePetName(petEntry);
        if (!newName.empty())
            summon->SetName(newName);
    }

    void Register() override
    {
        OnEffectHit += SpellEffectFn(SpellSummonPetOverrideScript::HandleSummon, EFFECT_0, SPELL_EFFECT_SUMMON_PET);
        OnEffectHit += SpellEffectFn(SpellSummonPetOverrideScript::HandleSummon, EFFECT_1, SPELL_EFFECT_SUMMON_PET);
        OnEffectHit += SpellEffectFn(SpellSummonPetOverrideScript::HandleSummon, EFFECT_2, SPELL_EFFECT_SUMMON_PET);

        OnEffectHit += SpellEffectFn(SpellSummonPetOverrideScript::HandleSummon, EFFECT_0, SPELL_EFFECT_SUMMON);
        OnEffectHit += SpellEffectFn(SpellSummonPetOverrideScript::HandleSummon, EFFECT_1, SPELL_EFFECT_SUMMON);
        OnEffectHit += SpellEffectFn(SpellSummonPetOverrideScript::HandleSummon, EFFECT_2, SPELL_EFFECT_SUMMON);
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

    // NOTE: spell_script_names registration is handled by
    // data/sql/db-world/base/multiclass_summons.sql, which the DBUpdater auto-applies
    // during database loading at startup, BEFORE LoadSpellScriptNames().
    // DO NOT use runtime WorldDatabase.Execute() here - it runs AFTER LoadSpellScriptNames()
    // has already cached the table, so runtime INSERTs are only seen on the next restart.
}
