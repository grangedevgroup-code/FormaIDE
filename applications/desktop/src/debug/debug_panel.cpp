#include "debug/debug_panel.h"

#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>

#include "app/app_context.h"
#include "ui/theme.h"

namespace forma::ui {

DebugPanel::DebugPanel(ProjectController* controller, EditorArea* editors, QWidget* parent) : QWidget(parent), controller_(controller), editors_(editors)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);
    auto* top = new QHBoxLayout();
    adapter_ = new QComboBox(this);
    adapter_->setObjectName(QStringLiteral("debugAdapter"));
    top->addWidget(adapter_);
    program_ = new QLineEdit(this);
    program_->setObjectName(QStringLiteral("debugProgram"));
    program_->setPlaceholderText(tr("Program to debug (leave empty for the current file or last build)"));
    top->addWidget(program_, 1);
    auto add = [&](const QString& iconName, const QString& tip, const QString& name) {
        auto* button = new QToolButton(this);
        button->setIcon(icon(iconName));
        button->setToolTip(tip);
        button->setObjectName(name);
        top->addWidget(button);
        return button;
    };
    startButton_ = add(QStringLiteral("run"), tr("Start debugging (F5)"), QStringLiteral("debugStart"));
    continueButton_ = add(QStringLiteral("restart"), tr("Continue"), QStringLiteral("debugContinue"));
    pauseButton_ = add(QStringLiteral("pause"), tr("Pause"), QStringLiteral("debugPause"));
    overButton_ = add(QStringLiteral("step-over"), tr("Step over (F10)"), QStringLiteral("debugOver"));
    intoButton_ = add(QStringLiteral("step-into"), tr("Step into (F11)"), QStringLiteral("debugInto"));
    outButton_ = add(QStringLiteral("step-out"), tr("Step out (Shift+F11)"), QStringLiteral("debugOut"));
    stopButton_ = add(QStringLiteral("stop"), tr("Stop"), QStringLiteral("debugStop"));
    layout->addLayout(top);
    state_ = new QLabel(tr("Not running. Click in the margin of an editor to set a breakpoint."), this);
    state_->setObjectName(QStringLiteral("debugState"));
    state_->setProperty("role", "muted");
    layout->addWidget(state_);
    auto* split = new QSplitter(Qt::Horizontal, this);
    stack_ = new QTreeWidget(split);
    stack_->setObjectName(QStringLiteral("debugStack"));
    stack_->setHeaderLabels({tr("Call stack"), tr("Location")});
    stack_->setRootIsDecorated(false);
    variables_ = new QTreeWidget(split);
    variables_->setObjectName(QStringLiteral("debugVariables"));
    variables_->setHeaderLabels({tr("Variable"), tr("Value"), tr("Type")});
    auto* watchHolder = new QWidget(split);
    auto* watchLayout = new QVBoxLayout(watchHolder);
    watchLayout->setContentsMargins(0, 0, 0, 0);
    watchLayout->setSpacing(2);
    watches_ = new QTreeWidget(watchHolder);
    watches_->setObjectName(QStringLiteral("debugWatches"));
    watches_->setHeaderLabels({tr("Watch"), tr("Value")});
    watches_->setRootIsDecorated(false);
    watchLayout->addWidget(watches_, 1);
    watchInput_ = new QLineEdit(watchHolder);
    watchInput_->setPlaceholderText(tr("Add a watch expression"));
    watchLayout->addWidget(watchInput_);
    console_ = new QPlainTextEdit(split);
    console_->setObjectName(QStringLiteral("debugConsole"));
    console_->setReadOnly(true);
    console_->setFont(Theme::instance().codeFont(9));
    console_->setMaximumBlockCount(5000);
    layout->addWidget(split, 1);

    connect(startButton_, &QToolButton::clicked, this, &DebugPanel::start);
    connect(stopButton_, &QToolButton::clicked, this, &DebugPanel::stop);
    connect(continueButton_, &QToolButton::clicked, this, [this] { if (session_) session_->continueExecution(threadId_); });
    connect(pauseButton_, &QToolButton::clicked, this, [this] { if (session_) session_->pause(threadId_); });
    connect(overButton_, &QToolButton::clicked, this, [this] { if (session_) session_->next(threadId_); });
    connect(intoButton_, &QToolButton::clicked, this, [this] { if (session_) session_->stepIn(threadId_); });
    connect(outButton_, &QToolButton::clicked, this, [this] { if (session_) session_->stepOut(threadId_); });
    connect(watchInput_, &QLineEdit::returnPressed, this, &DebugPanel::addWatch);
    connect(stack_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
        frameId_ = item->data(0, Qt::UserRole).toInt();
        loadFrame(frameId_);
        QString path = item->data(0, Qt::UserRole + 1).toString();
        if (!path.isEmpty()) {
            QString relative = QDir(controller_->rootPath()).relativeFilePath(path);
            if (!relative.startsWith(QStringLiteral("..")))
                controller_->openFile(relative, item->data(0, Qt::UserRole + 2).toInt());
        }
    });
    connect(variables_, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) {
        int reference = item->data(0, Qt::UserRole).toInt();
        if (reference == 0 || !session_ || item->data(0, Qt::UserRole + 1).toBool())
            return;
        item->setData(0, Qt::UserRole + 1, true);
        qDeleteAll(item->takeChildren());
        QPointer<QTreeWidget> tree(variables_);
        session_->variables(reference, [this, tree, item](Result<std::vector<debugger::Variable>> list) {
            if (!list)
                return;
            auto copy = std::move(list.value());
            QMetaObject::invokeMethod(qApp, [tree, item, copy] {
                if (!tree)
                    return;
                for (const auto& variable : copy) {
                    auto* child = new QTreeWidgetItem(item);
                    child->setText(0, toQ(variable.name));
                    child->setText(1, toQ(variable.value));
                    child->setText(2, toQ(variable.type));
                    child->setData(0, Qt::UserRole, variable.variablesReference);
                    if (variable.variablesReference != 0)
                        new QTreeWidgetItem(child);
                }
            }, Qt::QueuedConnection);
        });
    });
    connect(editors_, &EditorArea::breakpointsChanged, this, [this](const QString& path, const std::set<int>& lines) {
        if (!session_)
            return;
        std::vector<debugger::Breakpoint> list;
        for (int line : lines)
            list.push_back({line});
        session_->setBreakpoints(paths::fromUtf8(fromQ(QDir(controller_->rootPath()).filePath(path))), list);
    });
    connect(controller_, &ProjectController::projectClosed, this, &DebugPanel::stop);
    refreshAdapters();
    updateButtons();
}

