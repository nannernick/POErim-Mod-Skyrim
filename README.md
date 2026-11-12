# POErim

Diablo-style randomized affix loot for Skyrim SE/AE, with Path of Exile style currency crafting being built on top of it. An SKSE plugin; no ESP yet.

POErim is a fork of **Diablo In Skyrim (DiabloRim)** by tangwesley. All of the loot system is that mod's work: the roller, the affix tables, how affixes are attached to items and kept in the save. This fork adds per-item affix records, the crafting rules, richer tooltips and a debug menu, and is designed to be played with [ARPG POV](https://www.nexusmods.com/skyrimspecialedition/mods/198647).

> **Do not install POErim alongside the original Diablo In Skyrim.** They are the same plugin (`DiabloInSkyrim.dll`) and share one co-save. POErim replaces it, and reads its saves.

## Status

| | |
|---|---|
| Affix loot on NPC gear, containers, vendors, quest rewards | Working (from Diablo In Skyrim) |
| Per-item affix records in the co-save | **New in 1.1.0** |
| Old items get their record rebuilt from their enchantment | **New in 1.1.0** |
| Prefixes, suffixes and an implicit slot (PoE layout) | **New in 1.1.0** |
| Tooltips: item level and affix lines; Alt shows Implicit / Prefixes / Suffixes with tier and roll range | **New in 1.1.0** |
| Debug menu in SKSE Menu Framework | **New in 1.1.0** |
| Crafting rules for the five orbs, in the offline harness and the in-game simulator | **New in 1.1.0** |
| Crafting on real items, from the debug menu (free orbs) | **New in 1.2.0** |
| Orbs as items, orb drops, using orbs in Grid Inventory, ARPG POV labels for orbs | Not yet: next builds |

## Features

**Loot.** Weapons, armour and jewellery roll 0 to 5 affixes. Each affix is tier I to III, worth 1 to 3 points, and the point total sets the item's colour:

| Colour | Points |
|---|---|
| White | 0 |
| Blue | 1-3 |
| Yellow | 4-6 |
| Purple | 7-9 |
| Orange | 10-12 |
| Red | 13-15 |

Affixes follow Path of Exile's layout: up to **2 prefixes** and **2 suffixes** (the explicit affixes crafting works on), plus an **implicit** in the last slot. The implicit is one stronger affix tied to the kind of item (elemental damage on weapons, resistances on rings, life and armour on body armour, and so on: `DiabloLoot_affixes_implicits.csv`), and only rolls on items of level 35 or higher. Red needs all five slots, so red still starts at 35.

Prefixes are damage, leech, attributes, armour and unarmed damage; everything else is a suffix. Each affix's kind comes from its category, or from an optional last `type` column in the CSV. NPC gear rolls at the NPC's level; container and quest loot at yours. Vanilla-enchanted items, uniques and quest items are left alone.

**Item records.** Every rolled item now keeps what it rolled: affix ids, tiers, values and the item level. Items from a save made before 1.1.0 have that read back out of their enchantment the first time you hover them; the affixes are exact and the item level is an estimate (shown as "estimated").

**Tooltips** (Grid Inventory). Under the item's name: its item level, the implicit above a rule, then the prefixes and suffixes in the item's colour. Hold **Alt** to split them under Implicit / Prefixes (n/2) / Suffixes (n/2) headings and add each affix's tier and the range that tier can roll.

**Crafting orbs.** In this build orbs are used from the debug menu's *Craft on items* page and cost nothing; orb items and drops come next.

| Orb | Works on | Does |
|---|---|---|
| Orb of Transmutation | a plain item | adds 1 prefix or suffix |
| Orb of Augmentation | exactly 1 prefix or suffix | adds 1 |
| Exalted Orb | 2 or 3 prefixes/suffixes | adds 1 on a side with room |
| Exalted Orb | full 2 + 2 | adds the implicit (needs item level 35) |
| Chaos Orb | 2 or more prefixes/suffixes | rerolls them, keeps the count and the implicit |
| Orb of Scouring | anything | removes everything, implicit included |

What can be crafted: weapons, armour and jewellery that the loot system itself would roll. Not craftable: items with an enchantment of their own (vanilla-enchanted, player-enchanted, socketed), uniques and artifacts, quest items, and old items whose affixes are no longer in the table. An equipped item is taken off, crafted and put back on, so its new affixes apply at once.

New affixes are drawn by the same rules as a drop: the item's slots, its stored item level, the prefix/suffix caps, no affix twice and no two affixes with the same effect. The colour is recalculated from the points, so it can change. An orb that cannot be used does nothing and is not spent.

## Requirements

