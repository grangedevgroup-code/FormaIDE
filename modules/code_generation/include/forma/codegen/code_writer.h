#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "forma/codegen/generator.h"

namespace forma::codegen {

std::string escapeLiteral(std::string_view value, LiteralStyle style);
Result<std::string> unescapeLiteral(std::string_view literal, LiteralStyle style);

struct BindingTarget {
    std::string screen;
    std::string component;
    std::string key;
    ValueKind kind = ValueKind::String;
    LiteralStyle style = LiteralStyle::CLike;
};

class CodeWriter {
public:
    explicit CodeWriter(std::string indentUnit = "    ") : indentUnit_(std::move(indentUnit)) {}

    CodeWriter& line(std::string_view text = {});
    CodeWriter& raw(std::string_view text);
    CodeWriter& indent();
    CodeWriter& dedent();
    CodeWriter& bound(std::string_view prefix, std::string_view literal, std::string_view suffix, BindingTarget target);

    void beginAnchor(std::string screen, std::string component);
    void endAnchor();

    int currentLine() const { return line_; }
    const std::string& text() const { return text_; }

    GeneratedFile finish(std::string path, FileCategory category = FileCategory::Generated);

private:
    std::string currentIndent() const;

    std::string indentUnit_;
    int depth_ = 0;
    int line_ = 1;
    std::string text_;
    std::vector<SourceBinding> bindings_;
    std::vector<ComponentAnchor> anchors_;
    std::vector<ComponentAnchor> openAnchors_;
};

}