DebugPanel::~DebugPanel()
{
    session_.reset();
}

void DebugPanel::refreshAdapters()
{
    adapter_->clear();
    for (const auto& adapter : debugger::knownAdapters()) {
        adapter_->addItem(adapter.found ? toQ(adapter.name) : tr("%1 (not installed)").arg(toQ(adapter.name)), toQ(adapter.id));
        adapter_->setItemData(adapter_->count() - 1, adapter.found ? QString() : toQ(adapter.install), Qt::ToolTipRole);
    }
}

bool DebugPanel::active() const
{
    return session_ && session_->state() != debugger::SessionState::Idle && session_->state() != debugger::SessionState::Terminated;
}

QString DebugPanel::stateText() const
{
    return state_->text();
}

void DebugPanel::updateButtons()
{
    bool running = active();
    bool stopped = running && session_->state() == debugger::SessionState::Stopped;
    startButton_->setEnabled(!running);
    stopButton_->setEnabled(running);
    continueButton_->setEnabled(stopped);
    pauseButton_->setEnabled(running && !stopped);
    overButton_->setEnabled(stopped);
    intoButton_->setEnabled(stopped);
    outButton_->setEnabled(stopped);
}

void DebugPanel::appendOutput(const QString& text, bool isError)
{
    QTextCharFormat format;
    format.setForeground(isError ? colors().danger : colors().text);
    QTextCursor cursor(console_->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text, format);
    console_->ensureCursorVisible();
}

std::string DebugPanel::defaultAdapterFor(const QString& path) const
{
    const editor::LanguageDefinition* language = AppContext::instance().languages().forPath(fromQ(path));
    if (language != nullptr && !language->debugAdapters.empty()) {
        auto adapters = debugger::knownAdapters();
        for (const auto& wanted : language->debugAdapters) {
            for (const auto& adapter : adapters) {
                if (adapter.id == wanted && adapter.found)
                    return wanted;
            }
        }
        return language->debugAdapters.front();
    }
    return {};
}

