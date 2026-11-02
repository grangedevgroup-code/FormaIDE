#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace forma::text {

std::vector<std::string> split(std::string_view input, char separator);
std::vector<std::string> splitLines(std::string_view input);
std::string join(const std::vector<std::string>& parts, std::string_view separator);
std::string trim(std::string_view input);
std::string toLower(std::string_view input);
std::string toUpper(std::string_view input);
std::string replaceAll(std::string input, std::string_view from, std::string_view to);
bool startsWith(std::string_view input, std::string_view prefix);
bool endsWith(std::string_view input, std::string_view suffix);
bool contains(std::string_view input, std::string_view needle);
bool containsIgnoreCase(std::string_view input, std::string_view needle);

std::vector<std::string> identifierWords(std::string_view input);
std::string toPascalCase(std::string_view input);
std::string toCamelCase(std::string_view input);
std::string toSnakeCase(std::string_view input);
std::string toKebabCase(std::string_view input);
std::string toScreamingSnake(std::string_view input);
std::string toIdentifier(std::string_view input);
bool isIdentifier(std::string_view input);

std::string formatNumber(double value);

}
