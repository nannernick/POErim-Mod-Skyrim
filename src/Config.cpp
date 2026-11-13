// =============================================================================
//  Settings. See Config.h.
// =============================================================================

#include "PCH.h"

#include "Config.h"

#include "GridTint.h"
#include "roll/Roll.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace
{
    constexpr const char* kPath = "Data/SKSE/Plugins/DiabloLoot.ini";

    bool g_distribution = true;
    bool g_questRewards = true;
    bool g_discovery = false;
    bool g_containerTrace = false;
    bool g_debugLoadLog = false;
    bool g_verboseSurvey = false;

    // 1.0 is vanilla. Clamped rather than validated: an INI is edited by hand,
    // and a fat-fingered 30 should give a very full chest, not a hang.
    float g_containerLoot = 1.0f;

    // Starts as whatever the roller ships with, so that not writing the setting
    // and deleting the whole file come out the same. (The roller ships U+25C6.)
    std::string g_tierMarker{ roll::TierMark() };

    // ★THIS ONE HAS NO FIXED DEFAULT, and cannot sensibly have one: the right
    // answer depends on whether anything ELSE is drawing the band.
    //
    // With Grid Inventory present the tooltip carries the marker and the grid
    // colours the item, so putting the glyphs in the name too is the same fact
    // three times -- while still joining every list's sort key and every save.
    // Without it the name is the only surface there is, and defaulting to off
    // would silently show the player nothing.
    //
    // So the default is decided at the bottom of Load(), from whether the host
    // is there. An explicit INI line always wins, which is what the `set` flag
    // is for -- "0" and "absent" must not mean the same thing.
    bool g_tierMarkerInName = false;
    bool g_tierMarkerInNameSet = false;

    // Off, and the three numbers only matter once it is on. See Config.h.
    bool g_weaponCharge = false;
    int  g_weaponChargeCost = 10;
    int  g_weaponChargeCostPerPoint = 2;

    // Indexed by roll::PowerStepOf. Step 0 is a placeholder that is never read.
    std::uint16_t g_weaponChargeAmount[roll::kPowerSteps] = { 0, 1000, 1500, 2000, 2500, 3000 };

    bool          g_wielderBuffs = true;

    // The tooltip lines the grid is offered, and the key that adds each affix's
    // tier and range to them. 0x12 is VK_MENU -- either Alt.
    bool g_tooltipItemLevel = true;
    bool g_tooltipAffixLines = true;
    int  g_tooltipDetailKey = 0x12;

    // How many prefixes and suffixes an item may carry. Read into the roller
    // (roll::SetAffixCaps) with the other settings.
    int g_maxPrefixes = 3;
    int g_maxSuffixes = 3;
    bool  g_vendorStock = true;
    bool  g_priceHook = true;
    bool  g_sellPricesScaled = false;
    float g_priceMult[roll::kPowerSteps] = { 1.0f, 1.5f, 2.0f, 3.0f, 5.0f, 10.0f };

    bool  g_replaceVanilla = true;
    bool  g_dropMultAuto = true;
    float g_dropMultOverride = 1.0f;
    float g_dropPerDifficulty = 0.15f;

    // Rarity weights per curve band, -1 = keep the curve's own (1.2.0) split.
    std::array<std::array<int, 3>, 4> g_rarityWeights{ { { -1, -1, -1 }, { -1, -1, -1 },
        { -1, -1, -1 }, { -1, -1, -1 } } };
    constexpr int kRarityBandLevels[4]{ 1, 12, 25, 40 };

    int              g_fifthMinLevel = 35;
    int              g_sixthMinLevel = 40;
    roll::VaalRules  g_vaal;
    bool             g_orbDrops = true;
    Config::GearMode g_gearMode = Config::GearMode::kAllInventory;
    bool             g_orbsInCorpse = false;
    bool             g_bundleGold = true;
    int              g_orbCleanupDays = 3;
    bool             g_orbDropNotify = false;
    roll::DropTuning g_drops;

    constexpr const char* kOrbWeightKeys[roll::kOrbCount]{ "orbweighttransmute",
        "orbweightaugment", "orbweightalchemy", "orbweightexalt", "orbweightchaos",
        "orbweightscour", "orbweightdivine", "orbweightvaal", "orbweightregal" };
    constexpr const char* kOrbWeightNames[roll::kOrbCount]{ "OrbWeightTransmute",
        "OrbWeightAugment", "OrbWeightAlchemy", "OrbWeightExalt", "OrbWeightChaos",
        "OrbWeightScour", "OrbWeightDivine", "OrbWeightVaal", "OrbWeightRegal" };

    constexpr std::uint32_t Rgba(std::uint8_t a_r, std::uint8_t a_g, std::uint8_t a_b)
    {
        return (0xFFu << 24) | (std::uint32_t{ a_b } << 16) | (std::uint32_t{ a_g } << 8) | a_r;
    }

    // Path of Exile's item-name colours, near enough: white, blue, yellow, the
    // currency's sandy gold, and corrupted red.
    std::array<std::uint32_t, static_cast<std::size_t>(Config::RarityColour::kCount)> g_rarityColor{
        Rgba(200, 200, 200), Rgba(136, 136, 255), Rgba(255, 255, 119), Rgba(215, 180, 95),
        Rgba(210, 40, 40)
    };
    std::once_flag g_rarityColorOnce;
    constexpr const char* kRarityColorKeys[]{ "raritycolornormal", "raritycolormagic",
        "raritycolorrare", "raritycolorcurrency", "raritycolorcorrupted" };

    std::string Trim(std::string a_text)
    {
        const auto first = a_text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return {};
        }
        const auto last = a_text.find_last_not_of(" \t\r\n");
        return a_text.substr(first, last - first + 1);
    }

    // "1", "true", "yes", "on" -- anything else is false.
    //
    // Permissive on purpose: someone editing an INI by hand should not have to
    // guess which spelling of true this particular mod wanted.
    bool AsBool(std::string a_value)
    {
        std::transform(a_value.begin(), a_value.end(), a_value.begin(),
            [](unsigned char a_ch) { return static_cast<char>(std::tolower(a_ch)); });
        return a_value == "1" || a_value == "true" || a_value == "yes" || a_value == "on";
    }

    // ★A BAD NUMBER KEEPS THE DEFAULT AND SAYS SO. std::stof throws on garbage,
    // and an exception escaping Load() over a typo would take the whole plugin
    // down at kDataLoaded -- for a setting whose entire job is optional.
    float AsFloat(const std::string& a_value, float a_fallback, float a_min, float a_max)
    {
        try {
            std::size_t used = 0;
            const float parsed = std::stof(a_value, &used);
            if (used != a_value.size()) {
                logger::warn("{}: '{}' has trailing junk; using {}", kPath, a_value, a_fallback);
                return a_fallback;
            }
            return std::clamp(parsed, a_min, a_max);
        } catch (const std::exception&) {
            logger::warn("{}: '{}' is not a number; using {}", kPath, a_value, a_fallback);
            return a_fallback;
        }
    }

    // "RRGGBB" or "#RRGGBB" -> ImU32. False and nothing written on a bad value.
    bool ParseHexColour(std::string a_value, std::uint32_t& a_out)
    {
        if (!a_value.empty() && a_value[0] == '#') {
            a_value.erase(0, 1);
        }
        if (a_value.size() != 6) {
            return false;
        }
        unsigned value = 0;
        for (const char ch : a_value) {
            const int digit = std::isdigit(static_cast<unsigned char>(ch)) ? ch - '0'
                              : std::isxdigit(static_cast<unsigned char>(ch))
                                  ? std::tolower(static_cast<unsigned char>(ch)) - 'a' + 10
                                  : -1;
            if (digit < 0) {
                return false;
            }
            value = (value << 4) | static_cast<unsigned>(digit);
        }
        a_out = Rgba(static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 8),
            static_cast<std::uint8_t>(value));
        return true;
    }

    // Lower-cased key -> value for every line of the file. Used by the early
    // rarity-colour read, which must not touch anything else Load() sets.
    std::vector<std::pair<std::string, std::string>> ReadPairs()
    {
        std::vector<std::pair<std::string, std::string>> out;
        std::ifstream                                    file{ kPath };
        std::string                                      line;
        while (file && std::getline(file, line)) {
            const auto trimmed = Trim(line);
            if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';' || trimmed[0] == '[') {
                continue;
            }
            const auto eq = trimmed.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            auto key = Trim(trimmed.substr(0, eq));
            std::transform(key.begin(), key.end(), key.begin(),
                [](unsigned char a_ch) { return static_cast<char>(std::tolower(a_ch)); });
            out.emplace_back(std::move(key), Trim(trimmed.substr(eq + 1)));
        }
        return out;
    }

    void LoadRarityColours()
    {
        for (const auto& [key, value] : ReadPairs()) {
            for (std::size_t i = 0; i < std::size(kRarityColorKeys); ++i) {
                if (key == kRarityColorKeys[i] && !ParseHexColour(value, g_rarityColor[i])) {
                    logger::warn("{}: {} = '{}' is not RRGGBB; keeping the default", kPath, key, value);
                }
            }
        }
    }

    // Reads one band's charge amount off an INI line. Sixteen bits is the
    // engine's own ceiling on an instance's charge.
    void ReadPriceMult(int a_step, const std::string& a_value)
    {
        auto& slot = g_priceMult[a_step];
        slot = AsFloat(a_value, slot, 0.01f, 1000.0f);
    }

    void ReadChargeAmount(int a_step, const std::string& a_value)
    {
        auto& slot = g_weaponChargeAmount[a_step];
        slot = static_cast<std::uint16_t>(
            AsFloat(a_value, static_cast<float>(slot), 1.0f, 65535.0f));
    }
}

