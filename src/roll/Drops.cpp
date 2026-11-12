// =============================================================================
//  Orb drops. Pure -- see Drops.h.
// =============================================================================

#include "Drops.h"

#include <algorithm>
#include <cmath>

namespace roll
{
    float OrbCountFor(int a_level, const DropTuning& a_tuning) noexcept
    {
        const float level = static_cast<float>(std::max(1, a_level));
        const float raw = a_tuning.baseCount + a_tuning.countPerLevel * level;
        return std::clamp(raw, 0.0f, std::max(0.0f, a_tuning.maxCount));
    }

    float DifficultyMultiplier(int a_difficultyLevel, float a_step) noexcept
    {
        const int   level = std::clamp(a_difficultyLevel, 0, 5);
        const float mult = 1.0f + std::max(0.0f, a_step) * static_cast<float>(level);
        return std::max(1.0f, mult);
    }

    int ScaleCount(int a_count, float a_mult, Rng& a_rng)
    {
        if (a_count <= 0 || !(a_mult > 1.0f)) {
            return a_count;   // ★NO DRAW AT 1.0: the identity the harness checks
        }
        const double scaled = static_cast<double>(a_count) * static_cast<double>(a_mult);
        int          out = static_cast<int>(std::floor(scaled));
        const double frac = scaled - static_cast<double>(out);
        if (frac > 0.0) {
            std::uniform_real_distribution<double> unit{ 0.0, 1.0 };
            if (unit(a_rng) < frac) {
                ++out;
            }
        }
        return out;
    }

    OrbDrop RollOrbDrop(int a_level, const DropTuning& a_tuning, Rng& a_rng, float a_scale,
        float a_difficulty)
    {
        OrbDrop drop;

        std::uniform_real_distribution<float> unit{ 0.0f, 1.0f };
        if (a_tuning.chance <= 0.0f || unit(a_rng) >= a_tuning.chance) {
            return drop;
        }

        const float mean = OrbCountFor(a_level, a_tuning) * std::max(0.0f, a_tuning.multiplier) *
                           std::max(0.0f, a_scale);
        int count = static_cast<int>(std::floor(mean));
        if (unit(a_rng) < mean - static_cast<float>(count)) {
            ++count;
        }
        count = ScaleCount(count, a_difficulty, a_rng);
        // A ceiling that has nothing to do with balance: a multiplier typed
        // with one zero too many should not place a thousand references.
        count = std::clamp(count, 0, 64);
        if (count == 0) {
            return drop;
        }

        long long total = 0;
        for (const int weight : a_tuning.weights) {
            total += std::max(0, weight);
        }
        if (total <= 0) {
            return drop;
        }

        std::uniform_int_distribution<long long> pick{ 0, total - 1 };
        for (int i = 0; i < count; ++i) {
            long long roll = pick(a_rng);
            for (int orb = 0; orb < kOrbCount; ++orb) {
                const long long weight = std::max(0, a_tuning.weights[static_cast<std::size_t>(orb)]);
                if (roll < weight) {
                    drop.counts[static_cast<std::size_t>(orb)]++;
                    drop.total++;
                    break;
                }
                roll -= weight;
            }
        }
        return drop;
    }
}
