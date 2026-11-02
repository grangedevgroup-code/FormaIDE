#include "code/editor_area.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QThread>
#include <QUrl>
#include <QToolButton>
#include <QVBoxLayout>
#include <thread>

#include "ScintillaEditBase.h"
#include "app/app_context.h"
#include "forma/core/hash.h"
#include "forma/core/paths.h"
#include "forma/core/text.h"
#include "ui/theme.h"

namespace forma::ui {

EditorArea::EditorArea(ProjectController* controller, QWidget* parent) : QWidget(parent), controller_(controller)
{
    qRegisterMetaType<std::set<int>>();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    notice_ = new QFrame(this);
    notice_->setObjectName(QStringLiteral("serverNotice"));
    notice_->setProperty("role", "editorNotice");
    auto* noticeRow = new QHBoxLayout(notice_);
    noticeRow->setContentsMargins(10, 5, 8, 5);
    noticeText_ = new QLabel(notice_);
    noticeText_->setObjectName(QStringLiteral("serverNoticeText"));
    noticeText_->setWordWrap(true);
    noticeRow->addWidget(noticeText_, 1);
    noticeInstall_ = new QPushButton(tr("Install"), notice_);
    noticeInstall_->setObjectName(QStringLiteral("serverNoticeInstall"));
    noticeInstall_->setProperty("role", "primary");
    noticeRow->addWidget(noticeInstall_);
    auto* noticeLater = new QPushButton(tr("Not now"), notice_);
    noticeLater->setObjectName(QStringLiteral("serverNoticeLater"));
    noticeRow->addWidget(noticeLater);
    notice_->hide();
    layout->addWidget(notice_);
    connect(noticeInstall_, &QPushButton::clicked, this, [this] { Q_EMIT serverInstallRequested(noticeLanguageName_, noticeServer_, noticeHint_); });
    connect(noticeLater, &QPushButton::clicked, this, [this] {
        if (Tab* tab = currentTab(); tab != nullptr && tab->editor->language() != nullptr)
            dismissedNotices_.insert(tab->editor->language()->id);
        notice_->hide();
    });
    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("editorTabs"));
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    tabs_->setDocumentMode(true);
    tabs_->setElideMode(Qt::ElideMiddle);
    layout->addWidget(tabs_, 1);
    empty_ = new QWidget(this);
    auto* emptyLayout = new QVBoxLayout(empty_);
    auto* hint = new QLabel(tr("Open a file from the Files panel.\nCtrl+P finds a file by name, and Ctrl+Shift+F searches the whole project."), empty_);
    hint->setAlignment(Qt::AlignCenter);
    hint->setProperty("role", "muted");
    emptyLayout->addWidget(hint);
    layout->addWidget(empty_, 1);
    tabs_->hide();
    connect(tabs_, &QTabWidget::tabCloseRequested, this, &EditorArea::closeTab);
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int) {
        Tab* tab = currentTab();
        updateServerNotice();
        Q_EMIT currentFileChanged(tab != nullptr ? tab->path : QString());
        if (tab != nullptr) {
            QString name = tab->editor->language() != nullptr ? toQ(tab->editor->language()->name) : tr("Plain text");
            Q_EMIT cursorChanged(tab->editor->currentLine(), tab->editor->currentColumn(), name);
        }
    });
    connect(controller_, &ProjectController::fileOpenRequested, this, [this](const QString& path, int line, int column) { openFile(path, line, column); });
    connect(controller_, &ProjectController::externalFileChanged, this, &EditorArea::reloadIfUnmodified);
    connect(controller_, &ProjectController::generationFinished, this, [this](const codegen::GenerationReport& report) {
        for (const auto& file : report.files)
            reloadIfUnmodified(toQ(file.path));
    });
    connect(controller_, &ProjectController::projectClosed, this, [this] {
        stopServers();
        while (tabs_->count() > 0) {
            auto* editor = qobject_cast<CodeEditor*>(tabs_->widget(0));
            tabs_->removeTab(0);
            delete editor;
        }
        tabList_.clear();
        breakpoints_.clear();
        diagnostics_.clear();
        tabs_->hide();
        empty_->show();
    });
}

EditorArea::~EditorArea()
{
    stopServers();
}

void EditorArea::stopServers()
{
    for (auto& [id, client] : clients_)
        client->stop();
    clients_.clear();
}

EditorArea::Tab* EditorArea::tabFor(CodeEditor* editor)
{
    for (auto& tab : tabList_) {
        if (tab->editor == editor)
            return tab.get();
    }
    return nullptr;
}

EditorArea::Tab* EditorArea::currentTab()
{
    return tabFor(qobject_cast<CodeEditor*>(tabs_->currentWidget()));
}

CodeEditor* EditorArea::currentEditor() const
{
    return qobject_cast<CodeEditor*>(tabs_->currentWidget());
}

QString EditorArea::currentPath() const
{
    auto* self = const_cast<EditorArea*>(this);
    Tab* tab = self->currentTab();
    return tab != nullptr ? tab->path : QString();
}

QString EditorArea::currentAbsolutePath() const
{
    QString path = currentPath();
    return path.isEmpty() ? QString() : QDir(controller_->rootPath()).filePath(path);
}

