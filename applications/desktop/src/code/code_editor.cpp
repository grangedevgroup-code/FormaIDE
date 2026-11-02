#include "code/code_editor.h"

#include <QCheckBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>

#include "ILexer.h"
#include "Lexilla.h"
#include "SciLexer.h"
#include "Scintilla.h"
#include "ScintillaEditBase.h"
#include "forma/core/json.h"
#include "forma/core/text.h"
#include "forma/resources/resources.h"
#include "ui/theme.h"

namespace forma::ui {

namespace {

constexpr int kMarginNumbers = 0;
constexpr int kMarginBreakpoints = 1;
constexpr int kMarginFold = 2;
constexpr int kMarkerBreakpoint = 20;
constexpr int kMarkerExecution = 21;
constexpr int kIndicatorError = 8;
constexpr int kIndicatorWarning = 9;
constexpr int kIndicatorFind = 10;

long toBgr(const QColor& color)
{
    return color.red() | (color.green() << 8) | (color.blue() << 16);
}

long toBgra(const QColor& color)
{
    return color.red() | (color.green() << 8) | (color.blue() << 16) | (color.alpha() << 24);
}

const Json& lexerStyles()
{
    static const Json table = [] {
        auto data = resources::find("languages/lexer_styles.json");
        if (data) {
            auto parsed = parseJson(*data);
            if (parsed)
                return *parsed;
        }
        return Json(Json::object());
    }();
    return table;
}

struct CodeColors {
    QColor comment;
    QColor keyword;
    QColor type;
    QColor string;
    QColor number;
    QColor preprocessor;
    QColor function;
    QColor operatorColor;
    QColor error;
    QColor success;
};

CodeColors codeColors()
{
    CodeColors c;
    if (Theme::instance().isDark()) {
        c.comment = QColor("#7D8594");
        c.keyword = QColor("#7FB0FF");
        c.type = QColor("#5FC9C4");
        c.string = QColor("#E6B673");
        c.number = QColor("#D29AE0");
        c.preprocessor = QColor("#A9B665");
        c.function = QColor("#E2C08D");
        c.operatorColor = QColor("#B4BAC6");
        c.error = QColor("#F0716B");
        c.success = QColor("#5CBF78");
    } else {
        c.comment = QColor("#6B7280");
        c.keyword = QColor("#1F5FBF");
        c.type = QColor("#0B7A75");
        c.string = QColor("#A3510C");
        c.number = QColor("#8034A8");
        c.preprocessor = QColor("#5B7A00");
        c.function = QColor("#7A5A00");
        c.operatorColor = QColor("#4A5160");
        c.error = QColor("#C42F2A");
        c.success = QColor("#2C7A3F");
    }
    return c;
}

}

CodeEditor::CodeEditor(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    findBar_ = new FindBar(this);
    findBar_->hide();
    layout->addWidget(findBar_);
    auto* row = new QHBoxLayout();
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    editor_ = new ScintillaEditBase(this);
    editor_->setObjectName(QStringLiteral("scintilla"));
    row->addWidget(editor_, 1);
    layout->addLayout(row, 1);

    editor_->send(SCI_SETCODEPAGE, SC_CP_UTF8);
    editor_->send(SCI_SETMULTIPLESELECTION, 1);
    editor_->send(SCI_SETADDITIONALSELECTIONTYPING, 1);
    editor_->send(SCI_SETMULTIPASTE, SC_MULTIPASTE_EACH);
    editor_->send(SCI_SETVIRTUALSPACEOPTIONS, SCVS_RECTANGULARSELECTION);
    editor_->send(SCI_SETINDENTATIONGUIDES, SC_IV_LOOKBOTH);
    editor_->send(SCI_SETTABWIDTH, 4);
    editor_->send(SCI_SETUSETABS, 0);
    editor_->send(SCI_SETINDENT, 4);
    editor_->send(SCI_SETTABINDENTS, 1);
    editor_->send(SCI_SETBACKSPACEUNINDENTS, 1);
    editor_->send(SCI_SETEOLMODE, SC_EOL_LF);
    editor_->send(SCI_SETSCROLLWIDTHTRACKING, 1);
    editor_->send(SCI_SETSCROLLWIDTH, 200);
    editor_->send(SCI_SETCARETLINEVISIBLEALWAYS, 1);
    editor_->send(SCI_SETCARETLINEFRAME, 0);
    editor_->send(SCI_SETCARETWIDTH, 2);
    editor_->send(SCI_SETMOUSEDWELLTIME, 500);
    editor_->send(SCI_SETBUFFEREDDRAW, 1);
    editor_->send(SCI_SETLAYOUTCACHE, SC_CACHE_PAGE);
    editor_->send(SCI_SETIDLESTYLING, SC_IDLESTYLING_TOVISIBLE);
    editor_->send(SCI_AUTOCSETIGNORECASE, 1);
    editor_->send(SCI_AUTOCSETORDER, SC_ORDER_PERFORMSORT);
    editor_->send(SCI_AUTOCSETMAXHEIGHT, 12);
    editor_->send(SCI_SETMARGINS, 3);
    editor_->send(SCI_SETMARGINTYPEN, kMarginNumbers, SC_MARGIN_NUMBER);
    editor_->send(SCI_SETMARGINTYPEN, kMarginBreakpoints, SC_MARGIN_SYMBOL);
    editor_->send(SCI_SETMARGINWIDTHN, kMarginBreakpoints, 14);
    editor_->send(SCI_SETMARGINSENSITIVEN, kMarginBreakpoints, 1);
    editor_->send(SCI_SETMARGINMASKN, kMarginBreakpoints, (1 << kMarkerBreakpoint) | (1 << kMarkerExecution));
    editor_->send(SCI_SETMARGINTYPEN, kMarginFold, SC_MARGIN_SYMBOL);
    editor_->send(SCI_SETMARGINMASKN, kMarginFold, SC_MASK_FOLDERS);
    editor_->send(SCI_SETMARGINWIDTHN, kMarginFold, 14);
    editor_->send(SCI_SETMARGINSENSITIVEN, kMarginFold, 1);
    editor_->send(SCI_SETAUTOMATICFOLD, SC_AUTOMATICFOLD_SHOW | SC_AUTOMATICFOLD_CLICK | SC_AUTOMATICFOLD_CHANGE);
    const std::pair<int, int> markers[] = {{SC_MARKNUM_FOLDEROPEN, SC_MARK_ARROWDOWN}, {SC_MARKNUM_FOLDER, SC_MARK_ARROW}, {SC_MARKNUM_FOLDERSUB, SC_MARK_EMPTY}, {SC_MARKNUM_FOLDERTAIL, SC_MARK_EMPTY}, {SC_MARKNUM_FOLDEREND, SC_MARK_ARROW}, {SC_MARKNUM_FOLDEROPENMID, SC_MARK_ARROWDOWN}, {SC_MARKNUM_FOLDERMIDTAIL, SC_MARK_EMPTY}};
    for (const auto& [marker, symbol] : markers)
        editor_->send(SCI_MARKERDEFINE, marker, symbol);
    editor_->send(SCI_MARKERDEFINE, kMarkerBreakpoint, SC_MARK_CIRCLE);
    editor_->send(SCI_MARKERDEFINE, kMarkerExecution, SC_MARK_SHORTARROW);
    editor_->send(SCI_INDICSETSTYLE, kIndicatorError, INDIC_SQUIGGLE);
    editor_->send(SCI_INDICSETSTYLE, kIndicatorWarning, INDIC_SQUIGGLE);
    editor_->send(SCI_INDICSETSTYLE, kIndicatorFind, INDIC_FULLBOX);
    editor_->send(SCI_SETMODEVENTMASK, SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT);

    connect(editor_, &ScintillaEditBase::modified, this, [this](Scintilla::ModificationFlags type, Scintilla::Position, Scintilla::Position, Scintilla::Position, const QByteArray&, Scintilla::Position, Scintilla::FoldLevel, Scintilla::FoldLevel) {
        int flags = static_cast<int>(type);
        if ((flags & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)) == 0 || loading_)
            return;
        updateLineMargin();
        Q_EMIT textEdited();
    });
    connect(editor_, &ScintillaEditBase::savePointChanged, this, [this](bool dirty) {
        if (!loading_)
            Q_EMIT modificationChanged(dirty);
    });
    connect(editor_, &ScintillaEditBase::updateUi, this, [this](Scintilla::Update) {
        updateBraces();
        Q_EMIT cursorMoved(currentLine(), currentColumn());
    });
    connect(editor_, &ScintillaEditBase::charAdded, this, &CodeEditor::onCharacterAdded);
    connect(editor_, &ScintillaEditBase::marginClicked, this, [this](Scintilla::Position position, Scintilla::KeyMod, int margin) {
        int line = static_cast<int>(editor_->send(SCI_LINEFROMPOSITION, static_cast<uptr_t>(position))) + 1;
        if (margin == kMarginBreakpoints) {
            bool enable = breakpoints_.count(line) == 0;
            if (enable)
                breakpoints_.insert(line);
            else
                breakpoints_.erase(line);
            setBreakpoints(breakpoints_);
            Q_EMIT breakpointToggled(line, enable);
        }
    });
    connect(editor_, &ScintillaEditBase::dwellStart, this, [this](int x, int y) {
        sptr_t position = editor_->send(SCI_POSITIONFROMPOINTCLOSE, static_cast<uptr_t>(x), y);
        if (position < 0)
            return;
        lsp::Position output;
        output.line = static_cast<int>(editor_->send(SCI_LINEFROMPOSITION, static_cast<uptr_t>(position)));
        output.character = static_cast<int>(editor_->send(SCI_COUNTCODEUNITS, static_cast<uptr_t>(editor_->send(SCI_POSITIONFROMLINE, static_cast<uptr_t>(output.line))), position));
        Q_EMIT hoverRequested(output);
    });
    connect(editor_, &ScintillaEditBase::dwellEnd, this, [this](int, int) { editor_->send(SCI_CALLTIPCANCEL); });
    connect(editor_, &ScintillaEditBase::autoCompleteSelection, this, [](Scintilla::Position, const QString&) {});
    connect(&Theme::instance(), &Theme::changed, this, &CodeEditor::applyTheme);
    connect(editor_, &ScintillaEditBase::keyPressed, this, [this](QKeyEvent* event) {
        if (event->key() == Qt::Key_Space && event->modifiers() == Qt::ControlModifier)
            Q_EMIT completionRequested();
    });
    applyTheme();
}

