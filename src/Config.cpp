// =============================================================================
//  Settings. See Config.h.
// =============================================================================

#include "PCH.h"

#include "Config.h"

#include "GridTint.h"
#include "roll/Roll.h"

#include <algorithm>
#include <format>
#include <fstream>
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

    // Indexed by roll::Band. White is a placeholder that is never read: a white
    // item carries no enchantment, so there is nothing to charge.
    std::uint16_t g_weaponChargeAmount[roll::kBandCount] = { 0, 1000, 1500, 2000, 2500, 3000 };

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
    float g_priceMult[roll::kBandCount] = { 1.0f, 1.5f, 2.0f, 3.0f, 5.0f, 10.0f };

    int              g_fifthMinLevel = 35;
    int              g_sixthMinLevel = 40;
    roll::VaalRules  g_vaal;
    bool             g_orbDrops = true;
    Config::GearMode g_gearMode = Config::GearMode::kOrbsGround;
    bool             g_orbsInCorpse = false;
    bool             g_bundleGold = true;
    int              g_orbCleanupDays = 3;
    bool             g_orbDropNotify = false;
    roll::DropTuning g_drops;

    constexpr const char* kOrbWeightKeys[roll::kOrbCount]{ "orbweighttransmute",
        "orbweightaugment", "orbweightalchemy", "orbweightexalt", "orbweightchaos",
        "orbweightscour", "orbweightdivine", "orbweightvaal" };
    constexpr const char* kOrbWeightNames[roll::kOrbCount]{ "OrbWeightTransmute",
        "OrbWeightAugment", "OrbWeightAlchemy", "OrbWeightExalt", "OrbWeightChaos",
        "OrbWeightScour", "OrbWeightDivine", "OrbWeightVaal" };

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

    // Reads one band's charge amount off an INI line. Sixteen bits is the
    // engine's own ceiling on an instance's charge.
    void ReadPriceMult(roll::Band a_band, const std::string& a_value)
    {
        auto& slot = g_priceMult[static_cast<int>(a_band)];
        slot = AsFloat(a_value, slot, 0.01f, 1000.0f);
    }

    void ReadChargeAmount(roll::Band a_band, const std::string& a_value)
    {
        auto& slot = g_weaponChargeAmount[static_cast<int>(a_band)];
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
            ReadChargeAmount(roll::Band::kBlue, value);
        } else if (key == "weaponchargeyellow") {
            ReadChargeAmount(roll::Band::kYellow, value);
        } else if (key == "weaponchargepurple") {
            ReadChargeAmount(roll::Band::kPurple, value);
        } else if (key == "weaponchargeorange") {
            ReadChargeAmount(roll::Band::kOrange, value);
        } else if (key == "weaponchargered") {
            ReadChargeAmount(roll::Band::kRed, value);
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
            ReadPriceMult(roll::Band::kBlue, value);
        } else if (key == "pricemultyellow") {
            ReadPriceMult(roll::Band::kYellow, value);
        } else if (key == "pricemultpurple") {
            ReadPriceMult(roll::Band::kPurple, value);
        } else if (key == "pricemultorange") {
            ReadPriceMult(roll::Band::kOrange, value);
        } else if (key == "pricemultred") {
            ReadPriceMult(roll::Band::kRed, value);
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
            g_vaal.weightImplicit = static_cast<int>(AsFloat(value, 1.0f, 0.0f, 1000.0f));
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
                             "ORBS_GROUND, ITEMS_INVENTORY; keeping ORBS_GROUND", kPath, value);
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
    logger::info("{}: affixes {} prefix + {} suffix, fifth from level {}, sixth from {}; Vaal "
                 "{} (weights {}/{}/{}/{}); orb drops {} ({}, orbs {}), cleanup after {} day(s)",
        kPath, g_maxPrefixes, g_maxSuffixes, roll::DefaultFifthAffixMinLevel(),
        roll::DefaultSixthAffixMinLevel(), g_vaal.canDestroy ? "can destroy" : "never destroys",
        g_vaal.weightDestroy, g_vaal.weightValues, g_vaal.weightImplicit, g_vaal.weightReroll,
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
        g_weaponCharge ? std::format("ON ({}/{}/{}/{}/{} charge blue..red, {} + {}/point per hit)",
                             g_weaponChargeAmount[static_cast<int>(roll::Band::kBlue)],
                             g_weaponChargeAmount[static_cast<int>(roll::Band::kYellow)],
                             g_weaponChargeAmount[static_cast<int>(roll::Band::kPurple)],
                             g_weaponChargeAmount[static_cast<int>(roll::Band::kOrange)],
                             g_weaponChargeAmount[static_cast<int>(roll::Band::kRed)],
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

float Config::PriceMult(roll::Band a_band)
{
    const auto index = static_cast<int>(a_band);
    if (index < 0 || index >= roll::kBandCount) {
        return 1.0f;
    }
    return g_priceMult[index];
}

bool Config::SellPricesScaled()
{
    return g_sellPricesScaled;
}

bool Config::PriceHookEnabled()
{
    return g_priceHook;
}

std::uint16_t Config::WeaponChargeAmount(roll::Band a_band)
{
    const auto index = static_cast<int>(a_band);
    if (index < 0 || index >= roll::kBandCount) {
        return 0;
    }
    return g_weaponChargeAmount[index];
}

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
    g_drops = roll::DropTuning{};
    g_orbCleanupDays = 3;
    g_orbDropNotify = false;
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
