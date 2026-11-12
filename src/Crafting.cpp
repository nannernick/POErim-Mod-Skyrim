// =============================================================================
//  Crafting. See Crafting.h.
// =============================================================================

#include "PCH.h"

#include "Crafting.h"

#include "Apply.h"
#include "GridTint.h"
#include "Persist.h"
#include "Records.h"
#include "Wielder.h"

#include "roll/Record.h"

#include <algorithm>
#include <format>
#include <random>

namespace
{
    roll::Rng& Rng()
    {
        static thread_local roll::Rng rng{ std::random_device{}() };
        return rng;
    }

    bool IsWeapon(const RE::TESBoundObject* a_object)
    {
        return a_object && a_object->Is(RE::FormType::Weapon);
    }

    // A base record that ships with an enchantment of its own. Apply declines
    // these at attach time; asked here first so the player gets a reason
    // instead of an orb that silently did nothing.
    bool HasOwnEnchantment(RE::TESBoundObject* a_object)
    {
        if (auto* weapon = a_object->As<RE::TESObjectWEAP>()) {
            return weapon->formEnchanting != nullptr;
        }
        if (auto* armor = a_object->As<RE::TESObjectARMO>()) {
            return armor->formEnchanting != nullptr;
        }
        return false;
    }

    // ---- finding the unit ----------------------------------------------------

    struct Unit
    {
        RE::TESBoundObject* object{ nullptr };
        RE::ExtraDataList*  xList{ nullptr };   // null: a plain unit with no instance data
        bool                found{ false };
        bool                worn{ false };
        bool                wornLeft{ false };
    };

    void NoteWorn(Unit& a_unit)
    {
        if (!a_unit.xList) {
            return;
        }
        a_unit.wornLeft = a_unit.xList->HasType<RE::ExtraWornLeft>();
        a_unit.worn = a_unit.wornLeft || a_unit.xList->HasType<RE::ExtraWorn>();
    }

    // GetInventory hands back its own copy of each entry; the lists inside are
    // the real ones. Walking the copy is what keeps this off the list the
    // engine is working on (Apply.cpp records what walking that one cost).
    Unit FindUnit(RE::PlayerCharacter* a_player, const Crafting::Target& a_target)
    {
        Unit unit;
        auto inventory = a_player->GetInventory([&](RE::TESBoundObject& a_object) {
            return a_object.GetFormID() == a_target.base;
        });

        for (auto& [object, data] : inventory) {
            const auto  count = data.first;
            const auto& entry = data.second;
            if (!object || count <= 0) {
                continue;
            }
            unit.object = object;

            RE::ExtraDataList* plainWorn = nullptr;
            if (entry && entry->extraLists) {
                for (auto* xList : *entry->extraLists) {
                    if (!xList) {
                        continue;
                    }
                    const auto* xEnch = xList->GetByType<RE::ExtraEnchantment>();
                    if (a_target.enchantment != 0) {
                        if (xEnch && xEnch->enchantment &&
                            xEnch->enchantment->GetFormID() == a_target.enchantment) {
                            unit.xList = xList;
                            unit.found = true;
                            NoteWorn(unit);
                            return unit;
                        }
                        continue;
                    }
                    // A plain unit is wanted. A list that stands for several
                    // units would enchant all of them at once, so it is not one.
                    if (xEnch || xList->HasType(RE::ExtraDataType::kAliasInstanceArray) ||
                        xList->GetCount() != 1) {
                        continue;
                    }
                    const bool worn = xList->HasType<RE::ExtraWorn>() ||
                                      xList->HasType<RE::ExtraWornLeft>();
                    if (!worn) {
                        unit.xList = xList;   // the one in the pack, by preference
                        unit.found = true;
                        return unit;
                    }
                    if (!plainWorn) {
                        plainWorn = xList;
                    }
                }
            }

            if (a_target.enchantment == 0) {
                // No loose list: the worn one if there is one, otherwise a bare
                // stack unit, which ToNewInstance will give an instance.
                unit.xList = plainWorn;
                unit.found = true;
                NoteWorn(unit);
            }
            return unit;
        }
        return unit;
    }

    // ---- taking a worn item off and putting it back --------------------------

