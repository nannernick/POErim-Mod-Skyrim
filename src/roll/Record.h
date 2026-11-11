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
    };

    struct ItemRecord
    {
        std::uint16_t            itemLevel{ 1 };
        std::uint8_t             flags{ kRecordNone };
        std::vector<AffixRecord> affixes;
    };

    [[nodiscard]] const Affix*     FindAffix(const AffixTable& a_table, std::string_view a_id);
    [[nodiscard]] const AffixTier* FindTier(const Affix& a_affix, int a_tier);

    [[nodiscard]] ItemRecord MakeRecord(const RolledItem& a_rolled, int a_itemLevel,
        std::uint8_t a_flags = kRecordNone);

    // The record as a RolledItem against the table as it is NOW. Returns false
    // when any affix or tier in the record is no longer in the table; a_out
    // then holds the ones that did resolve, which is enough to display but not
    // to craft on -- rebuilding from it would silently delete the rest.
    bool ToRolled(const ItemRecord& a_record, const AffixTable& a_table, RolledItem& a_out);

    // Which tier of an affix a magnitude was rolled from: the row whose value
    // range contains it. This is how an item rolled before records existed
    // gets one -- its enchantment still carries every magnitude. Duration and
    // area break a tie between rows whose ranges touch. nullptr when no row
    // contains the value, which means the table has been edited since the item
    // rolled and the item cannot be described by it any more.
    [[nodiscard]] const AffixTier* MatchTier(const Affix& a_affix, float a_magnitude,
        std::uint32_t a_duration, std::uint32_t a_area);
}
