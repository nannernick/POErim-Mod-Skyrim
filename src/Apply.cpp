// =============================================================================
//  Phase 4 -- turning a roll into a live item. See Apply.h.
// =============================================================================

#include "PCH.h"

#include "Apply.h"

#include "Config.h"
#include "Orbs.h"
#include "Persist.h"

#include <algorithm>
#include <atomic>
#include <format>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

namespace
{
    // affixId -> the magic effect that implements it. Built once at kDataLoaded.
    std::unordered_map<std::string, RE::EffectSetting*> g_effects;

    // The same effects as a set, for the one question that asks about an effect
    // rather than an affix: "could this enchantment have come from our table?"
    // Kept alongside rather than derived on demand -- it is asked once per
    // enchantment on the player's gear every time an enchanting table opens.
    std::unordered_set<const RE::EffectSetting*> g_effectSet;

    // ★2.1.0: the two maps are rebuilt by a table RELOAD while the game runs,
    // and read from the render thread (tooltips) and from equip events, not
    // all of which hold the table lock. A lock of their own.
    std::shared_mutex g_effectsLock;

    // "Skyrim.esm|0x07A0FF" -> the live form.
    //
    // ★The token is a plugin name and a LOCAL id, never a raw FormID: a raw one
    // carries the plugin's load-order position in its high byte, so it means
    // something different on every machine. LookupForm reapplies the local
    // index for whatever position the plugin occupies here.
    RE::EffectSetting* ResolveToken(std::string_view a_token)
    {
        const auto bar = a_token.find('|');
        if (bar == std::string_view::npos) {
            return nullptr;
        }

        const auto plugin = a_token.substr(0, bar);
        auto       idText = a_token.substr(bar + 1);
        if (idText.rfind("0x", 0) == 0 || idText.rfind("0X", 0) == 0) {
            idText.remove_prefix(2);
        }

        RE::FormID localID = 0;
        for (const char ch : idText) {
            const int digit = std::isxdigit(static_cast<unsigned char>(ch))
                ? (std::isdigit(static_cast<unsigned char>(ch)) ? ch - '0'
                                                                : (std::tolower(ch) - 'a' + 10))
                : -1;
            if (digit < 0) {
                return nullptr;
            }
            localID = (localID << 4) | static_cast<RE::FormID>(digit);
        }

        auto* handler = RE::TESDataHandler::GetSingleton();
        return handler ? handler->LookupForm<RE::EffectSetting>(localID, plugin) : nullptr;
    }

    bool IsWeapon(RE::TESBoundObject* a_object)
    {
        return a_object && a_object->Is(RE::FormType::Weapon);
    }

    RE::TESEnchantableForm* AsEnchantable(RE::TESBoundObject* a_object)
    {
        if (!a_object) {
            return nullptr;
        }
        if (auto* weapon = a_object->As<RE::TESObjectWEAP>()) {
            return static_cast<RE::TESEnchantableForm*>(weapon);
        }
        if (auto* armor = a_object->As<RE::TESObjectARMO>()) {
            return static_cast<RE::TESEnchantableForm*>(armor);
        }
        return nullptr;
    }

    // Every weapon and armour record reachable from a leveled item list. See the
    // note in Apply.h: this set IS the "non-unique" test.
    std::unordered_set<RE::FormID> g_generic;

    // Walks one leveled list, following nested lists.
    //
    // ★A VISITED SET, NOT A DEPTH LIMIT. Leveled lists nest freely and nothing
    // stops a modded one referring to an ancestor -- rare, but a cycle here is
    // an infinite recursion during load, which presents as "the game hangs on
    // startup with this mod installed" and gives no clue why.
    void CollectLeveled(RE::TESLeveledList* a_list, std::unordered_set<RE::FormID>& a_seen)
    {
        if (!a_list) {
            return;
        }
        for (std::uint32_t i = 0; i < a_list->numEntries; ++i) {
            auto* form = a_list->entries[i].form;
            if (!form) {
                continue;
            }
            if (auto* nested = form->As<RE::TESLevItem>()) {
                if (a_seen.insert(nested->GetFormID()).second) {
                    CollectLeveled(static_cast<RE::TESLeveledList*>(nested), a_seen);
                }
                continue;
            }
            if (form->Is(RE::FormType::Weapon) || form->Is(RE::FormType::Armor)) {
                g_generic.insert(form->GetFormID());
            }
        }
    }

    // The half of applying a roll that has nothing to do with where the result
    // is going to be hung: the created ENCH, with its never-drain signature.
    //
    // The affix points of a roll that actually FIRE ON HIT. A wielder-side
    // affix -- a weapon skill, a spell school's cost -- is delivered to the
    // wielder by Wielder.cpp, not by the weapon, so it is not part of what a
    // swing spends.
    int OnHitPoints(const roll::RolledItem& a_rolled)
    {
        int points = 0;
        for (const auto& rolled : a_rolled.affixes) {
            if (!rolled.affix) {
                continue;
            }
            const auto* effect = Apply::EffectFor(rolled.affix->id);
            const bool  passive = effect && Apply::IsWielderEffect(effect);
            if (!passive) {
                points += std::max(rolled.points, 0);
            }
        }
        return points;
    }

    // What one hit costs this roll, in charge. Zero -- the default, and the
    // only answer for armour -- is "never drain".
    //
    // ★ONLY WHAT FIRES IS PAID FOR. A sword whose only affix is One-Handed
    // has nothing to discharge on a hit -- the bonus lives on the wielder --
    // so it carries no charge at all; and a sword with One-Handed beside Fire
    // Damage pays for the fire alone.
    std::int32_t PerHitCost(const roll::RolledItem& a_rolled, RE::TESBoundObject* a_object)
    {
        if (!Config::WeaponChargeEnabled() || !IsWeapon(a_object)) {
            return 0;
        }
        const int onHit = OnHitPoints(a_rolled);
        if (onHit <= 0) {
            return 0;
        }
        // A red rolled at fifteen points drains faster than a one-point blue,
        // the way a strong vanilla enchantment costs more per hit than a weak
        // one. Floored at one: a cost of zero is the never-drain signature,
        // and the two must not be confusable through a zero-cost INI line.
        const auto cost = Config::WeaponChargeCost() +
                          Config::WeaponChargeCostPerPoint() * onHit;
        return static_cast<std::int32_t>(std::max(cost, 1));
    }

