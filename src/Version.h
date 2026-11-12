// =============================================================================
//  One place for the version and the name the player sees.
// =============================================================================
//  POErim is this fork's name: Diablo In Skyrim's affix loot, plus Path of
//  Exile style crafting on top of it.
//
//  ★THE PLUGIN'S OWN NAME DOES NOT CHANGE, and that is deliberate. The DLL is
//  still DiabloInSkyrim.dll and SKSE still knows it as "DiabloInSkyrim", for
//  two reasons that both come down to saves:
//
//    * the co-save is keyed 'DISK' either way, so POErim and the original
//      loaded side by side would each read and write the other's records. One
//      file name means a mod manager replaces one with the other instead of
//      installing both.
//    * the data files (DiabloLoot.ini, DiabloLoot_affixes*.csv) are ones
//      players have already edited. Renaming them would silently reset every
//      setting on update.
//
//  So the name below is what menus, messages and the README say, and nothing
//  that touches a file or a save.
// =============================================================================
#pragma once

#include <cstdint>

namespace Poerim
{
    inline constexpr std::uint16_t kMajor = 1;
    inline constexpr std::uint16_t kMinor = 1;
    inline constexpr std::uint16_t kPatch = 0;

    inline constexpr const char* kDisplayName = "POErim";
}