void Config::Load()
{
    std::ifstream file{ kPath };
    if (!file) {
        logger::info("no {} -- using defaults (distribution on, discovery off)", kPath);
        return;
    }

    std::string line;
    std::size_t applied = 0;
    bool        first = true;

    while (std::getline(file, line)) {
        // ★THE BYTE-ORDER MARK, EATEN. Saving this file as UTF-8 is the
        // documented advice for a non-ASCII tier marker, and Notepad writes a
        // BOM when it does. Left in place it glues itself to the first line --
        // which is either an ignored comment or, if someone reorders the file,
        // a setting name that silently stops matching.
        if (first) {
            first = false;
            if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }
        }

        const auto trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';' || trimmed[0] == '[') {
            continue;
        }

        const auto eq = trimmed.find('=');
        if (eq == std::string::npos) {
            logger::warn("{}: ignoring line without '=': {}", kPath, trimmed);
            continue;
        }

        auto key = Trim(trimmed.substr(0, eq));
        const auto value = Trim(trimmed.substr(eq + 1));
        std::transform(key.begin(), key.end(), key.begin(),
            [](unsigned char a_ch) { return static_cast<char>(std::tolower(a_ch)); });

        if (key == "distribution") {
            g_distribution = AsBool(value);
        } else if (key == "questrewards") {
            g_questRewards = AsBool(value);
        } else if (key == "containerlootmultiplier") {
            g_containerLoot = AsFloat(value, 1.0f, 1.0f, 10.0f);
        } else if (key == "discovery") {
            g_discovery = AsBool(value);
        } else if (key == "verbosesurvey") {
            g_verboseSurvey = AsBool(value);
        } else if (key == "debugloadlog") {
            g_debugLoadLog = AsBool(value);
        } else if (key == "containertrace") {
            g_containerTrace = AsBool(value);
        } else if (key == "tiermarker") {
            // ★VERBATIM, AND AN EMPTY VALUE IS A REAL ANSWER. "TierMarker="
            // means the player wants no marker, which is different from leaving
            // the line out -- so this cannot fall back to the default on empty.
            // No case folding either: the value is text to display, not a
            // keyword to match.
            g_tierMarker = value;
        } else if (key == "tiermarkerinname") {
            g_tierMarkerInName = AsBool(value);
            g_tierMarkerInNameSet = true;
        } else if (key == "weaponcharge") {
            g_weaponCharge = AsBool(value);
        } else if (key == "weaponchargeblue") {
            ReadChargeAmount(1, value);
        } else if (key == "weaponchargeyellow") {
            ReadChargeAmount(2, value);
        } else if (key == "weaponchargepurple") {
            ReadChargeAmount(3, value);
        } else if (key == "weaponchargeorange") {
            ReadChargeAmount(4, value);
        } else if (key == "weaponchargered") {
            ReadChargeAmount(5, value);
        } else if (key == "weaponchargecost") {
            g_weaponChargeCost = static_cast<int>(AsFloat(value, 10.0f, 1.0f, 10000.0f));
        } else if (key == "weaponchargecostperpoint") {
            g_weaponChargeCostPerPoint = static_cast<int>(AsFloat(value, 2.0f, 0.0f, 1000.0f));
        } else if (key == "wielderbuffs") {
            g_wielderBuffs = AsBool(value);
        } else if (key == "maxprefixes") {
            g_maxPrefixes = static_cast<int>(AsFloat(value, 3.0f, 0.0f, 6.0f));
        } else if (key == "maxsuffixes") {
            g_maxSuffixes = static_cast<int>(AsFloat(value, 3.0f, 0.0f, 6.0f));
        } else if (key == "tooltipitemlevel") {
            g_tooltipItemLevel = AsBool(value);
        } else if (key == "tooltipaffixlines") {
            g_tooltipAffixLines = AsBool(value);
        } else if (key == "tooltipdetailkey") {
            g_tooltipDetailKey = static_cast<int>(AsFloat(value, 18.0f, 0.0f, 255.0f));
        } else if (key == "vendorstock") {
            g_vendorStock = AsBool(value);
        } else if (key == "sellpricesscaled") {
            g_sellPricesScaled = AsBool(value);
        } else if (key == "pricehook") {
            g_priceHook = AsBool(value);
        } else if (key == "pricemultblue") {
            ReadPriceMult(1, value);
        } else if (key == "pricemultyellow") {
            ReadPriceMult(2, value);
        } else if (key == "pricemultpurple") {
            ReadPriceMult(3, value);
        } else if (key == "pricemultorange") {
            ReadPriceMult(4, value);
        } else if (key == "pricemultred") {
            ReadPriceMult(5, value);
        } else if (key == "fifthaffixminlevel") {
            g_fifthMinLevel = static_cast<int>(AsFloat(value, 35.0f, 1.0f, 255.0f));
        } else if (key == "sixthaffixminlevel") {
            g_sixthMinLevel = static_cast<int>(AsFloat(value, 40.0f, 1.0f, 255.0f));
        } else if (key == "vaalcandestroy") {
            g_vaal.canDestroy = AsBool(value);
        } else if (key == "vaalweightdestroy") {
            g_vaal.weightDestroy = static_cast<int>(AsFloat(value, 1.0f, 0.0f, 1000.0f));
        } else if (key == "vaalweightvalues") {
            g_vaal.weightValues = static_cast<int>(AsFloat(value, 1.0f, 0.0f, 1000.0f));
        } else if (key == "vaalweightimplicit") {
            // 1.2.0's "add the implicit" outcome. Every item has one now, so
            // the outcome became VaalAddImplicitChance (a second implicit).
            logger::warn("{}: VaalWeightImplicit is gone in 2.1.0 (every item has an implicit "
                         "now); use VaalAddImplicitChance. Ignored.", kPath);
        } else if (key == "vaaladdimplicitchance") {
            g_vaal.addImplicitChance = AsFloat(value, 0.25f, 0.0f, 1.0f);
        } else if (key == "replacevanillaenchants") {
            g_replaceVanilla = AsBool(value);
        } else if (key.rfind("raritycolor", 0) == 0) {
            bool known = false;
            for (std::size_t i = 0; i < std::size(kRarityColorKeys); ++i) {
                known = known || key == kRarityColorKeys[i];
            }
            if (!known) {
                logger::warn("{}: unknown setting '{}'", kPath, key);
                continue;
            }
            // Read by LoadRarityColours, which the grid may already have run.
        } else if (key.rfind("rarityweightslevel", 0) == 0) {
            const int level = std::atoi(key.c_str() + std::strlen("rarityweightslevel"));
            int       band = -1;
            for (int b = 0; b < 4; ++b) {
                if (kRarityBandLevels[b] == level) {
                    band = b;
                }
            }
            std::array<int, 3> w{ -1, -1, -1 };
            int                parsed = 0;
            std::string        token;
            for (std::size_t i = 0; i <= value.size(); ++i) {
                if (i == value.size() || value[i] == ',') {
                    if (parsed < 3) {
                        w[static_cast<std::size_t>(parsed)] =
                            static_cast<int>(AsFloat(Trim(token), 0.0f, 0.0f, 100000.0f));
                    }
                    ++parsed;
                    token.clear();
                } else {
                    token += value[i];
                }
            }
            if (band < 0 || parsed != 3) {
                logger::warn("{}: {} wants three numbers (normal,magic,rare) and a level of 1, 12, "
                             "25 or 40; ignored", kPath, key);
                continue;
            }
            g_rarityWeights[static_cast<std::size_t>(band)] = w;
        } else if (key == "dropquantitymultiplier") {
            std::string mode = value;
            std::transform(mode.begin(), mode.end(), mode.begin(),
                [](unsigned char a_ch) { return static_cast<char>(std::tolower(a_ch)); });
            if (mode == "auto" || mode.empty()) {
                g_dropMultAuto = true;
            } else {
                const float parsed = AsFloat(value, 1.0f, -1000.0f, 20.0f);
                if (parsed < 1.0f) {
                    logger::warn("{}: DropQuantityMultiplier {} is below 1.0; clamped to 1.0",
                        kPath, parsed);
                }
                g_dropMultAuto = false;
                g_dropMultOverride = (std::max)(1.0f, parsed);
            }
        } else if (key == "dropquantityperdifficulty") {
            g_dropPerDifficulty = AsFloat(value, 0.15f, 0.0f, 2.0f);
        } else if (key == "vaalweightreroll") {
            g_vaal.weightReroll = static_cast<int>(AsFloat(value, 1.0f, 0.0f, 1000.0f));
        } else if (key == "orbdrops") {
            g_orbDrops = AsBool(value);
        } else if (key == "enemygearmode") {
            std::string mode = value;
            std::transform(mode.begin(), mode.end(), mode.begin(),
                [](unsigned char a_ch) { return static_cast<char>(std::toupper(a_ch)); });
            if (mode == "ALL_GROUND") {
                g_gearMode = Config::GearMode::kAllGround;
            } else if (mode == "ALL_INVENTORY") {
                g_gearMode = Config::GearMode::kAllInventory;
            } else if (mode == "ORBS_GROUND") {
                g_gearMode = Config::GearMode::kOrbsGround;
            } else if (mode == "ITEMS_INVENTORY") {
                g_gearMode = Config::GearMode::kItemsInventory;
            } else {
                logger::warn("{}: EnemyGearMode '{}' is not one of ALL_GROUND, ALL_INVENTORY, "
                             "ORBS_GROUND, ITEMS_INVENTORY; keeping {}", kPath, value, GearModeName(g_gearMode));
            }
        } else if (key == "orbplacement") {
            std::string mode = value;
            std::transform(mode.begin(), mode.end(), mode.begin(),
                [](unsigned char a_ch) { return static_cast<char>(std::toupper(a_ch)); });
            g_orbsInCorpse = mode == "INSIDE_CORPSE_INVENTORY";
        } else if (key == "bundlegold") {
            g_bundleGold = AsBool(value);
        } else if (key == "orbcleanupdays") {
            g_orbCleanupDays = static_cast<int>(AsFloat(value, 3.0f, 0.0f, 365.0f));
        } else if (key == "orbdropnotify") {
            g_orbDropNotify = AsBool(value);
        } else if (key == "orbdropmultiplier") {
            g_drops.multiplier = AsFloat(value, 1.0f, 0.0f, 20.0f);
        } else if (key == "orbdropchance") {
            g_drops.chance = AsFloat(value, 0.85f, 0.0f, 1.0f);
        } else if (key == "orbbasecount") {
            g_drops.baseCount = AsFloat(value, 1.5f, 0.0f, 20.0f);
        } else if (key == "orbcountperlevel") {
            g_drops.countPerLevel = AsFloat(value, 0.03f, 0.0f, 2.0f);
        } else if (key == "orbmaxcount") {
            g_drops.maxCount = AsFloat(value, 4.0f, 0.0f, 64.0f);
        } else if (key.rfind("orbweight", 0) == 0) {
            bool known = false;
            for (int i = 0; i < roll::kOrbCount; ++i) {
                if (key == kOrbWeightKeys[i]) {
                    g_drops.weights[static_cast<std::size_t>(i)] =
                        static_cast<int>(AsFloat(value, 0.0f, 0.0f, 100000.0f));
                    known = true;
                }
            }
            if (!known) {
                logger::warn("{}: unknown setting '{}'", kPath, key);
                continue;
            }
        } else {
            // Named, because a silently ignored setting is how someone spends an
            // evening wondering why their edit did nothing.
            logger::warn("{}: unknown setting '{}'", kPath, key);
            continue;
        }
        ++applied;
    }

    // ★THE ROLLER IS TOLD ONCE, HERE. src/roll/ links against nothing and
    // knows nothing about files; pushing the value in at load keeps it that way,
    // and keeps the balance harness able to link the same code.
    // ★THE HOST HAS ALREADY ANNOUNCED ITSELF BY NOW, and the ordering is what
    // makes this legal: Grid Inventory broadcasts at kPostLoad and Load() runs
    // at kDataLoaded, which is strictly later. Asking any earlier would read
    // "absent" for every player who has it.
    if (!g_tierMarkerInNameSet) {
        g_tierMarkerInName = !GridTint::Registered();
    }

    roll::SetTierMark(g_tierMarker);
    roll::SetTierMarkInName(g_tierMarkerInName);
    roll::SetAffixCaps(g_maxPrefixes, g_maxSuffixes);
    roll::SetAffixLevelGates(g_fifthMinLevel, g_sixthMinLevel);
    roll::SetDefaultVaalRules(g_vaal);
    {
        auto bands = roll::DefaultBands();
        for (std::size_t b = 0; b < bands.size() && b < g_rarityWeights.size(); ++b) {
            const auto& w = g_rarityWeights[b];
            if (w[0] >= 0 && w[1] >= 0 && w[2] >= 0) {
                roll::SetRarityWeights(bands[b], w[0], w[1], w[2]);
            }
        }
        roll::SetDefaultBands(std::move(bands));
        for (const auto& band : roll::DefaultBands()) {
            const auto w = roll::RarityWeightsOf(band);
            logger::info("{}: rarity weights at level {}: normal {} / magic {} / rare {}", kPath,
                band.level, w[0], w[1], w[2]);
        }
    }
    LoadRarityColours();
    logger::info("{}: affixes {} prefix + {} suffix, fifth from level {}, sixth from {}; Vaal "
                 "{} (second implicit {:.0f}%, then destroy/values/reroll {}/{}/{}); orb drops {} ({}, "
        "orbs {}), cleanup after {} day(s)",
        kPath, g_maxPrefixes, g_maxSuffixes, roll::DefaultFifthAffixMinLevel(),
        roll::DefaultSixthAffixMinLevel(), g_vaal.canDestroy ? "can destroy" : "never destroys",
        100.0f * g_vaal.addImplicitChance, g_vaal.weightDestroy, g_vaal.weightValues,
        g_vaal.weightReroll,
        g_orbDrops ? "on" : "OFF", Config::GearModeName(g_gearMode),
        g_orbsInCorpse ? "in the corpse" : "by gear mode", g_orbCleanupDays);

    logger::info(
        "{}: {} setting(s) applied -- distribution {}, quest rewards {}, discovery {}, "
        "verbose survey {}, tier marker {} ({}), container loot x{:.2f}, weapon charge {}",
        kPath, applied, g_distribution ? "on" : "OFF", g_questRewards ? "on" : "OFF",
        g_discovery ? "ON" : "off", g_verboseSurvey ? "ON" : "off",
        g_tierMarker.empty() ? std::string{ "(none)" } : "'" + g_tierMarker + "'",
        g_tierMarkerInName ? "in item names" : "on hover only (names left alone)",
        g_containerLoot,
        g_weaponCharge ? std::format("ON ({}/{}/{}/{}/{} charge by power step, {} + {}/point per hit)",
                             g_weaponChargeAmount[1], g_weaponChargeAmount[2],
                             g_weaponChargeAmount[3], g_weaponChargeAmount[4],
                             g_weaponChargeAmount[5],
                             g_weaponChargeCost, g_weaponChargeCostPerPoint)
                       : std::string{ "off (affixes never drain)" });
}

