// =============================================================================
//  DiabloLootAPI, served. See api/DiabloLootAPI.h.
// =============================================================================

#include "PCH.h"

#include "LootApi.h"

#include "Config.h"
#include "GridTint.h"
#include "Orbs.h"
#include "Persist.h"

#include "api/DiabloLootAPI.h"
#include "roll/Roll.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>

namespace
{
    // v1's band, from the 2.1.0 rarity: 0 normal, 1 magic, 2 rare, 5 corrupted.
    std::uint8_t BandOfList(const void* a_xl)
    {
        const auto code = GridTint::CodeFor(a_xl);
        if (code == 0) {
            return 0;
        }
        if (roll::CorruptedFromCode(code)) {
            return 5;
        }
        switch (roll::RarityFromCode(code)) {
        case roll::Rarity::kMagic: return 1;
        case roll::Rarity::kRare:  return 2;
        default:                   return 0;
        }
    }

    std::uint8_t BandOfRef(std::uint32_t a_ref)
    {
        const auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_ref);
        return ref ? BandOfList(&ref->extraList) : 0;
    }

    // v1's five slots: blue, yellow, (yellow), (yellow), red.
    std::uint32_t GetPalette(std::uint32_t* a_out, std::uint32_t a_capacity)
    {
        if (!a_out) {
            return 0;
        }
        const std::uint32_t palette[]{ Config::RarityColor(Config::RarityColour::kMagic),
            Config::RarityColor(Config::RarityColour::kRare), Config::RarityColor(Config::RarityColour::kRare),
            Config::RarityColor(Config::RarityColour::kRare),
            Config::RarityColor(Config::RarityColour::kCorrupted) };
        const auto n = (std::min)(a_capacity, static_cast<std::uint32_t>(std::size(palette)));
        for (std::uint32_t i = 0; i < n; ++i) {
            a_out[i] = palette[i];
        }
        return n;
    }

    bool IsCurrency(std::uint32_t a_base)
    {
        return Orbs::OrbOf(a_base) >= 0;
    }

    std::uint32_t CurrencyColor(std::uint32_t a_base)
    {
        const int orb = Orbs::OrbOf(a_base);
        return orb >= 0 ? Orbs::Color(static_cast<roll::Orb>(orb)) : 0u;
    }

    std::uint32_t CleanName(std::uint32_t a_ref, char* a_out, std::uint32_t a_capacity)
    {
        if (!a_out || a_capacity == 0) {
            return 0;
        }
        a_out[0] = '\0';
        auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_ref);
        const char* name = ref ? ref->GetDisplayFullName() : nullptr;
        if (!name) {
            return 0;
        }
        std::string text = name;
        const std::string_view mark = roll::TierMark();
        if (!mark.empty()) {
            while (text.size() >= mark.size() &&
                   text.compare(text.size() - mark.size(), mark.size(), mark) == 0) {
                text.erase(text.size() - mark.size());
            }
            while (!text.empty() && text.back() == ' ') {
                text.pop_back();
            }
        }
        const auto n = (std::min)(text.size(), static_cast<std::size_t>(a_capacity - 1));
        std::memcpy(a_out, text.data(), n);
        a_out[n] = '\0';
        return static_cast<std::uint32_t>(n);
    }

    std::uint32_t TooltipLines(std::uint32_t a_ref, DiabloLootAPI::Line* a_out, std::uint32_t a_capacity)
    {
        auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_ref);
        auto* base = ref ? ref->GetBaseObject() : nullptr;
        if (!a_out || !base || a_capacity == 0) {
            return 0;
        }
        static_assert(sizeof(DiabloLootAPI::Line) == sizeof(GridInvAPI::TooltipLine),
            "the two line types share one layout");
        GridInvAPI::TooltipLine lines[DiabloLootAPI::kMaxLines]{};
        const auto cap = (std::min)(a_capacity, DiabloLootAPI::kMaxLines);
        const auto n = GridTint::LinesFor(base->GetFormID(), &ref->extraList, lines, cap);
        for (std::uint32_t i = 0; i < n && i < cap; ++i) {
            std::memcpy(a_out[i].text, lines[i].text, sizeof(a_out[i].text));
            a_out[i].rgba = lines[i].rgba;
            a_out[i].indent = lines[i].indent;
            a_out[i].separatorBefore = lines[i].separatorBefore;
            a_out[i]._pad[0] = a_out[i]._pad[1] = 0;
        }
        return (std::min)(n, cap);
    }

    // ---- v2 ------------------------------------------------------------------
    //
    // ★CACHED BY ENCHANTMENT, invalidated on craft. ARPG POV asks once per
    // labelled reference per frame; the answer for an enchantment only changes
    // when an orb rebuilds it (which also changes the enchantment), so the
    // cache is keyed by the enchantment's id and dropped on every craft and
    // every load (LootApi::Invalidate).
    std::mutex                                          g_cacheLock;
    std::unordered_map<std::uint32_t, DiabloLootAPI::ItemInfo> g_cache;
    std::atomic<std::uint32_t>                          g_v2Calls{ 0 };
    std::atomic<std::uint32_t>                          g_lastColour{ 0 };
    std::atomic<std::uint32_t>                          g_lastBase{ 0 };

    bool Fill(std::uint32_t a_base, const void* a_xl, DiabloLootAPI::ItemInfo* a_out)
    {
        if (!a_out || a_out->structSize != sizeof(DiabloLootAPI::ItemInfo)) {
            return false;
        }
        g_v2Calls.fetch_add(1, std::memory_order_relaxed);
        a_out->rarity = DiabloLootAPI::kRarityUnknown;
        a_out->corrupted = 0;
        a_out->currency = 0;
        a_out->explicitMods = 0;
        a_out->rgba = 0;
        a_out->_reserved = 0;

        if (Orbs::OrbOf(a_base) >= 0) {
            a_out->rarity = DiabloLootAPI::kRarityCurrency;
            a_out->currency = 1;
            a_out->rgba = Config::RarityColor(Config::RarityColour::kCurrency);
            g_lastBase.store(a_base);
            g_lastColour.store(a_out->rgba);
            return true;
        }
        const auto* xe = a_xl ? static_cast<const RE::ExtraDataList*>(a_xl)->GetByType<RE::ExtraEnchantment>()
                              : nullptr;
        if (!xe || !xe->enchantment) {
            return false;
        }
        const auto ench = xe->enchantment->GetFormID();
        {
            std::scoped_lock lock{ g_cacheLock };
            if (const auto it = g_cache.find(ench); it != g_cache.end()) {
                *a_out = it->second;
                g_lastBase.store(a_base);
                g_lastColour.store(a_out->rgba);
                return a_out->rarity != DiabloLootAPI::kRarityUnknown;
            }
        }
        const auto code = GridTint::CodeFor(a_xl);
        if (code != 0) {
            const auto rarity = roll::RarityFromCode(code);
            a_out->rarity = rarity == roll::Rarity::kMagic ? DiabloLootAPI::kRarityMagic
                            : rarity == roll::Rarity::kRare ? DiabloLootAPI::kRarityRare
                                                            : DiabloLootAPI::kRarityNormal;
            a_out->corrupted = roll::CorruptedFromCode(code) ? 1 : 0;
            a_out->rgba = GridTint::ColourFor(a_base, a_xl);
            roll::ItemRecord record;
            if (Persist::GetItemRecord(ench, record)) {
                a_out->explicitMods = static_cast<std::uint8_t>((std::min)(record.affixes.size(), std::size_t{ 6 }));
            }
        }
        {
            std::scoped_lock lock{ g_cacheLock };
            g_cache[ench] = *a_out;
        }
        g_lastBase.store(a_base);
        g_lastColour.store(a_out->rgba);
        return a_out->rarity != DiabloLootAPI::kRarityUnknown;
    }

    bool InfoOfItem(std::uint32_t a_base, const void* a_xl, DiabloLootAPI::ItemInfo* a_out)
    {
        return Fill(a_base, a_xl, a_out);
    }

    bool InfoOfRef(std::uint32_t a_ref, DiabloLootAPI::ItemInfo* a_out)
    {
        const auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_ref);
        const auto* base = ref ? ref->GetBaseObject() : nullptr;
        if (!base) {
            if (a_out && a_out->structSize == sizeof(DiabloLootAPI::ItemInfo)) {
                a_out->rarity = DiabloLootAPI::kRarityUnknown;
                a_out->rgba = 0;
            }
            return false;
        }
        return Fill(base->GetFormID(), &ref->extraList, a_out);
    }

    const DiabloLootAPI::InterfaceV2 g_interfaceV2{
        sizeof(DiabloLootAPI::InterfaceV2),
        DiabloLootAPI::kVersion2,
        &InfoOfRef,
        &InfoOfItem,
    };

    const DiabloLootAPI::Interface g_interface{
        sizeof(DiabloLootAPI::Interface),
        DiabloLootAPI::kVersion,
        &BandOfRef,
        &BandOfList,
        &GetPalette,
        &IsCurrency,
        &CurrencyColor,
        &CleanName,
        &TooltipLines,
    };
}

void LootApi::Broadcast()
{
    const bool sent = SKSE::GetMessagingInterface()->Dispatch(DiabloLootAPI::kMsgHello,
        const_cast<DiabloLootAPI::Interface*>(&g_interface), sizeof(g_interface), nullptr);
    const bool sentV2 = SKSE::GetMessagingInterface()->Dispatch(DiabloLootAPI::kMsgHelloV2,
        const_cast<DiabloLootAPI::InterfaceV2*>(&g_interfaceV2), sizeof(g_interfaceV2), nullptr);
    logger::info("loot api: DiabloLootAPI v{} broadcast {}, v{} {}", DiabloLootAPI::kVersion,
        sent ? "sent" : "had no listener", DiabloLootAPI::kVersion2, sentV2 ? "sent" : "had no listener");
}

void LootApi::Invalidate()
{
    std::scoped_lock lock{ g_cacheLock };
    g_cache.clear();
}

std::uint32_t LootApi::V2Calls()
{
    return g_v2Calls.load();
}

bool LootApi::LastAnswer(std::uint32_t& a_base, std::uint32_t& a_rgba)
{
    a_base = g_lastBase.load();
    a_rgba = g_lastColour.load();
    return a_base != 0;
}