void DebugPanel::start()
{
    if (!controller_->hasProject() || active())
        return;
    QString programText = program_->text().trimmed();
    QString current = editors_->currentAbsolutePath();
    if (programText.isEmpty()) {
        for (const auto& artifact : controller_->artifacts()) {
            if (artifact.kind == "Windows executable" || artifact.kind == "Executable")
                programText = QDir(toQ(paths::toUtf8(controller_->session()->targetDirectory(*controller_->target())))).filePath(toQ(artifact.path));
        }
    }
    if (programText.isEmpty())
        programText = current;
    if (programText.isEmpty()) {
        state_->setText(tr("Choose a program, build the project, or open a file to debug."));
        return;
    }
    std::string wanted = defaultAdapterFor(programText);
    if (!wanted.empty())
        adapter_->setCurrentIndex(std::max(0, adapter_->findData(toQ(wanted))));
    auto adapter = debugger::findAdapter(fromQ(adapter_->currentData().toString()));
    if (!adapter || !adapter->found) {
        state_->setText(tr("%1 is not installed. %2").arg(adapter_->currentText(), adapter ? toQ(adapter->install) : QString()));
        return;
    }
    console_->clear();
    stack_->clear();
    variables_->clear();
    session_ = std::make_unique<debugger::DebugSession>();
    debugger::LaunchOptions options;
    options.program = fromQ(programText);
    options.workingDirectory = controller_->session()->root();
    Json extra = Json::object();
    extra["name"] = "FormaIDE";
    extra["type"] = adapter->type;
    extra["request"] = "launch";
    if (adapter->id == "debugpy")
        extra["console"] = "internalConsole";
    if (adapter->id == "delve") {
        extra["mode"] = "debug";
    }
    options.extra = extra;
    for (const auto& [file, lines] : editors_->allBreakpoints()) {
        std::vector<debugger::Breakpoint> list;
        for (int line : lines)
            list.push_back({line});
        session_->setBreakpoints(paths::fromUtf8(fromQ(QDir(controller_->rootPath()).filePath(file))), list);
    }
    QPointer<DebugPanel> self(this);
    debugger::DebugSession::Callbacks callbacks;
    callbacks.onState = [self](debugger::SessionState state) {
        QMetaObject::invokeMethod(qApp, [self, state] {
            if (!self)
                return;
            QString text;
            switch (state) {
            case debugger::SessionState::Starting:
                text = self->tr("Starting...");
                break;
            case debugger::SessionState::Running:
                text = self->tr("Running");
                self->stack_->clear();
                self->variables_->clear();
                self->editors_->clearExecutionLocation();
                break;
            case debugger::SessionState::Stopped:
                text = self->tr("Paused");
                break;
            case debugger::SessionState::Terminated:
                text = self->tr("Finished");
                self->stack_->clear();
                self->variables_->clear();
                self->editors_->clearExecutionLocation();
                break;
            case debugger::SessionState::Idle:
                break;
            }
            self->state_->setText(text);
            self->updateButtons();
            Q_EMIT self->stateChanged(text);
        }, Qt::QueuedConnection);
    };
    callbacks.onStopped = [self](const debugger::StopEvent& event) {
        debugger::StopEvent copy = event;
        QMetaObject::invokeMethod(qApp, [self, copy] { if (self) self->onStopped(copy); }, Qt::QueuedConnection);
    };
    callbacks.onOutput = [self](std::string_view category, std::string_view output) {
        QString text = QString::fromUtf8(output.data(), static_cast<qsizetype>(output.size()));
        bool isError = category == "stderr" || category == "error";
        QMetaObject::invokeMethod(qApp, [self, text, isError] { if (self) self->appendOutput(text, isError); }, Qt::QueuedConnection);
    };
    callbacks.onExited = [self](int code) {
        QMetaObject::invokeMethod(qApp, [self, code] { if (self) self->appendOutput(self->tr("\nThe program ended with exit code %1\n").arg(code)); }, Qt::QueuedConnection);
    };
    auto status = session_->start(*adapter, options, callbacks);
    if (!status) {
        state_->setText(toQ(status.error().message));
        session_.reset();
    }
    Q_EMIT controller_->automationLogged(tr("Debugger"), toQ(adapter->command.front()) + QLatin1Char(' ') + programText);
    updateButtons();
}

