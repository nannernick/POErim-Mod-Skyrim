// =============================================================================
//  Roll harness -- a console program, not part of the plugin.
// =============================================================================
//  Rolls a large sample at several item levels and prints what actually comes
//  out: the point histogram, the rarity split (normal / magic / rare, by mod
//  count since 2.1.0), how often each affix appears, and the implicits.
//
//  This exists because loot balance is not a thing you can reason your way to.
//  You change a number, look at the distribution, and change it again -- and
//  doing that inside Skyrim costs a launch and a dungeon per attempt.
//
//    RollHarness [csv] [--samples N] [--seed N] [--levels 1,12,25,34,35,40] [--npc]
//
//  CRAFTING. The orbs are tuned the same way the drops are -- by looking:
//
//    RollHarness --craft "transmute,augment,regal,exalt*3,chaos*50,divine,vaal"
//        Starts every sample as a NORMAL item carrying its base implicit (as
//        every item is made since 2.1.0) and uses the orbs in that order;
//        orb*N repeats one. After each step: how many items the orb worked on,
//        why it failed on the rest, and the rarity split of the whole sample.
//        Every step checks the rules and prints "<-- BUG" on any break: caps
//        by rarity, level gates, mod groups, implicits (one from creation, a
//        second only from Vaal, never two of a group), Regal results, Chaos
//        counts, and that nothing ever changes a corrupted item or an item an
//        orb refused. (A run of the same orb is printed as one step.)
//          --bare       start without the implicit (the 1.2.0 stream)
//          --level N    one item level instead of the --levels list
//          --slots X    every sample is this kind of item, e.g. WEAPON|ONEHANDED
//          --chains N   how many items to run the chain on (same as --samples)
//          --vaal-keep  Vaal Orbs never destroy the item
//
//    RollHarness --drops
//        Simulates kills at each level and prints what they drop: orbs per
//        kill, the share of each orb, and how many kills one Divine takes.
//          --drop-mult X     the DIFFICULTY multiplier (1.0 Novice .. 1.75
//                            Legendary); 1.0 draws nothing extra
//          --global-mult X   OrbDropMultiplier from the INI
//          --orb-weights a,b,...  replaces the default weights (9 numbers)
//
//    RollHarness --selftest
//        Fixed checks that need no eyeballing: Regal refusals on 0, 1, 3 and
//        6 mods, every refusal leaves the item bit-identical, Chaos keeps 3-6,
//        the co-save round trip for 0 (legacy v1), 1 and 2 implicits and the
//        corrupted flag, and the difficulty multiplier's mean. Exit code 1 and
//        "<-- BUG" lines on failure.
//
//    --caps P,S         prefix and suffix caps (default 3,3)
//
//    RollHarness --chaos-until 18
//        Rolls an item the way a drop would, then spends Chaos Orbs until its
//        point total reaches that number. Prints how many orbs that takes.
//
//    --merge file.csv   merges an add-on table first (repeatable)
//
//  Both crafting modes also push every finished item through the save record
//  and back, and through the legacy tier match, and say how many survived --
//  so a table whose tier ranges overlap is caught here and not in a save.
// =============================================================================

