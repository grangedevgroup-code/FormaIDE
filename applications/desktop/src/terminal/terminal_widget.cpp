#include "terminal/terminal_widget.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QContextMenuEvent>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QScrollBar>
#include <QToolButton>
#include <QVBoxLayout>

#include "app/app_context.h"
#include "ui/theme.h"

namespace forma::ui {

namespace {

const QRgb kAnsiDark[16] = {0xFF1B1D22, 0xFFF0716B, 0xFF5CBF78, 0xFFE0A93A, 0xFF6AA0FF, 0xFFC792EA, 0xFF5FC9C4, 0xFFD0D3DA,
                            0xFF6E7482, 0xFFFF8A84, 0xFF7ED99A, 0xFFF2C25C, 0xFF8FB8FF, 0xFFD8AEF2, 0xFF84DCD7, 0xFFFFFFFF};
const QRgb kAnsiLight[16] = {0xFF1C1F26, 0xFFC42F2A, 0xFF2C7A3F, 0xFF97640A, 0xFF2360C0, 0xFF8034A8, 0xFF0B7A75, 0xFF8A90A0,
                             0xFF5A6170, 0xFFD9443E, 0xFF38934F, 0xFFB57A10, 0xFF3B7BE0, 0xFF9A4CC4, 0xFF129A94, 0xFF1C1F26};

}

TerminalWidget::TerminalWidget(QWidget* parent) : QAbstractScrollArea(parent)
{
    setObjectName(QStringLiteral("terminal"));
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled);
    setFrameShape(QFrame::NoFrame);
    viewport()->setCursor(Qt::IBeamCursor);
    viewport()->setMouseTracking(false);
    setFont(Theme::instance().codeFont(10));
    updateMetrics();
    screen_ = std::make_unique<terminal::Screen>(120, 30);
    screen_->onReply = [this](std::string_view reply) {
        if (pty_) {
            auto status = pty_->write(reply);
            (void)status;
        }
    };
    refreshTimer_.setInterval(16);
    connect(&refreshTimer_, &QTimer::timeout, this, &TerminalWidget::refresh);
    refreshTimer_.start();
    connect(&Theme::instance(), &Theme::changed, viewport(), QOverload<>::of(&QWidget::update));
}

TerminalWidget::~TerminalWidget()
{
    refreshTimer_.stop();
    pty_.reset();
}

void TerminalWidget::updateMetrics()
{
    QFontMetricsF metrics(font());
    cellWidth_ = std::max(1, qCeil(metrics.horizontalAdvance(QLatin1Char('M'))));
    cellHeight_ = std::max(1, qCeil(metrics.height()));
    ascent_ = qCeil(metrics.ascent());
}

Status TerminalWidget::start(const terminal::ShellInfo& shell, const QString& directory, const std::map<std::string, std::string>& environment)
{
    terminal::Pty::Options options;
    options.program = shell.program;
    options.arguments = shell.arguments;
    options.workingDirectory = paths::fromUtf8(fromQ(directory));
    options.environment = environment;
    options.columns = screen_->columns();
    options.rows = screen_->rows();
    QPointer<TerminalWidget> self(this);
    auto started = terminal::Pty::start(
        options,
        [this](std::string_view data) {
            std::lock_guard lock(mutex_);
            screen_->feed(data);
            dirty_ = true;
        },
        [self](int code) {
            QMetaObject::invokeMethod(qApp, [self, code] {
                if (self)
                    Q_EMIT self->finished(code);
            }, Qt::QueuedConnection);
        });
    if (!started)
        return started.error();
    pty_ = std::move(started.value());
    return {};
}

void TerminalWidget::writeText(const QString& text)
{
    if (pty_) {
        auto status = pty_->write(fromQ(text));
        (void)status;
    }
}

void TerminalWidget::runCommand(const QString& command)
{
    writeText(command + QLatin1Char('\r'));
}

void TerminalWidget::stop()
{
    if (pty_)
        pty_->terminate();
}

