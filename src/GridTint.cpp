// =============================================================================
//  Grid Inventory tint. See GridTint.h.
// =============================================================================

#include "PCH.h"

#include "GridTint.h"

#include "Apply.h"
#include "Config.h"
#include "Crafting.h"
#include "Orbs.h"
#include "Persist.h"
#include "Records.h"
#include "Sockets.h"
#include "api/GridInventoryAPI.h"
#include "roll/Roll.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

namespace
{
    std::atomic<bool> g_registered{ false };
    std::atomic<bool> g_offered{ false };
    std::atomic<bool> g_applierSent{ false };

    // The host's service table, kept from its announcement: it is valid for
    // the life of the process. Null until then, and forever without the host.
    std::atomic<const GridInvAPI::HostServices*> g_services{ nullptr };

    // ImU32 on the host's build is 0xAABBGGRR -- red in the LOW byte. Written
    // as a function so the constants below can be read in the order a colour is
    // normally quoted, rather than reversed by hand and mis-transcribed once.
    constexpr std::uint32_t Rgba(std::uint8_t a_r, std::uint8_t a_g, std::uint8_t a_b)
    {
        return (std::uint32_t{ 0xFF } << 24) | (std::uint32_t{ a_b } << 16) |
               (std::uint32_t{ a_g } << 8) | std::uint32_t{ a_r };
    }

    // ★TINT TIERS (2.1.0): rarity, not the point band. Normal is a tier of its
    // own and NOT "no opinion": since every item carries its base implicit, a
    // normal item has an enchantment, and the host would otherwise draw it in
    // its enchanted blue. Colours come from DiabloLoot.ini [Rarity].
    enum TintTier : std::uint8_t
    {
        kTintNone = 0,
        kTintNormal = 1,
        kTintMagic = 2,
        kTintRare = 3,
        kTintCorrupted = 4,
        kTintCurrency = 5,
        kTintCount = 5
    };
    static_assert(kTintCount <= GridInvAPI::kMaxTintTier, "the ABI cannot carry this many tiers");

    std::uint32_t TierColour(std::uint8_t a_tier)
    {
        switch (a_tier) {
        case kTintNormal:    return Config::RarityColor(Config::RarityColour::kNormal);
        case kTintMagic:     return Config::RarityColor(Config::RarityColour::kMagic);
        case kTintRare:      return Config::RarityColor(Config::RarityColour::kRare);
        case kTintCorrupted: return Config::RarityColor(Config::RarityColour::kCorrupted);
        case kTintCurrency:  return Config::RarityColor(Config::RarityColour::kCurrency);
        default:             return 0u;
        }
    }

    std::uint8_t TierOfCode(std::uint8_t a_code)
    {
        if (a_code == 0) {
            return kTintNone;
        }
        if (roll::CorruptedFromCode(a_code)) {
            return kTintCorrupted;   // ★red overrides the rarity colour
        }
        switch (roll::RarityFromCode(a_code)) {
        case roll::Rarity::kNormal: return kTintNormal;
        case roll::Rarity::kMagic:  return kTintMagic;
        case roll::Rarity::kRare:   return kTintRare;
        default:                    return kTintNone;
        }
    }

    // The detail view's headings: a quiet grey, so the affixes stay the loud part.
    constexpr std::uint32_t kHeadingColour = Rgba(150, 150, 150);

    // ---- the tables the host calls ---------------------------------------

    // The rarity code of one unit, 0 when this mod did not roll it.
    //
    // SHARED BY EVERY HOOK, and that sharing is the point: the tint, the
    // tooltip and the price must never disagree about an item.
    std::uint8_t CodeOf(const void* a_xl)
    {
        // No list of its own means a plain unit -- nothing this mod has touched,
        // because attaching an affix is exactly what creates one.
        if (!a_xl) {
            return 0;
        }
        const auto* list = static_cast<const RE::ExtraDataList*>(a_xl);
        const auto* xe = list->GetByType<RE::ExtraEnchantment>();
        if (!xe || !xe->enchantment) {
            return 0;
        }
        // THE ENCHANTMENT IS THE KEY, not the base form and not the list: the
        // one thing that belongs to this instance and survives being carried,
        // looted, stored and reloaded. A miss on ours is computed once from
        // the item record (Records::CodeFor); a vanilla one answers 0.
        return Records::CodeFor(xe->enchantment);
    }

    // HOT PATH. Once per visible tile per frame, again per equipment-doll slot,
    // and again while a tooltip is built. A hash lookup after the first ask.
    std::uint8_t GetTier(void*, std::uint32_t a_base, const void* a_xl)
    {
        if (Orbs::OrbOf(a_base) >= 0) {
            return kTintCurrency;
        }
        return TierOfCode(CodeOf(a_xl));
    }

