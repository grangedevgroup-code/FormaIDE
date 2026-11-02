#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "forma/core/result.h"

namespace forma::terminal {

struct Cell {
    char32_t character = U' ';
    std::uint32_t foreground = 0;
    std::uint32_t background = 0;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool inverse = false;
    bool wide = false;
};

constexpr std::uint32_t kDefaultColor = 0;
constexpr std::uint32_t kPaletteFlag = 0x01000000;
constexpr std::uint32_t kRgbFlag = 0x02000000;

class Screen {
public:
    Screen(int columns, int rows, int scrollback = 10000);

    void feed(std::string_view bytes);
    void resize(int columns, int rows);
    void reset();

    int columns() const { return columns_; }
    int rows() const { return rows_; }
    int cursorColumn() const { return cursorColumn_; }
    int cursorRow() const { return cursorRow_; }
    bool cursorVisible() const { return cursorVisible_; }
    bool alternateActive() const { return alternateActive_; }
    bool applicationCursorKeys() const { return applicationCursorKeys_; }
    bool bracketedPaste() const { return bracketedPaste_; }
    const std::string& title() const { return title_; }

    int scrollbackSize() const { return static_cast<int>(scrollback_.size()); }
    const std::vector<Cell>& line(int row) const;
    const std::vector<Cell>& scrollbackLine(int index) const;
    std::string lineText(int row) const;
    std::string text() const;

    std::function<void(std::string_view)> onReply;

private:
    using Line = std::vector<Cell>;

    void execute(char32_t code);
    void print(char32_t code);
    void newline();
    void reverseIndex();
    void scrollUp(int top, int bottom, int count);
    void scrollDown(int top, int bottom, int count);
    void eraseInLine(int mode);
    void eraseInDisplay(int mode);
    void csi(char final);
    void osc();
    void setMode(bool enable);
    void applySgr();
    void clampCursor();
    Cell blankCell() const;
    Line blankLine() const;
    int parameter(std::size_t index, int fallback) const;
    void switchAlternate(bool enable);

    enum class State { Ground, Escape, EscapeIntermediate, Csi, Osc, OscEscape, Charset };

    int columns_;
    int rows_;
    int scrollbackLimit_;
    std::vector<Line> lines_;
    std::vector<Line> savedPrimary_;
    std::deque<Line> scrollback_;
    int cursorColumn_ = 0;
    int cursorRow_ = 0;
    int savedColumn_ = 0;
    int savedRow_ = 0;
    int scrollTop_ = 0;
    int scrollBottom_ = 0;
    bool wrapPending_ = false;
    bool autoWrap_ = true;
    bool cursorVisible_ = true;
    bool alternateActive_ = false;
    bool applicationCursorKeys_ = false;
    bool bracketedPaste_ = false;
    bool originMode_ = false;
    Cell attributes_;
    State state_ = State::Ground;
    std::vector<int> parameters_;
    int currentParameter_ = -1;
    bool privateMarker_ = false;
    std::string oscBuffer_;
    std::string title_;
    std::string utf8Pending_;
    char32_t lastPrinted_ = U' ';
};

class Pty {
public:
    struct Options {
        std::string program;
        std::vector<std::string> arguments;
        std::filesystem::path workingDirectory;
        std::map<std::string, std::string> environment;
        int columns = 120;
        int rows = 30;
    };

    static Result<std::unique_ptr<Pty>> start(const Options& options, std::function<void(std::string_view)> onOutput, std::function<void(int)> onExit);
    ~Pty();

    Pty(const Pty&) = delete;
    Pty& operator=(const Pty&) = delete;

    Status write(std::string_view data);
    void resize(int columns, int rows);
    void terminate();
    bool running() const;
    std::optional<int> exitCode() const;

    struct Impl;

private:
    explicit Pty(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

struct ShellInfo {
    std::string id;
    std::string name;
    std::string program;
    std::vector<std::string> arguments;
};

std::vector<ShellInfo> availableShells();

std::string encodeKey(int key, bool control, bool alt, bool shift, bool applicationCursorKeys);

}
