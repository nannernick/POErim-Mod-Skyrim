# Push notes: POErim 1.2.0, crafting on real items

Do not commit this file.

## Commit message

```
POErim 1.2.0: crafting on real items from the debug menu

- New Crafting module: finds the item in the player's inventory, runs the
  orb rule, releases the old created enchantment through Apply::Release and
  attaches the new roll with Apply::ToItem, then updates band, record
  (flagged crafted), name and grid tint.
- Plain items with instance data are enchanted in place; bare stack units go
  through Apply::ToNewInstance (drop and pick up). Equipped items are taken
  off, crafted and put back on.
- Refused with a reason: items with their own enchantment, uniques, quest
  items, unreadable legacy items.
- Debug menu: new "Craft on items" page -- apply any orb for free, remove an
  affix, reroll an affix's value, spawn a base item on the ground at a chosen
  colour and item level. Each action needs a confirm tick and logs a line.
- Inventory snapshot now also lists plain craftable items.
```

## New files

```
src/Crafting.cpp
src/Crafting.h
```

## Changed files

```
CMakeLists.txt
README.md
src/DebugMenu.cpp
src/Version.h
vcpkg.json
```

## Files to delete

```
LICENSE-GPL   (same text as LICENSE)
```
