#pragma once
// =============================================================================
//  DiabloLootAPI -- what DiabloInSkyrim tells other plugins about loot. v1
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
//  Register an UNFILTERED listener (RegisterListener(nullptr, cb)) in
//  SKSEPluginLoad and act only on this type. Check structSize and version.
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

    // Bands: 0 white / not ours, 1 blue, 2 yellow, 3 purple, 4 orange, 5 red.
    // Colours are packed 0xAABBGGRR (ImU32 order).

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
}
