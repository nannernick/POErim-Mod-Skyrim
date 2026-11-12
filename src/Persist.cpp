// =============================================================================
//  Phase 5 -- the persistence spine. See Persist.h.
// =============================================================================

#include "PCH.h"

#include "Persist.h"

#include <algorithm>
#include <format>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // ★A four-character code, and it must be unique across every SKSE plugin the
    // user has installed. Collide with someone else's and both plugins read each
    // other's records as their own, which is a save-corrupting class of bug that
    // presents as "your mod broke my game" months later.
    constexpr std::uint32_t kUniqueID = 'DISK';

    // Record types inside our own namespace, so these only have to be unique
    // among ourselves.
    constexpr std::uint32_t kRolledActors = 'ARLL';

    // A SECOND RECORD TYPE, not a wider first one. An old build meeting a new
    // save skips this by its own "unknown record type" branch and carries on
    // with the actor set intact -- where widening kRolledActors would have made
    // the whole record unreadable and re-rolled the world.
    constexpr std::uint32_t kEnchTiers = 'ETIR';

    // A THIRD, for the same reason as the second: the set of created
    // enchantments that are ours. Old builds skip it and fall back to reading
    // the enchantment's shape, which still works for everything they rolled.
    constexpr std::uint32_t kAffixEnchs = 'AFFX';

    // The faction restock day a merchant chest was last rolled at. See the
    // header; one FormID and one day per entry.
    constexpr std::uint32_t kVendorDays = 'VDAY';

    // What each item rolled: affix ids, tiers, values and the item level, per
    // created enchantment. A FIFTH TYPE, and it carries a version of its own:
    // the four above are fixed-width entries and this one is not, so its
    // layout will change on a different schedule from theirs.
    //
    //   u32 count
    //   per entry:  u32 enchantment   u16 itemLevel   u8 flags   u8 affixCount
    //   per affix:  u8 idLength   idLength bytes   u8 tier   f32 value
    constexpr std::uint32_t kItemRecords = 'ITEM';
    constexpr std::uint32_t kItemRecordVersion = 1;

    // An affix id longer than this is not one this build wrote.
    constexpr std::size_t kMaxAffixIdLength = 96;

    // ★Bump this whenever the LAYOUT changes, never for content changes. The
    // loader refuses versions it does not know rather than guessing, so an old
    // build meeting a new save skips the record and re-rolls -- annoying, and
    // vastly better than misreading the bytes.
    constexpr std::uint32_t kVersion = 1;

    // Serialization callbacks run on the game's save/load thread; MarkRolled
    // runs from the task interface. Different threads, same container.
    std::mutex                       g_lock;
    std::unordered_set<RE::FormID>   g_rolled;

    // A LOCK OF ITS OWN, deliberately not g_lock. EnchTier is read once per
    // visible tile per frame on the render thread while the inventory is open;
    // MarkRolled is written from the task interface as actors load. Sharing one
    // mutex would put a UI redraw behind a distribution sweep for no reason --
    // the two sets are never touched together.
    std::mutex                                     g_tierLock;
    std::unordered_map<RE::FormID, std::uint8_t>   g_enchTier;

    // Under g_tierLock as well. It is written at exactly the moments the tier
    // map is -- attach, release, merge -- and read only when the enchanting
    // table opens, so it has no traffic of its own worth a lock of its own.
    std::unordered_set<RE::FormID>                 g_affixEnch;

    // Under g_tierLock as well: written once per barter, read once per barter.
    std::unordered_map<RE::FormID, std::uint32_t>  g_vendorDay;

    // Under g_tierLock as well: written where the band is written, read by a
    // tooltip or a craft -- never per tile per frame.
    std::unordered_map<RE::FormID, roll::ItemRecord> g_itemRecord;

    // ---- orb drops (Orbs.cpp) -------------------------------------------------
    // Actors whose death has already been turned into orbs, and the orb
    // references placed on the ground with the in-game day they were placed.
    // A lock of their own: written from the death drain, never near a tile.
    constexpr std::uint32_t kDroppedActors = 'ODRP';
    constexpr std::uint32_t kGroundOrbs = 'ORBR';
    std::mutex                                    g_dropLock;
    std::unordered_set<RE::FormID>                g_dropped;
    std::unordered_map<RE::FormID, std::uint32_t> g_groundOrbs;

    void SaveDrops(SKSE::SerializationInterface* a_intfc)
    {
        std::scoped_lock lock{ g_dropLock };
        if (a_intfc->OpenRecord(kDroppedActors, kVersion)) {
            const auto count = static_cast<std::uint32_t>(g_dropped.size());
            bool       ok = a_intfc->WriteRecordData(count);
            for (const auto formID : g_dropped) {
                ok = ok && a_intfc->WriteRecordData(formID);
            }
            if (!ok) {
                logger::error("save: the orb-drop record is short and will be rejected on load");
            }
        }
        if (a_intfc->OpenRecord(kGroundOrbs, kVersion)) {
            const auto count = static_cast<std::uint32_t>(g_groundOrbs.size());
            bool       ok = a_intfc->WriteRecordData(count);
            for (const auto& [formID, day] : g_groundOrbs) {
                ok = ok && a_intfc->WriteRecordData(formID) && a_intfc->WriteRecordData(day);
            }
            if (!ok) {
                logger::error("save: the ground-orb record is short and will be rejected on load");
            }
        }
    }

    void LoadDrops(SKSE::SerializationInterface* a_intfc, std::uint32_t a_type,
        std::uint32_t a_version)
    {
        if (a_version != kVersion) {
            logger::warn("load: orb record {:08X} is version {}, expected {}; skipped", a_type,
                a_version, kVersion);
            return;
        }
        std::uint32_t count = 0;
        if (!a_intfc->ReadRecordData(count)) {
            return;
        }
        std::scoped_lock lock{ g_dropLock };
        for (std::uint32_t i = 0; i < count; ++i) {
            RE::FormID    oldID = 0;
            std::uint32_t day = 0;
            if (!a_intfc->ReadRecordData(oldID) ||
                (a_type == kGroundOrbs && !a_intfc->ReadRecordData(day))) {
                logger::error("load: orb record {:08X} is short; kept what was read", a_type);
                return;
            }
            RE::FormID newID = 0;
            if (!a_intfc->ResolveFormID(oldID, newID)) {
                continue;
            }
            if (a_type == kDroppedActors) {
                g_dropped.insert(newID);
            } else {
                g_groundOrbs[newID] = day;
            }
        }
    }

    void SaveItemRecords(SKSE::SerializationInterface* a_intfc)
    {
        std::scoped_lock lock{ g_tierLock };

        if (!a_intfc->OpenRecord(kItemRecords, kItemRecordVersion)) {
            logger::error("save: could not open the item-record record; items in this save "
                          "will have their affix lists rebuilt from their enchantments");
            return;
        }

        const auto count = static_cast<std::uint32_t>(g_itemRecord.size());
        if (!a_intfc->WriteRecordData(count)) {
            logger::error("save: failed writing the item-record count");
            return;
        }

        for (const auto& [formID, record] : g_itemRecord) {
            const auto affixCount = static_cast<std::uint8_t>(
                (std::min)(record.affixes.size(), static_cast<std::size_t>(255)));
            bool ok = a_intfc->WriteRecordData(formID) &&
                      a_intfc->WriteRecordData(record.itemLevel) &&
                      a_intfc->WriteRecordData(record.flags) &&
                      a_intfc->WriteRecordData(affixCount);
            for (std::uint8_t i = 0; ok && i < affixCount; ++i) {
                const auto& affix = record.affixes[i];
                const auto  length = static_cast<std::uint8_t>(
                    (std::min)(affix.id.size(), kMaxAffixIdLength));
                ok = a_intfc->WriteRecordData(length) &&
                     (length == 0 || a_intfc->WriteRecordData(affix.id.data(), length)) &&
                     a_intfc->WriteRecordData(affix.tier) &&
                     a_intfc->WriteRecordData(affix.value);
            }
            if (!ok) {
                logger::error("save: failed writing an item record; the record is now short "
                              "and the rest will be rebuilt from enchantments on load");
                return;
            }
        }

        logger::info("save: wrote {} item record(s)", count);
    }

    void LoadItemRecords(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version)
    {
        if (a_version != kItemRecordVersion) {
            logger::warn("load: item-record record is version {}, this build understands {}. "
                         "Skipping it -- affix lists will be rebuilt from enchantments.",
                a_version, kItemRecordVersion);
            return;
        }

        std::uint32_t count = 0;
        if (!a_intfc->ReadRecordData(count)) {
            logger::error("load: could not read the item-record count");
            return;
        }

        std::size_t restored = 0;
        std::size_t dropped = 0;

        std::scoped_lock lock{ g_tierLock };
        for (std::uint32_t i = 0; i < count; ++i) {
            RE::FormID       oldID = 0;
            roll::ItemRecord record;
            std::uint8_t     affixCount = 0;
            if (!a_intfc->ReadRecordData(oldID) || !a_intfc->ReadRecordData(record.itemLevel) ||
                !a_intfc->ReadRecordData(record.flags) || !a_intfc->ReadRecordData(affixCount)) {
                logger::error("load: item-record record ended after {} of {} entries", i, count);
                break;
            }

            // ★THE WHOLE ENTRY IS READ BEFORE THE ID IS RESOLVED. The entries
            // are variable-width, so one that is going to be dropped still has
            // to be walked to find where the next one starts.
            bool ok = true;
            record.affixes.reserve(affixCount);
            for (std::uint8_t k = 0; ok && k < affixCount; ++k) {
                std::uint8_t      length = 0;
                roll::AffixRecord affix;
                ok = a_intfc->ReadRecordData(length) && length <= kMaxAffixIdLength;
                if (ok && length > 0) {
                    affix.id.resize(length);
                    ok = a_intfc->ReadRecordData(affix.id.data(), length) == length;
                }
                ok = ok && a_intfc->ReadRecordData(affix.tier) &&
                     a_intfc->ReadRecordData(affix.value);
                if (ok) {
                    record.affixes.push_back(std::move(affix));
                }
            }
            if (!ok) {
                // Not recoverable past this point: the next entry's start is
                // unknown. What was read so far is kept; the rest rebuild.
                logger::error("load: item record {} of {} is malformed; stopping there", i, count);
                break;
            }

            RE::FormID newID = 0;
            if (!a_intfc->ResolveFormID(oldID, newID)) {
                ++dropped;
                continue;
            }
            g_itemRecord[newID] = std::move(record);
            ++restored;
        }

        logger::info("load: restored {} item record(s){}", restored,
            dropped ? std::format(", dropped {} that no longer resolve", dropped) : "");
    }

    void SaveRolledActors(SKSE::SerializationInterface* a_intfc)
    {
        std::scoped_lock lock{ g_lock };

        if (!a_intfc->OpenRecord(kRolledActors, kVersion)) {
            logger::error("save: could not open the rolled-actor record; this save will "
                          "re-roll every actor when loaded");
            return;
        }

        const auto count = static_cast<std::uint32_t>(g_rolled.size());
        if (!a_intfc->WriteRecordData(count)) {
            logger::error("save: failed writing the rolled-actor count");
            return;
        }

        for (const auto formID : g_rolled) {
            if (!a_intfc->WriteRecordData(formID)) {
                logger::error("save: failed writing a rolled-actor id; the record is now "
                              "short and will be rejected on load");
                return;
            }
        }

        logger::info("save: wrote {} rolled actor(s)", count);
    }

    void SaveEnchTiers(SKSE::SerializationInterface* a_intfc)
    {
        std::scoped_lock lock{ g_tierLock };

        if (!a_intfc->OpenRecord(kEnchTiers, kVersion)) {
            logger::error("save: could not open the enchantment-tier record; affixed items "
                          "in this save will draw uncoloured");
            return;
        }

        const auto count = static_cast<std::uint32_t>(g_enchTier.size());
        if (!a_intfc->WriteRecordData(count)) {
            logger::error("save: failed writing the enchantment-tier count");
            return;
        }

        for (const auto& [formID, band] : g_enchTier) {
            if (!a_intfc->WriteRecordData(formID) || !a_intfc->WriteRecordData(band)) {
                logger::error("save: failed writing an enchantment tier; the record is now "
                              "short and will be rejected on load");
                return;
            }
        }

        logger::info("save: wrote {} enchantment tier(s)", count);
    }

    void SaveAffixEnchs(SKSE::SerializationInterface* a_intfc)
    {
        std::scoped_lock lock{ g_tierLock };

        if (!a_intfc->OpenRecord(kAffixEnchs, kVersion)) {
            logger::error("save: could not open the affix-enchantment record; finite-charge "
                          "weapons in this save will not be enchantable when loaded");
            return;
        }

        const auto count = static_cast<std::uint32_t>(g_affixEnch.size());
        if (!a_intfc->WriteRecordData(count)) {
            logger::error("save: failed writing the affix-enchantment count");
            return;
        }

        for (const auto formID : g_affixEnch) {
            if (!a_intfc->WriteRecordData(formID)) {
                logger::error("save: failed writing an affix-enchantment id; the record is now "
                              "short and will be rejected on load");
                return;
            }
        }

        logger::info("save: wrote {} affix enchantment(s)", count);
    }

    void SaveVendorDays(SKSE::SerializationInterface* a_intfc)
    {
        std::scoped_lock lock{ g_tierLock };

        if (!a_intfc->OpenRecord(kVendorDays, kVersion)) {
            logger::error("save: could not open the vendor-day record; every vendor chest "
                          "will re-roll on the next barter after loading");
            return;
        }

        const auto count = static_cast<std::uint32_t>(g_vendorDay.size());
        if (!a_intfc->WriteRecordData(count)) {
            logger::error("save: failed writing the vendor-day count");
            return;
        }

        for (const auto& [formID, day] : g_vendorDay) {
            if (!a_intfc->WriteRecordData(formID) || !a_intfc->WriteRecordData(day)) {
                logger::error("save: failed writing a vendor day; the record is now short "
                              "and will be rejected on load");
                return;
            }
        }

        logger::info("save: wrote {} vendor day(s)", count);
    }

    void LoadVendorDays(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version)
    {
        if (a_version != kVersion) {
            logger::warn("load: vendor-day record is version {}, this build understands {}. "
                         "Skipping it -- vendor chests will re-roll on the next barter.",
                a_version, kVersion);
            return;
        }

        std::uint32_t count = 0;
        if (!a_intfc->ReadRecordData(count)) {
            logger::error("load: could not read the vendor-day count");
            return;
        }

        std::size_t restored = 0;
        std::size_t dropped = 0;

        std::scoped_lock lock{ g_tierLock };
        for (std::uint32_t i = 0; i < count; ++i) {
            RE::FormID    oldID = 0;
            std::uint32_t day = 0;
            if (!a_intfc->ReadRecordData(oldID) || !a_intfc->ReadRecordData(day)) {
                logger::error("load: vendor-day record ended after {} of {} entries", i, count);
                break;
            }

            RE::FormID newID = 0;
            if (!a_intfc->ResolveFormID(oldID, newID)) {
                ++dropped;
                continue;
            }
            g_vendorDay[newID] = day;
            ++restored;
        }

        logger::info("load: restored {} vendor day(s){}", restored,
            dropped ? std::format(", dropped {} that no longer resolve", dropped) : "");
    }

    void SaveCallback(SKSE::SerializationInterface* a_intfc)
    {
        // One record each, written in turn. Kept as separate calls rather than
        // one body so a failure part-way through one set cannot also cost the
        // others -- each record's early returns end only its own record.
        SaveRolledActors(a_intfc);
        SaveEnchTiers(a_intfc);
        SaveAffixEnchs(a_intfc);
        SaveVendorDays(a_intfc);
        SaveItemRecords(a_intfc);
        SaveDrops(a_intfc);
    }

    // One kEnchTiers record. Split out so the dispatch in LoadCallback stays a
    // list of record types rather than two interleaved parsers.
    void LoadEnchTiers(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version)
    {
        if (a_version != kVersion) {
            logger::warn("load: enchantment-tier record is version {}, this build understands {}. "
                         "Skipping it -- affixed items in this save will draw uncoloured.",
                a_version, kVersion);
            return;
        }

        std::uint32_t count = 0;
        if (!a_intfc->ReadRecordData(count)) {
            logger::error("load: could not read the enchantment-tier count");
            return;
        }

        std::size_t restored = 0;
        std::size_t dropped = 0;

        std::scoped_lock lock{ g_tierLock };
        for (std::uint32_t i = 0; i < count; ++i) {
            RE::FormID   oldID = 0;
            std::uint8_t band = 0;
            if (!a_intfc->ReadRecordData(oldID) || !a_intfc->ReadRecordData(band)) {
                logger::error("load: enchantment-tier record ended after {} of {} entries",
                    i, count);
                break;
            }

            // THE SAME REMAP THE ACTOR SET GETS, and it matters more here, not
            // less. These are DYNAMIC forms -- created enchantments living in
            // the save's own created-object block -- and the engine renumbers
            // that block as objects come and go. ResolveFormID is what follows
            // one across the reload; a raw id would name a different
            // enchantment, and colour the wrong sword.
            RE::FormID newID = 0;
            if (!a_intfc->ResolveFormID(oldID, newID)) {
                // The enchantment did not survive: its item was destroyed, or
                // a plugin it depended on is gone. Nothing left to colour.
                ++dropped;
                continue;
            }
            if (band != 0) {
                g_enchTier[newID] = band;
                ++restored;
            }
        }

        logger::info("load: restored {} enchantment tier(s){}", restored,
            dropped ? std::format(", dropped {} that no longer resolve", dropped) : "");
    }

    // One kAffixEnchs record. The same shape as the tier loader minus the band.
    void LoadAffixEnchs(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version)
    {
        if (a_version != kVersion) {
            logger::warn("load: affix-enchantment record is version {}, this build understands {}. "
                         "Skipping it -- finite-charge weapons in this save will not be "
                         "enchantable.",
                a_version, kVersion);
            return;
        }

        std::uint32_t count = 0;
        if (!a_intfc->ReadRecordData(count)) {
            logger::error("load: could not read the affix-enchantment count");
            return;
        }

        std::size_t restored = 0;
        std::size_t dropped = 0;

        std::scoped_lock lock{ g_tierLock };
        for (std::uint32_t i = 0; i < count; ++i) {
            RE::FormID oldID = 0;
            if (!a_intfc->ReadRecordData(oldID)) {
                logger::error("load: affix-enchantment record ended after {} of {} entries",
                    i, count);
                break;
            }

            // Dynamic forms, remapped for the same reason the tier map is: the
            // created-object block is renumbered as objects come and go, and a
            // raw id would call some other enchantment ours.
            RE::FormID newID = 0;
            if (!a_intfc->ResolveFormID(oldID, newID)) {
                ++dropped;
                continue;
            }
            g_affixEnch.insert(newID);
            ++restored;
        }

        logger::info("load: restored {} affix enchantment(s){}", restored,
            dropped ? std::format(", dropped {} that no longer resolve", dropped) : "");
    }

    void LoadCallback(SKSE::SerializationInterface* a_intfc)
    {
        std::uint32_t type = 0;
        std::uint32_t version = 0;
        std::uint32_t length = 0;

        std::size_t restored = 0;
        std::size_t dropped = 0;

        while (a_intfc->GetNextRecordInfo(type, version, length)) {
            if (type == kEnchTiers) {
                LoadEnchTiers(a_intfc, version);
                continue;
            }
            if (type == kAffixEnchs) {
                LoadAffixEnchs(a_intfc, version);
                continue;
            }
            if (type == kVendorDays) {
                LoadVendorDays(a_intfc, version);
                continue;
            }
            if (type == kItemRecords) {
                LoadItemRecords(a_intfc, version);
                continue;
            }
            if (type == kDroppedActors || type == kGroundOrbs) {
                LoadDrops(a_intfc, type, version);
                continue;
            }
            if (type != kRolledActors) {
                logger::warn("load: skipping unknown record type {:08X}", type);
                continue;
            }
            if (version != kVersion) {
                // Loud, and deliberately not a guess. Re-rolling is recoverable;
                // misreading a different layout is not.
                logger::warn("load: rolled-actor record is version {}, this build understands {}. "
                             "Skipping it -- actors in this save will be re-rolled.",
                    version, kVersion);
                continue;
            }

            std::uint32_t count = 0;
            if (!a_intfc->ReadRecordData(count)) {
                logger::error("load: could not read the rolled-actor count");
                continue;
            }

            std::scoped_lock lock{ g_lock };
            for (std::uint32_t i = 0; i < count; ++i) {
                RE::FormID oldID = 0;
                if (!a_intfc->ReadRecordData(oldID)) {
                    logger::error("load: record ended after {} of {} ids", i, count);
                    break;
                }

                // ★THE LOAD-ORDER REMAP, and the whole reason this is not a raw
                // set of integers. A FormID's high byte is the plugin index at
                // the time of saving; the user may have added, removed or
                // reordered mods since. ResolveFormID rewrites it for the
                // current order and fails for forms whose plugin is simply gone.
                RE::FormID newID = 0;
                if (!a_intfc->ResolveFormID(oldID, newID)) {
                    // The actor's plugin was removed. Forgetting it is correct:
                    // there is nothing left to re-roll.
                    ++dropped;
                    continue;
                }
                g_rolled.insert(newID);
                ++restored;
            }
        }

        logger::info("load: restored {} rolled actor(s){}", restored,
            dropped ? std::format(", dropped {} whose plugin is gone", dropped) : "");
    }

    void RevertCallback(SKSE::SerializationInterface*)
    {
        std::size_t hadTiers = 0;
        std::size_t hadAffix = 0;
        {
            // The tier map is save-scoped exactly as the actor set is. Save A's
            // created enchantments do not exist in save B, and their FormIDs
            // are reused by B's own created objects -- so keeping the map would
            // not merely be stale, it would colour unrelated items. The affix
            // set is keyed the same way and would misbehave the same way: it
            // would let the table strip an enchantment that is not ours.
            std::scoped_lock tierLock{ g_tierLock };
            hadTiers = g_enchTier.size();
            hadAffix = g_affixEnch.size();
            g_enchTier.clear();
            g_affixEnch.clear();
            g_vendorDay.clear();
            g_itemRecord.clear();
        }
        {
            std::scoped_lock dropLock{ g_dropLock };
            g_dropped.clear();
            g_groundOrbs.clear();
        }

        std::scoped_lock lock{ g_lock };
        const auto       had = g_rolled.size();
        g_rolled.clear();

        // ★THIS IS NOT HOUSEKEEPING. Revert fires before a load replaces the
        // world. Skipping it would leave save A's rolled set in memory while
        // save B loads, and every actor A had already rolled would be silently
        // skipped in B -- a bug that only appears when someone loads two saves
        // in one session, which is to say, constantly.
        logger::info("revert: cleared {} rolled actor(s), {} enchantment tier(s) and {} affix "
                     "enchantment(s)",
            had, hadTiers, hadAffix);
    }
}