    // ---- first sight ------------------------------------------------------
    //
    // ★A UNIT THE PLAYER LOOKS AT gets its base implicit (Crafting::
    // GrantBaseImplicit): a plain generic weapon or armour, a 1.2.0 item with
    // none, or a vanilla/player-enchanted generic one (ReplaceVanillaEnchants).
    // The tooltip only QUEUES it, once per unit per session; the change runs
    // on the game thread, one task each. Items that are not in the player's
    // inventory are left alone (FindUnit finds nothing).
    std::mutex                                          g_sightLock;
    std::set<std::pair<std::uint32_t, std::uint32_t>>   g_sightAsked;

    // Off for lines asked through DiabloLootAPI: a GROUND item's hover must
    // never change some other unit in the player's pack.
    thread_local bool t_noFirstSight = false;

    void QueueFirstSight(std::uint32_t a_base, std::uint32_t a_enchantment)
    {
        if (t_noFirstSight) {
            return;
        }
        {
            std::scoped_lock lock{ g_sightLock };
            if (!g_sightAsked.emplace(a_base, a_enchantment).second) {
                return;
            }
        }
        SKSE::GetTaskInterface()->AddTask([a_base, a_enchantment]() {
            const auto outcome = Crafting::GrantBaseImplicit({ a_base, a_enchantment });
            if (outcome.ok) {
                logger::info("first sight[{:08X}]: {}", a_base, outcome.message);
            }
        });
    }

    bool WantsFirstSight(std::uint32_t a_base, const RE::ExtraDataList* a_xl)
    {
        auto* object = RE::TESForm::LookupByID<RE::TESBoundObject>(a_base);
        if (!object || (!object->Is(RE::FormType::Weapon) && !object->Is(RE::FormType::Armor)) ||
            Apply::SlotsOf(object) == roll::kNone) {
            return false;
        }
        const auto kind = Apply::ClassifyEnchanted(object, a_xl);
        if (kind == Apply::VanillaKind::kRefused) {
            return false;
        }
        // ★THE PLAYER'S OWN ENCHANTING is replaced only when they use an orb
        // on it, never by a hover: they paid for it a moment ago.
        if (kind == Apply::VanillaKind::kInstanceEnchanted) {
            return false;
        }
        if (kind != Apply::VanillaKind::kNone) {
            return Config::ReplaceVanillaEnchants();
        }
        return Apply::IsGeneric(object) && Apply::IsEligible(object, const_cast<RE::ExtraDataList*>(a_xl));
    }

    // The item the grid last built a tooltip for, as ids rather than pointers:
    // the list is borrowed for the call and the enchantment can be destroyed.
    // The debug menu's inspector reads this to show "the hovered item".
    std::atomic<std::uint32_t> g_hoverBase{ 0 };
    std::atomic<std::uint32_t> g_hoverEnch{ 0 };

    // One line into the host's buffer, truncated ON A CODEPOINT BOUNDARY. The
    // text can hold whatever the player typed into the INI or an affix table,
    // and cutting a UTF-8 sequence in half would hand the host bytes that are
    // not text -- so back off over any continuation bytes (10xxxxxx) first.
    void PutLine(GridInvAPI::TooltipLine& a_line, std::string_view a_text, std::uint32_t a_rgba,
        std::uint8_t a_indent = 0, bool a_separator = false)
    {
        std::size_t n = (std::min)(a_text.size(),
            static_cast<std::size_t>(GridInvAPI::kTooltipTextLen - 1));
        while (n > 0 && n < a_text.size() &&
               (static_cast<unsigned char>(a_text[n]) & 0xC0) == 0x80) {
            --n;
        }
        std::memcpy(a_line.text, a_text.data(), n);
        a_line.text[n] = '\0';
        a_line.rgba = a_rgba;
        a_line.indent = a_indent;
        a_line.separatorBefore = a_separator ? 1 : 0;
        a_line._pad[0] = a_line._pad[1] = 0;
    }

    // 12 rather than 12.0, 12.5 where the half matters. Values are rolled as
    // floats inside a range; the game rounds them on the item card and so do we.
    std::string Number(float a_value)
    {
        const float rounded = std::round(a_value);
        if (std::fabs(a_value - rounded) < 0.05f) {
            return std::format("{:.0f}", rounded);
        }
        return std::format("{:.1f}", a_value);
    }

