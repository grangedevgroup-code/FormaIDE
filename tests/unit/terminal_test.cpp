#include <gtest/gtest.h>

#include <mutex>

#include "forma/terminal/terminal.h"
#include "test_support.h"

using namespace forma::terminal;
using forma::testing::waitUntil;

TEST(TerminalScreen, PrintsAndWraps)
{
    Screen screen(5, 3);
    screen.feed("abcdefg");
    EXPECT_EQ(screen.lineText(0), "abcde");
    EXPECT_EQ(screen.lineText(1), "fg");
    EXPECT_EQ(screen.cursorRow(), 1);
    EXPECT_EQ(screen.cursorColumn(), 2);
}

TEST(TerminalScreen, CarriageReturnAndNewline)
{
    Screen screen(10, 3);
    screen.feed("one\r\ntwo\rT");
    EXPECT_EQ(screen.lineText(0), "one");
    EXPECT_EQ(screen.lineText(1), "Two");
}

TEST(TerminalScreen, ScrollsIntoScrollback)
{
    Screen screen(10, 2);
    screen.feed("1\r\n2\r\n3\r\n4");
    EXPECT_EQ(screen.scrollbackSize(), 2);
    EXPECT_EQ(screen.lineText(0), "3");
    EXPECT_EQ(screen.lineText(1), "4");
}

TEST(TerminalScreen, CursorMovementAndErase)
{
    Screen screen(10, 3);
    screen.feed("hello\x1b[2;3Hx");
    EXPECT_EQ(screen.lineText(1), "  x");
    screen.feed("\x1b[1;1H\x1b[K");
    EXPECT_EQ(screen.lineText(0), "");
    screen.feed("\x1b[2J");
    EXPECT_EQ(screen.lineText(1), "");
}

TEST(TerminalScreen, ColorsAndAttributes)
{
    Screen screen(10, 2);
    screen.feed("\x1b[1;31mR\x1b[0m\x1b[38;5;200mP\x1b[48;2;1;2;3mB");
    const auto& cells = screen.line(0);
    EXPECT_TRUE(cells[0].bold);
    EXPECT_EQ(cells[0].foreground, kPaletteFlag | 1u);
    EXPECT_FALSE(cells[1].bold);
    EXPECT_EQ(cells[1].foreground, kPaletteFlag | 200u);
    EXPECT_EQ(cells[2].background, kRgbFlag | 0x010203u);
}

TEST(TerminalScreen, AlternateScreenRestoresContent)
{
    Screen screen(10, 2);
    screen.feed("keep");
    screen.feed("\x1b[?1049h");
    EXPECT_TRUE(screen.alternateActive());
    screen.feed("other");
    screen.feed("\x1b[?1049l");
    EXPECT_FALSE(screen.alternateActive());
    EXPECT_EQ(screen.lineText(0), "keep");
}

TEST(TerminalScreen, ScrollRegion)
{
    Screen screen(5, 4);
    screen.feed("a\r\nb\r\nc\r\nd");
    screen.feed("\x1b[2;3r\x1b[3;1H\n");
    EXPECT_EQ(screen.lineText(0), "a");
    EXPECT_EQ(screen.lineText(1), "c");
    EXPECT_EQ(screen.lineText(2), "");
    EXPECT_EQ(screen.lineText(3), "d");
}

TEST(TerminalScreen, Utf8AcrossChunks)
{
    Screen screen(10, 2);
    std::string text = "\xE2\x82\xAC!";
    screen.feed(text.substr(0, 1));
    screen.feed(text.substr(1));
    EXPECT_EQ(screen.line(0)[0].character, U'\x20AC');
    EXPECT_EQ(screen.lineText(0), text);
}

TEST(TerminalScreen, TitleAndModes)
{
    Screen screen(10, 2);
    screen.feed("\x1b]0;my title\x07\x1b[?25l\x1b[?2004h\x1b[?1h");
    EXPECT_EQ(screen.title(), "my title");
    EXPECT_FALSE(screen.cursorVisible());
    EXPECT_TRUE(screen.bracketedPaste());
    EXPECT_TRUE(screen.applicationCursorKeys());
}

TEST(TerminalScreen, AnswersCursorQueries)
{
    Screen screen(10, 3);
    std::string reply;
    screen.onReply = [&](std::string_view data) { reply.append(data); };
    screen.feed("\x1b[2;4H\x1b[6n");
    EXPECT_EQ(reply, "\x1b[2;4R");
}

TEST(TerminalScreen, ResizeKeepsContent)
{
    Screen screen(10, 3);
    screen.feed("one\r\ntwo\r\nthree");
    screen.resize(20, 2);
    EXPECT_EQ(screen.lineText(1), "three");
    EXPECT_EQ(screen.scrollbackSize(), 1);
    screen.resize(5, 4);
    EXPECT_EQ(screen.columns(), 5);
    EXPECT_EQ(screen.rows(), 4);
}

TEST(TerminalKeys, EncodesSpecialKeys)
{
    EXPECT_EQ(encodeKey(0x01000013, false, false, false, false), "\x1b[A");
    EXPECT_EQ(encodeKey(0x01000013, false, false, false, true), "\x1bOA");
    EXPECT_EQ(encodeKey(0x01000004, false, false, false, false), "\r");
    EXPECT_EQ(encodeKey('C', true, false, false, false), "\x03");
    EXPECT_EQ(encodeKey(0x01000007, false, false, false, false), "\x1b[3~");
    EXPECT_EQ(encodeKey('a', false, false, false, false), "");
}

TEST(TerminalPty, RunsAShellCommand)
{
    Pty::Options options;
#if defined(_WIN32)
    options.program = "cmd.exe";
    options.arguments = {"/d", "/c", "echo forma-terminal-ok"};
#else
    options.program = "/bin/sh";
    options.arguments = {"-c", "echo forma-terminal-ok"};
#endif
    std::mutex mutex;
    std::string output;
    auto pty = Pty::start(options, [&](std::string_view data) {
        std::lock_guard lock(mutex);
        output.append(data);
    }, {});
    ASSERT_TRUE(pty.ok()) << pty.error().message;
    EXPECT_TRUE(waitUntil([&] {
        std::lock_guard lock(mutex);
        return output.find("forma-terminal-ok") != std::string::npos;
    }));
    EXPECT_TRUE(waitUntil([&] { return !(*pty)->running(); }));
}

TEST(TerminalPty, InteractiveInputAndResize)
{
    Pty::Options options;
#if defined(_WIN32)
    options.program = "cmd.exe";
    options.arguments = {"/d"};
#else
    options.program = "/bin/sh";
#endif
    std::mutex mutex;
    std::string output;
    auto pty = Pty::start(options, [&](std::string_view data) {
        std::lock_guard lock(mutex);
        output.append(data);
    }, {});
    ASSERT_TRUE(pty.ok());
    (*pty)->resize(100, 20);
    ASSERT_TRUE((*pty)->write("echo typed-input-ok\r").ok());
    EXPECT_TRUE(waitUntil([&] {
        std::lock_guard lock(mutex);
        std::size_t first = output.find("typed-input-ok");
        return first != std::string::npos && output.find("typed-input-ok", first + 1) != std::string::npos;
    }));
    (*pty)->terminate();
    EXPECT_TRUE(waitUntil([&] { return !(*pty)->running(); }));
}

TEST(TerminalShells, FindsAtLeastOne)
{
    EXPECT_FALSE(availableShells().empty());
}
