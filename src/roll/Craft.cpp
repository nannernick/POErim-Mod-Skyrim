// =============================================================================
//  Crafting orbs. Pure -- see Craft.h.
// =============================================================================

#include "Craft.h"

#include <algorithm>
#include <string>

namespace roll
{
    namespace
    {
        RollContext CraftContext(const RollContext& a_ctx)
        {
            RollContext ctx = a_ctx;
            ctx.forNpc = false;   // npcExclude is about what NPCs are handed, not what a player makes
            ctx.noWhite = false;
            return ctx;
        }

        // The pool for adding to THIS item: everything a fresh roll could pick,
        // minus what the item already has. That subtraction is the whole of the
        // "no duplicate affix" rule.
        std::vector<const Affix*> PoolFor(const RolledItem& a_item, const RollContext& a_ctx,
            const AffixTable& a_table)
        {
            auto pool = EligiblePool(a_table, a_ctx);
            std::erase_if(pool, [&](const Affix* a_candidate) {
                return std::any_of(a_item.affixes.begin(), a_item.affixes.end(),
                    [&](const RolledAffix& a_have) { return a_have.affix == a_candidate; });
            });
            return pool;
        }

        // Draws until one lands or the pool runs dry. kSkipped costs an affix
        // from the pool and nothing else, exactly as it does inside Roll.
        bool DrawOne(std::vector<const Affix*>& a_pool, const RollContext& a_ctx,
            const Tuning& a_tuning, Rng& a_rng, RolledAffix& a_out)
        {
            const auto tierWeights = TierWeightsFor(a_tuning, a_ctx.itemLevel);
            while (!a_pool.empty()) {
                switch (DrawAffix(a_pool, a_ctx, tierWeights, a_rng, a_out)) {
                case Draw::kDrawn:
                    return true;
                case Draw::kExhausted:
                    return false;
                case Draw::kSkipped:
                    break;
                }
            }
            return false;
        }

        void Recount(RolledItem& a_item)
        {
            a_item.tier = 0;
            for (const auto& rolled : a_item.affixes) {
                a_item.tier += rolled.points;
            }
        }

        // The count rule alone -- the part that needs no table.
        CraftVerdict CountRule(Orb a_orb, int a_count, int a_itemLevel, const Tuning& a_tuning)
        {
            switch (a_orb) {
            case Orb::kTransmute:
                return a_count == 0 ? CraftVerdict::kOk : CraftVerdict::kNeedsNoAffixes;
            case Orb::kAugment:
                return a_count == 1 ? CraftVerdict::kOk : CraftVerdict::kNeedsOneAffix;
            case Orb::kExalt:
                if (a_count < 2) {
                    return CraftVerdict::kNeedsTwoToFour;
                }
                if (a_count >= kMaxAffixes) {
                    return CraftVerdict::kFull;
                }
                // ★THE SAME FLOOR THE ROLLER HOLDS. Red needs five affixes and
                // five affixes need this level; an orb is not a way round the
                // promise that red starts here and not a level sooner.
                if (a_count == kMaxAffixes - 1 && a_itemLevel < a_tuning.fifthAffixMinLevel) {
                    return CraftVerdict::kFifthLocked;
                }
                return CraftVerdict::kOk;
            case Orb::kChaos:
                return a_count >= 2 ? CraftVerdict::kOk : CraftVerdict::kNeedsTwoOrMore;
            case Orb::kScour:
                return a_count >= 1 ? CraftVerdict::kOk : CraftVerdict::kNothingToRemove;
            }
            return CraftVerdict::kNotAffixable;
        }

        bool Adds(Orb a_orb) noexcept
        {
            return a_orb == Orb::kTransmute || a_orb == Orb::kAugment || a_orb == Orb::kExalt;
        }
    }

    const char* OrbName(Orb a_orb) noexcept
    {
        switch (a_orb) {
        case Orb::kTransmute: return "Orb of Transmutation";
        case Orb::kAugment:   return "Orb of Augmentation";
        case Orb::kExalt:     return "Exalted Orb";
        case Orb::kChaos:     return "Chaos Orb";
        case Orb::kScour:     return "Orb of Scouring";
        }
        return "?";
    }