    const RE::BGSEquipSlot* SlotFor(const Unit& a_unit)
    {
        if (!a_unit.wornLeft) {
            return nullptr;   // the engine's own choice: right hand, or the armour's slot
        }
        auto* defaults = RE::BGSDefaultObjectManager::GetSingleton();
        return defaults ? defaults->GetObject<RE::BGSEquipSlot>(
                              RE::BGSDefaultObjectManager::DefaultObject::kLeftHandEquip)
                        : nullptr;
    }

    void Unequip(RE::PlayerCharacter* a_player, const Unit& a_unit)
    {
        if (auto* manager = RE::ActorEquipManager::GetSingleton()) {
            // Not queued, no sound, applied now: this is bookkeeping, not the
            // player changing their gear.
            manager->UnequipObject(a_player, a_unit.object, a_unit.xList, 1, SlotFor(a_unit), false,
                false, false, true);
        }
    }

    void Equip(RE::PlayerCharacter* a_player, const Unit& a_unit, RE::ExtraDataList* a_xList)
    {
        if (auto* manager = RE::ActorEquipManager::GetSingleton()) {
            manager->EquipObject(a_player, a_unit.object, a_xList, 1, SlotFor(a_unit), false, false,
                false, true);
        }
    }

    // ---- the rebuild ----------------------------------------------------------

    void NoteRoll(RE::FormID a_enchantment, const roll::RolledItem& a_rolled, int a_itemLevel,
        std::uint8_t a_flags)
    {
        Persist::NoteEnchTier(a_enchantment, static_cast<std::uint8_t>(roll::BandOf(a_rolled.tier)));
        Persist::NoteItemRecord(a_enchantment, roll::MakeRecord(a_rolled, a_itemLevel, a_flags));
    }

    // Makes the unit carry a_rolled -- or nothing, when a_rolled is empty.
    // a_hadOurs says whether the unit's list carries one of our enchantments
    // now, which is the one that has to be released first.
    Crafting::Outcome Rebuild(RE::PlayerCharacter* a_player, const Unit& a_unit, bool a_hadOurs,
        const roll::RolledItem& a_rolled, int a_itemLevel, std::uint8_t a_flags)
    {
        Crafting::Outcome outcome;
        const bool        weapon = IsWeapon(a_unit.object);
        const bool        toPlain = a_rolled.affixes.empty();
        const auto        id = a_unit.object->GetFormID();

        // ---- a bare stack unit: the engine has to build the instance --------
        if (!a_unit.xList) {
            if (toPlain) {
                outcome.message = "This item has no affixes to remove";
                return outcome;
            }
            if (!Apply::CanDropFrom(a_player)) {
                outcome.message = "This item can't be crafted here";
                return outcome;
            }
            logger::info("craft[{:08X}]: plain unit with no instance data -- going through "
                         "the drop-and-pickup",
                id);
            const auto applied = Apply::ToNewInstance(a_rolled, a_unit.object, a_player);
            if (!applied) {
                outcome.message = applied.declined
                    ? "The game handed back a different copy of this item; try again"
                    : "The enchantment could not be built";
                logger::warn("craft[{:08X}]: drop-and-pickup {} -- item unchanged", id,
                    applied.declined ? "declined" : "failed");
                return outcome;
            }
            const auto enchID = applied.enchantment->GetFormID();
            NoteRoll(enchID, a_rolled, a_itemLevel, a_flags);
            outcome.ok = true;
            outcome.enchantment = enchID;
            outcome.band = static_cast<std::uint8_t>(roll::BandOf(a_rolled.tier));
            outcome.message = applied.name;
            return outcome;
        }

        // ---- an item with a list: in place ------------------------------------
        //
        // ★THE ORDER AROUND A WORN ITEM MATTERS, AND IT IS ABOUT THE LIST'S
        // LIFETIME. Taking an item off removes its worn marker, and a list left
        // with nothing on it is the engine's to delete. So the list is only
        // ever handed to the unequip while it still carries an enchantment:
        //
        //   to a new roll   craft in place, THEN off and back on (the list has
        //                   the new enchantment throughout)
        //   to plain        off FIRST (the old enchantment keeps the list
        //                   alive), release, and put the BASE back on -- the
        //                   list is not named again after the release.
        if (a_unit.worn && toPlain) {
            Unequip(a_player, a_unit);
        }

        if (a_hadOurs) {
            Apply::Release(a_unit.xList, weapon);
        }

        if (toPlain) {
            if (a_unit.worn) {
                Equip(a_player, a_unit, nullptr);
            }
            outcome.ok = true;
            outcome.message = a_unit.object->GetName() ? a_unit.object->GetName() : "";
            return outcome;
        }

        const auto applied = Apply::ToItem(a_rolled, a_unit.object, a_unit.xList);
        if (!applied) {
            // The old enchantment is already released. Said plainly, in the log
            // and to the player, because the item has lost its affixes.
            logger::error("craft[{:08X}]: the new enchantment could not be built AFTER the old "
                          "one was released; the item is now plain",
                id);
            outcome.message = "The enchantment could not be built; the item is now plain";
            return outcome;
        }

        const auto enchID = applied.enchantment->GetFormID();
        NoteRoll(enchID, a_rolled, a_itemLevel, a_flags);

        if (a_unit.worn) {
            Unequip(a_player, a_unit);
            Equip(a_player, a_unit, a_unit.xList);
        }

        outcome.ok = true;
        outcome.enchantment = enchID;
        outcome.band = static_cast<std::uint8_t>(roll::BandOf(a_rolled.tier));
        outcome.message = applied.name;
        return outcome;
    }