    std::string_view UnitSuffix(const roll::Affix& a_affix)
    {
        if (a_affix.unit == "pct") {
            return "%";
        }
        if (a_affix.unit == "seconds") {
            return "s";
        }
        return "";
    }

    // "Fire Damage 14", or with the detail key held "Fire Damage 14  [Tier II: 11 to 20]".
    std::string AffixLine(const roll::RolledAffix& a_rolled, bool a_detail)
    {
        const auto& affix = *a_rolled.affix;
        const bool  toggle = affix.unit == "toggle";

        std::string line = affix.name;
        if (!toggle) {
            line += ' ';
            line += Number(a_rolled.value);
            line += UnitSuffix(affix);
        }
        if (a_detail) {
            static constexpr const char* kRoman[]{ "", "I", "II", "III" };
            const char* numeral = (a_rolled.tier >= 1 && a_rolled.tier <= 3) ? kRoman[a_rolled.tier] : "?";
            if (const auto* tier = roll::FindTier(affix, a_rolled.tier); tier && !toggle) {
                line += std::format("  [Tier {}: {} to {}]", numeral, Number(tier->minValue),
                    Number(tier->maxValue));
            } else {
                line += std::format("  [Tier {}]", numeral);
            }
        }
        return line;
    }

    // ★WHAT THE ITEM ROLLED, ON HOVER.
    //
    // NOT the hot path: once per tooltip. It may build strings, which GetTier
    // above may not.
    //
    // Up to three kinds of line, each with its own switch in the INI:
    //
    //   the marker    the band glyphs the item's NAME used to carry. Silent when
    //                 TierMarkerInName is on: the host draws the name on this
    //                 same tooltip, and the glyphs twice looks like a bug.
    //   item level    what the item rolled at, which is what a crafting orb
    //                 rolls new affixes against.
    //   the affixes   one line each, in the band's colour. With the detail key
    //                 held (Alt by default) each also shows its tier and the
    //                 range that tier rolls -- how good THIS roll is.
    // What an orb does, on the orb's own tooltip. PoE1 rules (2.1.0).
    std::uint32_t OrbLines(roll::Orb a_orb, GridInvAPI::TooltipLine* a_out, std::uint32_t a_capacity)
    {
        const char* what = "";
        switch (a_orb) {
        case roll::Orb::kTransmute: what = "Turns a normal item magic, with 1 mod"; break;
        case roll::Orb::kAugment:   what = "Adds a second mod to a magic item with 1"; break;
        case roll::Orb::kRegal:     what = "Turns a magic item with 2 mods rare, adding a third"; break;
        case roll::Orb::kAlchemy:   what = "Turns a normal item rare, with 4 mods"; break;
        case roll::Orb::kExalt:     what = "Adds a mod to a rare item (up to 6)"; break;
        case roll::Orb::kChaos:     what = "Rerolls a rare item's mods, keeping the count"; break;
        case roll::Orb::kScour:     what = "Removes every mod; implicits stay"; break;
        case roll::Orb::kDivine:    what = "Rerolls the values of every mod and implicit"; break;
        case roll::Orb::kVaal:      what = "Corrupts an item, with unpredictable results"; break;
        }
        std::uint32_t n = 0;
        if (n < a_capacity) {
            PutLine(a_out[n++], what, Config::RarityColor(Config::RarityColour::kCurrency));
        }
        if (n < a_capacity) {
            PutLine(a_out[n++], "Right-click, then click an item to use. Shift keeps it.",
                kHeadingColour, 0, true);
        }
        return n;
    }

    // ---- the currency applier ------------------------------------------------

    void CopyReason(const std::string& a_text, char* a_out, std::uint32_t a_capacity)
    {
        if (!a_out || a_capacity == 0) {
            return;
        }
        const auto n = (std::min)(a_text.size(), static_cast<std::size_t>(a_capacity - 1));
        std::memcpy(a_out, a_text.data(), n);
        a_out[n] = '\0';
    }

    bool IsCurrency(void*, std::uint32_t a_base)
    {
        return Orbs::OrbOf(a_base) >= 0;
    }

    std::uint32_t Query(void*, std::uint32_t a_currency, std::uint32_t a_target, const void* a_xl,
        char* a_reason, std::uint32_t a_capacity)
    {
        const int orb = Orbs::OrbOf(a_currency);
        if (orb < 0) {
            CopyReason("Not an orb", a_reason, a_capacity);
            return GridInvAPI::kApplyNo;
        }
        std::string reason;
        const bool  ok = Orbs::Check(static_cast<roll::Orb>(orb), a_target,
            static_cast<const RE::ExtraDataList*>(a_xl), reason);
        CopyReason(reason, a_reason, a_capacity);
        return ok ? GridInvAPI::kApplyYes : GridInvAPI::kApplyNo;
    }

