#include "shell/main_window.h"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QMenu>
#include <QStyle>
#include <QTimer>
#include <thread>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QLayout>
#include <QScreen>
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

constexpr int kLayoutVersion = 2;

class DeviceCombo : public QComboBox {
public:
    using QComboBox::QComboBox;
    std::function<void()> beforeShow;

    void showPopup() override
    {
        if (beforeShow)
            beforeShow();
        QComboBox::showPopup();
    }
};

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
    buildToolbar();
    buildMenus();
    buildStatusBar();
    wireController();
    onProjectClosed();
    layout()->invalidate();
    layout()->activate();
    QSize available = QGuiApplication::primaryScreen()->availableSize();
    resize(std::min(1440, available.width() * 95 / 100), std::min(900, available.height() * 95 / 100));
    restoreLayout();
    QTimer::singleShot(300, this, &MainWindow::checkToolsSummary);
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
    center_ = new QStackedWidget(mainPage_);
    center_->setObjectName(QStringLiteral("workspaceCenter"));
    canvas_ = new CanvasWidget(controller_, center_);
    editors_ = new EditorArea(controller_, center_);
    projectPage_ = new ProjectPage(controller_, center_);
    preview_ = new PreviewPanel(controller_, center_);
    center_->addWidget(canvas_);
    center_->addWidget(editors_);
    center_->addWidget(projectPage_);
    center_->addWidget(preview_);
    row->addWidget(center_, 1);
    pages_->addWidget(mainPage_);
    setCentralWidget(pages_);
    connect(welcome_, &WelcomePage::newRequested, this, [this] { newProject(); });
    connect(welcome_, &WelcomePage::templateRequested, this, [this](const QString& id) { newProject(id); });
    connect(welcome_, &WelcomePage::openRequested, this, &MainWindow::openProject);
    connect(welcome_, &WelcomePage::importRequested, this, &MainWindow::importFolder);
    connect(welcome_, &WelcomePage::recentRequested, this, &MainWindow::openProjectFolder);
    connect(welcome_, &WelcomePage::toolsRequested, this, [this] { openSetup(); });
    connect(welcome_, &WelcomePage::settingsRequested, this, [this] { openSettings(); });
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
    build_ = new BuildPanel(controller_, this);
    debug_ = new DebugPanel(controller_, editors_, this);
    assistant_ = new AssistantPanel(controller_, editors_, this);
    inspector_->setFrameProvider([this](const QString& screen, const QString& component) { return canvas_->worldFrameOf(screen, component); });

    auto* screens = addDock(QStringLiteral("screens"), tr("Screens"), screens_, Qt::LeftDockWidgetArea);
    auto* layers = addDock(QStringLiteral("layers"), tr("Layers"), layers_, Qt::LeftDockWidgetArea);
    auto* components = addDock(QStringLiteral("components"), tr("Components"), palette_, Qt::LeftDockWidgetArea);
    tabifyDockWidget(screens, layers);
    tabifyDockWidget(layers, components);
    auto* filesDock = addDock(QStringLiteral("files"), tr("Files"), files_, Qt::LeftDockWidgetArea);
    auto* searchDock = addDock(QStringLiteral("search"), tr("Search"), search_, Qt::LeftDockWidgetArea);
    auto* gitDock = addDock(QStringLiteral("git"), tr("Git"), git_, Qt::LeftDockWidgetArea);
    tabifyDockWidget(components, filesDock);
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
    auto* buildDock = addDock(QStringLiteral("build"), tr("Build"), build_, Qt::BottomDockWidgetArea);
    auto* diffDock = addDock(QStringLiteral("diff"), tr("Changes"), diff_, Qt::BottomDockWidgetArea);
    auto* automationDock = addDock(QStringLiteral("automation"), tr("Activity"), automation_, Qt::BottomDockWidgetArea);
    auto* debugDock = addDock(QStringLiteral("debug"), tr("Debugger"), debug_, Qt::BottomDockWidgetArea);
    tabifyDockWidget(terminalDock, outputDock);
    tabifyDockWidget(outputDock, problemsDock);
    tabifyDockWidget(problemsDock, buildDock);
    tabifyDockWidget(buildDock, automationDock);
    tabifyDockWidget(automationDock, diffDock);
    tabifyDockWidget(diffDock, debugDock);
    terminalDock->raise();
    resizeDocks({layers, inspectorDock}, {264, 288}, Qt::Horizontal);
    resizeDocks({terminalDock}, {240}, Qt::Vertical);

    const QStringList standardBottom = {QStringLiteral("terminal"), QStringLiteral("output"), QStringLiteral("problems"), QStringLiteral("build"), QStringLiteral("automation")};
    workspaces_ = {
        {QStringLiteral("design"), tr("Design"), QStringLiteral("design"), canvas_, {QStringLiteral("layers"), QStringLiteral("components"), QStringLiteral("inspector")}, standardBottom, false},
        {QStringLiteral("code"), tr("Code"), QStringLiteral("code"), editors_, {QStringLiteral("files"), QStringLiteral("search"), QStringLiteral("git")}, standardBottom, true},
        {QStringLiteral("preview"), tr("Preview"), QStringLiteral("preview"), preview_, {QStringLiteral("screens")}, standardBottom, false},
        {QStringLiteral("debug"), tr("Debug"), QStringLiteral("debug"), editors_, {QStringLiteral("files")}, {QStringLiteral("debug"), QStringLiteral("output"), QStringLiteral("terminal")}, true},
        {QStringLiteral("project"), tr("Project"), QStringLiteral("project"), projectPage_, {QStringLiteral("files")}, standardBottom, false},
    };
}

