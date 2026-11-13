// =============================================================================
//  The debug menu. See DebugMenu.h.
// =============================================================================

#include "PCH.h"

#include "DebugMenu.h"

#include "Apply.h"
#include "Config.h"
#include "Orbs.h"
#include "Crafting.h"
#include "Distribute.h"
#include "GridTint.h"
#include "LootApi.h"
#include "TableReload.h"
#include "Persist.h"
#include "Records.h"
#include "Sockets.h"
#include "Version.h"

#include "roll/Craft.h"
#include "roll/Record.h"
#include "roll/Roll.h"

// The framework's header uses std::filesystem without including it.
#include <filesystem>

#include "SKSEMenuFramework.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace
{
    using namespace ImGuiMCP;

    constexpr const char* kSection = "Diablo In Skyrim Debug Menu";

    std::atomic<bool> g_available{ false };

    // Everything below the lock is written by a game-thread task and read by
    // the render callbacks, or the other way round.
    std::mutex g_lock;

    void RunOnGameThread(std::function<void()> a_task)
    {
        if (auto* tasks = SKSE::GetTaskInterface()) {
            tasks->AddTask(std::move(a_task));
        }
    }

    // ImU32 (0xAABBGGRR) -> ImVec4, so the menu shows the INI's own colours.
    ImVec4 FromRgba(std::uint32_t a_rgba)
    {
        return { static_cast<float>(a_rgba & 0xFF) / 255.0f, static_cast<float>((a_rgba >> 8) & 0xFF) / 255.0f,
            static_cast<float>((a_rgba >> 16) & 0xFF) / 255.0f, 1.0f };
    }

    ImVec4 RarityColour(roll::Rarity a_rarity, bool a_corrupted)
    {
        if (a_corrupted) {
            return FromRgba(Config::RarityColor(Config::RarityColour::kCorrupted));
        }
        switch (a_rarity) {
        case roll::Rarity::kMagic: return FromRgba(Config::RarityColor(Config::RarityColour::kMagic));
        case roll::Rarity::kRare:  return FromRgba(Config::RarityColor(Config::RarityColour::kRare));
        default:                   return FromRgba(Config::RarityColor(Config::RarityColour::kNormal));
        }
    }

    const ImVec4 kBad{ 0.90f, 0.24f, 0.27f, 1.0f };
    const ImVec4 kGood{ 0.45f, 0.85f, 0.45f, 1.0f };

    // ---- one item, as the inspector and the simulator see it ----------------

    struct Item
    {
        std::string   name;
        RE::FormID    base{ 0 };
        std::uint32_t slots{ roll::kNone };
        bool          worn{ false };
        // A plain item an orb could be used on: no enchantment, so no view.
        // Listed so the crafting page has something for Transmutation.
        bool          plain{ false };
        int           plainLevel{ 1 };   // the level a plain item would roll at: the player's
        Records::View view;

        [[nodiscard]] int Level() const { return plain ? plainLevel : view.record.itemLevel; }
        [[nodiscard]] Crafting::Target Target() const { return { base, plain ? 0u : view.enchantment }; }
    };

    std::vector<Item> g_inventory;        // rolled items the player carries, as of the last refresh
    bool              g_inventoryRead = false;

    std::string NameOf(RE::TESBoundObject* a_object, RE::ExtraDataList* a_xList)
    {
        const char* name = a_xList ? a_xList->GetDisplayName(a_object) : nullptr;
        if (!name || !*name) {
            name = a_object ? a_object->GetName() : nullptr;
        }
        return (name && *name) ? name : "(unnamed)";
    }

    // GAME THREAD. GetInventory hands back its own copy of the entries, so the
    // walk is over that rather than over the list the engine is working on.
    void ReadInventory()
    {
        std::vector<Item> found;
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            auto inventory = player->GetInventory([](RE::TESBoundObject& a_object) {
                return a_object.Is(RE::FormType::Weapon) || a_object.Is(RE::FormType::Armor);
            });
            const int playerLevel = (std::max)(1, static_cast<int>(player->GetLevel()));
            for (auto& [object, data] : inventory) {
                const auto& entry = data.second;
                if (!object) {
                    continue;
                }
                // Units that already carry an enchantment, ours or not; whatever
                // is left of the count is plain.
                std::int32_t enchanted = 0;
                if (entry && entry->extraLists) {
                    for (auto* xList : *entry->extraLists) {
                        if (!xList) {
                            continue;
                        }
                        if (xList->HasType<RE::ExtraEnchantment>()) {
                            enchanted += (std::max)(1, xList->GetCount());
                        }
                        Item item;
                        if (!Records::Lookup(xList, item.view)) {
                            continue;
                        }
                        item.name = NameOf(object, xList);
                        item.base = object->GetFormID();
                        item.slots = Apply::SlotsOf(object);
                        item.worn = xList->HasType(RE::ExtraDataType::kWorn) ||
                                    xList->HasType(RE::ExtraDataType::kWornLeft);
                        found.push_back(std::move(item));
                    }
                }
                const char* reason = nullptr;
                if (data.first > enchanted && Crafting::Craftable(object, reason) &&
                    Apply::IsGeneric(object)) {
                    Item item;
                    item.name = NameOf(object, nullptr);
                    item.base = object->GetFormID();
                    item.slots = Apply::SlotsOf(object);
                    item.plain = true;
                    item.plainLevel = playerLevel;
                    found.push_back(std::move(item));
                }
            }
        }
        std::scoped_lock lock{ g_lock };
        g_inventory = std::move(found);
        g_inventoryRead = true;
    }

    // The item the grid last showed a tooltip for. Render thread: form lookups
    // and Persist's lock, nothing that mutates.
    bool HoveredItem(Item& a_out)
    {
        std::uint32_t base = 0;
        std::uint32_t enchID = 0;
        if (!GridTint::LastTooltipItem(base, enchID)) {
            return false;
        }
        const auto* ench = RE::TESForm::LookupByID<RE::EnchantmentItem>(enchID);
        if (!ench || !Records::Lookup(ench, a_out.view)) {
            return false;
        }
        auto* object = RE::TESForm::LookupByID<RE::TESBoundObject>(base);
        a_out.base = base;
        a_out.slots = Apply::SlotsOf(object);
        a_out.name = (object && object->GetName() && *object->GetName()) ? object->GetName() : "(unnamed)";
        a_out.worn = false;
        return true;
    }

    void DrawItem(const Item& a_item)
    {
        const auto& view = a_item.view;
        const auto  rarity = roll::RarityOf(view.rolled);

        TextColored(RarityColour(rarity, view.rolled.corrupted), "%s%s", a_item.name.c_str(),
            a_item.worn ? "  (worn)" : "");
        Text("Rarity: %s%s   Mods: %d   Implicits: %d of %d   Points: %d", roll::RarityName(rarity),
            view.rolled.corrupted ? " (corrupted)" : "", view.rolled.Explicits(), view.rolled.Implicits(),
            roll::kMaxImplicits, view.rolled.tier);
        Text("Item level: %u%s", static_cast<unsigned>(view.record.itemLevel),
            (view.record.flags & roll::kRecordLegacy) ? "  (estimated: rebuilt from the enchantment)" : "");
        Text("Base form: %08X   Created enchantment: %08X", a_item.base, view.enchantment);
        const bool magic = rarity == roll::Rarity::kMagic;
        Text("Slots: %s   Prefixes %d/%d  Suffixes %d/%d", roll::SlotsToString(a_item.slots).c_str(),
            view.rolled.Count(roll::AffixKind::kPrefix), magic ? roll::kMagicMaxPrefixes : roll::MaxPrefixes(),
            view.rolled.Count(roll::AffixKind::kSuffix), magic ? roll::kMagicMaxSuffixes : roll::MaxSuffixes());
        Text("Co-save: rarity code %02X, ours %s, record %s%s%s", Persist::EnchTier(view.enchantment),
            Persist::IsAffixEnch(view.enchantment) ? "yes" : "no",
            view.record.affixes.empty() && view.record.implicits.empty() ? "no" : "yes",
            (view.record.flags & roll::kRecordCrafted) ? ", crafted" : "",
            (view.record.flags & roll::kRecordImplicitGranted) ? ", implicit granted later" : "");
        if (!view.complete) {
            TextColored(kBad, "Some mods on this item are no longer in the affix table; "
                              "it cannot be crafted on.");
        }

        const auto list = [&](const std::vector<roll::AffixRecord>& a_entries) {
            for (const auto& entry : a_entries) {
                const roll::Affix* affix = nullptr;
                if (const auto* table = Records::Table()) {
                    affix = roll::FindAffix(*table, entry.id);
                }
                const auto* tier = affix ? roll::FindTier(*affix, entry.tier) : nullptr;
                if (affix && tier) {
                    BulletText("%s %s  tier %d (%d pt)  value %.2f  range %.2f to %.2f  [%s]",
                        roll::KindName(affix->kind), affix->name.c_str(), tier->tier, tier->points,
                        entry.value, tier->minValue, tier->maxValue, entry.id.c_str());
                } else {
                    BulletText("%s  tier %u  value %.2f  (not in the table)", entry.id.c_str(),
                        static_cast<unsigned>(entry.tier), entry.value);
                }
            }
        };
        Spacing();
        if (!view.record.implicits.empty()) {
            TextColored(RarityColour(rarity, view.rolled.corrupted), "Implicit");
            list(view.record.implicits);
        }
        list(view.record.affixes);
    }

    // ---- page: Inspect -------------------------------------------------------

    void __stdcall RenderInspect()
    {
        const Records::ReadGuard tableGuard{ Records::TableLock() };
        SeparatorText("Hovered in Grid Inventory");
        Item hovered;
        if (HoveredItem(hovered)) {
            DrawItem(hovered);
        } else {
            TextDisabled("Nothing yet. Hover a rolled item in Grid Inventory, then come back here.");
        }

        SeparatorText("Rolled items you are carrying");
        if (Button("Read my inventory")) {
            RunOnGameThread(ReadInventory);
        }
        SameLine();
        TextDisabled("A snapshot: press again after anything changes.");

        std::scoped_lock lock{ g_lock };
        if (g_inventoryRead && std::none_of(g_inventory.begin(), g_inventory.end(),
                                   [](const Item& a_item) { return !a_item.plain; })) {
            TextDisabled("No rolled items found.");
        }
        for (std::size_t i = 0; i < g_inventory.size(); ++i) {
            const auto& item = g_inventory[i];
            if (item.plain) {
                continue;   // nothing rolled to inspect; the crafting page lists these
            }
            const auto  label = std::format("{}{}##item{}", item.name, item.worn ? "  (worn)" : "", i);
            if (CollapsingHeader(label.c_str())) {
                DrawItem(item);
            }
        }

        Spacing();
        TextDisabled("To change an item, use the Craft on items page.");
    }

    // ---- page: Craft simulator ----------------------------------------------

    struct SlotChoice
    {
        const char*   label;
        std::uint32_t mask;
    };

    constexpr SlotChoice kSlotChoices[]{
        { "One-handed weapon", roll::kWeapon | roll::kOneHanded },
        { "Two-handed weapon", roll::kWeapon | roll::kTwoHanded },
        { "Bow", roll::kWeapon | roll::kBow },
        { "Staff", roll::kWeapon | roll::kStaff },
        { "Body armour", roll::kArmor | roll::kBody },
        { "Helmet", roll::kArmor | roll::kHead },
        { "Gauntlets", roll::kArmor | roll::kHands },
        { "Boots", roll::kArmor | roll::kFeet },
        { "Shield", roll::kArmor | roll::kShield },
        { "Ring", roll::kRing },
        { "Amulet", roll::kAmulet },
    };

    struct SimResult
    {
        bool                              ran{ false };
        int                               samples{ 0 };
        int                               worked{ 0 };
        std::string                       source;
        std::string                       orb;
        std::string                       refusal;
        std::array<int, 4>                rarity{};   // normal, magic, rare, corrupted
        double                            meanAffixes{ 0.0 };
    };

    SimResult g_sim;   // render thread only

    void RunSimulation(const roll::RolledItem& a_start, const roll::RollContext& a_ctx,
        roll::Orb a_orb, int a_samples, std::string a_source)
    {
        g_sim = SimResult{};
        g_sim.source = std::move(a_source);
        g_sim.orb = roll::OrbName(a_orb);
        g_sim.samples = a_samples;

        const auto* table = Records::Table();
        if (!table) {
            g_sim.refusal = "The affix table has not loaded.";
            g_sim.ran = true;
            return;
        }

        // The roller's shipped curve: what every roll site is constructed with.
        const roll::Tuning tuning;
        roll::Rng          rng{ std::random_device{}() };

        long long affixes = 0;
        for (int i = 0; i < a_samples; ++i) {
            roll::RolledItem item = a_start;
            const auto       verdict = roll::Craft(a_orb, item, a_ctx, *table, tuning, rng);
            if (verdict != roll::CraftVerdict::kOk) {
                // The count rule refuses every sample alike, so the first
                // refusal is the whole answer.
                g_sim.refusal = roll::Reason(verdict);
                break;
            }
            ++g_sim.worked;
            g_sim.rarity[static_cast<std::size_t>(roll::RarityOf(item)) - 1]++;
            if (item.corrupted) {
                g_sim.rarity[3]++;
            }
            affixes += item.Explicits();
        }
        if (g_sim.worked > 0) {
            g_sim.meanAffixes = static_cast<double>(affixes) / g_sim.worked;
        }
        g_sim.ran = true;
    }

    void __stdcall RenderCraft()
    {
        const Records::ReadGuard tableGuard{ Records::TableLock() };
        static int s_source = 0;      // 0 = plain item, 1 = hovered, 2+ = inventory snapshot
        static int s_slot = 0;
        static int s_level = 20;
        static int s_orb = 0;
        static int s_samples = 1000;

        TextWrapped("Uses one orb on a copy of an item, many times over, and shows what rarity "
                    "the copies come out. Nothing in the game is changed and no orb is spent.");

        SeparatorText("Item");
        Item hovered;
        const bool haveHovered = HoveredItem(hovered);

        std::vector<Item> inventory;
        {
            std::scoped_lock lock{ g_lock };
            inventory = g_inventory;
        }
        std::erase_if(inventory, [](const Item& a_item) { return a_item.plain; });

        std::vector<std::string> labels{ "A normal item (base implicit only)" };
        labels.push_back(haveHovered ? std::format("Hovered: {}", hovered.name)
                                     : std::string{ "Hovered: (nothing)" });
        for (const auto& item : inventory) {
            labels.push_back(std::format("Carried: {}", item.name));
        }
        if (s_source >= static_cast<int>(labels.size())) {
            s_source = 0;
        }
        if (BeginCombo("Start from", labels[static_cast<std::size_t>(s_source)].c_str())) {
            for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
                const auto label = std::format("{}##src{}", labels[static_cast<std::size_t>(i)], i);
                if (Selectable(label.c_str(), i == s_source)) {
                    s_source = i;
                }
            }
            EndCombo();
        }
        if (inventory.empty()) {
            TextDisabled("Carried items appear here after \"Read my inventory\" on the Inspect page.");
        }

        roll::RolledItem  start;
        roll::RollContext ctx;
        std::string       sourceName;
        bool              usable = true;

        if (s_source == 0) {
            std::array<const char*, std::size(kSlotChoices)> names{};
            for (std::size_t i = 0; i < std::size(kSlotChoices); ++i) {
                names[i] = kSlotChoices[i].label;
            }
            Combo("Kind", &s_slot, names.data(), static_cast<int>(names.size()));
            SliderInt("Item level", &s_level, 1, 80);
            ctx.itemLevel = s_level;
            ctx.itemSlots = kSlotChoices[std::clamp(s_slot, 0, static_cast<int>(std::size(kSlotChoices)) - 1)].mask;
            sourceName = std::format("normal {}, item level {}", names[static_cast<std::size_t>(s_slot)], s_level);
            if (const auto* table = Records::Table()) {
                roll::Tuning base;
                for (auto& band : base.bands) {
                    band.countWeights.fill(0);
                    band.countWeights[0] = 1;
                }
                roll::Rng probe{ 777u };
                start = roll::Roll(*table, ctx, base, probe);
            }
        } else {
            const Item* item = nullptr;
            if (s_source == 1) {
                item = haveHovered ? &hovered : nullptr;
            } else {
                item = &inventory[static_cast<std::size_t>(s_source - 2)];
            }
            if (!item) {
                TextDisabled("Hover a rolled item in Grid Inventory first.");
                usable = false;
            } else if (!item->view.complete) {
                TextDisabled("That item has mods the table no longer knows; it cannot be crafted on.");
                usable = false;
            } else {
                start = item->view.rolled;
                ctx.itemLevel = item->view.record.itemLevel;
                ctx.itemSlots = item->slots;
                sourceName = std::format("{}, item level {}, {} mod(s)", item->name, ctx.itemLevel,
                    start.Explicits());
                Text("%s", sourceName.c_str());
            }
        }

        SeparatorText("Orb");
        std::array<const char*, roll::kOrbCount> orbNames{};
        for (int i = 0; i < roll::kOrbCount; ++i) {
            orbNames[static_cast<std::size_t>(i)] = roll::OrbName(static_cast<roll::Orb>(i));
        }
        Combo("Orb", &s_orb, orbNames.data(), roll::kOrbCount);
        SliderInt("Times", &s_samples, 1, 100000);

        BeginDisabled(!usable);
        if (Button("Craft x N")) {
            RunSimulation(start, ctx, static_cast<roll::Orb>(std::clamp(s_orb, 0, roll::kOrbCount - 1)),
                std::clamp(s_samples, 1, 100000), sourceName);
        }
        EndDisabled();

        if (!g_sim.ran) {
            return;
        }
        SeparatorText("Result");
        Text("%s on %s", g_sim.orb.c_str(), g_sim.source.c_str());
        if (!g_sim.refusal.empty()) {
            TextColored(kBad, "Refused: %s", g_sim.refusal.c_str());
            if (g_sim.worked == 0) {
                return;
            }
        }
        Text("Worked %d of %d time(s); mean mods afterwards %.2f", g_sim.worked, g_sim.samples,
            g_sim.meanAffixes);
        static constexpr const char* kRarityNames[]{ "normal", "magic", "rare", "corrupted" };
        for (std::size_t b = 0; b < g_sim.rarity.size(); ++b) {
            const int   count = g_sim.rarity[b];
            const float share = g_sim.worked > 0 ? static_cast<float>(count) / static_cast<float>(g_sim.worked) : 0.0f;
            const auto  overlay = std::format("{}  {}  ({:.1f}%)", kRarityNames[b], count, 100.0f * share);
            ProgressBar(share, ImVec2(-1.0f, 0.0f), overlay.c_str());
        }
    }

    // ---- page: Craft on items ------------------------------------------------
    //
    // The real thing, on real items, for free. Every button here changes the
    // save, so each needs the confirm box ticked -- it clears itself after one
    // action -- and each writes a line to the log from Crafting.cpp.

    Crafting::Target g_forgeSelect;   // under g_lock; follows the item through a craft
    std::string      g_forgeReport;   // under g_lock

    // GAME THREAD, after an action: re-read the inventory so the page shows the
    // item as it now is, and keep it selected under its new enchantment.
    void AfterAction(const Crafting::Target& a_target, const Crafting::Outcome& a_outcome,
        std::string a_what)
    {
        ReadInventory();
        std::scoped_lock lock{ g_lock };
        g_forgeReport = std::format("{}: {}{}", a_what, a_outcome.ok ? "" : "refused -- ",
            a_outcome.message.empty() ? "done" : a_outcome.message);
        if (a_outcome.ok) {
            g_forgeSelect = { a_target.base, a_outcome.enchantment };
        }
    }

    void __stdcall RenderForge()
    {
        const Records::ReadGuard tableGuard{ Records::TableLock() };
        static int  s_orb = 0;
        static bool s_confirm = false;
        static char s_base[16] = "00012EB7";   // Iron Sword
        static int  s_rarity = 1;
        static int  s_level = 20;

        TextWrapped("Uses orbs on the items you carry without spending any, edits single mods and "
                    "implicits, and spawns rolled items. All of it changes your save: keep a backup.");

        if (Button("Read my inventory")) {
            RunOnGameThread(ReadInventory);
        }
        SameLine();
        TextDisabled("Lists rolled items and plain ones an orb could be used on.");

        std::vector<Item> items;
        Crafting::Target  selected;
        std::string       report;
        {
            std::scoped_lock lock{ g_lock };
            items = g_inventory;
            selected = g_forgeSelect;
            report = g_forgeReport;
        }

        const auto labelOf = [](const Item& a_item) {
            return std::format("{}{}{}", a_item.name, a_item.plain ? "  (plain)" : "",
                a_item.worn ? "  (worn)" : "");
        };

        int index = -1;
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            const auto target = items[static_cast<std::size_t>(i)].Target();
            if (target.base == selected.base && target.enchantment == selected.enchantment) {
                index = i;
                break;
            }
        }

        const std::string preview = index >= 0 ? labelOf(items[static_cast<std::size_t>(index)])
                                               : std::string{ "(choose an item)" };
        if (BeginCombo("Item", preview.c_str())) {
            for (int i = 0; i < static_cast<int>(items.size()); ++i) {
                const auto& item = items[static_cast<std::size_t>(i)];
                const auto  label = std::format("{}##forge{}", labelOf(item), i);
                if (Selectable(label.c_str(), i == index)) {
                    std::scoped_lock lock{ g_lock };
                    g_forgeSelect = item.Target();
                }
            }
            EndCombo();
        }

        Checkbox("Yes, change my save (clears after each action)", &s_confirm);

        const auto* table = Records::Table();
        if (index >= 0 && table) {
            const Item& item = items[static_cast<std::size_t>(index)];
            const auto  target = item.Target();

            if (item.plain) {
                Text("%s  --  plain (no implicit yet); gets its base implicit at item level %d",
                    item.name.c_str(), item.Level());
            } else {
                DrawItem(item);
            }

            SeparatorText("Orb");
            std::array<const char*, roll::kOrbCount> orbNames{};
            for (int i = 0; i < roll::kOrbCount; ++i) {
                orbNames[static_cast<std::size_t>(i)] = roll::OrbName(static_cast<roll::Orb>(i));
            }
            Combo("Orb##forge", &s_orb, orbNames.data(), roll::kOrbCount);
            const auto orb = static_cast<roll::Orb>(std::clamp(s_orb, 0, roll::kOrbCount - 1));

            // The same question the orb itself will ask, asked early so the
            // button can say why it is greyed out.
            const roll::RollContext ctx{ item.Level(), item.slots, false };
            const roll::Tuning      tuning;
            const bool readable = item.plain || item.view.complete;
            const auto verdict = readable
                ? roll::CanCraft(orb, item.plain ? roll::RolledItem{} : item.view.rolled, ctx, *table, tuning)
                : roll::CraftVerdict::kNotAffixable;
            if (!readable) {
                TextDisabled("This item has mods the table no longer knows; it can't be crafted.");
            } else if (verdict != roll::CraftVerdict::kOk) {
                TextDisabled("%s", roll::Reason(verdict));
            }

            BeginDisabled(!s_confirm || verdict != roll::CraftVerdict::kOk);
            if (Button("Apply orb (free)")) {
                s_confirm = false;
                RunOnGameThread([target, orb]() {
                    AfterAction(target, Crafting::ApplyOrb(target, orb), roll::OrbName(orb));
                });
            }
            EndDisabled();

            if (!item.plain && readable) {
                const bool corrupted = item.view.rolled.corrupted;
                const auto editRow = [&](const roll::RolledAffix& a_rolled, int a_i, bool a_canRemove) {
                    if (!a_rolled.affix) {
                        return;
                    }
                    const std::string id = a_rolled.affix->id;
                    PushID(a_i);
                    BeginDisabled(!s_confirm || corrupted);
                    if (a_canRemove) {
                        if (Button("Remove")) {
                            s_confirm = false;
                            RunOnGameThread([target, id]() {
                                AfterAction(target, Crafting::RemoveAffix(target, id), "Remove " + id);
                            });
                        }
                        SameLine();
                    }
                    if (Button("Reroll value")) {
                        s_confirm = false;
                        RunOnGameThread([target, id]() {
                            AfterAction(target, Crafting::RerollValue(target, id), "Reroll " + id);
                        });
                    }
                    EndDisabled();
                    SameLine();
                    Text("%s %s  tier %d  %.2f", roll::KindName(a_rolled.affix->kind),
                        a_rolled.affix->name.c_str(), a_rolled.tier, a_rolled.value);
                    PopID();
                };

                SeparatorText("Implicits");
                int shown = 0;
                for (int i = 0; i < static_cast<int>(item.view.rolled.affixes.size()); ++i) {
                    const auto& rolled = item.view.rolled.affixes[static_cast<std::size_t>(i)];
                    if (rolled.affix && rolled.affix->Implicit()) {
                        editRow(rolled, i, false);
                        ++shown;
                    }
                }
                if (shown == 0) {
                    TextDisabled("None yet (a 1.2.0 item gets one the first time it is hovered).");
                }
                BeginDisabled(!s_confirm || corrupted || item.view.rolled.Implicits() != 1);
                if (Button("Vaal-add implicit (corrupts; no-op if none is valid)")) {
                    s_confirm = false;
                    RunOnGameThread([target]() {
                        AfterAction(target, Crafting::AddSecondImplicit(target), "Vaal-add implicit");
                    });
                }
                EndDisabled();

                SeparatorText("Corrupted lock");
                bool lock = corrupted;
                BeginDisabled(!s_confirm);
                if (Checkbox("Corrupted (no orb, Divine or Scouring works; no rerolls)", &lock)) {
                    s_confirm = false;
                    RunOnGameThread([target, lock]() {
                        AfterAction(target, Crafting::SetCorrupted(target, lock), lock ? "Corrupt" : "Uncorrupt");
                    });
                }
                EndDisabled();

                SeparatorText("Edit mods");
                if (corrupted) {
                    TextColored(kBad, "Corrupted: edits are locked.");
                }
                for (int i = 0; i < static_cast<int>(item.view.rolled.affixes.size()); ++i) {
                    const auto& rolled = item.view.rolled.affixes[static_cast<std::size_t>(i)];
                    if (rolled.affix && !rolled.affix->Implicit()) {
                        editRow(rolled, i, true);
                    }
                }
            }
        } else if (items.empty()) {
            TextDisabled("Press \"Read my inventory\" first.");
        }

        SeparatorText("Spawn an item at my feet");
        InputText("Base FormID (hex)", s_base, sizeof(s_base));
        if (index >= 0) {
            SameLine();
            if (Button("Use the selected item's base")) {
                const auto text = std::format("{:08X}", items[static_cast<std::size_t>(index)].base);
                std::snprintf(s_base, sizeof(s_base), "%s", text.c_str());
            }
        }
        static constexpr const char* kSpawnRarity[]{ "normal", "magic", "rare" };
        Combo("Rarity", &s_rarity, kSpawnRarity, 3);
        SliderInt("Item level##spawn", &s_level, 1, 80);
        TextDisabled("Every rarity carries its base implicit. The item appears on the ground.");

        BeginDisabled(!s_confirm);
        if (Button("Spawn")) {
            s_confirm = false;
            const auto base = static_cast<RE::FormID>(std::strtoul(s_base, nullptr, 16));
            const int  rarity = s_rarity;
            const int  level = s_level;
            RunOnGameThread([base, rarity, level]() {
                const auto outcome = Crafting::SpawnAtFeet(base, rarity, level);
                std::scoped_lock lock{ g_lock };
                g_forgeReport = std::format("Spawn: {}{}", outcome.ok ? "" : "refused -- ",
                    outcome.message);
            });
        }
        EndDisabled();

        if (!report.empty()) {
            SeparatorText("Last action");
            TextWrapped("%s", report.c_str());
        }
    }

    // ---- page: Co-save -------------------------------------------------------

    std::string g_orphanReport;   // under g_lock
    std::string g_clearReport;    // under g_lock

    // GAME THREAD. An entry is orphaned when the enchantment it is keyed on no
    // longer exists -- released by a path that did not go through
    // Apply::ReleaseCreated, most likely. Reported, never removed here: a
    // stale entry is inert until its id is reused, and deciding to drop
    // co-save state is not something a diagnostic should do on its own.
    void CheckOrphans()
    {
        const auto  ids = Persist::KnownEnchantments();
        std::size_t orphans = 0;
        std::string examples;
        for (const auto id : ids) {
            if (RE::TESForm::LookupByID<RE::EnchantmentItem>(id)) {
                continue;
            }
            if (orphans < 8) {
                examples += std::format("{}{:08X}", examples.empty() ? "" : ", ", id);
            }
            ++orphans;
        }
        logger::info("debug menu: orphan check -- {} enchantment(s) known to the co-save, {} with "
                     "no live form{}",
            ids.size(), orphans, examples.empty() ? "" : " (" + examples + ")");

        std::scoped_lock lock{ g_lock };
        g_orphanReport = orphans == 0
            ? std::format("{} enchantment(s) checked; every one still exists.", ids.size())
            : std::format("{} of {} have no live enchantment: {}{}", orphans, ids.size(), examples,
                  orphans > 8 ? ", ..." : "");
    }

    // GAME THREAD, and it changes the save: every actor and container in the
    // player's cell becomes eligible to roll again.
    void ClearCellMarks()
    {
        std::size_t cleared = 0;
        RE::FormID  cellID = 0;
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            if (auto* cell = player->GetParentCell()) {
                cellID = cell->GetFormID();
                cell->ForEachReference([&](RE::TESObjectREFR* a_refr) {
                    if (a_refr && Persist::ForgetRolled(a_refr->GetFormID())) {
                        ++cleared;
                    }
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            }
        }
        logger::warn("debug menu: CLEARED {} rolled mark(s) in cell {:08X} at the player's request; "
                     "those actors and containers will roll again",
            cleared, cellID);

        std::scoped_lock lock{ g_lock };
        g_clearReport = std::format("Cleared {} mark(s) in cell {:08X}.", cleared, cellID);
    }

    void __stdcall RenderCoSave()
    {
        static bool s_confirmClear = false;

        SeparatorText("What the co-save holds");
        Text("Rolled actors and containers: %zu", Persist::RolledCount());
        Text("Rarity codes (by enchantment): %zu", Persist::EnchTierCount());
        Text("Affix enchantments (ours):    %zu", Persist::AffixEnchCount());
        Text("Item records:                 %zu", Persist::ItemRecordCount());
        Text("Vendor restock days:          %zu", Persist::VendorDayCount());

        const auto records = Records::CurrentStats();
        Text("Records rebuilt from old items this session: %llu  (could not be read: %llu)",
            static_cast<unsigned long long>(records.reconstructed),
            static_cast<unsigned long long>(records.unreadable));
        TextDisabled("Rarity codes are recomputed for 1.2.0 items the first time they are drawn;\n"
                     "an older item gets its record the first time its tooltip is shown.");

        SeparatorText("Orphaned enchantments");
        if (Button("Check")) {
            RunOnGameThread(CheckOrphans);
        }
        {
            std::scoped_lock lock{ g_lock };
            if (!g_orphanReport.empty()) {
                SameLine();
                TextWrapped("%s", g_orphanReport.c_str());
            }
        }

        SeparatorText("Rolled marks in this cell");
        TextWrapped("Makes every actor and container in the cell you are standing in eligible to "
                    "roll again. Items they already carry keep their affixes. This changes your save.");
        Checkbox("Yes, clear the marks for this cell", &s_confirmClear);
        BeginDisabled(!s_confirmClear);
        if (Button("Clear rolled marks")) {
            s_confirmClear = false;
            RunOnGameThread(ClearCellMarks);
        }
        EndDisabled();
        {
            std::scoped_lock lock{ g_lock };
            if (!g_clearReport.empty()) {
                TextDisabled("%s", g_clearReport.c_str());
            }
        }

        SeparatorText("Distribution this session");
        const auto stats = Distribute::CurrentStats();
        Text("Actors seen %llu, rolled %llu; items affixed %llu, rolled white %llu",
            static_cast<unsigned long long>(stats.actorsSeen),
            static_cast<unsigned long long>(stats.actorsRolled),
            static_cast<unsigned long long>(stats.itemsAffixed),
            static_cast<unsigned long long>(stats.rolledWhite));
        Text("Containers opened %llu, rolled %llu; marks forgotten on reset %llu",
            static_cast<unsigned long long>(stats.containersOpened),
            static_cast<unsigned long long>(stats.containersRolled),
            static_cast<unsigned long long>(stats.marksForgotten));
    }

    // ---- page: Log -----------------------------------------------------------

    void __stdcall RenderLog()
    {
        static constexpr const char* kLevels[]{ "trace", "debug", "info", "warn", "error" };
        static constexpr spdlog::level::level_enum kLevelValues[]{ spdlog::level::trace,
            spdlog::level::debug, spdlog::level::info, spdlog::level::warn, spdlog::level::err };

        int current = 2;
        for (int i = 0; i < static_cast<int>(std::size(kLevelValues)); ++i) {
            if (spdlog::get_level() == kLevelValues[i]) {
                current = i;
            }
        }
        if (Combo("Log level", &current, kLevels, static_cast<int>(std::size(kLevels)))) {
            const auto level = kLevelValues[std::clamp(current, 0, static_cast<int>(std::size(kLevelValues)) - 1)];
            spdlog::set_level(level);
            spdlog::flush_on(level);
            logger::info("debug menu: log level set to {}", kLevels[current]);
        }
        TextDisabled("Lasts until the game closes. \"debug\" shows each step of an item being rolled.");

        SeparatorText("Log file");
        static std::string s_path;
        if (s_path.empty()) {
            if (auto directory = SKSE::log::log_directory()) {
                s_path = roll::PathToUtf8(*directory / "DiabloInSkyrim.log");
            } else {
                s_path = "(the SKSE log folder could not be found)";
            }
        }
        TextWrapped("%s", s_path.c_str());
    }

    // ---- page: Integration ---------------------------------------------------

    // ---- orbs & drops ----------------------------------------------------------

    std::string g_orbReport;   // under g_lock

    void __stdcall RenderOrbs()
    {
        static int  s_orb = 0;
        static int  s_count = 5;
        static bool s_confirm = false;
        static bool s_confirmClean = false;
        static bool s_loaded = false;
        static roll::DropTuning s_tuning;
        static bool s_enabled = true;
        static int  s_mode = 1;   // ALL_INVENTORY, the shipped default
        static int  s_cleanup = 3;
        static bool s_notify = false;
        if (!s_loaded) {
            s_tuning = Config::Drops();
            s_enabled = Config::OrbDropsEnabled();
            s_mode = static_cast<int>(Config::EnemyGearMode());
            s_cleanup = Config::OrbCleanupDays();
            s_notify = Config::OrbDropNotify();
            s_loaded = true;
        }

        if (!Orbs::Available()) {
            TextColored(kBad, "DiabloInSkyrim.esp is not loaded: no orbs exist.");
        }

        SeparatorText("Spawn orbs");
        std::array<const char*, roll::kOrbCount> orbNames{};
        for (int i = 0; i < roll::kOrbCount; ++i) {
            orbNames[static_cast<std::size_t>(i)] = roll::OrbName(static_cast<roll::Orb>(i));
        }
        Combo("Orb##give", &s_orb, orbNames.data(), roll::kOrbCount);
        SliderInt("How many", &s_count, 1, 100);
        Checkbox("Yes, change my save##orbs", &s_confirm);
        BeginDisabled(!s_confirm || !Orbs::Available());
        if (Button("Put in my inventory")) {
            s_confirm = false;
            const auto orb = static_cast<roll::Orb>(std::clamp(s_orb, 0, roll::kOrbCount - 1));
            const int  count = s_count;
            RunOnGameThread([orb, count]() { Orbs::Give(orb, count); });
        }
        EndDisabled();
        TextDisabled("Arrives on the next game frame, which is when this menu closes.");

        SeparatorText("Drop quantity by difficulty");
        {
            static bool  s_auto = true;
            static float s_override = 1.0f;
            static bool  s_multLoaded = false;
            if (!s_multLoaded || !s_loaded) {
                s_auto = Config::DropMultiplierAuto();
                s_override = Config::DropMultiplierOverride();
                s_multLoaded = true;
            }
            static constexpr const char* kDifficulty[]{ "Novice", "Apprentice", "Adept", "Expert", "Master",
                "Legendary" };
            const int difficulty = std::clamp(Orbs::LastDifficulty(), 0, 5);
            Text("Active multiplier: x%.2f   Source: %s", Orbs::PreviewDropMultiplier(),
                Config::DropMultiplierAuto()
                    ? std::format("Auto ({}, +{:.2f} per level)", kDifficulty[difficulty],
                          Config::DropQuantityPerDifficulty()).c_str()
                    : "Override");
            Text("Last kill used x%.2f", Orbs::LastDropMultiplier());
            bool multChanged = Checkbox("Auto (follow the game's difficulty)", &s_auto);
            BeginDisabled(s_auto);
            multChanged |= SliderFloat("Override multiplier", &s_override, 1.0f, 3.0f, "%.2f");
            EndDisabled();
            if (multChanged) {
                const bool  autoMode = s_auto;
                const float value = s_override;
                RunOnGameThread([autoMode, value]() {
                    Config::SetDropMultiplierAuto(autoMode);
                    Config::SetDropMultiplierOverride(value);
                    Orbs::RefreshDifficulty();
                });
            }
            if (Button("Re-read difficulty")) {
                RunOnGameThread([]() { Orbs::RefreshDifficulty(); });
            }
            SameLine();
            TextDisabled("Orb counts only; gold and gear are unchanged. Save/Reset below include it.");
        }

        SeparatorText("Drops (live)");
        bool changed = false;
        changed |= Checkbox("Orbs drop from kills", &s_enabled);
        static constexpr const char* kModes[]{ "ALL_GROUND", "ALL_INVENTORY", "ORBS_GROUND", "ITEMS_INVENTORY" };
        changed |= Combo("Enemy gear", &s_mode, kModes, 4);
        changed |= SliderFloat("Global multiplier", &s_tuning.multiplier, 0.0f, 10.0f, "%.2f");
        changed |= SliderFloat("Chance a kill drops orbs", &s_tuning.chance, 0.0f, 1.0f, "%.2f");
        changed |= SliderFloat("Base count", &s_tuning.baseCount, 0.0f, 10.0f, "%.2f");
        changed |= SliderFloat("Count per enemy level", &s_tuning.countPerLevel, 0.0f, 0.5f, "%.3f");
        changed |= SliderFloat("Most per kill", &s_tuning.maxCount, 0.0f, 20.0f, "%.1f");
        for (int i = 0; i < roll::kOrbCount; ++i) {
            const auto label = std::format("Weight: {}", orbNames[static_cast<std::size_t>(i)]);
            changed |= SliderInt(label.c_str(), &s_tuning.weights[static_cast<std::size_t>(i)], 0, 1000);
        }
        changed |= SliderInt("Clean up ground orbs after (days, 0 = never)", &s_cleanup, 0, 30);
        changed |= Checkbox("Notify on drops", &s_notify);
        if (changed) {
            const auto tuning = s_tuning;
            const bool enabled = s_enabled;
            const auto mode = static_cast<Config::GearMode>(std::clamp(s_mode, 0, 3));
            const int  cleanup = s_cleanup;
            const bool notify = s_notify;
            RunOnGameThread([tuning, enabled, mode, cleanup, notify]() {
                Config::Drops() = tuning;
                Config::SetOrbDropsEnabled(enabled);
                Config::SetEnemyGearMode(mode);
                Config::SetOrbCleanupDays(cleanup);
                Config::SetOrbDropNotify(notify);
            });
        }
        if (Button("Save to DiabloLoot.ini")) {
            RunOnGameThread([]() {
                const bool ok = Config::SaveDrops();
                std::scoped_lock lock{ g_lock };
                g_orbReport = ok ? "Saved." : "Could not write DiabloLoot.ini.";
            });
        }
        SameLine();
        if (Button("Reset from the INI")) {
            s_loaded = false;
            RunOnGameThread([]() { Config::ResetDrops(); });
        }
        SameLine();
        if (Button("Force the next kill to drop")) {
            Orbs::ForceNextDrop();
        }
        TextDisabled("ALL_GROUND and ITEMS_INVENTORY move enemy gear/gold to the ground; off by default.");

        SeparatorText("This session");
        const auto stats = Orbs::CurrentStats();
        Text("Deaths handled %llu, kills that dropped %llu, orbs dropped %llu, orbs used %llu",
            static_cast<unsigned long long>(stats.deaths), static_cast<unsigned long long>(stats.dropped),
            static_cast<unsigned long long>(stats.orbs), static_cast<unsigned long long>(stats.used));
        Text("Ground orb stacks tracked: %zu   Corpses marked: %zu", Persist::GroundOrbCount(),
            Persist::DroppedCount());
        Checkbox("Yes, remove every tracked ground orb", &s_confirmClean);
        BeginDisabled(!s_confirmClean);
        if (Button("Clean up now")) {
            s_confirmClean = false;
            RunOnGameThread([]() {
                const auto n = Orbs::Cleanup(true);
                logger::warn("debug menu: REMOVED {} ground orb stack(s) at the player's request", n);
                std::scoped_lock lock{ g_lock };
                g_orbReport = std::format("Removed {} ground orb stack(s).", n);
            });
        }
        EndDisabled();
        std::scoped_lock lock{ g_lock };
        if (!g_orbReport.empty()) {
            TextDisabled("%s", g_orbReport.c_str());
        }
    }

    void StatusLine(const char* a_what, bool a_ok, const char* a_yes, const char* a_no)
    {
        Text("%s:", a_what);
        SameLine();
        TextColored(a_ok ? ImVec4{ 0.45f, 0.85f, 0.45f, 1.0f } : ImVec4{ 0.90f, 0.55f, 0.25f, 1.0f },
            "%s", a_ok ? a_yes : a_no);
    }

    void __stdcall RenderIntegration()
    {
        Text("%s %u.%u.%u  (SKSE plugin DiabloInSkyrim)", Poerim::kDisplayName,
            static_cast<unsigned>(Poerim::kMajor), static_cast<unsigned>(Poerim::kMinor),
            static_cast<unsigned>(Poerim::kPatch));
        Text("Skyrim runtime %s", REL::Module::get().version().string(".").c_str());

        SeparatorText("ARPG POV (required)");
        StatusLine("ARPGPOV.dll", ::GetModuleHandleW(L"ARPGPOV.dll") != nullptr, "loaded",
            "NOT LOADED -- POErim is built to be played with ARPG POV");
        StatusLine("Using DiabloLootAPI v2 (rarity colours)", LootApi::V2Calls() > 0,
            std::format("yes ({} calls)", LootApi::V2Calls()).c_str(), "no calls yet");
        {
            std::uint32_t base = 0;
            std::uint32_t rgba = 0;
            if (LootApi::LastAnswer(base, rgba)) {
                auto* object = RE::TESForm::LookupByID<RE::TESBoundObject>(base);
                Text("Last colour handed over:");
                SameLine();
                TextColored(FromRgba(rgba), "%08X  %s", rgba, object && object->GetName() ? object->GetName() : "");
            }
        }

        SeparatorText("Grid Inventory");
        const bool grid = ::GetModuleHandleW(L"GridInventory.dll") != nullptr;
        StatusLine("GridInventory.dll", grid, "loaded", "not loaded");
        StatusLine("Host announced its services", GridTint::HostServicesKnown(), "yes", "no");
        StatusLine("Tint table delivered", GridTint::Registered(), "yes", "no");
        StatusLine("Orb applier delivered", GridTint::ApplierDelivered(), "yes",
            "no -- orbs can't be used from the grid (needs the POErim Grid Inventory build)");
        StatusLine("Currency pouch", GridTint::PouchActive(), "active",
            GridTint::GridReportSeen() ? "off (GridInventory_ui.ini [CurrencyPouch])" : "unknown (no report from the grid yet)");

        SeparatorText("Orbs");
        StatusLine("DiabloInSkyrim.esp orbs", Orbs::Available(), "all found",
            "MISSING -- enable DiabloInSkyrim.esp");
        if (BeginTable("orbs", 4)) {
            TableSetupColumn("Orb");
            TableSetupColumn("Model path");
            TableSetupColumn("Grid size");
            TableSetupColumn("Icon source");
            TableHeadersRow();
            for (int i = 0; i < roll::kOrbCount; ++i) {
                const auto orb = static_cast<roll::Orb>(i);
                auto*      form = Orbs::Form(orb);
                TableNextRow();
                TableNextColumn();
                Text("%s", roll::OrbName(orb));
                TableNextColumn();
                const auto model = Orbs::ModelOf(orb);
                TextColored(Orbs::IsGemMesh(model) ? kGood : kBad, "%s", model.empty() ? "(none)" : model.c_str());
                GridTint::CurrencyRow row{};
                const bool known = form && GridTint::GridCurrencyRow(form->GetFormID(), row);
                TableNextColumn();
                if (known) {
                    TextColored(row.w == 1 && row.h == 1 ? kGood : kBad, "%ux%u", row.w, row.h);
                } else {
                    TextColored(kBad, "? (grid has not reported)");
                }
                TableNextColumn();
                static constexpr const char* kSources[]{ "none", "PNG", "3D capture", "category drawing" };
                if (known) {
                    TextColored(row.iconSource == 1 ? kGood : kBad, "%s", kSources[std::min<int>(row.iconSource, 3)]);
                } else {
                    TextColored(kBad, "?");
                }
            }
            EndTable();
        }
        TextDisabled("Red: not a vanilla gem mesh, not 1x1, or not its own PNG.");
        TextDisabled("Delivered means the message arrived. Whether each table was accepted -- tint,\n"
                     "tooltip, price, sockets -- is in Grid Inventory's own log.");

        SeparatorText("Other");
        StatusLine("Socketing", Sockets::Connected(), "connected", "not found");
        StatusLine("Affix table", Records::Table() && !Records::Table()->Empty(),
            "loaded", "NOT LOADED -- nothing can roll");
        if (const auto* table = Records::Table()) {
            Text("%zu affixes, %zu tier rows", table->Affixes().size(), table->TierRowCount());
        }
        StatusLine("Distribution", Distribute::Enabled(), "on", "off (Distribution=0 in DiabloLoot.ini)");
        StatusLine("Replace vanilla enchantments", Config::ReplaceVanillaEnchants(), "on", "off");

        SeparatorText("Affix table");
        static std::string s_reload;
        if (Button("Reload the CSVs now")) {
            RunOnGameThread([]() {
                std::string report;
                ReloadAffixTable(report);
                DebugMenu::Forget();
                std::scoped_lock lock{ g_lock };
                s_reload = std::move(report);
            });
        }
        SameLine();
        TextDisabled("Takes the table lock; items already made keep their enchantments.");
        {
            std::scoped_lock lock{ g_lock };
            if (!s_reload.empty()) {
                TextWrapped("%s", s_reload.c_str());
            }
        }

        SeparatorText("Vanilla enchantments that will be replaced");
        static std::string s_vanilla;
        if (Button("List them (writes the log)")) {
            RunOnGameThread([]() {
                std::string report = ListReplaceableEnchantments();
                std::scoped_lock lock{ g_lock };
                s_vanilla = std::move(report);
            });
        }
        {
            std::scoped_lock lock{ g_lock };
            if (!s_vanilla.empty()) {
                TextWrapped("%s", s_vanilla.c_str());
            }
        }
    }
}

