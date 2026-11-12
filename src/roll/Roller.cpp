// =============================================================================
//  The roll. Pure -- see Roll.h.
// =============================================================================
//  Shape of a roll, in order:
//
//    1. how many affixes  -- interpolated from the item level against the count
//                            curve, so a mudcrab's drop and a boss's drop differ
//                            in COUNT before they differ in anything else
//    2. which affixes     -- weighted draw from the pool eligible for this
//                            item's slots, without replacement, so no item ever
//                            rolls the same attribute twice
//    3. which tier of each -- weighted by the tier curve, restricted to tiers
//                            the item level has unlocked
//    4. what value        -- uniform inside that tier's band
//
//  Item tier is the sum of the points, which is what makes it a power budget
//  rather than a label: 0-18 falls out of 0-6 affixes worth 1-3 each. The fifth
//  and sixth affix are gated on item level (Tuning::fifthAffixMinLevel and
//  sixthAffixMinLevel), and with the fifth the red band, which nothing short of
//  five affixes can reach.
// =============================================================================

#include "Roll.h"

#include <algorithm>
#include <numeric>
#include <string>

namespace roll
{
    namespace
    {
        // Linear interpolation between the two bracketing bands. Below the first
        // band or above the last, the nearest band is used unchanged.
        // The member pointer is `std::array<int,N> Curve::*`, NOT
        // `const std::array<int,N>& Curve::*` -- there is no such thing as a
        // pointer to a reference member, and writing the const-ref form is an
        // easy way to get four cascading errors that name the wrong lines.
        template <std::size_t N>
        std::array<int, N> Interpolate(const std::vector<Curve>& a_bands, int a_level,
            std::array<int, N> Curve::*a_member)
        {
            if (a_bands.empty()) {
                return {};
            }
            if (a_level <= a_bands.front().level) {
                return a_bands.front().*a_member;
            }
            if (a_level >= a_bands.back().level) {
                return a_bands.back().*a_member;
            }

            for (std::size_t i = 1; i < a_bands.size(); ++i) {
                const auto& hi = a_bands[i];
                if (a_level > hi.level) {
                    continue;
                }
                const auto& lo = a_bands[i - 1];
                const int   span = hi.level - lo.level;
                const float t = span > 0 ? static_cast<float>(a_level - lo.level) / static_cast<float>(span) : 0.0f;

                std::array<int, N> out{};
                for (std::size_t k = 0; k < N; ++k) {
                    const float a = static_cast<float>((lo.*a_member)[k]);
                    const float b = static_cast<float>((hi.*a_member)[k]);
                    out[k] = static_cast<int>(a + (b - a) * t + 0.5f);
                }
                return out;
            }
            return a_bands.back().*a_member;
        }

        // Picks an index in proportion to its weight. Returns -1 when every
        // weight is zero, which is a real case -- an item level below every
        // affix's gate -- and must not be a crash.
        int WeightedPick(const std::vector<std::uint32_t>& a_weights, Rng& a_rng)
        {
            const std::uint64_t total = std::accumulate(a_weights.begin(), a_weights.end(), std::uint64_t{ 0 });
            if (total == 0) {
                return -1;
            }
            std::uniform_int_distribution<std::uint64_t> dist{ 0, total - 1 };
            std::uint64_t                                roll = dist(a_rng);
            for (std::size_t i = 0; i < a_weights.size(); ++i) {
                if (roll < a_weights[i]) {
                    return static_cast<int>(i);
                }
                roll -= a_weights[i];
            }
            return static_cast<int>(a_weights.size()) - 1;
        }

        template <std::size_t N>
        int WeightedPickArray(const std::array<int, N>& a_weights, Rng& a_rng)
        {
            std::vector<std::uint32_t> weights;
            weights.reserve(N);
            for (const int weight : a_weights) {
                weights.push_back(weight > 0 ? static_cast<std::uint32_t>(weight) : 0u);
            }
            return WeightedPick(weights, a_rng);
        }
    }

    Rarity RarityOf(int a_explicits) noexcept
    {
        if (a_explicits <= 0) {
            return Rarity::kNormal;
        }
        return a_explicits < kRareMinMods ? Rarity::kMagic : Rarity::kRare;
    }