bool TerminalWidget::running() const
{
    return pty_ && pty_->running();
}

QString TerminalWidget::visibleText() const
{
    std::lock_guard lock(mutex_);
    return toQ(screen_->text());
}

void TerminalWidget::refresh()
{
    std::lock_guard lock(mutex_);
    if (!dirty_)
        return;
    dirty_ = false;
    updateScrollBar();
    QString title = toQ(screen_->title());
    if (title != lastTitle_) {
        lastTitle_ = title;
        Q_EMIT titleChanged(title);
    }
    viewport()->update();
}

void TerminalWidget::updateScrollBar()
{
    QScrollBar* bar = verticalScrollBar();
    bool atBottom = bar->value() == bar->maximum();
    bar->setRange(0, screen_->scrollbackSize());
    bar->setPageStep(screen_->rows());
    if (atBottom)
        bar->setValue(bar->maximum());
}

QColor TerminalWidget::colorFor(std::uint32_t value, bool foreground) const
{
    const ThemeColors& c = colors();
    if (value == terminal::kDefaultColor)
        return foreground ? c.text : c.editor;
    if (value & terminal::kRgbFlag)
        return QColor((value >> 16) & 255, (value >> 8) & 255, value & 255);
    int index = static_cast<int>(value & 255);
    const QRgb* palette = Theme::instance().isDark() ? kAnsiDark : kAnsiLight;
    if (index < 16)
        return QColor(palette[index]);
    if (index < 232) {
        int v = index - 16;
        auto level = [](int n) { return n == 0 ? 0 : 55 + n * 40; };
        return QColor(level(v / 36), level((v / 6) % 6), level(v % 6));
    }
    int gray = 8 + (index - 232) * 10;
    return QColor(gray, gray, gray);
}

const std::vector<terminal::Cell>& TerminalWidget::rowAt(int absoluteRow) const
{
    int scrollback = screen_->scrollbackSize();
    if (absoluteRow < scrollback)
        return screen_->scrollbackLine(absoluteRow);
    return screen_->line(absoluteRow - scrollback);
}

void TerminalWidget::paintEvent(QPaintEvent*)
{
    std::lock_guard lock(mutex_);
    QPainter painter(viewport());
    const ThemeColors& c = colors();
    painter.fillRect(viewport()->rect(), c.editor);
    painter.setFont(font());
    int firstRow = verticalScrollBar()->value();
    QPoint selectionFrom = selectionStart_;
    QPoint selectionTo = selectionEnd_;
    if (selectionFrom.y() > selectionTo.y() || (selectionFrom.y() == selectionTo.y() && selectionFrom.x() > selectionTo.x()))
        std::swap(selectionFrom, selectionTo);
    bool hasSelection = selectionFrom.y() >= 0 && selectionFrom != selectionTo;
    int rows = screen_->rows();
    for (int row = 0; row < rows; ++row) {
        int absolute = firstRow + row;
        if (absolute >= screen_->scrollbackSize() + rows)
            break;
        const auto& cells = rowAt(absolute);
        int y = row * cellHeight_ + 2;
        int column = 0;
        while (column < static_cast<int>(cells.size())) {
            const terminal::Cell& first = cells[static_cast<std::size_t>(column)];
            int run = column;
            QString text;
            QColor foreground = colorFor(first.foreground, true);
            QColor background = colorFor(first.background, false);
            if (first.inverse)
                std::swap(foreground, background);
            auto inSelection = [&](int col) {
                if (!hasSelection || absolute < selectionFrom.y() || absolute > selectionTo.y())
                    return false;
                if (absolute == selectionFrom.y() && col < selectionFrom.x())
                    return false;
                if (absolute == selectionTo.y() && col >= selectionTo.x())
                    return false;
                return true;
            };
            bool selected = inSelection(column);
            while (run < static_cast<int>(cells.size())) {
                const terminal::Cell& cell = cells[static_cast<std::size_t>(run)];
                if (cell.foreground != first.foreground || cell.background != first.background || cell.bold != first.bold || cell.italic != first.italic
                    || cell.underline != first.underline || cell.inverse != first.inverse || inSelection(run) != selected)
                    break;
                if (cell.character != 0)
                    text += QString::fromUcs4(&cell.character, 1);
                ++run;
            }
            QRect area(column * cellWidth_ + 4, y, (run - column) * cellWidth_, cellHeight_);
            if (selected)
                painter.fillRect(area, c.selection);
            else if (background != c.editor)
                painter.fillRect(area, background);
            QString trimmed = text;
            while (trimmed.endsWith(QLatin1Char(' ')))
                trimmed.chop(1);
            if (!trimmed.isEmpty()) {
                QFont cellFont = font();
                cellFont.setBold(first.bold);
                cellFont.setItalic(first.italic);
                cellFont.setUnderline(first.underline);
                painter.setFont(cellFont);
                painter.setPen(selected ? c.selectionText : foreground);
                int x = area.x();
                for (qsizetype i = 0; i < trimmed.size();) {
                    qsizetype count = 1;
                    if (trimmed[i].isHighSurrogate() && i + 1 < trimmed.size())
                        count = 2;
                    painter.drawText(x, y + ascent_, trimmed.mid(i, count));
                    x += cellWidth_;
                    i += count;
                }
            }
            column = run;
        }
    }
    if (screen_->cursorVisible() && firstRow == screen_->scrollbackSize()) {
        QRect cursor(screen_->cursorColumn() * cellWidth_ + 4, screen_->cursorRow() * cellHeight_ + 2, cellWidth_, cellHeight_);
        if (focused_) {
            QColor block = c.text;
            block.setAlpha(140);
            painter.fillRect(cursor, block);
        } else {
            painter.setPen(c.textMuted);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(cursor.adjusted(0, 0, -1, -1));
        }
    }
}

