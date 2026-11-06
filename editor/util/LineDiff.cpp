// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LineDiff.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

using namespace jasperengine;
using editor::LineChange;

// a named namespace: the editor's unity build puts other files' helpers beside these
namespace jasperengine::editor::linediff {

using Lines = std::span<const std::string>;

constexpr int MAX_SHIFT = 100;

// Myers' O(ND) diff, marking the lines removed from a and added to b
bool myersDiff(const std::vector<int>& a, const std::vector<int>& b, std::vector<bool>& removed, std::vector<bool>& added) {
    const int n = static_cast<int>(a.size());
    const int m = static_cast<int>(b.size());
    const int maxEdits = std::min(n + m, editor::LineDiff::kMaxEdits);
    const int offset = maxEdits + 1;

    // Furthest x on each diagonal, kept for every edit count to walk the path back
    std::vector<int> v(2 * maxEdits + 3, 0);
    std::vector<std::vector<int>> trace;

    for (int d = 0; d <= maxEdits; d++) {
        trace.emplace_back(2 * d + 1);
        std::vector<int>& row = trace.back();

        for (int k = -d; k <= d; k += 2) {
            int x = (k == -d || (k != d && v[offset + k - 1] < v[offset + k + 1])) ? v[offset + k + 1] : v[offset + k - 1] + 1;
            int y = x - k;
            while (x < n && y < m && a[x] == b[y]) {
                x++;
                y++;
            }
            v[offset + k] = x;
            row[k + d] = x;

            if (x < n || y < m) continue;

            removed.assign(n, false);
            added.assign(m, false);
            for (int step = d; step > 0; step--) {
                const std::vector<int>& prev = trace[step - 1];
                const int diagonal = x - y;
                const bool down = diagonal == -step ||
                    (diagonal != step && prev[diagonal - 1 + step - 1] < prev[diagonal + 1 + step - 1]);
                const int prevDiagonal = down ? diagonal + 1 : diagonal - 1;
                x = prev[prevDiagonal + step - 1];
                y = x - prevDiagonal;
                if (down) {
                    added[y] = true;
                } else {
                    removed[x] = true;
                }
            }
            return true;
        }
    }

    return false;
}

// The removed and added lines between two matches make one change
void collectChanges(const std::vector<bool>& removed, const std::vector<bool>& added, int offset, std::vector<LineChange>& changes) {
    const int n = static_cast<int>(removed.size());
    const int m = static_cast<int>(added.size());
    int i = 0;
    int j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && !removed[i] && !added[j]) {
            i++;
            j++;
            continue;
        }

        const int startI = i;
        const int startJ = j;
        while (i < n && removed[i]) i++;
        while (j < m && added[j]) j++;
        if (i == startI && j == startJ) break;

        changes.push_back({offset + startI, offset + i, offset + startJ, offset + j});
    }
}

bool isInsertionOrDeletion(const LineChange& change) {
    return change.originalStart == change.originalEnd || change.modifiedStart == change.modifiedEnd;
}

void shiftChange(LineChange& change, int delta) {
    change.originalStart += delta;
    change.originalEnd += delta;
    change.modifiedStart += delta;
    change.modifiedEnd += delta;
}

// Slides insertions and deletions over repeated lines, merging those that meet
void joinByShifting(Lines a, Lines b, std::vector<LineChange>& changes) {
    if (changes.empty()) return;

    std::vector<LineChange> left = {changes[0]};
    for (size_t i = 1; i < changes.size(); i++) {
        LineChange& prev = left.back();
        LineChange cur = changes[i];

        if (isInsertionOrDeletion(cur)) {
            const int gap = cur.originalStart - prev.originalEnd;
            int d = 1;
            while (d <= gap && a[cur.originalStart - d] == a[cur.originalEnd - d] &&
                   b[cur.modifiedStart - d] == b[cur.modifiedEnd - d]) {
                d++;
            }
            d--;

            if (d == gap) {
                prev.originalEnd = cur.originalEnd - gap;
                prev.modifiedEnd = cur.modifiedEnd - gap;
                continue;
            }
            shiftChange(cur, -d);
        }
        left.push_back(cur);
    }

    std::vector<LineChange> right;
    for (size_t i = 0; i + 1 < left.size(); i++) {
        LineChange cur = left[i];
        LineChange& next = left[i + 1];

        if (isInsertionOrDeletion(cur)) {
            const int gap = next.originalStart - cur.originalEnd;
            int d = 0;
            while (d < gap && a[cur.originalStart + d] == a[cur.originalEnd + d] &&
                   b[cur.modifiedStart + d] == b[cur.modifiedEnd + d]) {
                d++;
            }

            if (d == gap) {
                next.originalStart = cur.originalStart + gap;
                next.modifiedStart = cur.modifiedStart + gap;
                continue;
            }
            shiftChange(cur, d);
        }
        right.push_back(cur);
    }
    right.push_back(left.back());

    changes = std::move(right);
}

