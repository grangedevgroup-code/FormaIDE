#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace forma::codegen {

struct DiffHunk {
    int baseStart = 0;
    int baseCount = 0;
    int otherStart = 0;
    int otherCount = 0;
};

std::vector<DiffHunk> diffLines(const std::vector<std::string>& base, const std::vector<std::string>& other);

struct MergeResult {
    std::string text;
    int conflicts = 0;
    bool clean() const { return conflicts == 0; }
};

MergeResult mergeThreeWay(std::string_view base, std::string_view mine, std::string_view theirs,
                          std::string_view mineLabel = "your edits", std::string_view theirsLabel = "generated");

std::string unifiedDiff(std::string_view before, std::string_view after, std::string_view beforeName, std::string_view afterName, int context = 3);

int mapLineThroughDiff(const std::vector<DiffHunk>& hunks, int baseLine);

struct LineStats {
    int added = 0;
    int removed = 0;
};

LineStats diffStats(std::string_view before, std::string_view after);

}
