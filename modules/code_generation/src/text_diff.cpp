#include "forma/codegen/text_diff.h"

#include <algorithm>
#include <cstdint>

#include "forma/core/text.h"

namespace forma::codegen {

namespace {

constexpr int kMaxEditDistance = 8000;

std::vector<DiffHunk> myers(const std::vector<std::string>& a, int aStart, int aEnd, const std::vector<std::string>& b, int bStart, int bEnd)
{
    int n = aEnd - aStart;
    int m = bEnd - bStart;
    std::vector<DiffHunk> hunks;
    if (n == 0 && m == 0)
        return hunks;
    if (n == 0 || m == 0) {
        hunks.push_back({aStart, n, bStart, m});
        return hunks;
    }
    int limit = std::min(n + m, kMaxEditDistance);
    int offset = limit + 1;
    std::vector<int> v(static_cast<std::size_t>(2 * limit + 3), 0);
    std::vector<std::vector<int>> trace;
    int foundD = -1;
    for (int d = 0; d <= limit; ++d) {
        trace.push_back(v);
        for (int k = -d; k <= d; k += 2) {
            int x;
            if (k == -d || (k != d && v[static_cast<std::size_t>(offset + k - 1)] < v[static_cast<std::size_t>(offset + k + 1)]))
                x = v[static_cast<std::size_t>(offset + k + 1)];
            else
                x = v[static_cast<std::size_t>(offset + k - 1)] + 1;
            int y = x - k;
            while (x < n && y < m && a[static_cast<std::size_t>(aStart + x)] == b[static_cast<std::size_t>(bStart + y)]) {
                ++x;
                ++y;
            }
            v[static_cast<std::size_t>(offset + k)] = x;
            if (x >= n && y >= m) {
                foundD = d;
                break;
            }
        }
        if (foundD >= 0)
            break;
    }
    if (foundD < 0) {
        hunks.push_back({aStart, n, bStart, m});
        return hunks;
    }
    struct Step {
        int x;
        int y;
        int prevX;
        int prevY;
    };
    std::vector<Step> steps;
    int x = n;
    int y = m;
    for (int d = foundD; d > 0; --d) {
        const auto& previous = trace[static_cast<std::size_t>(d)];
        int k = x - y;
        int prevK;
        if (k == -d || (k != d && previous[static_cast<std::size_t>(offset + k - 1)] < previous[static_cast<std::size_t>(offset + k + 1)]))
            prevK = k + 1;
        else
            prevK = k - 1;
        int prevX = previous[static_cast<std::size_t>(offset + prevK)];
        int prevY = prevX - prevK;
        while (x > prevX && y > prevY) {
            --x;
            --y;
        }
        steps.push_back({x, y, prevX, prevY});
        x = prevX;
        y = prevY;
    }
    std::reverse(steps.begin(), steps.end());
    DiffHunk current{};
    bool open = false;
    for (const auto& step : steps) {
        int deleted = step.x - step.prevX;
        int inserted = step.y - step.prevY;
        int atA = aStart + step.prevX;
        int atB = bStart + step.prevY;
        if (open && current.baseStart + current.baseCount == atA && current.otherStart + current.otherCount == atB) {
            current.baseCount += deleted;
            current.otherCount += inserted;
        } else {
            if (open)
                hunks.push_back(current);
            current = {atA, deleted, atB, inserted};
            open = true;
        }
    }
    if (open)
        hunks.push_back(current);
    return hunks;
}

std::string joinLines(const std::vector<std::string>& lines, bool trailingNewline)
{
    std::string output;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        output += lines[i];
        if (i + 1 < lines.size() || trailingNewline)
            output.push_back('\n');
    }
    return output;
}

bool endsWithNewline(std::string_view text)
{
    return !text.empty() && text.back() == '\n';
}

void appendRange(std::vector<std::string>& output, const std::vector<std::string>& lines, int start, int count)
{
    for (int i = 0; i < count; ++i)
        output.push_back(lines[static_cast<std::size_t>(start + i)]);
}

std::vector<std::string> sideOfRegion(const std::vector<std::string>& base, const std::vector<std::string>& side, const std::vector<DiffHunk>& hunks, int regionStart, int regionEnd)
{
    std::vector<std::string> output;
    int cursor = regionStart;
    for (const auto& hunk : hunks) {
        if (hunk.baseStart + hunk.baseCount < regionStart || hunk.baseStart > regionEnd)
            continue;
        if (hunk.baseStart < regionStart || hunk.baseStart + hunk.baseCount > regionEnd)
            continue;
        appendRange(output, base, cursor, hunk.baseStart - cursor);
        appendRange(output, side, hunk.otherStart, hunk.otherCount);
        cursor = hunk.baseStart + hunk.baseCount;
    }
    if (cursor < regionEnd)
        appendRange(output, base, cursor, regionEnd - cursor);
    return output;
}

}

