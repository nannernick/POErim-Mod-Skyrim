// =============================================================================
//  Roll harness -- a console program, not part of the plugin.
// =============================================================================
//  Rolls a large sample at several item levels and prints what actually comes
//  out: the tier histogram, the colour-band split, how often each affix appears,
//  and how many items roll nothing at all.
//
//  This exists because loot balance is not a thing you can reason your way to.
//  You change a number, look at the distribution, and change it again -- and
//  doing that inside Skyrim costs a launch and a dungeon per attempt.
//
//    RollHarness [csv] [--samples N] [--seed N] [--levels 1,12,25,34,35,40] [--npc]
//
//  CRAFTING. The orbs are tuned the same way the drops are -- by looking:
//
//    RollHarness --craft transmute,augment,exalt,exalt,exalt
//        Starts every sample as a plain item and uses the orbs in that order.
//        After each step: how many items the orb worked on, why it failed on
//        the rest, and the colour-band split of the whole sample.
//
//    RollHarness --chaos-until purple
//        Rolls an item the way a drop would, then spends Chaos Orbs until it
//        reaches that band. Prints how many orbs that takes.
//
//    --merge file.csv   merges an add-on table first (repeatable)
//
//  Both crafting modes also push every finished item through the save record
//  and back, and through the legacy tier match, and say how many survived --
//  so a table whose tier ranges overlap is caught here and not in a save.
// =============================================================================

#include "roll/Craft.h"
#include "roll/Record.h"
#include "roll/Roll.h"