    std::uint32_t Commit(void*, std::uint32_t a_currency, std::uint32_t a_target, void* a_xl,
        char* a_reason, std::uint32_t a_capacity)
    {
        const int orb = Orbs::OrbOf(a_currency);
        if (orb < 0) {
            CopyReason("Not an orb", a_reason, a_capacity);
            return GridInvAPI::kApplyNo;
        }
        std::string message;
        const bool  ok = Orbs::Use(static_cast<roll::Orb>(orb), a_target,
            static_cast<RE::ExtraDataList*>(a_xl), message);
        CopyReason(message, a_reason, a_capacity);
        return ok ? GridInvAPI::kApplyYes : GridInvAPI::kApplyNo;
    }

    std::uint32_t GetLines(void*, std::uint32_t a_base, const void* a_xl,
        GridInvAPI::TooltipLine* a_out, std::uint32_t a_capacity)
    {
        if (!a_out || a_capacity == 0) {
            return 0;
        }

        if (const int orb = Orbs::OrbOf(a_base); orb >= 0) {
            return OrbLines(static_cast<roll::Orb>(orb), a_out, a_capacity);
        }
        const auto* list = static_cast<const RE::ExtraDataList*>(a_xl);
        const auto* xe = list ? list->GetByType<RE::ExtraEnchantment>() : nullptr;
        const bool  ours = xe && xe->enchantment && Persist::IsAffixEnch(xe->enchantment->GetFormID());
        if (!ours) {
            // Not ours yet: a plain generic item or a replaceable vanilla one
            // gets its base implicit now (queued, game thread).
            if (WantsFirstSight(a_base, list)) {
                QueueFirstSight(a_base, (xe && xe->enchantment) ? xe->enchantment->GetFormID() : 0u);
            }
            return 0;
        }
        const std::uint8_t code = CodeOf(a_xl);
        if (code == 0) {
            return 0;
        }
        const std::uint32_t colour = TierColour(TierOfCode(code));
        const auto          rarity = roll::RarityFromCode(code);

        const Records::ReadGuard guard{ Records::TableLock() };
        std::uint32_t            n = 0;

        if (!roll::TierMarkInName()) {
            if (const std::string mark = roll::RarityMark(rarity); !mark.empty()) {
                PutLine(a_out[n], mark, colour);
                ++n;
            }
        }

        const bool wantLevel = Config::TooltipItemLevel();
        const bool wantAffixes = Config::TooltipAffixLines();

        Records::View view;
        if (!Records::Lookup(list, view)) {
            return n;   // the player's own merge, or an item the table can no longer read
        }
        g_hoverBase.store(a_base);
        g_hoverEnch.store(view.enchantment);
        // A 1.2.0 item with no implicit gets its base one on first sight.
        if (view.rolled.Implicits() == 0 && (view.record.flags & roll::kRecordImplicitGranted) == 0) {
            QueueFirstSight(a_base, view.enchantment);
        }

        if (wantLevel && n < a_capacity) {
            const bool estimated = (view.record.flags & roll::kRecordLegacy) != 0;
            PutLine(a_out[n],
                std::format("Item Level {}{}", view.record.itemLevel, estimated ? " (estimated)" : ""),
                0u, 0, n > 0);
            ++n;
        }
        if (view.rolled.corrupted && n < a_capacity) {
            PutLine(a_out[n], "Corrupted", Config::RarityColor(Config::RarityColour::kCorrupted));
            ++n;
        }
        if (!wantAffixes) {
            return n;
        }

        // GetAsyncKeyState, not the game's input: this runs on the host's
        // render thread, and all it needs is whether one key is down now.
        const int  key = Config::TooltipDetailKey();
        const bool detail = key > 0 && (::GetAsyncKeyState(key) & 0x8000) != 0;

        // ★PATH OF EXILE'S LAYOUT. Implicits are HIDDEN in the normal tooltip
        // (2.1.0) and shown only with the detail key held, under an "Implicit"
        // heading in the item's colour, above a rule. The explicits follow,
        // prefixes before suffixes; with the key held each kind gets a heading
        // with how full it is, and each mod its tier and roll range.
        const bool magic = rarity == roll::Rarity::kMagic;
        const auto section = [&](roll::AffixKind a_kind, const char* a_heading, int a_cap,
                                 std::uint32_t a_headingColour, bool& a_ruleNext) {
            int have = 0;
            for (const auto& rolled : view.rolled.affixes) {
                if (rolled.affix && rolled.affix->kind == a_kind) {
                    ++have;
                }
            }
            if (have == 0 && !detail) {
                return;
            }
            if (detail && n < a_capacity) {
                const std::string heading = a_cap > 0 ? std::format("{} ({}/{})", a_heading, have, a_cap)
                                                      : std::string{ a_heading };
                PutLine(a_out[n], heading, a_headingColour, 0, a_ruleNext);
                ++n;
                a_ruleNext = false;
            }
            for (const auto& rolled : view.rolled.affixes) {
                if (n >= a_capacity) {
                    return;
                }
                if (!rolled.affix || rolled.affix->kind != a_kind) {
                    continue;
                }
                PutLine(a_out[n], AffixLine(rolled, detail), colour,
                    detail ? std::uint8_t{ 1 } : std::uint8_t{ 0 }, a_ruleNext);
                ++n;
                a_ruleNext = false;
            }
        };

        bool rule = false;
        if (detail && view.rolled.HasImplicit()) {
            section(roll::AffixKind::kImplicit, "Implicit", 0, colour, rule);
            rule = true;   // the rule PoE draws under the implicit
        }
        section(roll::AffixKind::kPrefix, "Prefixes", magic ? roll::kMagicMaxPrefixes : roll::MaxPrefixes(),
            kHeadingColour, rule);
        section(roll::AffixKind::kSuffix, "Suffixes", magic ? roll::kMagicMaxSuffixes : roll::MaxSuffixes(),
            kHeadingColour, rule);
        return n;
    }

