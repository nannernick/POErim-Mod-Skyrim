#pragma once
// =============================================================================
//  DiabloLootAPI -- what DiabloInSkyrim tells other plugins about loot. v1 + v2
// =============================================================================
//  For a camera / loot-label mod (ARPG POV) that wants to colour labels by
//  band, mark currency, and show an item's affixes on hover without knowing
//  anything about this mod's data.
//
//  ★SAME DISCIPLINE AS GridInventoryAPI.h: <cstdint> only, POD only, every
//  struct size asserted, strings in caller-owned buffers, pointers borrowed for
//  the call. A released struct never grows: bump kVersion instead.
//
//  HANDSHAKE: at kPostLoad DiabloInSkyrim BROADCASTS
//
//      type kMsgHello ('DLHR'), data const Interface*, valid for the process
//
//  and, since POErim 2.1.0, ALSO
//
//      type kMsgHelloV2 ('DLH2'), data const InterfaceV2*, valid for the process
//
//  Register an UNFILTERED listener (RegisterListener(nullptr, cb)) in
//  SKSEPluginLoad and act only on these types. Check structSize and version.
//  ★NO ABI BREAK: v1 is unchanged and still sent. A consumer that only knows
//  v1 keeps working; one that wants rarity takes v2 and, if v2 never arrives
//  (an older DiabloInSkyrim), treats every item as kRarityUnknown and falls
//  back to whatever it did before.
//
//  THREADING: every call reads form data and DiabloInSkyrim's own tables under
//  their locks. Safe from the game thread and from a render thread; it never
//  opens a menu or changes the world. refFormID calls look the reference up
//  by FormID, so pass a reference that exists now.
//
//  Vendor this file verbatim. Do not edit the copy.
// =============================================================================

#include <cstdint>

namespace DiabloLootAPI
{
    inline constexpr std::uint32_t kVersion = 1;
    inline constexpr std::uint32_t kMsgHello = 0x444C4852;   // 'DLHR'

    inline constexpr std::uint32_t kLineLen = 128;   // bytes incl. NUL
    inline constexpr std::uint32_t kMaxLines = 16;

    // v1 "bands", kept for v1 consumers. Since 2.1.0 they carry the rarity:
    // 0 normal / not ours, 1 magic, 2 rare, 5 corrupted. Colours are packed
    // 0xAABBGGRR (ImU32 order).

    struct Line
    {
        char          text[kLineLen];   // UTF-8, NUL-terminated
        std::uint32_t rgba;             // 0 = your default colour
        std::uint8_t  indent;           // 0..3
        std::uint8_t  separatorBefore;  // non-zero = a rule above this line
        std::uint8_t  _pad[2];
    };
    static_assert(sizeof(Line) == 136, "Line is part of the ABI");

    struct Interface
    {
        std::uint32_t structSize;   // = sizeof(Interface)
        std::uint32_t version;      // = kVersion

        // The band of a reference in the world (a dropped item), or of one
        // inventory sub-stack's RE::ExtraDataList*.
        std::uint8_t (*BandOfRef)(std::uint32_t refFormID);
        std::uint8_t (*BandOfList)(const void* xl);

        // The band colours: out[0] for blue ... out[4] for red. Returns count.
        std::uint32_t (*GetPalette)(std::uint32_t* out, std::uint32_t capacity);

        // Currency: whether a base form is one of the orbs, and its colour
        // (0 when it is not).
        bool (*IsCurrency)(std::uint32_t base);
        std::uint32_t (*CurrencyColor)(std::uint32_t base);

        // The reference's display name without the band marker glyphs.
        // Writes at most `capacity` bytes incl. NUL; returns the length.
        std::uint32_t (*CleanName)(std::uint32_t refFormID, char* out, std::uint32_t capacity);

        // The hover lines for a reference in the world: item level, corrupted,
        // affix lines in band colour -- the same lines Grid Inventory shows.
        // With Alt held (the INI's TooltipDetailKey) each affix gains its tier
        // and roll range. Returns how many were written.
        std::uint32_t (*TooltipLines)(std::uint32_t refFormID, Line* out, std::uint32_t capacity);
    };
    static_assert(sizeof(Interface) == 64, "Interface is part of the ABI");

    // ---- v2 (POErim 2.1.0): rarity and colour --------------------------------

    inline constexpr std::uint32_t kVersion2 = 2;
    inline constexpr std::uint32_t kMsgHelloV2 = 0x444C4832;   // 'DLH2'

    enum Rarity : std::uint8_t
    {
        kRarityUnknown = 0,   // not ours, not loaded, or nothing to say: use your fallback
        kRarityNormal = 1,
        kRarityMagic = 2,
        kRarityRare = 3,
        kRarityUnique = 4,    // reserved: uniques are not rolled; never returned today
        kRarityCurrency = 5,  // an orb
    };

    struct ItemInfo
    {
        std::uint32_t structSize;   // = sizeof(ItemInfo), set by the CALLER
        std::uint8_t  rarity;       // Rarity
        std::uint8_t  corrupted;    // non-zero: draw it red (rgba already is)
        std::uint8_t  currency;     // non-zero: a crafting orb -- show it as loot
        std::uint8_t  explicitMods; // 0..6
        std::uint32_t rgba;         // the colour to draw it in; 0 with kRarityUnknown
        std::uint32_t _reserved;
    };
    static_assert(sizeof(ItemInfo) == 16, "ItemInfo is part of the ABI");

    struct InterfaceV2
    {
        std::uint32_t structSize;   // = sizeof(InterfaceV2)
        std::uint32_t version;      // = kVersion2

        // A reference in the world (a dropped item, a corpse's or chest's
        // ground item), or one inventory sub-stack (base form + its
        // RE::ExtraDataList*, which may be null). Return false and fill
        // kRarityUnknown when there is nothing to say. `out->structSize`
        // must be sizeof(ItemInfo) or the call refuses.
        bool (*InfoOfRef)(std::uint32_t refFormID, ItemInfo* out);
        bool (*InfoOfItem)(std::uint32_t base, const void* xl, ItemInfo* out);
    };
    static_assert(sizeof(InterfaceV2) == 24, "InterfaceV2 is part of the ABI");
}