int indentation(const std::string& line) {
    int i = 0;
    while (i < static_cast<int>(line.size()) && (line[i] == ' ' || line[i] == '\t')) i++;
    return i;
}

// An edge between less indented lines reads better
int boundaryScore(Lines lines, int offset) {
    const int before = offset > 0 ? indentation(lines[offset - 1]) : 0;
    const int after = offset < static_cast<int>(lines.size()) ? indentation(lines[offset]) : 0;
    return 1000 - (before + after);
}

// Best slide for lines [start, end), which other lacks at `at`
int bestShift(Lines other, Lines lines, int at, int start, int end, int otherMin, int otherMax, int linesMin, int linesMax) {
    int before = 1;
    while (at - before >= otherMin && start - before >= linesMin && before < MAX_SHIFT &&
           lines[start - before] == lines[end - before]) {
        before++;
    }
    before--;

    int after = 0;
    while (at + after < otherMax && end + after < linesMax && after < MAX_SHIFT &&
           lines[start + after] == lines[end + after]) {
        after++;
    }

    // Ties keep the earliest position
    int best = 0;
    int bestScore = -1;
    for (int delta = -before; delta <= after; delta++) {
        const int score = boundaryScore(other, at + delta) + boundaryScore(lines, start + delta) + boundaryScore(lines, end + delta);
        if (score > bestScore) {
            bestScore = score;
            best = delta;
        }
    }
    return best;
}

// Moves each insertion and deletion to the slide with the best edges
void shiftToBetterBoundaries(Lines a, Lines b, std::vector<LineChange>& changes) {
    for (size_t i = 0; i < changes.size(); i++) {
        LineChange& change = changes[i];
        const bool insertion = change.originalStart == change.originalEnd;
        const bool deletion = change.modifiedStart == change.modifiedEnd;
        if (insertion == deletion) continue;

        const int aMin = i > 0 ? changes[i - 1].originalEnd + 1 : 0;
        const int aMax = i + 1 < changes.size() ? changes[i + 1].originalStart - 1 : static_cast<int>(a.size());
        const int bMin = i > 0 ? changes[i - 1].modifiedEnd + 1 : 0;
        const int bMax = i + 1 < changes.size() ? changes[i + 1].modifiedStart - 1 : static_cast<int>(b.size());

        const int delta = insertion
            ? bestShift(a, b, change.originalStart, change.modifiedStart, change.modifiedEnd, aMin, aMax, bMin, bMax)
            : bestShift(b, a, change.modifiedStart, change.originalStart, change.originalEnd, bMin, bMax, aMin, aMax);
        shiftChange(change, delta);
    }
}

// Joins changes kept apart only by lines with barely any text
void joinAcrossShortMatches(Lines a, std::vector<LineChange>& changes) {
    auto size = [](const LineChange& change) {
        return (change.originalEnd - change.originalStart) + (change.modifiedEnd - change.modifiedStart);
    };

    for (int pass = 0; pass <= 10 && changes.size() > 1; pass++) {
        bool joined = false;
        std::vector<LineChange> result = {changes[0]};

        for (size_t i = 1; i < changes.size(); i++) {
            LineChange& last = result.back();
            const LineChange& cur = changes[i];

            int visible = 0;
            for (int line = last.originalEnd; line < cur.originalStart && visible <= 4; line++) {
                for (char ch : a[line]) {
                    if (!std::isspace(static_cast<unsigned char>(ch))) visible++;
                }
            }

            if (visible <= 4 && (size(last) > 5 || size(cur) > 5)) {
                last.originalEnd = cur.originalEnd;
                last.modifiedEnd = cur.modifiedEnd;
                joined = true;
            } else {
                result.push_back(cur);
            }
        }

        changes = std::move(result);
        if (!joined) break;
    }
}

std::string hunkRange(int start, int count) {
    // unified diff: a zero-length range names the line before it
    const int first = count == 0 ? start : start + 1;
    return std::to_string(first) + (count == 1 ? std::string() : "," + std::to_string(count));
}

}


const char* editor::lineChangeKindName(LineChange::Kind kind) {
    switch (kind) {
        case LineChange::Kind::Added: return "added";
        case LineChange::Kind::Modified: return "modified";
        case LineChange::Kind::Deleted: return "deleted";
    }
    return "";
}

