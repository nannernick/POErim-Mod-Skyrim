// =============================================================================
//  Crafting orbs -- changing an item that already exists. Pure, like the roller.
// =============================================================================
//  Path of Exile 1's currency rules (2.1.0). Rarity is the explicit count
//  (Roll.h): 0 normal, 1-2 magic (one prefix + one suffix at most), 3-6 rare.
//  Implicits sit outside all of it and no orb adds or removes one, except a
//  Vaal Orb's chance at a second.
//
//      Transmutation   normal                 -> magic, 1 mod
//      Augmentation    magic with 1 mod       -> 2 mods, still magic (the
//                                                other kind: 1P + 1S at most)
//      Regal           magic with 2 mods      -> 3 mods, rare; nothing it had
//                                                changes
//      Alchemy         normal                 -> rare, 4 mods
//      Exalted         rare, 3 to 5 mods      -> one more; the fifth needs item
//                                                level fifthAffixMinLevel, the
//                                                sixth sixthAffixMinLevel
//      Chaos           rare, 3 to 6 mods      -> a new set, same count
//      Scouring        any mods               -> normal; implicits stay
//      Divine          anything with a range  -> every value, implicits
//                                                included, rerolled inside its
//                                                tier
//      Vaal            anything uncorrupted   -> CORRUPTS the item: no orb,
//                                                Divine and Scouring included,
//                                                works on it again. First a
//                                                VaalRules::addImplicitChance
//                                                draw for a second implicit
//                                                (a no-op when none can be
//                                                added); otherwise one of, by
//                                                weight:
//                                                  * the item is destroyed
//                                                    (can be switched off)
//                                                  * every value moves up, or
//                                                    every value moves down,
//                                                    inside its tier
//                                                  * the explicits become
//                                                    different affixes at
//                                                    random tiers
//                                                and when none of those apply,
//                                                corruption alone.
//
//  Every refusal leaves the item bit-identical, and the orb is not consumed.
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
        kRegal,   // 2.1.0, DiabloInSkyrim.esp 0x809
    };

    inline constexpr int kOrbCount = static_cast<int>(Orb::kRegal) + 1;

    [[nodiscard]] const char* OrbName(Orb a_orb) noexcept;       // "Orb of Transmutation"
    [[nodiscard]] const char* OrbKey(Orb a_orb) noexcept;        // "transmute"
    [[nodiscard]] bool        ParseOrb(std::string_view a_text, Orb& a_out) noexcept;

    // How many affixes an Orb of Alchemy puts on a plain item.
    inline constexpr int kAlchemyAffixes = 4;

    enum class CraftVerdict
    {
        kOk,
        kNotAffixable,      // the item presents no slot the table knows
        kNeedsNormal,       // Transmutation or Alchemy on an item with mods
        kNeedsOneAffix,     // Augmentation on an item without exactly one mod
        kNeedsRare,         // Exalted or Chaos on a normal or magic item
        kNeedsMagicTwo,     // Regal on a magic item with one mod
        kNeedsMagic,        // Regal on a normal or rare item
        kNothingToRemove,   // Scouring on a normal item
        kFull,              // every prefix and suffix slot is taken
        kFifthLocked,       // the fifth affix needs Tuning::fifthAffixMinLevel
        kNoEligibleAffix,   // nothing left in the pool for this item
        kSixthLocked,       // the sixth affix needs Tuning::sixthAffixMinLevel
        kNeedsAffixes,      // Divine on an item with nothing on it at all
        kNothingToReroll,   // Divine on an item whose affixes have no range
        kCorrupted,         // the item is corrupted; no orb works on it
        kNothingToCorrupt,  // Vaal on an item it cannot change at all
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
        kImplicitAdded,    // a second implicit
        kRerolled,
        kCorruptedOnly,    // corrupted and nothing else (no valid outcome)
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
