# mod-multiclass-summons

An AzerothCore module for World of Warcraft 3.3.5a (WotLK) that resolves pet limitations in a multiclassing environment (such as Dad's MMO Lab). 

## Features
1. **Multiclass Pet Respawn Fix**: Solves the mounting, dismounting, and pet load-blocker bugs when you are a Death Knight base class with a Warlock/Mage subclass. It decouples core pet attribute checks from the player's primary class and checks the pet itself.
2. **Multi-Active Summons**: Allows players to have multiple summons (Imp, Voidwalker, Succubus, Felhunter, Felguard, Water Elemental, Ghoul) active at the same time. A module-managed registry tracks the side summons (session-only; not restored after relog).
   * **Primary Summon**: The first summon (cast while the pet slot is free) is a normal, full **real `Pet`** — complete ability bar and full client-side control via the pet action bar/pet frame. The module does not interfere with it.
   * **Secondary Summons**: Any subsequent summon spawns as a controllable side **guardian**. They follow you, join combat, and auto-cast their creature-template spells (e.g. Firebolt, Waterbolt, Claw). They have no client action bar — a 3.3.5a client limitation (one pet bar only), and their abilities are limited to what the creature template defines.
   * **One per type**: only one active summon per creature; re-casting refreshes it rather than stacking duplicates. If the primary pet dies/dismisses/expires, the module promotes a remaining side summon to primary automatically (by re-casting it, so it becomes a full real Pet).
   * **Players only**: playerbots are skipped and keep stock single-pet behaviour (requires playerbots' `WorldSession::IsBot()`).

---

## Installation

### 1. Place the Module
Clone or copy this repository into the `modules/` directory of your AzerothCore source folder:
```bash
cd /path/to/azerothcore-wotlk/modules
git clone https://github.com/bdodroid/mod-multiclass-summons.git
```
*Note: For **Dad's MMO Lab / The Lab** (e.g. on Steam Deck), the module folder should be placed under `/home/deck/wow-server-playerbots/modules/mod-multiclass-summons`.*

### 2. Recompile the Server
Run CMake and compile your server as you normally do:
```bash
# Example if building outside Docker:
mkdir -p build && cd build
cmake ..
make -j$(nproc) && make install

# Example if using Dad's MMO Lab / AzerothCore Docker setup:
./acore.sh compiler build
```

Restart your `ac-worldserver` container/application. The module ships its spell-override registration in `data/sql/db-world/base/multiclass_summons.sql`, which AzerothCore's DBUpdater applies automatically during database loading at startup (before the spell system reads `spell_script_names`).

---

## Database Cleanup (Required for Existing Bugged Pets)
If you have characters that already encountered this bug, their pet database records may be corrupted. Run the following SQL query on your database to reset their pets (replace `[GUID]` with your character's GUID):

```sql
USE acore_characters;
DELETE FROM character_pet WHERE owner = [GUID];
```
*(You can retrieve your character's GUID using `USE acore_characters; SELECT guid, name FROM characters WHERE name = 'YourCharacterName';`)*

---

## Technical Details
A file-local `SummonManager` tracks each player's **side guardians** (in-memory, session-only): one-per-entry uniqueness, primary promotion, and cleanup. The primary is a real `Pet` owned by the core, not tracked here.

This module operates via hooks on:

* **`SpellScript`** (`SpellSummonPetOverrideScript`): bound to the summon spells. For non-bot players it decides (`ShouldDivert`): if the pet slot is free, or the cast matches the current primary's entry, it lets the **default real-pet effect** run (full pet). Otherwise it prevents the default and spawns a controllable **side guardian**.
* **`PlayerScript`**:
  * `OnPlayerBeforeTempSummonInitStats`: flags module summon guardians with `UNIT_MASK_CONTROLLABLE_GUARDIAN` + `CharmInfo` (before `AIM_Initialize`) so they pick up `PetAI` and auto-cast.
  * `OnPlayerUpdate`: throttled (~1s) reconcile — prunes dead summons and promotes a new primary if the slot frees up.
  * `OnPlayerLogout`: clears/unsummons the player's summons (session-only).
  * `OnPlayerBeforeLoadPetFromDB`: kept for **real** pets only (hunter pets on multiclass characters) — bypasses the Death Knight pet exception for non-undead pets. Module summons are guardians and never use this path.
  * `OnPlayerIsClass`: for **`CLASS_CONTEXT_PET` only**, treats a character as a given class if they have learned that class's pet-summon spell (`HasSpell`), so pet permanency / action bar / power-type checks work for multiclass characters. Strictly gated — it returns `nullopt` for any character that lacks the spell (so it never affects freshly created characters) and for every non-pet context.

---

## Compatibility & Single-Class Server Impact

This module targets AzerothCore 3.3.5a servers running **playerbots** (it skips bots via `WorldSession::IsBot()`), such as Dad's MMO Lab / Unbound. It works on both single-class and custom multiclassing setups. It does not write to `character_pet`, so it cannot cause pet database corruption.

> **Build requirement:** because it calls `WorldSession::IsBot()`, it must be compiled against a core that includes the playerbots module. On a core without playerbots, remove the `IsBot()` guards (or stub them) before building.

### Single-Class Server Impact (Caveats)
If installed on a standard, single-class WotLK server, the gameplay changes are minor and isolated:
* **Warlocks**: A single-class Warlock will be able to summon all of their demons (Imp, Voidwalker, Succubus, Felhunter, Felguard) at the same time. The first demon summoned will act as the primary pet (displaying the pet action bar and control frame), while any subsequent demons will spawn as controllable minions that follow and auto-cast their main abilities in combat.
* **Other Classes**: All other single-class players (Mages, Hunters, Death Knights) are **unaffected**. Their pet summoning rules, limits, and behaviors remain 100% standard and Blizzlike.

---

## Future Planned Features
* **Minion Mount/Dismount Persistence**: Automatically save active secondary minions when mounting, and automatically restore/respawn them upon dismounting so players don't have to manually recast them.


