#include "forma/codegen/code_writer.h"

#include <algorithm>
#include <cstdio>

#include "forma/core/text.h"

namespace forma::codegen {

std::string escapeLiteral(std::string_view value, LiteralStyle style)
{
    std::string output;
    output.reserve(value.size() + 4);
    switch (style) {
    case LiteralStyle::CLike:
    case LiteralStyle::Python:
        for (char ch : value) {
            switch (ch) {
            case '\\':
                output += "\\\\";
                break;
            case '"':
                output += "\\\"";
                break;
            case '\n':
                output += "\\n";
                break;
            case '\r':
                output += "\\r";
                break;
            case '\t':
                output += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof buffer, "\\x%02x", static_cast<unsigned char>(ch));
                    output += buffer;
                } else {
                    output.push_back(ch);
                }
                break;
            }
        }
        break;
    case LiteralStyle::Xml:
    case LiteralStyle::Html:
        for (char ch : value) {
            switch (ch) {
            case '&':
                output += "&amp;";
                break;
            case '<':
                output += "&lt;";
                break;
            case '>':
                output += "&gt;";
                break;
            case '"':
                output += "&quot;";
                break;
            case '\n':
                output += style == LiteralStyle::Xml ? "&#10;" : "<br>";
                break;
            default:
                output.push_back(ch);
                break;
            }
        }
        break;
    case LiteralStyle::Raw:
        output.assign(value);
        break;
    }
    return output;
}

Result<std::string> unescapeLiteral(std::string_view literal, LiteralStyle style)
{
    std::string output;
    switch (style) {
    case LiteralStyle::CLike:
    case LiteralStyle::Python:
        for (std::size_t i = 0; i < literal.size(); ++i) {
            char ch = literal[i];
            if (ch == '"')
                return makeError("sync.literal", "The text contains an unescaped quote, so it isn't a single literal");
            if (ch != '\\') {
                output.push_back(ch);
                continue;
            }
            if (++i >= literal.size())
                return makeError("sync.literal", "The text ends with a lone backslash");
            switch (literal[i]) {
            case 'n':
                output.push_back('\n');
                break;
            case 'r':
                output.push_back('\r');
                break;
            case 't':
                output.push_back('\t');
                break;
            case '\\':
                output.push_back('\\');
                break;
            case '"':
                output.push_back('"');
                break;
            case '\'':
                output.push_back('\'');
                break;
            case '$':
                output.push_back('$');
                break;
            default:
                return makeError("sync.literal", "Unsupported escape in text");
            }
        }
        return output;
    case LiteralStyle::Xml:
    case LiteralStyle::Html:
        for (std::size_t i = 0; i < literal.size(); ++i) {
            char ch = literal[i];
            if (ch == '<') {
                if (style == LiteralStyle::Html && literal.substr(i, 4) == "<br>") {
                    output.push_back('\n');
                    i += 3;
                    continue;
                }
                return makeError("sync.literal", "The text contains markup");
            }
            if (ch != '&') {
                output.push_back(ch);
                continue;
            }
            if (literal.substr(i, 5) == "&amp;") {
                output.push_back('&');
                i += 4;
            } else if (literal.substr(i, 4) == "&lt;") {
                output.push_back('<');
                i += 3;
            } else if (literal.substr(i, 4) == "&gt;") {
                output.push_back('>');
                i += 3;
            } else if (literal.substr(i, 6) == "&quot;") {
                output.push_back('"');
                i += 5;
            } else if (literal.substr(i, 5) == "&#10;") {
                output.push_back('\n');
                i += 4;
            } else {
                return makeError("sync.literal", "Unsupported entity in text");
            }
        }
        return output;
    case LiteralStyle::Raw:
        return std::string(literal);
    }
    return std::string(literal);
}

std::string CodeWriter::currentIndent() const
{
    std::string output;
    for (int i = 0; i < depth_; ++i)
        output += indentUnit_;
    return output;
}

CodeWriter& CodeWriter::line(std::string_view value)
{
    if (!value.empty()) {
        text_ += currentIndent();
        text_.append(value);
    }
    text_.push_back('\n');
    ++line_;
    return *this;
}

CodeWriter& CodeWriter::raw(std::string_view value)
{
    text_.append(value);
    line_ += static_cast<int>(std::count(value.begin(), value.end(), '\n'));
    return *this;
}

CodeWriter& CodeWriter::indent()
{
    ++depth_;
    return *this;
}

CodeWriter& CodeWriter::dedent()
{
    if (depth_ > 0)
        --depth_;
    return *this;
}

CodeWriter& CodeWriter::bound(std::string_view prefix, std::string_view literal, std::string_view suffix, BindingTarget target)
{
    SourceBinding binding;
    binding.line = line_;
    binding.screen = std::move(target.screen);
    binding.component = std::move(target.component);
    binding.key = std::move(target.key);
    binding.prefix = std::string(prefix);
    binding.suffix = std::string(suffix);
    binding.kind = target.kind;
    binding.style = target.style;
    bindings_.push_back(std::move(binding));
    std::string full(prefix);
    full.append(literal);
    full.append(suffix);
    return line(full);
}

void CodeWriter::beginAnchor(std::string screen, std::string component)
{
    ComponentAnchor anchor;
    anchor.screen = std::move(screen);
    anchor.component = std::move(component);
    anchor.startLine = line_;
    openAnchors_.push_back(std::move(anchor));
}

void CodeWriter::endAnchor()
{
    if (openAnchors_.empty())
        return;
    ComponentAnchor anchor = std::move(openAnchors_.back());
    openAnchors_.pop_back();
    anchor.endLine = std::max(anchor.startLine, line_ - 1);
    anchors_.push_back(std::move(anchor));
}

GeneratedFile CodeWriter::finish(std::string path, FileCategory category)
{
    GeneratedFile file;
    file.path = std::move(path);
    file.content = text_;
    file.category = category;
    file.bindings = bindings_;
    file.anchors = anchors_;
    return file;
}

}