void TerminalWidget::resizeEvent(QResizeEvent* event)
{
    QAbstractScrollArea::resizeEvent(event);
    int columns = std::max(10, (viewport()->width() - 8) / cellWidth_);
    int rows = std::max(2, (viewport()->height() - 4) / cellHeight_);
    std::lock_guard lock(mutex_);
    if (columns != screen_->columns() || rows != screen_->rows()) {
        screen_->resize(columns, rows);
        if (pty_)
            pty_->resize(columns, rows);
        updateScrollBar();
        verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    }
}

void TerminalWidget::keyPressEvent(QKeyEvent* event)
{
    bool control = event->modifiers() & Qt::ControlModifier;
    bool shift = event->modifiers() & Qt::ShiftModifier;
    bool alt = event->modifiers() & Qt::AltModifier;
    if (control && shift && event->key() == Qt::Key_C) {
        copySelection();
        return;
    }
    if ((control && shift && event->key() == Qt::Key_V) || (shift && event->key() == Qt::Key_Insert)) {
        paste();
        return;
    }
    if (control && event->key() == Qt::Key_V) {
        paste();
        return;
    }
    if (control && event->key() == Qt::Key_C && selectionStart_ != selectionEnd_ && selectionStart_.y() >= 0) {
        copySelection();
        selectionStart_ = selectionEnd_ = QPoint(-1, -1);
        viewport()->update();
        return;
    }
    if (shift && (event->key() == Qt::Key_PageUp || event->key() == Qt::Key_PageDown)) {
        QScrollBar* bar = verticalScrollBar();
        bar->setValue(bar->value() + (event->key() == Qt::Key_PageUp ? -bar->pageStep() : bar->pageStep()));
        viewport()->update();
        return;
    }
    bool applicationKeys;
    {
        std::lock_guard lock(mutex_);
        applicationKeys = screen_->applicationCursorKeys();
    }
    std::string sequence = terminal::encodeKey(event->key(), control, alt, shift, applicationKeys);
    if (sequence.empty()) {
        QString text = event->text();
        if (text.isEmpty())
            return;
        sequence = fromQ(text);
        if (alt)
            sequence.insert(sequence.begin(), '\x1b');
    }
    if (pty_) {
        auto status = pty_->write(sequence);
        (void)status;
    }
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}

