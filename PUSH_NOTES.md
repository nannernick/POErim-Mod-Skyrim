# Push notes: POErim 1.1.0 (Diablorim-Fork)

Do not commit this file.

## Commit message

```
POErim 1.1.0: per-item affix records, crafting rules, tooltips, debug menu, CI

- Co-save: new 'ITEM' record type holding each item's affix ids, tiers,
  values and item level, keyed by the created enchantment. Existing record
  types and their version are unchanged, so old saves load as before.
- Items rolled before this build get their record rebuilt from the
  enchantment's effects on first hover or inspect (item level estimated).
- Roller: the pool and single-affix draw are now shared functions; drop
  output is byte-identical to before.
- New pure crafting layer (Transmutation, Augmentation, Exalted, Chaos,
  Scouring) and harness modes --craft, --chaos-until, --merge.
- Grid tooltip: item level and affix lines; Alt shows tier and roll range.
- POErim Debug Menu in SKSE Menu Framework (optional dependency).
- ARPG POV is checked for at load and reported when missing.
- GitHub Actions workflow building the DLL and Vortex-installable zips.
- README and LICENSE (GPL-3.0).
```

## New files

```
.github/workflows/build.yml
LICENSE
README.md
include/SKSEMenuFramework.h
src/DebugMenu.cpp
src/DebugMenu.h
src/Records.cpp
src/Records.h
src/Version.h
src/roll/Craft.cpp
src/roll/Craft.h
src/roll/Record.cpp
src/roll/Record.h
```

## Changed files

```
CMakeLists.txt
data/DiabloLoot.ini
src/Apply.cpp
src/Apply.h
src/Config.cpp
src/Config.h
src/Distribute.cpp
src/GridTint.cpp
src/GridTint.h
src/Persist.cpp
src/Persist.h
src/QuestReward.cpp
src/Sockets.cpp
src/Sockets.h
src/main.cpp
src/roll/Roll.h
src/roll/Roller.cpp
tools/RollHarness/main.cpp
vcpkg.json
```

## Files to delete

None.
