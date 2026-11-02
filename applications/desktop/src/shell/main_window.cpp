#include "shell/main_window.h"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QStackedWidget>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include "ScintillaEditBase.h"
#include "app/app_context.h"
#include "forma/core/filesystem_service.h"
#include "ui/theme.h"

namespace forma::ui {

namespace {

constexpr int kLayoutVersion = 1;

QString dockFor(const QString& id)
{
    return QStringLiteral("dock.") + id;
}

}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    setObjectName(QStringLiteral("mainWindow"));
    setWindowTitle(QStringLiteral("FormaIDE"));
    setWindowIcon(QIcon());
    resize(1440, 900);
    setDockNestingEnabled(true);
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
    setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);
    controller_ = new ProjectController(this);
    buildCentral();
    buildDocks();
    buildRail();
    buildToolbar();
    buildMenus();
    buildStatusBar();
    wireController();
    onProjectClosed();
    restoreLayout();
}

void MainWindow::buildCentral()
{
    pages_ = new QStackedWidget(this);
    pages_->setObjectName(QStringLiteral("pages"));
    welcome_ = new WelcomePage(pages_);
    pages_->addWidget(welcome_);
    mainPage_ = new QWidget(pages_);
    auto* row = new QHBoxLayout(mainPage_);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    rail_ = new QFrame(this);
    rail_->setObjectName(QStringLiteral("workspaceRail"));
    rail_->setProperty("role", "rail");
    rail_->setFixedWidth(68);
    railBar_ = new QToolBar(tr("Workspaces"), this);
    railBar_->setObjectName(QStringLiteral("railToolbar"));
    railBar_->setMovable(false);
    railBar_->setFloatable(false);
    railBar_->setContentsMargins(0, 0, 0, 0);
    railBar_->addWidget(rail_);
    addToolBar(Qt::LeftToolBarArea, railBar_);
    center_ = new QStackedWidget(mainPage_);
    center_->setObjectName(QStringLiteral("workspaceCenter"));
    canvas_ = new CanvasWidget(controller_, center_);
    editors_ = new EditorArea(controller_, center_);
    projectPage_ = new ProjectPage(controller_, center_);
    preview_ = new PreviewPanel(controller_, center_);
    build_ = new BuildPanel(controller_, center_);
    center_->addWidget(canvas_);
    center_->addWidget(editors_);
    center_->addWidget(projectPage_);
    center_->addWidget(preview_);
    center_->addWidget(build_);
    row->addWidget(center_, 1);
    pages_->addWidget(mainPage_);
    setCentralWidget(pages_);
    connect(welcome_, &WelcomePage::newRequested, this, &MainWindow::newProject);
    connect(welcome_, &WelcomePage::openRequested, this, &MainWindow::openProject);
    connect(welcome_, &WelcomePage::importRequested, this, &MainWindow::importFolder);
    connect(welcome_, &WelcomePage::recentRequested, this, &MainWindow::openProjectFolder);
}

QDockWidget* MainWindow::addDock(const QString& id, const QString& title, QWidget* widget, Qt::DockWidgetArea area)
{
    auto* dock = new QDockWidget(title, this);
    dock->setObjectName(dockFor(id));
    dock->setWidget(widget);
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetFloatable);
    addDockWidget(area, dock);
    docks_[id] = dock;
    return dock;
}