std::string CodeEditor::rawText() const
{
    sptr_t length = editor_->send(SCI_GETLENGTH);
    std::string buffer(static_cast<std::size_t>(length) + 1, '\0');
    editor_->send(SCI_GETTEXT, static_cast<uptr_t>(length) + 1, reinterpret_cast<sptr_t>(buffer.data()));
    buffer.resize(static_cast<std::size_t>(length));
    return buffer;
}

QString CodeEditor::text() const
{
    return toQ(rawText());
}

void CodeEditor::setText(const QString& value, bool keepUndo)
{
    QByteArray bytes = value.toUtf8();
    loading_ = true;
    if (value.contains(QStringLiteral("\r\n")))
        editor_->send(SCI_SETEOLMODE, SC_EOL_CRLF);
    else
        editor_->send(SCI_SETEOLMODE, SC_EOL_LF);
    sptr_t firstLine = editor_->send(SCI_GETFIRSTVISIBLELINE);
    sptr_t position = editor_->send(SCI_GETCURRENTPOS);
    editor_->send(SCI_SETTEXT, 0, reinterpret_cast<sptr_t>(bytes.constData()));
    if (!keepUndo) {
        editor_->send(SCI_EMPTYUNDOBUFFER);
    } else {
        editor_->send(SCI_GOTOPOS, static_cast<uptr_t>(std::min<sptr_t>(position, editor_->send(SCI_GETLENGTH))));
        editor_->send(SCI_SETFIRSTVISIBLELINE, static_cast<uptr_t>(firstLine));
    }
    editor_->send(SCI_SETSAVEPOINT);
    loading_ = false;
    updateLineMargin();
}