#include "roll/Craft.h"
#include "roll/Drops.h"
#include "roll/Record.h"
#include "roll/Roll.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{
    struct Options
    {
        std::string      csv{ "data/DiabloLoot_affixes.csv" };
        int              samples{ 100000 };
        unsigned         seed{ 20260826 };
        // 34 and 35 straddle the fifth-affix floor, so the default run shows
        // red switching on -- and shows it absent one level below.
        // 39 and 40 do the same for the sixth affix.
        std::vector<int> levels{ 1, 12, 25, 34, 35, 39, 40 };
        bool             forNpc{ false };

        std::vector<std::string> merges;
        std::vector<roll::Orb>   chain;          // --craft
        int                      chaosTarget{ -1 };  // --chaos-until, a point total
        std::uint32_t            slots{ roll::kNone };   // --slots; kNone = the mixed stream
        bool                     drops{ false };     // --drops
        float                    dropMult{ 1.0f };   // --drop-mult (difficulty)
        float                    globalMult{ 1.0f }; // --global-mult (OrbDropMultiplier)
        std::vector<int>         orbWeights;         // --orb-weights
        bool                     bare{ false };      // --bare
        bool                     selftest{ false };  // --selftest
    };

    std::vector<std::string> SplitCommas(const char* a_text)
    {
        std::vector<std::string> out;
        std::string              token;
        for (const char* p = a_text;; ++p) {
            if (*p == ',' || *p == '\0') {
                if (!token.empty()) {
                    out.push_back(token);
                    token.clear();
                }
                if (*p == '\0') {
                    break;
                }
            } else {
                token += *p;
            }
        }
        return out;
    }

    std::vector<int> ParseLevels(const char* a_text)
    {
        std::vector<int> out;
        std::string      token;
        for (const char* p = a_text;; ++p) {
            if (*p == ',' || *p == '\0') {
                if (!token.empty()) {
                    out.push_back(std::atoi(token.c_str()));
                    token.clear();
                }
                if (*p == '\0') {
                    break;
                }
            } else {
                token += *p;
            }
        }
        return out;
    }

    void Bar(int a_count, int a_total, int a_width = 44)
    {
        const int filled = a_total > 0 ? (a_count * a_width) / a_total : 0;
        for (int i = 0; i < a_width; ++i) {
            std::fputc(i < filled ? '#' : ' ', stdout);
        }
    }

    // A representative spread of item slots, so the numbers reflect a mixed loot
    // stream rather than one archetype. Weapons and armor dominate real drops;
    // jewellery is rarer, and its affix pool is different enough to matter.
    struct SlotSample
    {
        const char*   label;
        std::uint32_t mask;
        int           share;
    };

    constexpr SlotSample kSlots[]{
        { "one-handed", roll::kWeapon | roll::kOneHanded, 20 },
        { "two-handed", roll::kWeapon | roll::kTwoHanded, 10 },
        { "bow", roll::kWeapon | roll::kBow, 8 },
        { "staff", roll::kWeapon | roll::kStaff, 2 },
        { "body armor", roll::kArmor | roll::kBody, 25 },
        { "shield", roll::kArmor | roll::kShield, 10 },
        { "boots", roll::kArmor | roll::kFeet, 8 },
        { "gauntlets", roll::kArmor | roll::kHands, 7 },
        { "ring", roll::kRing, 6 },
        { "amulet", roll::kAmulet, 4 },
    };

    std::uint32_t PickSlots(roll::Rng& a_rng)
    {
        int slotTotal = 0;
        for (const auto& slot : kSlots) {
            slotTotal += slot.share;
        }
        int pick = static_cast<int>(a_rng() % static_cast<unsigned>(slotTotal));
        for (const auto& slot : kSlots) {
            if (pick < slot.share) {
                return slot.mask;
            }
            pick -= slot.share;
        }
        return kSlots[0].mask;
    }

    // normal, magic, rare, corrupted (any rarity)
    using RarityCounts = std::array<int, 4>;

    void CountRarity(RarityCounts& a_counts, const roll::RolledItem& a_item)
    {
        a_counts[static_cast<std::size_t>(roll::RarityOf(a_item)) - 1]++;
        if (a_item.corrupted) {
            a_counts[3]++;
        }
    }

    void PrintRarity(const RarityCounts& a_counts, int a_total)
    {
        static constexpr const char* kNames[]{ "normal", "magic", "rare", "corrupt" };
        for (std::size_t b = 0; b < a_counts.size(); ++b) {
            const int count = a_counts[b];
            std::printf("      %-7s %6d  %5.1f%%  ", kNames[b], count,
                a_total > 0 ? 100.0 * count / a_total : 0.0);
            Bar(count, a_total, 30);
            std::printf("\n");
        }
    }

    bool SameItem(const roll::RolledItem& a_lhs, const roll::RolledItem& a_rhs)
    {
        if (a_lhs.tier != a_rhs.tier || a_lhs.corrupted != a_rhs.corrupted ||
            a_lhs.affixes.size() != a_rhs.affixes.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a_lhs.affixes.size(); ++i) {
            const auto& l = a_lhs.affixes[i];
            const auto& r = a_rhs.affixes[i];
            if (l.affix != r.affix || l.tier != r.tier || l.points != r.points || l.value != r.value ||
                l.duration != r.duration || l.area != r.area) {
                return false;
            }
        }
        return true;
    }

    // A normal item as the game now makes one: its base implicit and nothing else.
    roll::RolledItem NormalBase(const roll::AffixTable& a_table, const roll::RollContext& a_ctx,
        roll::Rng& a_rng, bool a_bare)
    {
        if (a_bare) {
            return {};
        }
        roll::Tuning tuning;
        for (auto& band : tuning.bands) {
            band.countWeights.fill(0);
            band.countWeights[0] = 1;
        }
        return roll::Roll(a_table, a_ctx, tuning, a_rng);
    }

    // Every rule an item must keep, whatever was done to it. Returns the
    // number of broken rules (0 = fine).
    int RuleBreaks(const roll::RolledItem& a_item, int a_level, const roll::Tuning& a_tuning,
        bool a_vaalTouched)
    {
        int        broken = 0;
        const int  p = a_item.Count(roll::AffixKind::kPrefix);
        const int  s = a_item.Count(roll::AffixKind::kSuffix);
        const int  imp = a_item.Implicits();
        const auto rarity = roll::RarityOf(a_item);
        if (p > roll::MaxPrefixes() || s > roll::MaxSuffixes()) {
            ++broken;
        }
        if (rarity == roll::Rarity::kMagic && (p > roll::kMagicMaxPrefixes || s > roll::kMagicMaxSuffixes)) {
            ++broken;
        }
        if ((a_item.Explicits() >= 5 && a_level < a_tuning.fifthAffixMinLevel) ||
            (a_item.Explicits() >= 6 && a_level < a_tuning.sixthAffixMinLevel)) {
            ++broken;
        }
        if (imp > roll::kMaxImplicits || (imp >= 2 && !a_vaalTouched) || (imp >= 2 && !a_item.corrupted)) {
            ++broken;
        }
        for (std::size_t a = 0; a < a_item.affixes.size(); ++a) {
            for (std::size_t b = a + 1; b < a_item.affixes.size(); ++b) {
                if (a_item.affixes[a].affix == a_item.affixes[b].affix ||
                    a_item.affixes[a].affix->group == a_item.affixes[b].affix->group) {
                    ++broken;
                }
            }
        }
        return broken;
    }

    // What the save does to an item, done here: record, back to a roll, and
    // the tier of every affix recovered from its value alone.
    struct RecordCheck
    {
        long long items{ 0 };
        long long roundTripBad{ 0 };
        long long affixes{ 0 };
        long long tierMatchBad{ 0 };

        void Add(const roll::RolledItem& a_item, int a_level, const roll::AffixTable& a_table)
        {
            ++items;
            const auto       record = roll::MakeRecord(a_item, a_level);
            roll::RolledItem back;
            // Through the co-save's own byte layout (version 2), and back.
            std::vector<std::uint8_t> bytes;
            roll::WriteItemRecordV2([&](const void* a_src, std::size_t a_n) {
                const auto* b = static_cast<const std::uint8_t*>(a_src);
                bytes.insert(bytes.end(), b, b + a_n);
                return true;
            }, record);
            std::size_t      at = 0;
            roll::ItemRecord read;
            const bool       parsed = roll::ReadItemRecord([&](void* a_dst, std::size_t a_n) {
                if (at + a_n > bytes.size()) {
                    return false;
                }
                std::memcpy(a_dst, bytes.data() + at, a_n);
                at += a_n;
                return true;
            }, roll::kItemRecordV2, read);
            // Implicits come back first, as a roll makes them; compare by kind.
            roll::RolledItem ordered = a_item;
            std::stable_partition(ordered.affixes.begin(), ordered.affixes.end(),
                [](const roll::RolledAffix& a_r) { return a_r.affix->Implicit(); });
            bool same = parsed && at == bytes.size() && roll::ToRolled(read, a_table, back) &&
                        SameItem(back, ordered);
            if (!same) {
                ++roundTripBad;
            }
            for (const auto& rolled : a_item.affixes) {
                ++affixes;
                const auto* tier = roll::MatchTier(*rolled.affix, rolled.value, rolled.duration,
                    rolled.area);
                if (!tier || tier->tier != rolled.tier) {
                    ++tierMatchBad;
                }
            }
        }

        void Print() const
        {
            std::printf("  save record round trip: %lld of %lld item(s) came back identical\n",
                items - roundTripBad, items);
            std::printf("  legacy tier match:      %lld of %lld affix(es) recovered their tier "
                        "from the value alone%s\n",
                affixes - tierMatchBad, affixes,
                tierMatchBad ? "  <-- tier ranges overlap in this table" : "");
        }
    };

    void RunChain(const Options& a_options, const roll::AffixTable& a_table,
        const roll::Tuning& a_tuning)
    {
        std::printf("crafting chain:");
        for (const auto orb : a_options.chain) {
            std::printf(" %s", roll::OrbKey(orb));
        }
        std::printf("\n  every sample starts as a normal item%s\n\n",
            a_options.bare ? " with nothing on it (--bare)" : " carrying its base implicit");

        int totalBugs = 0;
        for (const int level : a_options.levels) {
            roll::Rng rng{ a_options.seed + static_cast<unsigned>(level) };

            std::vector<roll::RolledItem> items(static_cast<std::size_t>(a_options.samples));
            std::vector<std::uint32_t>    slots(items.size());
            std::vector<char>             destroyed(items.size(), 0);
            std::vector<char>             vaalTouched(items.size(), 0);
            for (std::size_t i = 0; i < items.size(); ++i) {
                slots[i] = a_options.slots != roll::kNone ? a_options.slots : PickSlots(rng);
                items[i] = NormalBase(a_table, { level, slots[i], false }, rng, a_options.bare);
            }
            int startImplicitless = 0;
            for (const auto& item : items) {
                startImplicitless += item.Implicits() == 0 ? 1 : 0;
            }
            std::printf("================ item level %d ================\n", level);
            if (!a_options.bare) {
                std::printf("  start: %d of %d item(s) carry their base implicit%s\n",
                    a_options.samples - startImplicitless, a_options.samples,
                    startImplicitless ? "  (the rest: no implicit row for that kind of item)" : "");
            }

            int step = 0;
            for (std::size_t at = 0; at < a_options.chain.size();) {
                // A run of the same orb ("chaos*50") is one step: its uses are
                // added up, and the split is the sample after the last of them.
                const auto  orb = a_options.chain[at];
                std::size_t run = 1;
                while (at + run < a_options.chain.size() && a_options.chain[at + run] == orb) {
                    ++run;
                }
                at += run;

                std::map<roll::CraftVerdict, long long> verdicts;
                std::map<roll::CraftEffect, long long>  effects;
                long long refusedChanged = 0;   // a refusal that changed the item
                long long corruptChanged = 0;   // anything that changed a corrupted item
                long long regalWrong = 0;       // a Regal result that is not exactly 3 mods, rare
                long long chaosWrong = 0;       // a Chaos result whose count moved
                long long augmentWrong = 0;     // Augmentation leaving the item not magic with 2
                long long scourWrong = 0;       // Scouring touching an implicit or leaving a mod
                std::map<int, long long> regalRefusedAt;   // explicit count -> refusals
                for (std::size_t use = 0; use < run; ++use) {
                    for (std::size_t i = 0; i < items.size(); ++i) {
                        if (destroyed[i]) {
                            continue;
                        }
                        const roll::RollContext ctx{ level, slots[i], false };
                        const roll::RolledItem  before = items[i];
                        roll::CraftEffect       effect = roll::CraftEffect::kNone;
                        const auto verdict = roll::Craft(orb, items[i], ctx, a_table, a_tuning, rng,
                            &effect);
                        verdicts[verdict]++;
                        if (verdict == roll::CraftVerdict::kOk && effect != roll::CraftEffect::kNone) {
                            effects[effect]++;
                        }
                        if (verdict != roll::CraftVerdict::kOk && !SameItem(before, items[i])) {
                            ++refusedChanged;
                        }
                        if (before.corrupted && (verdict == roll::CraftVerdict::kOk || !SameItem(before, items[i]))) {
                            ++corruptChanged;
                        }
                        if (orb == roll::Orb::kRegal) {
                            if (verdict == roll::CraftVerdict::kOk) {
                                if (items[i].Explicits() != 3 || roll::RarityOf(items[i]) != roll::Rarity::kRare) {
                                    ++regalWrong;
                                }
                                // Everything it had is still there, unchanged.
                                for (const auto& had : before.affixes) {
                                    const bool kept = std::any_of(items[i].affixes.begin(), items[i].affixes.end(),
                                        [&](const roll::RolledAffix& a_now) {
                                            return a_now.affix == had.affix && a_now.tier == had.tier &&
                                                   a_now.value == had.value;
                                        });
                                    if (!kept) {
                                        ++regalWrong;
                                    }
                                }
                            } else {
                                regalRefusedAt[before.Explicits()]++;
                            }
                        }
                        if (orb == roll::Orb::kChaos && verdict == roll::CraftVerdict::kOk &&
                            items[i].Explicits() != before.Explicits()) {
                            ++chaosWrong;
                        }
                        if (orb == roll::Orb::kAugment && verdict == roll::CraftVerdict::kOk &&
                            (items[i].Explicits() != 2 || roll::RarityOf(items[i]) != roll::Rarity::kMagic)) {
                            ++augmentWrong;
                        }
                        if (orb == roll::Orb::kScour && verdict == roll::CraftVerdict::kOk &&
                            (items[i].Explicits() != 0 || items[i].Implicits() != before.Implicits())) {
                            ++scourWrong;
                        }
                        if (orb == roll::Orb::kVaal && verdict == roll::CraftVerdict::kOk) {
                            vaalTouched[i] = 1;
                        }
                        if (effect == roll::CraftEffect::kDestroyed) {
                            destroyed[i] = 1;
                        }
                    }
                }

                RarityCounts rarity{};
                long long    affixes = 0;
                long long    implicitsTotal = 0;
                int          alive = 0;
                int          corrupted = 0;
                int          broken = 0;
                std::array<int, roll::kMaxImplicits + 1> implicitHist{};
                for (std::size_t i = 0; i < items.size(); ++i) {
                    if (destroyed[i]) {
                        continue;
                    }
                    const auto& item = items[i];
                    ++alive;
                    corrupted += item.corrupted ? 1 : 0;
                    CountRarity(rarity, item);
                    affixes += item.Explicits();
                    implicitsTotal += item.Implicits();
                    implicitHist[static_cast<std::size_t>(std::clamp(item.Implicits(), 0, roll::kMaxImplicits))]++;
                    broken += RuleBreaks(item, level, a_tuning, vaalTouched[i] != 0);
                }

                const long long uses = static_cast<long long>(run) * a_options.samples;
                std::printf("  step %d: %s%s -- worked %lld of %lld use(s) (%.1f%%)\n", ++step,
                    roll::OrbName(orb), run > 1 ? (" x" + std::to_string(run)).c_str() : "",
                    verdicts[roll::CraftVerdict::kOk], uses,
                    100.0 * static_cast<double>(verdicts[roll::CraftVerdict::kOk]) /
                        static_cast<double>(uses));
                for (const auto& [verdict, count] : verdicts) {
                    if (verdict != roll::CraftVerdict::kOk) {
                        std::printf("      refused %8lld: %s\n", count, roll::Reason(verdict));
                    }
                }
                for (const auto& [effect, count] : effects) {
                    std::printf("      outcome %8lld: %s (%.1f%%)\n", count, roll::EffectName(effect),
                        100.0 * static_cast<double>(count) /
                            static_cast<double>(verdicts[roll::CraftVerdict::kOk]));
                }
                if (orb == roll::Orb::kRegal && !regalRefusedAt.empty()) {
                    std::printf("      regal refused on:");
                    for (const auto& [mods, count] : regalRefusedAt) {
                        std::printf("  %d mod(s) x%lld", mods, count);
                    }
                    std::printf("\n");
                }
                PrintRarity(rarity, alive);
                std::printf("      items left: %d of %d, corrupted %d; mean mods/item: %.2f, mean "
                            "implicits/item: %.2f (0:%d 1:%d 2:%d)\n",
                    alive, a_options.samples, corrupted,
                    alive > 0 ? static_cast<double>(affixes) / alive : 0.0,
                    alive > 0 ? static_cast<double>(implicitsTotal) / alive : 0.0, implicitHist[0],
                    implicitHist[1], implicitHist[2]);
                const long long bugs = broken + refusedChanged + corruptChanged + regalWrong + chaosWrong +
                                       augmentWrong + scourWrong;
                totalBugs += static_cast<int>(bugs);
                std::printf("      rule check: %d broken rule(s) (caps, gates, groups, implicits), "
                            "%lld refusal(s) that changed an item, %lld change(s) to a corrupted item, "
                            "%lld bad Regal, %lld bad Chaos, %lld bad Augment, %lld bad Scour%s\n",
                    broken, refusedChanged, corruptChanged, regalWrong, chaosWrong, augmentWrong,
                    scourWrong, bugs ? "  <-- BUG" : "");
            }

            RecordCheck check;
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (!destroyed[i]) {
                    check.Add(items[i], level, a_table);
                }
            }
            check.Print();
            std::printf("\n");
        }
        if (totalBugs) {
            std::printf("TOTAL: %d rule break(s)  <-- BUG\n", totalBugs);
        }
    }

    void RunChaosUntil(const Options& a_options, const roll::AffixTable& a_table,
        const roll::Tuning& a_tuning)
    {
        // A cap, because some targets are unreachable -- red below the fifth
        // affix's level, or a band above what the item's affix count can sum
        // to -- and the harness should say so rather than spin.
        constexpr int kCap = 2000;
        const int     target = a_options.chaosTarget;

        std::printf("chaos until %d points or more (cap %d orbs per item)\n", target, kCap);
        std::printf("  each sample is rolled as a drop first; items that drop below rare\n"
                    "  cannot take a Chaos Orb and are counted apart\n\n");

        for (const int level : a_options.levels) {
            roll::Rng rng{ a_options.seed + static_cast<unsigned>(level) };

            int                    already = 0;
            int                    ineligible = 0;
            int                    capped = 0;
            std::vector<int>       spent;
            RecordCheck            check;
            spent.reserve(static_cast<std::size_t>(a_options.samples));

            for (int i = 0; i < a_options.samples; ++i) {
                const roll::RollContext ctx{ level,
                    a_options.slots != roll::kNone ? a_options.slots : PickSlots(rng), false };
                auto                    item = roll::Roll(a_table, ctx, a_tuning, rng);
                if (item.tier >= target) {
                    ++already;
                    check.Add(item, level, a_table);
                    continue;
                }
                if (roll::CanCraft(roll::Orb::kChaos, item, ctx, a_table, a_tuning) !=
                    roll::CraftVerdict::kOk) {
                    ++ineligible;
                    continue;
                }
                int used = 0;
                while (used < kCap && item.tier < target) {
                    if (roll::Craft(roll::Orb::kChaos, item, ctx, a_table, a_tuning, rng) !=
                        roll::CraftVerdict::kOk) {
                        break;
                    }
                    ++used;
                }
                if (item.tier >= target) {
                    spent.push_back(used);
                    check.Add(item, level, a_table);
                } else {
                    ++capped;
                }
            }

            std::printf("================ item level %d ================\n", level);
            std::printf("  dropped at %d points or more already: %d\n", target, already);
            std::printf("  dropped below rare:                  %d\n", ineligible);
            std::printf("  never got there within the cap:      %d\n", capped);
            if (!spent.empty()) {
                std::sort(spent.begin(), spent.end());
                long long total = 0;
                for (const int used : spent) {
                    total += used;
                }
                const auto at = [&](double a_q) {
                    return spent[static_cast<std::size_t>(a_q * static_cast<double>(spent.size() - 1))];
                };
                std::printf("  reached it by crafting:              %zu\n", spent.size());
                std::printf("    orbs spent: mean %.1f, median %d, 90th percentile %d, worst %d\n",
                    static_cast<double>(total) / static_cast<double>(spent.size()), at(0.5), at(0.9),
                    spent.back());
            }
            check.Print();
            std::printf("\n");
        }
    }
}

