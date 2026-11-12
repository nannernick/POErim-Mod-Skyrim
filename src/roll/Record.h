// =============================================================================
//  The item record -- what one item rolled, in a form that can be written down.
// =============================================================================
//  A RolledItem points INTO the affix table: it is only good for as long as the
//  table it was rolled from stays loaded, and it cannot be saved. This is the
//  same information as plain data -- affix ids, tiers, values and the item
//  level -- which is what the co-save stores per item and what a crafting orb
//  starts from when it changes one.
//
//  Pure, like everything else in this directory: no RE/, no SKSE/. The harness
//  round-trips records through the same code the plugin saves with.
//
//  THE AFFIX IS STORED BY ID, NOT BY INDEX. The table is a CSV the player is
//  invited to edit and add-on files merge into it, so a position in it means
//  nothing across two launches. An id that no longer resolves is reported by
//  ToRolled rather than guessed at.
// =============================================================================
#pragma once

#include "Roll.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace roll
{
    struct AffixRecord
    {
        std::string  id;           // Affix::id
        std::uint8_t tier{ 0 };    // AffixTier::tier, 1..3
        float        value{ 0.0f };
    };

    enum RecordFlags : std::uint8_t
    {
        kRecordNone = 0,
        // Rebuilt from the item's enchantment because the save predates
        // records. The affixes are what the item really has; the item level is
        // an estimate (see Records.cpp), because nothing kept the real one.
        kRecordLegacy = 1u << 0,
        // A crafting orb has changed this item at least once.
        kRecordCrafted = 1u << 1,
        // A Vaal Orb has been used on it. No orb works on it again. This is
        // the only place the fact is kept: MakeRecord sets it from
        // RolledItem::corrupted and ToRolled hands it back.
        kRecordCorrupted = 1u << 2,
        // 2.1.0: the base implicit was given to this item AFTER it was made --
        // a legacy item with none, or a vanilla enchantment replaced on first
        // sight. Set so the grant is never rolled again on a later load.
        kRecordImplicitGranted = 1u << 3,
    };

    // ★EXPLICITS AND IMPLICITS ARE TWO LISTS (co-save 'ITEM' version 2).
    // Version 1 kept one list in which a corrupted item's single implicit sat
    // beside its explicits; it loads into `affixes` as it was and
    // NormalizeRecord moves the implicit across once the table is there to
    // say which is which.
    struct ItemRecord
    {
        std::uint16_t            itemLevel{ 1 };
        std::uint8_t             flags{ kRecordNone };
        std::vector<AffixRecord> affixes;     // explicits (prefixes and suffixes)
        std::vector<AffixRecord> implicits;   // 0 to kMaxImplicits
    };

    // Moves any implicit-kind entry out of `affixes` into `implicits`. True
    // when anything moved. Unknown ids stay where they are.
    bool NormalizeRecord(ItemRecord& a_record, const AffixTable& a_table);

    [[nodiscard]] const Affix*     FindAffix(const AffixTable& a_table, std::string_view a_id);
    [[nodiscard]] const AffixTier* FindTier(const Affix& a_affix, int a_tier);

    [[nodiscard]] ItemRecord MakeRecord(const RolledItem& a_rolled, int a_itemLevel,
        std::uint8_t a_flags = kRecordNone);

    // ---- the co-save layout, as pure code ------------------------------------
    //
    // Persist streams these through SKSE's serialization interface; the
    // harness streams them through a byte buffer. One implementation, so the
    // round-trip test in the harness is the save's own code.
    //
    //   version 1 (1.1.0 - 1.2.x), per entry after the enchantment id:
    //     u16 itemLevel  u8 flags  u8 affixCount  affixes...
    //   version 2 (2.1.0):
    //     u16 itemLevel  u8 flags  u8 explicitCount  explicits...
    //                              u8 implicitCount  implicits...
    //   affix:  u8 idLength  idLength bytes  u8 tier  f32 value
    inline constexpr std::uint32_t kItemRecordV1 = 1;
    inline constexpr std::uint32_t kItemRecordV2 = 2;
    inline constexpr std::size_t   kMaxAffixIdLength = 96;

    namespace detail
    {
        template <class Read>
        bool ReadAffixList(Read& a_read, std::vector<AffixRecord>& a_out)
        {
            std::uint8_t count = 0;
            if (!a_read(&count, sizeof(count))) {
                return false;
            }
            a_out.reserve(a_out.size() + count);
            for (std::uint8_t k = 0; k < count; ++k) {
                std::uint8_t length = 0;
                AffixRecord  affix;
                if (!a_read(&length, sizeof(length)) || length > kMaxAffixIdLength) {
                    return false;
                }
                if (length > 0) {
                    affix.id.resize(length);
                    if (!a_read(affix.id.data(), length)) {
                        return false;
                    }
                }
                if (!a_read(&affix.tier, sizeof(affix.tier)) || !a_read(&affix.value, sizeof(affix.value))) {
                    return false;
                }
                a_out.push_back(std::move(affix));
            }
            return true;
        }

        template <class Write>
        bool WriteAffixList(Write& a_write, const std::vector<AffixRecord>& a_list)
        {
            const auto count = static_cast<std::uint8_t>(a_list.size() > 255 ? 255 : a_list.size());
            bool       ok = a_write(&count, sizeof(count));
            for (std::uint8_t i = 0; ok && i < count; ++i) {
                const auto& affix = a_list[i];
                const auto  length = static_cast<std::uint8_t>(
                    affix.id.size() > kMaxAffixIdLength ? kMaxAffixIdLength : affix.id.size());
                ok = a_write(&length, sizeof(length)) &&
                     (length == 0 || a_write(affix.id.data(), length)) &&
                     a_write(&affix.tier, sizeof(affix.tier)) && a_write(&affix.value, sizeof(affix.value));
            }
            return ok;
        }
    }

    // a_read(void* dst, size_t n) -> bool; a_write(const void* src, size_t n) -> bool.
    // The body of one entry, after its enchantment id. Version 1 reads into
    // `affixes` only (see NormalizeRecord).
    template <class Read>
    bool ReadItemRecord(Read&& a_read, std::uint32_t a_version, ItemRecord& a_out)
    {
        a_out = ItemRecord{};
        if (!a_read(&a_out.itemLevel, sizeof(a_out.itemLevel)) || !a_read(&a_out.flags, sizeof(a_out.flags))) {
            return false;
        }
        if (!detail::ReadAffixList(a_read, a_out.affixes)) {
            return false;
        }
        if (a_version >= kItemRecordV2) {
            return detail::ReadAffixList(a_read, a_out.implicits);
        }
        return true;
    }

    template <class Write>
    bool WriteItemRecordV2(Write&& a_write, const ItemRecord& a_record)
    {
        return a_write(&a_record.itemLevel, sizeof(a_record.itemLevel)) &&
               a_write(&a_record.flags, sizeof(a_record.flags)) &&
               detail::WriteAffixList(a_write, a_record.affixes) &&
               detail::WriteAffixList(a_write, a_record.implicits);
    }

    // The record as a RolledItem against the table as it is NOW. Returns false
    // when any affix or tier in the record is no longer in the table; a_out
    // then holds the ones that did resolve, which is enough to display but not
    // to craft on -- rebuilding from it would silently delete the rest.
    bool ToRolled(const ItemRecord& a_record, const AffixTable& a_table, RolledItem& a_out);

    // Which tier of an affix a magnitude was rolled from: the row whose value
    // range contains it. This is how an item rolled before records existed
    // gets one -- its enchantment still carries every magnitude. Duration and
    // area break a tie between rows whose ranges touch; a value that sits in
    // two rows only through the float slack belongs to the row that holds it
    // without slack (2.1.0 fix for touching Summermyst ranges). nullptr when no row
    // contains the value, which means the table has been edited since the item
    // rolled and the item cannot be described by it any more.
    [[nodiscard]] const AffixTier* MatchTier(const Affix& a_affix, float a_magnitude,
        std::uint32_t a_duration, std::uint32_t a_area);
}
