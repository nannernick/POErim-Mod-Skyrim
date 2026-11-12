# Push notes: POErim 1.1.0, prefixes / suffixes / implicit

Do not commit this file.

## Commit message

```
Prefixes, suffixes and an implicit slot

- Affixes are now prefixes or suffixes (from their category, or an optional
  "type" CSV column). Items carry up to 2 of each (MaxPrefixes/MaxSuffixes
  in DiabloLoot.ini).
- The fifth affix slot is an implicit: one stronger affix tied to the item
  kind, from the new DiabloLoot_affixes_implicits.csv (explicit ranges x1.5),
  only at item level 35+. No two affixes on an item share a magic effect.
- Orbs: Exalted adds a prefix/suffix while there is room, the implicit on a
  full 2+2 item; Chaos rerolls prefixes/suffixes and keeps the implicit;
  Scouring removes everything.
- Names: prefix affixes name the front, suffix affixes the back.
- Tooltip: implicit above a rule, then prefixes and suffixes; holding Alt
  adds Implicit / Prefixes (n/2) / Suffixes (n/2) headings with tier and
  roll range.
- Debug menu Inspect shows each affix's kind and the prefix/suffix counts.
- Workflow packages the implicits table and runs the harness with it.
```

## New files

```
data/DiabloLoot_affixes_implicits.csv
```

## Changed files

```
.github/workflows/main.yml
README.md
data/DiabloLoot.ini
src/Config.cpp
src/DebugMenu.cpp
src/GridTint.cpp
src/Records.cpp
src/roll/AffixTable.cpp
src/roll/Craft.cpp
src/roll/Craft.h
src/roll/Roll.h
src/roll/Roller.cpp
```

## Files to delete

```
LICENSE-GPL   (same text as LICENSE)
```
