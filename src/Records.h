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

#include <shared_mutex>

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
        std::uint8_t     code{ 0 };        // roll::RarityCode, from the co-save (0 = not yet drawn)
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

    // The rarity code for an enchantment, computing and storing it on a miss.
    // 0 for anything that is not ours. Any thread; the hot path only pays the
    // computation once per enchantment.
    [[nodiscard]] std::uint8_t CodeFor(const RE::EnchantmentItem* a_enchantment);

    // ★THE TABLE LOCK (2.1.0, for reloading the CSV without a restart).
    // Everything that holds a roll::RolledItem -- whose affixes point INTO the
    // table -- holds a shared lock for as long as it does; ReloadTable takes
    // it exclusively, swaps the contents and resolves the effects again.
    // RolledItems must not outlive the lock they were made under.
    [[nodiscard]] std::shared_mutex& TableLock();
    using ReadGuard = std::shared_lock<std::shared_mutex>;

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