void MainWindow::buildToolbar()
{
    toolbar_ = addToolBar(tr("Main"));
    toolbar_->setObjectName(QStringLiteral("mainToolbar"));
    toolbar_->setMovable(false);
    toolbar_->setFloatable(false);
    toolbar_->setIconSize(QSize(16, 16));
    toolbar_->setContextMenuPolicy(Qt::PreventContextMenu);
    auto* bar = new QFrame(toolbar_);
    bar->setObjectName(QStringLiteral("topBar"));
    bar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(8, 6, 10, 6);
    row->setSpacing(4);
    auto makeAction = [this](const QString& name, const QString& iconName, const QString& text, const QKeySequence& key) {
        auto* action = new QAction(icon(iconName), text, this);
        action->setObjectName(name);
        if (!key.isEmpty())
            action->setShortcut(key);
        addAction(action);
        themedActions_.emplace_back(action, iconName);
        return action;
    };
    auto makeButton = [bar](QAction* action, const QString& tip) {
        auto* button = new QToolButton(bar);
        button->setDefaultAction(action);
        button->setProperty("role", "top");
        button->setIconSize(QSize(16, 16));
        button->setToolTip(tip);
        return button;
    };
    auto* menuButton = new QToolButton(bar);
    menuButton->setObjectName(QStringLiteral("mainMenuButton"));
    menuButton->setProperty("role", "top");
    menuButton->setIcon(icon(QStringLiteral("menu")));
    menuButton->setIconSize(QSize(16, 16));
    menuButton->setToolTip(tr("Menu"));
    menuButton->setPopupMode(QToolButton::InstantPopup);
    auto* mainMenu = new QMenu(menuButton);
    mainMenu->setObjectName(QStringLiteral("mainMenu"));
    menuButton->setMenu(mainMenu);
    row->addWidget(menuButton);
    projectLabel_ = new QLabel(bar);
    projectLabel_->setObjectName(QStringLiteral("toolbarProject"));
    projectLabel_->setContentsMargins(4, 0, 8, 0);
    QFont bold = projectLabel_->font();
    bold.setWeight(QFont::DemiBold);
    projectLabel_->setFont(bold);
    row->addWidget(projectLabel_);
    saveAction_ = makeAction(QStringLiteral("actionSave"), QStringLiteral("save"), tr("Save"), QKeySequence::Save);
    connect(saveAction_, &QAction::triggered, this, &MainWindow::saveEverything);
    undoAction_ = makeAction(QStringLiteral("actionUndo"), QStringLiteral("undo"), tr("Undo"), QKeySequence::Undo);
    connect(undoAction_, &QAction::triggered, this, &MainWindow::undo);
    redoAction_ = makeAction(QStringLiteral("actionRedo"), QStringLiteral("redo"), tr("Redo"), QKeySequence::Redo);
    connect(redoAction_, &QAction::triggered, this, &MainWindow::redo);
    row->addWidget(makeButton(undoAction_, tr("Undo (Ctrl+Z)")));
    row->addWidget(makeButton(redoAction_, tr("Redo (Ctrl+Y)")));
    row->addStretch(1);
    auto* modes = new QFrame(bar);
    modes->setObjectName(QStringLiteral("modeSwitch"));
    auto* modeRow = new QHBoxLayout(modes);
    modeRow->setContentsMargins(2, 2, 2, 2);
    modeRow->setSpacing(1);
    modeGroup_ = new QButtonGroup(this);
    modeGroup_->setExclusive(true);
    for (auto& workspace : workspaces_) {
        auto* button = new QToolButton(modes);
        button->setObjectName(QStringLiteral("mode.") + workspace.id);
        button->setText(workspace.title);
        button->setProperty("role", "mode");
        button->setCheckable(true);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        QString id = workspace.id;
        connect(button, &QToolButton::clicked, this, [this, id] { showWorkspace(id); });
        modeGroup_->addButton(button);
        modeRow->addWidget(button);
        workspace.button = button;
    }
    row->addWidget(modes);
    row->addStretch(1);
    runStatus_ = new QLabel(bar);
    runStatus_->setObjectName(QStringLiteral("runStatus"));
    row->addWidget(runStatus_);
    target_ = new QComboBox(bar);
    target_->setObjectName(QStringLiteral("toolbarTarget"));
    target_->setToolTip(tr("What to build"));
    target_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    row->addWidget(target_);
    auto* device = new DeviceCombo(bar);
    device->beforeShow = [this] { refreshDevices(); };
    device_ = device;
    device_->setObjectName(QStringLiteral("toolbarDevice"));
    device_->setToolTip(tr("Where to run the app"));
    device_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    row->addWidget(device_);
    configuration_ = new QComboBox(bar);
    configuration_->setObjectName(QStringLiteral("toolbarConfiguration"));
    configuration_->addItem(tr("Debug"), QStringLiteral("debug"));
    configuration_->addItem(tr("Release"), QStringLiteral("release"));
    configuration_->setToolTip(tr("Build configuration"));
    row->addWidget(configuration_);
    buildAction_ = makeAction(QStringLiteral("actionBuild"), QStringLiteral("build"), tr("Build"), QKeySequence(Qt::CTRL | Qt::Key_B));
    connect(buildAction_, &QAction::triggered, this, [this] {
        saveEverything();
        controller_->startBuild(configuration_->currentData().toString());
        showDock(QStringLiteral("output"));
    });
    row->addWidget(makeButton(buildAction_, tr("Build (Ctrl+B)")));
    runAction_ = makeAction(QStringLiteral("actionRun"), QStringLiteral("run"), tr("Run"), QKeySequence(Qt::CTRL | Qt::Key_R));
    runAction_->setIcon(Theme::instance().icon(QStringLiteral("run"), colors().accentText));
    connect(runAction_, &QAction::triggered, this, [this] {
        saveEverything();
        controller_->buildAndRun(configuration_->currentData().toString());
        showDock(QStringLiteral("output"));
    });
    auto* runButton = new QToolButton(bar);
    runButton->setObjectName(QStringLiteral("runButton"));
    runButton->setDefaultAction(runAction_);
    runButton->setProperty("role", "runPrimary");
    runButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    runButton->setToolTip(tr("Build and run (Ctrl+R)"));
    row->addWidget(runButton);
    stopAction_ = makeAction(QStringLiteral("actionStop"), QStringLiteral("stop"), tr("Stop"), QKeySequence());
    connect(stopAction_, &QAction::triggered, this, [this] {
        controller_->cancelBuild();
        controller_->stopApp();
    });
    row->addWidget(makeButton(stopAction_, tr("Stop")));
    auto* divider = new QFrame(bar);
    divider->setProperty("role", "sep");
    row->addWidget(divider);
    panelButton_ = new QToolButton(bar);
    panelButton_->setObjectName(QStringLiteral("panelToggle"));
    panelButton_->setProperty("role", "top");
    panelButton_->setIcon(icon(QStringLiteral("terminal")));
    panelButton_->setIconSize(QSize(16, 16));
    panelButton_->setCheckable(true);
    panelButton_->setToolTip(tr("Show or hide the terminal, output and problems"));
    connect(panelButton_, &QToolButton::clicked, this, &MainWindow::toggleBottomPanel);
    row->addWidget(panelButton_);
    assistantButton_ = new QToolButton(bar);
    assistantButton_->setObjectName(QStringLiteral("assistantToggle"));
    assistantButton_->setProperty("role", "top");
    assistantButton_->setIcon(icon(QStringLiteral("assistant")));
    assistantButton_->setIconSize(QSize(16, 16));
    assistantButton_->setCheckable(true);
    assistantButton_->setToolTip(tr("Show or hide the assistant"));
    connect(assistantButton_, &QToolButton::clicked, this, &MainWindow::setAssistantOpen);
    row->addWidget(assistantButton_);
    auto* settingsButton = new QToolButton(bar);
    settingsButton->setObjectName(QStringLiteral("settingsButton"));
    settingsButton->setProperty("role", "top");
    settingsButton->setIcon(icon(QStringLiteral("settings")));
    settingsButton->setIconSize(QSize(16, 16));
    settingsButton->setToolTip(tr("Settings"));
    connect(settingsButton, &QToolButton::clicked, this, [this] { openSettings(); });
    row->addWidget(settingsButton);
    toolbar_->addWidget(bar);
    connect(target_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (updatingTargets_ || !controller_->hasProject())
            return;
        controller_->session()->descriptor().activeTarget = fromQ(target_->currentData().toString());
        auto saved = controller_->session()->saveDescriptor();
        (void)saved;
        refreshDevices();
    });
    connect(device_, &QComboBox::activated, this, [this](int) {
        QString id = device_->currentData().toString();
        if (id == QStringLiteral("@setup")) {
            openSetup({QStringLiteral("emulator")}, tr("Connect a phone with USB debugging turned on, or install the emulator to run on a virtual phone."));
            refreshDevices();
            return;
        }
        controller_->setRunDevice(id);
    });
    connect(&Theme::instance(), &Theme::changed, this, [this, menuButton, settingsButton] {
        for (const auto& [action, name] : themedActions_)
            action->setIcon(icon(name));
        runAction_->setIcon(Theme::instance().icon(QStringLiteral("run"), colors().accentText));
        menuButton->setIcon(icon(QStringLiteral("menu")));
        settingsButton->setIcon(icon(QStringLiteral("settings")));
        panelButton_->setIcon(icon(QStringLiteral("terminal")));
        assistantButton_->setIcon(icon(QStringLiteral("assistant")));
    });
}