std::vector<DiffHunk> diffLines(const std::vector<std::string>& base, const std::vector<std::string>& other)
{
    int start = 0;
    int baseEnd = static_cast<int>(base.size());
    int otherEnd = static_cast<int>(other.size());
    while (start < baseEnd && start < otherEnd && base[static_cast<std::size_t>(start)] == other[static_cast<std::size_t>(start)])
        ++start;
    while (baseEnd > start && otherEnd > start && base[static_cast<std::size_t>(baseEnd - 1)] == other[static_cast<std::size_t>(otherEnd - 1)]) {
        --baseEnd;
        --otherEnd;
    }
    return myers(base, start, baseEnd, other, start, otherEnd);
}

MergeResult mergeThreeWay(std::string_view base, std::string_view mine, std::string_view theirs, std::string_view mineLabel, std::string_view theirsLabel)
{
    auto baseLines = text::splitLines(base);
    auto mineLines = text::splitLines(mine);
    auto theirLines = text::splitLines(theirs);
    auto mineHunks = diffLines(baseLines, mineLines);
    auto theirHunks = diffLines(baseLines, theirLines);

    MergeResult result;
    std::vector<std::string> output;
    std::size_t mineIndex = 0;
    std::size_t theirIndex = 0;
    int cursor = 0;
    int baseCount = static_cast<int>(baseLines.size());

    while (mineIndex < mineHunks.size() || theirIndex < theirHunks.size()) {
        bool startWithMine;
        if (mineIndex >= mineHunks.size()) {
            startWithMine = false;
        } else if (theirIndex >= theirHunks.size()) {
            startWithMine = true;
        } else if (mineHunks[mineIndex].baseStart != theirHunks[theirIndex].baseStart) {
            startWithMine = mineHunks[mineIndex].baseStart < theirHunks[theirIndex].baseStart;
        } else {
            startWithMine = mineHunks[mineIndex].baseCount == 0 || theirHunks[theirIndex].baseCount != 0;
        }
        const DiffHunk& first = startWithMine ? mineHunks[mineIndex] : theirHunks[theirIndex];
        int regionStart = first.baseStart;
        int regionEnd = first.baseStart + first.baseCount;
        std::size_t mineFirst = mineIndex;
        std::size_t theirFirst = theirIndex;
        if (startWithMine)
            ++mineIndex;
        else
            ++theirIndex;
        auto overlaps = [&](const DiffHunk& hunk) {
            bool regionEmpty = regionStart == regionEnd;
            if (hunk.baseCount == 0)
                return regionEmpty ? hunk.baseStart == regionStart : hunk.baseStart > regionStart && hunk.baseStart < regionEnd;
            if (regionEmpty)
                return false;
            return hunk.baseStart < regionEnd;
        };
        bool extended = true;
        while (extended) {
            extended = false;
            if (mineIndex < mineHunks.size() && overlaps(mineHunks[mineIndex])) {
                regionEnd = std::max(regionEnd, mineHunks[mineIndex].baseStart + mineHunks[mineIndex].baseCount);
                ++mineIndex;
                extended = true;
            }
            if (theirIndex < theirHunks.size() && overlaps(theirHunks[theirIndex])) {
                regionEnd = std::max(regionEnd, theirHunks[theirIndex].baseStart + theirHunks[theirIndex].baseCount);
                ++theirIndex;
                extended = true;
            }
        }
        appendRange(output, baseLines, cursor, regionStart - cursor);
        cursor = regionEnd;
        std::vector<DiffHunk> mineRegion(mineHunks.begin() + static_cast<std::ptrdiff_t>(mineFirst), mineHunks.begin() + static_cast<std::ptrdiff_t>(mineIndex));
        std::vector<DiffHunk> theirRegion(theirHunks.begin() + static_cast<std::ptrdiff_t>(theirFirst), theirHunks.begin() + static_cast<std::ptrdiff_t>(theirIndex));
        auto mineSide = sideOfRegion(baseLines, mineLines, mineRegion, regionStart, regionEnd);
        auto theirSide = sideOfRegion(baseLines, theirLines, theirRegion, regionStart, regionEnd);
        if (mineRegion.empty()) {
            output.insert(output.end(), theirSide.begin(), theirSide.end());
        } else if (theirRegion.empty() || mineSide == theirSide) {
            output.insert(output.end(), mineSide.begin(), mineSide.end());
        } else {
            ++result.conflicts;
            output.push_back("<<<<<<< " + std::string(mineLabel));
            output.insert(output.end(), mineSide.begin(), mineSide.end());
            output.push_back("=======");
            output.insert(output.end(), theirSide.begin(), theirSide.end());
            output.push_back(">>>>>>> " + std::string(theirsLabel));
        }
    }
    appendRange(output, baseLines, cursor, baseCount - cursor);
    bool trailing = !theirs.empty() ? endsWithNewline(theirs) : endsWithNewline(mine);
    result.text = joinLines(output, trailing);
    return result;
}