void Persist::Install()
{
    auto* intfc = SKSE::GetSerializationInterface();
    if (!intfc) {
        logger::error("no serialization interface; rolled actors will NOT persist and every "
                      "cell reload will re-roll");
        return;
    }

    intfc->SetUniqueID(kUniqueID);
    intfc->SetSaveCallback(SaveCallback);
    intfc->SetLoadCallback(LoadCallback);
    intfc->SetRevertCallback(RevertCallback);

    logger::info("serialization registered (id '{}{}{}{}', record version {})",
        static_cast<char>((kUniqueID >> 24) & 0xFF), static_cast<char>((kUniqueID >> 16) & 0xFF),
        static_cast<char>((kUniqueID >> 8) & 0xFF), static_cast<char>(kUniqueID & 0xFF),
        kVersion);
}

bool Persist::MarkRolled(RE::FormID a_actor)
{
    if (a_actor == 0) {
        return false;
    }
    std::scoped_lock lock{ g_lock };
    return g_rolled.insert(a_actor).second;
}

bool Persist::WasRolled(RE::FormID a_actor)
{
    std::scoped_lock lock{ g_lock };
    return g_rolled.contains(a_actor);
}

bool Persist::ForgetRolled(RE::FormID a_actor)
{
    if (a_actor == 0) {
        return false;
    }
    std::scoped_lock lock{ g_lock };
    return g_rolled.erase(a_actor) > 0;
}

