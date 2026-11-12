// =============================================================================
//  DiabloLootAPI, served. See api/DiabloLootAPI.h.
// =============================================================================

#include "PCH.h"

#include "LootApi.h"

#include "GridTint.h"
#include "Orbs.h"
#include "Persist.h"

#include "api/DiabloLootAPI.h"
#include "roll/Roll.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace
{
    std::uint8_t BandOfList(const void* a_xl)
    {
        if (!a_xl) {
            return 0;
        }
        const auto* xe = static_cast<const RE::ExtraDataList*>(a_xl)->GetByType<RE::ExtraEnchantment>();
        return (xe && xe->enchantment) ? Persist::EnchTier(xe->enchantment->GetFormID()) : 0;
    }

    std::uint8_t BandOfRef(std::uint32_t a_ref)
    {
        const auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_ref);
        return ref ? BandOfList(&ref->extraList) : 0;
    }

    std::uint32_t GetPalette(std::uint32_t* a_out, std::uint32_t a_capacity)
    {
        return a_out ? GridTint::PaletteFor(a_out, a_capacity) : 0;
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
    logger::info("loot api: DiabloLootAPI v{} broadcast {}", DiabloLootAPI::kVersion,
        sent ? "sent" : "had no listener");
}
