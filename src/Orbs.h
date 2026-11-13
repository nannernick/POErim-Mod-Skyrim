// =============================================================================
//  Orbs -- the currency items, what kills drop, and using one on an item.
// =============================================================================
//  The orbs are MISC records in DiabloInSkyrim.esp (tools/esp/build_esp.py):
//  0x800 is the DIS_CurrencyOrb keyword, 0x801 + roll::Orb is each orb. Without
//  the plugin nothing here does anything and one error goes to the log.
//
//  ★ARPG POV. Its loot filter shows a MISC item when it carries the vanilla
//  VendorItemGem keyword (gold label, M1 picks it up). The orbs get that
//  keyword at load, so the current ARPG POV shows them with no change on its
//  side. They also get a vanilla gem's mesh at load -- the ESP names no model.
//
//  ★DEATHS. One TESDeathEvent sink (dead == true only) queues the actor's
//  FormID; the queue is drained one actor per game frame through the task
//  interface, so a pack dying at once costs a frame each, never a spike. An
//  actor is marked in the co-save ('ODRP') before its orbs are placed, so a
//  corpse is never rolled twice; TESResetEvent forgets the mark.
// =============================================================================
#pragma once

#include "roll/Craft.h"

#include <cstdint>
#include <string>

namespace Orbs
{
    // Resolves the orb forms and dresses them (model, keyword). kDataLoaded.
    void Init();
    void Install();   // the death sink; after Init

    // Picks the death queue up again after a load (2.1.1). A drain that finds
    // the world loading stops and leaves the queue as it is; nothing re-queues
    // itself to wait. Called by the loading watch (Apply) when the world is
    // back. GAME THREAD.
    void Resume();

    [[nodiscard]] bool Available();
    [[nodiscard]] RE::TESObjectMISC* Form(roll::Orb a_orb);
    // The orb this base form is, or -1. Any thread: compares cached ids only.
    [[nodiscard]] int OrbOf(std::uint32_t a_base) noexcept;
    [[nodiscard]] std::uint32_t Color(roll::Orb a_orb) noexcept;   // 0xAABBGGRR

    // Whether a_orb could be used on this unit, and why not. Read-only; the
    // grid asks it on the render thread while hovering. a_xl may be null.
    [[nodiscard]] bool Check(roll::Orb a_orb, std::uint32_t a_base, const RE::ExtraDataList* a_xl,
        std::string& a_reason);

    // Uses one orb from the player's inventory on the unit. GAME THREAD. The
    // orb is consumed only when the craft happened.
    bool Use(roll::Orb a_orb, std::uint32_t a_base, RE::ExtraDataList* a_xl, std::string& a_message);

    // Debug: puts orbs straight into the player's inventory. GAME THREAD.
    void Give(roll::Orb a_orb, int a_count);
    // Debug: the next death rolls a guaranteed drop.
    void ForceNextDrop();
    // Removes ground orbs older than OrbCleanupDays. GAME THREAD.
    std::size_t Cleanup(bool a_all = false);

    // The mesh the orb form carries now, and whether it is a vanilla cut gem.
    [[nodiscard]] std::string ModelOf(roll::Orb a_orb);
    [[nodiscard]] bool        IsGemMesh(const std::string& a_model);

    // Drop quantity by difficulty. Last* is what the last kill used; Preview
    // is what the next one would (from the difficulty last read). Refresh
    // re-reads the difficulty -- GAME THREAD only.
    [[nodiscard]] float LastDropMultiplier();
    [[nodiscard]] int   LastDifficulty();
    [[nodiscard]] float PreviewDropMultiplier();
    void                RefreshDifficulty();

    struct Stats
    {
        std::uint64_t deaths{ 0 };
        std::uint64_t dropped{ 0 };   // kills that produced at least one orb
        std::uint64_t orbs{ 0 };
        std::uint64_t cleaned{ 0 };
        std::uint64_t used{ 0 };
    };
    [[nodiscard]] Stats CurrentStats();
}
