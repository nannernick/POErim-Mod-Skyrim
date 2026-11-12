// =============================================================================
//  Crafting -- the orb rules, carried out on a real item.
// =============================================================================
//  roll/Craft.h decides what an orb does to a roll. This is the half that needs
//  the game: find the item in the player's inventory, read what it rolled, run
//  the rule, and turn the answer back into an enchantment on that same item.
//
//  ★A CRAFT IS A RELEASE FOLLOWED BY AN ATTACH, in that order, both through
//  Apply. The old created enchantment is let go with Apply::Release -- never
//  RemoveByType alone; Apply.h says what that did to saves -- and the new roll
//  goes on with Apply::ToItem, the same call every drop is made with. Nothing
//  here builds an enchantment or touches the created-object manager itself.
//
//  ★THREE KINDS OF TARGET, and which one an item is decides the route:
//
//    an item this mod rolled      its list carries our enchantment. Released
//                                 and re-attached in place.
//    a plain item WITH a list     tempered, owned, worn: the engine already
//                                 gave it per-instance data. Attached in place.
//    a plain item with NO list    an untouched stack unit. There is nothing to
//                                 attach to, and a list cannot be made by hand
//                                 (Distribute.cpp says why), so it goes through
//                                 Apply::ToNewInstance: dropped and picked back
//                                 up, the engine building the instance.
//
//  ★A WORN ITEM IS TAKEN OFF AND PUT BACK ON. The effects an actor feels were
//  read from the enchantment when the item was equipped; changing it underneath
//  them changes the item card and nothing else. The re-equip is what makes the
//  new affixes real.
//
//  EVERYTHING HERE RUNS ON THE GAME THREAD. The debug menu queues it through
//  SKSE's task interface; the orb-on-item flow will do the same.
// =============================================================================
#pragma once

#include "roll/Craft.h"

#include <string>
#include <string_view>

namespace Crafting
{
    // One unit in the player's inventory: its base form, and the created
    // enchantment it carries -- zero for a plain unit. Ids, not pointers,
    // because the request is made on one thread and carried out on another.
    struct Target
    {
        RE::FormID base{ 0 };
        RE::FormID enchantment{ 0 };
    };

    struct Outcome
    {
        bool         ok{ false };
        std::string  message;            // what happened, or why nothing did
        RE::FormID   enchantment{ 0 };   // the item's enchantment afterwards; 0 when plain
        std::uint8_t code{ 0 };          // roll::RarityCode afterwards
    };

    // Uses an orb on the item. Nothing is consumed: this is the craft itself,
    // and whoever calls it owns the orb. A refusal leaves the item untouched.
    Outcome ApplyOrb(const Target& a_target, roll::Orb a_orb);

    // Debug edits. Removing the last affix returns the item to its plain base.
    Outcome RemoveAffix(const Target& a_target, std::string_view a_affixId);
    Outcome RerollValue(const Target& a_target, std::string_view a_affixId);

    // Places a new item of this base form at the player's feet, rolled until it
    // lands in a_rarity (0 normal, 1 magic, 2 rare) at a_itemLevel. Every
    // rarity carries its base implicit.
    Outcome SpawnAtFeet(RE::FormID a_base, int a_rarity, int a_itemLevel);

    // ★FIRST SIGHT (2.1.0). Gives the unit its base implicit: a plain generic
    // item, a 1.2.0 item with no implicit (flagged so it is never redone), or
    // a vanilla/player-enchanted generic item whose enchantment is replaced
    // (ReplaceVanillaEnchants). Player's inventory only; game thread.
    Outcome GrantBaseImplicit(const Target& a_target);

    // Debug: the Vaal second-implicit outcome on its own (same rules, and it
    // corrupts), and the corrupted flag by hand.
    Outcome AddSecondImplicit(const Target& a_target);
    Outcome SetCorrupted(const Target& a_target, bool a_corrupted);

    // Whether this base form is something an orb could ever be used on, and a
    // short reason when it is not. Reads form data only: any thread.
    [[nodiscard]] bool Craftable(RE::TESBoundObject* a_object, const char*& a_reason);
}