void CodeEditor::insertAtCursor(const QString& value)
{
    QByteArray bytes = value.toUtf8();
    editor_->send(SCI_ADDTEXT, static_cast<uptr_t>(bytes.size()), reinterpret_cast<sptr_t>(bytes.constData()));
}

bool CodeEditor::isModified() const
{
    return editor_->send(SCI_GETMODIFY) != 0;
}

void CodeEditor::markSaved()
{
    editor_->send(SCI_SETSAVEPOINT);
}

QString CodeEditor::selectedText() const
{
    sptr_t length = editor_->send(SCI_GETSELTEXT, 0, 0);
    if (length <= 0)
        return {};
    std::string buffer(static_cast<std::size_t>(length) + 1, '\0');
    editor_->send(SCI_GETSELTEXT, 0, reinterpret_cast<sptr_t>(buffer.data()));
    buffer.resize(static_cast<std::size_t>(length));
    return toQ(buffer);
}

void CodeEditor::gotoLine(int line, int column)
{
    int zero = std::max(0, line - 1);
    editor_->send(SCI_ENSUREVISIBLEENFORCEPOLICY, static_cast<uptr_t>(zero));
    sptr_t start = editor_->send(SCI_POSITIONFROMLINE, static_cast<uptr_t>(zero));
    sptr_t position = column > 0 ? editor_->send(SCI_FINDCOLUMN, static_cast<uptr_t>(zero), column - 1) : start;
    editor_->send(SCI_GOTOPOS, static_cast<uptr_t>(position));
    sptr_t visible = editor_->send(SCI_LINESONSCREEN);
    editor_->send(SCI_SETFIRSTVISIBLELINE, static_cast<uptr_t>(std::max<sptr_t>(0, zero - visible / 3)));
    editor_->setFocus();
}

int CodeEditor::currentLine() const
{
    return static_cast<int>(editor_->send(SCI_LINEFROMPOSITION, static_cast<uptr_t>(editor_->send(SCI_GETCURRENTPOS)))) + 1;
}

int CodeEditor::currentColumn() const
{
    return static_cast<int>(editor_->send(SCI_GETCOLUMN, static_cast<uptr_t>(editor_->send(SCI_GETCURRENTPOS)))) + 1;
}

int CodeEditor::lineCount() const
{
    return static_cast<int>(editor_->send(SCI_GETLINECOUNT));
}

lsp::Position CodeEditor::cursorPosition() const
{
    sptr_t position = editor_->send(SCI_GETCURRENTPOS);
    lsp::Position output;
    output.line = static_cast<int>(editor_->send(SCI_LINEFROMPOSITION, static_cast<uptr_t>(position)));
    sptr_t start = editor_->send(SCI_POSITIONFROMLINE, static_cast<uptr_t>(output.line));
    output.character = static_cast<int>(editor_->send(SCI_COUNTCODEUNITS, static_cast<uptr_t>(start), position));
    return output;
}

void CodeEditor::setLanguage(const editor::LanguageDefinition* language)
{
    language_ = language;
    std::string lexerName = language != nullptr ? language->lexer : std::string("null");
    Scintilla::ILexer5* lexer = CreateLexer(lexerName.c_str());
    editor_->send(SCI_SETILEXER, 0, reinterpret_cast<sptr_t>(lexer));
    if (lexer != nullptr) {
        editor_->sends(SCI_SETPROPERTY, reinterpret_cast<uptr_t>("fold"), "1");
        editor_->sends(SCI_SETPROPERTY, reinterpret_cast<uptr_t>("fold.compact"), "0");
        editor_->sends(SCI_SETPROPERTY, reinterpret_cast<uptr_t>("fold.comment"), "1");
        editor_->sends(SCI_SETPROPERTY, reinterpret_cast<uptr_t>("fold.preprocessor"), "1");
        editor_->sends(SCI_SETPROPERTY, reinterpret_cast<uptr_t>("fold.html"), "1");
        editor_->sends(SCI_SETPROPERTY, reinterpret_cast<uptr_t>("lexer.cpp.track.preprocessor"), "0");
        if (language != nullptr) {
            for (const auto& [key, value] : language->lexerProperties)
                editor_->sends(SCI_SETPROPERTY, reinterpret_cast<uptr_t>(key.c_str()), value.c_str());
            for (std::size_t i = 0; i < language->keywords.size() && i < 9; ++i)
                editor_->sends(SCI_SETKEYWORDS, i, language->keywords[i].c_str());
        }
    }
    if (language != nullptr) {
        editor_->send(SCI_SETTABWIDTH, static_cast<uptr_t>(language->indent));
        editor_->send(SCI_SETINDENT, static_cast<uptr_t>(language->indent));
        editor_->send(SCI_SETUSETABS, language->tabs ? 1 : 0);
        setWordWrap(language->wrap);
    }
    applyTheme();
}

void CodeEditor::setFontPointSize(int size)
{
    fontSize_ = size;
    applyTheme();
}

void CodeEditor::setWordWrap(bool on)
{
    editor_->send(SCI_SETWRAPMODE, on ? SC_WRAP_WORD : SC_WRAP_NONE);
}

