#include "forma/core/text.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace forma::text {

namespace {

bool isAlpha(unsigned char c) { return std::isalpha(c) != 0; }
bool isDigit(unsigned char c) { return std::isdigit(c) != 0; }
bool isAlnum(unsigned char c) { return std::isalnum(c) != 0; }
bool isUpper(unsigned char c) { return std::isupper(c) != 0; }
bool isLower(unsigned char c) { return std::islower(c) != 0; }

}

std::vector<std::string> split(std::string_view input, char separator)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        std::size_t end = input.find(separator, start);
        if (end == std::string_view::npos) {
            parts.emplace_back(input.substr(start));
            break;
        }
        parts.emplace_back(input.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

std::vector<std::string> splitLines(std::string_view input)
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < input.size()) {
        std::size_t end = input.find('\n', start);
        if (end == std::string_view::npos) {
            lines.emplace_back(input.substr(start));
            break;
        }
        std::size_t stop = end;
        if (stop > start && input[stop - 1] == '\r')
            --stop;
        lines.emplace_back(input.substr(start, stop - start));
        start = end + 1;
    }
    return lines;
}

std::string join(const std::vector<std::string>& parts, std::string_view separator)
{
    std::string output;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0)
            output.append(separator);
        output.append(parts[i]);
    }
    return output;
}

std::string trim(std::string_view input)
{
    std::size_t begin = 0;
    std::size_t end = input.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(input[begin])))
        ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])))
        --end;
    return std::string(input.substr(begin, end - begin));
}

std::string toLower(std::string_view input)
{
    std::string output(input);
    std::transform(output.begin(), output.end(), output.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return output;
}

std::string toUpper(std::string_view input)
{
    std::string output(input);
    std::transform(output.begin(), output.end(), output.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return output;
}

std::string replaceAll(std::string input, std::string_view from, std::string_view to)
{
    if (from.empty())
        return input;
    std::size_t position = 0;
    while ((position = input.find(from, position)) != std::string::npos) {
        input.replace(position, from.size(), to);
        position += to.size();
    }
    return input;
}

bool startsWith(std::string_view input, std::string_view prefix)
{
    return input.size() >= prefix.size() && input.substr(0, prefix.size()) == prefix;
}

bool endsWith(std::string_view input, std::string_view suffix)
{
    return input.size() >= suffix.size() && input.substr(input.size() - suffix.size()) == suffix;
}

bool contains(std::string_view input, std::string_view needle)
{
    return input.find(needle) != std::string_view::npos;
}

bool containsIgnoreCase(std::string_view input, std::string_view needle)
{
    return contains(toLower(input), toLower(needle));
}

std::vector<std::string> identifierWords(std::string_view input)
{
    std::vector<std::string> words;
    std::string word;
    auto flush = [&] {
        if (!word.empty()) {
            words.push_back(word);
            word.clear();
        }
    };
    for (std::size_t i = 0; i < input.size(); ++i) {
        auto c = static_cast<unsigned char>(input[i]);
        if (!isAlnum(c)) {
            flush();
            continue;
        }
        bool boundary = false;
        if (!word.empty()) {
            auto previous = static_cast<unsigned char>(word.back());
            if (isLower(previous) && isUpper(c))
                boundary = true;
            if (isUpper(previous) && isUpper(c) && i + 1 < input.size()
                && isLower(static_cast<unsigned char>(input[i + 1])))
                boundary = true;
        }
        if (boundary)
            flush();
        word.push_back(static_cast<char>(c));
    }
    flush();
    return words;
}

std::string toPascalCase(std::string_view input)
{
    std::string output;
    for (auto word : identifierWords(input)) {
        word = toLower(word);
        word[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(word[0])));
        output += word;
    }
    if (output.empty())
        output = "Unnamed";
    if (isDigit(static_cast<unsigned char>(output[0])))
        output.insert(output.begin(), 'N');
    return output;
}

std::string toCamelCase(std::string_view input)
{
    std::string output = toPascalCase(input);
    output[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(output[0])));
    return output;
}

std::string toSnakeCase(std::string_view input)
{
    auto words = identifierWords(input);
    for (auto& word : words)
        word = toLower(word);
    std::string output = join(words, "_");
    if (output.empty())
        output = "unnamed";
    if (isDigit(static_cast<unsigned char>(output[0])))
        output.insert(output.begin(), 'n');
    return output;
}

std::string toKebabCase(std::string_view input)
{
    return replaceAll(toSnakeCase(input), "_", "-");
}

std::string toScreamingSnake(std::string_view input)
{
    return toUpper(toSnakeCase(input));
}

std::string toIdentifier(std::string_view input)
{
    std::string output;
    for (char ch : input) {
        auto c = static_cast<unsigned char>(ch);
        output.push_back(isAlnum(c) || c == '_' ? static_cast<char>(c) : '_');
    }
    if (output.empty() || isDigit(static_cast<unsigned char>(output[0])))
        output.insert(output.begin(), '_');
    return output;
}

bool isIdentifier(std::string_view input)
{
    if (input.empty())
        return false;
    auto first = static_cast<unsigned char>(input[0]);
    if (!isAlpha(first) && first != '_')
        return false;
    return std::all_of(input.begin(), input.end(), [](unsigned char c) { return isAlnum(c) || c == '_'; });
}

std::string formatNumber(double value)
{
    if (std::abs(value - std::round(value)) < 1e-9) {
        char buffer[32];
        std::snprintf(buffer, sizeof buffer, "%lld", static_cast<long long>(std::llround(value)));
        return buffer;
    }
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.3f", value);
    std::string output(buffer);
    while (!output.empty() && output.back() == '0')
        output.pop_back();
    if (!output.empty() && output.back() == '.')
        output.pop_back();
    return output;
}

}