std::vector<LineChange> editor::LineDiff::compute(std::span<const std::string> original, std::span<const std::string> modified) {
    const int n = static_cast<int>(original.size());
    const int m = static_cast<int>(modified.size());

    // Edits are mostly local, the common ends need no diff
    int prefix = 0;
    while (prefix < n && prefix < m && original[prefix] == modified[prefix]) prefix++;
    int suffix = 0;
    while (suffix < n - prefix && suffix < m - prefix && original[n - 1 - suffix] == modified[m - 1 - suffix]) suffix++;

    const int originalCount = n - prefix - suffix;
    const int modifiedCount = m - prefix - suffix;

    std::vector<LineChange> changes;
    if (originalCount == 0 && modifiedCount == 0) return changes;

    std::vector<bool> removed;
    std::vector<bool> added;
    bool diffed = false;
    if (originalCount > 0 && modifiedCount > 0) {
        // Lines as ids, compared as integers
        std::unordered_map<std::string_view, int> ids;
        std::vector<int> a(originalCount);
        std::vector<int> b(modifiedCount);
        for (int i = 0; i < originalCount; i++) {
            a[i] = ids.emplace(original[prefix + i], static_cast<int>(ids.size())).first->second;
        }
        for (int i = 0; i < modifiedCount; i++) {
            b[i] = ids.emplace(modified[prefix + i], static_cast<int>(ids.size())).first->second;
        }
        diffed = linediff::myersDiff(a, b, removed, added);
    }

    if (diffed) {
        linediff::collectChanges(removed, added, prefix, changes);
    } else {
        // An insertion, a deletion, or too many edits to align
        changes.push_back({prefix, prefix + originalCount, prefix, prefix + modifiedCount});
    }

    linediff::joinByShifting(original, modified, changes);
    linediff::joinByShifting(original, modified, changes);
    linediff::shiftToBetterBoundaries(original, modified, changes);
    linediff::joinAcrossShortMatches(original, changes);

    return changes;
}

std::string editor::LineDiff::unified(std::string_view original, std::string_view modified, std::string_view oldName,
                                      std::string_view newName) {
    // lines as git compares them: byte for byte with their '\n' ('\r' kept), so a last line
    // without one differs from the same text with one
    auto keyed = [](std::string_view text) {
        std::vector<std::string> lines;
        size_t start = 0;
        while (start < text.size()) {
            const size_t end = text.find('\n', start);
            const size_t next = end == std::string_view::npos ? text.size() : end + 1;
            lines.emplace_back(text.substr(start, next - start));
            start = next;
        }
        return lines;
    };
    const std::vector<std::string> a = keyed(original);
    const std::vector<std::string> b = keyed(modified);
    const std::vector<LineChange> changes = compute(a, b);
    if (changes.empty()) return {};

    constexpr int CONTEXT = 3;
    std::string out = "--- a/" + std::string(oldName) + "\n+++ b/" + std::string(newName) + "\n";
    auto line = [&out](char sign, const std::string& text) {
        out += sign;
        out += text;
        if (text.empty() || text.back() != '\n') out += "\n\\ No newline at end of file\n";
    };

    size_t i = 0;
    while (i < changes.size()) {
        // changes closer than two contexts share a hunk
        size_t last = i;
        while (last + 1 < changes.size() && changes[last + 1].originalStart - changes[last].originalEnd <= 2 * CONTEXT) last++;

        const int originalStart = std::max(0, changes[i].originalStart - CONTEXT);
        const int modifiedStart = std::max(0, changes[i].modifiedStart - CONTEXT);
        const int originalEnd = std::min(static_cast<int>(a.size()), changes[last].originalEnd + CONTEXT);
        const int modifiedEnd = std::min(static_cast<int>(b.size()), changes[last].modifiedEnd + CONTEXT);
        out += "@@ -" + linediff::hunkRange(originalStart, originalEnd - originalStart) + " +" +
               linediff::hunkRange(modifiedStart, modifiedEnd - modifiedStart) + " @@\n";

        int o = originalStart;
        for (size_t c = i; c <= last; c++) {
            const LineChange& change = changes[c];
            for (; o < change.originalStart; o++) line(' ', a[o]);
            for (int r = change.originalStart; r < change.originalEnd; r++) line('-', a[r]);
            for (int m = change.modifiedStart; m < change.modifiedEnd; m++) line('+', b[m]);
            o = change.originalEnd;
        }
        for (; o < originalEnd; o++) line(' ', a[o]);
        i = last + 1;
    }
    return out;
}

std::vector<std::string> editor::LineDiff::splitLines(std::string_view text) {
    std::vector<std::string> lines;
    std::string line;
    for (char c : text) {
        if (c == '\n') {
            lines.push_back(std::move(line));
            line.clear();
        } else if (c != '\r') {
            line += c;
        }
    }
    lines.push_back(std::move(line));
    return lines;
}