void MainWindow::buildDocks()
{
    screens_ = new ScreensPanel(controller_, this);
    layers_ = new LayersPanel(controller_, this);
    palette_ = new PalettePanel(controller_, this);
    inspector_ = new InspectorPanel(controller_, this);
    files_ = new FileExplorer(controller_, this);
    search_ = new SearchPanel(controller_, this);
    git_ = new GitPanel(controller_, this);
    diff_ = new DiffView(this);
    terminal_ = new TerminalPanel(controller_, this);
    output_ = new OutputPanel(controller_, this);
    problems_ = new ProblemsPanel(controller_, this);
    automation_ = new AutomationLog(controller_, this);
    debug_ = new DebugPanel(controller_, editors_, this);
    assistant_ = new AssistantPanel(controller_, editors_, this);

    auto* screens = addDock(QStringLiteral("screens"), tr("Screens"), screens_, Qt::LeftDockWidgetArea);
    auto* layers = addDock(QStringLiteral("layers"), tr("Layers"), layers_, Qt::LeftDockWidgetArea);
    auto* components = addDock(QStringLiteral("components"), tr("Components"), palette_, Qt::LeftDockWidgetArea);
    splitDockWidget(screens, layers, Qt::Vertical);
    tabifyDockWidget(layers, components);
    resizeDocks({screens, layers}, {180, 560}, Qt::Vertical);
    auto* filesDock = addDock(QStringLiteral("files"), tr("Files"), files_, Qt::LeftDockWidgetArea);
    auto* searchDock = addDock(QStringLiteral("search"), tr("Search"), search_, Qt::LeftDockWidgetArea);
    auto* gitDock = addDock(QStringLiteral("git"), tr("Git"), git_, Qt::LeftDockWidgetArea);
    tabifyDockWidget(filesDock, searchDock);
    tabifyDockWidget(searchDock, gitDock);
    filesDock->raise();
    auto* inspectorDock = addDock(QStringLiteral("inspector"), tr("Inspector"), inspector_, Qt::RightDockWidgetArea);
    auto* assistantDock = addDock(QStringLiteral("assistant"), tr("Assistant"), assistant_, Qt::RightDockWidgetArea);
    tabifyDockWidget(inspectorDock, assistantDock);
    inspectorDock->raise();
    auto* terminalDock = addDock(QStringLiteral("terminal"), tr("Terminal"), terminal_, Qt::BottomDockWidgetArea);
    auto* outputDock = addDock(QStringLiteral("output"), tr("Output"), output_, Qt::BottomDockWidgetArea);
    auto* problemsDock = addDock(QStringLiteral("problems"), tr("Problems"), problems_, Qt::BottomDockWidgetArea);
    auto* diffDock = addDock(QStringLiteral("diff"), tr("Changes"), diff_, Qt::BottomDockWidgetArea);
    auto* automationDock = addDock(QStringLiteral("automation"), tr("Activity"), automation_, Qt::BottomDockWidgetArea);
    auto* debugDock = addDock(QStringLiteral("debug"), tr("Debugger"), debug_, Qt::BottomDockWidgetArea);
    tabifyDockWidget(problemsDock, outputDock);
    tabifyDockWidget(outputDock, terminalDock);
    tabifyDockWidget(terminalDock, diffDock);
    tabifyDockWidget(diffDock, automationDock);
    tabifyDockWidget(automationDock, debugDock);
    problemsDock->raise();
    resizeDocks({filesDock, inspectorDock}, {280, 320}, Qt::Horizontal);
    resizeDocks({problemsDock}, {220}, Qt::Vertical);

    workspaces_ = {
        {QStringLiteral("design"), tr("Design"), QStringLiteral("design"), canvas_, {QStringLiteral("screens"), QStringLiteral("layers"), QStringLiteral("components"), QStringLiteral("inspector"), QStringLiteral("problems")}},
        {QStringLiteral("code"), tr("Code"), QStringLiteral("code"), editors_, {QStringLiteral("files"), QStringLiteral("search"), QStringLiteral("git"), QStringLiteral("terminal"), QStringLiteral("output"), QStringLiteral("problems"), QStringLiteral("diff")}},
        {QStringLiteral("project"), tr("Project"), QStringLiteral("project"), projectPage_, {QStringLiteral("files")}},
        {QStringLiteral("preview"), tr("Preview"), QStringLiteral("preview"), preview_, {QStringLiteral("screens"), QStringLiteral("output")}},
        {QStringLiteral("debug"), tr("Debug"), QStringLiteral("debug"), editors_, {QStringLiteral("files"), QStringLiteral("debug"), QStringLiteral("output"), QStringLiteral("terminal")}},
        {QStringLiteral("build"), tr("Build"), QStringLiteral("build"), build_, {QStringLiteral("output"), QStringLiteral("problems"), QStringLiteral("automation")}},
        {QStringLiteral("assistant"), tr("Assistant"), QStringLiteral("assistant"), editors_, {QStringLiteral("files"), QStringLiteral("assistant"), QStringLiteral("problems")}},
    };
}