int EditorArea::indexOfPath(const QString& path) const
{
    for (int i = 0; i < tabs_->count(); ++i) {
        if (tabs_->tabToolTip(i) == path)
            return i;
    }
    return -1;
}

int EditorArea::openCount() const
{
    return tabs_->count();
}

QStringList EditorArea::openPaths() const
{
    QStringList paths;
    for (int i = 0; i < tabs_->count(); ++i)
        paths << tabs_->tabToolTip(i);
    return paths;
}

CodeEditor* EditorArea::openFile(const QString& relativePath, int line, int column)
{
    if (!controller_->hasProject())
        return nullptr;
    QString path = relativePath;
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    int existing = indexOfPath(path);
    if (existing >= 0) {
        tabs_->setCurrentIndex(existing);
        auto* editor = qobject_cast<CodeEditor*>(tabs_->widget(existing));
        if (line > 0)
            editor->gotoLine(line, column);
        return editor;
    }
    auto content = controller_->session()->filesystem().readFile(fromQ(path));
    if (!content) {
        controller_->reportStatus(toQ(content.error().message));
        return nullptr;
    }
    if (content->find('\0') != std::string::npos) {
        controller_->reportStatus(tr("%1 is a binary file").arg(path));
        return nullptr;
    }
    auto tab = std::make_unique<Tab>();
    tab->path = path;
    tab->baseHash = sha256Hex(*content);
    auto* editor = new CodeEditor(tabs_);
    tab->editor = editor;
    auto& app = AppContext::instance();
    const editor::LanguageDefinition* language = app.languages().forPath(fromQ(path));
    if (language == nullptr)
        language = app.languages().forShebang(content->substr(0, content->find('\n')));
    if (language == nullptr)
        language = &app.languages().plainText();
    editor->setLanguage(language);
    editor->setFontPointSize(app.settings().getInt("editor.fontSize", 10));
    editor->setText(toQ(*content));
    editor->setMinimap(app.settings().getBool("editor.minimap", false));
    tab->recoveryTimer = new QTimer(editor);
    tab->recoveryTimer->setSingleShot(true);
    tab->recoveryTimer->setInterval(2000);
    tab->syncTimer = new QTimer(editor);
    tab->syncTimer->setSingleShot(true);
    tab->syncTimer->setInterval(300);
    Tab* raw = tab.get();
    connect(tab->recoveryTimer, &QTimer::timeout, this, [this, raw] {
        if (controller_->hasProject() && raw->editor->isModified()) {
            auto saved = controller_->session()->recovery().saveBuffer(fromQ(raw->path), fromQ(raw->editor->text()), raw->baseHash);
            (void)saved;
        }
    });
    connect(tab->syncTimer, &QTimer::timeout, this, [this, raw] {
        lsp::LanguageClient* client = clientFor(raw->editor->language());
        if (client != nullptr && client->ready())
            client->didChange(paths::fromUtf8(fromQ(QDir(controller_->rootPath()).filePath(raw->path))), fromQ(raw->editor->text()));
    });
    connect(editor, &CodeEditor::textEdited, this, [raw] {
        raw->recoveryTimer->start();
        raw->syncTimer->start();
    });
    connect(editor, &CodeEditor::modificationChanged, this, [this, raw](bool) { updateTabTitle(*raw); });
    connect(editor, &CodeEditor::cursorMoved, this, [this, editor](int cursorLine, int cursorColumn) {
        if (editor == currentEditor())
            Q_EMIT cursorChanged(cursorLine, cursorColumn, editor->language() != nullptr ? toQ(editor->language()->name) : tr("Plain text"));
    });
    connect(editor, &CodeEditor::completionRequested, this, [this, raw] {
        lsp::LanguageClient* client = clientFor(raw->editor->language());
        if (client == nullptr || !client->ready()) {
            raw->editor->showWordCompletions();
            return;
        }
        QPointer<CodeEditor> guard(raw->editor);
        client->completion(paths::fromUtf8(fromQ(QDir(controller_->rootPath()).filePath(raw->path))), raw->editor->cursorPosition(), [guard](Result<std::vector<lsp::CompletionItem>> items) {
            if (!items)
                return;
            auto copy = std::move(items.value());
            QMetaObject::invokeMethod(qApp, [guard, copy] { if (guard) guard->showCompletions(copy); }, Qt::QueuedConnection);
        });
    });
    connect(editor, &CodeEditor::signatureRequested, this, [this, raw] {
        lsp::LanguageClient* client = clientFor(raw->editor->language());
        if (client == nullptr || !client->ready())
            return;
        QPointer<CodeEditor> guard(raw->editor);
        client->signatureHelp(paths::fromUtf8(fromQ(QDir(controller_->rootPath()).filePath(raw->path))), raw->editor->cursorPosition(), [guard](Result<std::string> value) {
            if (!value)
                return;
            QString text = toQ(*value);
            QMetaObject::invokeMethod(qApp, [guard, text] { if (guard) guard->showCallTip(text); }, Qt::QueuedConnection);
        });
    });
    connect(editor, &CodeEditor::hoverRequested, this, [this, raw](lsp::Position position) {
        lsp::LanguageClient* client = clientFor(raw->editor->language());
        if (client == nullptr || !client->ready())
            return;
        QPointer<CodeEditor> guard(raw->editor);
        client->hover(paths::fromUtf8(fromQ(QDir(controller_->rootPath()).filePath(raw->path))), position, [guard](Result<std::string> value) {
            if (!value || value->empty())
                return;
            QString text = toQ(*value);
            QMetaObject::invokeMethod(qApp, [guard, text] { if (guard) guard->showCallTip(text); }, Qt::QueuedConnection);
        });
    });
    connect(editor, &CodeEditor::breakpointToggled, this, [this, raw](int, bool) {
        breakpoints_[raw->path] = raw->editor->breakpoints();
        Q_EMIT breakpointsChanged(raw->path, raw->editor->breakpoints());
    });
    auto stored = breakpoints_.find(path);
    if (stored != breakpoints_.end())
        editor->setBreakpoints(stored->second);
    tabs_->addTab(editor, QFileInfo(path).fileName());
    tabs_->setTabToolTip(tabs_->count() - 1, path);
    tabs_->setCurrentWidget(editor);
    tabs_->show();
    empty_->hide();
    tabList_.push_back(std::move(tab));
    attachLanguageServer(*raw);
    if (line > 0)
        editor->gotoLine(line, column);
    editor->scintilla()->setFocus();
    return editor;
}

