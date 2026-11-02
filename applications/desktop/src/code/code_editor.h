#pragma once

#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QWidget>
#include <map>
#include <memory>
#include <set>

#include "app/project_controller.h"
#include "forma/editor/language_registry.h"
#include "forma/lsp/language_client.h"

class ScintillaEditBase;
class QCheckBox;

namespace forma::ui {

class FindBar;

class CodeEditor : public QWidget {
    Q_OBJECT

public:
    explicit CodeEditor(QWidget* parent = nullptr);

    void setLanguage(const editor::LanguageDefinition* language);
    const editor::LanguageDefinition* language() const { return language_; }
    QString text() const;
    void setText(const QString& text, bool keepUndo = false);
    void applyText(const QString& text);
    void insertAtCursor(const QString& text);
    bool isModified() const;
    void markSaved();
    QString selectedText() const;
    void gotoLine(int line, int column = 0);
    int currentLine() const;
    int currentColumn() const;
    int lineCount() const;
    void applyTheme();
    void setFontPointSize(int size);
    void setWordWrap(bool on);
    void setMinimap(bool on);
    bool minimapVisible() const;

    void undo();
    void redo();
    void toggleComment();
    void addNextOccurrence();
    void foldAll(bool fold);
    void duplicateLine();

    bool find(const QString& text, bool forward, bool matchCase, bool wholeWord, bool regex);
    int replaceAll(const QString& text, const QString& replacement, bool matchCase, bool wholeWord, bool regex);
    bool replaceCurrent(const QString& text, const QString& replacement, bool matchCase, bool wholeWord, bool regex);
    void showFindBar(bool replace);

    void setDiagnostics(const std::vector<lsp::Diagnostic>& diagnostics);
    void showCompletions(const std::vector<lsp::CompletionItem>& items);
    void showCallTip(const QString& text);
    void showWordCompletions();
    bool completionListVisible() const;
    void setReadOnly(bool on);
    lsp::Position cursorPosition() const;
    void setBreakpoints(const std::set<int>& lines);
    std::set<int> breakpoints() const { return breakpoints_; }
    void setExecutionLine(int line);
    ScintillaEditBase* scintilla() const { return editor_; }

Q_SIGNALS:
    void textEdited();
    void modificationChanged(bool modified);
    void cursorMoved(int line, int column);
    void completionRequested();
    void signatureRequested();
    void hoverRequested(lsp::Position position);
    void definitionRequested();
    void breakpointToggled(int line, bool enabled);
    void lineActivated(int line);

private:
    void updateBraces();
    void updateLineMargin();
    void onCharacterAdded(int character);
    void autoIndent();
    bool autoClosePair(int character);
    std::string rawText() const;

    ScintillaEditBase* editor_;
    ScintillaEditBase* minimap_ = nullptr;
    FindBar* findBar_ = nullptr;
    const editor::LanguageDefinition* language_ = nullptr;
    bool loading_ = false;
    int fontSize_ = 10;
    std::set<int> breakpoints_;
    int lastLineDigits_ = 0;
    std::vector<lsp::CompletionItem> lastCompletions_;
};

class FindBar : public QFrame {
    Q_OBJECT

public:
    explicit FindBar(CodeEditor* editor);
    void open(bool replace, const QString& seed);

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void search(bool forward);
    CodeEditor* editor_;
    QLineEdit* findEdit_;
    QLineEdit* replaceEdit_;
    QWidget* replaceRow_;
    QLabel* status_;
    QCheckBox* matchCase_;
    QCheckBox* wholeWord_;
    QCheckBox* regex_;
};

}

Q_DECLARE_METATYPE(forma::lsp::Position)