    Rarity RarityOf(const RolledItem& a_item) noexcept
    {
        return RarityOf(a_item.Explicits());
    }

    const char* RarityName(Rarity a_rarity) noexcept
    {
        switch (a_rarity) {
        case Rarity::kNone:   return "none";
        case Rarity::kNormal: return "normal";
        case Rarity::kMagic:  return "magic";
        case Rarity::kRare:   return "rare";
        }
        return "?";
    }

    std::uint8_t RarityCode(const RolledItem& a_item) noexcept
    {
        auto code = static_cast<std::uint8_t>(static_cast<std::uint8_t>(RarityOf(a_item)) |
                                              (PowerStepOf(a_item.tier) << 2));
        if (a_item.corrupted) {
            code = static_cast<std::uint8_t>(code | kRarityCorruptedBit);
        }
        return code;
    }

    Rarity RarityFromCode(std::uint8_t a_code) noexcept
    {
        return static_cast<Rarity>(a_code & 0x03);
    }

    int PowerStepFromCode(std::uint8_t a_code) noexcept
    {
        return (a_code >> 2) & 0x07;
    }

    bool CorruptedFromCode(std::uint8_t a_code) noexcept
    {
        return (a_code & kRarityCorruptedBit) != 0;
    }

    int PowerStepOf(int a_points) noexcept
    {
        if (a_points <= 0) {
            return 0;
        }
        if (a_points <= 3) {
            return 1;
        }
        if (a_points <= 6) {
            return 2;
        }
        if (a_points <= 9) {
            return 3;
        }
        if (a_points <= 12) {
            return 4;
        }
        return 5;
    }

    std::array<int, 3> RarityWeightsOf(const Curve& a_band) noexcept
    {
        const auto& w = a_band.countWeights;
        return { w[0], w[1] + w[2], w[3] + w[4] + w[5] + w[6] };
    }

    namespace
    {
        // Rescales a[first..last] to sum to a_total, keeping their split. A
        // group that was all zero takes the total on its first entry.
        void RescaleGroup(std::array<int, kMaxAffixes + 1>& a_w, std::size_t a_first,
            std::size_t a_last, int a_total)
        {
            a_total = std::max(0, a_total);
            int have = 0;
            for (std::size_t i = a_first; i <= a_last; ++i) {
                have += std::max(0, a_w[i]);
            }
            if (have <= 0) {
                for (std::size_t i = a_first; i <= a_last; ++i) {
                    a_w[i] = 0;
                }
                a_w[a_first] = a_total;
                return;
            }
            int given = 0;
            for (std::size_t i = a_first; i <= a_last; ++i) {
                const int share = static_cast<int>(
                    static_cast<long long>(std::max(0, a_w[i])) * a_total / have);
                a_w[i] = share;
                given += share;
            }
            // Rounding remainder onto the largest entry so the group sums exactly.
            std::size_t big = a_first;
            for (std::size_t i = a_first; i <= a_last; ++i) {
                if (a_w[i] > a_w[big]) {
                    big = i;
                }
            }
            a_w[big] += a_total - given;
        }
    }

    void SetRarityWeights(Curve& a_band, int a_normal, int a_magic, int a_rare) noexcept
    {
        a_band.countWeights[0] = std::max(0, a_normal);
        RescaleGroup(a_band.countWeights, 1, 2, a_magic);
        RescaleGroup(a_band.countWeights, 3, 6, a_rare);
    }

    // The tier marker: once for magic, twice for rare.
    //
    // ★ONE GLYPH, ONE PLACE, AND THE PLAYER PICKS IT. The default is written
    // as explicit UTF-8 bytes so the source file's encoding cannot change what
    // ships; the INI overrides it, because whether a glyph draws or comes out a
    // box depends on the player's fonts and menu replacers -- which is not
    // something this file can know.
    constexpr const char* kDefaultTierMark = "\xE2\x97\x86";

    // ★A COPY, not a string_view onto the caller's memory. What arrives is a
    // slice of a line parsed out of an INI, and that buffer is long gone by the
    // time the first item rolls.
    std::string& TierMarkStorage()
    {
        static std::string mark{ kDefaultTierMark };
        return mark;
    }

