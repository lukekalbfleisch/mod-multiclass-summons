# wow-unbound-multiclass-summons

An AzerothCore module for World of Warcraft 3.3.5a (WotLK) that resolves pet limitations in a multiclassing environment (such as Dad's MMO Lab). 

## Features
1. **Multiclass Pet Respawn Fix**: Solves the mounting, dismounting, and pet load-blocker bugs when you are a Death Knight base class with a Warlock/Mage subclass. It decouples core pet attribute checks from the player's primary class and checks the pet itself.
2. **Multi-Active Summons**: Allows players to have multiple permanent summoning pets (Imp, Voidwalker, Succubus, Felhunter, Felguard, Water Elemental, Ghoul) active at the same time.
   * **Primary Summon**: The first active summon has full client-side control via the pet action bar/pet frame.
   * **Secondary Summons**: Any subsequent summon casts while a pet is active will spawn the creature as a controllable guardian/minion. They will follow you, join combat, and auto-cast their main offensive spells (e.g. Firebolt, Waterbolt, Claw).

---

## Installation

### 1. Place the Module
Clone or copy this repository into the `modules/` directory of your AzerothCore source folder:
```bash
cd /path/to/azerothcore-wotlk/modules
git clone https://github.com/bdodroid/wow-unbound-multiclass-summons.git
```
*Note: For **Dad's MMO Lab / The Lab** (e.g. on Steam Deck), the module folder should be placed under `/home/deck/wow-server-playerbots/modules/`.*

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

Restart your `ac-worldserver` container/application. The module will dynamically register its spell overrides into the `spell_scripts` database table automatically during startup.

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
This module operates via hooks on:
* **`PlayerScript`**:
  * `OnPlayerBeforeLoadPetFromDB`: Bypasses the Death Knight exception check (which blocks pet loading without the "Master of Ghouls" talent) if the pet is not undead (i.e. demons/elementals).
  * `OnPlayerBeforeTempSummonInitStats`: Intercepts secondary minion summons and sets up `UNIT_MASK_CONTROLLABLE_GUARDIAN` and `CharmInfo` to enable `PetAI` and auto-casting of their default spells.
  * `OnPlayerIsClass`: Intercepts pet-context class queries, checking the player's learned spells to ensure correct permanency and action bar settings.
* **`SpellScript`**:
  * `SpellSummonPetOverrideScript`: Binds to summon spells and checks if the player already has an active pet. If so, it intercepts the hit effect, prevents default dismissal, and spawns the new summon as a controllable guardian.

---

## Compatibility & Single-Class Server Impact

This module is fully compatible with **any** AzerothCore 3.3.5a server setup, whether it runs standard single-class rules or custom multiclassing frameworks (like Dad's MMO Lab / Unbound). It is completely stable, lightweight, and will not cause server crashes or database corruption.

### Single-Class Server Impact (Caveats)
If installed on a standard, single-class WotLK server, the gameplay changes are minor and isolated:
* **Warlocks**: A single-class Warlock will be able to summon all of their demons (Imp, Voidwalker, Succubus, Felhunter, Felguard) at the same time. The first demon summoned will act as the primary pet (displaying the pet action bar and control frame), while any subsequent demons will spawn as controllable minions that follow and auto-cast their main abilities in combat.
* **Other Classes**: All other single-class players (Mages, Hunters, Death Knights) are **unaffected**. Their pet summoning rules, limits, and behaviors remain 100% standard and Blizzlike.

