# Changelog

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
