// =============================================================================
//  Orbs. See Orbs.h.
// =============================================================================

#include "PCH.h"

#include "Orbs.h"

#include "Apply.h"
#include "Config.h"
#include "Crafting.h"
#include "Persist.h"
#include "Records.h"
#include "Sockets.h"

#include "roll/Drops.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <deque>
#include <format>
#include <mutex>
#include <random>
#include <vector>

namespace
{
    constexpr const char*   kPlugin = "DiabloInSkyrim.esp";
    constexpr RE::FormID    kKeywordLocal = 0x800;
    constexpr RE::FormID    kFirstOrbLocal = 0x801;

    std::array<RE::TESObjectMISC*, roll::kOrbCount>          g_forms{};
    std::array<std::atomic<std::uint32_t>, roll::kOrbCount>  g_ids{};
    std::atomic<bool>                                        g_available{ false };

    std::mutex             g_queueLock;
    std::deque<RE::FormID> g_queue;
    bool                   g_draining = false;   // under g_queueLock
    std::atomic<bool>      g_forceNext{ false };

    std::mutex   g_statsLock;
    Orbs::Stats  g_stats;

    roll::Rng& Rng()
    {
        static thread_local roll::Rng rng{ std::random_device{}() };
        return rng;
    }

    constexpr std::uint32_t Rgba(std::uint8_t a_r, std::uint8_t a_g, std::uint8_t a_b)
    {
        return (0xFFu << 24) | (std::uint32_t{ a_b } << 16) | (std::uint32_t{ a_g } << 8) | a_r;
    }

    constexpr std::uint32_t kColors[roll::kOrbCount]{
        Rgba(90, 160, 245),    // transmute
        Rgba(75, 120, 235),    // augment
        Rgba(245, 170, 60),    // alchemy
        Rgba(230, 195, 70),    // exalt
        Rgba(205, 160, 75),    // chaos
        Rgba(200, 200, 205),   // scour
        Rgba(250, 215, 235),   // divine
        Rgba(215, 60, 40),     // vaal
        Rgba(120, 150, 235),   // regal (blue half; the sprite is blue and gold)
    };

    std::uint32_t Today()
    {
        const auto* calendar = RE::Calendar::GetSingleton();
        return calendar ? static_cast<std::uint32_t>(calendar->GetDaysPassed()) : 0u;
    }

    RE::BGSKeyword* FindKeyword(std::string_view a_editorID)
    {
        auto* handler = RE::TESDataHandler::GetSingleton();
        if (!handler) {
            return nullptr;
        }
        for (auto* keyword : handler->GetFormArray<RE::BGSKeyword>()) {
            if (keyword && a_editorID == keyword->GetFormEditorID()) {
                return keyword;
            }
        }
        return nullptr;
    }

    // ★ONE MESH FOR EVERY ORB (bug fix, 2.1.0).
    //
    // 1.2.0 handed each orb a DIFFERENT mesh, round-robin over every Skyrim.esm
    // MISC carrying VendorItemGem, as the load order had overridden it. Grid
    // Inventory sizes an item it has no entry for by the tuned sibling that
    // shares its nif (main.cpp DefFor, model-level defs) and captures its icon
    // from that nif -- so whichever orb landed on a mesh some other, bigger
    // item shared came out 2x2 with that item's picture (Nick: "the ones that
    // replace chests"). The fix here is one small vanilla cut gem for all of
    // them; the grid side pins every currency to 1x1 and its own PNG.
    //
    // The pick: the lowest-FormID Skyrim.esm gem whose model sits under
    // clutter\gems\, so a load-order override that moved a gem's model to a
    // container or a chest cannot be what the orbs inherit.
    std::string g_orbModel;

