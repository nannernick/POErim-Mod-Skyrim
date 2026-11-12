// =============================================================================
//  Reloading the affix CSVs without a restart (2.1.0), and the vanilla
//  enchantment survey. Both live in main.cpp, which owns the table.
// =============================================================================
#pragma once

#include <string>

// GAME THREAD. Loads DiabloLoot_affixes.csv and every add-on into a fresh table,
// resolves its effects, then -- under Records::TableLock() held exclusively --
// replaces the live table's contents. Items already in the world keep their
// enchantments; their records are read against the new table from now on.
// False (and the live table untouched) when the base file does not load.
bool ReloadAffixTable(std::string& a_report);

// GAME THREAD. Every generic enchanted variant in the load order whose
// enchantment ReplaceVanillaEnchants would replace, grouped by the
// enchantment's effects. Written to the log in full; returns a short summary.
std::string ListReplaceableEnchantments();