std::size_t Persist::RolledCount()
{
    std::scoped_lock lock{ g_lock };
    return g_rolled.size();
}

void Persist::Clear()
{
    std::scoped_lock lock{ g_lock };
    g_rolled.clear();
    std::scoped_lock tierLock{ g_tierLock };
    g_enchTier.clear();
    g_affixEnch.clear();
    g_vendorDay.clear();
    g_itemRecord.clear();
}

void Persist::NoteEnchTier(RE::FormID a_enchantment, std::uint8_t a_band)
{
    // Band 0 is "no opinion" and is the answer for anything absent, so storing
    // it would cost a hash entry to say what an empty map already says.
    if (a_enchantment == 0 || a_band == 0) {
        return;
    }
    std::scoped_lock lock{ g_tierLock };
    g_enchTier[a_enchantment] = a_band;
}

std::uint8_t Persist::EnchTier(RE::FormID a_enchantment)
{
    if (a_enchantment == 0) {
        return 0;
    }
    std::scoped_lock lock{ g_tierLock };
    const auto       it = g_enchTier.find(a_enchantment);
    return it == g_enchTier.end() ? std::uint8_t{ 0 } : it->second;
}

void Persist::ForgetEnchTier(RE::FormID a_enchantment)
{
    std::scoped_lock lock{ g_tierLock };
    g_enchTier.erase(a_enchantment);
}