void CodeEditor::setMinimap(bool on)
{
    if (on && minimap_ == nullptr) {
        minimap_ = new ScintillaEditBase(this);
        minimap_->setObjectName(QStringLiteral("minimap"));
        minimap_->setFixedWidth(90);
        minimap_->send(SCI_SETDOCPOINTER, 0, editor_->send(SCI_GETDOCPOINTER));
        minimap_->send(SCI_SETZOOM, static_cast<uptr_t>(-10));
        minimap_->send(SCI_SETMARGINS, 0);
        minimap_->send(SCI_SETVSCROLLBAR, 0);
        minimap_->send(SCI_SETHSCROLLBAR, 0);
        minimap_->send(SCI_SETREADONLY, 1);
        minimap_->send(SCI_SETCARETSTYLE, CARETSTYLE_INVISIBLE);
        auto* row = qobject_cast<QHBoxLayout*>(static_cast<QVBoxLayout*>(layout())->itemAt(1)->layout());
        row->addWidget(minimap_);
        connect(editor_, &ScintillaEditBase::verticalScrolled, minimap_, [this](int value) {
            sptr_t lines = editor_->send(SCI_GETLINECOUNT);
            sptr_t visible = minimap_->send(SCI_LINESONSCREEN);
            sptr_t own = editor_->send(SCI_LINESONSCREEN);
            if (lines > visible && lines > own)
                minimap_->send(SCI_SETFIRSTVISIBLELINE, static_cast<uptr_t>(value * (lines - visible) / (lines - own)));
        });
        connect(minimap_, &ScintillaEditBase::buttonPressed, this, [this](QMouseEvent* event) {
            sptr_t position = minimap_->send(SCI_POSITIONFROMPOINT, static_cast<uptr_t>(event->position().x()), static_cast<sptr_t>(event->position().y()));
            gotoLine(static_cast<int>(minimap_->send(SCI_LINEFROMPOSITION, static_cast<uptr_t>(position))) + 1);
        });
        applyTheme();
    }
    if (minimap_ != nullptr)
        minimap_->setVisible(on);
}

bool CodeEditor::minimapVisible() const
{
    return minimap_ != nullptr && minimap_->isVisible();
}

void CodeEditor::applyTheme()
{
    const ThemeColors& c = colors();
    CodeColors code = codeColors();
    QFont font = Theme::instance().codeFont(fontSize_);
    QByteArray family = font.family().toUtf8();
    auto setup = [&](ScintillaEditBase* target, int size) {
        target->sends(SCI_STYLESETFONT, STYLE_DEFAULT, family.constData());
        target->send(SCI_STYLESETSIZE, STYLE_DEFAULT, size);
        target->send(SCI_STYLESETFORE, STYLE_DEFAULT, toBgr(c.text));
        target->send(SCI_STYLESETBACK, STYLE_DEFAULT, toBgr(c.editor));
        target->send(SCI_STYLECLEARALL);
        target->send(SCI_SETCARETFORE, toBgr(c.text));
        target->send(SCI_SETSELBACK, 1, toBgr(c.selection));
        target->send(SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_BACK, toBgra(c.selection));
        target->send(SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_ADDITIONAL_BACK, toBgra(c.selection));
        target->send(SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_SECONDARY_BACK, toBgra(c.selection));
        target->send(SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_INACTIVE_BACK, toBgra(c.selection));
        target->send(SCI_SETELEMENTCOLOUR, SC_ELEMENT_CARET_LINE_BACK, toBgra(c.hover));
        target->send(SCI_SETCARETLINEVISIBLE, 1);
        target->send(SCI_SETWHITESPACEFORE, 1, toBgr(c.textFaint));
        target->send(SCI_STYLESETFORE, STYLE_LINENUMBER, toBgr(c.textFaint));
        target->send(SCI_STYLESETBACK, STYLE_LINENUMBER, toBgr(c.editor));
        target->send(SCI_STYLESETFORE, STYLE_INDENTGUIDE, toBgr(c.border));
        target->send(SCI_STYLESETBACK, STYLE_INDENTGUIDE, toBgr(c.editor));
        target->send(SCI_STYLESETFORE, STYLE_BRACELIGHT, toBgr(c.accent));
        target->send(SCI_STYLESETBOLD, STYLE_BRACELIGHT, 1);
        target->send(SCI_STYLESETFORE, STYLE_BRACEBAD, toBgr(c.danger));
        target->send(SCI_STYLESETBACK, STYLE_CALLTIP, toBgr(c.panelRaised));
        target->send(SCI_STYLESETFORE, STYLE_CALLTIP, toBgr(c.text));
        target->send(SCI_CALLTIPSETBACK, toBgr(c.panelRaised));
        target->send(SCI_CALLTIPSETFORE, toBgr(c.text));
        for (int margin = 0; margin < 3; ++margin)
            target->send(SCI_SETMARGINBACKN, margin, toBgr(c.editor));
        target->send(SCI_SETFOLDMARGINCOLOUR, 1, toBgr(c.editor));
        target->send(SCI_SETFOLDMARGINHICOLOUR, 1, toBgr(c.editor));
        for (int marker : {SC_MARKNUM_FOLDEROPEN, SC_MARKNUM_FOLDER, SC_MARKNUM_FOLDEREND, SC_MARKNUM_FOLDEROPENMID}) {
            target->send(SCI_MARKERSETFORE, marker, toBgr(c.textFaint));
            target->send(SCI_MARKERSETBACK, marker, toBgr(c.textFaint));
        }
        target->send(SCI_MARKERSETFORE, kMarkerBreakpoint, toBgr(c.danger));
        target->send(SCI_MARKERSETBACK, kMarkerBreakpoint, toBgr(c.danger));
        target->send(SCI_MARKERSETFORE, kMarkerExecution, toBgr(c.warning));
        target->send(SCI_MARKERSETBACK, kMarkerExecution, toBgr(c.warning));
        target->send(SCI_INDICSETFORE, kIndicatorError, toBgr(c.danger));
        target->send(SCI_INDICSETFORE, kIndicatorWarning, toBgr(c.warning));
        target->send(SCI_INDICSETFORE, kIndicatorFind, toBgr(c.accent));
        target->send(SCI_INDICSETALPHA, kIndicatorFind, 60);
        std::string lexerName = language_ != nullptr ? language_->lexer : std::string();
        const Json& table = lexerStyles();
        auto entry = table.find(lexerName);
        if (entry != table.end() && entry->is_object()) {
            for (auto item = entry->begin(); item != entry->end(); ++item) {
                int style = std::stoi(item.key());
                std::string category = item->get<std::string>();
                QColor color = c.text;
                bool bold = false;
                bool italic = false;
                if (category == "comment") {
                    color = code.comment;
                    italic = true;
                } else if (category == "keyword") {
                    color = code.keyword;
                } else if (category == "type") {
                    color = code.type;
                } else if (category == "string") {
                    color = code.string;
                } else if (category == "number") {
                    color = code.number;
                } else if (category == "preprocessor") {
                    color = code.preprocessor;
                } else if (category == "function") {
                    color = code.function;
                } else if (category == "operator") {
                    color = code.operatorColor;
                } else if (category == "error") {
                    color = code.error;
                } else if (category == "success") {
                    color = code.success;
                }
                target->send(SCI_STYLESETFORE, static_cast<uptr_t>(style), toBgr(color));
                target->send(SCI_STYLESETBOLD, static_cast<uptr_t>(style), bold);
                target->send(SCI_STYLESETITALIC, static_cast<uptr_t>(style), italic);
            }
        }
        target->send(SCI_COLOURISE, 0, -1);
    };
    setup(editor_, fontSize_);
    if (minimap_ != nullptr) {
        setup(minimap_, 2);
        minimap_->send(SCI_SETZOOM, 0);
    }
    lastLineDigits_ = 0;
    updateLineMargin();
}

