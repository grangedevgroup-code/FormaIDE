#include "app/project_controller.h"

#include <QDir>
#include <QFileInfo>
#include <QPointer>

#include "app/app_context.h"
#include "forma/core/log.h"
#include "forma/core/paths.h"
#include "forma/core/text.h"
#include "forma/ir/validation.h"

namespace forma::ui {

ProjectController::ProjectController(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<forma::ir::ChangeEvent>();
    qRegisterMetaType<forma::codegen::GenerationReport>();
    autosaveTimer_.setInterval(5000);
    connect(&autosaveTimer_, &QTimer::timeout, this, [this] {
        if (session_) {
            auto status = session_->autosaveDesign();
            (void)status;
        }
    });
    watchDebounce_.setSingleShot(true);
    watchDebounce_.setInterval(300);
    connect(&watchDebounce_, &QTimer::timeout, this, [this] {
        QStringList paths = pendingChanges_;
        pendingChanges_.clear();
        paths.removeDuplicates();
        Q_EMIT filesChanged(paths);
    });
    validationTimer_.setSingleShot(true);
    validationTimer_.setInterval(250);
    connect(&validationTimer_, &QTimer::timeout, this, &ProjectController::refreshValidation);
}

ProjectController::~ProjectController()
{
    closeProject();
}

QString ProjectController::rootPath() const
{
    return session_ ? toQ(paths::toUtf8(session_->root())) : QString();
}

void ProjectController::attachSession()
{
    listenerId_ = session_->editor().addListener([this](const ir::ChangeEvent& event) {
        Q_EMIT modelChanged(event);
        validationTimer_.start();
    });
    const auto& screens = session_->model().screens;
    currentScreen_ = screens.empty() ? QString() : toQ(screens.front().root.id);
    selection_.clear();
    watcher_ = createFileWatcher();
    QPointer<ProjectController> self(this);
    auto started = watcher_->start(session_->root(), [self](const std::vector<FileChange>& changes) {
        if (!self)
            return;
        std::vector<FileChange> copy = changes;
        QMetaObject::invokeMethod(self.data(), [self, copy] {
            if (self)
                self->onFilesChangedExternally(copy);
        }, Qt::QueuedConnection);
    });
    if (!started)
        log::warning("project", "file watching is off", Json{{"reason", started.error().message}});
    autosaveTimer_.start();
    AppContext::instance().recents().add(session_->root(), session_->descriptor().name);
    refreshValidation();
    Q_EMIT projectOpened();
    auto buffers = session_->recovery().buffers();
    bool design = session_->recovery().previousSessionCrashed() && session_->recovery().designState().has_value();
    int bufferCount = buffers && session_->recovery().previousSessionCrashed() ? static_cast<int>(buffers->size()) : 0;
    if (design || bufferCount > 0)
        Q_EMIT recoveryAvailable(bufferCount, design);
}

Status ProjectController::createProject(const project::NewProjectOptions& options)
{
    closeProject();
    auto& app = AppContext::instance();
    auto session = project::ProjectSession::create(options, app.templates(), app.generators());
    if (!session)
        return session.error();
    session_ = std::move(session.value());
    attachSession();
    return {};
}

Status ProjectController::openProject(const QString& directory)
{
    closeProject();
    auto session = project::ProjectSession::open(paths::fromUtf8(fromQ(directory)));
    if (!session)
        return session.error();
    session_ = std::move(session.value());
    attachSession();
    return {};
}

Status ProjectController::importFolder(const QString& directory)
{
    closeProject();
    auto session = project::ProjectSession::importExisting(paths::fromUtf8(fromQ(directory)));
    if (!session)
        return session.error();
    session_ = std::move(session.value());
    attachSession();
    return {};
}

void ProjectController::closeProject()
{
    if (!session_)
        return;
    stopApp();
    cancelBuild();
    if (buildJob_) {
        buildJob_->wait();
        buildJob_.reset();
    }
    autosaveTimer_.stop();
    if (watcher_) {
        watcher_->stop();
        watcher_.reset();
    }
    auto status = session_->saveAll();
    (void)status;
    session_->editor().removeListener(listenerId_);
    session_.reset();
    currentScreen_.clear();
    selection_.clear();
    buildProblems_.clear();
    designProblems_.clear();
    editorProblems_.clear();
    artifacts_.clear();
    Q_EMIT projectClosed();
}

Status ProjectController::saveAll()
{
    if (!session_)
        return {};
    return session_->saveAll();
}

void ProjectController::setCurrentScreen(const QString& id)
{
    if (id == currentScreen_)
        return;
    currentScreen_ = id;
    selection_.clear();
    Q_EMIT screenChanged();
    Q_EMIT selectionChanged();
}

void ProjectController::setSelection(const QStringList& ids)
{
    if (ids == selection_)
        return;
    selection_ = ids;
    Q_EMIT selectionChanged();
}

void ProjectController::selectComponent(const QString& screen, const QString& component)
{
    if (screen != currentScreen_) {
        currentScreen_ = screen;
        Q_EMIT screenChanged();
    }
    selection_ = {component};
    Q_EMIT selectionChanged();
}

void ProjectController::regenerate(bool quiet)
{
    if (!session_ || target() == nullptr || target()->generator.empty())
        return;
    auto saved = session_->saveDesign();
    (void)saved;
    auto analysis = session_->reconcileFromCode();
    if (analysis && !analysis->updates.empty()) {
        Q_EMIT syncFinished(tr("%n design value(s) updated from code", nullptr, static_cast<int>(analysis->updates.size())));
        auto savedAgain = session_->saveDesign();
        (void)savedAgain;
    }
    lastReport_ = session_->generate(AppContext::instance().generators());
    Q_EMIT generationFinished(lastReport_);
    if (lastReport_.blockedByErrors)
        reportStatus(tr("Code was not generated: fix the errors in Problems"));
    else if (lastReport_.hasConflicts())
        reportStatus(tr("Generated code with %n conflict(s) to review", nullptr, lastReport_.count(codegen::FileOutcome::Conflict)));
    else if (!quiet)
        reportStatus(tr("Code is up to date"));
    QStringList changed;
    for (const auto& file : lastReport_.files)
        changed << toQ(file.path);
    Q_EMIT filesChanged(changed);
}

void ProjectController::reconcileCode()
{
    if (!session_ || target() == nullptr)
        return;
    auto analysis = session_->reconcileFromCode();
    if (!analysis) {
        reportStatus(toQ(analysis.error().message));
        return;
    }
    QString summary;
    if (!analysis->anythingChanged())
        summary = tr("Design and code already match");
    else
        summary = tr("%1 value(s) moved to the design, %2 edit(s) kept as code, %3 changed on both sides")
                      .arg(analysis->updates.size()).arg(analysis->unmapped.size()).arg(analysis->conflicts.size());
    Q_EMIT syncFinished(summary);
    reportStatus(summary);
}

void ProjectController::resolveConflict(const QString& path, codegen::ConflictChoice choice)
{
    if (!session_)
        return;
    auto status = session_->resolveGenerationConflict(AppContext::instance().generators(), fromQ(path), choice);
    if (!status)
        reportStatus(toQ(status.error().message));
    regenerate(true);
}

void ProjectController::refreshValidation()
{
    if (!session_)
        return;
    designProblems_.clear();
    for (const auto& diagnostic : ir::validateModel(session_->model(), AppContext::instance().components())) {
        ProblemItem item;
        item.source = tr("Design");
        item.severity = diagnostic.severity == ir::Severity::Error ? 2 : diagnostic.severity == ir::Severity::Warning ? 1 : 0;
        item.message = toQ(diagnostic.message);
        item.screen = toQ(diagnostic.screen);
        item.component = toQ(diagnostic.component);
        designProblems_.push_back(std::move(item));
    }
    Q_EMIT problemsChanged();
}

build::BuildRequest ProjectController::requestFor(const QString& configuration) const
{
    build::BuildRequest request;
    const project::TargetConfig* active = target();
    request.projectDirectory = session_->targetDirectory(*active);
    std::string id = fromQ(configuration);
    request.configuration.id = id;
    request.configuration.kind = id;
    for (const auto& item : session_->model().buildConfigurations) {
        if (item.id == id) {
            request.configuration.kind = item.kind;
            request.configuration.architecture = item.architecture;
            request.configuration.minimumOs = item.minimumOs;
            request.configuration.compiler = item.compiler;
            request.configuration.optimization = item.optimization;
            request.configuration.flags = item.flags;
            request.configuration.environment = item.environment;
            request.configuration.outputDirectory = item.outputDirectory;
        }
    }
    if (!active->platforms.empty()) {
        request.targetPlatform = active->platforms.front();
        for (const auto& platform : active->platforms) {
            if (platform == build::hostPlatform())
                request.targetPlatform = platform;
        }
    }
    return request;
}

void ProjectController::startBuild(const QString& configuration, std::function<void()> then)
{
    if (!session_ || target() == nullptr)
        return;
    if (buildRunning()) {
        reportStatus(tr("A build is already running"));
        return;
    }
    if (!target()->generator.empty()) {
        regenerate(true);
        if (lastReport_.blockedByErrors)
            return;
    }
    auto& app = AppContext::instance();
    const build::IBuildProvider* provider = session_->buildProviderFor(app.builders(), *target());
    if (provider == nullptr) {
        Q_EMIT buildStarted();
        Q_EMIT buildOutput(tr("No build system was recognized in this folder. FormaIDE looks for files such as CMakeLists.txt, build.gradle.kts, package.json or Cargo.toml."), true);
        Q_EMIT buildFinished(false, 1, 0, 0);
        return;
    }
    build::BuildRequest request = requestFor(configuration);
    Q_EMIT buildStarted();
    buildProblems_.clear();
    Q_EMIT problemsChanged();
    auto plan = provider->plan(request, app.tools());
    if (!plan) {
        Q_EMIT buildOutput(toQ(plan.error().message), true);
        Q_EMIT buildFinished(false, 1, 0, 0);
        return;
    }
    for (const auto& step : plan->steps) {
        QString command = toQ(step.process.program);
        for (const auto& argument : step.process.arguments)
            command += QLatin1Char(' ') + toQ(argument);
        Q_EMIT automationLogged(tr("Build"), command);
    }
    if (buildJob_) {
        buildJob_->wait();
    }
    buildJob_ = std::make_unique<build::BuildJob>();
    QPointer<ProjectController> self(this);
    QString root = toQ(paths::toUtf8(request.projectDirectory));
    build::BuildCallbacks callbacks;
    callbacks.onStep = [self](std::string_view label) {
        QString text = QString::fromUtf8(label.data(), static_cast<qsizetype>(label.size()));
        QMetaObject::invokeMethod(self.data(), [self, text] { if (self) Q_EMIT self->buildStep(text); }, Qt::QueuedConnection);
    };
    callbacks.onOutput = [self](std::string_view line, bool isError) {
        QString text = QString::fromUtf8(line.data(), static_cast<qsizetype>(line.size()));
        QMetaObject::invokeMethod(self.data(), [self, text, isError] { if (self) Q_EMIT self->buildOutput(text, isError); }, Qt::QueuedConnection);
    };
    callbacks.onFinished = [self, root, then](const build::BuildResult& result) {
        build::BuildResult copy = result;
        QMetaObject::invokeMethod(self.data(), [self, root, then, copy] {
            if (!self)
                return;
            int errors = 0;
            int warnings = 0;
            std::vector<ProblemItem> items;
            for (const auto& diagnostic : copy.diagnostics) {
                ProblemItem item;
                item.source = toQ(diagnostic.tool);
                item.severity = diagnostic.severity == build::DiagnosticSeverity::Error ? 2 : diagnostic.severity == build::DiagnosticSeverity::Warning ? 1 : 0;
                errors += item.severity == 2 ? 1 : 0;
                warnings += item.severity == 1 ? 1 : 0;
                QString file = toQ(diagnostic.file);
                QFileInfo info(file);
                if (info.isRelative())
                    file = QDir(root).filePath(file);
                QString relative = QDir(self->rootPath()).relativeFilePath(QDir::cleanPath(file));
                item.file = relative.startsWith(QStringLiteral("..")) ? file : relative;
                item.line = diagnostic.line;
                item.column = diagnostic.column;
                item.message = toQ(diagnostic.message);
                if (!diagnostic.code.empty())
                    item.message = toQ(diagnostic.code) + QStringLiteral(": ") + item.message;
                items.push_back(std::move(item));
            }
            self->buildProblems_ = std::move(items);
            self->artifacts_ = copy.artifacts;
            bool success = copy.state == build::BuildState::Succeeded;
            if (self->session_) {
                auto statement = self->session_->history().database().prepare("INSERT INTO builds (started, finished, target, configuration, exit_code, errors, warnings, artifacts) VALUES (?,?,?,?,?,?,?,?)");
                if (statement) {
                    std::string now = paths::nowIso8601();
                    statement->bind(1, std::string_view(now)).bind(2, std::string_view(now)).bind(3, std::string_view("main")).bind(4, std::string_view("debug"))
                        .bind(5, static_cast<std::int64_t>(copy.exitCode)).bind(6, static_cast<std::int64_t>(errors)).bind(7, static_cast<std::int64_t>(warnings)).bind(8, std::to_string(copy.artifacts.size()));
                    auto ran = statement->run();
                    (void)ran;
                }
            }
            Q_EMIT self->problemsChanged();
            Q_EMIT self->buildFinished(success, errors, warnings, copy.durationMillis);
            if (success && then)
                then();
        }, Qt::QueuedConnection);
    };
    auto started = buildJob_->start(std::move(plan.value()), request.projectDirectory, callbacks);
    if (!started) {
        Q_EMIT buildOutput(toQ(started.error().message), true);
        Q_EMIT buildFinished(false, 1, 0, 0);
    }
}

void ProjectController::cancelBuild()
{
    if (buildJob_)
        buildJob_->cancel();
}

bool ProjectController::buildRunning() const
{
    return buildJob_ && buildJob_->running();
}

void ProjectController::runApp(const QString& configuration)
{
    if (!session_ || target() == nullptr)
        return;
    stopApp();
    auto& app = AppContext::instance();
    const build::IBuildProvider* provider = session_->buildProviderFor(app.builders(), *target());
    if (provider == nullptr) {
        Q_EMIT appOutput(tr("No build system was recognized in this folder."), true);
        return;
    }
    auto command = provider->runCommand(requestFor(configuration), app.tools());
    if (!command) {
        Q_EMIT appOutput(toQ(command.error().message), true);
        return;
    }
    QString commandLine = toQ(command->program);
    for (const auto& argument : command->arguments)
        commandLine += QLatin1Char(' ') + toQ(argument);
    QPointer<ProjectController> self(this);
    auto deliver = [self](std::string_view data, bool isError) {
        QString text = QString::fromUtf8(data.data(), static_cast<qsizetype>(data.size()));
        QMetaObject::invokeMethod(self.data(), [self, text, isError] { if (self) Q_EMIT self->appOutput(text, isError); }, Qt::QueuedConnection);
    };
    auto started = Process::start(
        *command, [deliver](std::string_view data) { deliver(data, false); }, [deliver](std::string_view data) { deliver(data, true); },
        [self](int code) {
            QMetaObject::invokeMethod(self.data(), [self, code] { if (self) Q_EMIT self->appStopped(code); }, Qt::QueuedConnection);
        });
    if (!started) {
        Q_EMIT appOutput(toQ(started.error().message), true);
        return;
    }
    runningApp_ = std::move(started.value());
    Q_EMIT automationLogged(tr("Run"), commandLine);
    Q_EMIT appStarted(commandLine);
}

void ProjectController::stopApp()
{
    if (runningApp_) {
        if (runningApp_->running())
            runningApp_->terminate();
        runningApp_->waitFor(std::chrono::seconds(5));
        runningApp_.reset();
    }
}

bool ProjectController::appRunning() const
{
    return runningApp_ && runningApp_->running();
}

void ProjectController::buildAndRun(const QString& configuration)
{
    startBuild(configuration, [this, configuration] { runApp(configuration); });
}

void ProjectController::setBuildProblems(std::vector<ProblemItem> problems)
{
    buildProblems_ = std::move(problems);
    Q_EMIT problemsChanged();
}

void ProjectController::setEditorProblems(const QString& file, std::vector<ProblemItem> problems)
{
    if (problems.empty())
        editorProblems_.erase(file);
    else
        editorProblems_[file] = std::move(problems);
    Q_EMIT problemsChanged();
}

std::vector<ProblemItem> ProjectController::problems() const
{
    std::vector<ProblemItem> all = buildProblems_;
    all.insert(all.end(), designProblems_.begin(), designProblems_.end());
    for (const auto& [file, list] : editorProblems_)
        all.insert(all.end(), list.begin(), list.end());
    return all;
}

QString ProjectController::buildErrorsText() const
{
    QString output;
    for (const auto& item : buildProblems_) {
        if (item.severity >= 1)
            output += QStringLiteral("%1:%2:%3: %4\n").arg(item.file).arg(item.line).arg(item.column).arg(item.message);
    }
    return output;
}

void ProjectController::openFile(const QString& relativePath, int line, int column)
{
    Q_EMIT fileOpenRequested(relativePath, line, column);
}

void ProjectController::notifyFilesChanged()
{
    Q_EMIT filesChanged({});
}

void ProjectController::reportStatus(const QString& text)
{
    Q_EMIT statusMessage(text);
}

void ProjectController::onFilesChangedExternally(const std::vector<FileChange>& changes)
{
    for (const auto& change : changes) {
        QString path = toQ(change.path);
        if (path.startsWith(QStringLiteral(".forma/")) || path.startsWith(QStringLiteral(".git/")) || path.startsWith(QStringLiteral("build/")))
            continue;
        pendingChanges_ << path;
        if (change.kind != FileChangeKind::Removed)
            Q_EMIT externalFileChanged(path);
    }
    if (!pendingChanges_.isEmpty())
        watchDebounce_.start();
}

}