namespace
{
    void RunDrops(const Options& a_options)
    {
        roll::DropTuning tuning;
        tuning.multiplier = a_options.globalMult;
        if (!a_options.orbWeights.empty()) {
            for (std::size_t i = 0; i < tuning.weights.size(); ++i) {
                tuning.weights[i] = i < a_options.orbWeights.size() ? a_options.orbWeights[i] : 0;
            }
        }

        std::printf("orb drops: %d kill(s) per level, multiplier x%.2f\n", a_options.samples,
            tuning.multiplier);
        if (a_options.dropMult != 1.0f) {
            std::printf("  difficulty multiplier x%.2f\n", a_options.dropMult);
        }
        std::printf("  chance %.0f%%, count %.2f + %.3f per level (cap %.1f)\n\n",
            100.0 * tuning.chance, tuning.baseCount, tuning.countPerLevel, tuning.maxCount);

        for (const int level : a_options.levels) {
            roll::Rng rng{ a_options.seed + static_cast<unsigned>(level) };

            std::array<long long, roll::kOrbCount> counts{};
            long long                              total = 0;
            int                                    empty = 0;
            std::array<int, 9>                     perKill{};   // 0..7, 8+
            for (int i = 0; i < a_options.samples; ++i) {
                const auto drop = roll::RollOrbDrop(level, tuning, rng, 1.0f, a_options.dropMult);
                if (drop.total == 0) {
                    ++empty;
                }
                perKill[static_cast<std::size_t>(std::min(drop.total, 8))]++;
                total += drop.total;
                for (int orb = 0; orb < roll::kOrbCount; ++orb) {
                    counts[static_cast<std::size_t>(orb)] += drop.counts[static_cast<std::size_t>(orb)];
                }
            }

            std::printf("================ enemy level %d ================\n", level);
            std::printf("  orbs per kill: %.2f   kills with none: %.1f%%\n",
                static_cast<double>(total) / a_options.samples, 100.0 * empty / a_options.samples);
            std::printf("  per kill:");
            for (std::size_t n = 0; n < perKill.size(); ++n) {
                if (perKill[n] > 0) {
                    std::printf("  %zu%s: %.1f%%", n, n == 8 ? "+" : "",
                        100.0 * perKill[n] / a_options.samples);
                }
            }
            std::printf("\n");
            for (int orb = 0; orb < roll::kOrbCount; ++orb) {
                const long long count = counts[static_cast<std::size_t>(orb)];
                std::printf("    %-22s %8lld  %5.1f%% of orbs  ", roll::OrbName(static_cast<roll::Orb>(orb)),
                    count, total > 0 ? 100.0 * static_cast<double>(count) / static_cast<double>(total) : 0.0);
                if (count > 0) {
                    std::printf("one per %.1f kill(s)\n", static_cast<double>(a_options.samples) /
                                                              static_cast<double>(count));
                } else {
                    std::printf("never\n");
                }
            }
            std::printf("\n");
        }
    }
}