    // The maximum charge the instance starts with, by the band it rolled.
    // Paired with PerHitCost: a charge with no cost is a meter that never
    // moves, and a cost with no charge is a weapon that fires from empty --
    // both look like bugs.
    std::uint16_t StartingCharge(const roll::RolledItem& a_rolled, RE::TESBoundObject* a_object)
    {
        if (PerHitCost(a_rolled, a_object) <= 0) {
            return 0;
        }
        return Config::WeaponChargeAmount(roll::PowerStepOf(a_rolled.tier));
    }

    // Split out because there are now TWO places to hang it -- an ExtraDataList
    // we were handed, and one the engine is about to make for us -- and the
    // rules about what may be enchanted at all must not be allowed to drift
    // between them. Returns nullptr for every legitimate decline as well as for
    // failure; the caller has no reason to tell those apart.
    RE::EnchantmentItem* BuildEnchantment(const roll::RolledItem& a_rolled,
        RE::TESBoundObject* a_object)
    {
        if (a_rolled.affixes.empty() || !a_object) {
            return nullptr;  // a tier-0 white item is a legitimate roll, not a failure
        }

        auto* manager = RE::BGSCreatedObjectManager::GetSingleton();
        if (!manager) {
            logger::error("apply: no BGSCreatedObjectManager");
            return nullptr;
        }

        // ★VANILLA-ENCHANTED ITEMS ARE LEFT ALONE. A settled decision, not a gap.
        //
        // ExtraEnchantment is inert whenever the record carries an EITM --
        // measured, in combat as well as on the item card -- so the only way to
        // add to an enchanted item is to stop it being one: remove the enchanted
        // record from the container, add its plain template, re-equip. That is
        // live surgery on an NPC mid-load, and the payoff is decoration on items
        // that already carry an identity of their own.
        //
        // An Iron Sword of Sparks stays an Iron Sword of Sparks. Everything
        // unenchanted carries the affix system.
        if (auto* enchantable = AsEnchantable(a_object); enchantable && enchantable->formEnchanting) {
            return nullptr;
        }

        RE::BSTArray<RE::Effect> effects;
        effects.reserve(static_cast<std::uint32_t>(a_rolled.affixes.size() + 4));

        for (const auto& rolled : a_rolled.affixes) {
            if (!rolled.affix) {
                continue;
            }
            auto* base = Apply::EffectFor(rolled.affix->id);
            if (!base) {
                continue;  // unresolvable token, already reported at load
            }

            auto& effect = effects.emplace_back();
            effect.baseEffect = base;
            effect.effectItem.magnitude = rolled.value;
            // ★Duration and area come from the TABLE, not from zero. Phase 0
            // built every effect at duration 0 and Turn Undead rendered as "flee
            // for 0 seconds" -- a whole class of affixes doing nothing, silently.
            effect.effectItem.duration = rolled.duration;
            effect.effectItem.area = rolled.area;
            effect.cost = 0.0f;
        }

        if (effects.empty()) {
            return nullptr;
        }

        auto* ench = IsWeapon(a_object) ? manager->AddWeaponEnchantment(effects)
                                        : manager->AddArmorEnchantment(effects);
        if (!ench) {
            logger::error("apply: the manager refused to create an enchantment for {} effect(s)",
                effects.size());
            return nullptr;
        }

        // The per-hit cost. The number is what the engine reads and the flag is
        // what tells it to read the number instead of auto-calculating from the
        // effects -- and auto-calculation is not an option here even when the
        // charge is meant to be finite, because the table rolls magnitudes far
        // outside anything the enchanter's own formula was tuned for.
        //
        // Zero means never drain: charge is irrelevant to such an enchantment,
        // it fires from empty, and that is what every affix has always been.
        // Nonzero is the opt-in finite charge, weapons only -- armour
        // enchantments are constant-effect and have no charge to spend.
        //
        // ★THE ENGINE DEDUPES IDENTICAL EFFECT SETS, so `ench` can be one an
        // earlier roll already built -- and it keeps whatever cost that roll
        // stamped. Two identical rolls with the setting flipped between them
        // share one enchantment and one cost; the second roll's setting loses.
        // Rare enough not to fight, and the alternative is a second enchantment
        // for the same effects, which the manager exists to prevent.
        ench->data.costOverride = PerHitCost(a_rolled, a_object);
        ench->data.flags.set(RE::EnchantmentItem::EnchantmentFlag::kCostOverride);

        return ench;
    }

    // Whether the manager still lists this enchantment after a release: false
    // means the last holder let go and the form is gone. Only the POINTER is
    // compared, never dereferenced -- when this returns false it is dangling.
    //
    // ★WHY THIS IS ASKED AT ALL. The manager dedupes identical effect sets, so
    // one created enchantment can be carried by several items at once. Every
    // holder that releases used to drop the map entries unconditionally, and
    // the siblings kept an enchantment that the records no longer knew: the
    // grid drew them uncoloured, and now that the enchanting table reads the
    // affix set, they would stop being enchantable as well.
    bool StillCreated(const RE::BGSCreatedObjectManager* a_manager,
        const RE::EnchantmentItem* a_ench, bool a_isWeapon)
    {
        if (!a_manager) {
            return false;
        }
        const auto& list = a_isWeapon ? a_manager->weaponEnchantments
                                      : a_manager->armorEnchantments;
        RE::BSSpinLockGuard guard{ a_manager->lock };
        for (const auto& entry : list) {
            if (entry.magicItem == a_ench) {
                return true;
            }
        }
        return false;
    }

