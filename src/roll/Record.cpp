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
            (rolled.affix->Implicit() ? record.implicits : record.affixes).push_back(std::move(entry));
        }
        return record;
    }

    bool NormalizeRecord(ItemRecord& a_record, const AffixTable& a_table)
    {
        bool moved = false;
        for (auto it = a_record.affixes.begin(); it != a_record.affixes.end();) {
            const Affix* affix = FindAffix(a_table, it->id);
            if (affix && affix->Implicit()) {
                a_record.implicits.push_back(std::move(*it));
                it = a_record.affixes.erase(it);
                moved = true;
            } else {
                ++it;
            }
        }
        return moved;
    }

    bool ToRolled(const ItemRecord& a_record, const AffixTable& a_table, RolledItem& a_out)
    {
        a_out = RolledItem{};
        a_out.corrupted = (a_record.flags & kRecordCorrupted) != 0;
        a_out.affixes.reserve(a_record.affixes.size() + a_record.implicits.size());

        bool       complete = true;
        const auto add = [&](const AffixRecord& a_entry) {
            const Affix* affix = FindAffix(a_table, a_entry.id);
            if (!affix) {
                complete = false;
                return;
            }
            const AffixTier* tier = FindTier(*affix, a_entry.tier);
            if (!tier) {
                complete = false;
                return;
            }

            RolledAffix rolled;
            rolled.affix = affix;
            rolled.tier = tier->tier;
            // Points, duration and area come from the table rather than the
            // record: they are properties of the tier row, and a rebalanced
            // table should be what a rebuilt item reflects.
            rolled.points = tier->points;
            rolled.value = a_entry.value;
            rolled.duration = tier->duration;
            rolled.area = tier->area;

            a_out.tier += rolled.points;
            a_out.affixes.push_back(rolled);
        };
        // Implicits first, the order a roll produces them in.
        for (const auto& entry : a_record.implicits) {
            add(entry);
        }
        for (const auto& entry : a_record.affixes) {
            add(entry);
        }
        return complete;
    }

    const AffixTier* MatchTier(const Affix& a_affix, float a_magnitude,
        std::uint32_t a_duration, std::uint32_t a_area)
    {
        // ★TWO PASSES. The first asks which row holds the value EXACTLY, as a
        // roll draws it: [lo, hi), and [lo, hi] for a row with nothing above
        // it. The second allows a little slack, for a value that has been
        // through an enchantment record and a save since it was drawn.
        //
        // One slack pass was the 1.2.0 rule, and it misread about 1 Summermyst
        // affix in 2,000: those rows TOUCH (Tier I 5-10, Tier II 10-15), so a
        // Tier II value of 10.004 was inside Tier I's slack and Tier I was
        // checked first. A value inside a row without slack is that row's.
        const AffixTier* exact = nullptr;
        const AffixTier* exactTied = nullptr;
        for (const auto& tier : a_affix.tiers) {
            const float lo = std::min(tier.minValue, tier.maxValue);
            const float hi = std::max(tier.minValue, tier.maxValue);
            const bool  inside = lo == hi ? a_magnitude == lo
                                          : (a_magnitude >= lo && a_magnitude < hi);
            const bool  atTop = a_magnitude == hi;
            if (!inside && !atTop) {
                continue;
            }
            const bool tied = tier.duration == a_duration && tier.area == a_area;
            if (inside) {
                if (tied) {
                    return &tier;
                }
                if (!exact) {
                    exact = &tier;
                }
            } else if (tied && !exactTied) {
                exactTied = &tier;   // the closed top of a row: only if nothing holds it half-open
            }
        }
        if (exact) {
            return exact;
        }
        if (exactTied) {
            return exactTied;
        }

        const float eps = 1e-3f * std::max(1.0f, std::fabs(a_magnitude));

        const AffixTier* fallback = nullptr;
        const AffixTier* nearest = nullptr;
        float            nearestGap = 0.0f;
        for (const auto& tier : a_affix.tiers) {
            const float lo = std::min(tier.minValue, tier.maxValue);
            const float hi = std::max(tier.minValue, tier.maxValue);
            if (a_magnitude < lo - eps || a_magnitude > hi + eps) {
                continue;
            }
            const float gap = a_magnitude < lo ? lo - a_magnitude : (a_magnitude > hi ? a_magnitude - hi : 0.0f);
            if (tier.duration == a_duration && tier.area == a_area) {
                if (!nearest || gap < nearestGap) {
                    nearest = &tier;
                    nearestGap = gap;
                }
            }
            if (!fallback) {
                fallback = &tier;
            }
        }
        return nearest ? nearest : fallback;
    }
}