void MainWindow::buildRail()
{
    auto* column = new QVBoxLayout(rail_);
    column->setContentsMargins(4, 6, 4, 6);
    column->setSpacing(2);
    railGroup_ = new QButtonGroup(this);
    railGroup_->setExclusive(true);
    for (auto& workspace : workspaces_) {
        auto* button = new QToolButton(rail_);
        button->setObjectName(QStringLiteral("rail.") + workspace.id);
        button->setText(workspace.title);
        button->setIcon(icon(workspace.iconName));
        button->setIconSize(QSize(20, 20));
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setCheckable(true);
        button->setFixedSize(60, 52);
        button->setToolTip(workspace.title);
        QString id = workspace.id;
        connect(button, &QToolButton::clicked, this, [this, id] { showWorkspace(id); });
        railGroup_->addButton(button);
        column->addWidget(button);
        workspace.button = button;
    }
    column->addStretch(1);
    auto* settings = new QToolButton(rail_);
    settings->setObjectName(QStringLiteral("rail.settings"));
    settings->setText(tr("Settings"));
    settings->setIcon(icon(QStringLiteral("settings")));
    settings->setIconSize(QSize(20, 20));
    settings->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    settings->setFixedSize(60, 52);
    connect(settings, &QToolButton::clicked, this, [this] { openSettings(); });
    column->addWidget(settings);
    connect(&Theme::instance(), &Theme::changed, this, [this, settings] {
        for (auto& workspace : workspaces_)
            static_cast<QToolButton*>(workspace.button)->setIcon(icon(workspace.iconName));
        settings->setIcon(icon(QStringLiteral("settings")));
    });
}

void MainWindow::buildToolbar()
{
    toolbar_ = addToolBar(tr("Main"));
    toolbar_->setObjectName(QStringLiteral("mainToolbar"));
    toolbar_->setMovable(false);
    toolbar_->setFloatable(false);
    toolbar_->setIconSize(QSize(16, 16));
    toolbar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    projectLabel_ = new QLabel(toolbar_);
    projectLabel_->setObjectName(QStringLiteral("toolbarProject"));
    projectLabel_->setContentsMargins(8, 0, 12, 0);
    QFont bold = projectLabel_->font();
    bold.setWeight(QFont::DemiBold);
    projectLabel_->setFont(bold);
    toolbar_->addWidget(projectLabel_);
    saveAction_ = toolbar_->addAction(icon(QStringLiteral("save")), tr("Save"), this, &MainWindow::saveEverything);
    saveAction_->setObjectName(QStringLiteral("actionSave"));
    saveAction_->setShortcut(QKeySequence::Save);
    undoAction_ = toolbar_->addAction(icon(QStringLiteral("undo")), tr("Undo"), this, &MainWindow::undo);
    undoAction_->setObjectName(QStringLiteral("actionUndo"));
    undoAction_->setShortcut(QKeySequence::Undo);
    redoAction_ = toolbar_->addAction(icon(QStringLiteral("redo")), tr("Redo"), this, &MainWindow::redo);
    redoAction_->setObjectName(QStringLiteral("actionRedo"));
    redoAction_->setShortcut(QKeySequence::Redo);
    toolbar_->addSeparator();
    target_ = new QComboBox(toolbar_);
    target_->setObjectName(QStringLiteral("toolbarTarget"));
    target_->setToolTip(tr("What to build"));
    target_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    toolbar_->addWidget(target_);
    configuration_ = new QComboBox(toolbar_);
    configuration_->setObjectName(QStringLiteral("toolbarConfiguration"));
    configuration_->addItem(tr("Debug"), QStringLiteral("debug"));
    configuration_->addItem(tr("Release"), QStringLiteral("release"));
    toolbar_->addWidget(configuration_);
    buildAction_ = toolbar_->addAction(icon(QStringLiteral("build")), tr("Build"), this, [this] {
        saveEverything();
        controller_->startBuild(configuration_->currentData().toString());
        docks_[QStringLiteral("output")]->raise();
    });
    buildAction_->setObjectName(QStringLiteral("actionBuild"));
    buildAction_->setShortcut(Qt::CTRL | Qt::Key_B);
    runAction_ = toolbar_->addAction(icon(QStringLiteral("run")), tr("Run"), this, [this] {
        saveEverything();
        controller_->buildAndRun(configuration_->currentData().toString());
        docks_[QStringLiteral("output")]->raise();
    });
    runAction_->setObjectName(QStringLiteral("actionRun"));
    runAction_->setShortcut(Qt::CTRL | Qt::Key_R);
    stopAction_ = toolbar_->addAction(icon(QStringLiteral("stop")), tr("Stop"), this, [this] {
        controller_->cancelBuild();
        controller_->stopApp();
    });
    stopAction_->setObjectName(QStringLiteral("actionStop"));
    connect(target_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (updatingTargets_ || !controller_->hasProject())
            return;
        controller_->session()->descriptor().activeTarget = fromQ(target_->currentData().toString());
        auto saved = controller_->session()->saveDescriptor();
        (void)saved;
    });
    connect(&Theme::instance(), &Theme::changed, this, [this] {
        saveAction_->setIcon(icon(QStringLiteral("save")));
        undoAction_->setIcon(icon(QStringLiteral("undo")));
        redoAction_->setIcon(icon(QStringLiteral("redo")));
        buildAction_->setIcon(icon(QStringLiteral("build")));
        runAction_->setIcon(icon(QStringLiteral("run")));
        stopAction_->setIcon(icon(QStringLiteral("stop")));
    });
}