bool Config::DistributionEnabled()
{
    return g_distribution;
}

bool Config::QuestRewardsEnabled()
{
    return g_questRewards;
}

float Config::ContainerLootMultiplier()
{
    return g_containerLoot;
}

bool Config::DiscoveryEnabled()
{
    return g_discovery;
}

bool Config::ContainerTrace()
{
    return g_containerTrace;
}

bool Config::DebugLoadLog()
{
    return g_debugLoadLog;
}

bool Config::VerboseSurvey()
{
    return g_verboseSurvey;
}

std::string_view Config::TierMarker()
{
    return g_tierMarker;
}

bool Config::TierMarkerInName()
{
    return g_tierMarkerInName;
}

bool Config::WeaponChargeEnabled()
{
    return g_weaponCharge;
}

bool Config::WielderBuffsEnabled()
{
    return g_wielderBuffs;
}

bool Config::VendorStockEnabled()
{
    return g_vendorStock;
}

float Config::PriceMult(int a_powerStep)
{
    if (a_powerStep < 0 || a_powerStep >= roll::kPowerSteps) {
        return 1.0f;
    }
    return g_priceMult[a_powerStep];
}

bool Config::SellPricesScaled()
{
    return g_sellPricesScaled;
}

bool Config::PriceHookEnabled()
{
    return g_priceHook;
}