void MainWindow::buildMenus()
{
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("New project..."), QKeySequence::New, this, [this] { newProject(); });
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
    file->addAction(tr("Tools and toolchains..."), this, [this] { openSetup(); });
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
    edit->addAction(tr("Find a command..."), QKeySequence(Qt::CTRL | Qt::Key_K), this, &MainWindow::commandPalette);
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
    view->addAction(tr("Show or hide the bottom panel"), QKeySequence(Qt::CTRL | Qt::Key_J), this, &MainWindow::toggleBottomPanel);
    view->addSeparator();
    view->addAction(tr("Zoom canvas to fit"), QKeySequence(Qt::CTRL | Qt::Key_0), this, [this] { canvas_->zoomToFit(); });
    QAction* rulers = view->addAction(tr("Canvas rulers"));
    rulers->setCheckable(true);
    connect(rulers, &QAction::toggled, canvas_, &CanvasWidget::setRulersVisible);
    QAction* grid = view->addAction(tr("Canvas pixel grid"));
    grid->setCheckable(true);
    connect(grid, &QAction::toggled, canvas_, &CanvasWidget::setGridVisible);
    QAction* snap = view->addAction(tr("Snap while dragging"));
    snap->setCheckable(true);
    snap->setChecked(canvas_->snapEnabled());
    connect(snap, &QAction::toggled, canvas_, &CanvasWidget::setSnapEnabled);
    QAction* flows = view->addAction(tr("Show links between screens"));
    flows->setCheckable(true);
    connect(flows, &QAction::toggled, canvas_, &CanvasWidget::setFlowsVisible);

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
    build->addAction(tr("Tools and toolchains..."), this, [this] { openSetup(); });

    QMenu* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("Documentation"), this, [] { QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/grangedevgroup-code/FormaIDE/tree/main/documentation"))); });
    help->addAction(tr("Report a problem"), this, [] { QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/grangedevgroup-code/FormaIDE/issues"))); });
    help->addAction(tr("About FormaIDE"), this, [this] {
        QMessageBox::about(this, tr("About FormaIDE"), tr("FormaIDE is a native, local-first visual development environment.\nMIT license.\n\nBuilt with Qt, Scintilla and Lexilla. See the third-party notices in the documentation."));
    });

    menus_ = {file, edit, view, build, help};
    auto* menuButton = toolbar_->findChild<QToolButton*>(QStringLiteral("mainMenuButton"));
    for (QMenu* menu : menus_) {
        menuButton->menu()->addMenu(menu);
        addActions(menu->actions());
    }
#if !defined(__APPLE__)
    menuBar()->setVisible(false);
#endif
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
    connect(controller_, &ProjectController::buildStarted, this, [this] {
        updateActions();
        setRunStatus(tr("Building..."));
    });
    connect(controller_, &ProjectController::buildStep, this, [this](const QString& label) { setRunStatus(label + QStringLiteral("...")); });
    connect(controller_, &ProjectController::buildFinished, this, [this](bool success, int errors, int, qint64 millis) {
        updateActions();
        if (success)
            setRunStatus(tr("Built in %1 s").arg(millis / 1000.0, 0, 'f', 1), QStringLiteral("ok"));
        else
            setRunStatus(errors > 1 ? tr("Build failed: %1 errors").arg(errors) : tr("Build failed"), QStringLiteral("bad"));
        if (errors > 0)
            showDock(QStringLiteral("problems"));
    });
    connect(controller_, &ProjectController::appStarted, this, [this](const QString& command) {
        updateActions();
        if (!controller_->isAndroidProject())
            setRunStatus(tr("Running"), QStringLiteral("ok"));
        Q_UNUSED(command);
    });
    connect(controller_, &ProjectController::runProgress, this, [this](const QString& text) { setRunStatus(text, text.startsWith(tr("Running")) ? QStringLiteral("ok") : QString()); });
    connect(controller_, &ProjectController::appStopped, this, [this](int code) {
        updateActions();
        setRunStatus(code == 0 ? tr("Stopped") : tr("The app stopped with code %1").arg(code), code == 0 ? QString() : QStringLiteral("bad"));
    });
    connect(controller_, &ProjectController::toolsMissing, this, [this](const QStringList& tools, const QString& message) {
        setRunStatus(tr("Tools needed"), QStringLiteral("bad"));
        openSetup(tools, message);
    });
    connect(controller_, &ProjectController::runNeedsDevice, this, [this](const QString& message, const QStringList& tools) {
        setRunStatus(tr("No device to run on"), QStringLiteral("bad"));
        output_->appendLine(message, true);
        refreshDevices();
        openSetup(tools, message);
    });
    connect(controller_, &ProjectController::noBuildSystem, this, [this](bool forRun) {
        CodeEditor* editor = editors_->currentEditor();
        if (forRun && editor != nullptr && editor->language() != nullptr && !editor->language()->run.empty()) {
            editors_->runCurrentFile();
            setRunStatus(tr("Running this file in the terminal"), QStringLiteral("ok"));
            return;
        }
        output_->appendLine(forRun ? tr("There's nothing to run yet. Open a file that FormaIDE knows how to run, or add a build file such as CMakeLists.txt, package.json or Cargo.toml.")
                                   : tr("There's nothing to build yet. FormaIDE looks for a build file such as CMakeLists.txt, build.gradle.kts, package.json or Cargo.toml."), true);
        setRunStatus(forRun ? tr("Nothing to run") : tr("Nothing to build"), QStringLiteral("bad"));
        showDock(QStringLiteral("output"));
    });
    connect(controller_, &ProjectController::runInTerminal, this, [this](const QString& command) {
        showDock(QStringLiteral("terminal"));
        terminal_->runInNewTerminal(command);
        setRunStatus(tr("Running in the terminal"), QStringLiteral("ok"));
    });
    connect(controller_, &ProjectController::runUrlFound, this, [this](const QString& url) {
        setRunStatus(tr("Running at %1").arg(url), QStringLiteral("ok"));
        output_->appendLine(tr("Opening %1 in your browser").arg(url));
        if (AppContext::instance().settings().getBool("run.openBrowser", true) && !qEnvironmentVariableIsSet("FORMA_NO_BROWSER"))
            QDesktopServices::openUrl(QUrl(url));
    });
    connect(build_, &BuildPanel::setupRequested, this, [this] { openSetup(); });
    connect(inspector_, &InspectorPanel::alignRequested, this, [this](int edge) { canvas_->alignSelection(static_cast<design::AlignEdge>(edge)); });
    connect(inspector_, &InspectorPanel::distributeRequested, this, [this](int axis) { canvas_->distributeSelection(static_cast<design::DistributeAxis>(axis)); });
    connect(inspector_, &InspectorPanel::prototypeShown, canvas_, &CanvasWidget::setFlowsVisible);
    connect(layers_, &LayersPanel::screenActivated, canvas_, &CanvasWidget::centerOnScreen);
    connect(editors_, &EditorArea::serverInstallRequested, this, [this](const QString& language, const QString& server, const QString& hint) {
        openSetup();
        setup_->focusServer(language, server, hint);
    });
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
        statusProblems_->setText(QStringLiteral("%1, %2").arg(errors == 1 ? tr("1 error") : tr("%1 errors").arg(errors), warnings == 1 ? tr("1 warning") : tr("%1 warnings").arg(warnings)));
    });
    connect(git_, &GitPanel::branchChanged, statusBranch_, &QLabel::setText);
    connect(git_, &GitPanel::diffRequested, this, [this](const QString&, const QString& text) {
        diff_->showDiff(text);
        diffOpen_ = true;
        showDock(QStringLiteral("diff"));
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
        showDock(QStringLiteral("terminal"));
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
    connect(assistant_, &AssistantPanel::setupRequested, this, [this] { openSettings(tr("Assistant")); });
    connect(preview_, &PreviewPanel::runRequested, runAction_, &QAction::trigger);
    connect(screens_, &ScreensPanel::screenActivated, canvas_, &CanvasWidget::centerOnScreen);
}

void MainWindow::setRunStatus(const QString& text, const QString& state)
{
    runStatus_->setText(runStatus_->fontMetrics().elidedText(text, Qt::ElideMiddle, 260));
    runStatus_->setToolTip(text);
    runStatus_->setProperty("status", state);
    runStatus_->style()->unpolish(runStatus_);
    runStatus_->style()->polish(runStatus_);
}

QString MainWindow::runStatusText() const
{
    return runStatus_->toolTip();
}

void MainWindow::setAssistantOpen(bool open)
{
    assistantOpen_ = open;
    assistantButton_->setChecked(open);
    QDockWidget* dock = docks_[QStringLiteral("assistant")];
    dock->setVisible(open);
    if (open)
        dock->raise();
    else if (docks_[QStringLiteral("inspector")]->isVisible())
        docks_[QStringLiteral("inspector")]->raise();
    updateDockTitles();
}

void MainWindow::openSetup(const QStringList& tools, const QString& reason)
{
    if (!setup_) {
        setup_ = new SetupDialog(this);
        connect(setup_, &SetupDialog::toolsChanged, this, [this] {
            build_->refreshToolchains(true);
            editors_->retryLanguageServers();
            refreshDevices();
            checkToolsSummary();
        });
    }
    setup_->show();
    setup_->raise();
    setup_->activateWindow();
    if (!tools.isEmpty())
        setup_->focusTools(tools, reason);
}

void MainWindow::checkToolsSummary()
{
    QPointer<MainWindow> self(this);
    std::thread([self] {
        auto& tools = AppContext::instance().tools();
        int ready = 0;
        int total = 0;
        for (const auto& stack : build::toolStacksForHost()) {
            bool complete = true;
            for (const auto& id : stack.required)
                complete = complete && tools.isAvailable(id);
            ready += complete ? 1 : 0;
            ++total;
        }
        QMetaObject::invokeMethod(qApp, [self, ready, total] {
            if (self)
                self->welcome_->setToolsSummary(ready, total);
        }, Qt::QueuedConnection);
    }).detach();
}

void MainWindow::refreshDevices()
{
    updatingDevices_ = true;
    device_->clear();
    if (controller_->hasProject() && controller_->target() != nullptr) {
        auto devices = controller_->runDevices();
        for (const auto& item : devices) {
            device_->addItem(toQ(item.label), toQ(item.id));
            if (!item.ready)
                device_->setItemData(device_->count() - 1, 0, Qt::UserRole - 1);
        }
        if (controller_->isAndroidProject()) {
            if (devices.empty())
                device_->addItem(tr("No device"), QString());
            device_->addItem(tr("Set up a device..."), QStringLiteral("@setup"));
        }
        int index = device_->findData(controller_->runDevice());
        device_->setCurrentIndex(std::max(0, index));
    }
    device_->setVisible(device_->count() > 0);
    updatingDevices_ = false;
}

bool MainWindow::bottomPanelOpen() const
{
    for (const auto& workspace : workspaces_) {
        if (workspace.id == workspace_)
            return workspace.bottomOpen;
    }
    return false;
}

void MainWindow::toggleBottomPanel()
{
    for (auto& workspace : workspaces_) {
        if (workspace.id != workspace_)
            continue;
        workspace.bottomOpen = !workspace.bottomOpen;
        for (const auto& id : workspace.bottom)
            docks_[id]->setVisible(workspace.bottomOpen);
        if (diffOpen_)
            docks_[QStringLiteral("diff")]->setVisible(workspace.bottomOpen);
        if (workspace.bottomOpen)
            docks_[workspace.bottom.front()]->raise();
        panelButton_->setChecked(workspace.bottomOpen);
        break;
    }
    updateDockTitles();
}

void MainWindow::commandPalette()
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("commandPalette"));
    dialog.setWindowTitle(tr("Find a command"));
    dialog.resize(560, 420);
    auto* layout = new QVBoxLayout(&dialog);
    auto* filter = new QLineEdit(&dialog);
    filter->setObjectName(QStringLiteral("commandFilter"));
    filter->setPlaceholderText(tr("Type what you want to do"));
    layout->addWidget(filter);
    auto* list = new QListWidget(&dialog);
    list->setObjectName(QStringLiteral("commandList"));
    layout->addWidget(list, 1);
    std::vector<std::pair<QString, QAction*>> commands;
    std::function<void(QMenu*, const QString&)> collect = [&](QMenu* menu, const QString& prefix) {
        for (QAction* action : menu->actions()) {
            if (action->isSeparator())
                continue;
            if (action->menu() != nullptr) {
                collect(action->menu(), prefix + action->text().remove(QLatin1Char('&')) + QStringLiteral(": "));
                continue;
            }
            QString label = prefix + action->text().remove(QLatin1Char('&'));
            if (!action->shortcut().isEmpty())
                label += QStringLiteral("    ") + action->shortcut().toString(QKeySequence::NativeText);
            commands.emplace_back(label, action);
        }
    };
    for (QMenu* menu : menus_)
        collect(menu, menu->title().remove(QLatin1Char('&')) + QStringLiteral(": "));
    auto fill = [&](const QString& text) {
        list->clear();
        for (int i = 0; i < static_cast<int>(commands.size()); ++i) {
            const auto& [label, action] = commands[static_cast<std::size_t>(i)];
            if (!label.contains(text, Qt::CaseInsensitive) || !action->isEnabled())
                continue;
            auto* item = new QListWidgetItem(label, list);
            item->setData(Qt::UserRole, i);
        }
        if (list->count() > 0)
            list->setCurrentRow(0);
    };
    fill(QString());
    connect(filter, &QLineEdit::textChanged, &dialog, fill);
    QAction* chosen = nullptr;
    auto choose = [&] {
        if (list->currentItem() != nullptr) {
            chosen = commands[static_cast<std::size_t>(list->currentItem()->data(Qt::UserRole).toInt())].second;
            dialog.accept();
        }
    };
    connect(filter, &QLineEdit::returnPressed, &dialog, choose);
    connect(list, &QListWidget::itemActivated, &dialog, choose);
    dialog.exec();
    if (chosen != nullptr)
        chosen->trigger();
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
    target_->setVisible(target_->count() > 1);
    updatingTargets_ = false;
}

