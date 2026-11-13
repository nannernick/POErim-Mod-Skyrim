# Changelog

## 2.1.1

### Fixed
- **Orbs landed on the inventory board instead of the pouch** after a corpse *Take* / *Take all*, a purchase or a chest transfer, and only moved to the pouch when something else (moving gold, say) rebuilt the board. Those arrive through Grid Inventory's partial add, which never asked the pouch filter; only a full rebuild did. Every way a tile is added now asks it, and a board built with the filter one way (a container or shop open) is rebuilt as soon as it is shown the other way (the plain inventory).
- **Splitting an orb stack made every orb vanish** until the next rebuild. A split of an orb the pouch shows is refused and the board rebuilt; the engine's count never moves, so nothing is lost or duplicated.
- **The pouch filter only ever matches the nine orbs** (`DiabloInSkyrim.esp` 0x801-0x809, by form ID, MISC only). It already asked POErim's form-ID list; the MISC check is a second guard so quest items, keys, gems and soul gems can never be hidden or armed.
- **A hover or an orb could swap out a quest item.** When every unit of a form had a list of its own (a single quest item, say), the "plain unit" lookup still answered "a bare stack unit", and the bare-unit path let the engine pick which unit to drop or, for an enchanted record, remove. Quest-aliased units are now skipped in every branch of the lookup, a bare unit is only used when one exists, and a bare unit beside a quest copy is refused.
- **Loads and load doors.** Deaths, actor-loaded rolls, the post-load sweeps, quest rewards, first-sight implicits and wielder reconciles no longer touch a reference or an inventory while a save loads, while the load screen is up, or while the player has no attached cell. Deaths and the post-load sweeps wait and run when the load screen goes down; a corpse whose cell is not attached is left alone. Nothing re-queues itself to wait.
- **Grid Inventory UI state across loads.** Text-input capture only counts while the board is on screen, so a field left focused can no longer keep closing the Tween menu (Tab) and the Journal (Esc) or block movement after the menu has gone. At every save load, every load screen (load doors included) and every menu close: text focus, held keys and mouse buttons, the armed orb, the pouch's drag and queued drops are cleared and the quick wheel is closed (an open wheel blanks every button at MenuControls).
- **The pouch's game-thread tick does nothing** without a player with 3D in an attached cell, or while the load screen is up; its window draws only over a live board.
- The debug menu's *Reset from the INI* now puts `EnemyGearMode`, `OrbDrops`, `OrbPlacement` and `BundleGold` back to their defaults when the INI has no line for them.

### Changed
- **Orbs drop into the corpse by default**: `EnemyGearMode=ALL_INVENTORY` in code and in `DiabloLoot.ini` (was `ORBS_GROUND`).
- **Currency pouch:** the toggle is labelled *Orbs*, the window shows your total or "Orbs you pick up appear here" while empty, and a **Deposit all** button takes every orb off the board and keeps it in the pouch (the engine's inventory is not touched).
- POErim's Grid build defaults to the **Fable Crimson** skin. `!skin3` in `GridInventory_ui.ini` still wins.

### New settings

| File | Setting | Default | What it does |
|---|---|---|---|
| `DiabloLoot.ini` | `DebugLoadLog` | 0 | Logs load screens, save loads, and every task that waited for a load or was skipped because its cell was unloading |
| `GridInventory_ui.ini` | `CurrencyPouchDebugLog` | 0 | A line per board refresh: what asked for it, tiles shown, orbs hidden for the pouch, other forms hidden, and where each quest item went |
| `GridInventory_ui.ini` | `UiStateDebugLog` | 0 | A line per menu open/close and per reset at loads, load screens and menu closes, with the capture and block flags |

Changed default: `EnemyGearMode` ORBS_GROUND → ALL_INVENTORY.

### Known issues
- Orbs on the ground still use a vanilla cut-gem mesh; the orb texture does not show in the world.
- Orbs inside a corpse get no ARPG POV name tag (labels are for ground items).
- The "menu keys dead after a load door" report and the cell-change crash are not reproduced. See the README's Known issues for the logs to send.

## 2.1.0

### Fixed
- **Orbs 2x2 / chest or gem icon on the cursor.** Each orb used to borrow a different vanilla mesh, picked round-robin from every Skyrim.esm MISC with `VendorItemGem` (whatever mesh the load order gave it). Grid Inventory sizes an untuned item from any tuned item that shares its mesh, and captures its icon from that mesh. So an orb that landed on a shared or overridden mesh came out 2x2 with that item's picture. Now every orb uses one vanilla cut-gem mesh, Grid pins every currency to 1x1 (20 to a cell) in every window, and currencies draw only their own PNG (no 3D capture, no category drawing), with a log warning on a miss.
- **A failed rebuild no longer leaves an item plain.** The new enchantment is built before the old one is released (`Apply::Replace`). If the build fails, the item is unchanged.
- **Summermyst tier misreads** (about 1 in 2,000 rebuilt legacy mods): tier matching now prefers the row that holds the value exactly, before allowing float slack.
- **Outfit gear** (guard helmets and the like) counts as ordinary loot, so it stays craftable after Scouring.
- The affix CSVs can be **reloaded without a restart** (debug menu), behind a table lock.

### Changed
- **Rarity by mod count**, as in Path of Exile: normal / magic (1-2, 1P+1S) / rare (3-6), corrupted red. The six point bands are gone from colour, naming, tooltips and the API. Old items are re-coloured by their count.
- **Implicits replace enchantments.** Every craftable item has exactly one base implicit from creation. Vanilla-enchanted ordinary items have their enchantment replaced (`ReplaceVanillaEnchants`). Implicits show only with Alt. A Vaal Orb may add a second.
- **Orbs follow Path of Exile 1 rules.** Augmentation keeps an item magic, Exalted and Chaos need rare, Scouring keeps implicits, Divine rerolls implicits too, and corruption locks everything.
- Vaal: the "add an implicit" outcome became a 25% chance at a second implicit. The split stays 25% each by default.
- Price and charge multipliers follow the point total, not the colour.
- Orb sprites redrawn: original 16x16 pixel art with a 2 px black outline.

### Added
- **Regal Orb** (magic with 2 mods -> rare with 3). New form `DiabloInSkyrim.esp` 0x809.
- **Drop quantity by difficulty** (x1.00 Novice to x1.75 Legendary; `DropQuantityMultiplier`).
- **Currency pouch** window in Grid Inventory.
- **DiabloLootAPI v2** ('DLH2'): rarity, corrupted, currency flag and colour per item. v1 is unchanged.
- Grid Inventory `kMsgCurrencyReport` signal (no ABI change).
- Debug menu: per-orb mesh, size and icon check; ARPG POV API status; implicit editing; Vaal-add implicit; corrupted lock; difficulty controls; CSV reload; list of vanilla enchantments that will be replaced.
- Roll harness: `--selftest`, Regal and implicit rule checks, `--drop-mult` (difficulty), `--orb-weights`, `--bare`.

### Co-save
- 'ITEM' record version 2 (explicits and implicits as two lists); version 1 still loads.
- 'ETIR' version 2 holds the rarity code. Version 1 bands are discarded and recomputed from the records.
- Saves written by 2.1.0 load in 1.2.0 with uncoloured items and records rebuilt from enchantments. Don't go back.