    // ★WHAT A ROLLED ITEM IS WORTH AT THE HOST'S COUNTER -- the same rule
    // Pricing.cpp applies to the vanilla menus, answered over the ABI because
    // the host's shop never goes near the function that hook patches: it
    // calls the engine's value routine itself, from its own DLL, and the
    // vanilla BarterMenu the hook reads direction from is hidden while the
    // host's window is up. Without this table a purple sword sold in the grid
    // for its plain value with SellPricesScaled on OR off, since neither
    // setting was ever consulted.
    //
    // NOT the hot path: once per shelf cell at collect, once per tooltip, once
    // per sale. Still one extra-data walk and one hash lookup, and two reads
    // of settings that were loaded once at startup.
    float GetMultiplier(void*, std::uint32_t, const void* a_xl, std::uint32_t a_side)
    {
        const int step = roll::PowerStepFromCode(CodeOf(a_xl));
        if (step == 0) {
            return 1.0f;   // not ours, or no points: the host prices it as it always did
        }
        // The selling side is scaled only when the INI says so -- the same
        // default Pricing.h explains: what a merchant asks goes up, what a
        // merchant pays does not, unless the player chose otherwise.
        if (a_side == GridInvAPI::kPriceSell && !Config::SellPricesScaled()) {
            return 1.0f;
        }
        const float mult = Config::PriceMult(step);
        return mult > 0.0f ? mult : 1.0f;
    }

    // ★ANOTHER MOD'S SOCKETS, IN THE HOST'S WELLS. Socketing's row for the
    // unit, turned into the badges the grid already knows how to draw: a
    // filled socket names the shard's own item form (the host falls back to
    // capturing that form's model if the picture below will not load), an
    // empty one is a bare well -- exactly what the circle alone should say.
    //
    // HOT PATH, the same as GetTier: the host's partner window and doll ask
    // per cell per frame. Sockets::RowOf is one extra-data read and one hash
    // lookup once a unit has been seen.
    bool GetSockets(void*, std::uint32_t a_base, const void* a_xl, GridInvAPI::Overlay* a_out)
    {
        if (!a_out) {
            return false;
        }
        const auto* row = Sockets::RowOf(a_base, static_cast<const RE::ExtraDataList*>(a_xl));
        if (!row) {
            return false;
        }
        const auto n = (std::min)(static_cast<std::uint32_t>(row->count), GridInvAPI::kMaxBadges);
        a_out->count = static_cast<std::uint8_t>(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            auto& b = a_out->badges[i];
            if (row->shard[i] != 0) {
                b.state = GridInvAPI::kBadgeFilled;
                b.iconId = row->shard[i];
                // Socketing's own tint for the stone, on the rim. ★The Badge
                // takes 0xRRGGBBAA -- NOT the palette's ImU32 order above;
                // the host converts this one itself (see Badges.cpp).
                b.tintRGBA = (row->rgb[i] << 8) | 0xFFu;
            } else {
                b.state = GridInvAPI::kBadgeEmpty;
                b.iconId = 0;
                b.tintRGBA = 0;
            }
        }
        return n > 0;
    }

