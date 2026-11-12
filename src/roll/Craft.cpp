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
        bool DrawOneWith(std::vector<const Affix*>& a_pool, const RollContext& a_ctx,
            const std::array<int, kMaxAffixPoints>& a_tierWeights, Rng& a_rng, RolledAffix& a_out)
        {
            while (!a_pool.empty()) {
                switch (DrawAffix(a_pool, a_ctx, a_tierWeights, a_rng, a_out)) {
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

        bool DrawOne(std::vector<const Affix*>& a_pool, const RollContext& a_ctx,
            const Tuning& a_tuning, Rng& a_rng, RolledAffix& a_out)
        {
            return DrawOneWith(a_pool, a_ctx, TierWeightsFor(a_tuning, a_ctx.itemLevel), a_rng,
                a_out);
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

        // The tier row an affix on the item was rolled from.
        const AffixTier* TierOf(const RolledAffix& a_rolled)
        {
            if (!a_rolled.affix) {
                return nullptr;
            }
            for (const auto& tier : a_rolled.affix->tiers) {
                if (tier.tier == a_rolled.tier) {
                    return &tier;
                }
            }
            return nullptr;
        }

        // Whether any affix on the item has a range to move inside. Toggles and
        // fixed-value rows do not.
        bool AnyRange(const RolledItem& a_item)
        {
            return std::any_of(a_item.affixes.begin(), a_item.affixes.end(),
                [](const RolledAffix& a_rolled) {
                    const auto* tier = TierOf(a_rolled);
                    return tier && tier->minValue != tier->maxValue;
                });
        }

        // The item's implicit alone -- what Chaos and a Vaal reroll keep, and
        // what a refilled explicit pool has to stay clear of.
        RolledItem ImplicitOnly(const RolledItem& a_item)
        {
            RolledItem kept;
            kept.corrupted = a_item.corrupted;
            for (const auto& rolled : a_item.affixes) {
                if (rolled.affix && rolled.affix->Implicit()) {
                    kept.affixes.push_back(rolled);
                }
            }
            return kept;
        }

        std::vector<const Affix*> ImplicitsFor(const RolledItem& a_item, const RollContext& a_ctx,
            const AffixTable& a_table)
        {
            auto pool = ImplicitPool(a_table, a_ctx);
            FitPool(pool, a_item);
            return pool;
        }

        // The level floor on the Nth explicit affix.
        CraftVerdict GateFor(int a_count, int a_itemLevel, const Tuning& a_tuning)
        {
            if (a_count >= 6 && a_itemLevel < a_tuning.sixthAffixMinLevel) {
                return CraftVerdict::kSixthLocked;
            }
            if (a_count >= 5 && a_itemLevel < a_tuning.fifthAffixMinLevel) {
                return CraftVerdict::kFifthLocked;
            }
            return CraftVerdict::kOk;
        }

        // The count rule alone -- the part that needs no table.
        CraftVerdict CountRule(Orb a_orb, const RolledItem& a_item, int a_itemLevel,
            const Tuning& a_tuning)
        {
            // ★FIRST, AND FOR EVERY ORB. Corruption is the end of crafting on an
            // item; Scouring is not a way back.
            if (a_item.corrupted) {
                return CraftVerdict::kCorrupted;
            }

            const int explicits = a_item.Explicits();
            switch (a_orb) {
            case Orb::kTransmute:
            case Orb::kAlchemy:
                return a_item.affixes.empty() ? CraftVerdict::kOk : CraftVerdict::kNeedsNoAffixes;
            case Orb::kAugment:
                return explicits == 1 ? CraftVerdict::kOk : CraftVerdict::kNeedsOneAffix;
            case Orb::kExalt:
                if (explicits < 2) {
                    return CraftVerdict::kNeedsTwoToFour;
                }
                if (explicits >= ExplicitCap()) {
                    return CraftVerdict::kFull;
                }
                // ★THE SAME FLOORS THE ROLLER HOLDS. Red needs a fifth affix
                // and red starts at fifthAffixMinLevel -- an orb is not a way
                // round that, or round the sixth's.
                return GateFor(explicits + 1, a_itemLevel, a_tuning);
            case Orb::kChaos:
                return explicits >= 2 ? CraftVerdict::kOk : CraftVerdict::kNeedsTwoOrMore;
            case Orb::kScour:
                return a_item.affixes.empty() ? CraftVerdict::kNothingToRemove : CraftVerdict::kOk;
            case Orb::kDivine:
                if (a_item.affixes.empty()) {
                    return CraftVerdict::kNeedsAffixes;
                }
                return AnyRange(a_item) ? CraftVerdict::kOk : CraftVerdict::kNothingToReroll;
            case Orb::kVaal:
                return CraftVerdict::kOk;   // which outcomes apply needs the table
            }
            return CraftVerdict::kNotAffixable;
        }

        bool Adds(Orb a_orb) noexcept
        {
            return a_orb == Orb::kTransmute || a_orb == Orb::kAugment || a_orb == Orb::kAlchemy ||
                   a_orb == Orb::kExalt;
        }

        // The pool the next explicit draw on this item comes from.
        std::vector<const Affix*> PoolFor(const RolledItem& a_item, const RollContext& a_ctx,
            const AffixTable& a_table)
        {
            auto pool = EligiblePool(a_table, a_ctx);
            FitPool(pool, a_item);
            return pool;
        }

        // ---- Vaal -------------------------------------------------------------

        struct VaalChoices
        {
            bool destroy{ false };
            bool values{ false };
            bool implicit{ false };
            bool reroll{ false };

            // ★DESTRUCTION ALONE IS NOT A CHOICE. An item that could only be
            // destroyed -- a plain ring with no implicit to gain, say -- would
            // make the orb a delete button, so it is refused instead.
            [[nodiscard]] bool Any() const noexcept { return values || implicit || reroll; }
        };

        VaalChoices VaalChoicesFor(const RolledItem& a_item, const RollContext& a_ctx,
            const AffixTable& a_table, const Tuning& a_tuning)
        {
            VaalChoices choices;
            const auto& rules = a_tuning.vaal;
            choices.values = rules.weightValues > 0 && AnyRange(a_item);
            choices.implicit = rules.weightImplicit > 0 && !a_item.HasImplicit() &&
                               !ImplicitsFor(a_item, a_ctx, a_table).empty();
            if (rules.weightReroll > 0 && a_item.Explicits() > 0) {
                auto pool = EligiblePool(a_table, a_ctx);
                FitPool(pool, ImplicitOnly(a_item));
                choices.reroll = !pool.empty();
            }
            choices.destroy = rules.canDestroy && rules.weightDestroy > 0 && choices.Any();
            return choices;
        }

        // Every value moved toward the strong end of its tier (maxValue, in the
        // CSV's own sense: a cost reduction's "max" is the more negative one)
        // or toward the weak end. Uniform between where it is and that end.
        void ShiftValues(RolledItem& a_item, bool a_up, Rng& a_rng)
        {
            for (auto& rolled : a_item.affixes) {
                const auto* tier = TierOf(rolled);
                if (!tier || tier->minValue == tier->maxValue) {
                    continue;
                }
                const float end = a_up ? tier->maxValue : tier->minValue;
                const float lo = std::min(rolled.value, end);
                const float hi = std::max(rolled.value, end);
                if (lo == hi) {
                    continue;
                }
                std::uniform_real_distribution<float> dist{ lo, hi };
                rolled.value = dist(a_rng);
            }
        }

        // The explicits become other affixes, as many as there were, at tiers
        // drawn evenly from the ones the item level has unlocked. The affixes
        // it had are kept out of the draw while the pool can still fill the
        // count without them. False -- and a_item untouched -- when it cannot
        // be filled at all.
        bool VaalReroll(RolledItem& a_item, const RollContext& a_ctx, const AffixTable& a_table,
            Rng& a_rng)
        {
            RolledItem fresh = ImplicitOnly(a_item);
            const int  wanted = std::min(a_item.Explicits(), ExplicitCap());

            auto pool = EligiblePool(a_table, a_ctx);
            FitPool(pool, fresh);

            auto different = pool;
            std::erase_if(different, [&](const Affix* a_candidate) {
                return std::any_of(a_item.affixes.begin(), a_item.affixes.end(),
                    [&](const RolledAffix& a_have) { return a_have.affix == a_candidate; });
            });
            if (static_cast<int>(different.size()) >= wanted) {
                pool = std::move(different);
            }

            const std::array<int, kMaxAffixPoints> flat{ 1, 1, 1 };
            int                                    drawn = 0;
            while (drawn < wanted) {
                RolledAffix rolled;
                if (!DrawOneWith(pool, a_ctx, flat, a_rng, rolled)) {
                    break;
                }
                fresh.affixes.push_back(rolled);
                ++drawn;
                FitPool(pool, fresh);
            }
            if (drawn == 0) {
                return false;
            }
            a_item.affixes = std::move(fresh.affixes);
            return true;
        }

        CraftVerdict Vaal(RolledItem& a_item, const RollContext& a_ctx, const AffixTable& a_table,
            const Tuning& a_tuning, Rng& a_rng, CraftEffect& a_effect)
        {
            const auto choices = VaalChoicesFor(a_item, a_ctx, a_table, a_tuning);
            if (!choices.Any()) {
                return CraftVerdict::kNothingToCorrupt;
            }
            const auto& rules = a_tuning.vaal;

            enum Outcome { kDestroy, kValues, kImplicit, kReroll };
            const int weights[]{
                choices.destroy ? rules.weightDestroy : 0,
                choices.values ? rules.weightValues : 0,
                choices.implicit ? rules.weightImplicit : 0,
                choices.reroll ? rules.weightReroll : 0,
            };
            int total = 0;
            for (const int weight : weights) {
                total += weight;
            }
            std::uniform_int_distribution<int> dist{ 0, total - 1 };
            int                                pick = dist(a_rng);
            int                                outcome = kDestroy;
            for (int i = 0; i < 4; ++i) {
                if (pick < weights[i]) {
                    outcome = i;
                    break;
                }
                pick -= weights[i];
            }

            switch (outcome) {
            case kDestroy:
                a_effect = CraftEffect::kDestroyed;
                return CraftVerdict::kOk;   // a_item is left alone: there is no item
            case kValues:
                {
                    const bool up = (a_rng() & 1u) != 0;
                    ShiftValues(a_item, up, a_rng);
                    a_effect = up ? CraftEffect::kValuesUp : CraftEffect::kValuesDown;
                }
                break;
            case kImplicit:
                {
                    auto        pool = ImplicitsFor(a_item, a_ctx, a_table);
                    RolledAffix added;
                    if (!DrawOne(pool, a_ctx, a_tuning, a_rng, added)) {
                        return CraftVerdict::kNoEligibleAffix;   // a_item untouched
                    }
                    a_item.affixes.push_back(added);
                    a_effect = CraftEffect::kImplicitAdded;
                }
                break;
            case kReroll:
                if (!VaalReroll(a_item, a_ctx, a_table, a_rng)) {
                    return CraftVerdict::kNoEligibleAffix;
                }
                a_effect = CraftEffect::kRerolled;
                break;
            }
            a_item.corrupted = true;
            Recount(a_item);
            return CraftVerdict::kOk;
        }
    }

    const char* OrbName(Orb a_orb) noexcept
    {
        switch (a_orb) {
        case Orb::kTransmute: return "Orb of Transmutation";
        case Orb::kAugment:   return "Orb of Augmentation";
        case Orb::kAlchemy:   return "Orb of Alchemy";
        case Orb::kExalt:     return "Exalted Orb";
        case Orb::kChaos:     return "Chaos Orb";
        case Orb::kScour:     return "Orb of Scouring";
        case Orb::kDivine:    return "Divine Orb";
        case Orb::kVaal:      return "Vaal Orb";
        }
        return "?";
    }

    const char* OrbKey(Orb a_orb) noexcept
    {
        switch (a_orb) {
        case Orb::kTransmute: return "transmute";
        case Orb::kAugment:   return "augment";
        case Orb::kAlchemy:   return "alchemy";
        case Orb::kExalt:     return "exalt";
        case Orb::kChaos:     return "chaos";
        case Orb::kScour:     return "scour";
        case Orb::kDivine:    return "divine";
        case Orb::kVaal:      return "vaal";
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
        case CraftVerdict::kFifthLocked:      return "Item level too low for a fifth affix";
        case CraftVerdict::kNoEligibleAffix:  return "No affix left that this item can roll";
        case CraftVerdict::kSixthLocked:      return "Item level too low for a sixth affix";
        case CraftVerdict::kNeedsAffixes:     return "Needs an item with affixes";
        case CraftVerdict::kNothingToReroll:  return "This item's affixes have no values to reroll";
        case CraftVerdict::kCorrupted:        return "Corrupted items cannot be crafted";
        case CraftVerdict::kNothingToCorrupt: return "A Vaal Orb can do nothing to this item";
        }
        return "";
    }

    const char* EffectName(CraftEffect a_effect) noexcept
    {
        switch (a_effect) {
        case CraftEffect::kNone:          return "";
        case CraftEffect::kDestroyed:     return "destroyed";
        case CraftEffect::kValuesUp:      return "values raised";
        case CraftEffect::kValuesDown:    return "values lowered";
        case CraftEffect::kImplicitAdded: return "implicit added";
        case CraftEffect::kRerolled:      return "affixes rerolled";
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
        if (verdict != CraftVerdict::kOk || a_orb == Orb::kScour || a_orb == Orb::kDivine) {
            return verdict;
        }

        const auto ctx = CraftContext(a_ctx);
        if (a_orb == Orb::kVaal) {
            return VaalChoicesFor(a_item, ctx, a_table, a_tuning).Any()
                       ? CraftVerdict::kOk
                       : CraftVerdict::kNothingToCorrupt;
        }

        // Anything that draws needs something to draw from. For Chaos the
        // item's explicits go back in the pool, so only the implicit is held
        // against it.
        bool any = false;
        if (Adds(a_orb)) {
            any = !PoolFor(a_item, ctx, a_table).empty();
        } else {
            auto pool = EligiblePool(a_table, ctx);
            FitPool(pool, ImplicitOnly(a_item));
            any = !pool.empty();
        }
        return any ? CraftVerdict::kOk : CraftVerdict::kNoEligibleAffix;
    }

    CraftVerdict Craft(Orb a_orb, RolledItem& a_item, const RollContext& a_ctx,
        const AffixTable& a_table, const Tuning& a_tuning, Rng& a_rng, CraftEffect* a_effect)
    {
        CraftEffect  scratch = CraftEffect::kNone;
        CraftEffect& effect = a_effect ? *a_effect : scratch;
        effect = CraftEffect::kNone;

        const auto verdict = CanCraft(a_orb, a_item, a_ctx, a_table, a_tuning);
        if (verdict != CraftVerdict::kOk) {
            return verdict;
        }
        const auto ctx = CraftContext(a_ctx);

        if (a_orb == Orb::kScour) {
            // Everything, an implicit included: Scouring returns the plain
            // base, which is what the brief asks of it.
            a_item.affixes.clear();
            a_item.tier = 0;
            return CraftVerdict::kOk;
        }

        if (a_orb == Orb::kDivine) {
            // Each value drawn afresh from the tier row it already sits in.
            // Tiers, points and therefore the colour do not move.
            for (auto& rolled : a_item.affixes) {
                const auto* tier = TierOf(rolled);
                if (!tier || tier->minValue == tier->maxValue) {
                    continue;
                }
                const float lo = std::min(tier->minValue, tier->maxValue);
                const float hi = std::max(tier->minValue, tier->maxValue);
                std::uniform_real_distribution<float> dist{ lo, hi };
                rolled.value = dist(a_rng);
            }
            return CraftVerdict::kOk;
        }

        if (a_orb == Orb::kVaal) {
            return Vaal(a_item, ctx, a_table, a_tuning, a_rng, effect);
        }

        if (a_orb == Orb::kAlchemy) {
            // Four at once, built to one side. A pool too small for four --
            // jewellery in a trimmed table -- gives what it can; none at all
            // leaves the item plain and the orb unspent.
            RolledItem fresh;
            auto       pool = PoolFor(fresh, ctx, a_table);
            const int  wanted = std::min(kAlchemyAffixes, ExplicitCap());
            while (static_cast<int>(fresh.affixes.size()) < wanted) {
                RolledAffix rolled;
                if (!DrawOne(pool, ctx, a_tuning, a_rng, rolled)) {
                    break;
                }
                fresh.affixes.push_back(rolled);
                FitPool(pool, fresh);
            }
            if (fresh.affixes.empty()) {
                return CraftVerdict::kNoEligibleAffix;
            }
            a_item.affixes = std::move(fresh.affixes);
            Recount(a_item);
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

        // Chaos: the explicits rerolled, their count kept, an implicit kept.
        // Built to one side so a pool that cannot fill the count leaves the
        // item as it was -- an orb that quietly removed an affix would be a
        // worse Scouring.
        //
        // An item can carry more explicits of one kind than the caps now allow
        // (the caps are a setting). It is rerolled to the caps.
        RolledItem fresh = ImplicitOnly(a_item);
        const int  had = a_item.Explicits();
        const int  wanted = (std::min)(had, ExplicitCap());

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
        a_item.affixes = std::move(fresh.affixes);
        Recount(a_item);
        return CraftVerdict::kOk;
    }
}