void CodeEditor::updateLineMargin()
{
    int digits = QString::number(std::max(100, lineCount())).size();
    if (digits == lastLineDigits_)
        return;
    lastLineDigits_ = digits;
    QByteArray sample(digits + 1, '9');
    sptr_t width = editor_->sends(SCI_TEXTWIDTH, STYLE_LINENUMBER, sample.constData());
    editor_->send(SCI_SETMARGINWIDTHN, kMarginNumbers, width);
}

void CodeEditor::updateBraces()
{
    sptr_t position = editor_->send(SCI_GETCURRENTPOS);
    auto isBrace = [&](sptr_t at) {
        if (at < 0)
            return false;
        int ch = static_cast<int>(editor_->send(SCI_GETCHARAT, static_cast<uptr_t>(at)));
        return ch == '(' || ch == ')' || ch == '[' || ch == ']' || ch == '{' || ch == '}';
    };
    sptr_t brace = isBrace(position) ? position : isBrace(position - 1) ? position - 1 : -1;
    if (brace < 0) {
        editor_->send(SCI_BRACEHIGHLIGHT, static_cast<uptr_t>(-1), -1);
        return;
    }
    sptr_t match = editor_->send(SCI_BRACEMATCH, static_cast<uptr_t>(brace), 0);
    if (match < 0)
        editor_->send(SCI_BRACEBADLIGHT, static_cast<uptr_t>(brace));
    else
        editor_->send(SCI_BRACEHIGHLIGHT, static_cast<uptr_t>(brace), match);
}

void CodeEditor::autoIndent()
{
    sptr_t position = editor_->send(SCI_GETCURRENTPOS);
    sptr_t line = editor_->send(SCI_LINEFROMPOSITION, static_cast<uptr_t>(position));
    if (line == 0)
        return;
    sptr_t indent = editor_->send(SCI_GETLINEINDENTATION, static_cast<uptr_t>(line - 1));
    sptr_t lastChar = editor_->send(SCI_GETLINEENDPOSITION, static_cast<uptr_t>(line - 1)) - 1;
    sptr_t lineStart = editor_->send(SCI_POSITIONFROMLINE, static_cast<uptr_t>(line - 1));
    while (lastChar >= lineStart && (editor_->send(SCI_GETCHARAT, static_cast<uptr_t>(lastChar)) == ' ' || editor_->send(SCI_GETCHARAT, static_cast<uptr_t>(lastChar)) == '\t'))
        --lastChar;
    if (lastChar >= lineStart) {
        int ch = static_cast<int>(editor_->send(SCI_GETCHARAT, static_cast<uptr_t>(lastChar)));
        if (ch == '{' || ch == '(' || ch == '[' || (ch == ':' && language_ != nullptr && language_->id == "python"))
            indent += editor_->send(SCI_GETINDENT);
    }
    editor_->send(SCI_SETLINEINDENTATION, static_cast<uptr_t>(line), indent);
    editor_->send(SCI_GOTOPOS, static_cast<uptr_t>(editor_->send(SCI_GETLINEINDENTPOSITION, static_cast<uptr_t>(line))));
}