    // ★A DIAGNOSTIC USED TO LIVE HERE AND IT CRASHED THE GAME. Removed, not
    // fixed, and the reason is worth keeping.
    //
    // It walked the player's entryList to report the entry's count and every
    // extra list on it, so that a surprise would arrive with the before-and-after
    // already in the log. The walk faulted on a freed InventoryEntryData -- a
    // node filled with 0xCC, read as `entry->object != a_object` -- because it
    // ran on the frame the reward was being handed over, while the engine was
    // still mutating the list it was reading.
    //
    // The lesson is not "that diagnostic was buggy". It is that the player's
    // inventory is NOT safe to walk from a task fired by a container-changed
    // event: the event announces a mutation that is still in progress. Anything
    // that reads that list has to wait until the engine is done with it.

    // The rolled name, written onto the instance only when it differs.
    //
    // ★Only override when the name actually CHANGES. With the prefix/suffix
    // columns still empty, every roll composes back to the base name, and
    // stamping an ExtraTextDisplayData identical to the real name is pure cost:
    // it claims a per-instance slot, and it is one more thing for the socket mod
    // and the item card to reconcile for no visible benefit.
    std::string NameInstance(const roll::RolledItem& a_rolled, RE::TESBoundObject* a_object,
        RE::ExtraDataList* a_xList)
    {
        const char*       baseName = a_object->GetName();
        const std::string plain = (baseName && *baseName) ? baseName : "Item";
        const std::string name = a_rolled.Name(plain);

        if (a_xList && !name.empty() && name != plain) {
            a_xList->SetOverrideName(name.c_str());
        }
        return name;
    }
}

std::size_t Apply::IndexLeveledItems()
{
    g_generic.clear();

    auto* handler = RE::TESDataHandler::GetSingleton();
    if (!handler) {
        logger::error("leveled index: no data handler; every item will read as unique "
                      "and quest rewards will roll nothing");
        return 0;
    }

    std::unordered_set<RE::FormID> seen;
    std::size_t                    lists = 0;

    for (auto* list : handler->GetFormArray<RE::TESLevItem>()) {
        if (!list) {
            continue;
        }
        ++lists;
        seen.insert(list->GetFormID());
        CollectLeveled(static_cast<RE::TESLeveledList*>(list), seen);
    }

    // ★OUTFITS TOO (2.1.0). A guard's helmet, a soldier's cuirass: gear an
    // NPC is DRESSED in and that no leveled list hands out. It is as generic
    // as an Iron Sword -- every guard wears one -- and treating it as a unique
    // left it uncraftable the moment it was plain (after Scouring, or white).
    std::size_t outfits = 0;
    for (auto* outfit : handler->GetFormArray<RE::BGSOutfit>()) {
        if (!outfit) {
            continue;
        }
        ++outfits;
        for (auto* form : outfit->outfitItems) {
            if (!form) {
                continue;
            }
            if (auto* nested = form->As<RE::TESLevItem>()) {
                if (seen.insert(nested->GetFormID()).second) {
                    CollectLeveled(static_cast<RE::TESLeveledList*>(nested), seen);
                }
            } else if (form->Is(RE::FormType::Weapon) || form->Is(RE::FormType::Armor)) {
                g_generic.insert(form->GetFormID());
            }
        }
    }

    logger::info("leveled index: {} weapon/armour record(s) reachable from {} list(s) and {} outfit(s)",
        g_generic.size(), lists, outfits);
    if (g_generic.empty() && lists) {
        // Lists exist but nothing came out of them, which cannot be true of a
        // real load order -- so the walk is wrong, not the data.
        logger::error("  ...and that is zero, which means the walk is broken, not your "
                      "load order; nothing will be treated as generic");
    }
    return g_generic.size();
}

bool Apply::IsGeneric(const RE::TESBoundObject* a_object)
{
    return a_object && g_generic.contains(a_object->GetFormID());
}

std::size_t Apply::GenericCount()
{
    return g_generic.size();
}

namespace
{
    // The weapon types an effect's skill governs, or kNone when the effect is
    // not a weapon-skill fortify at all.
    //
    // ★STAVES COUNT AS ONE-HANDED. No skill governs a staff, but it is held
    // in one hand the way a sword is, and the decision was that a One-Handed
    // bonus belongs on it. Two-Handed and Archery stay on their own weapons.
    //
    // Keyed on the ACTOR VALUE and not the affix id, so a Summermyst row or a
    // renamed base row that reaches the same effect is narrowed the same way.
    // The three modifier values per skill are the ones the survey found
    // vanilla actually uses -- Fortify One-Handed keys off the power modifier,
    // not the bare skill -- and the bare value is kept so a mod that does it
    // the direct way is not missed.
    std::uint32_t WeaponTypeOfSkill(const RE::EffectSetting* a_effect)
    {
        using AV = RE::ActorValue;
        switch (a_effect->data.primaryAV) {
        case AV::kOneHanded:
        case AV::kOneHandedModifier:
        case AV::kOneHandedPowerModifier:
            return roll::kOneHanded | roll::kStaff;
        case AV::kTwoHanded:
        case AV::kTwoHandedModifier:
        case AV::kTwoHandedPowerModifier:
            return roll::kTwoHanded;
        case AV::kArchery:
        case AV::kMarksmanModifier:
        case AV::kMarksmanPowerModifier:
            return roll::kBow;
        default:
            return roll::kNone;
        }
    }

    // A Fortify <School> -- "spells of this school cost less". Wielder-side
    // like a weapon skill, and unlike one it names no weapon type: the CSV
    // decides where it rolls, and the slots are left as written.
    bool IsSchoolCostEffect(const RE::EffectSetting* a_effect)
    {
        using AV = RE::ActorValue;
        switch (a_effect->data.primaryAV) {
        case AV::kAlterationModifier:
        case AV::kAlterationPowerModifier:
        case AV::kConjurationModifier:
        case AV::kConjurationPowerModifier:
        case AV::kDestructionModifier:
        case AV::kDestructionPowerModifier:
        case AV::kIllusionModifier:
        case AV::kIllusionPowerModifier:
        case AV::kRestorationModifier:
        case AV::kRestorationPowerModifier:
            return true;
        default:
            return false;
        }
    }

