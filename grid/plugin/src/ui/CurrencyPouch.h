#pragma once

// =============================================================================
//  Currencies (POErim 2.1.0): sizing, icons, the report, and the pouch window
// =============================================================================
//  A CURRENCY is any base form a registered Applier claims (IsCurrency) --
//  POErim's orbs. Nothing here knows what an orb is; with no applier
//  registered every function below is a no-op, which is what keeps upstream
//  Grid Inventory users exactly where they were.
//
//    * SIZE: a currency is always 1x1 and stacks 20 to a cell on a board
//      (DefFor in main.cpp). The cap is DISPLAY ONLY -- the grid's usual
//      Mabinogi stacking splits a count of 45 into tiles of 20/20/5; the
//      engine's own count is never touched.
//    * ICON: a currency draws its per-item PNG
//      (GridInventory_fallback/item/<Plugin_0xID>.png) and nothing else: no 3D
//      capture, no category drawing. A miss logs a warning once.
//    * REPORT: kMsgCurrencyReport tells the extension what each currency got.
//    * POUCH: a separate window beside the inventory with one slot per
//      currency, a view onto the player's own inventory -- no storage of its
//      own, so nothing to dupe, lose or save. GridInventory_ui.ini
//      [CurrencyPouch] Enabled / Hotkey / PosX / PosY.
// =============================================================================

#include "ui/IconCache.h"

#include <cstdint>

namespace FUI::CurrencyPouch
{
    // Whether this base form is a currency (asks the applier; false without one).
    // Render-thread safe.
    [[nodiscard]] bool IsCurrency(const RE::TESBoundObject* a_obj);
    [[nodiscard]] bool IsCurrencyId(std::uint32_t a_base);

    // The currency's PNG, or nullptr (logs once per form on a miss).
    [[nodiscard]] const IconCache::Icon* IconOf(RE::TESBoundObject* a_obj);

    // Whether the player's own board leaves this form out because the pouch
    // shows it instead: pouch on, currency, and an ordinary inventory session
    // (never in a container or a shop, where orbs must be movable as tiles).
    // ★2.1.1: a currency is a MISC the applier claims by form ID (POErim: its
    // nine orbs, DiabloInSkyrim.esp 0x801-0x809) -- nothing by type, mesh,
    // keyword or gem flag, so a quest item, key or gem is never hidden.
    [[nodiscard]] bool HidesFromBoard(const RE::TESBoundObject* a_obj);

    // The filter's switch on its own: pouch on and the plain inventory.
    // Every board build reads it (Grid::Rebuild), whatever caused the build.
    [[nodiscard]] bool HidingNow();

    // GridInventory_ui.ini CurrencyPouchDebugLog / UiStateDebugLog (both 0 by
    // default): a line per board refresh (why, shown, hidden), and a line per
    // UI-state reset at loads, load screens and menu closes.
    [[nodiscard]] bool DebugBoard();
    [[nodiscard]] bool DebugUiState();

    // Drop the drag and any queued ground drop. Loads, load screens, menu close.
    void ResetTransient(const char* a_why);

    [[nodiscard]] bool Enabled();

    // GAME THREAD, from UIRoot::Tick: counts, queued ground drops, the one-time
    // preload of every currency icon and the report.
    void Tick();

    // RENDER THREAD, top level, after the board: the toggle button and the window.
    void Draw();

    // Close the window (ESC layering). True when it was open.
    bool Close();
}
