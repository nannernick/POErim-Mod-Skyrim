// =============================================================================
//  Item records. See Records.h.
// =============================================================================

#include "PCH.h"

#include "Records.h"

#include "Apply.h"
#include "Persist.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_set>

namespace
{
    std::atomic<const roll::AffixTable*> g_table{ nullptr };

    // Enchantments that were asked about and could not be read. Without this a
    // tooltip held over one would walk its effects against the whole table on
    // every frame it stays up, to reach the same "no" each time.
    std::mutex                     g_lock;
    std::unordered_set<RE::FormID> g_unreadable;
    Records::Stats                 g_stats;

    // The roller's own floor for a fifth affix. Reconstruction has no Tuning
    // in hand -- each roll site owns its own copy -- and the default is what
    // every one of them is constructed with.
    int FifthAffixFloor()
    {
        return roll::Tuning{}.fifthAffixMinLevel;
    }

    bool Reconstruct(const RE::EnchantmentItem* a_ench, const roll::AffixTable& a_table,
        roll::ItemRecord& a_out)
    {
        a_out = roll::ItemRecord{};
        a_out.flags = roll::kRecordLegacy;

        int minLevel = 1;
        for (const auto* effect : a_ench->effects) {
            if (!effect || !effect->baseEffect) {
                return false;
            }
            const float magnitude = effect->effectItem.magnitude;
            const auto  duration = static_cast<std::uint32_t>(effect->effectItem.duration);
            const auto  area = static_cast<std::uint32_t>(effect->effectItem.area);

            // ★SEVERAL AFFIXES CAN SHARE ONE EFFECT -- Poison Resist and Poison
            // Immunity are the same magic effect at different magnitudes -- so
            // the effect alone does not name the affix. The magnitude does: it
            // falls inside exactly one of their tier rows.
            const roll::Affix*     found = nullptr;
            const roll::AffixTier* foundTier = nullptr;
            // Explicit rows first: an item old enough to need this mostly
            // predates implicits. An implicit row is the fallback (a record
            // lost from a 2.1.0 save still reads back).
            for (const bool implicitPass : { false, true }) {
                for (const auto& affix : a_table.Affixes()) {
                    if (affix.Implicit() != implicitPass) {
                        continue;
                    }
                    if (Apply::EffectFor(affix.id) != effect->baseEffect) {
                        continue;
                    }
                    const bool taken = std::any_of(a_out.affixes.begin(), a_out.affixes.end(),
                        [&](const roll::AffixRecord& a_have) { return a_have.id == affix.id; });
                    if (taken) {
                        continue;
                    }
                    if (const auto* tier = roll::MatchTier(affix, magnitude, duration, area)) {
                        found = &affix;
                        foundTier = tier;
                        break;
                    }
                }
                if (found) {
                    break;
                }
            }
            if (!found) {
                return false;
            }
            // An item never carries the same affix twice; an enchantment that
            // appears to is not one of ours as rolled.
            const bool duplicate = std::any_of(a_out.affixes.begin(), a_out.affixes.end(),
                [&](const roll::AffixRecord& a_have) { return a_have.id == found->id; });
            if (duplicate) {
                return false;
            }

            roll::AffixRecord entry;
            entry.id = found->id;
            entry.tier = static_cast<std::uint8_t>(foundTier->tier);
            entry.value = magnitude;
            a_out.affixes.push_back(std::move(entry));
            minLevel = (std::max)(minLevel, foundTier->minItemLevel);
        }

        if (a_out.affixes.empty() ||
            a_out.affixes.size() > static_cast<std::size_t>(roll::kMaxAffixes + roll::kMaxImplicits)) {
            return false;
        }
        roll::NormalizeRecord(a_out, a_table);
        if (a_out.affixes.size() > static_cast<std::size_t>(roll::kMaxAffixes) ||
            a_out.implicits.size() > static_cast<std::size_t>(roll::kMaxImplicits)) {
            return false;
        }
        if (a_out.affixes.size() >= 5) {
            minLevel = (std::max)(minLevel, FifthAffixFloor());
        }
        if (a_out.affixes.size() >= 6) {
            minLevel = (std::max)(minLevel, roll::Tuning{}.sixthAffixMinLevel);
        }

        int level = minLevel;
        if (const auto* player = RE::PlayerCharacter::GetSingleton()) {
            level = (std::max)(level, static_cast<int>(player->GetLevel()));
        }
        a_out.itemLevel = static_cast<std::uint16_t>(std::clamp(level, 1, 65535));
        return true;
    }
}