bool EditorArea::restoreBuffer(const QString& path, const QString& content)
{
    CodeEditor* editor = openFile(path);
    if (editor == nullptr)
        return false;
    editor->applyText(content);
    return true;
}

void EditorArea::updateTabTitle(Tab& tab)
{
    int index = tabs_->indexOf(tab.editor);
    if (index < 0)
        return;
    QString name = QFileInfo(tab.path).fileName();
    if (tab.editor->isModified())
        name += QStringLiteral(" *");
    tabs_->setTabText(index, name);
}

lsp::LanguageClient* EditorArea::clientFor(const editor::LanguageDefinition* language)
{
    if (language == nullptr)
        return nullptr;
    auto it = clients_.find(language->id);
    return it == clients_.end() ? nullptr : it->second.get();
}

void EditorArea::updateServerNotice()
{
    Tab* tab = currentTab();
    const editor::LanguageDefinition* language = tab != nullptr ? tab->editor->language() : nullptr;
    if (language == nullptr || language->servers.empty() || dismissedNotices_.count(language->id) > 0 || !AppContext::instance().settings().getBool("editor.languageServers", true)) {
        notice_->hide();
        return;
    }
    lsp::ServerChoice choice = lsp::chooseServer(*language);
    if (choice.found) {
        notice_->hide();
        return;
    }
    noticeLanguageName_ = toQ(language->name);
    noticeServer_ = toQ(language->servers.front().command.front());
    noticeHint_ = toQ(choice.install);
    build::InstallPlan plan = build::planServerInstall(language->name, language->servers.front().command.front(), choice.install);
    noticeInstall_->setVisible(plan.automatic());
    noticeText_->setText(plan.automatic() ? tr("Autocomplete, error checking and go to definition for %1 need its language server. Word completion still works without it.").arg(noticeLanguageName_)
                                          : tr("Autocomplete and error checking for %1 need its language server: %2. Word completion still works without it.").arg(noticeLanguageName_, noticeHint_));
    notice_->show();
}

QString EditorArea::serverNotice() const
{
    return notice_->isHidden() ? QString() : noticeText_->text();
}

void EditorArea::retryLanguageServers()
{
    for (auto& tab : tabList_)
        attachLanguageServer(*tab);
    updateServerNotice();
}

void EditorArea::attachLanguageServer(Tab& tab)
{
    const editor::LanguageDefinition* language = tab.editor->language();
    if (language == nullptr || language->servers.empty() || !AppContext::instance().settings().getBool("editor.languageServers", true))
        return;
    lsp::LanguageClient* client = clientFor(language);
    QString absolute = QDir(controller_->rootPath()).filePath(tab.path);
    if (client == nullptr) {
        lsp::ServerChoice choice = lsp::chooseServer(*language);
        if (!choice.found) {
            serverStatus_ = tr("No language server for %1").arg(toQ(language->name));
            Q_EMIT serverStatusChanged(serverStatus_);
            updateServerNotice();
            return;
        }
        auto created = std::make_unique<lsp::LanguageClient>(controller_->session()->root(), language->id);
        QPointer<EditorArea> self(this);
        created->setDiagnosticsCallback([self](const std::string& path, const std::vector<lsp::Diagnostic>& list) {
            QString file = toQ(path);
            std::vector<lsp::Diagnostic> copy = list;
            QMetaObject::invokeMethod(qApp, [self, file, copy] {
                if (!self || !self->controller_->hasProject())
                    return;
                QString relative = QDir(self->controller_->rootPath()).relativeFilePath(file);
                self->diagnostics_[relative] = copy;
                int index = self->indexOfPath(relative);
                if (index >= 0)
                    qobject_cast<CodeEditor*>(self->tabs_->widget(index))->setDiagnostics(copy);
                std::vector<ProblemItem> items;
                for (const auto& diagnostic : copy) {
                    ProblemItem item;
                    item.source = toQ(diagnostic.source.empty() ? std::string("language server") : diagnostic.source);
                    item.severity = diagnostic.severity == 1 ? 2 : diagnostic.severity == 2 ? 1 : 0;
                    item.file = relative;
                    item.line = diagnostic.range.start.line + 1;
                    item.column = diagnostic.range.start.character + 1;
                    item.message = toQ(diagnostic.message);
                    items.push_back(std::move(item));
                }
                self->controller_->setEditorProblems(relative, std::move(items));
            }, Qt::QueuedConnection);
        });
        QString name = toQ(language->name);
        QString server = toQ(choice.command.front());
        auto started = created->start(choice.command, [self, name, server] {
            QMetaObject::invokeMethod(qApp, [self, name, server] {
                if (self) {
                    self->serverStatus_ = tr("%1 language server is running").arg(name);
                    Q_EMIT self->serverStatusChanged(self->serverStatus_);
                }
            }, Qt::QueuedConnection);
        });
        if (!started) {
            serverStatus_ = toQ(started.error().message);
            Q_EMIT serverStatusChanged(serverStatus_);
            return;
        }
        client = created.get();
        clients_[language->id] = std::move(created);
    }
    client->didOpen(paths::fromUtf8(fromQ(absolute)), fromQ(tab.editor->text()));
}

