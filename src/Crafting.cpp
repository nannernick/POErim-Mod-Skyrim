// =============================================================================
//  Crafting. See Crafting.h.
// =============================================================================

#include "PCH.h"

#include "Crafting.h"

#include "Apply.h"
#include "Config.h"
#include "GridTint.h"
#include "LootApi.h"
#include "Persist.h"
#include "Records.h"
#include "Wielder.h"

#include "roll/Record.h"

#include <algorithm>
#include <format>
#include <mutex>
#include <random>
#include <set>

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
        Persist::NoteEnchTier(a_enchantment, roll::RarityCode(a_rolled));
        Persist::NoteItemRecord(a_enchantment, roll::MakeRecord(a_rolled, a_itemLevel, a_flags));
    }

    // A fresh item's base: its implicit and nothing else, as a roll makes it.
    roll::RolledItem BaseImplicitRoll(const roll::RollContext& a_ctx)
    {
        roll::Tuning tuning;
        for (auto& band : tuning.bands) {
            band.countWeights.fill(0);
            band.countWeights[0] = 1;
        }
        const auto* table = Records::Table();
        return table ? roll::Roll(*table, a_ctx, tuning, Rng()) : roll::RolledItem{};
    }

    // Makes the unit carry a_rolled -- or nothing, when a_rolled is empty.
    // a_hadOurs says whether the unit's list carries one of our enchantments
    // now. a_vanilla says what foreign enchantment it carries instead, which
    // is replaced (2.1.0) rather than refused.
    Crafting::Outcome Rebuild(RE::PlayerCharacter* a_player, const Unit& a_unit, bool a_hadOurs,
        const roll::RolledItem& a_rolled, int a_itemLevel, std::uint8_t a_flags,
        Apply::VanillaKind a_vanilla = Apply::VanillaKind::kNone)
    {
        Crafting::Outcome outcome;
        const bool        weapon = IsWeapon(a_unit.object);
        const bool        toPlain = a_rolled.affixes.empty();
        const auto        id = a_unit.object->GetFormID();

        // ---- an enchanted RECORD: back to its plain template -----------------
        //
        // ExtraEnchantment is inert on a record with its own EITM, so the only
        // way to put an implicit on an "Iron Sword of Frost" is to make it an
        // Iron Sword. The swap is remove-one, add-one of the template, and then
        // the same drop-and-pickup a plain unit goes through. ★A worn unit is
        // refused before this point (the swap would take it off for good).
        if (a_vanilla == Apply::VanillaKind::kRecordEnchanted) {
            auto* plain = Apply::PlainTemplateOf(a_unit.object);
            if (!plain || !Apply::CanDropFrom(a_player)) {
                outcome.message = "This item can't be changed here";
                return outcome;
            }
            logger::info("craft[{:08X}]: replacing its vanilla enchantment -- swapping to the plain "
                         "template {:08X}",
                id, plain->GetFormID());
            a_player->RemoveItem(a_unit.object, 1, RE::ITEM_REMOVE_REASON::kRemove, a_unit.xList, nullptr);
            a_player->AddObjectToContainer(plain, nullptr, 1, nullptr);
            if (toPlain) {
                outcome.ok = true;
                outcome.message = plain->GetName() ? plain->GetName() : "";
                return outcome;
            }
            const auto applied = Apply::ToNewInstance(a_rolled, plain, a_player);
            if (!applied) {
                // The swap happened and the roll did not: the player has the
                // plain template. Said so -- it is not silent loss.
                logger::warn("craft[{:08X}]: swapped to the template, but the implicit could not be "
                             "attached; the item is the plain {:08X}",
                    id, plain->GetFormID());
                outcome.message = "Its enchantment was removed, but the implicit could not be added; "
                                  "use an orb on it again";
                return outcome;
            }
            const auto enchID = applied.enchantment->GetFormID();
            NoteRoll(enchID, a_rolled, a_itemLevel, a_flags);
            outcome.ok = true;
            outcome.enchantment = enchID;
            outcome.code = roll::RarityCode(a_rolled);
            outcome.message = applied.name;
            return outcome;
        }

        // ---- a bare stack unit: the engine has to build the instance --------
        if (!a_unit.xList) {
            if (toPlain) {
                outcome.message = "This item has no mods to remove";
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
            outcome.code = roll::RarityCode(a_rolled);
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
        const bool releasable = a_hadOurs || a_vanilla == Apply::VanillaKind::kInstanceEnchanted;
        if (toPlain) {
            if (a_unit.worn) {
                Unequip(a_player, a_unit);
            }
            if (releasable) {
                Apply::Release(a_unit.xList, weapon);
            }
            if (a_unit.worn) {
                Equip(a_player, a_unit, nullptr);
            }
            outcome.ok = true;
            outcome.message = a_unit.object->GetName() ? a_unit.object->GetName() : "";
            return outcome;
        }

        // ★BUILD FIRST, RELEASE AFTER (bug fix): Apply::Replace leaves the item
        // exactly as it was when the new enchantment cannot be built.
        const auto applied = Apply::Replace(a_rolled, a_unit.object, a_unit.xList);
        if (!applied) {
            logger::error("craft[{:08X}]: the new enchantment could not be built; the item was "
                          "left as it was",
                id);
            outcome.message = "The enchantment could not be built; nothing changed";
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
        outcome.code = roll::RarityCode(a_rolled);
        outcome.message = applied.name;
        return outcome;
    }

    void Finish(RE::PlayerCharacter* a_player)
    {
        // The wielder ability follows what is in hand, and the grid caches the
        // band per tile: both are told once, after the item has settled.
        Wielder::Reconcile(a_player);
        GridTint::RequestRebuild();
        LootApi::Invalidate();
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
        Apply::VanillaKind   vanilla{ Apply::VanillaKind::kNone };
        bool                 granted{ false };   // Resolve gave a 1.2.0 item its implicit
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

        if (a_target.enchantment != 0 && Persist::IsAffixEnch(a_target.enchantment)) {
            Records::View view;
            if (!Records::Lookup(a_out.unit.xList, view) || !view.complete) {
                return "This item's mods can't be read, so it can't be crafted";
            }
            a_out.rolled = view.rolled;
            a_out.ctx.itemLevel = view.record.itemLevel;
            a_out.flags = view.record.flags;
            a_out.hadOurs = true;
            // ★A 1.2.0 ITEM WITH NO IMPLICIT gets its base implicit now, the
            // first time it is touched, and the record says so for good.
            if (a_out.rolled.Implicits() == 0 && (a_out.flags & roll::kRecordImplicitGranted) == 0) {
                const auto base = BaseImplicitRoll(a_out.ctx);
                for (const auto& rolled : base.affixes) {
                    a_out.rolled.affixes.insert(a_out.rolled.affixes.begin(), rolled);
                    a_out.rolled.tier += rolled.points;
                }
                a_out.flags = static_cast<std::uint8_t>(a_out.flags | roll::kRecordImplicitGranted);
                a_out.granted = !base.affixes.empty();
            }
            return nullptr;
        }

        // A plain item, or one with somebody else's enchantment. Only ones the
        // loot system itself would roll: a unique or an artifact is in no
        // leveled list and no outfit, and is left as its author made it.
        a_out.vanilla = Apply::ClassifyEnchanted(a_out.unit.object, a_out.unit.xList);
        if (a_out.vanilla == Apply::VanillaKind::kRefused) {
            return Apply::IsGeneric(a_out.unit.object) ? "Items with an enchantment of their own can't be crafted"
                                                      : "Unique items can't be crafted";
        }
        if (a_out.vanilla != Apply::VanillaKind::kNone) {
            if (!Config::ReplaceVanillaEnchants()) {
                return "Items with an enchantment of their own can't be crafted";
            }
            if (a_out.unit.worn && a_out.vanilla == Apply::VanillaKind::kRecordEnchanted) {
                return "Unequip it first";
            }
        } else {
            if (!Apply::IsGeneric(a_out.unit.object)) {
                return "Unique items can't be crafted";
            }
            if (!Apply::IsEligible(a_out.unit.object, a_out.unit.xList)) {
                return "This item can't carry mods";
            }
        }
        // Rolled now, by the player: their level is the item's, as it is for
        // anything they take out of a chest. It gets its base implicit first,
        // as every item now does from creation.
        a_out.ctx.itemLevel = (std::max)(1, static_cast<int>(a_out.player->GetLevel()));
        a_out.rolled = BaseImplicitRoll(a_out.ctx);
        a_out.flags = roll::kRecordImplicitGranted;
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
    // ★AN ENCHANTED RECORD IS CRAFTABLE WHEN ITS ENCHANTMENT CAN BE REPLACED
    // (2.1.0): a generic variant with a plain template, and the setting on.
    if (HasOwnEnchantment(a_object) &&
        (!Config::ReplaceVanillaEnchants() ||
            Apply::ClassifyEnchanted(a_object, nullptr) != Apply::VanillaKind::kRecordEnchanted)) {
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
    const Records::ReadGuard tableGuard{ Records::TableLock() };
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
        subject.ctx.itemLevel, static_cast<std::uint8_t>(subject.flags | roll::kRecordCrafted),
        subject.vanilla);
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
    const Records::ReadGuard tableGuard{ Records::TableLock() };
    Subject     subject;
    const char* reason = Resolve(a_target, subject);
    if (reason) {
        return Refused(reason);
    }

    if (subject.rolled.corrupted) {
        return Refused("Corrupted");
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
        subject.ctx.itemLevel, static_cast<std::uint8_t>(subject.flags | roll::kRecordCrafted),
        subject.vanilla);
    Finish(subject.player);

    logger::warn("debug edit[{:08X}]: REMOVED affix '{}' at the player's request -- {}{}",
        a_target.base, a_affixId, outcome.ok ? "now " : "FAILED: ", outcome.message);
    return outcome;
}

Crafting::Outcome Crafting::RerollValue(const Target& a_target, std::string_view a_affixId)
{
    const Records::ReadGuard tableGuard{ Records::TableLock() };
    Subject     subject;
    const char* reason = Resolve(a_target, subject);
    if (reason) {
        return Refused(reason);
    }

    if (subject.rolled.corrupted) {
        return Refused("Corrupted");
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
        subject.ctx.itemLevel, static_cast<std::uint8_t>(subject.flags | roll::kRecordCrafted),
        subject.vanilla);
    Finish(subject.player);

    logger::warn("debug edit[{:08X}]: REROLLED the value of '{}' at the player's request, "
                 "{:.2f} -> {:.2f} -- {}{}",
        a_target.base, a_affixId, was, now, outcome.ok ? "now " : "FAILED: ", outcome.message);
    return outcome;
}

Crafting::Outcome Crafting::SpawnAtFeet(RE::FormID a_base, int a_rarity, int a_itemLevel)
{
    const Records::ReadGuard tableGuard{ Records::TableLock() };
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
    if (!Craftable(object, reason) || HasOwnEnchantment(object)) {
        return Refused(reason ? reason : "Pick an unenchanted base");
    }

    // 0 normal, 1 magic, 2 rare -- rolled the way a drop is, until it lands.
    const int  rarity = std::clamp(a_rarity, 0, 2);
    const int  level = std::clamp(a_itemLevel, 1, 255);
    const auto wanted = static_cast<roll::Rarity>(rarity + 1);

    roll::RolledItem        rolled;
    const roll::Tuning      tuning;
    const roll::RollContext ctx{ level, Apply::SlotsOf(object), false };
    bool                    hit = false;
    for (int attempt = 0; attempt < 20000 && !hit; ++attempt) {
        rolled = roll::Roll(*table, ctx, tuning, Rng());
        hit = roll::RarityOf(rolled) == wanted;
    }
    if (!hit) {
        return Refused("No roll at that item level reaches that rarity for this item");
    }

    // ★ON THE GROUND, NOT INTO THE PACK. A reference the engine has just placed
    // has an ExtraDataList of its own, which is the one thing a new inventory
    // item lacks -- so the enchantment goes on with the ordinary ToItem and
    // there is no drop-and-pickup.
    const auto placed = player->PlaceObjectAtMe(object, false);
    if (!placed) {
        return Refused("The game would not place the item");
    }

    Outcome outcome;
    outcome.ok = true;
    outcome.message = object->GetName() ? object->GetName() : "";

    if (!rolled.affixes.empty()) {
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
        outcome.code = roll::RarityCode(rolled);
        outcome.message = applied.name;
    }

    logger::warn("debug spawn[{:08X}]: PLACED {} at the player's feet at the player's request -- "
                 "{}, item level {}, {} mod(s) + {} implicit(s)",
        a_base, outcome.message, roll::RarityName(wanted), level, rolled.Explicits(), rolled.Implicits());
    return outcome;
}

Crafting::Outcome Crafting::GrantBaseImplicit(const Target& a_target)
{
    const Records::ReadGuard tableGuard{ Records::TableLock() };
    Subject     subject;
    const char* reason = Resolve(a_target, subject);
    if (reason) {
        return Refused(reason);
    }
    // Resolve already put the base implicit on a plain or replaced item, and
    // granted one to an old record without. Nothing to do otherwise.
    const bool plainOrForeign = !subject.hadOurs;
    const bool legacyGrant = subject.hadOurs && subject.granted;
    if (!plainOrForeign && !legacyGrant) {
        return Refused("It already has its implicit");
    }
    if (subject.rolled.Implicits() == 0 && subject.vanilla == Apply::VanillaKind::kNone) {
        return Refused("No implicit fits this kind of item");
    }
    auto outcome = Rebuild(subject.player, subject.unit, subject.hadOurs, subject.rolled,
        subject.ctx.itemLevel, subject.flags, subject.vanilla);
    Finish(subject.player);
    logger::info("implicit[{:08X}]: base implicit granted on first sight -- {}{}", a_target.base,
        outcome.ok ? "now " : "FAILED: ", outcome.message);
    return outcome;
}

Crafting::Outcome Crafting::AddSecondImplicit(const Target& a_target)
{
    const Records::ReadGuard tableGuard{ Records::TableLock() };
    Subject     subject;
    const char* reason = Resolve(a_target, subject);
    if (reason) {
        return Refused(reason);
    }
    if (subject.rolled.corrupted) {
        return Refused("Corrupted");
    }
    if (subject.rolled.Implicits() != 1) {
        return Refused("Needs exactly one implicit");
    }
    auto pool = roll::ImplicitPool(*Records::Table(), subject.ctx);
    roll::FitImplicitPool(pool, subject.rolled);
    roll::RolledAffix added;
    const roll::Tuning tuning;
    bool               drawn = false;
    while (!pool.empty() && !drawn) {
        const auto result = roll::DrawAffix(pool, subject.ctx,
            roll::TierWeightsFor(tuning, subject.ctx.itemLevel), Rng(), added);
        if (result == roll::Draw::kExhausted) {
            break;
        }
        drawn = result == roll::Draw::kDrawn;
    }
    if (!drawn) {
        // ★The same rule as the Vaal outcome: no valid second implicit is a
        // no-op, never an error.
        return Refused("No valid second implicit for this item (no-op)");
    }
    auto rolled = subject.rolled;
    rolled.affixes.push_back(added);
    rolled.tier += added.points;
    rolled.corrupted = true;   // as the Vaal outcome would leave it
    auto outcome = Rebuild(subject.player, subject.unit, subject.hadOurs, rolled,
        subject.ctx.itemLevel, static_cast<std::uint8_t>(subject.flags | roll::kRecordCrafted),
        subject.vanilla);
    Finish(subject.player);
    logger::warn("debug edit[{:08X}]: VAAL-ADDED implicit '{}' at the player's request -- {}{}",
        a_target.base, added.affix ? added.affix->id : "?", outcome.ok ? "now " : "FAILED: ", outcome.message);
    return outcome;
}

Crafting::Outcome Crafting::SetCorrupted(const Target& a_target, bool a_corrupted)
{
    const Records::ReadGuard tableGuard{ Records::TableLock() };
    Subject     subject;
    const char* reason = Resolve(a_target, subject);
    if (reason) {
        return Refused(reason);
    }
    if (!subject.hadOurs) {
        return Refused("Only an item with mods or an implicit can be marked");
    }
    // ★THE RECORD ONLY: corruption lives in the co-save flags, not in the
    // enchantment, so nothing is rebuilt.
    roll::ItemRecord record;
    if (!Persist::GetItemRecord(a_target.enchantment, record)) {
        return Refused("No record for this item");
    }
    record.flags = a_corrupted ? static_cast<std::uint8_t>(record.flags | roll::kRecordCorrupted)
                               : static_cast<std::uint8_t>(record.flags & ~roll::kRecordCorrupted);
    Persist::NoteItemRecord(a_target.enchantment, record);
    auto rolled = subject.rolled;
    rolled.corrupted = a_corrupted;
    Persist::NoteEnchTier(a_target.enchantment, roll::RarityCode(rolled));
    Finish(subject.player);
    logger::warn("debug edit[{:08X}]: corrupted flag {} at the player's request", a_target.base,
        a_corrupted ? "SET" : "CLEARED");
    Outcome outcome;
    outcome.ok = true;
    outcome.enchantment = a_target.enchantment;
    outcome.code = roll::RarityCode(rolled);
    outcome.message = a_corrupted ? "Corrupted" : "No longer corrupted";
    return outcome;
}
