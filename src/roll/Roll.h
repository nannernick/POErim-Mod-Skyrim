// =============================================================================
//  The roller -- pure logic, deliberately free of Skyrim.
// =============================================================================
//  Nothing in this directory includes RE/, SKSE/, or PCH.h, and nothing here
//  needs a running game. That is not tidiness for its own sake: loot balance is
//  found by iterating, and iterating inside Skyrim costs a launch, a load and a
//  fight per attempt. A console harness linking these same files rolls a hundred
//  thousand items in under a second and prints the tier histogram, which turns
//  an hour per iteration into a keystroke.
//
//  The engine-facing half -- turning a RolledItem into an ENCH and stapling it
//  to an ExtraDataList -- lives elsewhere and depends on this, never the reverse.
// =============================================================================
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace roll
{
    // ----------------------------------------------------------------- paths
    // A path rendered for a log line or an error message. Always UTF-8, never
    // path::string(): that one converts through the ANSI code page and THROWS
    // on any character the page cannot hold, which on a non-English Windows is
    // a routine filename, not an edge case. Nothing that merely wants to print
    // a name should be able to take the process down.
    [[nodiscard]] inline std::string PathToUtf8(const std::filesystem::path& a_path)
    {
        const auto u8 = a_path.u8string();
        return std::string(u8.begin(), u8.end());
    }

    // ---------------------------------------------------------------- slots
    // A bit per place an affix can live. An item presents the mask it occupies
    // and an affix presents the mask it allows; they must intersect.
    //
    // Weapons carry a TYPE bit beside the generic one, the way armour carries
    // a piece bit beside kArmor: a greatsword presents kWeapon | kTwoHanded. An
    // affix that says WEAPON lands on any of them; one that says TWOHANDED
    // lands only there. That is how a skill bonus stays on the weapon whose
    // skill it is -- a two-handed weapon can never carry a One-Handed affix,
    // because no row that means One-Handed carries the kTwoHanded bit.
    //
    // Staves have a bit of their own. No combat skill governs them, but they
    // are held in one hand beside a spell, which is exactly where a One-Handed
    // or spell-cost bonus belongs -- so those rows name STAFF and the
    // elemental on-hit rows, which say WEAPON, reach them as before.
    enum Slot : std::uint32_t
    {
        kNone = 0,
        kWeapon = 1u << 0,  // any weapon, whatever its type
        kArmor = 1u << 1,   // any armor piece, including shields
        kShield = 1u << 2,
        kHead = 1u << 3,
        kBody = 1u << 4,
        kHands = 1u << 5,
        kFeet = 1u << 6,
        kRing = 1u << 7,
        kAmulet = 1u << 8,
        kOneHanded = 1u << 9,   // sword, dagger, axe, mace -- the One-Handed skill
        kTwoHanded = 1u << 10,  // greatsword, battleaxe, warhammer -- Two-Handed
        kBow = 1u << 11,        // bow and crossbow -- Archery
        kStaff = 1u << 12,      // staff -- no skill, one hand, a spell in the other
    };

    // The typed weapon bits together, so "does this row name a weapon type"
    // is one test rather than four.
    inline constexpr std::uint32_t kWeaponTypes = kOneHanded | kTwoHanded | kBow | kStaff;

    [[nodiscard]] std::uint32_t ParseSlots(std::string_view a_text);
    [[nodiscard]] std::string   SlotsToString(std::uint32_t a_slots);

    // ------------------------------------------------------------ affix data
    // One tier row of one affix. Points is explicit rather than derived from the
    // tier index, because the data has exceptions the index cannot express:
    // Paralysis has no Tier I, and the toggles are worth 1 or 2 with no ladder
    // at all.
    struct AffixTier
    {
        int           tier{ 0 };
        int           points{ 0 };
        float         minValue{ 0.0f };
        float         maxValue{ 0.0f };
        std::uint32_t duration{ 0 };
        std::uint32_t area{ 0 };
        std::uint32_t weight{ 100 };
        int           minItemLevel{ 0 };
    };

    // ---------------------------------------------------------- affix kinds
    //
    // Path of Exile's split, on this mod's points model. An item carries up to
    // MaxPrefixes() prefixes and MaxSuffixes() suffixes -- its EXPLICIT affixes,
    // the ones the orbs work on; three and three by default, six in all. The
    // fifth and sixth only arrive at Tuning::fifthAffixMinLevel and
    // sixthAffixMinLevel.
    //
    // An IMPLICIT is a separate slot OUTSIDE that cap: one stronger affix tied
    // to the kind of item (a ring's resistance, a weapon's elemental damage).
    // Nothing rolls one any more -- a Vaal Orb's corruption is the only way an
    // item gains it -- so a finished item can read "6 + implicit". Items rolled
    // by 1.1/1.2, where the implicit was the fifth slot, keep theirs.
    //
    // The kind is the CSV's eighteenth column. A table without it -- an add-on
    // written before the column existed -- gets one from the affix's category
    // (KindForCategory), so nothing has to be edited for it to keep loading.
    enum class AffixKind : std::uint8_t
    {
        kPrefix,
        kSuffix,
        kImplicit,
    };

    [[nodiscard]] const char* KindName(AffixKind a_kind) noexcept;   // "prefix"
    [[nodiscard]] AffixKind   KindForCategory(std::string_view a_category) noexcept;

    // The explicit caps. Process-wide, like the name marker, because every roll
    // site and every orb must agree on them; set once from DiabloLoot.ini.
    void SetAffixCaps(int a_maxPrefixes, int a_maxSuffixes) noexcept;
    [[nodiscard]] int MaxPrefixes() noexcept;
    [[nodiscard]] int MaxSuffixes() noexcept;

    struct Affix
    {
        std::string            id;
        std::string            name;
        std::string            category;
        std::string            unit;
        std::string            mgef;    // resolved by the engine layer, not here
        std::string            prefix;
        std::string            suffix;
        // The MOD GROUP: an item never carries two affixes of one group. The
        // CSV's optional nineteenth column; blank, it is the affixId, and
        // affixes that share a magic effect are put in one group automatically
        // (two of those would merge into a single line in the enchantment and
        // could not be told apart again).
        std::string            group;
        bool                   groupExplicit{ false };
        std::uint32_t          slots{ kNone };
        bool                   npcExclude{ false };
        AffixKind              kind{ AffixKind::kSuffix };
        std::vector<AffixTier> tiers;

        [[nodiscard]] bool Implicit() const noexcept { return kind == AffixKind::kImplicit; }
    };

    // ------------------------------------------------------------- the table
    class AffixTable
    {
    public:
        // Loads the CSV, appending a human-readable line to a_errors for every
        // malformed row. Returns false only when nothing usable loaded --
        // individual bad rows are skipped and reported, never fatal, because a
        // user editing a balance CSV should get a complaint rather than a crash.
        bool Load(const std::filesystem::path& a_path, std::vector<std::string>& a_errors);
        bool LoadFromString(std::string_view a_csv, std::vector<std::string>& a_errors);

        // Adds to what is already loaded. An affixId that already exists has its
        // tiers REPLACED, so an add-on can rebalance a base affix as well as
        // introduce new ones. This is how a "Summermyst affixes" patch ships as a
        // drop-in file instead of a fork of the base table.
        bool MergeFile(const std::filesystem::path& a_path, std::vector<std::string>& a_errors);
        bool Merge(std::string_view a_csv, std::vector<std::string>& a_errors);

        [[nodiscard]] const std::vector<Affix>& Affixes() const noexcept { return _affixes; }
        [[nodiscard]] std::vector<Affix>&       Affixes() noexcept { return _affixes; }
        [[nodiscard]] std::size_t               TierRowCount() const noexcept;
        [[nodiscard]] bool                      Empty() const noexcept { return _affixes.empty(); }

    private:
        std::vector<Affix> _affixes;
    };

    // ------------------------------------------------------------- the limits
    // The most affixes one item can carry, and the tier that many affixes at
    // Tier III add up to. Everything sized by "how many affixes" -- the count
    // curve, the harness's histogram -- reads these rather than repeating the
    // number, so the fifth affix was a change here and not a hunt.
    //
    // Six EXPLICIT affixes (three prefixes, three suffixes) and one implicit
    // slot beside them, so the point total tops out at seven affixes' worth.
    inline constexpr int kMaxAffixes = 6;
    inline constexpr int kMaxImplicits = 1;
    inline constexpr int kMaxAffixPoints = 3;
    inline constexpr int kMaxTier = (kMaxAffixes + kMaxImplicits) * kMaxAffixPoints;  // 21

    // The level floors for the fifth and sixth affix that a default-constructed
    // Tuning starts from. Process-wide for the same reason the caps are: every
    // roll site and every orb builds its own Tuning, and they must all agree
    // with DiabloLoot.ini. Set once on load; 35 and 40 until then.
    void SetAffixLevelGates(int a_fifthMinLevel, int a_sixthMinLevel) noexcept;
    [[nodiscard]] int DefaultFifthAffixMinLevel() noexcept;
    [[nodiscard]] int DefaultSixthAffixMinLevel() noexcept;

    // What a Vaal Orb may do (Craft.h). Weights are relative; an outcome the
    // item cannot take is left out of the draw.
    struct VaalRules
    {
        bool canDestroy{ true };
        int  weightDestroy{ 1 };
        int  weightValues{ 1 };
        int  weightImplicit{ 1 };
        int  weightReroll{ 1 };
    };
    void SetDefaultVaalRules(const VaalRules& a_rules) noexcept;
    [[nodiscard]] VaalRules DefaultVaalRules() noexcept;

    // ------------------------------------------------------------- the curves
    // Everything tunable lives here so the harness can sweep it without a
    // rebuild of anything else.
    struct Curve
    {
        int                                level{ 0 };
        std::array<int, kMaxAffixes + 1>   countWeights{};       // 0..6 affixes
        std::array<int, kMaxAffixPoints>   tierWeights{};        // tier I..III
    };

    struct Tuning
    {
        // Bands are interpolated between, so the curve is smooth rather than
        // stepped. Levels must be ascending.
        //
        // The last two count weights are the FIFTH and SIXTH AFFIX. They are
        // zero everywhere below the level-40 band on purpose: interpolation
        // would otherwise leak a sliver of them into the twenties, and the two
        // floors below are what make sure none of that sliver ever lands.
        //
        // The level-40 band's 6 + 2 is the 8 the single fifth-affix weight used
        // to be, so the share of items with five OR MORE affixes is unchanged.
        std::vector<Curve> bands{
            { 1, { 55, 30, 12, 3, 0, 0, 0 }, { 80, 18, 2 } },
            { 12, { 30, 33, 24, 11, 2, 0, 0 }, { 55, 35, 10 } },
            { 25, { 15, 27, 30, 20, 8, 0, 0 }, { 32, 43, 25 } },
            { 40, { 6, 16, 28, 26, 16, 6, 2 }, { 18, 40, 42 } },
        };

        // The item level below which a fifth affix cannot roll, whatever the
        // curve says. Red -- the five-star band -- needs thirteen points, and
        // thirteen points need five affixes, so this is also the floor under
        // red: nothing below it can come out red, however lucky the tiers.
        //
        // A FLOOR AND NOT A BAND, because the curve is interpolated and a band
        // at 35 reading zero would still let level 36 roll a fraction of the
        // level-40 weight. That is the smooth ramp the curve is for; this is
        // the promise that the ramp starts here and not a level sooner.
        int fifthAffixMinLevel{ DefaultFifthAffixMinLevel() };

        // The same floor for the sixth affix. A full 3 + 3 item starts here.
        int sixthAffixMinLevel{ DefaultSixthAffixMinLevel() };

        // What a Vaal Orb may do; from DiabloLoot.ini through SetDefaultVaalRules.
        VaalRules vaal{ DefaultVaalRules() };

        // NPCs roll the same curve as the player (your decision), but the hook
        // is here so an NPC-only ceiling is a one-line change rather than a
        // refactor. At kMaxTier it is no ceiling at all.
        int npcMaxTier{ kMaxTier };
    };

    // ------------------------------------------------------------- the result
    struct RolledAffix
    {
        const Affix* affix{ nullptr };
        int          tier{ 0 };
        int          points{ 0 };
        float        value{ 0.0f };
        std::uint32_t duration{ 0 };
        std::uint32_t area{ 0 };
    };

    struct RolledItem
    {
        std::vector<RolledAffix> affixes;
        int                      tier{ 0 };  // sum of points, 0..kMaxTier

        // A Vaal Orb has been used on it: no orb works on it again. Carried in
        // the save record's flags (Record.h), not in the enchantment.
        bool                     corrupted{ false };

        [[nodiscard]] std::string Name(std::string_view a_baseName) const;

        // How many of each kind the item carries.
        [[nodiscard]] int Count(AffixKind a_kind) const noexcept;
        [[nodiscard]] int Explicits() const noexcept
        {
            return Count(AffixKind::kPrefix) + Count(AffixKind::kSuffix);
        }
        [[nodiscard]] bool HasImplicit() const noexcept { return Count(AffixKind::kImplicit) > 0; }
    };

    struct RollContext
    {
        int           itemLevel{ 1 };
        std::uint32_t itemSlots{ kNone };
        bool          forNpc{ false };

        // Refuse the empty roll: this item gets at least one affix.
        //
        // ★A PROPERTY OF THE OCCASION, NOT OF THE CURVE, which is why it lives
        // here beside the item level rather than in Tuning. The curve says what
        // loot looks like in general and is swept by the harness as one thing;
        // this says that THIS roll is happening because something already
        // decided the item was worth marking. A quest reward is the case: the
        // player finished something to get it, and handing back a plain sword is
        // the system saying nothing on the one occasion it was asked directly.
        //
        // Implemented by zeroing the count-0 weight before the draw, not by
        // rolling again until something sticks. Those give the same
        // distribution -- the counts conditioned on being non-zero -- but only
        // one of them terminates in bounded time when the curve has no non-zero
        // weight to offer.
        bool noWhite{ false };
    };

    using Rng = std::mt19937;

    // The roll itself. Deterministic for a given rng state, which is what makes
    // the harness's numbers reproducible and a regression in balance visible.
    [[nodiscard]] RolledItem Roll(const AffixTable& a_table, const RollContext& a_ctx,
        const Tuning& a_tuning, Rng& a_rng);

    // ------------------------------------------------------- the single draw
    // The two halves of Roll's inner loop, exposed because a crafting orb adds
    // an affix to an existing item by the same rules a fresh roll picks one:
    // same slot filter, same level gates, same weights, same tier curve. Roll
    // itself is written in terms of these, so the two cannot drift apart.
    //
    // The pool is every affix this item could carry at its level. A caller
    // adding to an item removes what the item already has before drawing --
    // that is the "no duplicate affix" rule.
    // EXPLICIT affixes only. The implicit has a pool of its own.
    [[nodiscard]] std::vector<const Affix*> EligiblePool(const AffixTable& a_table,
        const RollContext& a_ctx);

    // The implicits this item could carry at its level and slot.
    [[nodiscard]] std::vector<const Affix*> ImplicitPool(const AffixTable& a_table,
        const RollContext& a_ctx);

    // Narrows a pool to what still fits on a_item: drops affixes it already
    // has, affixes in the same mod group as one it has (which covers two
    // affixes sharing a magic effect -- an implicit and an explicit of the same
    // effect would merge into one line in the enchantment and could not be
    // told apart again), and any kind whose cap the item has reached.
    void FitPool(std::vector<const Affix*>& a_pool, const RolledItem& a_item);

    // The tier curve at one item level, interpolated as Roll interpolates it.
    [[nodiscard]] std::array<int, kMaxAffixPoints> TierWeightsFor(const Tuning& a_tuning,
        int a_itemLevel);

    enum class Draw
    {
        kDrawn,      // a_out is filled and the affix has left the pool
        kSkipped,    // an affix left the pool but produced nothing; draw again
        kExhausted,  // nothing in the pool can be drawn at all
    };

    Draw DrawAffix(std::vector<const Affix*>& a_pool, const RollContext& a_ctx,
        const std::array<int, kMaxAffixPoints>& a_tierWeights, Rng& a_rng, RolledAffix& a_out);

    // 0 white, 1-3 blue, 4-6 yellow, 7-9 purple, 10-12 orange, 13 and up red.
    //
    // Red needs at least five affixes on the item -- four at Tier III stop at
    // twelve -- and a fifth explicit only arrives at or above
    // Tuning::fifthAffixMinLevel. Both gates fall out of the arithmetic rather
    // than being checked here, which is why BandOf still takes just the tier.
    // (A corrupted item's implicit counts toward the total like any affix.)
    enum class Band
    {
        kWhite,
        kBlue,
        kYellow,
        kPurple,
        kOrange,
        kRed
    };

    inline constexpr int kBandCount = static_cast<int>(Band::kRed) + 1;

    [[nodiscard]] Band        BandOf(int a_tier) noexcept;
    [[nodiscard]] const char* BandName(Band a_band) noexcept;

    // ------------------------------------------------------------- tier mark
    // The glyph repeated once per band at the end of a rolled item's name: one
    // for blue, five for red. Defaults to U+25C6 BLACK DIAMOND.
    //
    // HANDED IN, NOT BAKED IN. Which glyph actually draws depends on the
    // player's fonts and menu replacers, which is not something this file can
    // know -- so the plugin reads it from the INI and sets it once on load, and
    // the roller goes on knowing nothing about INIs. An EMPTY mark means no
    // marker at all: a supported choice, not a broken setting.
    //
    // Not thread-safe, deliberately: set once during load, before the first
    // roll, and never touched again.
    void SetTierMark(std::string_view a_mark);

    [[nodiscard]] std::string_view TierMark() noexcept;

    // The marker for one band, ready to draw: the mark repeated once per band,
    // so blue gets one and red five. Empty for a white item, and empty when
    // the mark itself is empty.
    //
    // TAKES A BAND, NOT A TIER, and that is the useful shape rather than an
    // arbitrary one. Persisted state records the band -- the raw tier is a
    // roll-time quantity that does not survive the save -- so anything asking
    // about an item already in the world has a Band and nothing else.
    [[nodiscard]] std::string BandMark(Band a_band);

    // Whether Name() appends the marker to the item's stored name.
    //
    // A SETTING RATHER THAN A CONSTANT because the marker is not free: it joins
    // the sort key of every inventory list, it is what a narrow barter column
    // truncates, and it is written into the save as a per-instance name
    // override. Whether that is worth paying depends on what else is drawing
    // the band -- with Grid Inventory installed the tint already says it, and
    // the glyphs are redundant.
    void               SetTierMarkInName(bool a_enabled) noexcept;
    [[nodiscard]] bool TierMarkInName() noexcept;
}