    const char* OrbKey(Orb a_orb) noexcept
    {
        switch (a_orb) {
        case Orb::kTransmute: return "transmute";
        case Orb::kAugment:   return "augment";
        case Orb::kExalt:     return "exalt";
        case Orb::kChaos:     return "chaos";
        case Orb::kScour:     return "scour";
        }
        return "?";
    }

    bool ParseOrb(std::string_view a_text, Orb& a_out) noexcept
    {
        for (int i = 0; i < kOrbCount; ++i) {
            const auto orb = static_cast<Orb>(i);
            if (a_text == OrbKey(orb)) {
                a_out = orb;
                return true;
            }
        }
        return false;
    }

    const char* Reason(CraftVerdict a_verdict) noexcept
    {
        switch (a_verdict) {
        case CraftVerdict::kOk:               return "";
        case CraftVerdict::kNotAffixable:     return "This item cannot carry affixes";
        case CraftVerdict::kNeedsNoAffixes:   return "Needs an item with no affixes";
        case CraftVerdict::kNeedsOneAffix:    return "Needs an item with exactly one affix";
        case CraftVerdict::kNeedsTwoToFour:   return "Needs an item with two to four affixes";
        case CraftVerdict::kNeedsTwoOrMore:   return "Needs an item with at least two affixes";
        case CraftVerdict::kNothingToRemove:  return "This item has no affixes to remove";
        case CraftVerdict::kFull:             return "This item cannot hold another affix";
        case CraftVerdict::kFifthLocked:      return "Item level too low for a fifth affix";
        case CraftVerdict::kNoEligibleAffix:  return "No affix left that this item can roll";
        }
        return "";
    }

    CraftVerdict CanCraft(Orb a_orb, const RolledItem& a_item, const RollContext& a_ctx,
        const AffixTable& a_table, const Tuning& a_tuning)
    {
        if (a_ctx.itemSlots == kNone) {
            return CraftVerdict::kNotAffixable;
        }
        const auto verdict = CountRule(a_orb, static_cast<int>(a_item.affixes.size()),
            a_ctx.itemLevel, a_tuning);
        if (verdict != CraftVerdict::kOk || a_orb == Orb::kScour) {
            return verdict;
        }

        // Anything that draws needs something to draw from. For Chaos the
        // item's own affixes go back in the pool, so the plain pool is asked.
        const auto ctx = CraftContext(a_ctx);
        const bool any = Adds(a_orb) ? !PoolFor(a_item, ctx, a_table).empty()
                                     : !EligiblePool(a_table, ctx).empty();
        return any ? CraftVerdict::kOk : CraftVerdict::kNoEligibleAffix;
    }

    CraftVerdict Craft(Orb a_orb, RolledItem& a_item, const RollContext& a_ctx,
        const AffixTable& a_table, const Tuning& a_tuning, Rng& a_rng)
    {
        const auto verdict = CanCraft(a_orb, a_item, a_ctx, a_table, a_tuning);
        if (verdict != CraftVerdict::kOk) {
            return verdict;
        }
        const auto ctx = CraftContext(a_ctx);

        if (a_orb == Orb::kScour) {
            a_item.affixes.clear();
            a_item.tier = 0;
            return CraftVerdict::kOk;
        }

        if (Adds(a_orb)) {
            auto        pool = PoolFor(a_item, ctx, a_table);
            RolledAffix added;
            if (!DrawOne(pool, ctx, a_tuning, a_rng, added)) {
                return CraftVerdict::kNoEligibleAffix;   // a_item untouched
            }
            a_item.affixes.push_back(added);
            Recount(a_item);
            return CraftVerdict::kOk;
        }

        // Chaos: every affix rerolled, the count kept. Built to one side so a
        // pool that cannot fill the count leaves the item as it was -- an orb
        // that quietly removed an affix would be a worse Scouring.
        const auto wanted = a_item.affixes.size();
        auto       pool = EligiblePool(a_table, ctx);

        std::vector<RolledAffix> fresh;
        fresh.reserve(wanted);
        while (fresh.size() < wanted) {
            RolledAffix rolled;
            if (!DrawOne(pool, ctx, a_tuning, a_rng, rolled)) {
                break;
            }
            fresh.push_back(rolled);
        }
        if (fresh.size() < wanted) {
            return CraftVerdict::kNoEligibleAffix;
        }
        a_item.affixes = std::move(fresh);
        Recount(a_item);
        return CraftVerdict::kOk;
    }
}