namespace
{
    // Builds an item of exactly a_mods explicits (and its base implicit) on
    // a_slots by Transmute/Augment/Regal/Exalt, or Alchemy for 4+.
    bool BuildWithMods(int a_mods, const roll::AffixTable& a_table, const roll::RollContext& a_ctx,
        const roll::Tuning& a_tuning, roll::Rng& a_rng, roll::RolledItem& a_out)
    {
        a_out = NormalBase(a_table, a_ctx, a_rng, false);
        const auto use = [&](roll::Orb a_orb) {
            return roll::Craft(a_orb, a_out, a_ctx, a_table, a_tuning, a_rng) == roll::CraftVerdict::kOk;
        };
        if (a_mods >= 1 && !use(roll::Orb::kTransmute)) return false;
        if (a_mods >= 2 && !use(roll::Orb::kAugment)) return false;
        if (a_mods >= 3 && !use(roll::Orb::kRegal)) return false;
        for (int m = 4; m <= a_mods; ++m) {
            if (!use(roll::Orb::kExalt)) return false;
        }
        return a_out.Explicits() == a_mods;
    }

    int RunSelfTest(const Options& a_options, const roll::AffixTable& a_table, const roll::Tuning& a_tuning)
    {
        int        failures = 0;
        const auto fail = [&](const char* a_what) {
            std::printf("  FAIL: %s  <-- BUG\n", a_what);
            ++failures;
        };
        roll::Rng  rng{ a_options.seed };
        const roll::RollContext ctx{ 40, roll::kWeapon | roll::kOneHanded, false };

        // ---- Regal refusals on 0, 1, 3 and 6 mods, bit-identical, not consumed
        std::printf("Regal refusals (item level 40, one-handed weapon):\n");
        for (const int mods : { 0, 1, 3, 6 }) {
            roll::RolledItem item;
            if (!BuildWithMods(mods, a_table, ctx, a_tuning, rng, item)) {
                fail("could not build the test item");
                continue;
            }
            const auto before = item;
            const auto verdict = roll::Craft(roll::Orb::kRegal, item, ctx, a_table, a_tuning, rng);
            std::printf("  %d mod(s): %s -- \"%s\"%s\n", mods,
                verdict == roll::CraftVerdict::kOk ? "ACCEPTED" : "refused", roll::Reason(verdict),
                SameItem(before, item) ? ", item unchanged" : ", ITEM CHANGED");
            if (verdict == roll::CraftVerdict::kOk || !SameItem(before, item)) {
                fail("Regal must refuse and leave the item alone");
            }
            if (mods == 1 && verdict != roll::CraftVerdict::kNeedsMagicTwo) {
                fail("Regal on 1 mod must say: Needs 2 mods; use Augmentation first");
            }
        }
        // Corrupted / magic-with-2 accepted
        {
            roll::RolledItem item;
            if (BuildWithMods(2, a_table, ctx, a_tuning, rng, item)) {
                auto corrupt = item;
                corrupt.corrupted = true;
                const auto before = corrupt;
                if (roll::Craft(roll::Orb::kRegal, corrupt, ctx, a_table, a_tuning, rng) != roll::CraftVerdict::kCorrupted ||
                    !SameItem(before, corrupt)) {
                    fail("Regal on a corrupted item must refuse with Corrupted");
                }
                const auto had = item;
                if (roll::Craft(roll::Orb::kRegal, item, ctx, a_table, a_tuning, rng) != roll::CraftVerdict::kOk ||
                    item.Explicits() != 3 || roll::RarityOf(item) != roll::Rarity::kRare) {
                    fail("Regal on magic with 2 mods must give exactly 3 mods, rare");
                } else {
                    std::printf("  2 mod(s): accepted -> %d mods, %s\n", item.Explicits(),
                        roll::RarityName(roll::RarityOf(item)));
                }
                (void)had;
            }
        }

        // ---- every refusal leaves the item bit-identical, every orb
        std::printf("Refusals leave the item bit-identical (every orb x every mod count, 200 rounds):\n");
        long long refusals = 0;
        long long changed = 0;
        for (int round = 0; round < 200; ++round) {
            for (int mods = 0; mods <= 6; ++mods) {
                roll::RolledItem base;
                if (!BuildWithMods(mods, a_table, ctx, a_tuning, rng, base)) {
                    continue;
                }
                for (const bool corrupt : { false, true }) {
                    for (int o = 0; o < roll::kOrbCount; ++o) {
                        auto item = base;
                        item.corrupted = corrupt;
                        const auto before = item;
                        const auto verdict = roll::Craft(static_cast<roll::Orb>(o), item, ctx, a_table, a_tuning, rng);
                        if (corrupt && verdict != roll::CraftVerdict::kCorrupted) {
                            fail("an orb worked on a corrupted item");
                        }
                        if (verdict != roll::CraftVerdict::kOk) {
                            ++refusals;
                            if (!SameItem(before, item)) {
                                ++changed;
                            }
                        }
                    }
                }
            }
        }
        std::printf("  %lld refusal(s), %lld changed the item\n", refusals, changed);
        if (changed) {
            fail("a refusal changed the item");
        }

        // ---- Chaos keeps the count for 3 to 6
        std::printf("Chaos keeps the mod count (3..6, 500 each):\n");
        for (int mods = 3; mods <= 6; ++mods) {
            int kept = 0;
            int tried = 0;
            for (int i = 0; i < 500; ++i) {
                roll::RolledItem item;
                if (!BuildWithMods(mods, a_table, ctx, a_tuning, rng, item)) {
                    continue;
                }
                const int implicits = item.Implicits();
                ++tried;
                if (roll::Craft(roll::Orb::kChaos, item, ctx, a_table, a_tuning, rng) == roll::CraftVerdict::kOk &&
                    item.Explicits() == mods && item.Implicits() == implicits) {
                    ++kept;
                }
            }
            std::printf("  %d mods: %d of %d kept the count and the implicits\n", mods, kept, tried);
            if (kept != tried) {
                fail("Chaos changed the mod count");
            }
        }
        // Chaos and Exalt refuse magic
        {
            roll::RolledItem item;
            if (BuildWithMods(2, a_table, ctx, a_tuning, rng, item)) {
                auto copy = item;
                if (roll::Craft(roll::Orb::kChaos, copy, ctx, a_table, a_tuning, rng) != roll::CraftVerdict::kNeedsRare ||
                    roll::Craft(roll::Orb::kExalt, copy, ctx, a_table, a_tuning, rng) != roll::CraftVerdict::kNeedsRare) {
                    fail("Chaos and Exalted must refuse a magic item");
                }
            }
        }

        // ---- co-save round trip: 0 implicits (legacy v1), 1, 2, corrupted
        std::printf("Co-save round trip:\n");
        const auto bytesOf = [](const roll::ItemRecord& a_record, bool a_v1) {
            std::vector<std::uint8_t> bytes;
            const auto put = [&](const void* a_src, std::size_t a_n) {
                const auto* b = static_cast<const std::uint8_t*>(a_src);
                bytes.insert(bytes.end(), b, b + a_n);
                return true;
            };
            if (a_v1) {
                // The 1.2.0 writer, byte for byte: one list.
                put(&a_record.itemLevel, 2);
                put(&a_record.flags, 1);
                std::vector<roll::AffixRecord> all = a_record.affixes;
                all.insert(all.end(), a_record.implicits.begin(), a_record.implicits.end());
                roll::detail::WriteAffixList(put, all);
            } else {
                roll::WriteItemRecordV2(put, a_record);
            }
            return bytes;
        };
        const auto readBack = [](const std::vector<std::uint8_t>& a_bytes, std::uint32_t a_version,
                                  roll::ItemRecord& a_out) {
            std::size_t at = 0;
            const bool  ok = roll::ReadItemRecord([&](void* a_dst, std::size_t a_n) {
                if (at + a_n > a_bytes.size()) return false;
                std::memcpy(a_dst, a_bytes.data() + at, a_n);
                at += a_n;
                return true;
            }, a_version, a_out);
            return ok && at == a_bytes.size();
        };
        for (const int implicits : { 0, 1, 2 }) {
            for (const bool corrupt : { false, true }) {
                if (implicits == 2 && !corrupt) {
                    continue;   // a second implicit only ever comes with corruption
                }
                roll::RolledItem item;
                if (!BuildWithMods(3, a_table, ctx, a_tuning, rng, item)) {
                    fail("could not build the round-trip item");
                    continue;
                }
                // Shape the implicit count.
                std::erase_if(item.affixes, [](const roll::RolledAffix& a_r) { return a_r.affix->Implicit(); });
                if (implicits >= 1) {
                    auto pool = roll::ImplicitPool(a_table, ctx);
                    roll::FitImplicitPool(pool, item);
                    for (int k = 0; k < implicits && !pool.empty(); ++k) {
                        roll::RolledAffix added;
                        if (roll::DrawAffix(pool, ctx, roll::TierWeightsFor(a_tuning, 40), rng, added) == roll::Draw::kDrawn) {
                            item.affixes.push_back(added);
                            roll::FitImplicitPool(pool, item);
                        }
                    }
                }
                item.corrupted = corrupt;
                item.tier = 0;
                for (const auto& r : item.affixes) item.tier += r.points;
                const auto record = roll::MakeRecord(item, 40);
                const bool legacy = implicits <= 1;   // v1 could hold 0 or 1
                for (const bool v1 : { false, true }) {
                    if (v1 && !legacy) continue;
                    roll::ItemRecord read;
                    const bool       parsed = readBack(bytesOf(record, v1), v1 ? roll::kItemRecordV1 : roll::kItemRecordV2, read);
                    if (v1) {
                        roll::NormalizeRecord(read, a_table);
                    }
                    roll::RolledItem back;
                    auto             ordered = item;
                    std::stable_partition(ordered.affixes.begin(), ordered.affixes.end(),
                        [](const roll::RolledAffix& a_r) { return a_r.affix->Implicit(); });
                    const bool same = parsed && roll::ToRolled(read, a_table, back) && SameItem(back, ordered) &&
                                      static_cast<int>(read.implicits.size()) == item.Implicits() &&
                                      (((read.flags & roll::kRecordCorrupted) != 0) == corrupt);
                    std::printf("  %s, %d implicit(s)%s: %s\n", v1 ? "v1 (legacy)" : "v2         ", item.Implicits(),
                        corrupt ? ", corrupted" : "", same ? "identical" : "DIFFERENT");
                    if (!same) {
                        fail("co-save round trip");
                    }
                }
            }
        }

        // ---- difficulty multiplier: 1.0 draws nothing, mean scales
        std::printf("Difficulty multiplier:\n");
        for (int d = 0; d <= 5; ++d) {
            std::printf("  level %d -> x%.2f\n", d, roll::DifficultyMultiplier(d));
        }
        {
            roll::Rng a{ 7 };
            roll::Rng b{ 7 };
            (void)roll::ScaleCount(3, 1.0f, a);
            if (a() != b()) {
                fail("ScaleCount drew from the rng at 1.0");
            }
            for (const float mult : { 1.15f, 1.45f, 1.75f, 2.6f }) {
                roll::Rng r{ 11 };
                long long sum = 0;
                const int n = 400000;
                for (int i = 0; i < n; ++i) {
                    sum += roll::ScaleCount(3, mult, r);
                }
                const double mean = static_cast<double>(sum) / n;
                std::printf("  3 orbs x%.2f: mean %.4f (want %.4f)\n", mult, mean, 3.0 * mult);
                if (std::fabs(mean - 3.0 * mult) > 0.01) {
                    fail("ScaleCount mean");
                }
            }
        }

        // ---- tier match on touching ranges
        std::printf("Tier match on a touching boundary:\n");
        for (const auto& affix : a_table.Affixes()) {
            for (std::size_t t = 0; t + 1 < affix.tiers.size(); ++t) {
                const auto& lo = affix.tiers[t];
                const auto& hi = affix.tiers[t + 1];
                if (std::max(lo.minValue, lo.maxValue) != std::min(hi.minValue, hi.maxValue)) {
                    continue;
                }
                const float edge = std::min(hi.minValue, hi.maxValue);
                const float justAbove = std::nextafter(edge + 0.002f * std::max(1.0f, std::fabs(edge)), edge + 1.0f);
                const auto* got = roll::MatchTier(affix, justAbove, hi.duration, hi.area);
                if (!got || got->tier != hi.tier) {
                    std::printf("  %s %.4f -> tier %d, want %d\n", affix.id.c_str(), justAbove, got ? got->tier : -1, hi.tier);
                    fail("touching tier ranges");
                }
            }
        }

        std::printf("%s\n", failures ? "SELFTEST FAILED" : "selftest: all checks passed");
        return failures ? 1 : 0;
    }
}

