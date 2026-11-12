// =============================================================================
//  The item record. Pure -- see Record.h.
// =============================================================================

#include "Record.h"

#include <algorithm>
#include <cmath>

namespace roll
{
    const Affix* FindAffix(const AffixTable& a_table, std::string_view a_id)
    {
        // A linear walk, on purpose. The table is a few dozen affixes and this
        // is called per affix per tooltip or per craft, never per frame per
        // tile; an index would be one more thing to keep in step with a table
        // that add-on files merge into.
        for (const auto& affix : a_table.Affixes()) {
            if (affix.id == a_id) {
                return &affix;
            }
        }
        return nullptr;
    }

    const AffixTier* FindTier(const Affix& a_affix, int a_tier)
    {
        for (const auto& tier : a_affix.tiers) {
            if (tier.tier == a_tier) {
                return &tier;
            }
        }
        return nullptr;
    }

    ItemRecord MakeRecord(const RolledItem& a_rolled, int a_itemLevel, std::uint8_t a_flags)
    {
        ItemRecord record;
        record.itemLevel = static_cast<std::uint16_t>(std::clamp(a_itemLevel, 1, 65535));
        record.flags = a_flags;
        if (a_rolled.corrupted) {
            record.flags = static_cast<std::uint8_t>(record.flags | kRecordCorrupted);
        }
        record.affixes.reserve(a_rolled.affixes.size());
        for (const auto& rolled : a_rolled.affixes) {
            if (!rolled.affix) {
                continue;
            }
            AffixRecord entry;
            entry.id = rolled.affix->id;
            entry.tier = static_cast<std::uint8_t>(std::clamp(rolled.tier, 0, 255));
            entry.value = rolled.value;
            record.affixes.push_back(std::move(entry));
        }
        return record;
    }

    bool ToRolled(const ItemRecord& a_record, const AffixTable& a_table, RolledItem& a_out)
    {
        a_out = RolledItem{};
        a_out.corrupted = (a_record.flags & kRecordCorrupted) != 0;
        a_out.affixes.reserve(a_record.affixes.size());

        bool complete = true;
        for (const auto& entry : a_record.affixes) {
            const Affix* affix = FindAffix(a_table, entry.id);
            if (!affix) {
                complete = false;
                continue;
            }
            const AffixTier* tier = FindTier(*affix, entry.tier);
            if (!tier) {
                complete = false;
                continue;
            }

            RolledAffix rolled;
            rolled.affix = affix;
            rolled.tier = tier->tier;
            // Points, duration and area come from the table rather than the
            // record: they are properties of the tier row, and a rebalanced
            // table should be what a rebuilt item reflects.
            rolled.points = tier->points;
            rolled.value = entry.value;
            rolled.duration = tier->duration;
            rolled.area = tier->area;

            a_out.tier += rolled.points;
            a_out.affixes.push_back(rolled);
        }
        return complete;
    }

    const AffixTier* MatchTier(const Affix& a_affix, float a_magnitude,
        std::uint32_t a_duration, std::uint32_t a_area)
    {
        // A little slack: the value was drawn as a float inside the range and
        // has been through an enchantment record and a save since.
        const float eps = 1e-3f * std::max(1.0f, std::fabs(a_magnitude));

        const AffixTier* fallback = nullptr;
        for (const auto& tier : a_affix.tiers) {
            const float lo = std::min(tier.minValue, tier.maxValue);
            const float hi = std::max(tier.minValue, tier.maxValue);
            if (a_magnitude < lo - eps || a_magnitude > hi + eps) {
                continue;
            }
            if (tier.duration == a_duration && tier.area == a_area) {
                return &tier;
            }
            if (!fallback) {
                fallback = &tier;
            }
        }
        return fallback;
    }
}
