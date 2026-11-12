// =============================================================================
//  Crafting orbs -- changing an item that already exists. Pure, like the roller.
// =============================================================================
//  Path of Exile's currency, on this mod's own model. An item is up to
//  MaxPrefixes() prefixes and MaxSuffixes() suffixes -- its EXPLICIT affixes,
//  six by default -- plus one IMPLICIT slot outside that cap which only
//  corruption fills (Roll.h). Each affix is worth 1-3 points and the colour is
//  the point sum, so every rule below is written in terms of the explicit count
//  and the band falls out.
//
//      Transmutation   a plain item           -> 1 explicit
//      Augmentation    exactly 1 explicit     -> 2
//      Alchemy         a plain item           -> 4 explicits at once
//      Exalted         2 to 5 explicits       -> one more, on a side with room;
//                                                the fifth needs item level
//                                                fifthAffixMinLevel, the sixth
//                                                sixthAffixMinLevel
//      Chaos           2 or more explicits    -> explicits rerolled, same count
//      Scouring        anything with affixes  -> nothing: back to the plain base
//      Divine          anything with affixes  -> every VALUE rerolled inside the
//                                                tier it already has
//      Vaal            anything uncorrupted   -> CORRUPTS the item: no orb works
//                                                on it again. One of, at equal
//                                                weight by default:
//                                                  * the item is destroyed
//                                                    (can be switched off)
//                                                  * every value moves up, or
//                                                    every value moves down,
//                                                    inside its tier
//                                                  * an implicit is added,
//                                                    outside the six
//                                                  * the explicits become
//                                                    different affixes at
//                                                    random tiers
//                                                An outcome the item cannot take
//                                                is left out of the draw.
//
//  A new affix is drawn by the SAME function a fresh roll uses (DrawAffix in
//  Roll.h): the item's slots, its stored item level, the affix weights and the
//  tier curve all apply, and FitPool removes what the item already has, what
//  shares a mod group with it, and any kind that is full. There is no second
//  affix system here.
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
    // ★THE ORDER IS FROZEN. The orb items in DiabloInSkyrim.esp are numbered
    // from it (0x801 + this value), the debug menu indexes by it, and a drop
    // table in DiabloLoot.ini lists weights in it. New orbs go on the end.
    enum class Orb
    {
        kTransmute,
        kAugment,
        kAlchemy,
        kExalt,
        kChaos,
        kScour,
        kDivine,
        kVaal,
    };

    inline constexpr int kOrbCount = static_cast<int>(Orb::kVaal) + 1;

    [[nodiscard]] const char* OrbName(Orb a_orb) noexcept;       // "Orb of Transmutation"
    [[nodiscard]] const char* OrbKey(Orb a_orb) noexcept;        // "transmute"
    [[nodiscard]] bool        ParseOrb(std::string_view a_text, Orb& a_out) noexcept;

    // How many affixes an Orb of Alchemy puts on a plain item.
    inline constexpr int kAlchemyAffixes = 4;

    enum class CraftVerdict
    {
        kOk,
        kNotAffixable,      // the item presents no slot the table knows
        kNeedsNoAffixes,    // Transmutation or Alchemy on an item that already has some
        kNeedsOneAffix,     // Augmentation on an item without exactly one explicit
        kNeedsTwoToFour,    // Exalted on an item with fewer than two explicits
        kNeedsTwoOrMore,    // Chaos on an item with fewer than two explicits
        kNothingToRemove,   // Scouring on a plain item
        kFull,              // every prefix and suffix slot is taken
        kFifthLocked,       // the fifth affix needs Tuning::fifthAffixMinLevel
        kNoEligibleAffix,   // nothing left in the pool for this item
        kSixthLocked,       // the sixth affix needs Tuning::sixthAffixMinLevel
        kNeedsAffixes,      // Divine on a plain item
        kNothingToReroll,   // Divine on an item whose affixes have no range
        kCorrupted,         // the item is corrupted; no orb works on it
        kNothingToCorrupt,  // Vaal on an item none of its outcomes apply to
    };

    // One short line for the player: why the orb did nothing.
    [[nodiscard]] const char* Reason(CraftVerdict a_verdict) noexcept;

    // What a successful craft did beyond "the affixes changed". Only a Vaal Orb
    // reports anything but kNone.
    enum class CraftEffect
    {
        kNone,
        kDestroyed,        // the item is gone; a_item is left as it was
        kValuesUp,
        kValuesDown,
        kImplicitAdded,
        kRerolled,
    };

    [[nodiscard]] const char* EffectName(CraftEffect a_effect) noexcept;

    // Whether the orb may be used on this item, without changing anything and
    // without touching the rng. This is what a hover highlight asks.
    //
    // a_ctx carries the item's STORED level and its slots; forNpc and noWhite
    // are ignored -- crafting is the player's, and an orb is never a null roll.
    [[nodiscard]] CraftVerdict CanCraft(Orb a_orb, const RolledItem& a_item,
        const RollContext& a_ctx, const AffixTable& a_table, const Tuning& a_tuning);

    // Applies the orb. On kOk a_item has changed and its tier is recomputed
    // (or, for CraftEffect::kDestroyed, the item is to be removed); on anything
    // else a_item is exactly as it was and the orb must not be consumed.
    CraftVerdict Craft(Orb a_orb, RolledItem& a_item, const RollContext& a_ctx,
        const AffixTable& a_table, const Tuning& a_tuning, Rng& a_rng,
        CraftEffect* a_effect = nullptr);
}