    void Finish(RE::PlayerCharacter* a_player)
    {
        // The wielder ability follows what is in hand, and the grid caches the
        // band per tile: both are told once, after the item has settled.
        Wielder::Reconcile(a_player);
        GridTint::RequestRebuild();
    }

    // ---- what both the orb and the debug edits start from ---------------------

    struct Subject
    {
        RE::PlayerCharacter* player{ nullptr };
        Unit                 unit;
        roll::RolledItem     rolled;      // empty for a plain item
        roll::RollContext    ctx;
        std::uint8_t         flags{ roll::kRecordNone };
        bool                 hadOurs{ false };
    };

    // Fills a_out, or returns the reason the item cannot be crafted.
    const char* Resolve(const Crafting::Target& a_target, Subject& a_out)
    {
        if (!Records::Table()) {
            return "The affix table has not loaded";
        }
        a_out.player = RE::PlayerCharacter::GetSingleton();
        if (!a_out.player) {
            return "No player";
        }
        a_out.unit = FindUnit(a_out.player, a_target);
        if (!a_out.unit.found || !a_out.unit.object) {
            return "That item is no longer in your inventory";
        }

        const char* reason = nullptr;
        if (!Crafting::Craftable(a_out.unit.object, reason)) {
            return reason;
        }
        if (a_out.unit.xList && a_out.unit.xList->HasType(RE::ExtraDataType::kAliasInstanceArray)) {
            return "Quest items can't be crafted";
        }

        a_out.ctx.itemSlots = Apply::SlotsOf(a_out.unit.object);
        a_out.ctx.forNpc = false;

        if (a_target.enchantment != 0) {
            Records::View view;
            if (!Records::Lookup(a_out.unit.xList, view) || !view.complete) {
                return "This item's affixes can't be read, so it can't be crafted";
            }
            a_out.rolled = view.rolled;
            a_out.ctx.itemLevel = view.record.itemLevel;
            a_out.flags = view.record.flags;
            a_out.hadOurs = true;
            return nullptr;
        }

        // A plain item. Only ones the loot system itself would roll: a unique
        // or an artifact is not in any leveled list, and is left as its author
        // made it.
        if (!Apply::IsGeneric(a_out.unit.object)) {
            return "Unique items can't be crafted";
        }
        if (!Apply::IsEligible(a_out.unit.object, a_out.unit.xList)) {
            return "This item can't carry affixes";
        }
        // Rolled now, by the player: their level is the item's, as it is for
        // anything they take out of a chest.
        a_out.ctx.itemLevel = (std::max)(1, static_cast<int>(a_out.player->GetLevel()));
        return nullptr;
    }

    void Recount(roll::RolledItem& a_item)
    {
        a_item.tier = 0;
        for (const auto& rolled : a_item.affixes) {
            a_item.tier += rolled.points;
        }
    }

    Crafting::Outcome Refused(const char* a_reason)
    {
        Crafting::Outcome outcome;
        outcome.message = a_reason ? a_reason : "";
        return outcome;
    }
}

bool Crafting::Craftable(RE::TESBoundObject* a_object, const char*& a_reason)
{
    a_reason = nullptr;
    if (!a_object || (!a_object->Is(RE::FormType::Weapon) && !a_object->Is(RE::FormType::Armor))) {
        a_reason = "Only weapons, armour and jewellery can be crafted";
        return false;
    }
    if (HasOwnEnchantment(a_object)) {
        a_reason = "Items with an enchantment of their own can't be crafted";
        return false;
    }
    if (Apply::SlotsOf(a_object) == roll::kNone) {
        a_reason = "This item can't carry affixes";
        return false;
    }
    return true;
}