std::size_t Persist::EnchTierCount()
{
    std::scoped_lock lock{ g_tierLock };
    return g_enchTier.size();
}

void Persist::NoteAffixEnch(RE::FormID a_enchantment)
{
    if (a_enchantment == 0) {
        return;
    }
    std::scoped_lock lock{ g_tierLock };
    g_affixEnch.insert(a_enchantment);
}

bool Persist::IsAffixEnch(RE::FormID a_enchantment)
{
    if (a_enchantment == 0) {
        return false;
    }
    std::scoped_lock lock{ g_tierLock };
    return g_affixEnch.contains(a_enchantment);
}

void Persist::ForgetAffixEnch(RE::FormID a_enchantment)
{
    std::scoped_lock lock{ g_tierLock };
    g_affixEnch.erase(a_enchantment);
}

void Persist::NoteVendorDay(RE::FormID a_chest, std::uint32_t a_day)
{
    if (a_chest == 0) {
        return;
    }
    std::scoped_lock lock{ g_tierLock };
    g_vendorDay[a_chest] = a_day;
}

std::uint32_t Persist::VendorDay(RE::FormID a_chest)
{
    std::scoped_lock lock{ g_tierLock };
    const auto       it = g_vendorDay.find(a_chest);
    return it == g_vendorDay.end() ? 0u : it->second;
}