void MainWindow::buildMenus()
{
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("New project..."), QKeySequence::New, this, &MainWindow::newProject);
    file->addAction(tr("Open project..."), QKeySequence::Open, this, &MainWindow::openProject);
    file->addAction(tr("Open an existing code folder..."), this, &MainWindow::importFolder);
    file->addSeparator();
    file->addAction(saveAction_);
    file->addAction(tr("Close file"), QKeySequence(Qt::CTRL | Qt::Key_W), this, [this] { editors_->closeCurrent(); });
    file->addAction(tr("Close project"), this, [this] {
        if (saveEverything() && editors_->closeAll())
            controller_->closeProject();
    });
    file->addSeparator();
    file->addAction(tr("Settings..."), QKeySequence(Qt::CTRL | Qt::Key_Comma), this, [this] { openSettings(); });
    file->addSeparator();
    file->addAction(tr("Exit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    edit->addAction(undoAction_);
    edit->addAction(redoAction_);
    edit->addSeparator();
    edit->addAction(tr("Find"), QKeySequence::Find, this, [this] {
        if (CodeEditor* editor = editors_->currentEditor())
            editor->showFindBar(false);
    });
    edit->addAction(tr("Replace"), QKeySequence(Qt::CTRL | Qt::Key_H), this, [this] {
        if (CodeEditor* editor = editors_->currentEditor())
            editor->showFindBar(true);
    });
    edit->addAction(tr("Search the project"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F), this, [this] {
        showWorkspace(QStringLiteral("code"));
        docks_[QStringLiteral("search")]->raise();
        search_->focusSearch();
    });
    edit->addAction(tr("Go to file..."), QKeySequence(Qt::CTRL | Qt::Key_P), this, &MainWindow::quickOpen);
    edit->addSeparator();
    edit->addAction(tr("Toggle comment"), QKeySequence(Qt::CTRL | Qt::Key_Slash), this, [this] {
        if (CodeEditor* editor = editors_->currentEditor())
            editor->toggleComment();
    });
    edit->addAction(tr("Select next match"), QKeySequence(Qt::CTRL | Qt::Key_D), this, [this] {
        if (workspace_ == QStringLiteral("design")) {
            canvas_->duplicateSelection();
        } else if (CodeEditor* editor = editors_->currentEditor()) {
            editor->addNextOccurrence();
        }
    });
    edit->addAction(tr("Format document"), QKeySequence(Qt::SHIFT | Qt::ALT | Qt::Key_F), this, [this] { editors_->formatDocument(); });
    edit->addAction(tr("Rename symbol..."), QKeySequence(Qt::Key_F2), this, [this] { editors_->renameSymbol(); });
    edit->addAction(tr("Go to definition"), QKeySequence(Qt::Key_F12), this, [this] { editors_->goToDefinition(); });
    edit->addAction(tr("Find references"), QKeySequence(Qt::SHIFT | Qt::Key_F12), this, [this] {
        editors_->findReferences();
        docks_[QStringLiteral("search")]->raise();
    });
    edit->addAction(tr("Show this line on the canvas"), this, [this] { editors_->gotoComponentAtCursor(); });

    QMenu* view = menuBar()->addMenu(tr("&View"));
    int shortcut = 1;
    for (const auto& workspace : workspaces_) {
        QString id = workspace.id;
        view->addAction(workspace.title, QKeySequence(Qt::CTRL | static_cast<Qt::Key>(Qt::Key_0 + shortcut++)), this, [this, id] { showWorkspace(id); });
    }
    view->addSeparator();
    QMenu* panels = view->addMenu(tr("Panels"));
    for (const auto& [id, dock] : docks_)
        panels->addAction(dock->toggleViewAction());
    view->addAction(tr("Reset this workspace's layout"), this, [this] {
        auto& settings = AppContext::instance().settings();
        settings.set("layout." + fromQ(workspace_), nullptr);
        QString current = workspace_;
        workspace_.clear();
        showWorkspace(current);
    });
    view->addSeparator();
    view->addAction(tr("Zoom canvas to fit"), QKeySequence(Qt::CTRL | Qt::Key_0), this, [this] { canvas_->zoomToFit(); });

    QMenu* build = menuBar()->addMenu(tr("&Build"));
    build->addAction(tr("Generate code"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G), this, [this] {
        saveEverything();
        controller_->regenerate();
    });
    build->addAction(buildAction_);
    build->addAction(runAction_);
    build->addAction(stopAction_);
    build->addSeparator();
    build->addAction(tr("Start debugging"), QKeySequence(Qt::Key_F5), this, [this] {
        showWorkspace(QStringLiteral("debug"));
        debug_->start();
    });
    build->addAction(tr("Run current file in the terminal"), this, [this] { editors_->runCurrentFile(); });
    build->addAction(tr("Check toolchains again"), this, [this] {
        showWorkspace(QStringLiteral("build"));
        build_->refreshToolchains(true);
    });

    QMenu* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("Documentation"), this, [] { QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/grangedevgroup-code/FormaIDE/tree/main/documentation"))); });
    help->addAction(tr("Report a problem"), this, [] { QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/grangedevgroup-code/FormaIDE/issues"))); });
    help->addAction(tr("About FormaIDE"), this, [this] {
        QMessageBox::about(this, tr("About FormaIDE"), tr("FormaIDE is a native, local-first visual development environment.\nMIT license.\n\nBuilt with Qt, Scintilla and Lexilla. See the third-party notices in the documentation."));
    });
}

void MainWindow::buildStatusBar()
{
    statusText_ = new QLabel(this);
    statusText_->setObjectName(QStringLiteral("statusText"));
    statusText_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    statusProblems_ = new QLabel(this);
    statusProblems_->setObjectName(QStringLiteral("statusProblems"));
    statusBranch_ = new QLabel(this);
    statusBranch_->setObjectName(QStringLiteral("statusBranch"));
    statusServer_ = new QLabel(this);
    statusServer_->setObjectName(QStringLiteral("statusServer"));
    statusServer_->setMaximumWidth(320);
    statusCursor_ = new QLabel(this);
    statusLanguage_ = new QLabel(this);
    statusSaved_ = new QLabel(this);
    statusSaved_->setObjectName(QStringLiteral("statusSaved"));
    statusBar()->addWidget(statusText_, 1);
    statusBar()->addPermanentWidget(statusServer_);
    statusBar()->addPermanentWidget(statusBranch_);
    statusBar()->addPermanentWidget(statusProblems_);
    statusBar()->addPermanentWidget(statusLanguage_);
    statusBar()->addPermanentWidget(statusCursor_);
    statusBar()->addPermanentWidget(statusSaved_);
    statusBar()->setSizeGripEnabled(false);
}

void MainWindow::wireController()
{
    for (const auto& [id, dock] : docks_)
        connect(dock, &QDockWidget::dockLocationChanged, this, &MainWindow::updateDockTitles);
    connect(controller_, &ProjectController::projectOpened, this, &MainWindow::onProjectOpened);
    connect(controller_, &ProjectController::projectClosed, this, &MainWindow::onProjectClosed);
    connect(controller_, &ProjectController::statusMessage, statusText_, &QLabel::setText);
    connect(controller_, &ProjectController::modelChanged, this, [this](const ir::ChangeEvent&) { updateActions(); });
    connect(controller_, &ProjectController::buildStarted, this, &MainWindow::updateActions);
    connect(controller_, &ProjectController::buildFinished, this, [this](bool, int errors, int, qint64) {
        updateActions();
        if (errors > 0)
            docks_[QStringLiteral("problems")]->raise();
    });
    connect(controller_, &ProjectController::appStarted, this, [this](const QString&) { updateActions(); });
    connect(controller_, &ProjectController::appStopped, this, [this](int) { updateActions(); });
    connect(controller_, &ProjectController::recoveryAvailable, this, &MainWindow::checkRecovery);
    connect(controller_, &ProjectController::generationFinished, this, [this](const codegen::GenerationReport& report) {
        int conflicts = 0;
        for (const auto& file : report.files)
            conflicts += file.outcome == codegen::FileOutcome::Conflict ? 1 : 0;
        if (conflicts > 0) {
            statusText_->setText(tr("%n generated file(s) conflict with your edits. Nothing was overwritten. Open Build to choose.", nullptr, conflicts));
        }
    });
    connect(problems_, &ProblemsPanel::countsChanged, this, [this](int errors, int warnings) {
        statusProblems_->setText(tr("%1 errors, %2 warnings").arg(errors).arg(warnings));
    });
    connect(git_, &GitPanel::branchChanged, statusBranch_, &QLabel::setText);
    connect(git_, &GitPanel::diffRequested, this, [this](const QString&, const QString& text) {
        diff_->showDiff(text);
        docks_[QStringLiteral("diff")]->show();
        docks_[QStringLiteral("diff")]->raise();
    });
    connect(editors_, &EditorArea::cursorChanged, this, [this](int line, int column, const QString& language) {
        statusCursor_->setText(tr("Ln %1, Col %2").arg(line).arg(column));
        statusLanguage_->setText(language);
    });
    connect(editors_, &EditorArea::serverStatusChanged, this, [this](const QString& text) {
        statusServer_->setText(statusServer_->fontMetrics().elidedText(text, Qt::ElideRight, 310));
        statusServer_->setToolTip(text);
    });
    connect(editors_, &EditorArea::referencesFound, this, [this](const QString& title, const std::vector<lsp::Location>& locations) {
        search_->showLocations(title, locations);
        docks_[QStringLiteral("search")]->raise();
    });
    connect(editors_, &EditorArea::automationRequested, this, [this](const QString& command) {
        docks_[QStringLiteral("terminal")]->show();
        docks_[QStringLiteral("terminal")]->raise();
        terminal_->runInNewTerminal(command);
    });
    connect(editors_, &EditorArea::componentFocusRequested, this, [this](const QString& screen, const QString& component) {
        showWorkspace(QStringLiteral("design"));
        controller_->selectComponent(screen, component);
        canvas_->zoomToSelection();
    });
    connect(controller_, &ProjectController::fileOpenRequested, this, [this](const QString&, int, int) {
        if (center_->currentWidget() != editors_)
            showWorkspace(QStringLiteral("code"));
    });
    connect(canvas_, &CanvasWidget::openCodeRequested, this, [this](const QString& screen, const QString& component) {
        if (!controller_->hasProject() || controller_->target() == nullptr)
            return;
        auto location = codegen::locateComponent(controller_->session()->filesystem(), controller_->target()->id, fromQ(screen), fromQ(component));
        if (!location) {
            statusText_->setText(tr("Generate code first to jump to this component's source"));
            return;
        }
        controller_->openFile(toQ(location->file), location->line);
    });
    connect(inspector_, &InspectorPanel::openCodeRequested, canvas_, &CanvasWidget::openCodeRequested);
    connect(preview_, &PreviewPanel::runRequested, runAction_, &QAction::trigger);
    connect(screens_, &ScreensPanel::screenActivated, canvas_, &CanvasWidget::centerOnScreen);
}

void MainWindow::updateTitle()
{
    if (controller_->hasProject()) {
        setWindowTitle(QStringLiteral("%1 - FormaIDE").arg(toQ(controller_->session()->descriptor().name)));
        projectLabel_->setText(toQ(controller_->session()->descriptor().name));
    } else {
        setWindowTitle(QStringLiteral("FormaIDE"));
        projectLabel_->setText(QStringLiteral("FormaIDE"));
    }
}

void MainWindow::updateActions()
{
    bool open = controller_->hasProject();
    bool busy = controller_->buildRunning();
    bool canBuild = open && controller_->target() != nullptr;
    buildAction_->setEnabled(canBuild && !busy);
    runAction_->setEnabled(canBuild && !busy);
    stopAction_->setEnabled(busy || controller_->appRunning());
    saveAction_->setEnabled(open);
    undoAction_->setEnabled(open);
    redoAction_->setEnabled(open);
    target_->setEnabled(open);
    configuration_->setEnabled(open);
}

void MainWindow::updateTargets()
{
    updatingTargets_ = true;
    target_->clear();
    if (controller_->hasProject()) {
        const auto& descriptor = controller_->session()->descriptor();
        for (const auto& item : descriptor.targets)
            target_->addItem(toQ(item.label), toQ(item.id));
        target_->setCurrentIndex(std::max(0, target_->findData(toQ(descriptor.activeTarget))));
    }
    target_->setVisible(target_->count() > 0);
    updatingTargets_ = false;
}

void MainWindow::onProjectOpened()
{
    pages_->setCurrentWidget(mainPage_);
    toolbar_->show();
    railBar_->show();
    updateTitle();
    updateTargets();
    updateActions();
    AppContext::instance().recents().add(controller_->session()->root(), controller_->session()->descriptor().name);
    bool hasDesign = !controller_->model()->screens.empty();
    showWorkspace(hasDesign ? QStringLiteral("design") : QStringLiteral("code"));
    if (hasDesign)
        canvas_->zoomToFit();
    build_->refreshToolchains();
}

void MainWindow::onProjectClosed()
{
    workspace_.clear();
    for (const auto& [id, dock] : docks_)
        dock->hide();
    pages_->setCurrentWidget(welcome_);
    toolbar_->hide();
    railBar_->hide();
    welcome_->refresh();
    updateTitle();
    updateTargets();
    updateActions();
    statusProblems_->clear();
    statusBranch_->clear();
    statusServer_->clear();
    statusCursor_->clear();
    statusLanguage_->clear();
}

void MainWindow::showWorkspace(const QString& id)
{
    if (!controller_->hasProject())
        return;
    if (!workspace_.isEmpty() && workspace_ != id)
        saveLayout();
    workspace_ = id;
    for (const auto& workspace : workspaces_) {
        if (workspace.id != id)
            continue;
        workspace.button->setChecked(true);
        center_->setCurrentWidget(workspace.center);
        for (const auto& [dockId, dock] : docks_) {
            if (!workspace.docks.contains(dockId))
                dock->hide();
        }
        for (const auto& [dockId, dock] : docks_) {
            if (workspace.docks.contains(dockId))
                dock->show();
        }
        std::string saved = AppContext::instance().settings().getString("layout." + fromQ(id));
        if (!saved.empty())
            restoreState(QByteArray::fromBase64(QByteArray::fromStdString(saved)), kLayoutVersion);
        if (id == QStringLiteral("preview"))
            preview_->refresh();
        if (id == QStringLiteral("debug"))
            docks_[QStringLiteral("debug")]->raise();
        if (id == QStringLiteral("assistant"))
            docks_[QStringLiteral("assistant")]->raise();
        if (id == QStringLiteral("code") || id == QStringLiteral("project"))
            docks_[QStringLiteral("files")]->raise();
        if (id == QStringLiteral("design")) {
            docks_[QStringLiteral("inspector")]->raise();
            docks_[QStringLiteral("components")]->raise();
        }
        break;
    }
    updateDockTitles();
}

void MainWindow::showDock(const QString& id)
{
    auto it = docks_.find(id);
    if (it == docks_.end())
        return;
    it->second->show();
    it->second->raise();
    updateDockTitles();
}

void MainWindow::updateDockTitles()
{
    for (const auto& [id, dock] : docks_) {
        bool tabbed = false;
        for (QDockWidget* other : tabifiedDockWidgets(dock))
            tabbed = tabbed || !other->isHidden();
        bool hideTitle = tabbed || id == QStringLiteral("screens");
        if (hideTitle && dock->titleBarWidget() == nullptr)
            dock->setTitleBarWidget(new QWidget(dock));
        else if (!hideTitle && dock->titleBarWidget() != nullptr) {
            QWidget* old = dock->titleBarWidget();
            dock->setTitleBarWidget(nullptr);
            delete old;
        }
    }
}

void MainWindow::saveLayout()
{
    if (workspace_.isEmpty())
        return;
    AppContext::instance().settings().set("layout." + fromQ(workspace_), saveState(kLayoutVersion).toBase64().toStdString());
}

void MainWindow::restoreLayout()
{
    std::string geometry = AppContext::instance().settings().getString("window.geometry");
    if (!geometry.empty())
        restoreGeometry(QByteArray::fromBase64(QByteArray::fromStdString(geometry)));
}

bool MainWindow::saveEverything()
{
    if (!controller_->hasProject())
        return true;
    bool ok = editors_->saveAll();
    auto status = controller_->saveAll();
    if (!status) {
        statusText_->setText(toQ(status.error().message));
        return false;
    }
    if (ok)
        statusSaved_->setText(tr("Saved"));
    return ok;
}

void MainWindow::undo()
{
    if (center_->currentWidget() == editors_ && editors_->currentEditor() != nullptr) {
        editors_->currentEditor()->undo();
        return;
    }
    if (controller_->hasProject())
        controller_->editor()->undo();
}

void MainWindow::redo()
{
    if (center_->currentWidget() == editors_ && editors_->currentEditor() != nullptr) {
        editors_->currentEditor()->redo();
        return;
    }
    if (controller_->hasProject())
        controller_->editor()->redo();
}

void MainWindow::newProject()
{
    NewProjectDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (!saveEverything() || !editors_->closeAll())
        return;
    auto status = controller_->createProject(dialog.options());
    if (!status)
        QMessageBox::warning(this, tr("New project"), toQ(status.error().message));
}

void MainWindow::openProject()
{
    QString folder = QFileDialog::getExistingDirectory(this, tr("Open project"));
    if (!folder.isEmpty())
        openProjectFolder(folder);
}

bool MainWindow::openProjectFolder(const QString& path)
{
    if (!saveEverything() || !editors_->closeAll())
        return false;
    auto status = controller_->openProject(path);
    if (!status) {
        QMessageBox::warning(this, tr("Open project"), toQ(status.error().message));
        AppContext::instance().recents().remove(paths::fromUtf8(fromQ(path)));
        welcome_->refresh();
        return false;
    }
    return true;
}

void MainWindow::importFolder()
{
    QString folder = QFileDialog::getExistingDirectory(this, tr("Open an existing code folder"));
    if (folder.isEmpty() || !saveEverything() || !editors_->closeAll())
        return;
    auto status = controller_->importFolder(folder);
    if (!status)
        QMessageBox::warning(this, tr("Open folder"), toQ(status.error().message));
}

void MainWindow::openSettings(const QString& page)
{
    SettingsDialog dialog(this);
    if (!page.isEmpty())
        dialog.showPage(page);
    connect(&dialog, &SettingsDialog::settingsApplied, this, &MainWindow::applyEditorSettings);
    dialog.exec();
    assistant_->reloadProviders();
}

void MainWindow::applyEditorSettings()
{
    auto& settings = AppContext::instance().settings();
    for (const auto& path : editors_->openPaths()) {
        CodeEditor* editor = editors_->openFile(path);
        if (editor != nullptr) {
            editor->setFontPointSize(settings.getInt("editor.fontSize", 10));
            editor->setMinimap(settings.getBool("editor.minimap", false));
        }
    }
}

void MainWindow::quickOpen()
{
    if (!controller_->hasProject())
        return;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("quickOpen"));
    dialog.setWindowTitle(tr("Go to file"));
    dialog.resize(560, 400);
    auto* layout = new QVBoxLayout(&dialog);
    auto* filter = new QLineEdit(&dialog);
    filter->setPlaceholderText(tr("Type part of a file name"));
    layout->addWidget(filter);
    auto* list = new QListWidget(&dialog);
    layout->addWidget(list, 1);
    ListOptions options;
    options.recursive = true;
    options.ignoredNames = defaultIgnoredNames();
    QStringList all;
    auto entries = controller_->session()->filesystem().list("", options);
    if (entries) {
        for (const auto& entry : *entries) {
            if (!entry.directory)
                all << toQ(entry.path);
        }
    }
    auto fill = [list, &all](const QString& text) {
        list->clear();
        int shown = 0;
        for (const auto& path : all) {
            if (path.contains(text, Qt::CaseInsensitive)) {
                list->addItem(path);
                if (++shown >= 300)
                    break;
            }
        }
        if (list->count() > 0)
            list->setCurrentRow(0);
    };
    fill(QString());
    connect(filter, &QLineEdit::textChanged, &dialog, fill);
    auto choose = [&] {
        if (list->currentItem() != nullptr) {
            QString path = list->currentItem()->text();
            dialog.accept();
            controller_->openFile(path);
        }
    };
    connect(filter, &QLineEdit::returnPressed, &dialog, choose);
    connect(list, &QListWidget::itemActivated, &dialog, choose);
    dialog.exec();
}

void MainWindow::checkRecovery(int buffers, bool design)
{
    QStringList parts;
    if (buffers > 0)
        parts << tr("%n unsaved file(s)", nullptr, buffers);
    if (design)
        parts << tr("unsaved design changes");
    auto answer = QMessageBox::question(this, tr("Restore your work?"), tr("FormaIDE didn't close normally last time. It kept %1. Restore them now?").arg(parts.join(tr(" and "))));
    auto& recovery = controller_->session()->recovery();
    if (answer == QMessageBox::Yes) {
        if (design)
            controller_->session()->restoreAutosave();
        auto list = recovery.buffers();
        if (list) {
            for (const auto& buffer : *list)
                editors_->restoreBuffer(toQ(buffer.path), toQ(buffer.content));
        }
        showWorkspace(buffers > 0 ? QStringLiteral("code") : workspace_);
    } else {
        auto list = recovery.buffers();
        if (list) {
            for (const auto& buffer : *list) {
                auto cleared = recovery.clearBuffer(buffer.path);
                (void)cleared;
            }
        }
        auto cleared = recovery.clearDesignState();
        (void)cleared;
    }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (controller_->hasProject()) {
        if (!saveEverything() || !editors_->closeAll()) {
            event->ignore();
            return;
        }
        saveLayout();
        controller_->closeProject();
    }
    auto& app = AppContext::instance();
    app.settings().set("window.geometry", saveGeometry().toBase64().toStdString());
    app.saveSettings();
    event->accept();
}

}
