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

        int ExplicitCap()
        {
            return MaxPrefixes() + MaxSuffixes();
        }

        // What an Exalted Orb would add: an explicit while there is room for
        // one, the implicit once the explicits are full.
        enum class ExaltTarget
        {
            kExplicit,
            kImplicit,
        };

        ExaltTarget ExaltTargetFor(const RolledItem& a_item)
        {
            const bool explicitsFull = a_item.Count(AffixKind::kPrefix) >= MaxPrefixes() &&
                                       a_item.Count(AffixKind::kSuffix) >= MaxSuffixes();
            return explicitsFull ? ExaltTarget::kImplicit : ExaltTarget::kExplicit;
        }

        // The count rule alone -- the part that needs no table.
        CraftVerdict CountRule(Orb a_orb, const RolledItem& a_item, int a_itemLevel,
            const Tuning& a_tuning)
        {
            const int explicits = a_item.Explicits();
            switch (a_orb) {
            case Orb::kTransmute:
                return a_item.affixes.empty() ? CraftVerdict::kOk : CraftVerdict::kNeedsNoAffixes;
            case Orb::kAugment:
                return explicits == 1 ? CraftVerdict::kOk : CraftVerdict::kNeedsOneAffix;
            case Orb::kExalt:
                if (explicits < 2) {
                    return CraftVerdict::kNeedsTwoToFour;
                }
                if (ExaltTargetFor(a_item) == ExaltTarget::kExplicit) {
                    return CraftVerdict::kOk;
                }
                if (a_item.HasImplicit()) {
                    return CraftVerdict::kFull;
                }
                // ★THE SAME FLOOR THE ROLLER HOLDS. The implicit is the last
                // slot, red needs it, and red starts at this level -- an orb is
                // not a way round that.
                if (a_itemLevel < a_tuning.fifthAffixMinLevel) {
                    return CraftVerdict::kFifthLocked;
                }
                return CraftVerdict::kOk;
            case Orb::kChaos:
                return explicits >= 2 ? CraftVerdict::kOk : CraftVerdict::kNeedsTwoOrMore;
            case Orb::kScour:
                return a_item.affixes.empty() ? CraftVerdict::kNothingToRemove : CraftVerdict::kOk;
            }
            return CraftVerdict::kNotAffixable;
        }

        bool Adds(Orb a_orb) noexcept
        {
            return a_orb == Orb::kTransmute || a_orb == Orb::kAugment || a_orb == Orb::kExalt;
        }

        // The pool the next draw on this item comes from.
        std::vector<const Affix*> PoolFor(Orb a_orb, const RolledItem& a_item,
            const RollContext& a_ctx, const AffixTable& a_table)
        {
            auto pool = (a_orb == Orb::kExalt && ExaltTargetFor(a_item) == ExaltTarget::kImplicit)
                            ? ImplicitPool(a_table, a_ctx)
                            : EligiblePool(a_table, a_ctx);
            FitPool(pool, a_item);
            return pool;
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
        case CraftVerdict::kNeedsOneAffix:    return "Needs an item with exactly one prefix or suffix";
        case CraftVerdict::kNeedsTwoToFour:   return "Needs an item with at least two prefixes or suffixes";
        case CraftVerdict::kNeedsTwoOrMore:   return "Needs an item with at least two prefixes or suffixes";
        case CraftVerdict::kNothingToRemove:  return "This item has no affixes to remove";
        case CraftVerdict::kFull:             return "This item cannot hold another affix";
        case CraftVerdict::kFifthLocked:      return "Item level 35 needed for an implicit";
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
        const auto verdict = CountRule(a_orb, a_item, a_ctx.itemLevel, a_tuning);
        if (verdict != CraftVerdict::kOk || a_orb == Orb::kScour) {
            return verdict;
        }

        // Anything that draws needs something to draw from. For Chaos the
        // item's explicits go back in the pool, so only the implicit is held
        // against it.
        const auto ctx = CraftContext(a_ctx);
        bool       any = false;
        if (Adds(a_orb)) {
            any = !PoolFor(a_orb, a_item, ctx, a_table).empty();
        } else {
            RolledItem implicitOnly;
            for (const auto& rolled : a_item.affixes) {
                if (rolled.affix && rolled.affix->Implicit()) {
                    implicitOnly.affixes.push_back(rolled);
                }
            }
            auto pool = EligiblePool(a_table, ctx);
            FitPool(pool, implicitOnly);
            any = !pool.empty();
        }
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
            // Everything, the implicit included: Scouring returns the plain
            // base, which is what the brief asks of it.
            a_item.affixes.clear();
            a_item.tier = 0;
            return CraftVerdict::kOk;
        }

        if (Adds(a_orb)) {
            auto        pool = PoolFor(a_orb, a_item, ctx, a_table);
            RolledAffix added;
            if (!DrawOne(pool, ctx, a_tuning, a_rng, added)) {
                return CraftVerdict::kNoEligibleAffix;   // a_item untouched
            }
            a_item.affixes.push_back(added);
            Recount(a_item);
            return CraftVerdict::kOk;
        }

        // Chaos: the explicits rerolled, their count kept, the implicit kept.
        // Built to one side so a pool that cannot fill the count leaves the
        // item as it was -- an orb that quietly removed an affix would be a
        // worse Scouring.
        //
        // An item from before the caps existed can carry more explicits than
        // they allow (three prefixes, say). It is rerolled to the caps, and if
        // that loses it an affix and it has room for the implicit, it gets one
        // -- so a five-affix item stays five.
        RolledItem fresh;
        for (const auto& rolled : a_item.affixes) {
            if (rolled.affix && rolled.affix->Implicit()) {
                fresh.affixes.push_back(rolled);
            }
        }
        const int had = a_item.Explicits();
        const int wanted = (std::min)(had, ExplicitCap());

        auto pool = EligiblePool(a_table, ctx);
        FitPool(pool, fresh);
        int drawn = 0;
        while (drawn < wanted) {
            RolledAffix rolled;
            if (!DrawOne(pool, ctx, a_tuning, a_rng, rolled)) {
                break;
            }
            fresh.affixes.push_back(rolled);
            ++drawn;
            FitPool(pool, fresh);
        }
        if (drawn < wanted) {
            return CraftVerdict::kNoEligibleAffix;
        }
        if (had > wanted && !fresh.HasImplicit() && ctx.itemLevel >= a_tuning.fifthAffixMinLevel) {
            auto        implicits = ImplicitPool(a_table, ctx);
            RolledAffix rolled;
            FitPool(implicits, fresh);
            if (DrawOne(implicits, ctx, a_tuning, a_rng, rolled)) {
                fresh.affixes.push_back(rolled);
            }
        }
        a_item.affixes = std::move(fresh.affixes);
        Recount(a_item);
        return CraftVerdict::kOk;
    }
}