    // The picture of a shard or gem. NOT the hot path: the host asks once per
    // form per session and keeps the texture.
    bool GetArt(void*, std::uint32_t a_iconId, char* a_out, std::uint32_t a_capacity)
    {
        return Sockets::ArtOf(a_iconId, a_out, a_capacity);
    }

    // ★WHAT THE SOCKETS DO, for the tooltip. Socketing's effects ride a
    // created enchantment whose effects are hidden from every item card, so
    // the grid's own enchantment lines say nothing for a socketed unit; these
    // are drawn in their place. Socketing words them itself, already in the
    // TooltipLine layout and the grid's ImU32 colour order -- copied field by
    // field all the same, so the two headers only have to agree on meaning.
    //
    // Once per frame while the unit's tooltip is up; Sockets::LinesOf keeps
    // what Socketing said, so this is a hash lookup and a copy.
    std::uint32_t GetSocketLines(void*, std::uint32_t a_base, const void* a_xl,
        GridInvAPI::TooltipLine* a_out, std::uint32_t a_capacity)
    {
        if (!a_out || a_capacity == 0) {
            return 0;
        }
        const auto lines = Sockets::LinesOf(a_base, static_cast<const RE::ExtraDataList*>(a_xl));
        const auto n = static_cast<std::uint32_t>(
            (std::min)(lines.size(), static_cast<std::size_t>(a_capacity)));
        for (std::uint32_t i = 0; i < n; ++i) {
            const auto& src = lines[i];
            auto&       dst = a_out[i];
            static_assert(sizeof(dst.text) == sizeof(src.text), "both are 128-byte lines");
            std::memcpy(dst.text, src.text, sizeof(dst.text));
            dst.text[sizeof(dst.text) - 1] = '\0';   // never hand on an unterminated line
            dst.rgba = src.rgba;
            dst.indent = src.indent;
            dst.separatorBefore = src.separatorBefore;
        }
        return n;
    }

    std::uint32_t GetPalette(void*, std::uint32_t* a_out, std::uint32_t a_capacity)
    {
        if (!a_out) {
            return 0;
        }
        const auto n = (std::min)(a_capacity, static_cast<std::uint32_t>(kTintCount));
        for (std::uint32_t i = 0; i < n; ++i) {
            a_out[i] = TierColour(static_cast<std::uint8_t>(i + 1));
        }
        return n;
    }

    // ---- the handshake ----------------------------------------------------