bool EditorArea::saveTab(Tab& tab)
{
    if (!controller_->hasProject())
        return false;
    std::string contents = fromQ(tab.editor->text());
    WriteOptions options;
    options.origin = Origin::User;
    options.expectedHash = tab.baseHash;
    auto status = controller_->session()->filesystem().writeFile(fromQ(tab.path), contents, options);
    if (!status && status.error().code == "fs.conflict") {
        auto answer = QMessageBox::warning(this, tr("File changed on disk"), tr("%1 changed outside the editor since you opened it. Overwrite the version on disk? The old version is kept in the project's backups.").arg(tab.path), QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Save)
            return false;
        options.expectedHash.reset();
        status = controller_->session()->filesystem().writeFile(fromQ(tab.path), contents, options);
    }
    if (!status) {
        controller_->reportStatus(toQ(status.error().message));
        return false;
    }
    tab.baseHash = sha256Hex(contents);
    tab.editor->markSaved();
    auto cleared = controller_->session()->recovery().clearBuffer(fromQ(tab.path));
    (void)cleared;
    if (lsp::LanguageClient* client = clientFor(tab.editor->language()))
        client->didSave(paths::fromUtf8(fromQ(QDir(controller_->rootPath()).filePath(tab.path))), contents);
    updateTabTitle(tab);
    if (controller_->target() != nullptr && !controller_->target()->generator.empty())
        controller_->reconcileCode();
    return true;
}

bool EditorArea::saveCurrent()
{
    Tab* tab = currentTab();
    return tab != nullptr && saveTab(*tab);
}

bool EditorArea::saveAll()
{
    bool all = true;
    for (auto& tab : tabList_) {
        if (tab->editor->isModified())
            all = saveTab(*tab) && all;
    }
    return all;
}