    // ★A WEAPON-SKILL AFFIX ONLY ROLLS ON THE WEAPON ITS SKILL GOVERNS.
    //
    // The CSV can say it directly -- ONEHANDED instead of WEAPON -- but the
    // guarantee cannot rest on every future edit of every table remembering
    // to. So a row that lists the generic WEAPON bit and resolves to a
    // Fortify One-Handed / Two-Handed / Archery effect has that bit swapped
    // for the typed one here, where the effect is known. A Two-Handed row
    // that already says TWOHANDED is left alone; a Two-Handed row that
    // somehow says ONEHANDED is corrected and logged, because a greatsword
    // carrying a One-Handed bonus is exactly the nonsense this exists to
    // prevent. Apparel bits are untouched: skill fortifies belong on rings
    // and armour, and this is about weapons only.
    void NarrowWeaponSlots(roll::Affix& a_affix, const RE::EffectSetting* a_effect)
    {
        const auto typed = WeaponTypeOfSkill(a_effect);
        if (typed == roll::kNone) {
            return;
        }
        const auto before = a_affix.slots;
        const bool onWeapons = (before & (roll::kWeapon | roll::kWeaponTypes)) != 0;
        if (!onWeapons) {
            return;
        }
        auto after = before & ~(roll::kWeapon | roll::kWeaponTypes);
        after |= typed;
        if (after != before) {
            logger::info("affix \x27{}\x27 fortifies a weapon skill: weapon slots {} -> {}",
                a_affix.id, roll::SlotsToString(before), roll::SlotsToString(after));
            a_affix.slots = after;
        }
    }
}

std::size_t Apply::ResolveEffects(roll::AffixTable& a_table)
{
    std::unique_lock effectsLock{ g_effectsLock };
    g_effects.clear();
    g_effectSet.clear();

    std::size_t resolved = 0;
    std::size_t missing = 0;
    std::size_t invisible = 0;

    for (auto& affix : a_table.Affixes()) {
        if (affix.mgef.empty()) {
            logger::warn("affix '{}' has no mgef token; it can never be applied", affix.id);
            ++missing;
            continue;
        }

        auto* effect = ResolveToken(affix.mgef);
        if (!effect) {
            // Loud, and specific about the consequence. A token that fails here
            // is usually a plugin the user does not have, and the honest result
            // is one dead affix rather than a broken mod.
            logger::error("affix '{}': cannot resolve mgef '{}' -- is that plugin installed? "
                          "this affix will never roll",
                affix.id, affix.mgef);
            ++missing;
            continue;
        }

        // ★AN EFFECT THAT WILL NOT DISPLAY IS A HALF-BROKEN AFFIX.
        //
        // Grid Inventory (and vanilla) print nothing for an effect flagged
        // kHideInUI -- enchantments carry helper effects not meant to be read --
        // and nothing for one whose description is empty. The affix still WORKS;
        // it is simply invisible, so the player gets a renamed item with no
        // stated reason for the name. Reported as "Bow of Turning with no
        // enchantment effect on it", which is exactly what it looks like.
        //
        // Nothing in the mgef selection ever tested for this: the survey ranked
        // by how many enchantments used an effect and where it came from, and
        // both artifact-flavoured picks it made turn out to be the hidden kind.
        const bool hidden =
            effect->data.flags.all(RE::EffectSetting::EffectSettingData::Flag::kHideInUI);
        const char* desc = effect->magicItemDescription.c_str();
        if (hidden || !desc || !*desc) {
            // ★PRINT THE RESOLVED ID, NOT JUST THE NAME. A name alone cannot be
            // checked against the token that produced it, and when the two
            // disagreed -- the table naming one effect and this warning another
            // -- there was no way to tell from the log which of them was wrong.
            // The id and the token side by side make that a one-line diagnosis.
            logger::warn("affix \x27{}\x27 -> [{:08X}] {} will NOT show on the item card ({}); "
                         "the affix works but reads as an unexplained rename "
                         "(token \x27{}\x27)",
                affix.id, effect->GetFormID(), effect->GetName(),
                hidden ? "flagged hide-in-UI" : "no description text", affix.mgef);
            ++invisible;
        }

        NarrowWeaponSlots(affix, effect);

        g_effects[affix.id] = effect;
        g_effectSet.insert(effect);
        ++resolved;
    }

    logger::info("resolved {} of {} affix effects{}", resolved, a_table.Affixes().size(),
        missing ? std::format(" ({} unusable)", missing) : "");
    if (invisible) {
        logger::warn("  {} affix(es) resolve to an effect that draws no item-card line", invisible);
    }
    return resolved;
}

bool Apply::IsAffixEffect(const RE::EffectSetting* a_effect)
{
    std::shared_lock lock{ g_effectsLock };
    return a_effect && g_effectSet.contains(a_effect);
}

RE::EffectSetting* Apply::EffectFor(std::string_view a_affixId)
{
    std::shared_lock lock{ g_effectsLock };
    const auto       it = g_effects.find(std::string{ a_affixId });
    return it == g_effects.end() ? nullptr : it->second;
}

bool Apply::IsWielderEffect(const RE::EffectSetting* a_effect)
{
    return a_effect && (WeaponTypeOfSkill(a_effect) != roll::kNone || IsSchoolCostEffect(a_effect));
}

