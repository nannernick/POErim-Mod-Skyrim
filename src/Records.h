// =============================================================================
//  Item records -- the engine-facing half of roll/Record.h.
// =============================================================================
//  Persist stores what an item rolled; the roller knows what a record means.
//  This file is the piece between them that needs the game: it finds the
//  record for the item in front of it, and -- for an item rolled before
//  records existed -- reads one back out of the item's own enchantment.
//
//  ★A LEGACY ITEM IS RECONSTRUCTED, NOT WRITTEN OFF. Its created enchantment
//  still holds every effect and every magnitude, each effect maps back to the
//  affix that produces it, and the magnitude says which tier row it came from.
//  The one thing that cannot be recovered is the ITEM LEVEL, which nothing
//  kept. It is estimated as the lowest level that could have rolled those
//  tiers, raised to the player's level at the moment of reconstruction -- an
//  old item is at least as good a crafting base as one dropped today -- and
//  the record is flagged so the debug menu can say it is an estimate.
//
//  What stays unreadable, and is therefore not craftable:
//    * an enchantment the player merged their own effects into at the arcane
//      enchanter (Persist::IsAffixEnch says no -- it is theirs now)
//    * one with an effect the affix table no longer has, or a magnitude no
//      tier row contains: the table has been edited since the item rolled
// =============================================================================
#pragma once

#include "roll/Record.h"
#include "roll/Roll.h"

namespace Records
{
    // Borrowed; must outlive the session. Call at kDataLoaded, after
    // Apply::ResolveEffects -- reconstruction asks Apply which effect each
    // affix resolved to.
    void SetTable(const roll::AffixTable& a_table);

    [[nodiscard]] const roll::AffixTable* Table();

    // Records a fresh roll against the enchantment Apply just attached. The
    // one call every roll site makes, right beside Persist::NoteEnchTier.
    void Note(RE::FormID a_enchantment, const roll::RolledItem& a_rolled, int a_itemLevel);

    struct View
    {
        RE::FormID       enchantment{ 0 };
        std::uint8_t     band{ 0 };        // roll::Band, from the co-save
        roll::ItemRecord record;
        roll::RolledItem rolled;           // the record against the table as loaded now
        bool             complete{ false };  // every affix in the record still resolves
    };

    // What this unit rolled. False for anything this mod did not roll, and for
    // the unreadable cases listed above. Reconstructs and stores a legacy
    // record on first ask. Any thread: it reads form data and takes Persist's
    // lock, nothing more -- it is called from the grid's tooltip.
    bool Lookup(const RE::ExtraDataList* a_xList, View& a_out);
    bool Lookup(const RE::EnchantmentItem* a_enchantment, View& a_out);

    // Drops the "could not be read" memo. Call wherever Sockets::Forget is
    // called: created-object ids are handed out again by the next save.
    void Forget();

    struct Stats
    {
        std::uint64_t reconstructed{ 0 };
        std::uint64_t unreadable{ 0 };
    };

    [[nodiscard]] Stats CurrentStats();
}