Crafting::Outcome Crafting::ApplyOrb(const Target& a_target, roll::Orb a_orb)
{
    Subject     subject;
    const char* reason = Resolve(a_target, subject);
    if (reason) {
        logger::info("craft[{:08X}]: {} refused -- {}", a_target.base, roll::OrbName(a_orb), reason);
        return Refused(reason);
    }

    // The roller's shipped curve: what every roll site is constructed with.
    const roll::Tuning tuning;
    auto               rolled = subject.rolled;
    roll::CraftEffect  effect = roll::CraftEffect::kNone;
    const auto verdict = roll::Craft(a_orb, rolled, subject.ctx, *Records::Table(), tuning, Rng(),
        &effect);
    if (verdict != roll::CraftVerdict::kOk) {
        logger::info("craft[{:08X}]: {} refused -- {}", a_target.base, roll::OrbName(a_orb),
            roll::Reason(verdict));
        return Refused(roll::Reason(verdict));
    }

    if (effect == roll::CraftEffect::kDestroyed) {
        // ★A VAAL ORB'S WORST OUTCOME: the item is gone. Taken off first if
        // worn, removed with its own list, and its enchantment released (the
        // release keeps it while anything else still holds it).
        auto* object = subject.unit.object;
        auto* xList = subject.unit.xList;
        RE::EnchantmentItem* ench = nullptr;
        if (xList) {
            if (const auto* xEnch = xList->GetByType<RE::ExtraEnchantment>()) {
                ench = xEnch->enchantment;
            }
        }
        if (subject.unit.worn) {
            Unequip(subject.player, subject.unit);
        }
        subject.player->RemoveItem(object, 1, RE::ITEM_REMOVE_REASON::kRemove, xList, nullptr);
        if (ench && subject.hadOurs) {
            Apply::ReleaseCreated(ench, IsWeapon(object));
        }
        Finish(subject.player);
        logger::info("craft[{:08X}]: {} DESTROYED the item", a_target.base, roll::OrbName(a_orb));
        Crafting::Outcome gone;
        gone.ok = true;
        gone.message = "The Vaal Orb destroyed the item";
        return gone;
    }

    auto outcome = Rebuild(subject.player, subject.unit, subject.hadOurs, rolled,
        subject.ctx.itemLevel, static_cast<std::uint8_t>(subject.flags | roll::kRecordCrafted));
    Finish(subject.player);
    if (outcome.ok && effect != roll::CraftEffect::kNone) {
        outcome.message = std::format("{} -- corrupted, {}", outcome.message, roll::EffectName(effect));
    }

    logger::info("craft[{:08X}]: {} at item level {} -- {} -> {} affix(es), {} point(s), {}{}",
        a_target.base, roll::OrbName(a_orb), subject.ctx.itemLevel, subject.rolled.affixes.size(),
        rolled.affixes.size(), rolled.tier, outcome.ok ? "now " : "FAILED: ", outcome.message);
    return outcome;
}

Crafting::Outcome Crafting::RemoveAffix(const Target& a_target, std::string_view a_affixId)
{
    Subject     subject;
    const char* reason = Resolve(a_target, subject);
    if (reason) {
        return Refused(reason);
    }

    auto       rolled = subject.rolled;
    const auto before = rolled.affixes.size();
    std::erase_if(rolled.affixes,
        [&](const roll::RolledAffix& a_have) { return a_have.affix && a_have.affix->id == a_affixId; });
    if (rolled.affixes.size() == before) {
        return Refused("That affix is not on the item");
    }
    Recount(rolled);

    auto outcome = Rebuild(subject.player, subject.unit, subject.hadOurs, rolled,
        subject.ctx.itemLevel, static_cast<std::uint8_t>(subject.flags | roll::kRecordCrafted));
    Finish(subject.player);

    logger::warn("debug edit[{:08X}]: REMOVED affix '{}' at the player's request -- {}{}",
        a_target.base, a_affixId, outcome.ok ? "now " : "FAILED: ", outcome.message);
    return outcome;
}