std::uint32_t Apply::SlotsOf(RE::TESBoundObject* a_object)
{
    if (!a_object) {
        return roll::kNone;
    }

    if (auto* weapon = a_object->As<RE::TESObjectWEAP>()) {
        // The generic bit plus the type, so a Two-Handed row can single out
        // greatswords the way a FEET row singles out boots. Staves have a
        // type of their own; fists get the generic bit alone.
        using Type = RE::WEAPON_TYPE;
        switch (weapon->GetWeaponType()) {
        case Type::kOneHandSword:
        case Type::kOneHandDagger:
        case Type::kOneHandAxe:
        case Type::kOneHandMace:
            return roll::kWeapon | roll::kOneHanded;
        case Type::kTwoHandSword:
        case Type::kTwoHandAxe:
            return roll::kWeapon | roll::kTwoHanded;
        case Type::kBow:
        case Type::kCrossbow:
            return roll::kWeapon | roll::kBow;
        case Type::kStaff:
            return roll::kWeapon | roll::kStaff;
        default:
            return roll::kWeapon;  // fists, and anything a mod invents
        }
    }

    auto* armor = a_object->As<RE::TESObjectARMO>();
    if (!armor) {
        return roll::kNone;
    }

    using Biped = RE::BGSBipedObjectForm::BipedObjectSlot;
    const auto mask = armor->GetSlotMask();

    // Rings and amulets are jewellery, not armour: an affix eligible for "any
    // armor piece" should not land on a ring, and the CSV distinguishes them.
    if (mask.any(Biped::kRing)) {
        return roll::kRing;
    }
    if (mask.any(Biped::kAmulet)) {
        return roll::kAmulet;
    }

    std::uint32_t slots = roll::kArmor;
    if (mask.any(Biped::kShield)) {
        slots |= roll::kShield;
    }
    if (mask.any(Biped::kHead) || mask.any(Biped::kHair) || mask.any(Biped::kCirclet)) {
        slots |= roll::kHead;
    }
    if (mask.any(Biped::kBody)) {
        slots |= roll::kBody;
    }
    if (mask.any(Biped::kHands) || mask.any(Biped::kForearms)) {
        slots |= roll::kHands;
    }
    if (mask.any(Biped::kFeet) || mask.any(Biped::kCalves)) {
        slots |= roll::kFeet;
    }
    return slots;
}

bool Apply::IsEligible(RE::TESBoundObject* a_object, RE::ExtraDataList* a_xList)
{
    if (!a_object) {
        return false;
    }
    if (!IsWeapon(a_object) && !a_object->As<RE::TESObjectARMO>()) {
        return false;
    }

    // ★Quest items are refused outright, and this is the guard the base-swap
    // survey argued for. Swapping the base changes what the item IS; a quest
    // that hands over Shield of Solitude and later checks for that record would
    // break, and the failure would surface as an unfinishable quest hours later.
    if (a_xList) {
        if (a_xList->HasType(RE::ExtraDataType::kAliasInstanceArray)) {
            return false;
        }
        // Something already owns this item's enchantment slot -- the socket mod,
        // or a previous roll. Re-rolling is a separate operation with a release
        // path; it is not this function's job to silently stomp it.
        if (a_xList->HasType<RE::ExtraEnchantment>()) {
            return false;
        }
    }

    return true;
}

RE::TESBoundObject* Apply::PlainTemplateOf(RE::TESBoundObject* a_object)
{
    if (!a_object) {
        return nullptr;
    }
    if (auto* weapon = a_object->As<RE::TESObjectWEAP>()) {
        auto* plain = weapon->templateWeapon;
        return plain && !plain->formEnchanting ? plain : nullptr;
    }
    if (auto* armor = a_object->As<RE::TESObjectARMO>()) {
        auto* plain = armor->templateArmor;
        return plain && !plain->formEnchanting ? plain : nullptr;
    }
    return nullptr;
}

Apply::VanillaKind Apply::ClassifyEnchanted(RE::TESBoundObject* a_object, const RE::ExtraDataList* a_xList)
{
    if (!a_object || (!IsWeapon(a_object) && !a_object->As<RE::TESObjectARMO>())) {
        return VanillaKind::kNone;
    }
    if (a_xList && a_xList->HasType(RE::ExtraDataType::kAliasInstanceArray)) {
        return VanillaKind::kRefused;   // a quest item, whatever else it is
    }
    auto* enchantable = AsEnchantable(a_object);
    if (enchantable && enchantable->formEnchanting) {
        // ★THE THREE TESTS FOR "AN ORDINARY ENCHANTED VARIANT": it is generic
        // (a leveled list or an outfit hands it out), it names a plain template
        // (every generated "Iron Sword of Frost" points at "Iron Sword"; no
        // artifact does), and the template is itself unenchanted.
        if (IsGeneric(a_object) && PlainTemplateOf(a_object)) {
            return VanillaKind::kRecordEnchanted;
        }
        return VanillaKind::kRefused;
    }
    if (a_xList) {
        const auto* xEnch = a_xList->GetByType<RE::ExtraEnchantment>();
        if (xEnch && xEnch->enchantment && !Persist::IsAffixEnch(xEnch->enchantment->GetFormID())) {
            // ★A MERGE KEEPS ITS RARITY CODE (Enchanting.cpp): the player
            // enchanted one of OUR items on top of its mods. Never replaced --
            // that would delete the mods as well as the player's work.
            if (Persist::EnchTier(xEnch->enchantment->GetFormID()) != 0) {
                return VanillaKind::kRefused;
            }
            // The player's own enchanting (or a mod's). Generic bases only.
            return IsGeneric(a_object) && xEnch->enchantment->IsDynamicForm()
                       ? VanillaKind::kInstanceEnchanted
                       : VanillaKind::kRefused;
        }
    }
    return VanillaKind::kNone;
}