void CodeEditor::onCharacterAdded(int character)
{
    if (character == '\n') {
        autoIndent();
        return;
    }
    if (character == '}') {
        sptr_t position = editor_->send(SCI_GETCURRENTPOS);
        sptr_t line = editor_->send(SCI_LINEFROMPOSITION, static_cast<uptr_t>(position));
        if (position - 1 == editor_->send(SCI_GETLINEINDENTPOSITION, static_cast<uptr_t>(line))) {
            sptr_t match = editor_->send(SCI_BRACEMATCH, static_cast<uptr_t>(position - 1), 0);
            if (match >= 0)
                editor_->send(SCI_SETLINEINDENTATION, static_cast<uptr_t>(line), editor_->send(SCI_GETLINEINDENTATION, static_cast<uptr_t>(editor_->send(SCI_LINEFROMPOSITION, static_cast<uptr_t>(match)))));
        }
        return;
    }
    if (character == '(' || character == ',')
        Q_EMIT signatureRequested();
    else if (character == ')')
        editor_->send(SCI_CALLTIPCANCEL);
    if (character == '.' || character == '>' || character == ':') {
        Q_EMIT completionRequested();
    } else if (std::isalnum(character) || character == '_') {
        sptr_t position = editor_->send(SCI_GETCURRENTPOS);
        sptr_t start = editor_->send(SCI_WORDSTARTPOSITION, static_cast<uptr_t>(position), 1);
        if (position - start == 3 && editor_->send(SCI_AUTOCACTIVE) == 0)
            Q_EMIT completionRequested();
    }
}

void CodeEditor::undo()
{
    editor_->send(SCI_UNDO);
}

void CodeEditor::redo()
{
    editor_->send(SCI_REDO);
}

void CodeEditor::toggleComment()
{
    if (language_ == nullptr)
        return;
    std::string prefix = language_->lineComment;
    sptr_t start = editor_->send(SCI_LINEFROMPOSITION, static_cast<uptr_t>(editor_->send(SCI_GETSELECTIONSTART)));
    sptr_t endPosition = editor_->send(SCI_GETSELECTIONEND);
    sptr_t end = editor_->send(SCI_LINEFROMPOSITION, static_cast<uptr_t>(endPosition));
    if (end > start && endPosition == editor_->send(SCI_POSITIONFROMLINE, static_cast<uptr_t>(end)))
        --end;
    if (prefix.empty()) {
        if (language_->blockCommentStart.empty())
            return;
        std::string open = language_->blockCommentStart + " ";
        std::string close = " " + language_->blockCommentEnd;
        sptr_t a = editor_->send(SCI_GETLINEINDENTPOSITION, static_cast<uptr_t>(start));
        sptr_t b = editor_->send(SCI_GETLINEENDPOSITION, static_cast<uptr_t>(end));
        editor_->send(SCI_BEGINUNDOACTION);
        editor_->sends(SCI_INSERTTEXT, static_cast<uptr_t>(b), close.c_str());
        editor_->sends(SCI_INSERTTEXT, static_cast<uptr_t>(a), open.c_str());
        editor_->send(SCI_ENDUNDOACTION);
        return;
    }
    bool allCommented = true;
    auto lineStartsWith = [&](sptr_t line, const std::string& value) {
        sptr_t position = editor_->send(SCI_GETLINEINDENTPOSITION, static_cast<uptr_t>(line));
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (editor_->send(SCI_GETCHARAT, static_cast<uptr_t>(position) + i) != value[i])
                return false;
        }
        return true;
    };
    for (sptr_t line = start; line <= end; ++line) {
        if (editor_->send(SCI_GETLINEINDENTPOSITION, static_cast<uptr_t>(line)) == editor_->send(SCI_GETLINEENDPOSITION, static_cast<uptr_t>(line)))
            continue;
        if (!lineStartsWith(line, prefix))
            allCommented = false;
    }
    editor_->send(SCI_BEGINUNDOACTION);
    for (sptr_t line = start; line <= end; ++line) {
        sptr_t position = editor_->send(SCI_GETLINEINDENTPOSITION, static_cast<uptr_t>(line));
        if (position == editor_->send(SCI_GETLINEENDPOSITION, static_cast<uptr_t>(line)))
            continue;
        if (allCommented) {
            std::size_t length = prefix.size();
            if (editor_->send(SCI_GETCHARAT, static_cast<uptr_t>(position) + length) == ' ')
                ++length;
            editor_->send(SCI_DELETERANGE, static_cast<uptr_t>(position), static_cast<sptr_t>(length));
        } else {
            editor_->sends(SCI_INSERTTEXT, static_cast<uptr_t>(position), (prefix + " ").c_str());
        }
    }
    editor_->send(SCI_ENDUNDOACTION);
}

void CodeEditor::addNextOccurrence()
{
    if (editor_->send(SCI_GETSELECTIONEMPTY) != 0) {
        sptr_t position = editor_->send(SCI_GETCURRENTPOS);
        sptr_t start = editor_->send(SCI_WORDSTARTPOSITION, static_cast<uptr_t>(position), 1);
        sptr_t end = editor_->send(SCI_WORDENDPOSITION, static_cast<uptr_t>(position), 1);
        editor_->send(SCI_SETSELECTION, static_cast<uptr_t>(end), start);
        return;
    }
    editor_->send(SCI_TARGETWHOLEDOCUMENT);
    editor_->send(SCI_SETSEARCHFLAGS, SCFIND_MATCHCASE);
    editor_->send(SCI_MULTIPLESELECTADDNEXT);
}

void CodeEditor::foldAll(bool fold)
{
    editor_->send(SCI_FOLDALL, fold ? SC_FOLDACTION_CONTRACT : SC_FOLDACTION_EXPAND);
}

void CodeEditor::duplicateLine()
{
    editor_->send(SCI_SELECTIONDUPLICATE);
}