    bool& TierMarkInNameStorage() noexcept
    {
        // ★ON BY DEFAULT, because the name is currently the only surface that
        // can carry the marker at all.
        //
        // Drawing it on hover instead was tried and reverted: it wants a detour
        // on RE::ItemCard::SetItem, and CommonLibSSE's trampoline offers no way
        // to write one. Trampoline::write_branch<5> looks like the tool and is
        // not -- it PATCHES AN EXISTING CALL INSTRUCTION, decoding the four
        // bytes before its end as the displacement of the original target.
        // Aimed at a function's first byte it reads the prologue as a
        // displacement, hands back an address that is not code, and clobbers
        // the prologue on the way past. Measured, on a forge:
        // EXCEPTION_ACCESS_VIOLATION executing 0x7FF67556D1A0.
        //
        // Doing it properly needs the address-library id of a CALL SITE, which
        // is not something this file can derive.
        static bool inName = true;
        return inName;
    }

    // Normal 0 marks, magic 1, rare 2.
    std::string RarityMark(Rarity a_rarity)
    {
        const auto& mark = TierMarkStorage();
        std::size_t count = 0;
        if (a_rarity == Rarity::kMagic) {
            count = 1;
        } else if (a_rarity == Rarity::kRare) {
            count = 2;
        }
        std::string out;
        if (mark.empty() || count == 0) {
            return out;
        }
        out.reserve(mark.size() * count);
        for (std::size_t i = 0; i < count; ++i) {
            out += mark;
        }
        return out;
    }

    void SetTierMark(std::string_view a_mark)
    {
        TierMarkStorage().assign(a_mark);
    }

    std::string_view TierMark() noexcept
    {
        return TierMarkStorage();
    }

    void SetTierMarkInName(bool a_enabled) noexcept
    {
        TierMarkInNameStorage() = a_enabled;
    }

    bool TierMarkInName() noexcept
    {
        return TierMarkInNameStorage();
    }

    std::string RolledItem::Name(std::string_view a_baseName) const
    {
        // ★ONE FRAGMENT PER AFFIX. An affix defines both a prefix and a suffix,
        // but it may only ever contribute ONE of them -- otherwise a single-affix
        // item comes out as "Honed Iron Sword of Wielding", where both halves are
        // the same affix talking about itself twice.
        //
        // ★WITH KINDS, PATH OF EXILE'S RULE: a prefix affix names the front, a
        // suffix affix the back, the stronger of each kind speaking. The
        // implicit never names the item -- it belongs to the base, not to the
        // roll. A table whose kind leaves one side empty falls back to the old
        // rule for that side, so a name never comes out with a fragment missing
        // that an affix on the item could have supplied.
        const RolledAffix* forSuffix = nullptr;
        const RolledAffix* forPrefix = nullptr;

        const auto strongest = [&](AffixKind a_kind, bool a_wantSuffix, const RolledAffix* a_skip) {
            const RolledAffix* best = nullptr;
            for (const auto& rolled : affixes) {
                if (!rolled.affix || &rolled == a_skip || rolled.affix->Implicit()) {
                    continue;
                }
                if (rolled.affix->kind != a_kind) {
                    continue;
                }
                const auto& fragment = a_wantSuffix ? rolled.affix->suffix : rolled.affix->prefix;
                if (!fragment.empty() && (!best || rolled.points > best->points)) {
                    best = &rolled;
                }
            }
            return best;
        };

        forSuffix = strongest(AffixKind::kSuffix, true, nullptr);
        forPrefix = strongest(AffixKind::kPrefix, false, nullptr);
        if (!forSuffix) {
            forSuffix = strongest(AffixKind::kPrefix, true, forPrefix);
        }
        if (!forPrefix) {
            forPrefix = strongest(AffixKind::kSuffix, false, forSuffix);
        }

        std::string out;
        if (forPrefix) {
            out += forPrefix->affix->prefix;
            out += ' ';
        }
        out += a_baseName;
        if (forSuffix) {
            out += ' ';
            out += forSuffix->affix->suffix;
        }

        // ★RARITY, AS A REPEATED MARKER, TRAILING -- AND OPTIONAL.
        //
        // One mark for magic, two for rare (RarityMark).
        //
        // TRAILING, because a leading marker reorders every alphabetical item
        // list in the game.
        //
        // OPTIONAL, because a name is an expensive place to put it: the marker
        // joins the list's sort key, it is what a narrow barter column
        // truncates, and it goes into the save as a per-instance override. With
        // Grid Inventory installed the tint already carries the rarity and none
        // of that is worth paying -- see TierMarkInNameStorage for why the
        // cheaper surface, the item card, is not on offer.
        if (TierMarkInName()) {
            if (const std::string mark = RarityMark(RarityOf(*this)); !mark.empty()) {
                out += ' ';
                out += mark;
            }
        }
        return out;
    }