std::size_t Persist::VendorDayCount()
{
    std::scoped_lock lock{ g_tierLock };
    return g_vendorDay.size();
}

std::size_t Persist::AffixEnchCount()
{
    std::scoped_lock lock{ g_tierLock };
    return g_affixEnch.size();
}

void Persist::NoteItemRecord(RE::FormID a_enchantment, const roll::ItemRecord& a_record)
{
    if (a_enchantment == 0 || a_record.affixes.empty()) {
        return;
    }
    std::scoped_lock lock{ g_tierLock };
    const auto       it = g_itemRecord.find(a_enchantment);
    if (it == g_itemRecord.end()) {
        g_itemRecord.emplace(a_enchantment, a_record);
        return;
    }
    // A shared enchantment (see the header): the affixes are necessarily the
    // same, so only the level and the flags can have anything new to say.
    const auto level = (std::max)(it->second.itemLevel, a_record.itemLevel);
    it->second = a_record;
    it->second.itemLevel = level;
}

bool Persist::GetItemRecord(RE::FormID a_enchantment, roll::ItemRecord& a_out)
{
    if (a_enchantment == 0) {
        return false;
    }
    std::scoped_lock lock{ g_tierLock };
    const auto       it = g_itemRecord.find(a_enchantment);
    if (it == g_itemRecord.end()) {
        return false;
    }
    a_out = it->second;
    return true;
}