void Records::SetTable(const roll::AffixTable& a_table)
{
    g_table.store(&a_table);
}

const roll::AffixTable* Records::Table()
{
    return g_table.load();
}

void Records::Note(RE::FormID a_enchantment, const roll::RolledItem& a_rolled, int a_itemLevel)
{
    if (a_enchantment == 0 || a_rolled.affixes.empty()) {
        return;
    }
    Persist::NoteEnchTier(a_enchantment, roll::RarityCode(a_rolled));
    Persist::NoteItemRecord(a_enchantment, roll::MakeRecord(a_rolled, a_itemLevel));
}

std::shared_mutex& Records::TableLock()
{
    static std::shared_mutex lock;
    return lock;
}

std::uint8_t Records::CodeFor(const RE::EnchantmentItem* a_enchantment)
{
    if (!a_enchantment) {
        return 0;
    }
    const auto id = a_enchantment->GetFormID();
    if (const auto code = Persist::EnchTier(id); code != 0) {
        return code;
    }
    // A miss on the hot path: only ours is worth the work, and only once.
    if (!Persist::IsAffixEnch(id)) {
        return 0;
    }
    ReadGuard guard{ TableLock() };
    View      view;
    if (!Lookup(a_enchantment, view)) {
        return 0;
    }
    const auto code = roll::RarityCode(view.rolled);
    Persist::NoteEnchTier(id, code);
    return code;
}

bool Records::Lookup(const RE::ExtraDataList* a_xList, View& a_out)
{
    if (!a_xList) {
        return false;
    }
    const auto* xEnch = a_xList->GetByType<RE::ExtraEnchantment>();
    return xEnch && Lookup(xEnch->enchantment, a_out);
}

bool Records::Lookup(const RE::EnchantmentItem* a_enchantment, View& a_out)
{
    const auto* table = g_table.load();
    if (!a_enchantment || !table) {
        return false;
    }
    const auto id = a_enchantment->GetFormID();

    a_out = View{};
    a_out.enchantment = id;
    a_out.code = Persist::EnchTier(id);

    if (!Persist::GetItemRecord(id, a_out.record)) {
        // Ours as rolled, or not ours to describe. The ownership set is what
        // tells an affix enchantment from one the player merged their own
        // effects into, which keeps its band and stops being a roll.
        if (!Persist::IsAffixEnch(id)) {
            return false;
        }
        {
            std::scoped_lock lock{ g_lock };
            if (g_unreadable.contains(id)) {
                return false;
            }
        }
        if (!Reconstruct(a_enchantment, *table, a_out.record)) {
            std::scoped_lock lock{ g_lock };
            g_unreadable.insert(id);
            ++g_stats.unreadable;
            logger::info("records: enchantment {:08X} predates item records and could not be "
                         "read back from the affix table; it is not craftable",
                id);
            return false;
        }
        Persist::NoteItemRecord(id, a_out.record);
        {
            std::scoped_lock lock{ g_lock };
            ++g_stats.reconstructed;
        }
        logger::info("records: rebuilt the record for enchantment {:08X} from its effects -- "
                     "{} affix(es), item level estimated at {}",
            id, a_out.record.affixes.size(), a_out.record.itemLevel);
    }

    // ★A 1.2.0 RECORD keeps a corrupted item's implicit in the explicit list;
    // it moves across once, here, and is written back so the save carries the
    // 2.1.0 layout from now on.
    if (roll::NormalizeRecord(a_out.record, *table)) {
        Persist::NoteItemRecord(id, a_out.record);
    }
    a_out.complete = roll::ToRolled(a_out.record, *table, a_out.rolled);
    return true;
}

void Records::Forget()
{
    std::scoped_lock lock{ g_lock };
    g_unreadable.clear();
}

Records::Stats Records::CurrentStats()
{
    std::scoped_lock lock{ g_lock };
    return g_stats;
}
