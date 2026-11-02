#include <algorithm>

#include "forma/terminal/terminal.h"

namespace forma::terminal {

namespace {

bool isWide(char32_t c)
{
    return (c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF) || (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF)
        || (c >= 0xFE30 && c <= 0xFE6F) || (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1FAFF)
        || (c >= 0x20000 && c <= 0x3FFFD);
}

bool isCombining(char32_t c)
{
    return (c >= 0x0300 && c <= 0x036F) || (c >= 0x200B && c <= 0x200F) || (c >= 0xFE00 && c <= 0xFE0F);
}

void appendUtf8(std::string& output, char32_t c)
{
    if (c < 0x80) {
        output.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        output.push_back(static_cast<char>(0xC0 | (c >> 6)));
        output.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        output.push_back(static_cast<char>(0xE0 | (c >> 12)));
        output.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | (c >> 18)));
        output.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

}

Screen::Screen(int columns, int rows, int scrollback) : columns_(std::max(1, columns)), rows_(std::max(1, rows)), scrollbackLimit_(scrollback)
{
    reset();
}

void Screen::reset()
{
    attributes_ = Cell{};
    lines_.assign(static_cast<std::size_t>(rows_), blankLine());
    savedPrimary_.clear();
    cursorColumn_ = 0;
    cursorRow_ = 0;
    savedColumn_ = 0;
    savedRow_ = 0;
    scrollTop_ = 0;
    scrollBottom_ = rows_ - 1;
    wrapPending_ = false;
    autoWrap_ = true;
    cursorVisible_ = true;
    alternateActive_ = false;
    applicationCursorKeys_ = false;
    bracketedPaste_ = false;
    originMode_ = false;
    state_ = State::Ground;
}

Cell Screen::blankCell() const
{
    Cell cell;
    cell.background = attributes_.background;
    return cell;
}

Screen::Line Screen::blankLine() const
{
    return Line(static_cast<std::size_t>(columns_), blankCell());
}

void Screen::resize(int columns, int rows)
{
    columns = std::max(1, columns);
    rows = std::max(1, rows);
    if (columns == columns_ && rows == rows_)
        return;
    columns_ = columns;
    if (rows < rows_) {
        int excess = rows_ - rows;
        int blankBelow = rows_ - 1 - cursorRow_;
        int fromBottom = std::min(excess, blankBelow);
        if (fromBottom > 0)
            lines_.resize(lines_.size() - static_cast<std::size_t>(fromBottom));
        int fromTop = excess - fromBottom;
        for (int i = 0; i < fromTop; ++i) {
            if (!alternateActive_) {
                scrollback_.push_back(lines_.front());
                if (static_cast<int>(scrollback_.size()) > scrollbackLimit_)
                    scrollback_.pop_front();
            }
            lines_.erase(lines_.begin());
        }
        cursorRow_ = std::max(0, cursorRow_ - fromTop);
    } else {
        while (static_cast<int>(lines_.size()) < rows)
            lines_.push_back(blankLine());
    }
    rows_ = rows;
    for (auto& line : lines_)
        line.resize(static_cast<std::size_t>(columns_), blankCell());
    for (auto& line : savedPrimary_)
        line.resize(static_cast<std::size_t>(columns_), blankCell());
    savedPrimary_.resize(savedPrimary_.empty() ? 0 : static_cast<std::size_t>(rows_), blankLine());
    scrollTop_ = 0;
    scrollBottom_ = rows_ - 1;
    wrapPending_ = false;
    clampCursor();
}

const std::vector<Cell>& Screen::line(int row) const
{
    return lines_[static_cast<std::size_t>(std::clamp(row, 0, rows_ - 1))];
}

const std::vector<Cell>& Screen::scrollbackLine(int index) const
{
    return scrollback_[static_cast<std::size_t>(index)];
}

std::string Screen::lineText(int row) const
{
    std::string output;
    const auto& cells = line(row);
    int last = static_cast<int>(cells.size()) - 1;
    while (last >= 0 && cells[static_cast<std::size_t>(last)].character == U' ')
        --last;
    for (int i = 0; i <= last; ++i) {
        const Cell& cell = cells[static_cast<std::size_t>(i)];
        if (cell.character != 0)
            appendUtf8(output, cell.character);
    }
    return output;
}

std::string Screen::text() const
{
    std::string output;
    for (int row = 0; row < rows_; ++row) {
        output += lineText(row);
        output.push_back('\n');
    }
    return output;
}

void Screen::clampCursor()
{
    cursorColumn_ = std::clamp(cursorColumn_, 0, columns_ - 1);
    cursorRow_ = std::clamp(cursorRow_, 0, rows_ - 1);
}

void Screen::feed(std::string_view bytes)
{
    for (char byte : bytes) {
        auto b = static_cast<unsigned char>(byte);
        if (state_ == State::Ground || state_ == State::Osc) {
            if (!utf8Pending_.empty() || b >= 0x80) {
                if (b < 0x80) {
                    utf8Pending_.clear();
                } else {
                    utf8Pending_.push_back(byte);
                    auto first = static_cast<unsigned char>(utf8Pending_[0]);
                    std::size_t need = first >= 0xF0 ? 4 : first >= 0xE0 ? 3 : first >= 0xC0 ? 2 : 1;
                    if (utf8Pending_.size() < need)
                        continue;
                    char32_t code = 0xFFFD;
                    if (need == 2)
                        code = ((first & 0x1Fu) << 6) | (static_cast<unsigned char>(utf8Pending_[1]) & 0x3Fu);
                    else if (need == 3)
                        code = ((first & 0x0Fu) << 12) | ((static_cast<unsigned char>(utf8Pending_[1]) & 0x3Fu) << 6) | (static_cast<unsigned char>(utf8Pending_[2]) & 0x3Fu);
                    else if (need == 4)
                        code = ((first & 0x07u) << 18) | ((static_cast<unsigned char>(utf8Pending_[1]) & 0x3Fu) << 12) | ((static_cast<unsigned char>(utf8Pending_[2]) & 0x3Fu) << 6) | (static_cast<unsigned char>(utf8Pending_[3]) & 0x3Fu);
                    if (state_ == State::Osc)
                        oscBuffer_ += utf8Pending_;
                    else
                        print(code);
                    utf8Pending_.clear();
                    continue;
                }
            }
        }
        switch (state_) {
        case State::Ground:
            if (b == 0x1B)
                state_ = State::Escape;
            else if (b < 0x20 || b == 0x7F)
                execute(b);
            else
                print(b);
            break;
        case State::Escape:
            parameters_.clear();
            currentParameter_ = -1;
            privateMarker_ = false;
            if (b == '[') {
                state_ = State::Csi;
            } else if (b == ']') {
                oscBuffer_.clear();
                state_ = State::Osc;
            } else if (b == '(' || b == ')' || b == '*' || b == '+') {
                state_ = State::Charset;
            } else if (b == '#' || b == ' ' || b == '%') {
                state_ = State::EscapeIntermediate;
            } else {
                state_ = State::Ground;
                switch (b) {
                case 'D':
                    newline();
                    break;
                case 'E':
                    cursorColumn_ = 0;
                    newline();
                    break;
                case 'M':
                    reverseIndex();
                    break;
                case '7':
                    savedColumn_ = cursorColumn_;
                    savedRow_ = cursorRow_;
                    break;
                case '8':
                    cursorColumn_ = savedColumn_;
                    cursorRow_ = savedRow_;
                    clampCursor();
                    wrapPending_ = false;
                    break;
                case 'c':
                    reset();
                    break;
                default:
                    break;
                }
            }
            break;
        case State::EscapeIntermediate:
        case State::Charset:
            state_ = State::Ground;
            break;
        case State::Csi:
            if (b >= '0' && b <= '9') {
                currentParameter_ = (currentParameter_ < 0 ? 0 : currentParameter_) * 10 + (b - '0');
            } else if (b == ';' || b == ':') {
                parameters_.push_back(currentParameter_);
                currentParameter_ = -1;
            } else if (b == '?' || b == '>' || b == '<' || b == '=') {
                privateMarker_ = true;
            } else if (b >= 0x40 && b <= 0x7E) {
                parameters_.push_back(currentParameter_);
                currentParameter_ = -1;
                csi(static_cast<char>(b));
                state_ = State::Ground;
            } else if (b == 0x1B) {
                state_ = State::Escape;
            } else if (b < 0x20) {
                execute(b);
            }
            break;
        case State::Osc:
            if (b == 0x07) {
                osc();
                state_ = State::Ground;
            } else if (b == 0x1B) {
                state_ = State::OscEscape;
            } else {
                oscBuffer_.push_back(byte);
                if (oscBuffer_.size() > 65536)
                    state_ = State::Ground;
            }
            break;
        case State::OscEscape:
            osc();
            state_ = b == '\\' ? State::Ground : State::Escape;
            break;
        }
    }
}

void Screen::execute(char32_t code)
{
    switch (code) {
    case 0x08:
        if (cursorColumn_ > 0)
            --cursorColumn_;
        wrapPending_ = false;
        break;
    case 0x09:
        cursorColumn_ = std::min(columns_ - 1, (cursorColumn_ / 8 + 1) * 8);
        break;
    case 0x0A:
    case 0x0B:
    case 0x0C:
        newline();
        break;
    case 0x0D:
        cursorColumn_ = 0;
        wrapPending_ = false;
        break;
    default:
        break;
    }
}

void Screen::print(char32_t code)
{
    if (isCombining(code))
        return;
    bool wide = isWide(code);
    if (wrapPending_ && autoWrap_) {
        cursorColumn_ = 0;
        newline();
        wrapPending_ = false;
    }
    if (wide && cursorColumn_ == columns_ - 1) {
        if (autoWrap_) {
            cursorColumn_ = 0;
            newline();
        }
    }
    Cell cell = attributes_;
    cell.character = code;
    cell.wide = wide;
    auto& row = lines_[static_cast<std::size_t>(cursorRow_)];
    row[static_cast<std::size_t>(cursorColumn_)] = cell;
    lastPrinted_ = code;
    if (wide && cursorColumn_ + 1 < columns_) {
        Cell filler = attributes_;
        filler.character = 0;
        row[static_cast<std::size_t>(cursorColumn_ + 1)] = filler;
    }
    int step = wide ? 2 : 1;
    if (cursorColumn_ + step >= columns_) {
        cursorColumn_ = columns_ - 1;
        wrapPending_ = true;
    } else {
        cursorColumn_ += step;
    }
}

void Screen::newline()
{
    wrapPending_ = false;
    if (cursorRow_ == scrollBottom_)
        scrollUp(scrollTop_, scrollBottom_, 1);
    else if (cursorRow_ < rows_ - 1)
        ++cursorRow_;
}

void Screen::reverseIndex()
{
    if (cursorRow_ == scrollTop_)
        scrollDown(scrollTop_, scrollBottom_, 1);
    else if (cursorRow_ > 0)
        --cursorRow_;
}

void Screen::scrollUp(int top, int bottom, int count)
{
    count = std::min(count, bottom - top + 1);
    for (int i = 0; i < count; ++i) {
        if (top == 0 && !alternateActive_) {
            scrollback_.push_back(std::move(lines_[static_cast<std::size_t>(top)]));
            if (static_cast<int>(scrollback_.size()) > scrollbackLimit_)
                scrollback_.pop_front();
        }
        lines_.erase(lines_.begin() + top);
        lines_.insert(lines_.begin() + bottom, blankLine());
    }
}

void Screen::scrollDown(int top, int bottom, int count)
{
    count = std::min(count, bottom - top + 1);
    for (int i = 0; i < count; ++i) {
        lines_.erase(lines_.begin() + bottom);
        lines_.insert(lines_.begin() + top, blankLine());
    }
}

void Screen::eraseInLine(int mode)
{
    auto& row = lines_[static_cast<std::size_t>(cursorRow_)];
    int from = mode == 0 ? cursorColumn_ : 0;
    int to = mode == 1 ? cursorColumn_ + 1 : columns_;
    for (int i = from; i < to && i < columns_; ++i)
        row[static_cast<std::size_t>(i)] = blankCell();
}

void Screen::eraseInDisplay(int mode)
{
    if (mode == 3) {
        scrollback_.clear();
        return;
    }
    if (mode == 0) {
        eraseInLine(0);
        for (int i = cursorRow_ + 1; i < rows_; ++i)
            lines_[static_cast<std::size_t>(i)] = blankLine();
    } else if (mode == 1) {
        eraseInLine(1);
        for (int i = 0; i < cursorRow_; ++i)
            lines_[static_cast<std::size_t>(i)] = blankLine();
    } else {
        for (auto& row : lines_)
            row = blankLine();
    }
}

int Screen::parameter(std::size_t index, int fallback) const
{
    if (index >= parameters_.size() || parameters_[index] < 0)
        return fallback;
    return parameters_[index];
}

void Screen::switchAlternate(bool enable)
{
    if (enable == alternateActive_)
        return;
    if (enable) {
        savedPrimary_ = lines_;
        savedColumn_ = cursorColumn_;
        savedRow_ = cursorRow_;
        lines_.assign(static_cast<std::size_t>(rows_), blankLine());
        alternateActive_ = true;
    } else {
        if (!savedPrimary_.empty())
            lines_ = savedPrimary_;
        savedPrimary_.clear();
        cursorColumn_ = savedColumn_;
        cursorRow_ = savedRow_;
        alternateActive_ = false;
        clampCursor();
    }
}

void Screen::setMode(bool enable)
{
    for (std::size_t i = 0; i < parameters_.size(); ++i) {
        int mode = parameter(i, 0);
        if (privateMarker_) {
            switch (mode) {
            case 1:
                applicationCursorKeys_ = enable;
                break;
            case 6:
                originMode_ = enable;
                cursorRow_ = enable ? scrollTop_ : 0;
                cursorColumn_ = 0;
                break;
            case 7:
                autoWrap_ = enable;
                break;
            case 25:
                cursorVisible_ = enable;
                break;
            case 47:
            case 1047:
            case 1049:
                switchAlternate(enable);
                break;
            case 2004:
                bracketedPaste_ = enable;
                break;
            default:
                break;
            }
        }
    }
}

void Screen::applySgr()
{
    if (parameters_.empty())
        parameters_.push_back(0);
    for (std::size_t i = 0; i < parameters_.size(); ++i) {
        int code = parameter(i, 0);
        switch (code) {
        case 0:
            attributes_ = Cell{};
            break;
        case 1:
            attributes_.bold = true;
            break;
        case 3:
            attributes_.italic = true;
            break;
        case 4:
            attributes_.underline = true;
            break;
        case 7:
            attributes_.inverse = true;
            break;
        case 22:
            attributes_.bold = false;
            break;
        case 23:
            attributes_.italic = false;
            break;
        case 24:
            attributes_.underline = false;
            break;
        case 27:
            attributes_.inverse = false;
            break;
        case 39:
            attributes_.foreground = kDefaultColor;
            break;
        case 49:
            attributes_.background = kDefaultColor;
            break;
        case 38:
        case 48: {
            std::uint32_t value = kDefaultColor;
            int kind = parameter(i + 1, 0);
            if (kind == 5) {
                value = kPaletteFlag | static_cast<std::uint32_t>(parameter(i + 2, 0) & 0xFF);
                i += 2;
            } else if (kind == 2) {
                value = kRgbFlag | (static_cast<std::uint32_t>(parameter(i + 2, 0) & 0xFF) << 16) | (static_cast<std::uint32_t>(parameter(i + 3, 0) & 0xFF) << 8) | static_cast<std::uint32_t>(parameter(i + 4, 0) & 0xFF);
                i += 4;
            }
            (code == 38 ? attributes_.foreground : attributes_.background) = value;
            break;
        }
        default:
            if (code >= 30 && code <= 37)
                attributes_.foreground = kPaletteFlag | static_cast<std::uint32_t>(code - 30);
            else if (code >= 40 && code <= 47)
                attributes_.background = kPaletteFlag | static_cast<std::uint32_t>(code - 40);
            else if (code >= 90 && code <= 97)
                attributes_.foreground = kPaletteFlag | static_cast<std::uint32_t>(code - 90 + 8);
            else if (code >= 100 && code <= 107)
                attributes_.background = kPaletteFlag | static_cast<std::uint32_t>(code - 100 + 8);
            break;
        }
    }
}

void Screen::csi(char final)
{
    if (privateMarker_ && final != 'h' && final != 'l')
        return;
    wrapPending_ = wrapPending_ && (final == 'm');
    switch (final) {
    case 'A':
        cursorRow_ = std::max(cursorRow_ >= scrollTop_ ? scrollTop_ : 0, cursorRow_ - std::max(1, parameter(0, 1)));
        break;
    case 'B':
    case 'e':
        cursorRow_ = std::min(cursorRow_ <= scrollBottom_ ? scrollBottom_ : rows_ - 1, cursorRow_ + std::max(1, parameter(0, 1)));
        break;
    case 'C':
    case 'a':
        cursorColumn_ = std::min(columns_ - 1, cursorColumn_ + std::max(1, parameter(0, 1)));
        break;
    case 'D':
        cursorColumn_ = std::max(0, cursorColumn_ - std::max(1, parameter(0, 1)));
        break;
    case 'E':
        cursorRow_ = std::min(rows_ - 1, cursorRow_ + std::max(1, parameter(0, 1)));
        cursorColumn_ = 0;
        break;
    case 'F':
        cursorRow_ = std::max(0, cursorRow_ - std::max(1, parameter(0, 1)));
        cursorColumn_ = 0;
        break;
    case 'G':
    case '`':
        cursorColumn_ = std::clamp(parameter(0, 1) - 1, 0, columns_ - 1);
        break;
    case 'd':
        cursorRow_ = std::clamp(parameter(0, 1) - 1 + (originMode_ ? scrollTop_ : 0), 0, rows_ - 1);
        break;
    case 'H':
    case 'f':
        cursorRow_ = std::clamp(parameter(0, 1) - 1 + (originMode_ ? scrollTop_ : 0), 0, rows_ - 1);
        cursorColumn_ = std::clamp(parameter(1, 1) - 1, 0, columns_ - 1);
        break;
    case 'J':
        eraseInDisplay(parameter(0, 0));
        break;
    case 'K':
        eraseInLine(parameter(0, 0));
        break;
    case 'L':
        if (cursorRow_ >= scrollTop_ && cursorRow_ <= scrollBottom_)
            scrollDown(cursorRow_, scrollBottom_, std::max(1, parameter(0, 1)));
        break;
    case 'M':
        if (cursorRow_ >= scrollTop_ && cursorRow_ <= scrollBottom_) {
            int count = std::min(std::max(1, parameter(0, 1)), scrollBottom_ - cursorRow_ + 1);
            for (int i = 0; i < count; ++i) {
                lines_.erase(lines_.begin() + cursorRow_);
                lines_.insert(lines_.begin() + scrollBottom_, blankLine());
            }
        }
        break;
    case 'P': {
        auto& row = lines_[static_cast<std::size_t>(cursorRow_)];
        int count = std::min(std::max(1, parameter(0, 1)), columns_ - cursorColumn_);
        row.erase(row.begin() + cursorColumn_, row.begin() + cursorColumn_ + count);
        row.resize(static_cast<std::size_t>(columns_), blankCell());
        break;
    }
    case '@': {
        auto& row = lines_[static_cast<std::size_t>(cursorRow_)];
        int count = std::min(std::max(1, parameter(0, 1)), columns_ - cursorColumn_);
        row.insert(row.begin() + cursorColumn_, static_cast<std::size_t>(count), blankCell());
        row.resize(static_cast<std::size_t>(columns_));
        break;
    }
    case 'X': {
        auto& row = lines_[static_cast<std::size_t>(cursorRow_)];
        int count = std::min(std::max(1, parameter(0, 1)), columns_ - cursorColumn_);
        for (int i = 0; i < count; ++i)
            row[static_cast<std::size_t>(cursorColumn_ + i)] = blankCell();
        break;
    }
    case 'S':
        scrollUp(scrollTop_, scrollBottom_, std::max(1, parameter(0, 1)));
        break;
    case 'T':
        scrollDown(scrollTop_, scrollBottom_, std::max(1, parameter(0, 1)));
        break;
    case 'b':
        for (int i = 0; i < std::max(1, parameter(0, 1)); ++i)
            print(lastPrinted_);
        break;
    case 'm':
        applySgr();
        break;
    case 'h':
        setMode(true);
        break;
    case 'l':
        setMode(false);
        break;
    case 'r': {
        int top = std::clamp(parameter(0, 1) - 1, 0, rows_ - 1);
        int bottom = std::clamp(parameter(1, rows_) - 1, 0, rows_ - 1);
        if (top < bottom) {
            scrollTop_ = top;
            scrollBottom_ = bottom;
            cursorRow_ = originMode_ ? top : 0;
            cursorColumn_ = 0;
        }
        break;
    }
    case 's':
        savedColumn_ = cursorColumn_;
        savedRow_ = cursorRow_;
        break;
    case 'u':
        cursorColumn_ = savedColumn_;
        cursorRow_ = savedRow_;
        clampCursor();
        break;
    case 'n':
        if (onReply) {
            if (parameter(0, 0) == 6)
                onReply("\x1b[" + std::to_string(cursorRow_ + 1) + ";" + std::to_string(cursorColumn_ + 1) + "R");
            else if (parameter(0, 0) == 5)
                onReply("\x1b[0n");
        }
        break;
    case 'c':
        if (onReply)
            onReply("\x1b[?1;2c");
        break;
    default:
        break;
    }
}

void Screen::osc()
{
    auto separator = oscBuffer_.find(';');
    if (separator == std::string::npos)
        return;
    std::string code = oscBuffer_.substr(0, separator);
    if (code == "0" || code == "2")
        title_ = oscBuffer_.substr(separator + 1);
}

std::string encodeKey(int key, bool control, bool alt, bool shift, bool applicationCursorKeys)
{
    enum { KeyUp = 0x01000013, KeyDown = 0x01000015, KeyLeft = 0x01000012, KeyRight = 0x01000014, KeyHome = 0x01000010, KeyEnd = 0x01000011,
           KeyPageUp = 0x01000016, KeyPageDown = 0x01000017, KeyDelete = 0x01000007, KeyInsert = 0x01000006, KeyBackspace = 0x01000003,
           KeyReturn = 0x01000004, KeyEnter = 0x01000005, KeyTab = 0x01000001, KeyBacktab = 0x01000002, KeyEscape = 0x01000000, KeyF1 = 0x01000030 };
    int modifier = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (control ? 4 : 0);
    auto cursor = [&](char letter) -> std::string {
        if (modifier > 1)
            return "\x1b[1;" + std::to_string(modifier) + letter;
        return std::string(applicationCursorKeys ? "\x1bO" : "\x1b[") + letter;
    };
    auto tilde = [&](int code) -> std::string {
        if (modifier > 1)
            return "\x1b[" + std::to_string(code) + ";" + std::to_string(modifier) + "~";
        return "\x1b[" + std::to_string(code) + "~";
    };
    switch (key) {
    case KeyUp:
        return cursor('A');
    case KeyDown:
        return cursor('B');
    case KeyRight:
        return cursor('C');
    case KeyLeft:
        return cursor('D');
    case KeyHome:
        return cursor('H');
    case KeyEnd:
        return cursor('F');
    case KeyInsert:
        return tilde(2);
    case KeyDelete:
        return tilde(3);
    case KeyPageUp:
        return tilde(5);
    case KeyPageDown:
        return tilde(6);
    case KeyBackspace:
        return control ? "\x08" : "\x7f";
    case KeyReturn:
    case KeyEnter:
        return "\r";
    case KeyTab:
        return "\t";
    case KeyBacktab:
        return "\x1b[Z";
    case KeyEscape:
        return "\x1b";
    default:
        break;
    }
    if (key >= KeyF1 && key < KeyF1 + 12) {
        static const int codes[] = {11, 12, 13, 14, 15, 17, 18, 19, 20, 21, 23, 24};
        int index = key - KeyF1;
        if (index < 4 && modifier == 1)
            return std::string("\x1bO") + static_cast<char>('P' + index);
        return tilde(codes[index]);
    }
    if (control && key >= 'A' && key <= 'Z')
        return std::string(1, static_cast<char>(key - 'A' + 1));
    if (control && key == ' ')
        return std::string(1, '\0');
    return {};
}

}