bool EditorArea::closeTab(int index)
{
    auto* editor = qobject_cast<CodeEditor*>(tabs_->widget(index));
    Tab* tab = tabFor(editor);
    if (tab == nullptr)
        return true;
    if (editor->isModified()) {
        auto answer = QMessageBox::question(this, tr("Unsaved changes"), tr("Save changes to %1?").arg(tab->path), QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (answer == QMessageBox::Cancel)
            return false;
        if (answer == QMessageBox::Save && !saveTab(*tab))
            return false;
    }
    if (controller_->hasProject()) {
        auto cleared = controller_->session()->recovery().clearBuffer(fromQ(tab->path));
        (void)cleared;
    }
    if (lsp::LanguageClient* client = clientFor(editor->language()))
        client->didClose(paths::fromUtf8(fromQ(QDir(controller_->rootPath()).filePath(tab->path))));
    tabs_->removeTab(index);
    tabList_.erase(std::remove_if(tabList_.begin(), tabList_.end(), [&](const auto& item) { return item->editor == editor; }), tabList_.end());
    delete editor;
    if (tabs_->count() == 0) {
        tabs_->hide();
        empty_->show();
    }
    return true;
}

void EditorArea::closeCurrent()
{
    if (tabs_->currentIndex() >= 0)
        closeTab(tabs_->currentIndex());
}

bool EditorArea::closeAll()
{
    while (tabs_->count() > 0) {
        if (!closeTab(0))
            return false;
    }
    return true;
}

void EditorArea::reloadIfUnmodified(const QString& relativePath)
{
    int index = indexOfPath(relativePath);
    if (index < 0 || !controller_->hasProject())
        return;
    auto* editor = qobject_cast<CodeEditor*>(tabs_->widget(index));
    Tab* tab = tabFor(editor);
    auto content = controller_->session()->filesystem().readFile(fromQ(relativePath));
    if (!content)
        return;
    std::string hash = sha256Hex(*content);
    if (hash == tab->baseHash)
        return;
    if (editor->isModified()) {
        controller_->reportStatus(tr("%1 changed on disk. You have unsaved edits, so it was not reloaded.").arg(relativePath));
        return;
    }
    tab->baseHash = hash;
    editor->setText(toQ(*content), true);
    if (lsp::LanguageClient* client = clientFor(editor->language()))
        client->didChange(paths::fromUtf8(fromQ(QDir(controller_->rootPath()).filePath(relativePath))), *content);
}

void EditorArea::goToDefinition()
{
    Tab* tab = currentTab();
    if (tab == nullptr)
        return;
    lsp::LanguageClient* client = clientFor(tab->editor->language());
    if (client == nullptr || !client->ready()) {
        controller_->reportStatus(serverStatus_.isEmpty() ? tr("No language server is running for this file") : serverStatus_);
        return;
    }
    QPointer<EditorArea> self(this);
    client->definition(paths::fromUtf8(fromQ(currentAbsolutePath())), tab->editor->cursorPosition(), [self](Result<std::vector<lsp::Location>> locations) {
        if (!locations || locations->empty())
            return;
        lsp::Location first = locations->front();
        QMetaObject::invokeMethod(qApp, [self, first] {
            if (!self)
                return;
            QString relative = QDir(self->controller_->rootPath()).relativeFilePath(toQ(first.path));
            if (relative.startsWith(QStringLiteral("..")))
                self->controller_->reportStatus(tr("The definition is outside the project: %1").arg(toQ(first.path)));
            else
                self->openFile(relative, first.range.start.line + 1, first.range.start.character + 1);
        }, Qt::QueuedConnection);
    });
}

void EditorArea::findReferences()
{
    Tab* tab = currentTab();
    if (tab == nullptr)
        return;
    lsp::LanguageClient* client = clientFor(tab->editor->language());
    if (client == nullptr || !client->ready()) {
        controller_->reportStatus(serverStatus_.isEmpty() ? tr("No language server is running for this file") : serverStatus_);
        return;
    }
    QPointer<EditorArea> self(this);
    client->references(paths::fromUtf8(fromQ(currentAbsolutePath())), tab->editor->cursorPosition(), [self](Result<std::vector<lsp::Location>> locations) {
        if (!locations)
            return;
        auto copy = std::move(locations.value());
        QMetaObject::invokeMethod(qApp, [self, copy] {
            if (self)
                Q_EMIT self->referencesFound(tr("References"), copy);
        }, Qt::QueuedConnection);
    });
}

void EditorArea::renameSymbol()
{
    Tab* tab = currentTab();
    if (tab == nullptr)
        return;
    lsp::LanguageClient* client = clientFor(tab->editor->language());
    if (client == nullptr || !client->ready()) {
        controller_->reportStatus(serverStatus_.isEmpty() ? tr("No language server is running for this file") : serverStatus_);
        return;
    }
    bool accepted = false;
    QString name = QInputDialog::getText(this, tr("Rename symbol"), tr("New name"), QLineEdit::Normal, QString(), &accepted);
    if (!accepted || name.isEmpty())
        return;
    QPointer<EditorArea> self(this);
    client->rename(paths::fromUtf8(fromQ(currentAbsolutePath())), tab->editor->cursorPosition(), fromQ(name), [self](Result<lsp::WorkspaceEdit> edit) {
        if (!edit)
            return;
        auto copy = std::move(edit.value());
        QMetaObject::invokeMethod(qApp, [self, copy] {
            if (!self)
                return;
            for (const auto& [file, edits] : copy.files) {
                QString relative = QDir(self->controller_->rootPath()).relativeFilePath(toQ(file));
                if (relative.startsWith(QStringLiteral("..")))
                    continue;
                CodeEditor* editor = self->openFile(relative);
                if (editor != nullptr)
                    editor->applyText(toQ(lsp::applyTextEdits(fromQ(editor->text()), edits)));
            }
            self->controller_->reportStatus(tr("Renamed in %n file(s). Review and save.", nullptr, static_cast<int>(copy.files.size())));
        }, Qt::QueuedConnection);
    });
}

void EditorArea::formatDocument()
{
    Tab* tab = currentTab();
    if (tab == nullptr)
        return;
    lsp::LanguageClient* client = clientFor(tab->editor->language());
    if (client != nullptr && client->ready()) {
        QPointer<CodeEditor> guard(tab->editor);
        const editor::LanguageDefinition* language = tab->editor->language();
        client->formatting(paths::fromUtf8(fromQ(currentAbsolutePath())), language->indent, !language->tabs, [guard](Result<std::vector<lsp::TextEdit>> edits) {
            if (!edits)
                return;
            auto copy = std::move(edits.value());
            QMetaObject::invokeMethod(qApp, [guard, copy] {
                if (guard)
                    guard->applyText(toQ(lsp::applyTextEdits(fromQ(guard->text()), copy)));
            }, Qt::QueuedConnection);
        });
        return;
    }
    const editor::LanguageDefinition* language = tab->editor->language();
    if (language == nullptr || language->formatter.empty()) {
        controller_->reportStatus(tr("No formatter is known for this language"));
        return;
    }
    if (!findExecutable(language->formatter.front())) {
        controller_->reportStatus(tr("The formatter %1 is not installed").arg(toQ(language->formatter.front())));
        return;
    }
    if (!saveTab(*tab))
        return;
    QStringList arguments;
    for (std::size_t i = 1; i < language->formatter.size(); ++i)
        arguments << toQ(text::replaceAll(language->formatter[i], "${file}", fromQ(currentAbsolutePath())));
    QProcess process;
    process.setWorkingDirectory(controller_->rootPath());
    process.start(toQ(language->formatter.front()), arguments);
    process.waitForFinished(30000);
    reloadIfUnmodified(tab->path);
}

void EditorArea::runCurrentFile()
{
    Tab* tab = currentTab();
    if (tab == nullptr || tab->editor->language() == nullptr || tab->editor->language()->run.empty()) {
        controller_->reportStatus(tr("FormaIDE doesn't know how to run this kind of file"));
        return;
    }
    saveTab(*tab);
    QString command;
    for (const auto& part : tab->editor->language()->run) {
        QString piece = toQ(text::replaceAll(part, "${file}", fromQ(currentPath())));
        if (!command.isEmpty())
            command += QLatin1Char(' ');
        command += piece.contains(QLatin1Char(' ')) ? QLatin1Char('"') + piece + QLatin1Char('"') : piece;
    }
    Q_EMIT automationRequested(command);
}

void EditorArea::setExecutionLocation(const QString& path, int line)
{
    clearExecutionLocation();
    CodeEditor* editor = openFile(path, line);
    if (editor != nullptr) {
        editor->setExecutionLine(line);
        executionPath_ = path;
    }
}

void EditorArea::clearExecutionLocation()
{
    if (executionPath_.isEmpty())
        return;
    int index = indexOfPath(executionPath_);
    if (index >= 0)
        qobject_cast<CodeEditor*>(tabs_->widget(index))->setExecutionLine(0);
    executionPath_.clear();
}

void EditorArea::gotoComponentAtCursor()
{
    Tab* tab = currentTab();
    if (tab == nullptr || !controller_->hasProject() || controller_->target() == nullptr)
        return;
    auto reference = codegen::componentAtLine(controller_->session()->filesystem(), controller_->target()->id, fromQ(tab->path), tab->editor->currentLine());
    if (!reference) {
        controller_->reportStatus(tr("This line doesn't belong to a component on the canvas"));
        return;
    }
    Q_EMIT componentFocusRequested(toQ(reference->screen), toQ(reference->component));
}

class ExplorerFilter : public QSortFilterProxyModel {
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    QString needle;

protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override
    {
        auto* source = qobject_cast<QFileSystemModel*>(sourceModel());
        QModelIndex index = source->index(row, 0, parent);
        QString name = source->fileName(index);
        const auto& ignored = defaultIgnoredNames();
        if (name != QStringLiteral("bin") && name != QStringLiteral("vendor") && name != QStringLiteral("out") && name != QStringLiteral("dist")) {
            for (const auto& item : ignored) {
                if (name == toQ(item))
                    return false;
            }
        }
        if (needle.isEmpty() || source->isDir(index))
            return true;
        return name.contains(needle, Qt::CaseInsensitive);
    }

    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override
    {
        auto* source = qobject_cast<QFileSystemModel*>(sourceModel());
        bool leftDir = source->isDir(left);
        bool rightDir = source->isDir(right);
        if (leftDir != rightDir)
            return leftDir;
        return source->fileName(left).compare(source->fileName(right), Qt::CaseInsensitive) < 0;
    }
};

FileExplorer::FileExplorer(ProjectController* controller, QWidget* parent) : QWidget(parent), controller_(controller)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    filter_ = new QLineEdit(this);
    filter_->setPlaceholderText(tr("Filter files"));
    filter_->setClearButtonEnabled(true);
    auto* holder = new QHBoxLayout();
    holder->setContentsMargins(6, 4, 6, 4);
    holder->setSpacing(2);
    holder->addWidget(filter_, 1);
    auto* addFile = new QToolButton(this);
    addFile->setObjectName(QStringLiteral("newFileButton"));
    addFile->setIcon(icon(QStringLiteral("file")));
    addFile->setToolTip(tr("New file"));
    holder->addWidget(addFile);
    auto* addFolder = new QToolButton(this);
    addFolder->setObjectName(QStringLiteral("newFolderButton"));
    addFolder->setIcon(icon(QStringLiteral("folder")));
    addFolder->setToolTip(tr("New folder"));
    holder->addWidget(addFolder);
    connect(addFile, &QToolButton::clicked, this, [this] {
        if (controller_->hasProject())
            newFile(controller_->rootPath());
    });
    connect(addFolder, &QToolButton::clicked, this, [this] {
        if (controller_->hasProject())
            newFolder(controller_->rootPath());
    });
    layout->addLayout(holder);
    model_ = new QFileSystemModel(this);
    model_->setReadOnly(true);
    model_->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    auto* filterModel = new ExplorerFilter(this);
    proxy_ = filterModel;
    proxy_->setSourceModel(model_);
    proxy_->setDynamicSortFilter(true);
    proxy_->setRecursiveFilteringEnabled(false);
    view_ = new QTreeView(this);
    view_->setObjectName(QStringLiteral("fileTree"));
    view_->setModel(proxy_);
    view_->setHeaderHidden(true);
    for (int column = 1; column < 4; ++column)
        view_->setColumnHidden(column, true);
    view_->setIndentation(14);
    view_->setAnimated(false);
    view_->setContextMenuPolicy(Qt::CustomContextMenu);
    view_->setSortingEnabled(true);
    view_->sortByColumn(0, Qt::AscendingOrder);
    layout->addWidget(view_, 1);
    connect(filter_, &QLineEdit::textChanged, this, [this, filterModel](const QString& value) {
        filterModel->needle = value;
        filterModel->invalidate();
    });
    auto openIndex = [this](const QModelIndex& index) {
        QModelIndex source = proxy_->mapToSource(index);
        if (model_->isDir(source))
            return;
        controller_->openFile(QDir(controller_->rootPath()).relativeFilePath(model_->filePath(source)));
    };
    connect(view_, &QTreeView::activated, this, openIndex);
    connect(view_, &QTreeView::clicked, this, openIndex);
    connect(view_, &QWidget::customContextMenuRequested, this, &FileExplorer::showMenu);
    connect(controller_, &ProjectController::projectOpened, this, [this] { setRoot(controller_->rootPath()); });
    connect(controller_, &ProjectController::projectClosed, this, [this] { setRoot(QString()); });
}