int Apply::SwapVanillaLoose(RE::TESObjectREFR* a_refr)
{
    if (!a_refr || a_refr->IsPlayerRef()) {
        return 0;
    }
    // A COPY of the inventory (GetInventory), never the live entry list.
    auto inventory = a_refr->GetInventory([](RE::TESBoundObject& a_object) {
        return (a_object.Is(RE::FormType::Weapon) || a_object.Is(RE::FormType::Armor)) &&
               PlainTemplateOf(&a_object) != nullptr;
    });
    int swapped = 0;
    for (auto& [object, data] : inventory) {
        if (!object || data.first <= 0 || ClassifyEnchanted(object, nullptr) != VanillaKind::kRecordEnchanted) {
            continue;
        }
        // ★LOOSE UNITS ONLY. Anything with a list of its own -- worn, a quest
        // alias, owned, tempered -- is left exactly as it is; it is replaced
        // the first time the player looks at it in their own inventory.
        int loose = data.first;
        if (const auto& entry = data.second; entry && entry->extraLists) {
            for (const auto* xList : *entry->extraLists) {
                if (xList) {
                    loose -= xList->GetCount();
                }
            }
        }
        if (loose <= 0) {
            continue;
        }
        auto* plain = PlainTemplateOf(object);
        a_refr->RemoveItem(object, loose, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
        a_refr->AddObjectToContainer(plain, nullptr, loose, nullptr);
        swapped += loose;
    }
    return swapped;
}

Apply::Applied Apply::Replace(const roll::RolledItem& a_rolled, RE::TESBoundObject* a_object,
    RE::ExtraDataList* a_xList)
{
    Applied result;
    if (!a_xList) {
        return result;
    }
    // ★BUILT BEFORE ANYTHING IS LET GO (bug fix, 2.1.0). 1.2.0 released the old
    // enchantment and then built the new one, and a build that failed left the
    // item plain with its affixes gone. Now a failure leaves it exactly as it
    // was. The engine dedupes effect sets, so the new one can be the old one:
    // AddWeaponEnchantment has taken a reference, the release below drops the
    // item's previous reference, and the count comes out right either way.
    auto* ench = BuildEnchantment(a_rolled, a_object);
    if (!ench) {
        return result;
    }
    if (auto* xEnch = a_xList->GetByType<RE::ExtraEnchantment>(); xEnch && xEnch->enchantment) {
        auto* old = xEnch->enchantment;
        if (!old->IsDynamicForm()) {
            if (auto* manager = RE::BGSCreatedObjectManager::GetSingleton()) {
                manager->DestroyEnchantment(ench, IsWeapon(a_object));
            }
            logger::error("replace: {:08X} is not a created object; item left alone", old->GetFormID());
            return result;
        }
        a_xList->RemoveByType(RE::ExtraDataType::kEnchantment);
        a_xList->RemoveByType(RE::ExtraDataType::kTextDisplayData);
        ReleaseCreated(old, IsWeapon(a_object));
    }
    a_xList->SetEnchantment(ench, StartingCharge(a_rolled, a_object), false);
    Persist::NoteAffixEnch(ench->GetFormID());
    result.enchantment = ench;
    result.name = NameInstance(a_rolled, a_object, a_xList);
    return result;
}

Apply::Applied Apply::ToItem(const roll::RolledItem& a_rolled, RE::TESBoundObject* a_object,
    RE::ExtraDataList* a_xList)
{
    Applied result;

    if (!a_xList) {
        return result;
    }

    auto* ench = BuildEnchantment(a_rolled, a_object);
    if (!ench) {
        return result;
    }

    // ★Charge ZERO by default, deliberately. GetEnchantmentCharge's first
    // branch is guarded by `charge != 0`, so zero makes it fall through: an
    // unenchanted base draws no charge meter at all, which is the honest look
    // for an affix that never drains. Measured: it still fires from empty.
    // With weapon charge on, this is a real number and the meter draws.
    a_xList->SetEnchantment(ench, StartingCharge(a_rolled, a_object), false);
    Persist::NoteAffixEnch(ench->GetFormID());

    result.enchantment = ench;
    // The base is always the item itself now that enchanted records are declined.
    result.name = NameInstance(a_rolled, a_object, a_xList);

    return result;
}

namespace
{
    // ★THREAD-LOCAL, AND A COUNT RATHER THAN A FLAG. The events raised by the
    // drop and the pickup are dispatched synchronously on the calling thread, so
    // a thread_local is exactly the right scope -- a Distribute task rolling an
    // NPC on the main thread must not blind a sink to something happening
    // elsewhere. The count, not a bool, because nothing here promises this is
    // never re-entered and a nested clear would lift the guard early.
    thread_local int t_surgeryDepth = 0;

    struct SurgeryScope
    {
        SurgeryScope() { ++t_surgeryDepth; }
        ~SurgeryScope() { --t_surgeryDepth; }

        SurgeryScope(const SurgeryScope&)            = delete;
        SurgeryScope& operator=(const SurgeryScope&) = delete;
    };
}

bool Apply::InSurgery()
{
    return t_surgeryDepth > 0;
}

namespace
{
    std::atomic<bool>                  g_loadingSave{ false };   // kPreLoadGame .. kPostLoadGame
    std::atomic<bool>                  g_loadScreen{ false };    // LoadingMenu open
    std::mutex                         g_readyLock;
    std::vector<std::function<void()>> g_whenReady;

    void FlushWhenReady(const char* a_why)
    {
        std::vector<std::function<void()>> tasks;
        {
            std::scoped_lock lock{ g_readyLock };
            tasks.swap(g_whenReady);
        }
        if (tasks.empty()) {
            return;
        }
        if (Config::DebugLoadLog()) {
            logger::info("loading: {} -- running {} task(s) held for the load", a_why, tasks.size());
        }
        auto* intfc = SKSE::GetTaskInterface();
        for (auto& task : tasks) {
            if (intfc) {
                intfc->AddTask(std::move(task));
            }
        }
    }

    class LoadingSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        static LoadingSink* GetSingleton()
        {
            static LoadingSink singleton;
            return &singleton;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            if (!a_event || a_event->menuName != RE::LoadingMenu::MENU_NAME) {
                return RE::BSEventNotifyControl::kContinue;
            }
            g_loadScreen.store(a_event->opening);
            if (Config::DebugLoadLog()) {
                logger::info("loading: load screen {} (save loading {})", a_event->opening ? "UP" : "DOWN",
                    g_loadingSave.load());
            }
            if (!a_event->opening && !g_loadingSave.load()) {
                // A task, so it runs after the engine has finished attaching the cell.
                if (auto* intfc = SKSE::GetTaskInterface()) {
                    intfc->AddTask([]() {
                        FlushWhenReady("load screen down");
                        Orbs::Resume();
                    });
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };
}

bool Apply::WorldBusy()
{
    if (g_loadingSave.load() || g_loadScreen.load()) {
        return true;
    }
    if (auto* ui = RE::UI::GetSingleton(); ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) {
        return true;
    }
    auto* player = RE::PlayerCharacter::GetSingleton();
    return !player || !CanDropFrom(player);
}

void Apply::RunWhenWorldReady(std::function<void()> a_task)
{
    if (!a_task) {
        return;
    }
    if (!g_loadingSave.load() && !g_loadScreen.load()) {
        if (auto* intfc = SKSE::GetTaskInterface()) {
            intfc->AddTask(std::move(a_task));
        }
        return;
    }
    std::scoped_lock lock{ g_readyLock };
    g_whenReady.push_back(std::move(a_task));
}

void Apply::NoteLoading(bool a_loading, const char* a_why)
{
    g_loadingSave.store(a_loading);
    if (Config::DebugLoadLog()) {
        logger::info("loading: {} (save loading {}, load screen {})", a_why ? a_why : "?", a_loading,
            g_loadScreen.load());
    }
    if (!a_loading) {
        // The load screen may already be down by kPostLoadGame; if not, the
        // watch flushes when it goes.
        auto* ui = RE::UI::GetSingleton();
        const bool screen = ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
        g_loadScreen.store(screen);
        if (!screen) {
            if (auto* intfc = SKSE::GetTaskInterface()) {
                intfc->AddTask([]() {
                    FlushWhenReady("save loaded");
                    Orbs::Resume();
                });
            }
        }
    }
}

void Apply::InstallLoadingWatch()
{
    if (auto* ui = RE::UI::GetSingleton()) {
        ui->AddEventSink<RE::MenuOpenCloseEvent>(LoadingSink::GetSingleton());
        logger::info("loading: load-screen watch installed");
    } else {
        logger::error("loading: no UI singleton; tasks will not wait for load screens");
    }
}

bool Apply::CanDropFrom(RE::TESObjectREFR* a_refr)
{
    if (!a_refr) {
        return false;
    }
    const auto* cell = a_refr->GetParentCell();
    return cell && cell->IsAttached() && a_refr->Is3DLoaded();
}

Apply::Applied Apply::ToNewInstance(const roll::RolledItem& a_rolled,
    RE::TESBoundObject* a_object, RE::TESObjectREFR* a_refr)
{
    Applied result;

    // Raised for the whole function, not just around PickUpObject: the drop is
    // an inventory change too, and it is announced the same way.
    const SurgeryScope surgery;

    auto* actor = a_refr ? a_refr->As<RE::Actor>() : nullptr;
    if (!actor || !a_object) {
        return result;
    }

    auto* ench = BuildEnchantment(a_rolled, a_object);
    if (!ench) {
        return result;
    }

    const bool weapon = IsWeapon(a_object);
    const auto id = a_object->GetFormID();

    // The enchantment is in the created-objects manager from the moment
    // BuildEnchantment returns. Any path out of here that does not attach it
    // must destroy it, or the save carries a record for an item that does not
    // exist and the block grows for as long as the save lives.
    const auto abandon = [&](std::string_view a_why) {
        if (auto* manager = RE::BGSCreatedObjectManager::GetSingleton()) {
            manager->DestroyEnchantment(ench, weapon);
        }
        logger::warn("apply[{:08X}]: left unrolled -- {}", id, a_why);
    };

    // ★NOWHERE TO DROP IT. RemoveItem with kDropping places a reference at the
    // actor's position in the actor's cell; with no cell that walk goes through
    // null, and it did: new game under Alternate Start, the player still
    // "Prisoner" with ParentCell None, and a Roughspun Tunic to roll. Nothing
    // has been touched yet, so this is a clean refusal. QuestReward asks
    // CanDropFrom itself and skips such grants; this is the backstop.
    if (!CanDropFrom(actor)) {
        abandon("the actor is not in a loaded cell with 3D; nowhere to drop the item");
        return result;
    }

    // ★STEP 1: OUT INTO THE WORLD. RemoveItem with kDropping hands back a real
    // reference, and a reference carries an ExtraDataList of its own -- built by
    // the engine, at the moment of the drop, correctly. That list is the thing
    // four earlier builds tried and failed to conjure.
    logger::debug("apply[{:08X}]: 1 dropping one to get a reference", id);
    auto handle = actor->RemoveItem(a_object, 1, RE::ITEM_REMOVE_REASON::kDropping, nullptr,
        nullptr);
    auto dropped = handle.get();
    if (!dropped) {
        abandon("the drop produced no reference");
        return result;
    }

    // ★OURS, SAID SO BEFORE ANYTHING PICKS IT UP. A reference the engine creates
    // in a cell inherits that cell's ownership, and the drop above goes through
    // RemoveItem rather than the inventory menu's own drop, which is the path
    // that stamps the player onto a dropped item. Inside a shop or a house that
    // left the reward off-limits to the very player who had just been handed
    // it: the pickup below counted as theft, and the armour came back marked
    // stolen -- or lay on the floor under a "Steal" prompt (Leather Armor
    // reward, 2026-09-05). The owner is the actor whose inventory it just left,
    // which is exactly what the engine's own drop would have recorded.
    if (auto* owner = actor->GetActorBase(); owner && dropped->GetOwner() != owner) {
        if (dropped->IsOffLimits()) {
            const auto* was = dropped->GetOwner();
            logger::info("apply[{:08X}]: dropped reference {:08X} was off-limits (owner {:08X}); "
                         "claiming it for the actor before the pickup",
                id, dropped->GetFormID(), was ? was->GetFormID() : 0);
        }
        dropped->SetOwner(owner);
    }

    // ★STEP 1b: LOOK AT WHAT WE ACTUALLY GOT, because we did not choose it.
    //
    // RemoveItem takes a COUNT, not an instance. If the player holds three
    // Silver Rings and one of them is already enchanted, the engine picks which
    // one leaves the stack and it may well pick that one -- and SetEnchantment
    // on it would overwrite a roll the player already had, stranding the old
    // created enchantment with nothing left to call Release on it.
    //
    // The caller used to head this off by refusing the whole stack whenever ANY
    // instance was enchanted, which is safe and much too broad: it skipped every
    // reward of a record the player was already carrying an affixed copy of, and
    // rings and common armour are exactly the records that duplicate. It was not
    // a decision that had to be made before the drop. A dropped reference brings
    // its own ExtraDataList with it, so the question the guard was guessing at
    // is simply readable here, and the answer is about THIS instance rather than
    // about the stack it came from.
    //
    // Wrong instance: hand it straight back the same way step 3 hands back the
    // right one, and leave. Nothing has been written to it.
    if (!IsEligible(a_object, &dropped->extraList)) {
        actor->PickUpObject(dropped.get(), 1, false, false);
        result.declined = true;
        if (auto* manager = RE::BGSCreatedObjectManager::GetSingleton()) {
            manager->DestroyEnchantment(ench, weapon);
        }
        logger::info("apply[{:08X}]: left unrolled -- the engine handed back an instance that "
                     "already carries an enchantment or a quest alias; it has been returned",
            id);
        return result;
    }

    // ★STEP 2: THE PROVEN CALL, on the reference's own list. This is the same
    // SetEnchantment that every affixed item already in the save went through,
    // which is what makes the result render like the rest of them rather than
    // like something assembled by hand.
    logger::debug("apply[{:08X}]: 2 got reference {:08X}, enchanting it", id,
        dropped->GetFormID());
    dropped->extraList.SetEnchantment(ench, StartingCharge(a_rolled, a_object), false);
    Persist::NoteAffixEnch(ench->GetFormID());
    result.name = NameInstance(a_rolled, a_object, &dropped->extraList);

    // ★STEP 3: BACK IN, BY THE ENGINE'S OWN PICKUP. PickUpObject moves the
    // reference and its extra data into the inventory and does the entry
    // accounting itself -- no count to reconcile, because nothing here invented
    // one. Silent: the pickup sound belongs to the player picking something up,
    // and nobody picked this up.
    //
    // If this somehow fails the item is lying on the floor, which the player can
    // simply take. That is the mildest failure mode any version of this has had.
    logger::debug("apply[{:08X}]: 3 handing it back", id);
    actor->PickUpObject(dropped.get(), 1, false, false);
    if (!dropped->IsDeleted() && !dropped->IsDisabled()) {
        // Not a crash, but the mildest failure mode has a face now: the affixed
        // item is lying at the actor's feet instead of in the pack.
        logger::warn("apply[{:08X}]: 3 the pickup left reference {:08X} in the world; the item "
                     "is on the ground where the actor stands",
            id, dropped->GetFormID());
    }

    // ★THE THREE LINES ABOVE STAY, at debug, and they are not leftovers.
    //
    // They exist because this function is three engine calls in a row on a live
    // inventory, and an earlier version of it logged once at the end -- which,
    // when it faulted, left the log silent and the crash stack pointing at
    // another mod's frame tick. Two builds were spent narrowing down which call
    // it was. Bracketing each one turned the third build into a reading rather
    // than another guess, and costs nothing at info level.
    result.enchantment = ench;
    return result;
}

void Apply::Release(RE::ExtraDataList* a_xList, bool a_isWeapon)
{
    if (!a_xList) {
        return;
    }

    auto* xEnch = a_xList->GetByType<RE::ExtraEnchantment>();
    if (!xEnch || !xEnch->enchantment) {
        return;
    }

    auto* ench = xEnch->enchantment;

    // Only created objects are ours to release. A base record's enchantment
    // reached through here would be a bug, and destroying it would be a very
    // bad one -- it is shared by every instance of that record in the game.
    if (!ench->IsDynamicForm()) {
        logger::error("release: {:08X} is not a created object; refusing to touch it",
            ench->GetFormID());
        return;
    }

    a_xList->RemoveByType(RE::ExtraDataType::kEnchantment);
    a_xList->RemoveByType(RE::ExtraDataType::kTextDisplayData);

    // ★The half that RemoveByType does not do. Detaching alone leaves the
    // manager counting a reference nothing holds; the save records that, and the
    // load throws -- measured twice, reproducibly, before this existed.
    ReleaseCreated(ench, a_isWeapon);
}

void Apply::ReleaseCreated(RE::EnchantmentItem* a_ench, bool a_isWeapon)
{
    if (!a_ench) {
        return;
    }

    // Read the id BEFORE the manager is told to let go -- once the last holder
    // releases, the form is gone and GetFormID() is a read through a dead
    // pointer.
    const auto enchID = a_ench->GetFormID();

    // The manager refcounts sharing, so this decrements and only actually
    // destroys when the last holder lets go.
    auto* manager = RE::BGSCreatedObjectManager::GetSingleton();
    if (manager) {
        manager->DestroyEnchantment(a_ench, a_isWeapon);
    }

    // ★THE RECORDS FOLLOW THE FORM, NOT THE HOLDER. While another item still
    // carries this enchantment, its band and its ownership are still true and
    // stay. Once nothing does, the entries must not outlive it: the engine
    // reuses created-object ids, and a stale entry would eventually colour --
    // or, worse, let the table strip -- some unrelated item that inherited the
    // number.
    if (StillCreated(manager, a_ench, a_isWeapon)) {
        logger::debug("release: {:08X} is still carried by another item; records kept", enchID);
        return;
    }
    Persist::ForgetEnchTier(enchID);
    Persist::ForgetAffixEnch(enchID);
    Persist::ForgetItemRecord(enchID);
}