void MainWindow::onProjectOpened()
{
    pages_->setCurrentWidget(mainPage_);
    toolbar_->show();
    updateTitle();
    updateTargets();
    updateActions();
    setRunStatus(QString());
    QTimer::singleShot(0, this, &MainWindow::refreshDevices);
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
    if (id == QStringLiteral("build")) {
        showDock(QStringLiteral("build"));
        return;
    }
    if (id == QStringLiteral("assistant")) {
        setAssistantOpen(true);
        return;
    }
    if (!workspace_.isEmpty() && workspace_ != id)
        saveLayout();
    workspace_ = id;
    for (auto& workspace : workspaces_) {
        if (workspace.id != id)
            continue;
        workspace.button->setChecked(true);
        center_->setCurrentWidget(workspace.center);
        auto wanted = [&](const QString& dockId) {
            if (dockId == QStringLiteral("assistant"))
                return assistantOpen_;
            if (workspace.docks.contains(dockId))
                return true;
            if (!workspace.bottomOpen)
                return false;
            return workspace.bottom.contains(dockId) || (dockId == QStringLiteral("diff") && diffOpen_);
        };
        std::string saved = AppContext::instance().settings().getString("layout." + fromQ(id));
        if (!saved.empty())
            restoreState(QByteArray::fromBase64(QByteArray::fromStdString(saved)), kLayoutVersion);
        for (const auto& [dockId, dock] : docks_) {
            if (!wanted(dockId))
                dock->hide();
        }
        for (const auto& [dockId, dock] : docks_) {
            if (wanted(dockId))
                dock->show();
        }
        panelButton_->setChecked(workspace.bottomOpen);
        if (id == QStringLiteral("preview"))
            preview_->refresh();
        if (id == QStringLiteral("debug"))
            docks_[QStringLiteral("debug")]->raise();
        if (id == QStringLiteral("code") || id == QStringLiteral("project"))
            docks_[QStringLiteral("files")]->raise();
        if (id == QStringLiteral("code") && workspace.bottomOpen)
            docks_[QStringLiteral("terminal")]->raise();
        if (id == QStringLiteral("design")) {
            if (assistantOpen_)
                docks_[QStringLiteral("assistant")]->raise();
            else
                docks_[QStringLiteral("inspector")]->raise();
            docks_[QStringLiteral("layers")]->raise();
            canvas_->setFocus();
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
    for (auto& workspace : workspaces_) {
        if (workspace.id != workspace_ || !(workspace.bottom.contains(id) || id == QStringLiteral("diff")))
            continue;
        if (!workspace.bottomOpen) {
            workspace.bottomOpen = true;
            for (const auto& dockId : workspace.bottom)
                docks_[dockId]->show();
            panelButton_->setChecked(true);
        }
    }
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
        bool hideTitle = tabbed || id == QStringLiteral("screens") || id == QStringLiteral("inspector") || id == QStringLiteral("layers") || id == QStringLiteral("files");
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

void MainWindow::newProject(const QString& templateId)
{
    NewProjectDialog dialog(this);
    if (!templateId.isEmpty())
        dialog.selectTemplate(templateId);
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (!saveEverything() || !editors_->closeAll())
        return;
    auto status = controller_->createProject(dialog.options());
    if (!status) {
        QMessageBox::warning(this, tr("New project"), toQ(status.error().message));
        return;
    }
    QStringList missing = dialog.toolsToInstall();
    if (!missing.isEmpty())
        openSetup(missing, tr("This project needs a few tools before it can build."));
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
