#include <gtest/gtest.h>

#include "forma/core/hash.h"
#include "forma/core/json.h"
#include "forma/core/text.h"

using namespace forma;

TEST(Hash, MatchesKnownSha256Vectors)
{
    EXPECT_EQ(sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT_EQ(sha256Hex(std::string(1000000, 'a')), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Text, CaseConversions)
{
    EXPECT_EQ(text::toPascalCase("screen_home"), "ScreenHome");
    EXPECT_EQ(text::toPascalCase("continue button"), "ContinueButton");
    EXPECT_EQ(text::toCamelCase("Continue Button"), "continueButton");
    EXPECT_EQ(text::toSnakeCase("ContinueButton"), "continue_button");
    EXPECT_EQ(text::toKebabCase("HTTPServer"), "http-server");
    EXPECT_EQ(text::toScreamingSnake("primaryColor"), "PRIMARY_COLOR");
    EXPECT_EQ(text::toPascalCase("123 go"), "N123Go");
    EXPECT_EQ(text::toPascalCase(""), "Unnamed");
}

TEST(Text, SplitLinesHandlesEveryLineEnding)
{
    auto lines = text::splitLines("a\r\nb\nc");
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0], "a");
    EXPECT_EQ(lines[1], "b");
    EXPECT_EQ(lines[2], "c");
    EXPECT_TRUE(text::splitLines("").empty());
}

TEST(Text, FormatNumber)
{
    EXPECT_EQ(text::formatNumber(16), "16");
    EXPECT_EQ(text::formatNumber(1.5), "1.5");
    EXPECT_EQ(text::formatNumber(0.125), "0.125");
    EXPECT_EQ(text::formatNumber(-3), "-3");
}

TEST(Json, PathHelpers)
{
    Json root = Json::object();
    jsonSetPath(root, "style.background", "#fff");
    jsonSetPath(root, "style.radius", 8);
    ASSERT_NE(jsonFindPath(root, "style.radius"), nullptr);
    EXPECT_EQ(jsonFindPath(root, "style.radius")->get<int>(), 8);
    jsonErasePath(root, "style.radius");
    EXPECT_EQ(jsonFindPath(root, "style.radius"), nullptr);
    jsonErasePath(root, "style.background");
    EXPECT_FALSE(root.contains("style"));
}

TEST(Json, CborRoundTrip)
{
    Json value = {{"name", "Home"}, {"count", 3}, {"items", {1, 2, 3}}};
    auto parsed = fromCbor(toCbor(value));
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(*parsed, value);
}

TEST(Json, ParseErrorIsReported)
{
    auto parsed = parseJson("{ not json");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.error().code, "json.parse");
}