bool CodeEditor::find(const QString& value, bool forward, bool matchCase, bool wholeWord, bool regex)
{
    if (value.isEmpty())
        return false;
    QByteArray bytes = value.toUtf8();
    int flags = (matchCase ? SCFIND_MATCHCASE : 0) | (wholeWord ? SCFIND_WHOLEWORD : 0) | (regex ? SCFIND_REGEXP | SCFIND_CXX11REGEX : 0);
    editor_->send(SCI_SETSEARCHFLAGS, flags);
    sptr_t length = editor_->send(SCI_GETLENGTH);
    auto attempt = [&](sptr_t from, sptr_t to) {
        editor_->send(SCI_SETTARGETRANGE, static_cast<uptr_t>(from), to);
        return editor_->send(SCI_SEARCHINTARGET, static_cast<uptr_t>(bytes.size()), reinterpret_cast<sptr_t>(bytes.constData()));
    };
    sptr_t found;
    if (forward) {
        found = attempt(editor_->send(SCI_GETSELECTIONEND), length);
        if (found < 0)
            found = attempt(0, length);
    } else {
        found = attempt(editor_->send(SCI_GETSELECTIONSTART), 0);
        if (found < 0)
            found = attempt(length, 0);
    }
    if (found < 0)
        return false;
    sptr_t end = editor_->send(SCI_GETTARGETEND);
    editor_->send(SCI_ENSUREVISIBLEENFORCEPOLICY, static_cast<uptr_t>(editor_->send(SCI_LINEFROMPOSITION, static_cast<uptr_t>(found))));
    editor_->send(SCI_SETSEL, static_cast<uptr_t>(found), end);
    editor_->send(SCI_SCROLLCARET);
    return true;
}

bool CodeEditor::replaceCurrent(const QString& value, const QString& replacement, bool matchCase, bool wholeWord, bool regex)
{
    QByteArray bytes = value.toUtf8();
    QByteArray with = replacement.toUtf8();
    int flags = (matchCase ? SCFIND_MATCHCASE : 0) | (wholeWord ? SCFIND_WHOLEWORD : 0) | (regex ? SCFIND_REGEXP | SCFIND_CXX11REGEX : 0);
    editor_->send(SCI_SETSEARCHFLAGS, flags);
    sptr_t start = editor_->send(SCI_GETSELECTIONSTART);
    sptr_t end = editor_->send(SCI_GETSELECTIONEND);
    editor_->send(SCI_SETTARGETRANGE, static_cast<uptr_t>(start), end);
    if (editor_->send(SCI_SEARCHINTARGET, static_cast<uptr_t>(bytes.size()), reinterpret_cast<sptr_t>(bytes.constData())) == start && editor_->send(SCI_GETTARGETEND) == end) {
        editor_->send(regex ? SCI_REPLACETARGETRE : SCI_REPLACETARGET, static_cast<uptr_t>(with.size()), reinterpret_cast<sptr_t>(with.constData()));
        editor_->send(SCI_SETSEL, static_cast<uptr_t>(editor_->send(SCI_GETTARGETEND)), editor_->send(SCI_GETTARGETEND));
    }
    return find(value, true, matchCase, wholeWord, regex);
}

int CodeEditor::replaceAll(const QString& value, const QString& replacement, bool matchCase, bool wholeWord, bool regex)
{
    if (value.isEmpty())
        return 0;
    QByteArray bytes = value.toUtf8();
    QByteArray with = replacement.toUtf8();
    int flags = (matchCase ? SCFIND_MATCHCASE : 0) | (wholeWord ? SCFIND_WHOLEWORD : 0) | (regex ? SCFIND_REGEXP | SCFIND_CXX11REGEX : 0);
    editor_->send(SCI_SETSEARCHFLAGS, flags);
    int count = 0;
    sptr_t position = 0;
    editor_->send(SCI_BEGINUNDOACTION);
    while (true) {
        editor_->send(SCI_SETTARGETRANGE, static_cast<uptr_t>(position), editor_->send(SCI_GETLENGTH));
        if (editor_->send(SCI_SEARCHINTARGET, static_cast<uptr_t>(bytes.size()), reinterpret_cast<sptr_t>(bytes.constData())) < 0)
            break;
        editor_->send(regex ? SCI_REPLACETARGETRE : SCI_REPLACETARGET, static_cast<uptr_t>(with.size()), reinterpret_cast<sptr_t>(with.constData()));
        position = editor_->send(SCI_GETTARGETEND);
        if (editor_->send(SCI_GETTARGETSTART) == position)
            position += 1;
        ++count;
    }
    editor_->send(SCI_ENDUNDOACTION);
    return count;
}

void CodeEditor::showFindBar(bool replace)
{
    findBar_->open(replace, selectedText().contains(QLatin1Char('\n')) ? QString() : selectedText());
}

void CodeEditor::setDiagnostics(const std::vector<lsp::Diagnostic>& diagnostics)
{
    sptr_t length = editor_->send(SCI_GETLENGTH);
    editor_->send(SCI_SETINDICATORCURRENT, kIndicatorError);
    editor_->send(SCI_INDICATORCLEARRANGE, 0, length);
    editor_->send(SCI_SETINDICATORCURRENT, kIndicatorWarning);
    editor_->send(SCI_INDICATORCLEARRANGE, 0, length);
    editor_->send(SCI_ANNOTATIONCLEARALL);
    editor_->send(SCI_ANNOTATIONSETVISIBLE, ANNOTATION_INDENTED);
    for (const auto& item : diagnostics) {
        sptr_t lineStart = editor_->send(SCI_POSITIONFROMLINE, static_cast<uptr_t>(item.range.start.line));
        if (lineStart < 0)
            continue;
        sptr_t from = editor_->send(SCI_POSITIONRELATIVECODEUNITS, static_cast<uptr_t>(lineStart), item.range.start.character);
        sptr_t endLine = editor_->send(SCI_POSITIONFROMLINE, static_cast<uptr_t>(item.range.end.line));
        sptr_t to = editor_->send(SCI_POSITIONRELATIVECODEUNITS, static_cast<uptr_t>(endLine), item.range.end.character);
        if (to <= from)
            to = from + 1;
        editor_->send(SCI_SETINDICATORCURRENT, item.severity <= 1 ? kIndicatorError : kIndicatorWarning);
        editor_->send(SCI_INDICATORFILLRANGE, static_cast<uptr_t>(from), to - from);
        if (item.severity <= 1)
            editor_->sends(SCI_ANNOTATIONSETTEXT, static_cast<uptr_t>(item.range.start.line), item.message.c_str());
    }
}