std::uint16_t Config::WeaponChargeAmount(int a_powerStep)
{
    if (a_powerStep < 0 || a_powerStep >= roll::kPowerSteps) {
        return 0;
    }
    return g_weaponChargeAmount[a_powerStep];
}

std::uint32_t Config::RarityColor(RarityColour a_which)
{
    // ★The grid asks at kPostLoad, before Load(); the first ask reads just
    // this section. Load() reads it again, so an edit plus a reload works too.
    std::call_once(g_rarityColorOnce, []() { LoadRarityColours(); });
    const auto i = static_cast<std::size_t>(a_which);
    return i < g_rarityColor.size() ? g_rarityColor[i] : 0u;
}

bool Config::ReplaceVanillaEnchants()
{
    return g_replaceVanilla;
}

bool Config::DropMultiplierAuto() { return g_dropMultAuto; }
void Config::SetDropMultiplierAuto(bool a_auto) { g_dropMultAuto = a_auto; }
float Config::DropMultiplierOverride() { return g_dropMultOverride; }
void Config::SetDropMultiplierOverride(float a_value)
{
    g_dropMultOverride = std::clamp(a_value, 1.0f, 20.0f);
}
float Config::DropQuantityPerDifficulty() { return g_dropPerDifficulty; }

int Config::WeaponChargeCost()
{
    return g_weaponChargeCost;
}

