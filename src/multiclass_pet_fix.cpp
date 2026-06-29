#include "ScriptMgr.h"
#include "Player.h"
#include "Pet.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "SpellScript.h"
#include "SpellInfo.h"
#include "DBCStores.h"
#include "Map.h"
#include "TemporarySummon.h"
#include "WorldSession.h"
#include <algorithm>
#include <unordered_map>
#include <vector>

// mod-multiclass-summons — module-managed multi-summon system.
//
// Goal: let a (multiclass) player have several summons active at once. The first
// summon while the pet slot is free becomes the controllable PRIMARY (it claims
// the pet slot, so the client shows the pet action bar/frame). Every subsequent
// summon spawns as a controllable side GUARDIAN (PetAI: follows, joins combat,
// auto-casts its creature-template spells; no client bar — a hard 3.3.5a limit).
//
// All of these summons are controllable GUARDIANS, not real Pets: the module owns
// their whole lifecycle (creation, one-per-entry uniqueness, primary promotion,
// cleanup). Nothing is written to character_pet, which sidesteps the core's
// single-class pet checks entirely. Summons are session-only (not restored on
// relog by design).
//
// Playerbots are skipped: bots keep stock single-pet behaviour so their AI (which
// relies on GetPet()) is unaffected. This depends on playerbots' WorldSession::IsBot().

namespace
{
    // Summon spells this module intercepts. Keep in sync with
    // data/sql/db-world/base/multiclass_summons.sql.
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

    bool IsPlayerBot(Player const* player)
    {
        return player->GetSession() && player->GetSession()->IsBot();
    }

    // Build a SummonProperties for a controllable guardian.
    //   Primary:   Category PET   -> claims the pet slot + action bar (like the
    //              permanent Water Elemental). IsGuardianPet() is true.
    //   Secondary: Category ALLY  -> does NOT claim the slot (IsGuardianPet() false),
    //              so it never dismisses the primary. Type GUARDIAN makes
    //              Map::SummonCreature instantiate a Guardian; the controllable mask
    //              is added in OnPlayerBeforeTempSummonInitStats before AIM_Initialize.
    SummonPropertiesEntry MakeProps(uint32 category, uint32 type)
    {
        SummonPropertiesEntry props{};
        props.Id = 67;
        props.Category = category;
        props.Faction = 0;
        props.Type = type;
        props.Slot = 0;
        props.Flags = 0;
        return props;
    }

    struct ActiveSummon
    {
        ObjectGuid guid;
        uint32 entry;
        uint32 spellId;
        int32 duration;
        bool primary;
    };

    struct PlayerSummons
    {
        std::vector<ActiveSummon> list;
        uint32 reconcileTimer = 0;
    };

    // Owns every module summon for every (non-bot) player. World-thread only, so a
    // plain map needs no locking.
    class SummonManager
    {
    public:
        static SummonManager& Instance()
        {
            static SummonManager instance;
            return instance;
        }

        // A target summon spell was cast. Spawn it as primary (slot free) or as a
        // side guardian, enforcing one active summon per creature entry.
        void HandleCast(Player* owner, uint32 spellId, uint32 entry, int32 duration)
        {
            PlayerSummons& ps = _players[owner->GetGUID()];

            // One active summon per creature entry: drop any existing instance first
            // (a re-cast refreshes rather than stacking an army of clones).
            RemoveEntry(owner, ps, entry);

            // First summon while the pet slot is free becomes the controllable
            // primary; otherwise (a pet/guardian already holds the slot) it is a
            // side guardian. Using the slot keeps us compatible with any real pet
            // the player legitimately has (e.g. a hunter pet on a multiclass char).
            bool const primary = owner->GetPetGUID().IsEmpty();

            if (TempSummon* summon = CreateGuardian(owner, entry, spellId, duration, primary))
            {
                ps.list.push_back({ summon->GetGUID(), entry, spellId, duration, primary });
                LOG_INFO("module.multiclass_pet_fix", "Summon: {} entry {} (spell {}) as {} for {}",
                    summon->GetGUID().ToString(), entry, spellId, primary ? "PRIMARY" : "guardian", owner->GetName());
            }
        }

        // Throttled per-player reconcile: prune dead summons and, if the primary is
        // gone but other summons remain, promote one so a controllable pet persists.
        void Update(Player* owner, uint32 diff)
        {
            auto it = _players.find(owner->GetGUID());
            if (it == _players.end())
                return;

            PlayerSummons& ps = it->second;
            ps.reconcileTimer += diff;
            if (ps.reconcileTimer < RECONCILE_INTERVAL)
                return;
            ps.reconcileTimer = 0;

            Reconcile(owner, ps);

            if (ps.list.empty())
                _players.erase(it);
        }

        // Session-only: drop the registry (and despawn the summons) when the player
        // leaves.
        void Clear(Player* owner)
        {
            auto it = _players.find(owner->GetGUID());
            if (it == _players.end())
                return;

            for (ActiveSummon const& summon : it->second.list)
                Unsummon(owner, summon.guid);

            _players.erase(it);
        }

    private:
        static constexpr uint32 RECONCILE_INTERVAL = 1000;

        std::unordered_map<ObjectGuid, PlayerSummons> _players;