void TerminalWidget::inputMethodEvent(QInputMethodEvent* event)
{
    if (!event->commitString().isEmpty())
        writeText(event->commitString());
    event->accept();
}

bool TerminalWidget::focusNextPrevChild(bool)
{
    return false;
}

QPoint TerminalWidget::cellAt(QPoint position) const
{
    int column = std::clamp((position.x() - 4 + cellWidth_ / 2) / cellWidth_, 0, screen_->columns());
    int row = std::max(0, (position.y() - 2) / cellHeight_);
    return QPoint(column, verticalScrollBar()->value() + row);
}

void TerminalWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        selectionStart_ = cellAt(event->pos());
        selectionEnd_ = selectionStart_;
        selecting_ = true;
        viewport()->update();
    } else if (event->button() == Qt::MiddleButton) {
        paste();
    }
}

void TerminalWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (selecting_) {
        selectionEnd_ = cellAt(event->pos());
        viewport()->update();
    }
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent*)
{
    selecting_ = false;
}

void TerminalWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    std::lock_guard lock(mutex_);
    QPoint cell = cellAt(event->pos());
    if (cell.y() >= screen_->scrollbackSize() + screen_->rows())
        return;
    const auto& cells = rowAt(cell.y());
    auto isWord = [&](int column) {
        if (column < 0 || column >= static_cast<int>(cells.size()))
            return false;
        char32_t c = cells[static_cast<std::size_t>(column)].character;
        return c != U' ' && c != 0 && c != U'"' && c != U'\'';
    };
    int start = cell.x();
    int end = cell.x();
    while (isWord(start - 1))
        --start;
    while (isWord(end))
        ++end;
    selectionStart_ = QPoint(start, cell.y());
    selectionEnd_ = QPoint(end, cell.y());
    viewport()->update();
}

void TerminalWidget::wheelEvent(QWheelEvent* event)
{
    QScrollBar* bar = verticalScrollBar();
    bar->setValue(bar->value() - event->angleDelta().y() / 40);
    viewport()->update();
}

void TerminalWidget::focusInEvent(QFocusEvent* event)
{
    focused_ = true;
    viewport()->update();
    QAbstractScrollArea::focusInEvent(event);
}

void TerminalWidget::focusOutEvent(QFocusEvent* event)
{
    focused_ = false;
    viewport()->update();
    QAbstractScrollArea::focusOutEvent(event);
}

QString TerminalWidget::selectedText() const
{
    std::lock_guard lock(mutex_);
    QPoint from = selectionStart_;
    QPoint to = selectionEnd_;
    if (from.y() < 0 || from == to)
        return {};
    if (from.y() > to.y() || (from.y() == to.y() && from.x() > to.x()))
        std::swap(from, to);
    QStringList lines;
    int total = screen_->scrollbackSize() + screen_->rows();
    for (int row = from.y(); row <= to.y() && row < total; ++row) {
        const auto& cells = rowAt(row);
        int begin = row == from.y() ? from.x() : 0;
        int end = row == to.y() ? to.x() : static_cast<int>(cells.size());
        QString line;
        for (int column = begin; column < end && column < static_cast<int>(cells.size()); ++column) {
            char32_t c = cells[static_cast<std::size_t>(column)].character;
            if (c != 0)
                line += QString::fromUcs4(&c, 1);
        }
        while (line.endsWith(QLatin1Char(' ')))
            line.chop(1);
        lines << line;
    }
    return lines.join(QLatin1Char('\n'));
}

void TerminalWidget::copySelection()
{
    QString text = selectedText();
    if (!text.isEmpty())
        QApplication::clipboard()->setText(text);
}