    void Offer()
    {
        // Once. The host warns and ignores a second table, and a warning in
        // someone else's log is a bad way to say "we have a bug".
        if (g_offered.exchange(true)) {
            return;
        }

        // Static, not a local. The host copies the struct during the call, but
        // `name` is a borrowed pointer it keeps -- so the string has to outlive
        // this function, and making the whole table static is the cheapest way
        // to be sure of that.
        static GridInvAPI::Tinter tinter{
            sizeof(GridInvAPI::Tinter),
            GridInvAPI::kABIVersion,
            "DiabloInSkyrim",
            nullptr,   // self: we keep no per-instance state
            &GetTier,
            &GetPalette,
        };

        const bool sent = SKSE::GetMessagingInterface()->Dispatch(
            GridInvAPI::kMsgRegisterTinter, &tinter, sizeof(tinter),
            GridInvAPI::kHostPluginName);

        // ★A SECOND TABLE, A SECOND SLOT, AND IT IS NOT THE PROVIDER ONE.
        //
        // The tooltip line cannot ride Provider::GetTooltipLines: that hook is
        // keyed by ItemKey, whose uid is ExtraUniqueID -- 0 for every renamed or
        // enchanted unit, which is every item we have touched. Annotator is
        // handed the ExtraDataList instead, exactly as the tinter is, and it is
        // its own slot, so a socket mod holding the provider slot keeps it.
        static GridInvAPI::Annotator annot{
            sizeof(GridInvAPI::Annotator),
            GridInvAPI::kABIVersion,
            "DiabloInSkyrim",
            nullptr,   // self: we keep no per-instance state
            &GetLines,
        };

        const bool sentAnnot = SKSE::GetMessagingInterface()->Dispatch(
            GridInvAPI::kMsgRegisterAnnot, &annot, sizeof(annot),
            GridInvAPI::kHostPluginName);

        // ★A THIRD TABLE, FOR THE COUNTER. The host's shop window is the one
        // place a price is shown that the call-site hook in Pricing.cpp cannot
        // reach (see GetMultiplier above), so the host asks us instead. Same
        // slot discipline as the two before it: its own message, and a host
        // built before it existed simply never sends for it -- the prices
        // there stay plain, as they were, and nothing else is lost.
        static GridInvAPI::Pricer pricer{
            sizeof(GridInvAPI::Pricer),
            GridInvAPI::kABIVersion,
            "DiabloInSkyrim",
            nullptr,   // self: we keep no per-instance state
            &GetMultiplier,
        };

        const bool sentPrice = SKSE::GetMessagingInterface()->Dispatch(
            GridInvAPI::kMsgRegisterPricer, &pricer, sizeof(pricer),
            GridInvAPI::kHostPluginName);

        // ★A FOURTH TABLE, AND ONLY WHEN THERE IS SOMETHING IN IT. Without
        // Socketing every answer would be "no sockets", asked per cell per
        // frame for nothing. The host announces itself during SKSE's kPostLoad,
        // which is when Socketing's interface can first be asked for -- so
        // this may well be the call that finds it; main.cpp's kPostLoad makes
        // the same one, and whichever runs first is the one that does it.
        // A host built before the table existed ignores the message, and its
        // wells stay the provider's, as they were.
        bool sentSockets = false;
        if (Sockets::Connect()) {
            static GridInvAPI::Socketer socketer{
                sizeof(GridInvAPI::Socketer),
                GridInvAPI::kABIVersion,
                "DiabloInSkyrim",
                nullptr,   // self: the rows live in Sockets.cpp
                &GetSockets,
                &GetArt,
                &GetSocketLines,
            };
            sentSockets = SKSE::GetMessagingInterface()->Dispatch(
                GridInvAPI::kMsgRegisterSocketer, &socketer, sizeof(socketer),
                GridInvAPI::kHostPluginName);
        }

        // Dispatch returning true means the message was DELIVERED, not that the
        // host accepted the table -- the version gate and the null-pointer check
        // both run inside its handler and refuse invisibly from here. The host
        // logs its own verdict; this line only proves we spoke.
        static GridInvAPI::Applier applier{
            sizeof(GridInvAPI::Applier),
            GridInvAPI::kABIVersion,
            "DiabloInSkyrim",
            nullptr,
            &IsCurrency,
            &Query,
            &Commit,
        };
        const bool sentApplier = SKSE::GetMessagingInterface()->Dispatch(
            GridInvAPI::kMsgRegisterApplier, &applier, sizeof(applier),
            GridInvAPI::kHostPluginName);
        g_applierSent.store(sentApplier);
        logger::info("grid tint: orb applier {}", sentApplier ? "delivered" : "not delivered");

        g_registered.store(sent);
        logger::info("grid tint: offered {} rarity tier(s) to {} -- tint {}, tooltip {}, price {}, sockets {}",
            static_cast<int>(kTintCount), GridInvAPI::kHostPluginName,
            sent ? "delivered" : "not delivered (is Grid Inventory installed?)",
            sentAnnot ? "delivered" : "not delivered",
            sentPrice ? "delivered" : "not delivered",
            sentSockets ? "delivered" : "not offered (no Socketing)");
    }

    // The host's last currency report (kMsgCurrencyReport): what size and
    // icon each orb really got, for the debug menu's Integration page.
    std::mutex                   g_reportLock;
    GridInvAPI::CurrencyReport   g_report{};
    std::atomic<bool>            g_reportSeen{ false };

    void OnCurrencyReport(const SKSE::MessagingInterface::Message* a_msg)
    {
        if (!a_msg->data || a_msg->dataLen < sizeof(GridInvAPI::CurrencyReport)) {
            return;
        }
        const auto* report = static_cast<const GridInvAPI::CurrencyReport*>(a_msg->data);
        if (report->structSize != sizeof(GridInvAPI::CurrencyReport) ||
            report->abiVersion != GridInvAPI::kABIVersion) {
            return;
        }
        std::scoped_lock lock{ g_reportLock };
        g_report = *report;
        g_report.count = (std::min)(g_report.count, GridInvAPI::kMaxCurrencyRows);
        g_reportSeen.store(true);
        for (std::uint32_t i = 0; i < g_report.count; ++i) {
            const auto& row = g_report.rows[i];
            if (row.w != 1 || row.h != 1 || row.iconSource != GridInvAPI::kCurrencyIconPng) {
                logger::warn("grid report: currency {:08X} is {}x{} with icon source {} -- expected "
                             "1x1 and its own PNG",
                    row.base, row.w, row.h, row.iconSource);
            }
        }
    }