    std::string Lower(std::string a_text)
    {
        for (auto& ch : a_text) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (ch == '/') {
                ch = '\\';
            }
        }
        return a_text;
    }

    std::string PickGemModel(RE::BGSKeyword* a_gemKeyword)
    {
        auto*       handler = RE::TESDataHandler::GetSingleton();
        std::string fallback;
        if (handler && a_gemKeyword) {
            RE::FormID best = 0xFFFFFFFF;
            std::string bestModel;
            for (auto* misc : handler->GetFormArray<RE::TESObjectMISC>()) {
                if (!misc || misc->GetFormID() >= 0x01000000 ||
                    !static_cast<RE::BGSKeywordForm*>(misc)->HasKeyword(a_gemKeyword)) {
                    continue;
                }
                const char* model = static_cast<RE::TESModel*>(misc)->GetModel();
                if (!model || !*model) {
                    continue;
                }
                const auto lower = Lower(model);
                if (lower.find("clutter\\gems\\") != std::string::npos && misc->GetFormID() < best) {
                    best = misc->GetFormID();
                    bestModel = model;
                }
                if (fallback.empty()) {
                    fallback = model;
                }
            }
            if (!bestModel.empty()) {
                return bestModel;
            }
        }
        if (!fallback.empty()) {
            logger::warn("orbs: no vanilla gem under clutter\\gems\\; using '{}'", fallback);
            return fallback;
        }
        if (auto* gold = RE::TESForm::LookupByID<RE::TESObjectMISC>(0x0000000F)) {
            if (const char* model = static_cast<RE::TESModel*>(gold)->GetModel(); model && *model) {
                logger::warn("orbs: no gem mesh at all; using gold's '{}'", model);
                return model;
            }
        }
        return {};
    }

    // ★THE DIFFICULTY, READ ON THE GAME THREAD (HandleDeath runs from the
    // death drain's task). 0 Novice .. 5 Legendary.
    int CurrentDifficulty()
    {
        const auto* player = RE::PlayerCharacter::GetSingleton();
        return player ? std::clamp(static_cast<int>(player->GetGameStatsData().difficulty), 0, 5) : 0;
    }

    std::atomic<float> g_lastDropMult{ 1.0f };
    std::atomic<int>   g_lastDifficulty{ 0 };

    float ActiveDropMultiplier()
    {
        const int difficulty = CurrentDifficulty();
        g_lastDifficulty.store(difficulty);
        const float mult = Config::DropMultiplierAuto()
                               ? roll::DifficultyMultiplier(difficulty, Config::DropQuantityPerDifficulty())
                               : (std::max)(1.0f, Config::DropMultiplierOverride());
        g_lastDropMult.store(mult);
        return mult;
    }

    // ★NEW LOOT NEVER CARRIES A VANILLA ENCHANTMENT (2.1.0): the corpse's
    // loose (non-worn) enchanted generic gear becomes its plain template, in
    // the same per-frame death task (Apply::SwapVanillaLoose).
    void SwapVanillaGear(RE::Actor* a_actor)
    {
        if (Config::ReplaceVanillaEnchants()) {
            if (const int n = Apply::SwapVanillaLoose(a_actor); n > 0) {
                logger::debug("orbs: {:08X} -- {} vanilla-enchanted item(s) became their plain template",
                    a_actor->GetFormID(), n);
            }
        }
    }

    // ---- deaths -------------------------------------------------------------

    void Notify(const std::string& a_text)
    {
        if (Config::OrbDropNotify()) {
            RE::SendHUDMessage::ShowHUDMessage(a_text.c_str());
        }
    }

    // A small ring around the corpse, so a stack of orbs is several labels.
    RE::NiPoint3 Scatter(const RE::NiPoint3& a_at, int a_index, int a_count)
    {
        const float angle = 6.2831853f * static_cast<float>(a_index) / static_cast<float>((std::max)(1, a_count));
        const float radius = 30.0f + 10.0f * static_cast<float>(a_index % 2);
        return { a_at.x + radius * std::cos(angle), a_at.y + radius * std::sin(angle), a_at.z + 10.0f };
    }

    RE::TESObjectREFR* PlaceStack(RE::Actor* a_actor, RE::TESBoundObject* a_object, int a_count,
        const RE::NiPoint3& a_at)
    {
        auto placed = a_actor->PlaceObjectAtMe(a_object, false);
        if (!placed) {
            return nullptr;
        }
        if (a_count > 1) {
            placed->extraList.SetCount(static_cast<std::uint16_t>((std::min)(a_count, 65535)));
        }
        placed->SetPosition(a_at);
        return placed.get();
    }

    // Weapons, armour and gold out of the corpse onto the ground. GearMode
    // ALL_GROUND / ITEMS_INVENTORY only; off by default.
    void MoveGear(RE::Actor* a_actor, bool a_gear, bool a_gold)
    {
        auto* gold = RE::TESForm::LookupByID<RE::TESBoundObject>(0x0000000F);
        auto  inventory = a_actor->GetInventory([&](RE::TESBoundObject& a_object) {
            if (a_gold && &a_object == gold) {
                return true;
            }
            return a_gear && (a_object.Is(RE::FormType::Weapon) || a_object.Is(RE::FormType::Armor)) &&
                   a_object.GetPlayable();
        });
        for (auto& [object, data] : inventory) {
            const int count = data.first;
            if (!object || count <= 0) {
                continue;
            }
            if (object == gold) {
                if (Config::BundleGold()) {
                    // ★ONE PILE: a single reference carrying the whole count.
                    a_actor->RemoveItem(object, count, RE::ITEM_REMOVE_REASON::kDropping, nullptr, nullptr);
                } else {
                    for (int i = 0; i < count; ++i) {
                        a_actor->RemoveItem(object, 1, RE::ITEM_REMOVE_REASON::kDropping, nullptr, nullptr);
                    }
                }
                continue;
            }
            // Sub-stacks first, each with its own list (enchantments, temper);
            // quest items stay where the quest put them.
            int listed = 0;
            if (const auto& entry = data.second; entry && entry->extraLists) {
                std::vector<RE::ExtraDataList*> lists;   // copied: RemoveItem edits the list
                for (auto* xList : *entry->extraLists) {
                    lists.push_back(xList);
                }
                for (auto* xList : lists) {
                    if (!xList || xList->HasType(RE::ExtraDataType::kAliasInstanceArray)) {
                        if (xList) {
                            listed += xList->GetCount();
                        }
                        continue;
                    }
                    const int n = xList->GetCount();
                    listed += n;
                    a_actor->RemoveItem(object, n, RE::ITEM_REMOVE_REASON::kDropping, xList, nullptr);
                }
            }
            if (count > listed) {
                a_actor->RemoveItem(object, count - listed, RE::ITEM_REMOVE_REASON::kDropping, nullptr, nullptr);
            }
        }
    }

    void HandleDeath(RE::FormID a_actorID)
    {
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_actorID);
        if (!actor || actor->IsPlayerRef() || !actor->IsDead()) {
            return;
        }
        // ★A CORPSE WHOSE CELL IS NOT ATTACHED IS NOT TOUCHED (2.1.1). Its
        // inventory and its 3D may be going away with the cell, and a write
        // there is the null-pointer write the crash reports show. Nothing is
        // marked: there is no second death event, so this corpse simply
        // drops nothing, which is the safe side of the trade.
        {
            const auto* cell = actor->GetParentCell();
            if (!cell || !cell->IsAttached()) {
                if (Config::DebugLoadLog()) {
                    logger::info("orbs: {:08X} skipped -- its cell is not attached (unloading or not loaded)",
                        a_actorID);
                }
                return;
            }
        }
        // Followers, summons and commanded actors drop nothing: they are not loot.
        if (actor->IsPlayerTeammate() || actor->IsSummoned() || actor->IsCommandedActor()) {
            return;
        }
        if (!Persist::MarkDropped(a_actorID)) {
            return;   // already rolled
        }
        {
            std::scoped_lock lock{ g_statsLock };
            ++g_stats.deaths;
        }

        SwapVanillaGear(actor);

        const auto mode = Config::EnemyGearMode();
        const bool gearOut = mode == Config::GearMode::kAllGround;
        const bool goldOut = mode == Config::GearMode::kAllGround || mode == Config::GearMode::kItemsInventory;
        if ((gearOut || goldOut) && Apply::CanDropFrom(actor)) {
            MoveGear(actor, gearOut, goldOut);
        }

        if (!Config::OrbDropsEnabled() || !g_available.load()) {
            return;
        }
        auto tuning = Config::Drops();
        if (g_forceNext.exchange(false)) {
            tuning.chance = 1.0f;
            tuning.baseCount = (std::max)(tuning.baseCount, 3.0f);
        }
        const auto drop = roll::RollOrbDrop(static_cast<int>(actor->GetLevel()), tuning, Rng(), 1.0f,
            ActiveDropMultiplier());
        if (drop.total == 0) {
            return;
        }

        const bool inCorpse = Config::OrbsInCorpse() || mode == Config::GearMode::kAllInventory ||
                              !Apply::CanDropFrom(actor);
        const auto at = actor->GetPosition();
        const auto day = Today();
        int        kinds = 0;
        for (const int count : drop.counts) {
            kinds += count > 0 ? 1 : 0;
        }
        int         placed = 0;
        std::string names;
        for (int i = 0; i < roll::kOrbCount; ++i) {
            const int count = drop.counts[static_cast<std::size_t>(i)];
            auto*     form = g_forms[static_cast<std::size_t>(i)];
            if (count <= 0 || !form) {
                continue;
            }
            if (inCorpse) {
                actor->AddObjectToContainer(form, nullptr, count, nullptr);
            } else if (auto* ref = PlaceStack(actor, form, count, Scatter(at, placed, kinds))) {
                Persist::NoteGroundOrb(ref->GetFormID(), day);
            }
            ++placed;
            names += std::format("{}{}{}", names.empty() ? "" : ", ",
                count > 1 ? std::format("{}x ", count) : std::string{}, roll::OrbName(static_cast<roll::Orb>(i)));
        }
        {
            std::scoped_lock lock{ g_statsLock };
            ++g_stats.dropped;
            g_stats.orbs += static_cast<std::uint64_t>(drop.total);
        }
        logger::debug("orbs: {:08X} (level {}) dropped {}{}", a_actorID, actor->GetLevel(), names,
            inCorpse ? " into the corpse" : "");
        Notify(names);
    }

    void Drain()
    {
        RE::FormID next = 0;
        {
            std::scoped_lock lock{ g_queueLock };
            if (g_queue.empty()) {
                g_draining = false;
                return;
            }
            // ★THE WORLD IS CHANGING: STOP, KEEP THE QUEUE (2.1.1). Not a
            // re-queue -- a task added from a task can run in the same drain,
            // and a loop that waits that way never yields the frame. Resume()
            // picks it up when the load screen goes down.
            if (Apply::WorldBusy()) {
                g_draining = false;
                if (Config::DebugLoadLog()) {
                    logger::info("orbs: {} death(s) held -- the world is loading", g_queue.size());
                }
                return;
            }
            next = g_queue.front();
            g_queue.pop_front();
        }
        HandleDeath(next);
        // ★ONE PER FRAME: the next one is a new task, which runs next frame.
        SKSE::GetTaskInterface()->AddTask([]() { Drain(); });
    }

    class DeathSink : public RE::BSTEventSink<RE::TESDeathEvent>
    {
    public:
        static DeathSink* GetSingleton()
        {
            static DeathSink singleton;
            return &singleton;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent* a_event,
            RE::BSTEventSource<RE::TESDeathEvent>*) override
        {
            // ★dead == true only: the event fires once as the actor starts to
            // die and once when it is dead. No inventory is touched here.
            if (!a_event || !a_event->dead || !a_event->actorDying) {
                return RE::BSEventNotifyControl::kContinue;
            }
            const auto id = a_event->actorDying->GetFormID();
            bool       start = false;
            {
                std::scoped_lock lock{ g_queueLock };
                g_queue.push_back(id);
                start = !g_draining;
                g_draining = true;
            }
            if (start) {
                SKSE::GetTaskInterface()->AddTask([]() { Drain(); });
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // The same rules Crafting applies, asked without changing anything.
    const char* CheckUnit(roll::Orb a_orb, RE::TESBoundObject* a_object, const RE::ExtraDataList* a_xl)
    {
        const Records::ReadGuard guard{ Records::TableLock() };
        const auto* table = Records::Table();
        if (!table) {
            return "The affix table has not loaded";
        }
        const char* reason = nullptr;
        if (!Crafting::Craftable(a_object, reason)) {
            return reason;
        }
        if (a_xl && a_xl->HasType(RE::ExtraDataType::kAliasInstanceArray)) {
            return "Quest items can't be crafted";
        }
        if (Sockets::RowOf(a_object->GetFormID(), a_xl)) {
            return "Socketed items can't be crafted";
        }

        roll::RolledItem  rolled;
        roll::RollContext ctx;
        ctx.itemSlots = Apply::SlotsOf(a_object);
        const auto* xEnch = a_xl ? a_xl->GetByType<RE::ExtraEnchantment>() : nullptr;
        const bool  ours = xEnch && xEnch->enchantment && Persist::IsAffixEnch(xEnch->enchantment->GetFormID());
        if (ours) {
            Records::View view;
            if (!Records::Lookup(a_xl, view)) {
                return "Too old to read: can't be crafted";
            }
            if (!view.complete) {
                return "An old item whose mods are gone from the table";
            }
            rolled = view.rolled;
            ctx.itemLevel = view.record.itemLevel;
        } else {
            const auto kind = Apply::ClassifyEnchanted(a_object, a_xl);
            if (kind == Apply::VanillaKind::kRefused) {
                return Apply::IsGeneric(a_object) ? "Items with an enchantment of their own can't be crafted"
                                                  : "Unique items can't be crafted";
            }
            if (kind != Apply::VanillaKind::kNone && !Config::ReplaceVanillaEnchants()) {
                return "Items with an enchantment of their own can't be crafted";
            }
            if (kind == Apply::VanillaKind::kNone && !Apply::IsGeneric(a_object)) {
                return "Unique items can't be crafted";
            }
            if (a_xl && kind == Apply::VanillaKind::kRecordEnchanted &&
                (a_xl->HasType(RE::ExtraDataType::kWorn) || a_xl->HasType(RE::ExtraDataType::kWornLeft))) {
                return "Unequip it first";
            }
            const auto* player = RE::PlayerCharacter::GetSingleton();
            ctx.itemLevel = player ? (std::max)(1, static_cast<int>(player->GetLevel())) : 1;
            // ★WHAT IT WILL BE when the orb lands: a normal item with its base
            // implicit. A fixed seed -- this is a question, not a roll.
            roll::Tuning base;
            for (auto& band : base.bands) {
                band.countWeights.fill(0);
                band.countWeights[0] = 1;
            }
            roll::Rng probe{ 12345u };
            rolled = roll::Roll(*table, ctx, base, probe);
        }
        const roll::Tuning tuning;
        const auto         verdict = roll::CanCraft(a_orb, rolled, ctx, *table, tuning);
        return verdict == roll::CraftVerdict::kOk ? nullptr : roll::Reason(verdict);
    }

    bool PlayerHas(RE::PlayerCharacter* a_player, RE::TESObjectMISC* a_form)
    {
        auto inventory = a_player->GetInventory([&](RE::TESBoundObject& a_object) {
            return &a_object == a_form;
        });
        for (const auto& [object, data] : inventory) {
            if (data.first > 0) {
                return true;
            }
        }
        return false;
    }
}

void Orbs::Init()
{
    auto* handler = RE::TESDataHandler::GetSingleton();
    if (!handler) {
        return;
    }
    if (!handler->LookupModByName(kPlugin)) {
        logger::error("orbs: {} is not in the load order -- no orbs will drop and none can be "
                      "used. Enable it in your mod manager.",
            kPlugin);
        return;
    }
    auto* ownKeyword = handler->LookupForm<RE::BGSKeyword>(kKeywordLocal, kPlugin);
    auto* gemKeyword = FindKeyword("VendorItemGem");
    g_orbModel = PickGemModel(gemKeyword);

    int found = 0;
    for (int i = 0; i < roll::kOrbCount; ++i) {
        auto* misc = handler->LookupForm<RE::TESObjectMISC>(kFirstOrbLocal + static_cast<RE::FormID>(i), kPlugin);
        g_forms[static_cast<std::size_t>(i)] = misc;
        g_ids[static_cast<std::size_t>(i)].store(misc ? misc->GetFormID() : 0u);
        if (!misc) {
            logger::error("orbs: {} has no {} at {:03X}", kPlugin, roll::OrbName(static_cast<roll::Orb>(i)),
                kFirstOrbLocal + i);
            continue;
        }
        ++found;
        // ★THE KEYWORD ARPG POV LOOKS FOR. Its loot filter shows a MISC when it
        // carries VendorItemGem; that is what labels the orb and lets M1 take it.
        auto* keywords = static_cast<RE::BGSKeywordForm*>(misc);
        if (gemKeyword && !keywords->HasKeyword(gemKeyword)) {
            keywords->AddKeyword(gemKeyword);
        }
        if (ownKeyword && !keywords->HasKeyword(ownKeyword)) {
            keywords->AddKeyword(ownKeyword);
        }
        // ★EVERY ORB, THE SAME MESH, ALWAYS SET: the ESP carries no MODL, and
        // a mesh an earlier session or another mod left on the form is not
        // trusted (see PickGemModel).
        auto* model = static_cast<RE::TESModel*>(misc);
        if (!g_orbModel.empty()) {
            model->SetModel(g_orbModel.c_str());
        }
        logger::info("orbs: {} [{:08X}] mesh '{}'", roll::OrbName(static_cast<roll::Orb>(i)),
            misc->GetFormID(), model->GetModel() ? model->GetModel() : "(none)");
    }
    g_available.store(found == roll::kOrbCount);
    logger::info("orbs: {} of {} orb forms found in {}; VendorItemGem {}, orb mesh '{}'", found,
        roll::kOrbCount, kPlugin, gemKeyword ? "found" : "NOT FOUND (ARPG POV will not label orbs)",
        g_orbModel);
}

void Orbs::Resume()
{
    bool start = false;
    {
        std::scoped_lock lock{ g_queueLock };
        start = !g_queue.empty() && !g_draining;
        if (start) {
            g_draining = true;
        }
    }
    if (start) {
        if (Config::DebugLoadLog()) {
            logger::info("orbs: resuming the death queue after the load");
        }
        SKSE::GetTaskInterface()->AddTask([]() { Drain(); });
    }
}

void Orbs::Install()
{
    if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
        holder->AddEventSink<RE::TESDeathEvent>(DeathSink::GetSingleton());
        logger::info("orbs: death sink installed");
    }
}

bool Orbs::Available()
{
    return g_available.load();
}

RE::TESObjectMISC* Orbs::Form(roll::Orb a_orb)
{
    const auto i = static_cast<std::size_t>(a_orb);
    return i < g_forms.size() ? g_forms[i] : nullptr;
}

int Orbs::OrbOf(std::uint32_t a_base) noexcept
{
    if (a_base == 0) {
        return -1;
    }
    for (int i = 0; i < roll::kOrbCount; ++i) {
        if (g_ids[static_cast<std::size_t>(i)].load(std::memory_order_relaxed) == a_base) {
            return i;
        }
    }
    return -1;
}

std::uint32_t Orbs::Color(roll::Orb a_orb) noexcept
{
    const auto i = static_cast<std::size_t>(a_orb);
    return i < std::size(kColors) ? kColors[i] : 0u;
}

bool Orbs::Check(roll::Orb a_orb, std::uint32_t a_base, const RE::ExtraDataList* a_xl,
    std::string& a_reason)
{
    auto* object = RE::TESForm::LookupByID<RE::TESBoundObject>(a_base);
    const char* reason = object ? CheckUnit(a_orb, object, a_xl) : "Nothing to craft on";
    a_reason = reason ? reason : "";
    return reason == nullptr;
}

bool Orbs::Use(roll::Orb a_orb, std::uint32_t a_base, RE::ExtraDataList* a_xl, std::string& a_message)
{
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* form = Form(a_orb);
    if (!player || !form) {
        a_message = "Orbs are not installed";
        return false;
    }
    if (!PlayerHas(player, form)) {
        a_message = std::format("No {} left", roll::OrbName(a_orb));
        return false;
    }
    if (!Check(a_orb, a_base, a_xl, a_message)) {
        return false;
    }
    Crafting::Target target{ a_base, 0 };
    if (a_xl) {
        if (const auto* xEnch = a_xl->GetByType<RE::ExtraEnchantment>(); xEnch && xEnch->enchantment) {
            target.enchantment = xEnch->enchantment->GetFormID();
        }
    }
    const auto outcome = Crafting::ApplyOrb(target, a_orb);
    a_message = outcome.message;
    if (!outcome.ok) {
        return false;
    }
    // ★CONSUMED ONLY NOW, after the craft happened.
    player->RemoveItem(form, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
    {
        std::scoped_lock lock{ g_statsLock };
        ++g_stats.used;
    }
    return true;
}

void Orbs::Give(roll::Orb a_orb, int a_count)
{
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* form = Form(a_orb);
    if (!player || !form || a_count <= 0) {
        return;
    }
    player->AddObjectToContainer(form, nullptr, a_count, nullptr);
    logger::warn("debug: GAVE {}x {} to the player at their request", a_count, roll::OrbName(a_orb));
}

void Orbs::ForceNextDrop()
{
    g_forceNext.store(true);
}

std::size_t Orbs::Cleanup(bool a_all)
{
    const int days = Config::OrbCleanupDays();
    if (days <= 0 && !a_all) {
        return 0;
    }
    const auto  today = Today();
    std::size_t cleaned = 0;
    for (const auto& [id, day] : Persist::GroundOrbs()) {
        auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
        // Picked up, or gone with its cell: nothing to clean.
        if (!ref || ref->IsDeleted() || ref->IsDisabled() || OrbOf(ref->GetBaseObject() ? ref->GetBaseObject()->GetFormID() : 0) < 0) {
            Persist::ForgetGroundOrb(id);
            continue;
        }
        if (!a_all && today < day + static_cast<std::uint32_t>(days)) {
            continue;
        }
        ref->Disable();
        ref->SetDelete(true);
        Persist::ForgetGroundOrb(id);
        ++cleaned;
    }
    if (cleaned > 0) {
        std::scoped_lock lock{ g_statsLock };
        g_stats.cleaned += cleaned;
        logger::info("orbs: cleaned up {} uncollected orb stack(s)", cleaned);
    }
    return cleaned;
}

std::string Orbs::ModelOf(roll::Orb a_orb)
{
    auto* form = Form(a_orb);
    const char* model = form ? static_cast<RE::TESModel*>(form)->GetModel() : nullptr;
    return model ? model : "";
}

bool Orbs::IsGemMesh(const std::string& a_model)
{
    return Lower(a_model).find("clutter\\gems\\") != std::string::npos;
}

float Orbs::LastDropMultiplier() { return g_lastDropMult.load(); }
int   Orbs::LastDifficulty() { return g_lastDifficulty.load(); }
float Orbs::PreviewDropMultiplier()
{
    return Config::DropMultiplierAuto()
               ? roll::DifficultyMultiplier(g_lastDifficulty.load(), Config::DropQuantityPerDifficulty())
               : (std::max)(1.0f, Config::DropMultiplierOverride());
}
void Orbs::RefreshDifficulty()
{
    (void)ActiveDropMultiplier();
}

Orbs::Stats Orbs::CurrentStats()
{
    std::scoped_lock lock{ g_statsLock };
    return g_stats;
}