void FileExplorer::setRoot(const QString& path)
{
    if (path.isEmpty()) {
        view_->setRootIndex(QModelIndex());
        return;
    }
    QModelIndex root = model_->setRootPath(path);
    view_->setRootIndex(proxy_->mapFromSource(root));
}

void FileExplorer::newFile(const QString& directory)
{
    bool accepted = false;
    QString name = QInputDialog::getText(this, tr("New file"), tr("File name"), QLineEdit::Normal, QString(), &accepted);
    if (!accepted || name.isEmpty())
        return;
    QString relative = QDir(controller_->rootPath()).relativeFilePath(QDir(directory).filePath(name));
    auto status = controller_->session()->filesystem().writeFile(fromQ(relative), "");
    if (!status) {
        controller_->reportStatus(toQ(status.error().message));
        return;
    }
    controller_->openFile(relative);
}

void FileExplorer::newFolder(const QString& directory)
{
    bool accepted = false;
    QString name = QInputDialog::getText(this, tr("New folder"), tr("Folder name"), QLineEdit::Normal, QString(), &accepted);
    if (!accepted || name.isEmpty())
        return;
    auto status = controller_->session()->filesystem().createDirectories(fromQ(QDir(controller_->rootPath()).relativeFilePath(QDir(directory).filePath(name))));
    if (!status)
        controller_->reportStatus(toQ(status.error().message));
}

