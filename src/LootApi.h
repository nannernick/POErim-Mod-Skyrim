#pragma once

#include <cstdint>

namespace LootApi
{
    void Broadcast();

    // Drops the v2 per-enchantment cache. Call after every craft and on load.
    void Invalidate();

    // For the debug menu's Integration page: how many v2 calls another plugin
    // has made (ARPG POV using the API), and the last colour handed over.
    [[nodiscard]] std::uint32_t V2Calls();
    [[nodiscard]] bool          LastAnswer(std::uint32_t& a_base, std::uint32_t& a_rgba);
}
