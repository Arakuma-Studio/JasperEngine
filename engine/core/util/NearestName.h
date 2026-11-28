// Copyright (c) 2026 Gamecrafters Guild Limited and contributors
// SPDX-License-Identifier: MIT
// From OpenEngine, Engine/Modules/Types/Include/Types/NearestName.h, unchanged except for the
// namespace and the JasperEngine addition at the end, NearestIdentifier (NOTICE.md).

#pragma once

// Nearest-name suggestions for diagnostics ("did you mean 'X'?"). Shared by
// every surface that validates an authored name against a known set — render
// pipeline resource refs, material shader keywords — so one spelling of the
// suggestion rule serves them all.

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine
{

// Distance beyond which two names are not plausibly the same typo'd word.
inline constexpr size_t kNearestNameMaxDistance = 2;

/// Levenshtein distance, capped: returns `cap + 1` as soon as the distance is
/// known to exceed `cap`. Suggestions only care about small distances, so the
/// early-out keeps a long candidate list cheap.
inline size_t LevenshteinCapped(std::string_view a, std::string_view b, size_t cap)
{
    const size_t n = a.size();
    const size_t m = b.size();
    if (n > m + cap || m > n + cap)
        return cap + 1;
    std::vector<size_t> prev(m + 1);
    std::vector<size_t> cur(m + 1);
    for (size_t j = 0; j <= m; ++j)
        prev[j] = j;
    for (size_t i = 1; i <= n; ++i)
    {
        cur[0] = i;
        size_t rowMin = cur[0];
        for (size_t j = 1; j <= m; ++j)
        {
            const size_t cost = (a[i - 1] == b[j - 1]) ? 0u : 1u;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
            rowMin = std::min(rowMin, cur[j]);
        }
        if (rowMin > cap)
            return cap + 1;
        std::swap(prev, cur);
    }
    return prev[m];
}

/// The candidate closest to `target` within `maxDistance`, or an empty string
/// when nothing is close enough. Ties resolve to the first candidate in range
/// order, so the caller controls determinism by ordering its candidates.
template <typename Range>
std::string NearestName(std::string_view target, const Range& candidates,
                        size_t maxDistance = kNearestNameMaxDistance)
{
    std::string best;
    size_t bestDist = maxDistance + 1;
    for (const auto& candidate : candidates)
    {
        const std::string_view sv(candidate);
        const size_t d = LevenshteinCapped(target, sv, maxDistance);
        if (d < bestDist)
        {
            bestDist = d;
            best.assign(sv);
        }
    }
    return bestDist <= maxDistance ? best : std::string();
}

/// " (did you mean 'X'?)" for the nearest candidate, or "" when none is close.
/// Ready to append to a diagnostic sentence.
template <typename Range>
std::string NearestNameSuffix(std::string_view target, const Range& candidates,
                              size_t maxDistance = kNearestNameMaxDistance)
{
    const std::string best = NearestName(target, candidates, maxDistance);
    return best.empty() ? std::string() : (" (did you mean '" + best + "'?)");
}

// ---- JasperEngine addition (not in OpenEngine) ----

/// `name` without case, '_' and '-': "localPosition", "local_position" and "Local-Position" all fold
/// to "localposition".
inline std::string FoldedName(std::string_view name)
{
    std::string out;
    out.reserve(name.size());
    for (const char c : name)
    {
        if (c == '_' || c == '-')
            continue;
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

/// NearestName for identifiers an agent types (MCP tool, argument, key and enum names): first a
/// candidate equal to `target` once both are folded (a camelCase spelling of a snake_case name, a
/// missing or extra '_'), then NearestName as written, then NearestName of the folded names (a
/// typo on top of a case confusion). "" when nothing is close enough.
template <typename Range>
std::string NearestIdentifier(std::string_view target, const Range& candidates,
                              size_t maxDistance = kNearestNameMaxDistance)
{
    const std::string folded = FoldedName(target);
    for (const auto& candidate : candidates)
    {
        const std::string_view sv(candidate);
        if (FoldedName(sv) == folded)
            return std::string(sv);
    }
    std::string best = NearestName(target, candidates, maxDistance);
    if (!best.empty())
        return best;
    size_t bestDist = maxDistance + 1;
    for (const auto& candidate : candidates)
    {
        const std::string_view sv(candidate);
        const size_t d = LevenshteinCapped(folded, FoldedName(sv), maxDistance);
        if (d < bestDist)
        {
            bestDist = d;
            best.assign(sv);
        }
    }
    return bestDist <= maxDistance ? best : std::string();
}

/// " (did you mean 'X'?)" for NearestIdentifier, or "" when none is close.
template <typename Range>
std::string NearestIdentifierSuffix(std::string_view target, const Range& candidates,
                                    size_t maxDistance = kNearestNameMaxDistance)
{
    const std::string best = NearestIdentifier(target, candidates, maxDistance);
    return best.empty() ? std::string() : (" (did you mean '" + best + "'?)");
}

} // namespace jasperengine