void FileExplorer::showMenu(const QPoint& position)
{
    if (!controller_->hasProject())
        return;
    QModelIndex proxyIndex = view_->indexAt(position);
    QModelIndex index = proxy_->mapToSource(proxyIndex);
    QString path = index.isValid() ? model_->filePath(index) : controller_->rootPath();
    bool isDirectory = !index.isValid() || model_->isDir(index);
    QString directory = isDirectory ? path : QFileInfo(path).absolutePath();
    QString relative = QDir(controller_->rootPath()).relativeFilePath(path);
    QMenu menu(this);
    menu.addAction(icon(QStringLiteral("file")), tr("New file..."), this, [this, directory] { newFile(directory); });
    menu.addAction(icon(QStringLiteral("folder")), tr("New folder..."), this, [this, directory] { newFolder(directory); });
    if (index.isValid()) {
        menu.addSeparator();
        menu.addAction(tr("Rename..."), this, [this, relative, path] {
            bool accepted = false;
            QString name = QInputDialog::getText(this, tr("Rename"), tr("New name"), QLineEdit::Normal, QFileInfo(path).fileName(), &accepted);
            if (!accepted || name.isEmpty())
                return;
            QString target = QDir(QFileInfo(relative).path()).filePath(name);
            auto status = controller_->session()->filesystem().rename(fromQ(relative), fromQ(QDir::cleanPath(target)));
            if (!status)
                controller_->reportStatus(toQ(status.error().message));
        });
        menu.addAction(icon(QStringLiteral("trash")), tr("Move to project trash"), this, [this, relative] {
            if (QMessageBox::question(this, tr("Delete"), tr("Move %1 to the project's trash? You can restore it from History.").arg(relative)) != QMessageBox::Yes)
                return;
            auto removed = controller_->session()->filesystem().remove(fromQ(relative));
            if (!removed)
                controller_->reportStatus(toQ(removed.error().message));
        });
        menu.addSeparator();
        menu.addAction(tr("Copy path"), this, [path] { QApplication::clipboard()->setText(QDir::toNativeSeparators(path)); });
    }
    menu.addAction(tr("Show in file manager"), this, [directory] { QDesktopServices::openUrl(QUrl::fromLocalFile(directory)); });
    menu.exec(view_->viewport()->mapToGlobal(position));
}

