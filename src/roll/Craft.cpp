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

        // The item's implicits alone -- what Chaos, Alchemy, Scouring and a
        // Vaal reroll keep, and what a refilled explicit pool has to stay
        // clear of.
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
            FitImplicitPool(pool, a_item);
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
            // ★FIRST, AND FOR EVERY ORB. Corruption locks the whole item:
            // Divine and Scouring included, implicits included.
            if (a_item.corrupted) {
                return CraftVerdict::kCorrupted;
            }

            const int  explicits = a_item.Explicits();
            const auto rarity = RarityOf(explicits);
            switch (a_orb) {
            case Orb::kTransmute:
            case Orb::kAlchemy:
                return rarity == Rarity::kNormal ? CraftVerdict::kOk : CraftVerdict::kNeedsNormal;
            case Orb::kAugment:
                return explicits == 1 ? CraftVerdict::kOk : CraftVerdict::kNeedsOneAffix;
            case Orb::kRegal:
                if (rarity != Rarity::kMagic) {
                    return CraftVerdict::kNeedsMagic;
                }
                if (explicits != 2) {
                    return CraftVerdict::kNeedsMagicTwo;
                }
                return GateFor(explicits + 1, a_itemLevel, a_tuning);
            case Orb::kExalt:
                if (rarity != Rarity::kRare) {
                    return CraftVerdict::kNeedsRare;
                }
                if (explicits >= ExplicitCap()) {
                    return CraftVerdict::kFull;
                }
                // ★THE SAME FLOORS THE ROLLER HOLDS. An orb is not a way round
                // the fifth's or the sixth's item level.
                return GateFor(explicits + 1, a_itemLevel, a_tuning);
            case Orb::kChaos:
                return rarity == Rarity::kRare ? CraftVerdict::kOk : CraftVerdict::kNeedsRare;
            case Orb::kScour:
                return explicits == 0 ? CraftVerdict::kNothingToRemove : CraftVerdict::kOk;
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
            return a_orb == Orb::kTransmute || a_orb == Orb::kAugment || a_orb == Orb::kRegal ||
                   a_orb == Orb::kExalt;
        }

        // The caps the next explicit added by this orb lives under: a magic
        // result (Transmutation, Augmentation) is one prefix and one suffix.
        void CapsFor(Orb a_orb, int& a_maxPrefixes, int& a_maxSuffixes)
        {
            if (a_orb == Orb::kTransmute || a_orb == Orb::kAugment) {
                a_maxPrefixes = kMagicMaxPrefixes;
                a_maxSuffixes = kMagicMaxSuffixes;
            } else {
                a_maxPrefixes = MaxPrefixes();
                a_maxSuffixes = MaxSuffixes();
            }
        }

        // The pool the next explicit draw on this item comes from.
        std::vector<const Affix*> PoolFor(const RolledItem& a_item, const RollContext& a_ctx,
            const AffixTable& a_table, int a_maxPrefixes, int a_maxSuffixes)
        {
            auto pool = EligiblePool(a_table, a_ctx);
            FitPool(pool, a_item, a_maxPrefixes, a_maxSuffixes);
            return pool;
        }

        std::vector<const Affix*> PoolFor(const RolledItem& a_item, const RollContext& a_ctx,
            const AffixTable& a_table)
        {
            return PoolFor(a_item, a_ctx, a_table, MaxPrefixes(), MaxSuffixes());
        }

        // ---- Vaal -------------------------------------------------------------

        struct VaalChoices
        {
            bool destroy{ false };
            bool values{ false };
            bool reroll{ false };
            bool implicit{ false };   // a second implicit can actually be added

            // ★DESTRUCTION ALONE IS NOT A CHOICE. An item whose only weighted
            // outcome would be destruction is corrupted with nothing else
            // changed instead, so the orb is never a delete button.
            [[nodiscard]] bool Any() const noexcept { return values || reroll; }
        };

        VaalChoices VaalChoicesFor(const RolledItem& a_item, const RollContext& a_ctx,
            const AffixTable& a_table, const Tuning& a_tuning)
        {
            VaalChoices choices;
            const auto& rules = a_tuning.vaal;
            choices.values = rules.weightValues > 0 && AnyRange(a_item);
            if (rules.weightReroll > 0 && a_item.Explicits() > 0) {
                auto pool = EligiblePool(a_table, a_ctx);
                FitPool(pool, ImplicitOnly(a_item));
                choices.reroll = !pool.empty();
            }
            choices.destroy = rules.canDestroy && rules.weightDestroy > 0 && choices.Any();
            // ★ONLY ON AN ITEM THAT ALREADY HAS ONE, and never past two. Same
            // table, no implicit twice, nothing sharing a mod group.
            choices.implicit = a_item.Implicits() == 1 && kMaxImplicits >= 2 &&
                               !ImplicitsFor(a_item, a_ctx, a_table).empty();
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
        // count without them. The rarity's caps hold: a magic item stays one
        // prefix and one suffix. False -- and a_item untouched -- when it
        // cannot be filled at all.
        bool VaalReroll(RolledItem& a_item, const RollContext& a_ctx, const AffixTable& a_table,
            Rng& a_rng)
        {
            RolledItem fresh = ImplicitOnly(a_item);
            const int  wanted = std::min(a_item.Explicits(), ExplicitCap());
            const bool magic = RarityOf(wanted) == Rarity::kMagic;
            const int  capP = magic ? kMagicMaxPrefixes : MaxPrefixes();
            const int  capS = magic ? kMagicMaxSuffixes : MaxSuffixes();

            auto pool = EligiblePool(a_table, a_ctx);
            FitPool(pool, fresh, capP, capS);

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
                FitPool(pool, fresh, capP, capS);
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
            const auto  choices = VaalChoicesFor(a_item, a_ctx, a_table, a_tuning);
            const auto& rules = a_tuning.vaal;

            // ★THE SECOND-IMPLICIT CHANCE IS DRAWN FIRST, at a fixed share, so
            // the other outcomes keep their relative odds. When it cannot add
            // one, it is a no-op: the item is corrupted and nothing else moves.
            std::uniform_real_distribution<float> unit{ 0.0f, 1.0f };
            const float chance = std::clamp(rules.addImplicitChance, 0.0f, 1.0f);
            if (chance > 0.0f && unit(a_rng) < chance) {
                a_effect = CraftEffect::kCorruptedOnly;
                if (choices.implicit) {
                    auto        pool = ImplicitsFor(a_item, a_ctx, a_table);
                    RolledAffix added;
                    if (DrawOne(pool, a_ctx, a_tuning, a_rng, added)) {
                        a_item.affixes.push_back(added);
                        a_effect = CraftEffect::kImplicitAdded;
                    }
                }
                a_item.corrupted = true;
                Recount(a_item);
                return CraftVerdict::kOk;
            }

            enum Outcome { kDestroy, kValues, kReroll };
            const int weights[]{
                choices.destroy ? rules.weightDestroy : 0,
                choices.values ? rules.weightValues : 0,
                choices.reroll ? rules.weightReroll : 0,
            };
            int total = 0;
            for (const int weight : weights) {
                total += std::max(0, weight);
            }
            if (total <= 0) {
                a_effect = CraftEffect::kCorruptedOnly;
                a_item.corrupted = true;
                Recount(a_item);
                return CraftVerdict::kOk;
            }
            std::uniform_int_distribution<int> dist{ 0, total - 1 };
            int                                pick = dist(a_rng);
            int                                outcome = kDestroy;
            for (int i = 0; i < 3; ++i) {
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
            case kReroll:
                if (!VaalReroll(a_item, a_ctx, a_table, a_rng)) {
                    a_effect = CraftEffect::kCorruptedOnly;
                } else {
                    a_effect = CraftEffect::kRerolled;
                }
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
        case Orb::kRegal:     return "Regal Orb";
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
        case Orb::kRegal:     return "regal";
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
        case CraftVerdict::kNotAffixable:     return "This item cannot carry mods";
        case CraftVerdict::kNeedsNormal:      return "Needs a normal item (no mods)";
        case CraftVerdict::kNeedsOneAffix:    return "Needs a magic item with exactly 1 mod";
        case CraftVerdict::kNeedsRare:        return "Needs a rare item";
        case CraftVerdict::kNeedsMagicTwo:    return "Needs 2 mods; use Augmentation first";
        case CraftVerdict::kNeedsMagic:       return "Needs a magic item with 2 mods";
        case CraftVerdict::kNothingToRemove:  return "This item has no mods to remove";
        case CraftVerdict::kFull:             return "This item cannot hold another mod";
        case CraftVerdict::kFifthLocked:      return "Item level too low for a fifth mod";
        case CraftVerdict::kNoEligibleAffix:  return "No mod left that this item can roll";
        case CraftVerdict::kSixthLocked:      return "Item level too low for a sixth mod";
        case CraftVerdict::kNeedsAffixes:     return "Needs an item with mods or an implicit";
        case CraftVerdict::kNothingToReroll:  return "This item's values have nothing to reroll";
        case CraftVerdict::kCorrupted:        return "Corrupted";
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
        case CraftEffect::kImplicitAdded: return "second implicit added";
        case CraftEffect::kRerolled:      return "mods rerolled";
        case CraftEffect::kCorruptedOnly: return "nothing else changed";
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
        if (verdict != CraftVerdict::kOk || a_orb == Orb::kScour || a_orb == Orb::kDivine ||
            a_orb == Orb::kVaal) {
            // ★A VAAL ORB ALWAYS DOES SOMETHING to an uncorrupted item: at worst
            // it corrupts it and nothing else (VaalChoices).
            return verdict;
        }

        const auto ctx = CraftContext(a_ctx);

        // Anything that draws needs something to draw from. For Chaos and
        // Alchemy the item's explicits go back in the pool, so only the
        // implicits are held against it.
        bool any = false;
        if (Adds(a_orb)) {
            int capP = 0;
            int capS = 0;
            CapsFor(a_orb, capP, capS);
            any = !PoolFor(a_item, ctx, a_table, capP, capS).empty();
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
            // The explicits go; the implicits are the base's and stay.
            a_item.affixes = ImplicitOnly(a_item).affixes;
            Recount(a_item);
            return CraftVerdict::kOk;
        }

        if (a_orb == Orb::kDivine) {
            // Each value drawn afresh from the tier row it already sits in,
            // implicits included. Tiers, points and the rarity do not move.
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
            // Four at once, built to one side on top of the implicits. A pool
            // too small for four -- jewellery in a trimmed table -- gives what
            // it can; none at all leaves the item as it was and the orb unspent.
            RolledItem fresh = ImplicitOnly(a_item);
            auto       pool = PoolFor(fresh, ctx, a_table);
            const int  wanted = std::min(kAlchemyAffixes, ExplicitCap());
            int        drawn = 0;
            while (drawn < wanted) {
                RolledAffix rolled;
                if (!DrawOne(pool, ctx, a_tuning, a_rng, rolled)) {
                    break;
                }
                fresh.affixes.push_back(rolled);
                ++drawn;
                FitPool(pool, fresh);
            }
            if (drawn == 0) {
                return CraftVerdict::kNoEligibleAffix;
            }
            a_item.affixes = std::move(fresh.affixes);
            Recount(a_item);
            return CraftVerdict::kOk;
        }

        if (Adds(a_orb)) {
            int capP = 0;
            int capS = 0;
            CapsFor(a_orb, capP, capS);
            auto        pool = PoolFor(a_item, ctx, a_table, capP, capS);
            RolledAffix added;
            if (!DrawOne(pool, ctx, a_tuning, a_rng, added)) {
                return CraftVerdict::kNoEligibleAffix;   // a_item untouched
            }
            a_item.affixes.push_back(added);
            Recount(a_item);
            return CraftVerdict::kOk;
        }

        // Chaos: a rare's explicits rerolled, their count kept, the implicits
        // kept. Built to one side so a pool that cannot fill the count leaves
        // the item as it was -- an orb that quietly removed a mod would be a
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
