// =============================================================================
//  Crafting orbs -- changing an item that already exists. Pure, like the roller.
// =============================================================================
//  Path of Exile's currency, on this mod's own model. An orb does not have a
//  rarity to change: an item here is 0-5 affixes worth 1-3 points each and its
//  colour is the point sum, so every rule below is written in terms of HOW MANY
//  AFFIXES the item has, and the band simply falls out afterwards.
//
//      Transmutation   0 affixes        -> 1
//      Augmentation    exactly 1        -> 2
//      Exalted         2 to 4           -> one more (the fifth is level-gated)
//      Chaos           2 or more        -> all rerolled, same count
//      Scouring        1 or more        -> none: back to the plain base
//
//  A new affix is drawn by the SAME function a fresh roll uses (DrawAffix in
//  Roll.h): the item's slots, its stored item level, the affix weights and the
//  tier curve all apply, and an affix the item already carries is taken out of
//  the pool first. There is no second affix system here.
//
//  NOTHING HERE TOUCHES THE GAME. The plugin turns the result into a new
//  enchantment; the harness runs the same code a million times and prints what
//  a chain of orbs actually produces.
// =============================================================================
#pragma once

#include "Roll.h"

#include <string_view>

namespace roll
{
    enum class Orb
    {
        kTransmute,
        kAugment,
        kExalt,
        kChaos,
        kScour,
    };

    inline constexpr int kOrbCount = static_cast<int>(Orb::kScour) + 1;

    [[nodiscard]] const char* OrbName(Orb a_orb) noexcept;       // "Orb of Transmutation"
    [[nodiscard]] const char* OrbKey(Orb a_orb) noexcept;        // "transmute"
    [[nodiscard]] bool        ParseOrb(std::string_view a_text, Orb& a_out) noexcept;

    enum class CraftVerdict
    {
        kOk,
        kNotAffixable,      // the item presents no slot the table knows
        kNeedsNoAffixes,    // Transmutation on an item that already has some
        kNeedsOneAffix,     // Augmentation on an item without exactly one
        kNeedsTwoToFour,    // Exalted on an item with fewer than two
        kNeedsTwoOrMore,    // Chaos on an item with fewer than two
        kNothingToRemove,   // Scouring on a plain item
        kFull,              // already at kMaxAffixes
        kFifthLocked,       // a fifth affix needs Tuning::fifthAffixMinLevel
        kNoEligibleAffix,   // nothing left in the pool for this item
    };

    // One short line for the player: why the orb did nothing.
    [[nodiscard]] const char* Reason(CraftVerdict a_verdict) noexcept;

    // Whether the orb may be used on this item, without changing anything and
    // without touching the rng. This is what a hover highlight asks.
    //
    // a_ctx carries the item's STORED level and its slots; forNpc and noWhite
    // are ignored -- crafting is the player's, and an orb is never a null roll.
    [[nodiscard]] CraftVerdict CanCraft(Orb a_orb, const RolledItem& a_item,
        const RollContext& a_ctx, const AffixTable& a_table, const Tuning& a_tuning);

    // Applies the orb. On kOk a_item has changed and its tier is recomputed;
    // on anything else a_item is exactly as it was and the orb must not be
    // consumed.
    CraftVerdict Craft(Orb a_orb, RolledItem& a_item, const RollContext& a_ctx,
        const AffixTable& a_table, const Tuning& a_tuning, Rng& a_rng);
}
