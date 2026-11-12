// =============================================================================
//  Orb drops -- how many orbs a kill hands over, and which. Pure, like the roller.
// =============================================================================
//  The plugin decides WHERE a drop goes (the ground or the corpse) and makes
//  sure a kill is only ever rolled once; this decides WHAT it is, so the harness
//  can simulate ten thousand kills at a level and print the rates before anyone
//  launches the game to feel them.
//
//  Shape of a drop, in order:
//
//    1. does this kill drop orbs at all   -- one draw against `chance`
//    2. how many                          -- baseCount + countPerLevel * level,
//                                            capped at maxCount, times the
//                                            global multiplier; the fraction is
//                                            a chance of one more
//    3. which orb, for each               -- a weighted draw
//
//  Orbs are meant to be COMMON: most kills drop one or two, nearly all of them
//  Transmutation and Augmentation. The rest are the weights' business.
// =============================================================================
#pragma once

#include "Craft.h"

#include <array>

namespace roll
{
    struct DropTuning
    {
        // Scales the count of every orb drop: kills, chests and vendors alike.
        float multiplier{ 1.0f };

        float chance{ 0.85f };
        float baseCount{ 1.5f };
        float countPerLevel{ 0.03f };
        float maxCount{ 4.0f };

        // Relative weights, in roll::Orb order: transmute, augment, alchemy,
        // exalt, chaos, scour, divine, vaal. The defaults sum to 1000, so each
        // reads as "per thousand orbs".
        std::array<int, kOrbCount> weights{ 410, 344, 45, 35, 70, 80, 6, 10 };
    };

    struct OrbDrop
    {
        std::array<int, kOrbCount> counts{};
        int                        total{ 0 };
    };

    // One kill's (or one chest's) orbs. a_scale multiplies the count for this
    // occasion only -- a chest is worth a different amount than a bandit.
    [[nodiscard]] OrbDrop RollOrbDrop(int a_level, const DropTuning& a_tuning, Rng& a_rng,
        float a_scale = 1.0f);

    // The mean orb count the tuning aims at for a kill at this level that drops.
    [[nodiscard]] float OrbCountFor(int a_level, const DropTuning& a_tuning) noexcept;
}