void DebugPanel::stop()
{
    if (session_)
        session_->stop();
    editors_->clearExecutionLocation();
    updateButtons();
}

void DebugPanel::onStopped(const debugger::StopEvent& event)
{
    threadId_ = event.threadId;
    state_->setText(tr("Paused: %1").arg(toQ(event.description.empty() ? event.reason : event.description)));
    Q_EMIT stateChanged(state_->text());
    updateButtons();
    if (!session_)
        return;
    if (threadId_ == 0) {
        QPointer<DebugPanel> self(this);
        session_->threads([self](Result<std::vector<debugger::ThreadInfo>> list) {
            if (!list || list->empty())
                return;
            int id = list->front().id;
            QMetaObject::invokeMethod(qApp, [self, id] {
                if (self) {
                    self->threadId_ = id;
                    self->onStopped({"pause", id, {}, true});
                }
            }, Qt::QueuedConnection);
        });
        return;
    }
    QPointer<DebugPanel> self(this);
    session_->stackTrace(threadId_, [self](Result<std::vector<debugger::StackFrame>> frames) {
        if (!frames)
            return;
        auto copy = std::move(frames.value());
        QMetaObject::invokeMethod(qApp, [self, copy] {
            if (!self)
                return;
            self->stack_->clear();
            bool first = true;
            for (const auto& frame : copy) {
                auto* item = new QTreeWidgetItem(self->stack_);
                item->setText(0, toQ(frame.name));
                item->setText(1, QStringLiteral("%1:%2").arg(QFileInfo(toQ(frame.path)).fileName()).arg(frame.line));
                item->setData(0, Qt::UserRole, frame.id);
                item->setData(0, Qt::UserRole + 1, toQ(frame.path));
                item->setData(0, Qt::UserRole + 2, frame.line);
                if (first) {
                    first = false;
                    self->frameId_ = frame.id;
                    self->stack_->setCurrentItem(item);
                    self->loadFrame(frame.id);
                    QString relative = QDir(self->controller_->rootPath()).relativeFilePath(toQ(frame.path));
                    if (!frame.path.empty() && !relative.startsWith(QStringLiteral("..")))
                        self->editors_->setExecutionLocation(relative, frame.line);
                }
            }
            self->refreshWatches();
        }, Qt::QueuedConnection);
    });
}

void DebugPanel::loadFrame(int frameId)
{
    if (!session_)
        return;
    variables_->clear();
    QPointer<DebugPanel> self(this);
    session_->scopes(frameId, [self](Result<std::vector<debugger::Scope>> scopes) {
        if (!scopes)
            return;
        auto copy = std::move(scopes.value());
        QMetaObject::invokeMethod(qApp, [self, copy] {
            if (!self || !self->session_)
                return;
            for (const auto& scope : copy) {
                auto* group = new QTreeWidgetItem(self->variables_);
                group->setText(0, toQ(scope.name));
                group->setData(0, Qt::UserRole, scope.variablesReference);
                new QTreeWidgetItem(group);
                if (!scope.expensive)
                    group->setExpanded(true);
            }
        }, Qt::QueuedConnection);
    });
}

void DebugPanel::addWatch()
{
    QString expression = watchInput_->text().trimmed();
    if (expression.isEmpty())
        return;
    watchInput_->clear();
    auto* item = new QTreeWidgetItem(watches_);
    item->setText(0, expression);
    refreshWatches();
}

void DebugPanel::refreshWatches()
{
    if (!session_ || session_->state() != debugger::SessionState::Stopped)
        return;
    for (int i = 0; i < watches_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = watches_->topLevelItem(i);
        QPointer<QTreeWidget> tree(watches_);
        session_->evaluate(fromQ(item->text(0)), frameId_, [tree, item](Result<std::string> value) {
            QString text = value ? toQ(*value) : toQ(value.error().message);
            QMetaObject::invokeMethod(qApp, [tree, item, text] { if (tree) item->setText(1, text); }, Qt::QueuedConnection);
        });
    }
}

}
