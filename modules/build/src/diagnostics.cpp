#include <regex>

#include "forma/build/build_system.h"
#include "forma/core/text.h"

namespace forma::build {

namespace {

DiagnosticSeverity severityFromWord(std::string word)
{
    word = text::toLower(word);
    if (word == "warning" || word == "warn" || word == "w")
        return DiagnosticSeverity::Warning;
    if (word == "note" || word == "info" || word == "hint")
        return DiagnosticSeverity::Note;
    return DiagnosticSeverity::Error;
}

int toInt(const std::string& value)
{
    try {
        return value.empty() ? 0 : std::stoi(value);
    } catch (const std::exception&) {
        return 0;
    }
}

struct Pattern {
    std::regex expression;
    int file;
    int line;
    int column;
    int severity;
    int code;
    int message;
    const char* tool;
};

const std::vector<Pattern>& patterns()
{
    static const std::vector<Pattern> list = {
        {std::regex(R"(^\s*(.+?)\((\d+)(?:,(\d+))?\)\s*:\s*(?:fatal\s+)?(error|warning|note|info)\s+([A-Za-z]+\d+)\s*:\s*(.*)$)"), 1, 2, 3, 4, 5, 6, "msvc"},
        {std::regex(R"(^\s*(.+?)\((\d+)(?:,(\d+))?\)\s*:\s*(?:fatal\s+)?(error|warning|note)\s*:\s*(.*)$)"), 1, 2, 3, 4, 0, 5, "msvc"},
        {std::regex(R"(^\s*([eEwW]):\s+file:///?(.+?):(\d+):(\d+)\s+(.*)$)"), 2, 3, 4, 1, 0, 5, "kotlin"},
        {std::regex(R"(^\s*([eEwW]):\s+(.+?):\s*\((\d+),\s*(\d+)\):\s*(.*)$)"), 2, 3, 4, 1, 0, 5, "kotlin"},
        {std::regex(R"(^\s*(?:CMake\s+)?(Error|Warning)(?:\s+\([^)]*\))?\s+at\s+(.+?):(\d+)(?:\s+\(.*\))?:?\s*(.*)$)"), 2, 3, 0, 1, 0, 4, "cmake"},
        {std::regex(R"(^\s*(.+?):(\d+):(\d+):\s*(?:fatal\s+)?(error|warning|note)(?:\[([^\]]+)\])?:\s*(.*)$)"), 1, 2, 3, 4, 5, 6, "gcc"},
        {std::regex(R"(^\s*(.+?):(\d+):\s*(?:fatal\s+)?(error|warning|note):\s*(.*)$)"), 1, 2, 0, 3, 0, 4, "gcc"},
        {std::regex(R"(^\s*(.+?):(\d+):(\d+):\s*(Error|Warning|Info):\s*(.*)$)"), 1, 2, 3, 4, 0, 5, "dart"},
        {std::regex(R"(^\s*(.+?\.go):(\d+):(\d+):\s*(.*)$)"), 1, 2, 3, 0, 0, 4, "go"},
        {std::regex(R"(^\s*File \"(.+?)\", line (\d+).*$)"), 1, 2, 0, 0, 0, 0, "python"},
    };
    return list;
}

}

std::optional<BuildDiagnostic> DiagnosticParser::parseLine(std::string_view input)
{
    std::string line(input);
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    if (line.size() > 4000 || line.empty())
        return std::nullopt;

    static const std::regex rustHead(R"(^(error|warning)(?:\[([A-Z0-9]+)\])?:\s*(.*)$)");
    static const std::regex rustPointer(R"(^\s*-->\s*(.+?):(\d+):(\d+)\s*$)");
    std::smatch match;
    if (std::regex_match(line, match, rustPointer) && hasPendingRust_) {
        BuildDiagnostic diagnostic;
        diagnostic.severity = pendingRustSeverity_;
        diagnostic.file = match[1];
        diagnostic.line = toInt(match[2]);
        diagnostic.column = toInt(match[3]);
        diagnostic.code = pendingRustCode_;
        diagnostic.message = pendingRustMessage_;
        diagnostic.tool = "rust";
        hasPendingRust_ = false;
        return diagnostic;
    }
    if (std::regex_match(line, match, rustHead)) {
        pendingRustSeverity_ = severityFromWord(match[1]);
        pendingRustCode_ = match[2];
        pendingRustMessage_ = match[3];
        hasPendingRust_ = true;
    }

    bool candidate = text::containsIgnoreCase(line, "error") || text::containsIgnoreCase(line, "warning") || text::containsIgnoreCase(line, "note")
        || text::contains(line, "e: ") || text::contains(line, "w: ") || text::contains(line, ".go:") || text::contains(line, "File \"");
    if (!candidate)
        return std::nullopt;

    for (const auto& pattern : patterns()) {
        if (!std::regex_match(line, match, pattern.expression))
            continue;
        BuildDiagnostic diagnostic;
        diagnostic.tool = pattern.tool;
        diagnostic.file = pattern.file > 0 ? std::string(match[static_cast<std::size_t>(pattern.file)]) : std::string();
        diagnostic.line = pattern.line > 0 ? toInt(match[static_cast<std::size_t>(pattern.line)]) : 0;
        diagnostic.column = pattern.column > 0 ? toInt(match[static_cast<std::size_t>(pattern.column)]) : 0;
        diagnostic.severity = pattern.severity > 0 ? severityFromWord(match[static_cast<std::size_t>(pattern.severity)]) : DiagnosticSeverity::Error;
        diagnostic.code = pattern.code > 0 ? std::string(match[static_cast<std::size_t>(pattern.code)]) : std::string();
        diagnostic.message = pattern.message > 0 ? std::string(match[static_cast<std::size_t>(pattern.message)]) : std::string("Exception at this line");
        auto bracket = diagnostic.message.rfind(" [");
        if (bracket != std::string::npos && !diagnostic.message.empty() && diagnostic.message.back() == ']'
            && text::contains(diagnostic.message.substr(bracket), "proj"))
            diagnostic.message.erase(bracket);
        if (diagnostic.file.find("http") == 0)
            return std::nullopt;
        return diagnostic;
    }
    return std::nullopt;
}

}