    // Takes EVERY sender. Only Grid Inventory's own 4CC types may be acted on
    // here -- see the note in GridTint.h.
    void OnApiMessage(SKSE::MessagingInterface::Message* a_msg)
    {
        if (a_msg && a_msg->type == GridInvAPI::kMsgCurrencyReport) {
            OnCurrencyReport(a_msg);
            return;
        }
        if (!a_msg || a_msg->type != GridInvAPI::kMsgHostReady) {
            return;
        }

        // MEASURE THE PAYLOAD BEFORE TRUSTING ANY OFFSET IN IT. This listener
        // hears every plugin in the load order, and another mod's message type
        // may collide with this 4CC by coincidence. structSize and abiVersion
        // both live inside the first eight bytes, so a short payload could
        // otherwise sail past the version check below.
        if (!a_msg->data || a_msg->dataLen < sizeof(GridInvAPI::HostReady)) {
            return;
        }
        const auto* ready = static_cast<const GridInvAPI::HostReady*>(a_msg->data);
        if (ready->abiVersion != GridInvAPI::kABIVersion ||
            ready->structSize != sizeof(GridInvAPI::HostReady)) {
            logger::warn("grid tint: host announced abi {} (we speak {}); not offering a table",
                ready->abiVersion, GridInvAPI::kABIVersion);
            return;
        }

        // Same rule for the table it points at: measured before it is kept.
        if (ready->services &&
            ready->services->structSize == sizeof(GridInvAPI::HostServices) &&
            ready->services->abiVersion == GridInvAPI::kABIVersion) {
            g_services.store(ready->services);
        }

        Offer();
    }
}

void GridTint::Install()
{
    // sender == nullptr: unfiltered, so the host's announcement actually lands.
    // Registered under a different sender key than the lifecycle listener in
    // main.cpp, which is what lets the two coexist.
    const bool ok = SKSE::GetMessagingInterface()->RegisterListener(nullptr, OnApiMessage);
    logger::info("grid tint: ABI listener registered: {}", ok ? "ok" : "FAILED");
}

bool GridTint::Registered()
{
    return g_registered.load();
}

void GridTint::RequestRebuild()
{
    // The host only sets a flag here, which is why any thread may ask.
    if (const auto* svc = g_services.load(); svc && svc->RequestRebuild) {
        svc->RequestRebuild();
    }
}

bool GridTint::HostServicesKnown()
{
    return g_services.load() != nullptr;
}

bool GridTint::LastTooltipItem(std::uint32_t& a_base, std::uint32_t& a_enchantment)
{
    a_base = g_hoverBase.load();
    a_enchantment = g_hoverEnch.load();
    return a_enchantment != 0;
}

void GridTint::ForgetTooltipItem()
{
    g_hoverBase.store(0);
    g_hoverEnch.store(0);
    std::scoped_lock lock{ g_sightLock };
    g_sightAsked.clear();
}

std::uint32_t GridTint::ColourFor(std::uint32_t a_base, const void* a_xl)
{
    return TierColour(GetTier(nullptr, a_base, a_xl));
}

std::uint8_t GridTint::CodeFor(const void* a_xl)
{
    return CodeOf(a_xl);
}

bool GridTint::GridReportSeen()
{
    return g_reportSeen.load();
}

bool GridTint::PouchActive()
{
    std::scoped_lock lock{ g_reportLock };
    return g_reportSeen.load() && g_report.pouchActive != 0;
}

bool GridTint::GridCurrencyRow(std::uint32_t a_base, CurrencyRow& a_out)
{
    std::scoped_lock lock{ g_reportLock };
    for (std::uint32_t i = 0; i < g_report.count; ++i) {
        if (g_report.rows[i].base == a_base) {
            a_out.w = g_report.rows[i].w;
            a_out.h = g_report.rows[i].h;
            a_out.iconSource = g_report.rows[i].iconSource;
            return true;
        }
    }
    return false;
}

bool GridTint::ApplierDelivered()
{
    return g_applierSent.load();
}

std::uint32_t GridTint::LinesFor(std::uint32_t a_base, const void* a_xl,
    GridInvAPI::TooltipLine* a_out, std::uint32_t a_capacity)
{
    t_noFirstSight = true;
    const auto n = GetLines(nullptr, a_base, a_xl, a_out, a_capacity);
    t_noFirstSight = false;
    return n;
}

std::uint32_t GridTint::PaletteFor(std::uint32_t* a_out, std::uint32_t a_capacity)
{
    return GetPalette(nullptr, a_out, a_capacity);
}
