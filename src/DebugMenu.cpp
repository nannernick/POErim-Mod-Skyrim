// =============================================================================
//  The debug menu. See DebugMenu.h.
// =============================================================================

#include "PCH.h"

#include "DebugMenu.h"

#include "Apply.h"
#include "Distribute.h"
#include "GridTint.h"
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
#include <format>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace
{
    using namespace ImGuiMCP;

    constexpr const char* kSection = "POErim Debug Menu";

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

    const ImVec4 kBandColour[roll::kBandCount]{
        { 0.80f, 0.80f, 0.80f, 1.0f },   // white
        { 0.41f, 0.61f, 1.00f, 1.0f },   // blue    -- the same five GridTint offers the grid
        { 0.94f, 0.84f, 0.39f, 1.0f },   // yellow
        { 0.73f, 0.47f, 0.94f, 1.0f },   // purple
        { 1.00f, 0.55f, 0.24f, 1.0f },   // orange
        { 0.90f, 0.24f, 0.27f, 1.0f },   // red
    };

    const ImVec4& BandColour(int a_band)
    {
        return kBandColour[std::clamp(a_band, 0, roll::kBandCount - 1)];
    }

    // ---- one item, as the inspector and the simulator see it ----------------

    struct Item
    {
        std::string   name;
        RE::FormID    base{ 0 };
        std::uint32_t slots{ roll::kNone };
        bool          worn{ false };
        Records::View view;
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
            for (auto& [object, data] : inventory) {
                const auto& entry = data.second;
                if (!object || !entry || !entry->extraLists) {
                    continue;
                }
                for (auto* xList : *entry->extraLists) {
                    Item item;
                    if (!xList || !Records::Lookup(xList, item.view)) {
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
        const int   band = view.band ? view.band : static_cast<int>(roll::BandOf(view.rolled.tier));

        TextColored(BandColour(band), "%s%s", a_item.name.c_str(), a_item.worn ? "  (worn)" : "");
        Text("Band: %s   Tier points: %d of %d", roll::BandName(static_cast<roll::Band>(band)),
            view.rolled.tier, roll::kMaxTier);
        Text("Item level: %u%s", static_cast<unsigned>(view.record.itemLevel),
            (view.record.flags & roll::kRecordLegacy) ? "  (estimated: rebuilt from the enchantment)" : "");
        Text("Base form: %08X   Created enchantment: %08X", a_item.base, view.enchantment);
        Text("Slots: %s   Prefixes %d/%d  Suffixes %d/%d  Implicit %s",
            roll::SlotsToString(a_item.slots).c_str(), view.rolled.Count(roll::AffixKind::kPrefix),
            roll::MaxPrefixes(), view.rolled.Count(roll::AffixKind::kSuffix), roll::MaxSuffixes(),
            view.rolled.HasImplicit() ? "yes" : "no");
        Text("Co-save: band %s, ours %s, record %s%s",
            Persist::EnchTier(view.enchantment) ? "yes" : "no",
            Persist::IsAffixEnch(view.enchantment) ? "yes" : "no",
            view.record.affixes.empty() ? "no" : "yes",
            (view.record.flags & roll::kRecordCrafted) ? ", crafted" : "");
        if (!view.complete) {
            TextColored(BandColour(5), "Some affixes on this item are no longer in the affix table; "
                                       "it cannot be crafted on.");
        }

        Spacing();
        for (const auto& entry : view.record.affixes) {
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
    }

    // ---- page: Inspect -------------------------------------------------------

    void __stdcall RenderInspect()
    {
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
        if (g_inventoryRead && g_inventory.empty()) {
            TextDisabled("No rolled items found.");
        }
        for (std::size_t i = 0; i < g_inventory.size(); ++i) {
            const auto& item = g_inventory[i];
            const auto  label = std::format("{}{}##item{}", item.name, item.worn ? "  (worn)" : "", i);
            if (CollapsingHeader(label.c_str())) {
                DrawItem(item);
            }
        }

        Spacing();
        TextDisabled("Editing affixes, spawning items and applying orbs arrive with the crafting build.");
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
        std::array<int, roll::kBandCount> bands{};
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
            g_sim.bands[static_cast<std::size_t>(roll::BandOf(item.tier))]++;
            affixes += static_cast<long long>(item.affixes.size());
        }
        if (g_sim.worked > 0) {
            g_sim.meanAffixes = static_cast<double>(affixes) / g_sim.worked;
        }
        g_sim.ran = true;
    }

    void __stdcall RenderCraft()
    {
        static int s_source = 0;      // 0 = plain item, 1 = hovered, 2+ = inventory snapshot
        static int s_slot = 0;
        static int s_level = 20;
        static int s_orb = 0;
        static int s_samples = 1000;

        TextWrapped("Uses one orb on a copy of an item, many times over, and shows what colour "
                    "the copies come out. Nothing in the game is changed and no orb is spent.");

        SeparatorText("Item");
        Item hovered;
        const bool haveHovered = HoveredItem(hovered);

        std::vector<Item> inventory;
        {
            std::scoped_lock lock{ g_lock };
            inventory = g_inventory;
        }

        std::vector<std::string> labels{ "A plain item" };
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
            sourceName = std::format("plain {}, item level {}", names[static_cast<std::size_t>(s_slot)], s_level);
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
                TextDisabled("That item has affixes the table no longer knows; it cannot be crafted on.");
                usable = false;
            } else {
                start = item->view.rolled;
                ctx.itemLevel = item->view.record.itemLevel;
                ctx.itemSlots = item->slots;
                sourceName = std::format("{}, item level {}, {} affix(es)", item->name, ctx.itemLevel,
                    start.affixes.size());
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
            TextColored(BandColour(5), "Refused: %s", g_sim.refusal.c_str());
            if (g_sim.worked == 0) {
                return;
            }
        }
        Text("Worked %d of %d time(s); mean affixes afterwards %.2f", g_sim.worked, g_sim.samples,
            g_sim.meanAffixes);
        for (int b = 0; b < roll::kBandCount; ++b) {
            const int   count = g_sim.bands[static_cast<std::size_t>(b)];
            const float share = g_sim.worked > 0 ? static_cast<float>(count) / static_cast<float>(g_sim.worked) : 0.0f;
            const auto  overlay = std::format("{}  {}  ({:.1f}%)", roll::BandName(static_cast<roll::Band>(b)),
                count, 100.0f * share);
            ProgressBar(share, ImVec2(-1.0f, 0.0f), overlay.c_str());
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
        Text("Enchantment bands:            %zu", Persist::EnchTierCount());
        Text("Affix enchantments (ours):    %zu", Persist::AffixEnchCount());
        Text("Item records:                 %zu", Persist::ItemRecordCount());
        Text("Vendor restock days:          %zu", Persist::VendorDayCount());

        const auto records = Records::CurrentStats();
        Text("Records rebuilt from old items this session: %llu  (could not be read: %llu)",
            static_cast<unsigned long long>(records.reconstructed),
            static_cast<unsigned long long>(records.unreadable));
        TextDisabled("Item records can be fewer than bands: an older item gets its record the first\n"
                     "time its tooltip is shown or it is inspected here.");

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

        SeparatorText("Grid Inventory");
        const bool grid = ::GetModuleHandleW(L"GridInventory.dll") != nullptr;
        StatusLine("GridInventory.dll", grid, "loaded", "not loaded");
        StatusLine("Host announced its services", GridTint::HostServicesKnown(), "yes", "no");
        StatusLine("Tint table delivered", GridTint::Registered(), "yes", "no");
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
}