    std::array<int, kMaxAffixPoints> TierWeightsFor(const Tuning& a_tuning, int a_itemLevel)
    {
        return Interpolate(a_tuning.bands, a_itemLevel, &Curve::tierWeights);
    }

    // Candidates: eligible for this slot, not excluded, and with at least one
    // tier the item level has unlocked.
    std::vector<const Affix*> EligiblePool(const AffixTable& a_table, const RollContext& a_ctx)
    {
        std::vector<const Affix*> pool;
        for (const auto& affix : a_table.Affixes()) {
            if (affix.Implicit()) {
                continue;
            }
            if ((affix.slots & a_ctx.itemSlots) == 0) {
                continue;
            }
            if (a_ctx.forNpc && affix.npcExclude) {
                continue;
            }
            const bool anyUnlocked = std::any_of(affix.tiers.begin(), affix.tiers.end(),
                [&](const AffixTier& a_tier) { return a_ctx.itemLevel >= a_tier.minItemLevel; });
            if (anyUnlocked) {
                pool.push_back(&affix);
            }
        }
        return pool;
    }

    std::vector<const Affix*> ImplicitPool(const AffixTable& a_table, const RollContext& a_ctx)
    {
        std::vector<const Affix*> pool;
        for (const auto& affix : a_table.Affixes()) {
            if (!affix.Implicit() || (affix.slots & a_ctx.itemSlots) == 0) {
                continue;
            }
            const bool anyUnlocked = std::any_of(affix.tiers.begin(), affix.tiers.end(),
                [&](const AffixTier& a_tier) { return a_ctx.itemLevel >= a_tier.minItemLevel; });
            if (anyUnlocked) {
                pool.push_back(&affix);
            }
        }
        return pool;
    }

    void FitPool(std::vector<const Affix*>& a_pool, const RolledItem& a_item)
    {
        FitPool(a_pool, a_item, MaxPrefixes(), MaxSuffixes());
    }

    void FitPool(std::vector<const Affix*>& a_pool, const RolledItem& a_item, int a_maxPrefixes,
        int a_maxSuffixes)
    {
        const bool prefixesFull = a_item.Count(AffixKind::kPrefix) >= a_maxPrefixes;
        const bool suffixesFull = a_item.Count(AffixKind::kSuffix) >= a_maxSuffixes;
        std::erase_if(a_pool, [&](const Affix* a_candidate) {
            if (a_candidate->kind == AffixKind::kPrefix && prefixesFull) {
                return true;
            }
            if (a_candidate->kind == AffixKind::kSuffix && suffixesFull) {
                return true;
            }
            return std::any_of(a_item.affixes.begin(), a_item.affixes.end(),
                [&](const RolledAffix& a_have) {
                    return a_have.affix == a_candidate ||
                           a_have.affix->group == a_candidate->group;
                });
        });
    }

    void FitImplicitPool(std::vector<const Affix*>& a_pool, const RolledItem& a_item)
    {
        std::erase_if(a_pool, [&](const Affix* a_candidate) {
            if (!a_candidate->Implicit()) {
                return true;
            }
            return std::any_of(a_item.affixes.begin(), a_item.affixes.end(),
                [&](const RolledAffix& a_have) {
                    return a_have.affix == a_candidate ||
                           a_have.affix->group == a_candidate->group;
                });
        });
    }

    int RolledItem::Count(AffixKind a_kind) const noexcept
    {
        int count = 0;
        for (const auto& rolled : affixes) {
            if (rolled.affix && rolled.affix->kind == a_kind) {
                ++count;
            }
        }
        return count;
    }

