#include "app/project_controller.h"

#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QRegularExpression>
#include <algorithm>
#include <filesystem>

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
        Q_EMIT noBuildSystem();
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
        QStringList missing = missingToolsForTarget();
        if (plan.error().code == "toolchain.missing" && !missing.isEmpty())
            Q_EMIT toolsMissing(missing, toQ(plan.error().message));
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
    lastRunUrl_.clear();
    outputTail_.clear();
    auto& app = AppContext::instance();
    const build::IBuildProvider* provider = session_->buildProviderFor(app.builders(), *target());
    if (provider == nullptr) {
        Q_EMIT noBuildSystem();
        return;
    }
    if (isAndroidProject()) {
        runOnAndroid(configuration);
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
        QMetaObject::invokeMethod(self.data(), [self, text, isError] {
            if (!self)
                return;
            Q_EMIT self->appOutput(text, isError);
            self->noticeUrl(text);
        }, Qt::QueuedConnection);
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

bool ProjectController::isAndroidProject() const
{
    if (!session_ || target() == nullptr)
        return false;
    const auto& platforms = target()->platforms;
    return std::find(platforms.begin(), platforms.end(), "android") != platforms.end() || !build::androidPackage(session_->root()).empty();
}

bool ProjectController::opensInBrowser() const
{
    if (!session_ || target() == nullptr)
        return false;
    const auto& platforms = target()->platforms;
    return std::find(platforms.begin(), platforms.end(), "web") != platforms.end();
}

std::vector<build::RunDevice> ProjectController::runDevices()
{
    std::vector<build::RunDevice> devices;
    if (isAndroidProject())
        return build::androidRunDevices(AppContext::instance().tools());
    if (opensInBrowser())
        devices.push_back({"browser", tr("Web browser").toStdString(), "browser", true});
    else
        devices.push_back({"local", tr("This computer").toStdString(), "local", true});
    return devices;
}

QStringList ProjectController::missingToolsForTarget()
{
    QStringList missing;
    if (!session_ || target() == nullptr)
        return missing;
    auto& app = AppContext::instance();
    auto consider = [&](const std::string& tool) {
        std::string id = tool;
        std::error_code ec;
        if (id == "gradle" && std::filesystem::exists(session_->root() / "gradlew", ec))
            return;
        if (!app.tools().detect(id, true).available && !missing.contains(toQ(id)))
            missing << toQ(id);
    };
    if (const project::ProjectTemplate* item = app.templates().find(session_->descriptor().templateId)) {
        for (const auto& tool : item->tools)
            consider(tool);
    }
    if (const build::IBuildProvider* provider = session_->buildProviderFor(app.builders(), *target())) {
        for (const auto& tool : provider->requiredTools())
            consider(tool);
        if (provider->id() == "cmake")
            consider("cpp-compiler");
    }
    return missing;
}

void ProjectController::noticeUrl(const QString& text)
{
    if (!lastRunUrl_.isEmpty())
        return;
    outputTail_ += text;
    outputTail_ = outputTail_.right(2000);
    static const QRegularExpression pattern(QStringLiteral("https?://(localhost|127\\.0\\.0\\.1|\\[::1\\])(:[0-9]+)?[^\\s\\x1b]*"));
    QRegularExpressionMatch match = pattern.match(outputTail_);
    if (match.hasMatch()) {
        lastRunUrl_ = match.captured(0);
        Q_EMIT runUrlFound(lastRunUrl_);
    }
}

void ProjectController::runOnAndroid(const QString& configuration)
{
    auto& tools = AppContext::instance().tools();
    if (!build::adbPath(tools)) {
        Q_EMIT runNeedsDevice(tr("Running on Android needs the Android SDK platform tools."), {QStringLiteral("android-sdk")});
        return;
    }
    if (configuration == QStringLiteral("release")) {
        Q_EMIT appOutput(tr("Release builds aren't signed yet, so Android won't install them. Run the Debug configuration, or sign the APK in app/build/outputs/apk/release.\n"), true);
        return;
    }
    auto devices = build::androidRunDevices(tools);
    QString chosen = runDevice_;
    bool stillThere = false;
    for (const auto& device : devices)
        stillThere = stillThere || toQ(device.id) == chosen;
    if (!stillThere) {
        chosen.clear();
        for (const auto& device : devices) {
            if (device.kind == "android-device" && device.ready) {
                chosen = toQ(device.id);
                break;
            }
        }
        if (chosen.isEmpty()) {
            for (const auto& device : devices) {
                if (device.kind == "android-avd") {
                    chosen = toQ(device.id);
                    break;
                }
            }
        }
    }
    if (chosen.isEmpty()) {
        Q_EMIT runNeedsDevice(tr("No Android device is connected and there's no virtual phone yet. Connect a phone with USB debugging turned on, or install the emulator."), {QStringLiteral("emulator")});
        return;
    }
    runDevice_ = chosen;
    if (chosen.startsWith(QStringLiteral("serial:"))) {
        startAndroidLaunch(chosen.mid(7), configuration, false);
        return;
    }
    QString avd = chosen.mid(4);
    QString running;
    auto adb = build::adbPath(tools);
    for (const auto& device : build::androidDevices(tools)) {
        if (!text::startsWith(device.serial, "emulator-"))
            continue;
        ProcessSpec query;
        query.program = paths::toUtf8(*adb);
        query.arguments = {"-s", device.serial, "emu", "avd", "name"};
        query.origin = Origin::System;
        auto result = runProcess(query, std::chrono::seconds(5));
        if (result && !text::splitLines(result->out).empty() && toQ(text::trim(text::splitLines(result->out).front())) == avd)
            running = toQ(device.serial);
    }
    if (!running.isEmpty()) {
        startAndroidLaunch(running, configuration, true);
        return;
    }
    std::string acceleration = build::androidAcceleration(tools);
    if (!acceleration.empty()) {
        Q_EMIT runNeedsDevice(tr("The emulator can't start because hardware acceleration isn't ready: %1 Connect a phone with USB debugging turned on, or install the acceleration driver.").arg(toQ(acceleration)), {QStringLiteral("android-acceleration")});
        return;
    }
    auto emulator = build::emulatorPath(tools);
    if (!emulator) {
        Q_EMIT runNeedsDevice(tr("The Android Emulator isn't installed."), {QStringLiteral("emulator")});
        return;
    }
    int port = build::freeEmulatorPort(tools);
    ProcessSpec spec;
    spec.program = paths::toUtf8(*emulator);
    spec.arguments = {"-avd", fromQ(avd), "-port", std::to_string(port)};
    if (auto extra = paths::env("FORMA_EMULATOR_FLAGS")) {
        for (const auto& flag : text::split(*extra, ' ')) {
            if (!flag.empty())
                spec.arguments.push_back(flag);
        }
    }
    if (auto sdk = tools.androidSdk()) {
        spec.environment["ANDROID_HOME"] = paths::toUtf8(*sdk);
        spec.environment["ANDROID_SDK_ROOT"] = paths::toUtf8(*sdk);
    }
    spec.origin = Origin::Build;
    Q_EMIT runProgress(tr("Starting %1").arg(avd));
    Q_EMIT appOutput(tr("Starting the emulator %1. The first start can take a few minutes.\n").arg(avd), false);
    QPointer<ProjectController> self(this);
    auto started = Process::start(spec, [self](std::string_view data) {
        QString text = QString::fromUtf8(data.data(), static_cast<qsizetype>(data.size()));
        QMetaObject::invokeMethod(self.data(), [self, text] { if (self && text.contains(QStringLiteral("ERROR"))) Q_EMIT self->appOutput(text, true); }, Qt::QueuedConnection);
    }, [](std::string_view) {});
    if (!started) {
        Q_EMIT appOutput(toQ(started.error().message) + QLatin1Char('\n'), true);
        return;
    }
    emulator_ = std::move(started.value());
    startAndroidLaunch(QStringLiteral("emulator-%1").arg(port), configuration, true);
}

void ProjectController::startAndroidLaunch(const QString& serial, const QString& configuration, bool waitForBoot)
{
    auto& tools = AppContext::instance().tools();
    auto adb = build::adbPath(tools);
    QString package = toQ(build::androidPackage(session_->root()));
    if (!adb || package.isEmpty()) {
        Q_EMIT appOutput(tr("This project has no applicationId in app/build.gradle.kts, so FormaIDE can't tell which app to start.\n"), true);
        return;
    }
    std::filesystem::path apk = session_->root() / "app" / "build" / "outputs" / "apk" / "debug" / "app-debug.apk";
    (void)configuration;
    std::error_code apkError;
    if (!std::filesystem::exists(apk, apkError)) {
        Q_EMIT appOutput(tr("The APK wasn't found at %1. Build the project first.\n").arg(toQ(paths::toUtf8(apk))), true);
        return;
    }
    auto step = [&](const char* label, std::vector<std::string> arguments) {
        ProcessSpec spec;
        spec.program = paths::toUtf8(*adb);
        spec.arguments = {"-s", fromQ(serial)};
        spec.arguments.insert(spec.arguments.end(), arguments.begin(), arguments.end());
        spec.origin = Origin::Build;
        spec.workingDirectory = session_->root();
        return build::BuildStep{label, spec};
    };
    build::BuildPlan plan;
    if (waitForBoot) {
        plan.steps.push_back(step("Wait for the device", {"wait-for-device"}));
        plan.steps.push_back(step("Wait for Android to finish starting", {"shell", "while [ \"$(getprop sys.boot_completed)\" != \"1\" ]; do sleep 1; done"}));
    }
    plan.steps.push_back(step("Install the app", {"install", "-r", paths::toUtf8(apk)}));
    plan.steps.push_back(step("Open the app", {"shell", "monkey", "-p", fromQ(package), "-c", "android.intent.category.LAUNCHER", "1"}));
    androidSerial_ = serial;
    androidPackageName_ = package;
    launchJob_ = std::make_unique<build::BuildJob>();
    QPointer<ProjectController> self(this);
    build::BuildCallbacks callbacks;
    callbacks.onStep = [self](std::string_view label) {
        QString text = QString::fromUtf8(label.data(), static_cast<qsizetype>(label.size()));
        QMetaObject::invokeMethod(self.data(), [self, text] {
            if (!self)
                return;
            Q_EMIT self->runProgress(text);
            Q_EMIT self->appOutput(text + QLatin1Char('\n'), false);
        }, Qt::QueuedConnection);
    };
    callbacks.onOutput = [self](std::string_view line, bool isError) {
        QString text = QString::fromUtf8(line.data(), static_cast<qsizetype>(line.size()));
        QMetaObject::invokeMethod(self.data(), [self, text, isError] { if (self) Q_EMIT self->appOutput(text + QLatin1Char('\n'), isError); }, Qt::QueuedConnection);
    };
    callbacks.onFinished = [self, serial, package](const build::BuildResult& result) {
        bool ok = result.state == build::BuildState::Succeeded;
        QString failed = toQ(result.failedStep);
        QMetaObject::invokeMethod(self.data(), [self, serial, package, ok, failed] {
            if (!self)
                return;
            if (!ok) {
                Q_EMIT self->appOutput(tr("Couldn't start the app on %1: %2 failed.\n").arg(serial, failed), true);
                Q_EMIT self->appStopped(1);
                return;
            }
            self->attachLogcat(serial, package, 0);
        }, Qt::QueuedConnection);
    };
    auto started = launchJob_->start(std::move(plan), session_->root(), callbacks);
    if (!started) {
        Q_EMIT appOutput(toQ(started.error().message) + QLatin1Char('\n'), true);
        return;
    }
    Q_EMIT appStarted(tr("%1 on %2").arg(package, serial));
}

void ProjectController::attachLogcat(const QString& serial, const QString& package, int attempt)
{
    auto adb = build::adbPath(AppContext::instance().tools());
    if (!adb || !session_)
        return;
    ProcessSpec query;
    query.program = paths::toUtf8(*adb);
    query.arguments = {"-s", fromQ(serial), "shell", "pidof", fromQ(package)};
    query.origin = Origin::System;
    auto result = runProcess(query, std::chrono::seconds(5));
    QString pid = result ? toQ(text::trim(result->out)).split(QLatin1Char(' ')).value(0) : QString();
    if (pid.isEmpty()) {
        if (attempt < 20) {
            QPointer<ProjectController> self(this);
            QTimer::singleShot(300, this, [self, serial, package, attempt] { if (self) self->attachLogcat(serial, package, attempt + 1); });
            return;
        }
        Q_EMIT appOutput(tr("The app was installed but stopped right after it opened. Check the device for a crash message.\n"), true);
        Q_EMIT appStopped(1);
        return;
    }
    Q_EMIT runProgress(tr("Running on %1").arg(serial));
    Q_EMIT appOutput(tr("%1 is running on %2. Its log follows.\n").arg(package, serial), false);
    ProcessSpec logcat;
    logcat.program = paths::toUtf8(*adb);
    logcat.arguments = {"-s", fromQ(serial), "logcat", "--pid=" + fromQ(pid), "-v", "brief"};
    logcat.origin = Origin::Build;
    QPointer<ProjectController> self(this);
    auto deliver = [self](std::string_view data) {
        QString text = QString::fromUtf8(data.data(), static_cast<qsizetype>(data.size()));
        QMetaObject::invokeMethod(self.data(), [self, text] { if (self) Q_EMIT self->appOutput(text, false); }, Qt::QueuedConnection);
    };
    auto started = Process::start(logcat, deliver, deliver, [self](int code) {
        QMetaObject::invokeMethod(self.data(), [self, code] { if (self) Q_EMIT self->appStopped(code); }, Qt::QueuedConnection);
    });
    if (started)
        runningApp_ = std::move(started.value());
}

void ProjectController::stopApp()
{
    if (launchJob_) {
        launchJob_->cancel();
        launchJob_->wait();
        launchJob_.reset();
    }
    if (!androidSerial_.isEmpty() && runningApp_) {
        if (auto adb = build::adbPath(AppContext::instance().tools())) {
            ProcessSpec spec;
            spec.program = paths::toUtf8(*adb);
            spec.arguments = {"-s", fromQ(androidSerial_), "shell", "am", "force-stop", fromQ(androidPackageName_)};
            spec.origin = Origin::System;
            auto stopped = runProcess(spec, std::chrono::seconds(5));
            (void)stopped;
        }
    }
    if (runningApp_) {
        if (runningApp_->running())
            runningApp_->terminate();
        runningApp_->waitFor(std::chrono::seconds(5));
        runningApp_.reset();
    }
}

bool ProjectController::appRunning() const
{
    return (runningApp_ && runningApp_->running()) || (launchJob_ && launchJob_->running());
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