void DebugMenu::Register()
{
    if (!SKSEMenuFramework::IsInstalled()) {
        logger::info("debug menu: SKSE Menu Framework is not installed; the {} is not available. "
                     "Everything else works without it.",
            kSection);
        return;
    }
    SKSEMenuFramework::SetSection(kSection);
    SKSEMenuFramework::AddSectionItem("Integration", RenderIntegration);
    SKSEMenuFramework::AddSectionItem("Inspect", RenderInspect);
    SKSEMenuFramework::AddSectionItem("Craft simulator", RenderCraft);
    SKSEMenuFramework::AddSectionItem("Craft on items", RenderForge);
    SKSEMenuFramework::AddSectionItem("Orbs & drops", RenderOrbs);
    SKSEMenuFramework::AddSectionItem("Co-save", RenderCoSave);
    SKSEMenuFramework::AddSectionItem("Log", RenderLog);
    g_available.store(true);
    logger::info("debug menu: registered \"{}\" with SKSE Menu Framework", kSection);
}

bool DebugMenu::Available()
{
    return g_available.load();
}

void DebugMenu::Forget()
{
    std::scoped_lock lock{ g_lock };
    g_inventory.clear();
    g_inventoryRead = false;
    g_orphanReport.clear();
    g_clearReport.clear();
    g_forgeReport.clear();
    g_forgeSelect = {};
}