SearchPanel::SearchPanel(ProjectController* controller, QWidget* parent) : QWidget(parent), controller_(controller)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 0);
    layout->setSpacing(4);
    query_ = new QLineEdit(this);
    query_->setObjectName(QStringLiteral("searchQuery"));
    query_->setPlaceholderText(tr("Search the project"));
    query_->setClearButtonEnabled(true);
    layout->addWidget(query_);
    auto* options = new QHBoxLayout();
    matchCase_ = new QCheckBox(tr("Match case"), this);
    regex_ = new QCheckBox(tr("Regular expression"), this);
    options->addWidget(matchCase_);
    options->addWidget(regex_);
    options->addStretch(1);
    layout->addLayout(options);
    status_ = new QLabel(this);
    status_->setProperty("role", "muted");
    layout->addWidget(status_);
    results_ = new QTreeWidget(this);
    results_->setObjectName(QStringLiteral("searchResults"));
    results_->setHeaderHidden(true);
    results_->setIndentation(12);
    results_->setUniformRowHeights(true);
    layout->addWidget(results_, 1);
    connect(query_, &QLineEdit::returnPressed, this, &SearchPanel::runSearch);
    connect(results_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        QString file = item->data(0, Qt::UserRole).toString();
        if (!file.isEmpty())
            controller_->openFile(file, item->data(0, Qt::UserRole + 1).toInt(), item->data(0, Qt::UserRole + 2).toInt());
    });
    connect(results_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
        QString file = item->data(0, Qt::UserRole).toString();
        int line = item->data(0, Qt::UserRole + 1).toInt();
        if (!file.isEmpty() && line > 0)
            controller_->openFile(file, line, item->data(0, Qt::UserRole + 2).toInt());
    });
}

void SearchPanel::focusSearch()
{
    query_->setFocus();
    query_->selectAll();
}

void SearchPanel::runSearch()
{
    if (!controller_->hasProject() || query_->text().isEmpty())
        return;
    if (cancel_)
        *cancel_ = true;
    cancel_ = std::make_shared<std::atomic<bool>>(false);
    auto cancelled = cancel_;
    results_->clear();
    status_->setText(tr("Searching..."));
    QString root = controller_->rootPath();
    QString query = query_->text();
    bool matchCase = matchCase_->isChecked();
    bool useRegex = regex_->isChecked();
    QPointer<SearchPanel> self(this);
    std::thread([self, root, query, matchCase, useRegex, cancelled] {
        auto filesystem = createLocalFilesystemService(paths::fromUtf8(fromQ(root)));
        ListOptions options;
        options.recursive = true;
        options.ignoredNames = defaultIgnoredNames();
        auto entries = filesystem->list("", options);
        QRegularExpression expression(useRegex ? query : QRegularExpression::escape(query), matchCase ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption);
        struct Hit {
            int line;
            int column;
            QString text;
        };
        int total = 0;
        int files = 0;
        if (entries && expression.isValid()) {
            for (const auto& entry : *entries) {
                if (cancelled->load())
                    return;
                if (entry.directory || entry.size > 4 * 1024 * 1024)
                    continue;
                auto content = filesystem->readFile(entry.path);
                if (!content || content->find('\0') != std::string::npos)
                    continue;
                std::vector<Hit> hits;
                int lineNumber = 0;
                for (const auto& line : text::splitLines(*content)) {
                    ++lineNumber;
                    QString value = toQ(line);
                    auto match = expression.match(value);
                    if (match.hasMatch() && hits.size() < 200)
                        hits.push_back({lineNumber, static_cast<int>(match.capturedStart()) + 1, value.trimmed().left(240)});
                }
                if (hits.empty())
                    continue;
                total += static_cast<int>(hits.size());
                ++files;
                QString path = toQ(entry.path);
                QMetaObject::invokeMethod(qApp, [self, path, hits, cancelled] {
                    if (!self || cancelled->load())
                        return;
                    auto* parent = new QTreeWidgetItem(self->results_);
                    parent->setText(0, QStringLiteral("%1 (%2)").arg(path).arg(hits.size()));
                    parent->setIcon(0, icon(QStringLiteral("file")));
                    for (const auto& hit : hits) {
                        auto* item = new QTreeWidgetItem(parent);
                        item->setText(0, QStringLiteral("%1: %2").arg(hit.line).arg(hit.text));
                        item->setData(0, Qt::UserRole, path);
                        item->setData(0, Qt::UserRole + 1, hit.line);
                        item->setData(0, Qt::UserRole + 2, hit.column);
                    }
                    parent->setExpanded(true);
                }, Qt::QueuedConnection);
            }
        }
        QMetaObject::invokeMethod(qApp, [self, total, files, cancelled] {
            if (self && !cancelled->load())
                self->status_->setText(tr("%1 result(s) in %2 file(s)").arg(total).arg(files));
        }, Qt::QueuedConnection);
    }).detach();
}

void SearchPanel::showLocations(const QString& title, const std::vector<lsp::Location>& locations)
{
    results_->clear();
    status_->setText(QStringLiteral("%1: %2").arg(title).arg(locations.size()));
    std::map<QString, QTreeWidgetItem*> groups;
    for (const auto& location : locations) {
        QString relative = QDir(controller_->rootPath()).relativeFilePath(toQ(location.path));
        QTreeWidgetItem*& parent = groups[relative];
        if (parent == nullptr) {
            parent = new QTreeWidgetItem(results_);
            parent->setText(0, relative);
            parent->setIcon(0, icon(QStringLiteral("file")));
        }
        auto* item = new QTreeWidgetItem(parent);
        item->setText(0, tr("Line %1").arg(location.range.start.line + 1));
        item->setData(0, Qt::UserRole, relative);
        item->setData(0, Qt::UserRole + 1, location.range.start.line + 1);
        item->setData(0, Qt::UserRole + 2, location.range.start.character + 1);
    }
    results_->expandAll();
}

}
