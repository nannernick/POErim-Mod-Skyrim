// =============================================================================
//  Affix table loading. Pure -- see Roll.h.
// =============================================================================
//  The CSV is hand-parsed rather than handed to rapidcsv, for one reason: this
//  directory has no dependencies beyond the STL, and that is what lets the
//  balance harness build and run anywhere without vcpkg, Skyrim, or a plugin
//  host. The format is deliberately plain -- no quoted fields, no embedded
//  commas -- so the parse is a split and the validation is where the work goes.
// =============================================================================

#include "Roll.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace roll
{
    namespace
    {
        std::string Trim(std::string_view a_text)
        {
            const auto first = a_text.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos) {
                return {};
            }
            const auto last = a_text.find_last_not_of(" \t\r\n");
            return std::string{ a_text.substr(first, last - first + 1) };
        }

        std::vector<std::string> Split(std::string_view a_line, char a_delim)
        {
            std::vector<std::string> out;
            std::size_t              start = 0;
            while (true) {
                const auto pos = a_line.find(a_delim, start);
                if (pos == std::string_view::npos) {
                    out.push_back(Trim(a_line.substr(start)));
                    break;
                }
                out.push_back(Trim(a_line.substr(start, pos - start)));
                start = pos + 1;
            }
            return out;
        }

        bool ToInt(const std::string& a_text, int& a_out)
        {
            if (a_text.empty()) {
                return false;
            }
            try {
                std::size_t used = 0;
                a_out = std::stoi(a_text, &used);
                return used == a_text.size();
            } catch (...) {
                return false;
            }
        }

        bool ToFloat(const std::string& a_text, float& a_out)
        {
            if (a_text.empty()) {
                return false;
            }
            try {
                std::size_t used = 0;
                a_out = std::stof(a_text, &used);
                return used == a_text.size();
            } catch (...) {
                return false;
            }
        }
    }

    const char* KindName(AffixKind a_kind) noexcept
    {
        switch (a_kind) {
        case AffixKind::kPrefix:   return "prefix";
        case AffixKind::kSuffix:   return "suffix";
        case AffixKind::kImplicit: return "implicit";
        }
        return "?";
    }

    AffixKind KindForCategory(std::string_view a_category) noexcept
    {
        // Path of Exile's habit: what makes an item HIT HARDER OR LIVE LONGER is
        // a prefix -- damage, leech, life, armour -- and what adjusts it is a
        // suffix: resistances, skills, costs, regeneration, utility, and the
        // on-hit control effects, which also gives weapons enough suffixes to
        // fill two slots.
        static constexpr std::string_view kPrefixes[]{ "Weapon Damage", "Absorb", "Attribute",
            "Physical", "Unarmed" };
        for (const auto prefix : kPrefixes) {
            if (a_category == prefix) {
                return AffixKind::kPrefix;
            }
        }
        return AffixKind::kSuffix;
    }

    namespace
    {
        int& CapStorage(AffixKind a_kind)
        {
            static int prefixes = 3;
            static int suffixes = 3;
            return a_kind == AffixKind::kPrefix ? prefixes : suffixes;
        }

        int& GateStorage(bool a_sixth)
        {
            static int fifth = 35;
            static int sixth = 40;
            return a_sixth ? sixth : fifth;
        }

        VaalRules& VaalStorage()
        {
            static VaalRules rules;
            return rules;
        }

        std::string Lower(std::string a_text)
        {
            std::transform(a_text.begin(), a_text.end(), a_text.begin(),
                [](unsigned char a_ch) { return static_cast<char>(std::tolower(a_ch)); });
            return a_text;
        }
    }

    void SetAffixLevelGates(int a_fifthMinLevel, int a_sixthMinLevel) noexcept
    {
        GateStorage(false) = std::max(1, a_fifthMinLevel);
        // The sixth affix can never come before the fifth.
        GateStorage(true) = std::max(GateStorage(false), a_sixthMinLevel);
    }

    int DefaultFifthAffixMinLevel() noexcept
    {
        return GateStorage(false);
    }

    int DefaultSixthAffixMinLevel() noexcept
    {
        return GateStorage(true);
    }

    void SetDefaultVaalRules(const VaalRules& a_rules) noexcept
    {
        VaalStorage() = a_rules;
    }

    VaalRules DefaultVaalRules() noexcept
    {
        return VaalStorage();
    }

    void SetAffixCaps(int a_maxPrefixes, int a_maxSuffixes) noexcept
    {
        CapStorage(AffixKind::kPrefix) = std::clamp(a_maxPrefixes, 0, kMaxAffixes);
        CapStorage(AffixKind::kSuffix) = std::clamp(a_maxSuffixes, 0, kMaxAffixes);
    }

    int MaxPrefixes() noexcept
    {
        return CapStorage(AffixKind::kPrefix);
    }

    int MaxSuffixes() noexcept
    {
        return CapStorage(AffixKind::kSuffix);
    }

    std::uint32_t ParseSlots(std::string_view a_text)
    {
        static const std::unordered_map<std::string, std::uint32_t> kNames{
            { "WEAPON", kWeapon }, { "ARMOR", kArmor }, { "SHIELD", kShield },
            { "HEAD", kHead }, { "BODY", kBody }, { "HANDS", kHands },
            { "FEET", kFeet }, { "RING", kRing }, { "AMULET", kAmulet },
            { "ONEHANDED", kOneHanded }, { "TWOHANDED", kTwoHanded }, { "BOW", kBow },
            { "STAFF", kStaff },
        };

        std::uint32_t mask = kNone;
        for (auto& token : Split(a_text, '|')) {
            std::transform(token.begin(), token.end(), token.begin(),
                [](unsigned char a_ch) { return static_cast<char>(std::toupper(a_ch)); });
            if (const auto it = kNames.find(token); it != kNames.end()) {
                mask |= it->second;
            }
        }
        return mask;
    }

    std::string SlotsToString(std::uint32_t a_slots)
    {
        static const std::pair<std::uint32_t, const char*> kNames[]{
            { kWeapon, "WEAPON" }, { kArmor, "ARMOR" }, { kShield, "SHIELD" },
            { kHead, "HEAD" }, { kBody, "BODY" }, { kHands, "HANDS" },
            { kFeet, "FEET" }, { kRing, "RING" }, { kAmulet, "AMULET" },
            { kOneHanded, "ONEHANDED" }, { kTwoHanded, "TWOHANDED" }, { kBow, "BOW" },
            { kStaff, "STAFF" },
        };

        std::string out;
        for (const auto& [bit, name] : kNames) {
            if (a_slots & bit) {
                if (!out.empty()) {
                    out += '|';
                }
                out += name;
            }
        }
        return out.empty() ? "NONE" : out;
    }

    std::size_t AffixTable::TierRowCount() const noexcept
    {
        std::size_t rows = 0;
        for (const auto& affix : _affixes) {
            rows += affix.tiers.size();
        }
        return rows;
    }

    bool AffixTable::Load(const std::filesystem::path& a_path, std::vector<std::string>& a_errors)
    {
        _affixes.clear();
        return MergeFile(a_path, a_errors);
    }

    bool AffixTable::MergeFile(const std::filesystem::path& a_path, std::vector<std::string>& a_errors)
    {
        // Opened by path, not by narrow string, so a non-ASCII install location
        // works on any locale instead of failing to open (or worse, see PathToUtf8).
        std::ifstream file{ a_path };
        if (!file) {
            a_errors.push_back("cannot open " + PathToUtf8(a_path));
            return false;
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        return Merge(buffer.str(), a_errors);
    }

    bool AffixTable::LoadFromString(std::string_view a_csv, std::vector<std::string>& a_errors)
    {
        return Merge(a_csv, a_errors);
    }

    // ★MERGE, not replace, and this is what makes the mod patchable.
    //
    // Add-on files let someone ship "Summermyst affixes for DiabloLoot" as a
    // drop-in rather than a fork of the base table. Rows for an affixId that
    // already exists REPLACE its tiers wholesale rather than appending, so a
    // patch can rebalance a vanilla affix instead of only adding new ones.
    bool AffixTable::Merge(std::string_view a_csv, std::vector<std::string>& a_errors)
    {

        // affixId,name,category,tier,points,minValue,maxValue,unit,duration,
        // area,slots,weight,minItemLevel,mgef,npcExclude,prefix,suffix[,type[,group]]
        //
        // The eighteenth column, type (prefix / suffix / implicit), is optional:
        // missing or blank, the category decides (KindForCategory).
        //
        // The nineteenth, group, is optional too: the mod group, of which an
        // item carries at most one affix. Missing or blank it is the affixId,
        // and affixes sharing a magic effect are grouped after the read.
        constexpr std::size_t kColumns = 17;

        // Seeded from what is already loaded, so an add-on file can see the base
        // table's affixes rather than silently duplicating them.
        std::unordered_map<std::string, std::size_t> index;  // affixId -> position
        for (std::size_t i = 0; i < _affixes.size(); ++i) {
            index[_affixes[i].id] = i;
        }

        // ★PRE-EXISTING, not "in the index". The index grows as this file is
        // read, so testing against it treats an affix's own second tier row as a
        // collision with its first and wipes it. Measured: the base table came
        // back with 90 rows instead of 133, every affix reduced to its last tier.
        //
        // Replacement is only meaningful against rows that were already there
        // when this file STARTED -- that is what "an add-on overrides the base"
        // means, and rows within one file are always additive.
        std::unordered_set<std::string> preExisting;
        for (const auto& affix : _affixes) {
            preExisting.insert(affix.id);
        }
        std::unordered_set<std::string> replaced;
        std::istringstream                           stream{ std::string{ a_csv } };
        std::string                                  line;
        std::size_t                                  lineNo = 0;
        bool                                         sawHeader = false;

        while (std::getline(stream, line)) {
            ++lineNo;
            const auto trimmed = Trim(line);
            if (trimmed.empty() || trimmed[0] == '#') {
                continue;
            }
            // ★A header is skipped WHEREVER it appears, not only on line one.
            // Files get concatenated -- by someone merging two add-ons, or by a
            // test harness -- and a second header buried mid-file would
            // otherwise be reported as a malformed row, which reads like a data
            // error in a file that is perfectly correct.
            if (trimmed.rfind("affixId", 0) == 0) {
                sawHeader = true;
                continue;
            }
            if (!sawHeader) {
                sawHeader = true;
                a_errors.push_back("line 1: expected a header starting with 'affixId'");
                // Not fatal -- fall through and try to read it as data.
            }

            const auto fields = Split(trimmed, ',');
            if (fields.size() < kColumns) {
                a_errors.push_back("line " + std::to_string(lineNo) + ": expected " +
                    std::to_string(kColumns) + " columns, found " + std::to_string(fields.size()));
                continue;
            }

            AffixTier tier;
            int       npcExclude = 0;
            bool      ok = true;

            ok &= ToInt(fields[3], tier.tier);
            ok &= ToInt(fields[4], tier.points);
            ok &= ToFloat(fields[5], tier.minValue);
            ok &= ToFloat(fields[6], tier.maxValue);
            if (!ok) {
                a_errors.push_back("line " + std::to_string(lineNo) + ": '" + fields[0] +
                    "' has a non-numeric tier/points/minValue/maxValue");
                continue;
            }

            int scratch = 0;
            tier.duration = ToInt(fields[8], scratch) ? static_cast<std::uint32_t>(scratch) : 0u;
            tier.area = ToInt(fields[9], scratch) ? static_cast<std::uint32_t>(scratch) : 0u;
            tier.weight = ToInt(fields[11], scratch) && scratch >= 0 ? static_cast<std::uint32_t>(scratch) : 100u;
            tier.minItemLevel = ToInt(fields[12], scratch) ? scratch : 0;
            npcExclude = ToInt(fields[14], scratch) ? scratch : 0;

            if (fields[0].empty()) {
                a_errors.push_back("line " + std::to_string(lineNo) + ": empty affixId");
                continue;
            }
            if (tier.points <= 0) {
                a_errors.push_back("line " + std::to_string(lineNo) + ": '" + fields[0] +
                    "' has points=" + std::to_string(tier.points) +
                    "; a zero-point affix would be invisible in the item tier");
                continue;
            }
            // ★minValue > maxValue is legal and expected: cost-reduction affixes
            // run negative, so "-5 to -8" is a widening range, not a typo. Only
            // flag it when the two have different signs, which really is one.
            if ((tier.minValue < 0.0f) != (tier.maxValue < 0.0f)) {
                a_errors.push_back("line " + std::to_string(lineNo) + ": '" + fields[0] +
                    "' has a value range that changes sign (" + fields[5] + ".." + fields[6] + ")");
            }

            const auto slots = ParseSlots(fields[10]);
            if (slots == kNone) {
                a_errors.push_back("line " + std::to_string(lineNo) + ": '" + fields[0] +
                    "' has unrecognized slots '" + fields[10] + "'; it can never roll");
                continue;
            }

            auto it = index.find(fields[0]);
            if (preExisting.contains(fields[0]) && !replaced.contains(fields[0])) {
                replaced.insert(fields[0]);
                _affixes[it->second].tiers.clear();
            }
            if (it == index.end()) {
                Affix affix;
                affix.id = fields[0];
                affix.name = fields[1];
                affix.category = fields[2];
                affix.unit = fields[7];
                affix.slots = slots;
                affix.mgef = fields[13];
                affix.npcExclude = npcExclude != 0;
                affix.prefix = fields.size() > 15 ? fields[15] : "";
                affix.suffix = fields.size() > 16 ? fields[16] : "";
                affix.kind = KindForCategory(affix.category);
                if (fields.size() > 17 && !fields[17].empty()) {
                    std::string type = fields[17];
                    std::transform(type.begin(), type.end(), type.begin(),
                        [](unsigned char a_ch) { return static_cast<char>(std::tolower(a_ch)); });
                    if (type == "prefix") {
                        affix.kind = AffixKind::kPrefix;
                    } else if (type == "suffix") {
                        affix.kind = AffixKind::kSuffix;
                    } else if (type == "implicit") {
                        affix.kind = AffixKind::kImplicit;
                    } else {
                        a_errors.push_back("line " + std::to_string(lineNo) + ": '" + fields[0] +
                            "' has type '" + fields[17] + "'; expected prefix, suffix or implicit -- "
                            "using " + KindName(affix.kind));
                    }
                }
                affix.group = affix.id;
                if (fields.size() > 18 && !fields[18].empty()) {
                    affix.group = fields[18];
                    affix.groupExplicit = true;
                }
                index[affix.id] = _affixes.size();
                _affixes.push_back(std::move(affix));
                it = index.find(fields[0]);
            }

            auto& affix = _affixes[it->second];
            const bool duplicate = std::any_of(affix.tiers.begin(), affix.tiers.end(),
                [&](const AffixTier& a_existing) { return a_existing.tier == tier.tier; });
            if (duplicate) {
                a_errors.push_back("line " + std::to_string(lineNo) + ": '" + fields[0] +
                    "' declares tier " + std::to_string(tier.tier) + " more than once");
                continue;
            }
            affix.tiers.push_back(tier);
        }

        // Sorting by tier lets the roller assume ascending order, and lets a
        // hand-edited CSV be in any order the author finds readable.
        for (auto& affix : _affixes) {
            std::sort(affix.tiers.begin(), affix.tiers.end(),
                [](const AffixTier& a_lhs, const AffixTier& a_rhs) { return a_lhs.tier < a_rhs.tier; });
        }

        std::erase_if(_affixes, [&](const Affix& a_affix) {
            if (a_affix.tiers.empty()) {
                a_errors.push_back("affix '" + a_affix.id + "' has no usable tier rows; dropped");
                return true;
            }
            return false;
        });

        // ★AUTO-GROUPING, redone on every merge because an add-on can bring an
        // affix that shares an effect with a base one. Every affix without a
        // group of its own written in the CSV joins the group of the FIRST
        // affix in the table that uses its magic effect -- that affix's written
        // group, or its id. Two affixes of one effect on one item would be one
        // merged line in the enchantment, which is what this rule exists for.
        std::unordered_map<std::string, std::string> groupOfEffect;
        for (auto& affix : _affixes) {
            if (!affix.groupExplicit) {
                affix.group = affix.id;
            }
            if (affix.mgef.empty()) {
                continue;
            }
            const auto [it, first] = groupOfEffect.try_emplace(Lower(affix.mgef), affix.group);
            if (!first && !affix.groupExplicit) {
                affix.group = it->second;
            }
        }

        return !_affixes.empty();
    }
}
