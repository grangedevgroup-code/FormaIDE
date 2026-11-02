#pragma once

#include <QAbstractScrollArea>
#include <QComboBox>
#include <QTabWidget>
#include <QTimer>
#include <QWidget>
#include <memory>
#include <mutex>

#include "app/project_controller.h"
#include "forma/terminal/terminal.h"

namespace forma::ui {

class TerminalWidget : public QAbstractScrollArea {
    Q_OBJECT

public:
    explicit TerminalWidget(QWidget* parent = nullptr);
    ~TerminalWidget() override;

    Status start(const terminal::ShellInfo& shell, const QString& directory, const std::map<std::string, std::string>& environment = {});
    void writeText(const QString& text);
    void runCommand(const QString& command);
    void stop();
    bool running() const;
    QString visibleText() const;
    QString selectedText() const;
    void copySelection();
    void paste();

Q_SIGNALS:
    void titleChanged(const QString& title);
    void finished(int exitCode);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    bool focusNextPrevChild(bool next) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    void updateMetrics();
    void updateScrollBar();
    void refresh();
    QColor colorFor(std::uint32_t value, bool foreground) const;
    QPoint cellAt(QPoint position) const;
    const std::vector<terminal::Cell>& rowAt(int absoluteRow) const;

    std::unique_ptr<terminal::Screen> screen_;
    std::unique_ptr<terminal::Pty> pty_;
    mutable std::recursive_mutex mutex_;
    QTimer refreshTimer_;
    bool dirty_ = false;
    int cellWidth_ = 8;
    int cellHeight_ = 16;
    int ascent_ = 12;
    QPoint selectionStart_{-1, -1};
    QPoint selectionEnd_{-1, -1};
    bool selecting_ = false;
    bool focused_ = false;
    QString lastTitle_;
};

class TerminalPanel : public QWidget {
    Q_OBJECT

public:
    explicit TerminalPanel(ProjectController* controller, QWidget* parent = nullptr);
    TerminalWidget* newSession(const QString& shellId = {});
    TerminalWidget* current() const;
    void runInNewTerminal(const QString& command);
    int sessionCount() const;

private:
    ProjectController* controller_;
    QTabWidget* tabs_;
    QComboBox* shellChoice_;
    std::vector<terminal::ShellInfo> shells_;
};

}