int Config::WeaponChargeCostPerPoint()
{
    return g_weaponChargeCostPerPoint;
}

bool Config::TooltipItemLevel()
{
    return g_tooltipItemLevel;
}

bool Config::TooltipAffixLines()
{
    return g_tooltipAffixLines;
}

int Config::TooltipDetailKey()
{
    return g_tooltipDetailKey;
}

// ---- orb drops ------------------------------------------------------------------

bool Config::OrbDropsEnabled() { return g_orbDrops; }
void Config::SetOrbDropsEnabled(bool a_enabled) { g_orbDrops = a_enabled; }
Config::GearMode Config::EnemyGearMode() { return g_gearMode; }
void Config::SetEnemyGearMode(GearMode a_mode) { g_gearMode = a_mode; }
bool Config::OrbsInCorpse() { return g_orbsInCorpse; }
bool Config::BundleGold() { return g_bundleGold; }
int  Config::OrbCleanupDays() { return g_orbCleanupDays; }
void Config::SetOrbCleanupDays(int a_days) { g_orbCleanupDays = std::clamp(a_days, 0, 365); }
bool Config::OrbDropNotify() { return g_orbDropNotify; }
void Config::SetOrbDropNotify(bool a_enabled) { g_orbDropNotify = a_enabled; }
roll::DropTuning& Config::Drops() { return g_drops; }