std::string unifiedDiff(std::string_view before, std::string_view after, std::string_view beforeName, std::string_view afterName, int context)
{
    auto a = text::splitLines(before);
    auto b = text::splitLines(after);
    auto hunks = diffLines(a, b);
    if (hunks.empty())
        return {};
    std::string output = "--- " + std::string(beforeName) + "\n+++ " + std::string(afterName) + "\n";
    std::size_t index = 0;
    while (index < hunks.size()) {
        std::size_t last = index;
        while (last + 1 < hunks.size()
               && hunks[last + 1].baseStart - (hunks[last].baseStart + hunks[last].baseCount) <= 2 * context)
            ++last;
        int aFrom = std::max(0, hunks[index].baseStart - context);
        int bFrom = std::max(0, hunks[index].otherStart - context);
        int aTo = std::min(static_cast<int>(a.size()), hunks[last].baseStart + hunks[last].baseCount + context);
        int bTo = std::min(static_cast<int>(b.size()), hunks[last].otherStart + hunks[last].otherCount + context);
        output += "@@ -" + std::to_string(aFrom + 1) + "," + std::to_string(aTo - aFrom) + " +" + std::to_string(bFrom + 1) + "," + std::to_string(bTo - bFrom) + " @@\n";
        int cursor = aFrom;
        for (std::size_t i = index; i <= last; ++i) {
            for (; cursor < hunks[i].baseStart; ++cursor)
                output += " " + a[static_cast<std::size_t>(cursor)] + "\n";
            for (int k = 0; k < hunks[i].baseCount; ++k)
                output += "-" + a[static_cast<std::size_t>(hunks[i].baseStart + k)] + "\n";
            for (int k = 0; k < hunks[i].otherCount; ++k)
                output += "+" + b[static_cast<std::size_t>(hunks[i].otherStart + k)] + "\n";
            cursor = hunks[i].baseStart + hunks[i].baseCount;
        }
        for (; cursor < aTo; ++cursor)
            output += " " + a[static_cast<std::size_t>(cursor)] + "\n";
        index = last + 1;
    }
    return output;
}

int mapLineThroughDiff(const std::vector<DiffHunk>& hunks, int baseLine)
{
    int delta = 0;
    for (const auto& hunk : hunks) {
        if (baseLine < hunk.baseStart)
            break;
        if (baseLine < hunk.baseStart + hunk.baseCount)
            return -1;
        delta += hunk.otherCount - hunk.baseCount;
    }
    return baseLine + delta;
}

LineStats diffStats(std::string_view before, std::string_view after)
{
    LineStats stats;
    for (const auto& hunk : diffLines(text::splitLines(before), text::splitLines(after))) {
        stats.removed += hunk.baseCount;
        stats.added += hunk.otherCount;
    }
    return stats;
}

}