    Draw DrawAffix(std::vector<const Affix*>& a_pool, const RollContext& a_ctx,
        const std::array<int, kMaxAffixPoints>& a_tierWeights, Rng& a_rng, RolledAffix& a_out)
    {
        // Draw without replacement: an affix already on the item is removed
        // from the pool, which is the "no duplicate attributes" rule from
        // your design notes enforced structurally rather than by retrying.
        std::vector<std::uint32_t> weights;
        weights.reserve(a_pool.size());
        for (const auto* affix : a_pool) {
            // An affix's drop weight is taken from its lowest unlocked tier,
            // so weight stays a property of the affix rather than drifting
            // with which tier happens to roll.
            std::uint32_t weight = 0;
            for (const auto& tier : affix->tiers) {
                if (a_ctx.itemLevel >= tier.minItemLevel) {
                    weight = tier.weight;
                    break;
                }
            }
            weights.push_back(weight);
        }

        const int chosen = WeightedPick(weights, a_rng);
        if (chosen < 0) {
            return Draw::kExhausted;
        }
        const Affix* affix = a_pool[static_cast<std::size_t>(chosen)];
        a_pool.erase(a_pool.begin() + chosen);

        // Which tier. Restricted to unlocked tiers, weighted by the curve --
        // and note the curve is indexed by TIER NUMBER, not by position, so
        // Paralysis skipping Tier I costs it nothing.
        std::vector<std::uint32_t>    tierPick;
        std::vector<const AffixTier*> unlocked;
        for (const auto& tier : affix->tiers) {
            if (a_ctx.itemLevel < tier.minItemLevel) {
                continue;
            }
            const std::size_t slot = static_cast<std::size_t>(tier.tier - 1);
            const int         weight = slot < a_tierWeights.size() ? a_tierWeights[slot] : 1;
            tierPick.push_back(weight > 0 ? static_cast<std::uint32_t>(weight) : 1u);
            unlocked.push_back(&tier);
        }
        if (unlocked.empty()) {
            return Draw::kSkipped;
        }

        const int tierIndex = WeightedPick(tierPick, a_rng);
        if (tierIndex < 0) {
            return Draw::kSkipped;
        }
        const AffixTier& tier = *unlocked[static_cast<std::size_t>(tierIndex)];

        // Value: uniform inside the band. min and max may both be negative
        // (cost reduction), so normalise rather than assuming min < max.
        const float lo = std::min(tier.minValue, tier.maxValue);
        const float hi = std::max(tier.minValue, tier.maxValue);
        std::uniform_real_distribution<float> valueDist{ lo, hi };

        a_out.affix = affix;
        a_out.tier = tier.tier;
        a_out.points = tier.points;
        a_out.value = (lo == hi) ? lo : valueDist(a_rng);
        a_out.duration = tier.duration;
        a_out.area = tier.area;
        return Draw::kDrawn;
    }

