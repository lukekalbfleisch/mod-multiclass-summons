# mod-multiclass-summons

An AzerothCore module for World of Warcraft 3.3.5a (WotLK) that resolves pet limitations in a multiclassing environment (such as DadsMMOLab's Unbound). 

---

## Features

* **Multiclass Pet Respawn Fix**: Resolves mounting, dismounting, and pet-load blocker bugs when using a Death Knight base class with a Warlock/Mage subclass.
* **Multi-Active Summons**: Allows players to have multiple summons (Imp, Voidwalker, Succubus, Felhunter, Felguard, Water Elemental, Ghoul) active simultaneously.
  * **Primary Summon**: The first summon claims the pet slot and gains full client-side action bar control.
  * **Secondary Summons**: Subsequent summons spawn as controllable side guardians that follow and attack automatically.
  * **Full Abilities**: Summons are granted their real pet ability sets (correct spell IDs/ranks) instead of basic attacks.
  * **Defensive AI**: Summons default to a defensive state rather than aggressive/pull-on-sight.
  * **Automatic Promotion**: If the primary summon dies or is dismissed, a secondary summon is promoted to primary.

---

## Limitations & Compatibility

* **Session-Only Summons**: Secondary summons are session-only and will not persist/restore after logging out or a server restart.
* **One Client Action Bar**: Due to WotLK client limitations, only the primary summon receives a pet action bar. Secondary summons are controlled automatically via wow' guardian logic.
* **One Summon per Creature Type**: Players cannot stack duplicates of the same creature type (e.g., one Imp and one Voidwalker are allowed, but not two Imps). Recasting refreshes the existing summon.
* **Playerbots Excluded**: Playerbots are excluded from multi-active summons and retain their standard single-pet behavior.
* **Compatibility**: Works on both standard AzerothCore and playerbots-enabled cores. Playerbot checking compiles out automatically if the `playerbots` module is not present.

---

## Installation

1. Copy or clone this repository into the `modules/` directory of your AzerothCore source folder:
   ```bash
   cd /path/to/azerothcore-wotlk/modules
   git clone https://github.com/bdodroid/mod-multiclass-summons.git
   ```

2. Rebuild your AzerothCore containers:
   ```bash
   docker compose build ac-worldserver
   ```

3. Restart the containers:
   ```bash
   docker compose up -d
   ```

---

## Database Cleanup (Optional)

If your characters have already encountered pet corruption bugs prior to installing this module, reset their pet database records:

```sql
USE acore_characters;
DELETE FROM character_pet WHERE owner = [CHARACTER_GUID];
```

---

## Technical Details

A file-local `SummonManager` handles:
* Managing primary/secondary summon creation and deletion.
* Ensuring uniqueness (one per creature entry) and automatic primary promotion.
* Session cleanup.

The module hooks into:
* **`SpellScript`** (`SpellSummonPetOverrideScript`): Intercepts pet summon spells to divert them to the `SummonManager`.
* **`PlayerScript`**:
  * `OnPlayerBeforeTempSummonInitStats`: Flags summons as controllable guardians and injects their correct spell sets.
  * `OnPlayerUpdate`: Handles periodic cleanup of dead summons and primary promotion (safely disabled while mounted to prevent dismount crashes).
  * `OnPlayerLogout`: Clears active summons.
  * `OnPlayerBeforeLoadPetFromDB`: Allows standard hunter pets for multiclass characters while bypassing Death Knight pet logic for non-undead pets.
  * `OnPlayerIsClass`: Evaluates class-specific pet properties (e.g., action bars, power types) for multiclass characters based on their known spells.