Crafting::Outcome Crafting::RerollValue(const Target& a_target, std::string_view a_affixId)
{
    Subject     subject;
    const char* reason = Resolve(a_target, subject);
    if (reason) {
        return Refused(reason);
    }

    auto  rolled = subject.rolled;
    float was = 0.0f;
    float now = 0.0f;
    bool  found = false;
    for (auto& have : rolled.affixes) {
        if (!have.affix || have.affix->id != a_affixId) {
            continue;
        }
        const auto* tier = roll::FindTier(*have.affix, have.tier);
        if (!tier) {
            return Refused("That affix's tier is no longer in the table");
        }
        const float lo = (std::min)(tier->minValue, tier->maxValue);
        const float hi = (std::max)(tier->minValue, tier->maxValue);
        was = have.value;
        if (lo != hi) {
            std::uniform_real_distribution<float> dist{ lo, hi };
            have.value = dist(Rng());
        }
        now = have.value;
        found = true;
        break;
    }
    if (!found) {
        return Refused("That affix is not on the item");
    }

    auto outcome = Rebuild(subject.player, subject.unit, subject.hadOurs, rolled,
        subject.ctx.itemLevel, static_cast<std::uint8_t>(subject.flags | roll::kRecordCrafted));
    Finish(subject.player);

    logger::warn("debug edit[{:08X}]: REROLLED the value of '{}' at the player's request, "
                 "{:.2f} -> {:.2f} -- {}{}",
        a_target.base, a_affixId, was, now, outcome.ok ? "now " : "FAILED: ", outcome.message);
    return outcome;
}

Crafting::Outcome Crafting::SpawnAtFeet(RE::FormID a_base, int a_band, int a_itemLevel)
{
    const auto* table = Records::Table();
    auto*       player = RE::PlayerCharacter::GetSingleton();
    auto*       object = RE::TESForm::LookupByID<RE::TESBoundObject>(a_base);
    if (!table || !player) {
        return Refused("Not ready");
    }
    if (!object) {
        return Refused("No item has that FormID");
    }
    const char* reason = nullptr;
    if (!Craftable(object, reason)) {
        return Refused(reason);
    }

    const int  band = std::clamp(a_band, 0, roll::kBandCount - 1);
    const int  level = std::clamp(a_itemLevel, 1, 255);
    const auto wanted = static_cast<roll::Band>(band);

    // Rolled the way a drop is, until one lands in the band asked for. A cap,
    // because some bands cannot be reached at some levels -- red below 35 --
    // and the answer to that should be a sentence, not a hang.
    roll::RolledItem   rolled;
    const roll::Tuning tuning;
    if (band > 0) {
        const roll::RollContext ctx{ level, Apply::SlotsOf(object), false, true };
        bool                    hit = false;
        for (int attempt = 0; attempt < 20000 && !hit; ++attempt) {
            rolled = roll::Roll(*table, ctx, tuning, Rng());
            hit = roll::BandOf(rolled.tier) == wanted;
        }
        if (!hit) {
            return Refused("No roll at that item level reaches that colour for this item");
        }
    }

    // ★ON THE GROUND, NOT INTO THE PACK. A reference the engine has just placed
    // has an ExtraDataList of its own, which is the one thing a new inventory
    // item lacks -- so the enchantment goes on with the ordinary ToItem and
    // there is no drop-and-pickup. It also means the item is picked up like any
    // other drop, which is the path that wants testing.
    const auto placed = player->PlaceObjectAtMe(object, false);
    if (!placed) {
        return Refused("The game would not place the item");
    }

    Outcome outcome;
    outcome.ok = true;
    outcome.message = object->GetName() ? object->GetName() : "";

    if (band > 0) {
        const auto applied = Apply::ToItem(rolled, object, &placed->extraList);
        if (!applied) {
            logger::warn("debug spawn[{:08X}]: placed, but the enchantment could not be built; "
                         "the item on the ground is plain",
                a_base);
            outcome.ok = false;
            outcome.message = "Placed, but the enchantment could not be built; it is plain";
            return outcome;
        }
        const auto enchID = applied.enchantment->GetFormID();
        NoteRoll(enchID, rolled, level, roll::kRecordNone);
        outcome.enchantment = enchID;
        outcome.band = static_cast<std::uint8_t>(band);
        outcome.message = applied.name;
    }

    logger::warn("debug spawn[{:08X}]: PLACED {} at the player's feet at the player's request -- "
                 "{} band, item level {}, {} affix(es)",
        a_base, outcome.message, roll::BandName(wanted), level, rolled.affixes.size());
    return outcome;
}
