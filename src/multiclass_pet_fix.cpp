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

    // Build SummonProperties for a controllable side guardian: Category ALLY so it does
    // NOT claim the pet slot (IsGuardianPet() false -> never dismisses the primary),
    // Type GUARDIAN so Map::SummonCreature instantiates a Guardian. The controllable
    // mask is added in OnPlayerBeforeTempSummonInitStats (before AIM_Initialize) so the
    // AI factory selects PetAI. The PRIMARY is a real Pet (default effect), not built
    // here, so it keeps its full pet ability bar.
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

    // A module-managed side guardian (the primary is a real Pet, tracked by the core's
    // pet slot, not here).
    struct ActiveSummon
    {
        ObjectGuid guid;
        uint32 entry;
        uint32 spellId;
        int32 duration;
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

        // Should this cast be diverted to a side guardian? No when the player has no
        // primary pet (let the default effect make a FULL real Pet), or when they are
        // re-casting the entry that is already their primary (let it refresh in place).
        bool ShouldDivert(Player* owner, uint32 entry)
        {
            ObjectGuid const petGuid = owner->GetPetGUID();
            if (petGuid.IsEmpty())
                return false;

            if (Creature* primary = ObjectAccessor::GetCreatureOrPetOrVehicle(*owner, petGuid))
                if (primary->GetEntry() == entry)
                    return false;

            return true;
        }

        // Remove any existing side guardian of this entry. Used when the entry is about
        // to become (or refresh) the real primary pet, so we never keep a primary plus a
        // duplicate guardian of the same creature.
        void DropEntry(Player* owner, uint32 entry)
        {
            auto it = _players.find(owner->GetGUID());
            if (it == _players.end())
                return;

            RemoveEntry(owner, it->second, entry);
            if (it->second.list.empty())
                _players.erase(it);
        }

        // Spawn a controllable side guardian, enforcing one active summon per entry.
        void AddSecondary(Player* owner, uint32 spellId, uint32 entry, int32 duration)
        {
            PlayerSummons& ps = _players[owner->GetGUID()];
            RemoveEntry(owner, ps, entry);

            if (TempSummon* summon = CreateGuardian(owner, entry, spellId, duration))
            {
                ps.list.push_back({ summon->GetGUID(), entry, spellId, duration });
                LOG_INFO("module.multiclass_pet_fix", "Summon: {} entry {} (spell {}) as guardian for {}",
                    summon->GetGUID().ToString(), entry, spellId, owner->GetName());
            }
            else if (ps.list.empty())
                _players.erase(owner->GetGUID());
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

        TempSummon* CreateGuardian(Player* owner, uint32 entry, uint32 spellId, int32 duration)
        {
            static SummonPropertiesEntry const props = MakeProps(SUMMON_CATEGORY_ALLY, SUMMON_TYPE_GUARDIAN);

            float x, y, z;
            owner->GetClosePoint(x, y, z, owner->GetObjectSize());

            TempSummon* summon = owner->GetMap()->SummonCreature(entry,
                Position(x, y, z, owner->GetOrientation()),
                &props, duration, owner, spellId);
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
            // summon by re-casting its summon spell. With the slot now free, ShouldDivert
            // returns false, so the default effect runs and produces a full real Pet.
            ActiveSummon const promote = ps.list.front();
            Unsummon(owner, promote.guid);
            ps.list.erase(ps.list.begin());

            owner->CastSpell(owner, promote.spellId, true);
            LOG_INFO("module.multiclass_pet_fix", "Promoted entry {} (spell {}) to primary pet for {}",
                promote.entry, promote.spellId, owner->GetName());
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
        PLAYERHOOK_ON_PLAYER_IS_CLASS,
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

    // Pet-context class identity for multiclass characters: if a character has learned
    // another class's pet-summon spell, treat them as that class for PET-ONLY checks
    // (pet permanency, action bar, power type). Strictly gated on HasSpell and
    // CLASS_CONTEXT_PET, so it never fires for a character that lacks the spell (e.g. a
    // freshly created character), and returns nullopt to defer to the real class
    // everywhere else.
    Optional<bool> OnPlayerIsClass(Player const* player, Classes playerClass, ClassContext context) override
    {
        if (context != CLASS_CONTEXT_PET)
            return std::nullopt;

        switch (playerClass)
        {
            case CLASS_WARLOCK:
                if (player->HasSpell(688) || player->HasSpell(697) || player->HasSpell(712) ||
                    player->HasSpell(691) || player->HasSpell(30146))
                    return true;
                break;
            case CLASS_MAGE:
                if (player->HasSpell(31687))
                    return true;
                break;
            case CLASS_DEATH_KNIGHT:
                if (player->HasSpell(46584))
                    return true;
                break;
            case CLASS_HUNTER:
                if (player->HasSpell(883))
                    return true;
                break;
            default:
                break;
        }

        return std::nullopt;
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

        SummonManager& manager = SummonManager::Instance();
        bool const divert = manager.ShouldDivert(owner, entry);

        LOG_INFO("module.multiclass_pet_fix", "HandleSummon: spell {} entry {} caster {} -> {}",
            GetSpellInfo()->Id, entry, owner->GetName(), divert ? "side guardian" : "default real pet");

        // No primary pet (or re-casting the current primary's entry): let the default
        // effect run so the player gets a FULL real Pet with its complete ability bar.
        // Just make sure we don't leave a duplicate side guardian of this entry behind.
        if (!divert)
        {
            manager.DropEntry(owner, entry);
            return;
        }

        // A different pet already holds the slot: spawn this summon as a controllable
        // side guardian and suppress the default (real-pet) effect.
        PreventHitDefaultEffect(effIndex);

        int32 duration = GetSpellInfo()->GetDuration();
        if (Player* modOwner = owner->GetSpellModOwner())
            modOwner->ApplySpellMod(GetSpellInfo()->Id, SPELLMOD_DURATION, duration);

        manager.AddSecondary(owner, GetSpellInfo()->Id, entry, duration);
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