int main(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--samples" && i + 1 < argc) {
            options.samples = std::atoi(argv[++i]);
        } else if (arg == "--chains" && i + 1 < argc) {
            options.samples = std::atoi(argv[++i]);
        } else if (arg == "--level" && i + 1 < argc) {
            options.levels = { std::atoi(argv[++i]) };
        } else if (arg == "--slots" && i + 1 < argc) {
            const char* text = argv[++i];
            options.slots = roll::ParseSlots(text);
            if (options.slots == roll::kNone) {
                std::fprintf(stderr, "unknown slots '%s' (e.g. WEAPON|ONEHANDED, ARMOR|BODY, RING)\n",
                    text);
                return 2;
            }
        } else if (arg == "--caps" && i + 1 < argc) {
            const auto caps = ParseLevels(argv[++i]);
            if (caps.size() != 2) {
                std::fprintf(stderr, "--caps wants two numbers: prefixes,suffixes\n");
                return 2;
            }
            roll::SetAffixCaps(caps[0], caps[1]);
        } else if (arg == "--vaal-keep") {
            auto rules = roll::DefaultVaalRules();
            rules.canDestroy = false;
            roll::SetDefaultVaalRules(rules);
        } else if (arg == "--drops") {
            options.drops = true;
        } else if (arg == "--drop-mult" && i + 1 < argc) {
            options.dropMult = static_cast<float>(std::atof(argv[++i]));
            if (options.dropMult < 1.0f) {
                std::printf("--drop-mult %.2f is below 1.0; clamped to 1.0\n", options.dropMult);
                options.dropMult = 1.0f;
            }
        } else if (arg == "--global-mult" && i + 1 < argc) {
            options.globalMult = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--orb-weights" && i + 1 < argc) {
            options.orbWeights = ParseLevels(argv[++i]);
        } else if (arg == "--bare") {
            options.bare = true;
        } else if (arg == "--selftest") {
            options.selftest = true;
        } else if (arg == "--seed" && i + 1 < argc) {
            options.seed = static_cast<unsigned>(std::atoi(argv[++i]));
        } else if (arg == "--levels" && i + 1 < argc) {
            options.levels = ParseLevels(argv[++i]);
        } else if (arg == "--npc") {
            options.forNpc = true;
        } else if (arg == "--merge" && i + 1 < argc) {
            options.merges.emplace_back(argv[++i]);
        } else if (arg == "--craft" && i + 1 < argc) {
            for (const auto& token : SplitCommas(argv[++i])) {
                // "exalt*3" is three Exalted Orbs.
                std::string name = token;
                int         times = 1;
                if (const auto star = token.find('*'); star != std::string::npos) {
                    name = token.substr(0, star);
                    times = std::atoi(token.c_str() + star + 1);
                }
                roll::Orb orb{};
                if (!roll::ParseOrb(name, orb) || times < 1 || times > 100000) {
                    std::fprintf(stderr, "unknown orb '%s' (transmute, augment, regal, alchemy, exalt, "
                                         "chaos, scour, divine, vaal; orb*N repeats)\n",
                        token.c_str());
                    return 2;
                }
                options.chain.insert(options.chain.end(), static_cast<std::size_t>(times), orb);
            }
        } else if (arg == "--chaos-until" && i + 1 < argc) {
            options.chaosTarget = std::atoi(argv[++i]);
            if (options.chaosTarget <= 0) {
                std::fprintf(stderr, "--chaos-until wants a point total, e.g. 18\n");
                return 2;
            }
        } else if (!arg.empty() && arg[0] != '-') {
            options.csv = arg;
        }
    }

    if (options.drops) {
        RunDrops(options);   // needs no affix table
        return 0;
    }

    // Built AFTER the arguments are read: --vaal-keep changes what a default
    // Tuning starts from.
    roll::AffixTable         table;
    std::vector<std::string> errors;
    if (!table.Load(options.csv, errors)) {
        std::fprintf(stderr, "FAILED to load %s\n", options.csv.c_str());
        for (const auto& error : errors) {
            std::fprintf(stderr, "  %s\n", error.c_str());
        }
        return 1;
    }

    for (const auto& merge : options.merges) {
        std::vector<std::string> mergeErrors;
        table.MergeFile(merge, mergeErrors);
        std::printf("merged %s\n", merge.c_str());
        for (const auto& error : mergeErrors) {
            std::printf("    %s\n", error.c_str());
        }
    }

    std::printf("affix table: %s\n", options.csv.c_str());
    std::printf("  %zu affixes, %zu tier rows\n", table.Affixes().size(), table.TierRowCount());
    if (!errors.empty()) {
        std::printf("  %zu validation complaint(s):\n", errors.size());
        for (const auto& error : errors) {
            std::printf("    %s\n", error.c_str());
        }
    }
    std::printf("  rolling %d items per level, seed %u%s\n", options.samples, options.seed,
        options.forNpc ? ", as NPC loot" : "");
    std::printf("  caps: %d prefixes + %d suffixes; fifth affix from level %d, sixth from %d\n\n",
        roll::MaxPrefixes(), roll::MaxSuffixes(), roll::DefaultFifthAffixMinLevel(),
        roll::DefaultSixthAffixMinLevel());

    const roll::Tuning tuning;

    if (options.selftest) {
        return RunSelfTest(options, table, tuning);
    }
    if (!options.chain.empty()) {
        RunChain(options, table, tuning);
        return 0;
    }
    if (options.chaosTarget >= 0) {
        RunChaosUntil(options, table, tuning);
        return 0;
    }

    // Cumulative slot picker.
    int slotTotal = 0;
    for (const auto& slot : kSlots) {
        slotTotal += slot.share;
    }

    for (const int level : options.levels) {
        roll::Rng rng{ options.seed + static_cast<unsigned>(level) };

        std::array<int, roll::kMaxTier + 1> tierCounts{};
        std::map<std::string, int> affixCounts;
        long long                  totalAffixes = 0;
        long long                  totalImplicits = 0;
        RarityCounts               rarity{};
        std::array<int, roll::kMaxAffixes + 1> modCounts{};

        for (int i = 0; i < options.samples; ++i) {
            int pick = static_cast<int>(rng() % static_cast<unsigned>(slotTotal));
            std::uint32_t mask = kSlots[0].mask;
            for (const auto& slot : kSlots) {
                if (pick < slot.share) {
                    mask = slot.mask;
                    break;
                }
                pick -= slot.share;
            }

            if (options.slots != roll::kNone) {
                mask = options.slots;
            }
            const roll::RollContext ctx{ level, mask, options.forNpc };
            const auto              rolled = roll::Roll(table, ctx, tuning, rng);

            tierCounts[static_cast<std::size_t>(std::clamp(rolled.tier, 0, roll::kMaxTier))]++;
            totalAffixes += rolled.Explicits();
            totalImplicits += rolled.Implicits();
            CountRarity(rarity, rolled);
            modCounts[static_cast<std::size_t>(std::clamp(rolled.Explicits(), 0, roll::kMaxAffixes))]++;
            for (const auto& affix : rolled.affixes) {
                affixCounts[affix.affix->name]++;
            }
        }

        std::printf("================ item level %d ================\n", level);

        int peak = 0;
        for (const int count : tierCounts) {
            peak = std::max(peak, count);
        }

        for (int tier = 0; tier <= roll::kMaxTier; ++tier) {
            const int count = tierCounts[static_cast<std::size_t>(tier)];
            std::printf("  points %2d %6d %5.1f%%  ", tier, count, 100.0 * count / options.samples);
            Bar(count, peak);
            std::printf("\n");
        }

        std::printf("  ---- mods per item ----\n");
        for (int m = 0; m <= roll::kMaxAffixes; ++m) {
            std::printf("    %d  %6d  %5.1f%%\n", m, modCounts[static_cast<std::size_t>(m)],
                100.0 * modCounts[static_cast<std::size_t>(m)] / options.samples);
        }
        std::printf("  ---- rarity ----\n");
        PrintRarity(rarity, options.samples);
        std::printf("  mean mods/item: %.2f   mean implicits/item: %.2f\n",
            static_cast<double>(totalAffixes) / options.samples,
            static_cast<double>(totalImplicits) / options.samples);

        std::vector<std::pair<std::string, int>> sorted{ affixCounts.begin(), affixCounts.end() };
        std::sort(sorted.begin(), sorted.end(),
            [](const auto& a_lhs, const auto& a_rhs) { return a_lhs.second > a_rhs.second; });
        std::printf("  most common: ");
        for (std::size_t i = 0; i < sorted.size() && i < 5; ++i) {
            std::printf("%s(%d) ", sorted[i].first.c_str(), sorted[i].second);
        }

        // A handful of actual names. The distribution says whether the NUMBERS
        // are right; only reading the names says whether the ITEMS are.
        std::printf("\n  sample names:\n");
        roll::Rng          nameRng{ options.seed + 9973u + static_cast<unsigned>(level) };
        static const char* kBases[]{ "Iron Sword", "Steel Cuirass", "Elven Bow",
            "Gold Ring", "Leather Boots", "Orcish Shield" };
        for (int shown = 0, tries = 0; shown < 6 && tries < 400; ++tries) {
            const auto&             slot = kSlots[static_cast<std::size_t>(tries) % std::size(kSlots)];
            const roll::RollContext ctx{ level, slot.mask, options.forNpc };
            const auto              rolled = roll::Roll(table, ctx, tuning, nameRng);
            if (rolled.Explicits() == 0) {
                continue;
            }
            std::printf("    P%-2d %-7s %s\n", rolled.tier,
                roll::RarityName(roll::RarityOf(rolled)),
                rolled.Name(kBases[static_cast<std::size_t>(shown) % std::size(kBases)]).c_str());
            ++shown;
        }
        std::printf("\n  affixes never rolled: ");
        int never = 0;
        for (const auto& affix : table.Affixes()) {
            if (!affixCounts.count(affix.name)) {
                if (never < 6) {
                    std::printf("%s ", affix.name.c_str());
                }
                ++never;
            }
        }
        std::printf("%s(%d total)\n\n", never > 6 ? "... " : "", never);
    }

    return 0;
}