const char* Config::GearModeName(GearMode a_mode)
{
    switch (a_mode) {
    case GearMode::kAllGround:      return "ALL_GROUND";
    case GearMode::kAllInventory:   return "ALL_INVENTORY";
    case GearMode::kOrbsGround:     return "ORBS_GROUND";
    case GearMode::kItemsInventory: return "ITEMS_INVENTORY";
    }
    return "?";
}

void Config::ResetDrops()
{
    // Load() only assigns what the file names, so the defaults go back first.
    // ★Every drop key SaveDrops writes, the gear mode and the switch included:
    // an INI without the line must come back at the shipped default, not at
    // whatever the debug menu last set.
    g_drops = roll::DropTuning{};
    g_orbDrops = true;
    g_gearMode = Config::GearMode::kAllInventory;
    g_orbsInCorpse = false;
    g_bundleGold = true;
    g_orbCleanupDays = 3;
    g_orbDropNotify = false;
    g_dropMultAuto = true;
    g_dropMultOverride = 1.0f;
    g_dropPerDifficulty = 0.15f;
    Load();
}

// Rewrites each drop key's value in place (or appends it under [Drops]),
// leaving every other line and comment of the player's file as it was.
bool Config::SaveDrops()
{
    std::vector<std::pair<std::string, std::string>> keys{
        { "OrbDrops", g_orbDrops ? "1" : "0" },
        { "EnemyGearMode", GearModeName(g_gearMode) },
        { "OrbDropMultiplier", std::format("{:.2f}", g_drops.multiplier) },
        { "OrbDropChance", std::format("{:.3f}", g_drops.chance) },
        { "OrbBaseCount", std::format("{:.2f}", g_drops.baseCount) },
        { "OrbCountPerLevel", std::format("{:.3f}", g_drops.countPerLevel) },
        { "OrbMaxCount", std::format("{:.1f}", g_drops.maxCount) },
        { "OrbCleanupDays", std::to_string(g_orbCleanupDays) },
        { "OrbDropNotify", g_orbDropNotify ? "1" : "0" },
        { "DropQuantityMultiplier", g_dropMultAuto ? std::string{ "Auto" }
                                                   : std::format("{:.2f}", g_dropMultOverride) },
        { "DropQuantityPerDifficulty", std::format("{:.2f}", g_dropPerDifficulty) },
    };
    for (int i = 0; i < roll::kOrbCount; ++i) {
        keys.emplace_back(kOrbWeightNames[i], std::to_string(g_drops.weights[static_cast<std::size_t>(i)]));
    }

    std::vector<std::string> lines;
    {
        std::ifstream in{ kPath };
        std::string   line;
        while (std::getline(in, line)) {
            lines.push_back(line);
        }
    }
    const auto lower = [](std::string a_text) {
        std::transform(a_text.begin(), a_text.end(), a_text.begin(),
            [](unsigned char a_ch) { return static_cast<char>(std::tolower(a_ch)); });
        return a_text;
    };
    std::vector<std::string> missing;
    for (const auto& [key, value] : keys) {
        bool done = false;
        for (auto& line : lines) {
            const auto trimmed = Trim(line);
            const auto eq = trimmed.find('=');
            if (eq == std::string::npos || trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
                continue;
            }
            if (lower(Trim(trimmed.substr(0, eq))) == lower(key)) {
                line = key + "=" + value;
                done = true;
                break;
            }
        }
        if (!done) {
            missing.push_back(key + "=" + value);
        }
    }
    if (!missing.empty()) {
        lines.emplace_back("");
        lines.emplace_back("[Drops]");
        lines.insert(lines.end(), missing.begin(), missing.end());
    }
    std::ofstream out{ kPath, std::ios::trunc };
    if (!out) {
        logger::error("{}: could not be written; drop tuning not saved", kPath);
        return false;
    }
    for (const auto& line : lines) {
        out << line << '\n';
    }
    logger::info("{}: drop tuning saved ({} key(s) updated, {} added)", kPath,
        keys.size() - missing.size(), missing.size());
    return true;
}