        TempSummon* CreateGuardian(Player* owner, uint32 entry, uint32 spellId, int32 duration, bool primary)
        {
            static SummonPropertiesEntry const primaryProps = MakeProps(SUMMON_CATEGORY_PET, SUMMON_TYPE_PET);
            static SummonPropertiesEntry const secondaryProps = MakeProps(SUMMON_CATEGORY_ALLY, SUMMON_TYPE_GUARDIAN);

            float x, y, z;
            owner->GetClosePoint(x, y, z, owner->GetObjectSize());

            TempSummon* summon = owner->GetMap()->SummonCreature(entry,
                Position(x, y, z, owner->GetOrientation()),
                primary ? &primaryProps : &secondaryProps,
                duration, owner, spellId);
            if (!summon)
                return nullptr;

            if (std::string name = sObjectMgr->GeneratePetName(entry); !name.empty())
                summon->SetName(name);

            return summon;
        }

        void Unsummon(Player* owner, ObjectGuid guid)
        {
            if (Creature* creature = ObjectAccessor::GetCreature(*owner, guid))
                if (TempSummon* summon = creature->ToTempSummon())
                    summon->UnSummon();
        }

        void RemoveEntry(Player* owner, PlayerSummons& ps, uint32 entry)
        {
            for (ActiveSummon const& summon : ps.list)
                if (summon.entry == entry)
                    Unsummon(owner, summon.guid);

            ps.list.erase(std::remove_if(ps.list.begin(), ps.list.end(),
                [entry](ActiveSummon const& summon) { return summon.entry == entry; }),
                ps.list.end());
        }

        void Reconcile(Player* owner, PlayerSummons& ps)
        {
            // Drop summons that have died or despawned.
            ps.list.erase(std::remove_if(ps.list.begin(), ps.list.end(),
                [owner](ActiveSummon const& summon)
                {
                    Creature* creature = ObjectAccessor::GetCreature(*owner, summon.guid);
                    return !creature || !creature->IsAlive();
                }),
                ps.list.end());

            if (ps.list.empty())
                return;

            // If the pet slot is occupied (our primary, or a legit real pet) there is
            // nothing to promote.
            if (!owner->GetPetGUID().IsEmpty())
                return;

            // The primary is gone and the slot is free: promote the oldest remaining
            // summon by re-spawning it as the controllable primary.
            ActiveSummon const promote = ps.list.front();
            Unsummon(owner, promote.guid);
            ps.list.erase(ps.list.begin());

            if (TempSummon* summon = CreateGuardian(owner, promote.entry, promote.spellId, promote.duration, true))
            {
                ps.list.push_back({ summon->GetGUID(), promote.entry, promote.spellId, promote.duration, true });
                LOG_INFO("module.multiclass_pet_fix", "Promoted entry {} (spell {}) to primary for {}",
                    promote.entry, promote.spellId, owner->GetName());
            }
        }
    };
}

class MulticlassPetFixPlayerScript : public PlayerScript
{
public:
    MulticlassPetFixPlayerScript() : PlayerScript("MulticlassPetFixPlayerScript",
    {
        PLAYERHOOK_ON_BEFORE_LOAD_PET_FROM_DB,
        PLAYERHOOK_ON_BEFORE_GUARDIAN_INIT_STATS_FOR_LEVEL,
        PLAYERHOOK_ON_BEFORE_TEMP_SUMMON_INIT_STATS,
        PLAYERHOOK_ON_UPDATE,
        PLAYERHOOK_ON_LOGOUT
    }) { }

    // Real-pet support (hunter pets on multiclass characters): bypass the Death
    // Knight pet exception for non-undead pets loaded from character_pet. Module
    // summons are guardians and never travel this path.
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
                // Force load from DB to bypass the DK pet exception check for all non-DK pets.
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

    // Flag module summon guardians controllable during InitStats (before AddToWorld
    // -> AIM_Initialize), so the AI factory selects PetAI (follow + autocast).
    // Primary summons (PET category) are already controllable from their ctor; doing
    // it again here is idempotent.
    void OnPlayerBeforeTempSummonInitStats(Player* player, TempSummon* tempSummon, uint32& /*duration*/) override
    {
        if (IsPlayerBot(player))
            return;

        if (!tempSummon->IsGuardian())
            return;

        Guardian* guardian = static_cast<Guardian*>(tempSummon);
        if (!IsMulticlassSummonSpell(guardian->GetUInt32Value(UNIT_CREATED_BY_SPELL)))
            return;

        // NOTE: Do NOT call AIM_Initialize() here. This fires during InitStats(),
        // before AddToWorld() runs it itself; the mask below ensures PetAI is picked.
        guardian->AddUnitTypeMask(UNIT_MASK_CONTROLLABLE_GUARDIAN);
        guardian->InitCharmInfo();
    }

    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        if (IsPlayerBot(player))
            return;

        SummonManager::Instance().Update(player, diff);
    }

    void OnPlayerLogout(Player* player) override
    {
        SummonManager::Instance().Clear(player);
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

        // Leave playerbots on stock single-pet behaviour (their AI relies on GetPet()).
        if (IsPlayerBot(owner))
            return;

        uint32 const entry = GetSpellInfo()->Effects[effIndex].MiscValue;
        if (!entry)
            return;

        // The module owns every one of these summons — never run the default
        // (real-pet) effect, which would dismiss the active pet.
        PreventHitDefaultEffect(effIndex);

        int32 duration = GetSpellInfo()->GetDuration();
        if (Player* modOwner = owner->GetSpellModOwner())
            modOwner->ApplySpellMod(GetSpellInfo()->Id, SPELLMOD_DURATION, duration);

        SummonManager::Instance().HandleCast(owner, GetSpellInfo()->Id, entry, duration);
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
}