void CodeEditor::showCompletions(const std::vector<lsp::CompletionItem>& items)
{
    lastCompletions_ = items;
    if (items.empty())
        return;
    std::string list;
    int count = 0;
    for (const auto& item : items) {
        if (item.label.empty() || text::contains(item.label, " "))
            continue;
        if (!list.empty())
            list.push_back(' ');
        list += item.insertText.empty() || text::contains(item.insertText, " ") ? item.label : item.insertText;
        if (++count >= 500)
            break;
    }
    if (list.empty())
        return;
    sptr_t position = editor_->send(SCI_GETCURRENTPOS);
    sptr_t start = editor_->send(SCI_WORDSTARTPOSITION, static_cast<uptr_t>(position), 1);
    editor_->sends(SCI_AUTOCSHOW, static_cast<uptr_t>(position - start), list.c_str());
}

void CodeEditor::showCallTip(const QString& value)
{
    if (value.isEmpty())
        return;
    QByteArray bytes = value.left(1200).toUtf8();
    editor_->sends(SCI_CALLTIPSHOW, static_cast<uptr_t>(editor_->send(SCI_GETCURRENTPOS)), bytes.constData());
}

void CodeEditor::setBreakpoints(const std::set<int>& lines)
{
    breakpoints_ = lines;
    editor_->send(SCI_MARKERDELETEALL, kMarkerBreakpoint);
    for (int line : lines)
        editor_->send(SCI_MARKERADD, static_cast<uptr_t>(line - 1), kMarkerBreakpoint);
}

void CodeEditor::setExecutionLine(int line)
{
    editor_->send(SCI_MARKERDELETEALL, kMarkerExecution);
    if (line > 0) {
        editor_->send(SCI_MARKERADD, static_cast<uptr_t>(line - 1), kMarkerExecution);
        gotoLine(line);
    }
}

FindBar::FindBar(CodeEditor* editor) : QFrame(editor), editor_(editor)
{
    setProperty("role", "panelHeader");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 4, 6, 4);
    layout->setSpacing(3);
    auto* first = new QHBoxLayout();
    findEdit_ = new QLineEdit(this);
    findEdit_->setObjectName(QStringLiteral("findEdit"));
    findEdit_->setPlaceholderText(tr("Find"));
    first->addWidget(findEdit_, 1);
    auto* previous = new QPushButton(tr("Previous"), this);
    auto* next = new QPushButton(tr("Next"), this);
    first->addWidget(previous);
    first->addWidget(next);
    matchCase_ = new QCheckBox(tr("Case"), this);
    wholeWord_ = new QCheckBox(tr("Word"), this);
    regex_ = new QCheckBox(tr("Regex"), this);
    first->addWidget(matchCase_);
    first->addWidget(wholeWord_);
    first->addWidget(regex_);
    status_ = new QLabel(this);
    status_->setProperty("role", "muted");
    first->addWidget(status_);
    auto* close = new QToolButton(this);
    close->setIcon(icon(QStringLiteral("close")));
    connect(close, &QToolButton::clicked, this, [this] { hide(); editor_->scintilla()->setFocus(); });
    first->addWidget(close);
    layout->addLayout(first);
    replaceRow_ = new QWidget(this);
    auto* second = new QHBoxLayout(replaceRow_);
    second->setContentsMargins(0, 0, 0, 0);
    replaceEdit_ = new QLineEdit(replaceRow_);
    replaceEdit_->setObjectName(QStringLiteral("replaceEdit"));
    replaceEdit_->setPlaceholderText(tr("Replace with"));
    second->addWidget(replaceEdit_, 1);
    auto* replaceOne = new QPushButton(tr("Replace"), replaceRow_);
    auto* replaceEvery = new QPushButton(tr("Replace all"), replaceRow_);
    second->addWidget(replaceOne);
    second->addWidget(replaceEvery);
    layout->addWidget(replaceRow_);
    connect(findEdit_, &QLineEdit::returnPressed, this, [this] { search(true); });
    connect(next, &QPushButton::clicked, this, [this] { search(true); });
    connect(previous, &QPushButton::clicked, this, [this] { search(false); });
    connect(replaceOne, &QPushButton::clicked, this, [this] {
        bool found = editor_->replaceCurrent(findEdit_->text(), replaceEdit_->text(), matchCase_->isChecked(), wholeWord_->isChecked(), regex_->isChecked());
        status_->setText(found ? QString() : tr("No more matches"));
    });
    connect(replaceEvery, &QPushButton::clicked, this, [this] {
        int count = editor_->replaceAll(findEdit_->text(), replaceEdit_->text(), matchCase_->isChecked(), wholeWord_->isChecked(), regex_->isChecked());
        status_->setText(tr("%n replaced", nullptr, count));
    });
}

void FindBar::open(bool replace, const QString& seed)
{
    show();
    replaceRow_->setVisible(replace);
    if (!seed.isEmpty())
        findEdit_->setText(seed);
    findEdit_->setFocus();
    findEdit_->selectAll();
    status_->clear();
}

void FindBar::search(bool forward)
{
    bool found = editor_->find(findEdit_->text(), forward, matchCase_->isChecked(), wholeWord_->isChecked(), regex_->isChecked());
    status_->setText(found ? QString() : tr("No matches"));
}

void FindBar::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        hide();
        editor_->scintilla()->setFocus();
        return;
    }
    QFrame::keyPressEvent(event);
}

}