void TerminalWidget::paste()
{
    QString text = QApplication::clipboard()->text();
    if (text.isEmpty())
        return;
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\r"));
    text.replace(QLatin1Char('\n'), QLatin1Char('\r'));
    bool bracketed;
    {
        std::lock_guard lock(mutex_);
        bracketed = screen_->bracketedPaste();
    }
    if (bracketed)
        text = QStringLiteral("\x1b[200~") + text + QStringLiteral("\x1b[201~");
    writeText(text);
}

void TerminalWidget::contextMenuEvent(QContextMenuEvent* event)
{
    QMenu menu(this);
    menu.addAction(tr("Copy"), this, &TerminalWidget::copySelection)->setEnabled(!selectedText().isEmpty());
    menu.addAction(tr("Paste"), this, &TerminalWidget::paste);
    menu.exec(event->globalPos());
}

TerminalPanel::TerminalPanel(ProjectController* controller, QWidget* parent) : QWidget(parent), controller_(controller)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* bar = new QHBoxLayout();
    bar->setContentsMargins(6, 2, 6, 2);
    shells_ = terminal::availableShells();
    shellChoice_ = new QComboBox(this);
    shellChoice_->setObjectName(QStringLiteral("shellChoice"));
    for (const auto& shell : shells_)
        shellChoice_->addItem(toQ(shell.name), toQ(shell.id));
    bar->addWidget(shellChoice_);
    auto* add = new QToolButton(this);
    add->setObjectName(QStringLiteral("newTerminalButton"));
    add->setIcon(icon(QStringLiteral("plus")));
    add->setToolTip(tr("New terminal"));
    connect(add, &QToolButton::clicked, this, [this] { newSession(shellChoice_->currentData().toString()); });
    bar->addWidget(add);
    bar->addStretch(1);
    layout->addLayout(bar);
    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("terminalTabs"));
    tabs_->setTabsClosable(true);
    tabs_->setDocumentMode(true);
    layout->addWidget(tabs_, 1);
    connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int index) {
        QWidget* page = tabs_->widget(index);
        tabs_->removeTab(index);
        delete page;
    });
    connect(controller_, &ProjectController::projectClosed, this, [this] {
        while (tabs_->count() > 0) {
            QWidget* page = tabs_->widget(0);
            tabs_->removeTab(0);
            delete page;
        }
    });
}

TerminalWidget* TerminalPanel::current() const
{
    return qobject_cast<TerminalWidget*>(tabs_->currentWidget());
}

int TerminalPanel::sessionCount() const
{
    return tabs_->count();
}

TerminalWidget* TerminalPanel::newSession(const QString& shellId)
{
    const terminal::ShellInfo* shell = nullptr;
    QString wanted = shellId.isEmpty() ? toQ(AppContext::instance().settings().getString("terminal.shell")) : shellId;
    for (const auto& item : shells_) {
        if (toQ(item.id) == wanted)
            shell = &item;
    }
    if (shell == nullptr && !shells_.empty())
        shell = &shells_.front();
    if (shell == nullptr) {
        controller_->reportStatus(tr("No shell was found on this computer"));
        return nullptr;
    }
    auto* widget = new TerminalWidget(tabs_);
    QString directory = controller_->hasProject() ? controller_->rootPath() : QDir::homePath();
    int index = tabs_->addTab(widget, toQ(shell->name));
    tabs_->setCurrentIndex(index);
    auto status = widget->start(*shell, directory);
    if (!status) {
        controller_->reportStatus(toQ(status.error().message));
        tabs_->removeTab(index);
        delete widget;
        return nullptr;
    }
    QPointer<TerminalWidget> guard(widget);
    connect(widget, &TerminalWidget::finished, this, [this, guard](int code) {
        if (!guard)
            return;
        int position = tabs_->indexOf(guard);
        if (position >= 0)
            tabs_->setTabText(position, tabs_->tabText(position) + tr(" (exit %1)").arg(code));
    });
    widget->setFocus();
    return widget;
}

void TerminalPanel::runInNewTerminal(const QString& command)
{
    TerminalWidget* widget = current();
    if (widget == nullptr || !widget->running())
        widget = newSession();
    if (widget != nullptr)
        widget->runCommand(command);
}

}