    RolledItem Roll(const AffixTable& a_table, const RollContext& a_ctx,
        const Tuning& a_tuning, Rng& a_rng)
    {
        RolledItem result;
        if (a_table.Empty() || a_ctx.itemSlots == kNone) {
            return result;
        }

        auto countWeights = Interpolate(a_tuning.bands, a_ctx.itemLevel, &Curve::countWeights);

        // ★THE FIFTH AFFIX HAS A FLOOR THE CURVE CANNOT LOWER. Interpolation
        // ramps every weight smoothly, and a smooth ramp from zero at 25 to
        // something at 40 is not zero at 30. Red is defined as the band only a
        // fifth affix can reach, and the design says red starts at 35 -- so
        // below that the weight is taken away here, after the curve has had its
        // say, rather than trusted to a band that would have to be tuned to
        // land exactly on zero.
        //
        // Done BEFORE the noWhite adjustment so its "every weight is zero"
        // fallback sees the weights that will actually be drawn from.
        //
        // The sixth affix has the same kind of floor, a few levels on.
        if (a_ctx.itemLevel < a_tuning.fifthAffixMinLevel) {
            countWeights[5] = 0;
            countWeights[6] = 0;
        }
        if (a_ctx.itemLevel < a_tuning.sixthAffixMinLevel) {
            countWeights[6] = 0;
        }

        if (a_ctx.noWhite) {
            countWeights[0] = 0;

            // ★THE FALLBACK IS NOT DEAD CODE. The shipped curve offers a
            // non-zero weight at every band, so this cannot fire today -- but
            // the curve is data a user is invited to edit, and a band tuned to
            // "white or nothing" would otherwise leave every weight at zero.
            // WeightedPick has to return something at that point, and what it
            // returns is not this function's decision to leave to it.
            const int total = std::accumulate(countWeights.begin(), countWeights.end(), 0);
            if (total <= 0) {
                countWeights[1] = 1;
            }
        }

        const int wanted = WeightedPickArray(countWeights, a_rng);

        const auto tierWeights = TierWeightsFor(a_tuning, a_ctx.itemLevel);

        // ★THE BASE IMPLICIT FIRST (2.1.0). Every craftable item gets exactly
        // one, white items included: it belongs to the base, sits outside the
        // 3 + 3, and replaces the vanilla enchantment new items used to carry.
        // Drawn before the explicits so the mod-group rule keeps an explicit
        // from repeating the implicit's effect. An item whose kind has no
        // implicit in the table simply has none.
        if (a_tuning.rollImplicit) {
            auto implicits = ImplicitPool(a_table, a_ctx);
            FitImplicitPool(implicits, result);
            while (!implicits.empty()) {
                RolledAffix rolled;
                const auto  drawn = DrawAffix(implicits, a_ctx, tierWeights, a_rng, rolled);
                if (drawn == Draw::kExhausted) {
                    break;
                }
                if (drawn == Draw::kDrawn) {
                    result.tier += rolled.points;
                    result.affixes.push_back(rolled);
                    break;
                }
            }
        }

        if (wanted <= 0) {
            return result;  // a normal (white) item, and that is a legitimate roll
        }

        // ★RARITY DECIDES THE CAPS. 1-2 mods is MAGIC: at most one prefix and
        // one suffix. 3-6 is RARE: the INI's caps. The count curve read as
        // rarity weights is in RarityWeightsOf.
        const int explicitCap = MaxPrefixes() + MaxSuffixes();
        const int explicitWanted = std::min(wanted, explicitCap);
        const bool magic = RarityOf(explicitWanted) == Rarity::kMagic;
        const int  capP = magic ? kMagicMaxPrefixes : MaxPrefixes();
        const int  capS = magic ? kMagicMaxSuffixes : MaxSuffixes();

        result.affixes.reserve(result.affixes.size() + static_cast<std::size_t>(explicitWanted));

        // The pool and the draw are functions of their own (below) because a
        // crafting orb adds an affix to an item by exactly the same rules a
        // fresh roll does -- see Craft.cpp. One implementation, two callers.
        auto pool = EligiblePool(a_table, a_ctx);
        FitPool(pool, result, capP, capS);

        int explicits = 0;
        while (explicits < explicitWanted && !pool.empty()) {
            RolledAffix rolled;
            const auto  drawn = DrawAffix(pool, a_ctx, tierWeights, a_rng, rolled);
            if (drawn == Draw::kExhausted) {
                break;
            }
            if (drawn == Draw::kSkipped) {
                continue;
            }
            result.tier += rolled.points;
            result.affixes.push_back(rolled);
            ++explicits;
            // A kind that just reached its cap leaves the pool.
            FitPool(pool, result, capP, capS);
        }

        if (a_ctx.forNpc && result.tier > a_tuning.npcMaxTier) {
            // Trim from the weakest end so the ceiling costs the item its least
            // interesting affix rather than its best one. The implicit belongs
            // to the base and is never trimmed.
            std::stable_sort(result.affixes.begin(), result.affixes.end(),
                [](const RolledAffix& a_lhs, const RolledAffix& a_rhs) {
                    const bool li = a_lhs.affix && a_lhs.affix->Implicit();
                    const bool ri = a_rhs.affix && a_rhs.affix->Implicit();
                    if (li != ri) {
                        return ri;   // explicits first: they are what gets trimmed
                    }
                    return a_lhs.points < a_rhs.points;
                });
            while (result.tier > a_tuning.npcMaxTier && !result.affixes.empty() &&
                   !(result.affixes.front().affix && result.affixes.front().affix->Implicit())) {
                result.tier -= result.affixes.front().points;
                result.affixes.erase(result.affixes.begin());
            }
        }

        return result;
    }
}
