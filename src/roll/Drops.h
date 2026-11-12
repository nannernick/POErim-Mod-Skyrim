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
//    2b. difficulty                       -- that whole count times the
//                                            difficulty multiplier (2.1.0):
//                                            floor(count * mult) for sure, and
//                                            one more with probability equal to
//                                            the remainder. Skipped outright at
//                                            1.0, so the draw is byte-identical
//                                            to a build without it.
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
        // exalt, chaos, scour, divine, vaal, regal. The first eight sum to
        // 1000; Regal (2.1.0) sits between Alchemy and Exalted at 40, so the
        // whole set is "per 1,040 orbs".
        std::array<int, kOrbCount> weights{ 410, 344, 45, 35, 70, 80, 6, 10, 40 };
    };

    struct OrbDrop
    {
        std::array<int, kOrbCount> counts{};
        int                        total{ 0 };
    };

    // One kill's (or one chest's) orbs. a_scale multiplies the count for this
    // occasion only -- a chest is worth a different amount than a bandit.
    // a_difficulty is the difficulty multiplier (DifficultyMultiplier), never
    // below 1.0.
    [[nodiscard]] OrbDrop RollOrbDrop(int a_level, const DropTuning& a_tuning, Rng& a_rng,
        float a_scale = 1.0f, float a_difficulty = 1.0f);

    // 1.0 + a_step * level, level 0 (Novice) .. 5 (Legendary), never below 1.
    [[nodiscard]] float DifficultyMultiplier(int a_difficultyLevel, float a_step = 0.15f) noexcept;

    // The difficulty step applied to a whole orb count: floor(count * mult)
    // plus one with probability equal to the remainder. Mean exactly
    // count * mult. Draws nothing when mult is 1.0 (or below, clamped).
    [[nodiscard]] int ScaleCount(int a_count, float a_mult, Rng& a_rng);

    // The mean orb count the tuning aims at for a kill at this level that drops.
    [[nodiscard]] float OrbCountFor(int a_level, const DropTuning& a_tuning) noexcept;
}