#include <algorithm>
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
        std::vector<int> levels{ 1, 12, 25, 34, 35, 40 };
        bool             forNpc{ false };

        std::vector<std::string> merges;
        std::vector<roll::Orb>   chain;          // --craft
        int                      chaosTarget{ -1 };  // --chaos-until, as a roll::Band
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

    void PrintBands(const std::array<int, roll::kBandCount>& a_bands, int a_total)
    {
        for (int b = 0; b < roll::kBandCount; ++b) {
            const int count = a_bands[static_cast<std::size_t>(b)];
            std::printf("      %-7s %6d  %5.1f%%  ", roll::BandName(static_cast<roll::Band>(b)),
                count, a_total > 0 ? 100.0 * count / a_total : 0.0);
            Bar(count, a_total, 30);
            std::printf("\n");
        }
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
            bool             same = roll::ToRolled(record, a_table, back) &&
                                    back.tier == a_item.tier &&
                                    back.affixes.size() == a_item.affixes.size();
            for (std::size_t i = 0; same && i < back.affixes.size(); ++i) {
                same = back.affixes[i].affix == a_item.affixes[i].affix &&
                       back.affixes[i].tier == a_item.affixes[i].tier &&
                       back.affixes[i].value == a_item.affixes[i].value;
            }
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
        std::printf("\n  every sample starts as a plain item\n\n");

        for (const int level : a_options.levels) {
            roll::Rng rng{ a_options.seed + static_cast<unsigned>(level) };

            std::vector<roll::RolledItem> items(static_cast<std::size_t>(a_options.samples));
            std::vector<std::uint32_t>    slots(items.size());
            for (auto& mask : slots) {
                mask = PickSlots(rng);
            }

            std::printf("================ item level %d ================\n", level);
            int step = 0;
            for (const auto orb : a_options.chain) {
                std::map<roll::CraftVerdict, int> verdicts;
                for (std::size_t i = 0; i < items.size(); ++i) {
                    const roll::RollContext ctx{ level, slots[i], false };
                    verdicts[roll::Craft(orb, items[i], ctx, a_table, a_tuning, rng)]++;
                }

                std::array<int, roll::kBandCount> bands{};
                long long                         affixes = 0;
                for (const auto& item : items) {
                    bands[static_cast<std::size_t>(roll::BandOf(item.tier))]++;
                    affixes += static_cast<long long>(item.affixes.size());
                }

                std::printf("  step %d: %s -- worked on %d of %d (%.1f%%)\n", ++step,
                    roll::OrbName(orb), verdicts[roll::CraftVerdict::kOk], a_options.samples,
                    100.0 * verdicts[roll::CraftVerdict::kOk] / a_options.samples);
                for (const auto& [verdict, count] : verdicts) {
                    if (verdict != roll::CraftVerdict::kOk) {
                        std::printf("      refused %6d: %s\n", count, roll::Reason(verdict));
                    }
                }
                PrintBands(bands, a_options.samples);
                std::printf("      mean affixes/item: %.2f\n",
                    static_cast<double>(affixes) / a_options.samples);
            }

            RecordCheck check;
            for (std::size_t i = 0; i < items.size(); ++i) {
                check.Add(items[i], level, a_table);
            }
            check.Print();
            std::printf("\n");
        }
    }

    void RunChaosUntil(const Options& a_options, const roll::AffixTable& a_table,
        const roll::Tuning& a_tuning)
    {
        // A cap, because some targets are unreachable -- red below the fifth
        // affix's level, or a band above what the item's affix count can sum
        // to -- and the harness should say so rather than spin.
        constexpr int kCap = 2000;
        const auto    target = static_cast<roll::Band>(a_options.chaosTarget);

        std::printf("chaos until %s or better (cap %d orbs per item)\n", roll::BandName(target),
            kCap);
        std::printf("  each sample is rolled as a drop first; items that drop with fewer than\n"
                    "  two affixes cannot take a Chaos Orb and are counted apart\n\n");

        for (const int level : a_options.levels) {
            roll::Rng rng{ a_options.seed + static_cast<unsigned>(level) };

            int                    already = 0;
            int                    ineligible = 0;
            int                    capped = 0;
            std::vector<int>       spent;
            RecordCheck            check;
            spent.reserve(static_cast<std::size_t>(a_options.samples));

            for (int i = 0; i < a_options.samples; ++i) {
                const roll::RollContext ctx{ level, PickSlots(rng), false };
                auto                    item = roll::Roll(a_table, ctx, a_tuning, rng);
                if (roll::BandOf(item.tier) >= target) {
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
                while (used < kCap && roll::BandOf(item.tier) < target) {
                    if (roll::Craft(roll::Orb::kChaos, item, ctx, a_table, a_tuning, rng) !=
                        roll::CraftVerdict::kOk) {
                        break;
                    }
                    ++used;
                }
                if (roll::BandOf(item.tier) >= target) {
                    spent.push_back(used);
                    check.Add(item, level, a_table);
                } else {
                    ++capped;
                }
            }

            std::printf("================ item level %d ================\n", level);
            std::printf("  dropped at %s or better already: %d\n", roll::BandName(target), already);
            std::printf("  dropped with under two affixes:      %d\n", ineligible);
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

int main(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--samples" && i + 1 < argc) {
            options.samples = std::atoi(argv[++i]);
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
                roll::Orb orb{};
                if (!roll::ParseOrb(token, orb)) {
                    std::fprintf(stderr, "unknown orb '%s' (transmute, augment, exalt, chaos, scour)\n",
                        token.c_str());
                    return 2;
                }
                options.chain.push_back(orb);
            }
        } else if (arg == "--chaos-until" && i + 1 < argc) {
            const std::string name = argv[++i];
            for (int b = 0; b < roll::kBandCount; ++b) {
                if (name == roll::BandName(static_cast<roll::Band>(b))) {
                    options.chaosTarget = b;
                }
            }
            if (options.chaosTarget < 0) {
                std::fprintf(stderr, "unknown band '%s' (blue, yellow, purple, orange, red)\n",
                    name.c_str());
                return 2;
            }
        } else if (!arg.empty() && arg[0] != '-') {
            options.csv = arg;
        }
    }

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
    std::printf("  rolling %d items per level, seed %u%s\n\n",
        options.samples, options.seed, options.forNpc ? ", as NPC loot" : "");

    const roll::Tuning tuning;

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

            const roll::RollContext ctx{ level, mask, options.forNpc };
            const auto              rolled = roll::Roll(table, ctx, tuning, rng);

            tierCounts[static_cast<std::size_t>(std::clamp(rolled.tier, 0, roll::kMaxTier))]++;
            totalAffixes += static_cast<long long>(rolled.affixes.size());
            for (const auto& affix : rolled.affixes) {
                affixCounts[affix.affix->name]++;
            }
        }

        std::printf("================ item level %d ================\n", level);

        int peak = 0;
        for (const int count : tierCounts) {
            peak = std::max(peak, count);
        }

        std::array<int, roll::kBandCount> bands{};
        for (int tier = 0; tier <= roll::kMaxTier; ++tier) {
            const int count = tierCounts[static_cast<std::size_t>(tier)];
            bands[static_cast<std::size_t>(roll::BandOf(tier))] += count;
            std::printf("  tier %2d %-7s %6d %5.1f%%  ", tier,
                roll::BandName(roll::BandOf(tier)), count,
                100.0 * count / options.samples);
            Bar(count, peak);
            std::printf("\n");
        }

        std::printf("  ---- colour bands ----\n");
        for (int b = 0; b < roll::kBandCount; ++b) {
            std::printf("    %-7s %6d  %5.1f%%\n", roll::BandName(static_cast<roll::Band>(b)),
                bands[static_cast<std::size_t>(b)], 100.0 * bands[static_cast<std::size_t>(b)] / options.samples);
        }
        std::printf("  mean affixes/item: %.2f\n", static_cast<double>(totalAffixes) / options.samples);

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
            if (rolled.affixes.empty()) {
                continue;
            }
            std::printf("    T%-2d %-7s %s\n", rolled.tier,
                roll::BandName(roll::BandOf(rolled.tier)),
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