| | |
|---|---|
| Game | Skyrim SE / AE, 1.5.97 through 1.7.x |
| Required | [SKSE64](https://skse.silverlock.org/), [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444) |
| Required | [ARPG POV](https://www.nexusmods.com/skyrimspecialedition/mods/198647). POErim is built around its camera, labels, outlines and loot filter. Without it POErim logs an error and shows a message on load, and keeps running so existing saves are not broken. |
| Recommended | Grid Inventory, for item colours, tooltips and prices in the inventory |
| Optional | SKSE Menu Framework, for the debug menu (and required by ARPG POV's point and click) |
| Optional | Socketing, Summermyst (affix add-on table included) |

## Install

1. Remove the original Diablo In Skyrim if you have it. Your saves carry over.
2. Download the `POErim` artifact from the latest [Actions build](../../actions) (or a release, once there is one).
3. In Vortex: *Install From File*, pick the zip, enable it.
4. Optional: install `POErim-Summermyst-Affixes` the same way if you use Summermyst.

The zip contains `SKSE/Plugins/DiabloInSkyrim.dll`, `DiabloLoot.ini`, `DiabloLoot_affixes.csv` and `DiabloLoot_affixes_implicits.csv`. Updating overwrites `DiabloLoot.ini`; keep a copy if you have edited it.

## Configuration

Player settings are in `Data/SKSE/Plugins/DiabloLoot.ini`. Every setting is optional and documented in the file. New in 1.1.0:

| Setting | Default | |
|---|---|---|
| `MaxPrefixes` | 2 | Prefixes an item can carry |
| `MaxSuffixes` | 2 | Suffixes an item can carry |
| `TooltipItemLevel` | 1 | Show the item level on the tooltip |
| `TooltipAffixLines` | 1 | Show one line per affix. Set to 0 if your grid already lists the same effects |
| `TooltipDetailKey` | 18 (Alt) | Hold to show the Implicit / Prefixes / Suffixes headings, tiers and roll ranges. A Windows virtual-key code; 0 turns it off |

Loot balance is in `DiabloLoot_affixes.csv`, implicits in `DiabloLoot_affixes_implicits.csv`. Any file named `DiabloLoot_affixes_*.csv` beside it is merged on top. Summermyst's affixes have no category the mod recognises, so they all count as suffixes unless you add a `type` column.

## Debug menu

With SKSE Menu Framework installed, press **F1** and open **POErim Debug Menu**:

| Page | What it does |
|---|---|
| Integration | Version, game runtime, and whether ARPG POV, Grid Inventory, Socketing and the affix table were found |
| Inspect | The item last hovered in Grid Inventory, and every rolled item you carry: colour, points, item level, prefix/suffix/implicit counts, each affix with its kind, tier, value and range, the created enchantment's FormID, what the co-save holds for it |
| Craft simulator | Uses one orb on a copy of an item N times and shows the colour split. Changes nothing in the game |
| Craft on items | **Changes your save.** Pick a carried item (rolled or plain), then: apply any orb for free, remove a single affix, or reroll one affix's value. Also spawns any base item on the ground at a chosen colour and item level (enter its FormID). Each action needs the confirm box ticked and writes a log line |
| Co-save | Counts of everything saved, a check for orphaned enchantments, and "clear rolled marks for this cell" (changes the save: asks first, and logs it) |
| Log | Log level for this session, and where the log file is |

Spawning orbs, drop tuning and reloading the CSV without a restart arrive with the drops build.

## Tuning offline: the roll harness

`RollHarness.exe` (in the `POErim-Tools` artifact) runs the roller and the orbs without the game.

```
RollHarness DiabloLoot_affixes.csv --merge DiabloLoot_affixes_implicits.csv --levels 1,12,25,35,40
RollHarness DiabloLoot_affixes.csv --craft transmute,augment,exalt,exalt,exalt
RollHarness DiabloLoot_affixes.csv --chaos-until purple
RollHarness DiabloLoot_affixes.csv --merge DiabloLoot_affixes_summermyst.csv --craft transmute,augment
```

The crafting modes also report whether every item survives the save record, and whether each affix's tier can be recovered from its value alone, which is what rebuilding old items relies on. In the Summermyst table a few tier ranges touch, so about 1 affix in 1,000 on an old item may be read as the neighbouring tier.

## Building

GitHub Actions builds every push (`.github/workflows/build.yml`). Locally: Visual Studio 2022, CMake, vcpkg with the `x64-windows-static` triplet; CommonLibSSE-NG is fetched at a pinned commit.

## Credits

- **tangwesley**: Diablo In Skyrim (DiabloRim), which this is a fork of
- **skypia0147-dev**: Grid Inventory and its extension API (`src/api/GridInventoryAPI.h`, vendored)
- **Socketing**: its published API header (`src/api/SocketingAPI.h`, vendored)
- **SKSE Menu Framework**: its public header (`include/SKSEMenuFramework.h`, vendored)
- **alandtse and the CommonLibSSE-NG contributors**
- Path of Exile is Grinding Gear Games'. POErim is a fan project and uses none of its assets.

## Licence

GPL-3.0. See [LICENSE](LICENSE).
