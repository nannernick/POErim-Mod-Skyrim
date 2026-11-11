// =============================================================================
//  The debug menu -- a page set in SKSE Menu Framework's F1 menu.
// =============================================================================
//  Player-facing settings stay in DiabloLoot.ini. This is the other half: the
//  things that are only worth having while tuning or diagnosing -- what an item
//  actually rolled, what the co-save holds, what an orb would do to an item a
//  thousand times over, which of the mods this one talks to are really there.
//
//  ★OPTIONAL, AND ABSENT IS NOT AN ERROR. SKSE Menu Framework is looked for by
//  module handle at kPostLoad; without it nothing is registered, one line goes
//  to the log, and the rest of the mod neither knows nor cares. The framework's
//  header is vendored (include/SKSEMenuFramework.h) and resolves every call by
//  name at run time, so there is nothing to link and nothing to fail to load.
//
//  ★THE PAGES DRAW ON THE RENDER THREAD. They may read; anything that touches
//  the world is queued to the game thread through SKSE's task interface, and
//  anything that CHANGES A SAVE sits behind a confirm box and writes a log line
//  when it runs.
// =============================================================================
#pragma once

namespace DebugMenu
{
    // Registers the pages. Call at kPostLoad, when every plugin is loaded.
    void Register();

    // Whether the pages were registered -- SKSE Menu Framework is installed.
    [[nodiscard]] bool Available();

    // Drops anything the pages cached about the world being replaced. Call
    // where Sockets::Forget is called.
    void Forget();
}