void Persist::ForgetItemRecord(RE::FormID a_enchantment)
{
    std::scoped_lock lock{ g_tierLock };
    g_itemRecord.erase(a_enchantment);
}

std::size_t Persist::ItemRecordCount()
{
    std::scoped_lock lock{ g_tierLock };
    return g_itemRecord.size();
}

std::vector<RE::FormID> Persist::KnownEnchantments()
{
    std::unordered_set<RE::FormID> all;
    {
        std::scoped_lock lock{ g_tierLock };
        for (const auto& [formID, band] : g_enchTier) {
            all.insert(formID);
        }
        all.insert(g_affixEnch.begin(), g_affixEnch.end());
        for (const auto& [formID, record] : g_itemRecord) {
            all.insert(formID);
        }
    }
    return { all.begin(), all.end() };
}

// ---- orb drops ---------------------------------------------------------------

bool Persist::MarkDropped(RE::FormID a_actor)
{
    if (a_actor == 0) {
        return false;
    }
    std::scoped_lock lock{ g_dropLock };
    return g_dropped.insert(a_actor).second;
}

bool Persist::WasDropped(RE::FormID a_actor)
{
    std::scoped_lock lock{ g_dropLock };
    return g_dropped.contains(a_actor);
}

bool Persist::ForgetDropped(RE::FormID a_actor)
{
    std::scoped_lock lock{ g_dropLock };
    return g_dropped.erase(a_actor) > 0;
}

std::size_t Persist::DroppedCount()
{
    std::scoped_lock lock{ g_dropLock };
    return g_dropped.size();
}

void Persist::NoteGroundOrb(RE::FormID a_ref, std::uint32_t a_day)
{
    if (a_ref == 0) {
        return;
    }
    std::scoped_lock lock{ g_dropLock };
    g_groundOrbs[a_ref] = a_day;
}

std::vector<std::pair<RE::FormID, std::uint32_t>> Persist::GroundOrbs()
{
    std::scoped_lock lock{ g_dropLock };
    return { g_groundOrbs.begin(), g_groundOrbs.end() };
}

void Persist::ForgetGroundOrb(RE::FormID a_ref)
{
    std::scoped_lock lock{ g_dropLock };
    g_groundOrbs.erase(a_ref);
}

std::size_t Persist::GroundOrbCount()
{
    std::scoped_lock lock{ g_dropLock };
    return g_groundOrbs.size();
}
