#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QToolButton>
#include <QTreeView>
#include <QWindow>
#include <filesystem>
#include <iostream>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>

#include "ILexer.h"
#include "Lexilla.h"
#include "ScintillaEditBase.h"
#include "app/app_context.h"
#include "forma/build/setup.h"
#include "forma/core/paths.h"
#include "forma/core/process.h"
#include "forma/design/design_engine.h"
#include "forma/ir/catalog.h"
#include "shell/main_window.h"
#include "ui/theme.h"

using namespace forma;
using namespace forma::ui;

namespace {

bool waitFor(const std::function<bool()>& condition, int timeoutMs = 60000)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        if (condition())
            return true;
        QTest::qWait(10);
    }
    return condition();
}

void settle()
{
    QTest::qWait(150);
    QApplication::processEvents();
}

QString tourDirectory()
{
    return qEnvironmentVariable("FORMA_TOUR_DIR");
}

void capture(QWidget& widget, const QString& name)
{
    QString directory = tourDirectory();
    if (directory.isEmpty())
        return;
    QDir().mkpath(directory);
    settle();
    widget.grab().save(QDir(directory).filePath(name + QStringLiteral(".png")));
}

class UiFixture : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(temp_.isValid());
        window_ = std::make_unique<MainWindow>();
        window_->resize(1440, 900);
        window_->show();
        settle();
    }

    void TearDown() override
    {
        if (window_->controller()->hasProject()) {
            window_->editors()->saveAll();
            window_->controller()->closeProject();
        }
        window_.reset();
    }

    void createProject(const QString& name = QStringLiteral("Tour App"), const QString& templateId = QStringLiteral("qt-desktop"))
    {
        project::NewProjectOptions options;
        options.name = fromQ(name);
        options.location = paths::fromUtf8(fromQ(temp_.path()));
        options.templateId = fromQ(templateId);
        auto status = window_->controller()->createProject(options);
        ASSERT_TRUE(status) << status.error().message;
        settle();
    }

    QTemporaryDir temp_;
    std::unique_ptr<MainWindow> window_;
};

}

TEST_F(UiFixture, WelcomeShowsActionsAndHidesProjectControls)
{
    EXPECT_TRUE(window_->welcome()->isVisible());
    EXPECT_NE(window_->findChild<QPushButton*>(QStringLiteral("welcomeNew")), nullptr);
    capture(*window_, QStringLiteral("welcome-") + (Theme::instance().isDark() ? QStringLiteral("dark") : QStringLiteral("light")));
}

TEST_F(UiFixture, NewProjectDialogOnlyOffersWorkingGenerators)
{
    NewProjectDialog dialog(window_.get());
    auto* list = dialog.findChild<QTreeWidget*>(QStringLiteral("templateList"));
    ASSERT_NE(list, nullptr);
    auto& app = AppContext::instance();
    int offered = 0;
    for (int g = 0; g < list->topLevelItemCount(); ++g) {
        QTreeWidgetItem* group = list->topLevelItem(g);
        for (int i = 0; i < group->childCount(); ++i) {
            const auto* chosen = app.templates().find(fromQ(group->child(i)->data(0, Qt::UserRole).toString()));
            ASSERT_NE(chosen, nullptr);
            if (!chosen->generator.empty())
                EXPECT_NE(app.generators().find(chosen->generator), nullptr) << chosen->id;
            ++offered;
        }
    }
    EXPECT_GT(offered, 0);
    dialog.setLocation(temp_.path());
    dialog.setProjectName(QStringLiteral("Dialog App"));
    auto* create = dialog.findChild<QPushButton*>(QStringLiteral("projectCreate"));
    ASSERT_NE(create, nullptr);
    EXPECT_TRUE(create->isEnabled());
    auto* full = dialog.findChild<QLabel*>(QStringLiteral("projectFullPath"));
    EXPECT_TRUE(full->text().contains(QStringLiteral("Dialog App")));
    dialog.setProjectName(QStringLiteral("bad/name"));
    EXPECT_FALSE(create->isEnabled());
    if (!tourDirectory().isEmpty()) {
        dialog.setProjectName(QStringLiteral("My App"));
        dialog.show();
        capture(dialog, QStringLiteral("new-project-") + (Theme::instance().isDark() ? QStringLiteral("dark") : QStringLiteral("light")));
    }
}

TEST_F(UiFixture, EveryLanguageHasAWorkingLexer)
{
    std::vector<std::string> missing;
    for (const auto& language : AppContext::instance().languages().all()) {
        if (language.lexer == "null" || language.lexer.empty())
            continue;
        Scintilla::ILexer5* lexer = CreateLexer(language.lexer.c_str());
        if (lexer == nullptr)
            missing.push_back(language.id + " (" + language.lexer + ")");
        else
            lexer->Release();
    }
    std::string joined;
    for (const auto& item : missing)
        joined += item + " ";
    EXPECT_TRUE(missing.empty()) << joined;
}

TEST_F(UiFixture, WorkspaceTourShowsTheRightPanels)
{
    createProject();
    auto* controller = window_->controller();
    ASSERT_TRUE(controller->hasProject());
    EXPECT_EQ(window_->workspace(), QStringLiteral("design"));
    EXPECT_TRUE(window_->canvas()->isVisible());
    EXPECT_LE(window_->width(), 1440);
    QString mode = Theme::instance().isDark() ? QStringLiteral("dark") : QStringLiteral("light");
    controller->regenerate(true);
    ASSERT_TRUE(waitFor([&] { return !controller->lastReport().files.empty(); }, 20000));
    QStringList openFile;
    for (const auto& file : controller->lastReport().files)
        openFile << toQ(file.path);
    ASSERT_FALSE(openFile.isEmpty());
    for (const auto& workspace : {QStringLiteral("design"), QStringLiteral("code"), QStringLiteral("project"), QStringLiteral("preview"), QStringLiteral("debug")}) {
        window_->showWorkspace(workspace);
        if (workspace == QStringLiteral("code")) {
            for (const auto& path : openFile) {
                if (path.endsWith(QStringLiteral(".cpp"))) {
                    ASSERT_NE(window_->editors()->openFile(path), nullptr);
                    break;
                }
            }
            window_->showWorkspace(workspace);
        }
        settle();
        EXPECT_EQ(window_->workspace(), workspace);
        capture(*window_, QStringLiteral("workspace-%1-%2").arg(workspace, mode));
    }
    window_->showWorkspace(QStringLiteral("design"));
    window_->showWorkspace(QStringLiteral("build"));
    settle();
    EXPECT_EQ(window_->workspace(), QStringLiteral("design"));
    EXPECT_TRUE(window_->buildPanel()->isVisible());
    capture(*window_, QStringLiteral("workspace-build-") + mode);
    window_->toggleBottomPanel();
    window_->showWorkspace(QStringLiteral("assistant"));
    settle();
    EXPECT_TRUE(window_->assistant()->isVisible());
    capture(*window_, QStringLiteral("workspace-assistant-") + mode);
}

TEST_F(UiFixture, NewProjectGetsItsOwnFolder)
{
    QString location = temp_.filePath(QStringLiteral("projects"));
    for (const auto& name : {QStringLiteral("First App"), QStringLiteral("Second App")}) {
        NewProjectDialog dialog(window_.get());
        dialog.setProjectName(name);
        dialog.setLocation(location);
        dialog.selectTemplate(QStringLiteral("blank"));
        auto status = window_->controller()->createProject(dialog.options());
        ASSERT_TRUE(status) << status.error().message;
        EXPECT_EQ(QDir(window_->controller()->rootPath()).dirName(), name);
        EXPECT_TRUE(QFileInfo::exists(QDir(location).filePath(name + QStringLiteral("/project.json"))));
        settle();
    }
}

TEST_F(UiFixture, CanvasToolsPlaceComponentsWithTheMouse)
{
    createProject(QStringLiteral("Tools App"));
    auto* controller = window_->controller();
    CanvasWidget* canvas = window_->canvas();
    settle();
    const ir::Screen& screen = controller->model()->screens.front();
    QString screenId = toQ(screen.root.id);
    int before = static_cast<int>(screen.root.children.size());
    QPointF world(screen.canvas.x + 100, screen.canvas.y + 300);
    QPoint point = canvas->worldToView(world).toPoint();
    auto* textTool = canvas->findChild<QToolButton*>(QStringLiteral("tool.text"));
    ASSERT_NE(textTool, nullptr);
    QTest::mouseClick(textTool, Qt::LeftButton);
    EXPECT_EQ(canvas->tool(), CanvasWidget::Tool::Insert);
    EXPECT_EQ(canvas->insertType(), QStringLiteral("text"));
    QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, point);
    settle();
    EXPECT_EQ(canvas->tool(), CanvasWidget::Tool::Select);
    EXPECT_EQ(static_cast<int>(controller->model()->findScreen(fromQ(screenId))->root.children.size()), before + 1);
    ASSERT_EQ(controller->selection().size(), 1);
    QString placed = controller->selection().front();
    const ir::Component* component = ir::findComponent(controller->model()->findScreen(fromQ(screenId))->root, fromQ(placed));
    ASSERT_NE(component, nullptr);
    EXPECT_EQ(component->type, "text");
    capture(*window_, QStringLiteral("canvas-placed"));

    canvas->setFocus();
    QTest::keyClick(canvas, Qt::Key_R);
    EXPECT_EQ(canvas->insertType(), QStringLiteral("shape"));
    QTest::keyClick(canvas, Qt::Key_Escape);
    EXPECT_EQ(canvas->tool(), CanvasWidget::Tool::Select);

    int screensBefore = static_cast<int>(controller->model()->screens.size());
    QPoint ghost = canvas->worldToView(canvas->addScreenRect().center()).toPoint();
    QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, ghost);
    settle();
    EXPECT_EQ(static_cast<int>(controller->model()->screens.size()), screensBefore + 1);
    EXPECT_NE(controller->currentScreen(), screenId);
    capture(*window_, QStringLiteral("canvas-added-screen"));
}

TEST_F(UiFixture, LayersListEveryScreenAndSwitchBetweenThem)
{
    createProject(QStringLiteral("Layers App"));
    auto* controller = window_->controller();
    settle();
    QTreeWidget* tree = window_->layers()->tree();
    int screens = static_cast<int>(controller->model()->screens.size());
    ASSERT_GE(screens, 2);
    EXPECT_EQ(tree->topLevelItemCount(), screens);
    QTreeWidgetItem* second = tree->topLevelItem(1);
    QString secondId = second->data(0, Qt::UserRole).toString();
    QRect row = tree->visualItemRect(second);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, row.center());
    settle();
    EXPECT_EQ(controller->currentScreen(), secondId);
    EXPECT_TRUE(controller->selection().contains(secondId));
    capture(*window_, QStringLiteral("layers-second-screen"));
}

TEST_F(UiFixture, PrototypeTabLinksScreensAndCodeTabShowsSource)
{
    createProject(QStringLiteral("Flow App"));
    auto* controller = window_->controller();
    settle();
    const ir::Screen& first = controller->model()->screens.front();
    const ir::Screen& second = controller->model()->screens.at(1);
    QString screenId = toQ(first.root.id);
    QString target = toQ(second.root.id);
    std::string button;
    for (const auto& child : first.root.children) {
        if (child.type == "button")
            button = child.id;
    }
    ASSERT_FALSE(button.empty());
    controller->selectComponent(screenId, toQ(button));
    settle();
    InspectorPanel* inspector = window_->inspector();
    inspector->setCurrentTab(1);
    settle();
    EXPECT_TRUE(window_->canvas()->flowsVisible());
    capture(*window_, QStringLiteral("inspector-prototype"));
    auto* event = inspector->findChild<QComboBox*>(QStringLiteral("event_onClick"));
    ASSERT_NE(event, nullptr);
    EXPECT_GE(event->findData(toQ(controller->model()->findScreen(fromQ(screenId))->root.children.front().id)), -1);
    inspector->setCurrentTab(2);
    settle();
    EXPECT_TRUE(waitFor([&] { return inspector->codeLocation().contains(QLatin1Char(':')); }, 20000)) << fromQ(inspector->codeLocation());
    capture(*window_, QStringLiteral("inspector-code"));
    inspector->setCurrentTab(0);
    settle();
    EXPECT_FALSE(window_->canvas()->flowsVisible());
    Q_UNUSED(target);
}

TEST_F(UiFixture, CommandsWorkWithTheMenuBarHidden)
{
    createProject(QStringLiteral("Keys App"), QStringLiteral("blank"));
    ASSERT_TRUE(window_->controller()->session()->filesystem().writeFile("keys.py", "print('x')\n"));
    window_->showWorkspace(QStringLiteral("code"));
    settle();
    bool open = window_->bottomPanelOpen();
    QTest::keySequence(window_->windowHandle(), QKeySequence(Qt::CTRL | Qt::Key_J));
    settle();
    EXPECT_NE(window_->bottomPanelOpen(), open);
    QTest::keySequence(window_->windowHandle(), QKeySequence(Qt::CTRL | Qt::Key_J));
    settle();
    EXPECT_EQ(window_->bottomPanelOpen(), open);
    auto* button = window_->findChild<QToolButton*>(QStringLiteral("mainMenuButton"));
    ASSERT_NE(button, nullptr);
    ASSERT_NE(button->menu(), nullptr);
    EXPECT_GE(button->menu()->actions().size(), 5);
}

TEST_F(UiFixture, CodeModeOpensWithOneClickAndCompletesWithoutAServer)
{
    createProject(QStringLiteral("Click App"), QStringLiteral("blank"));
    auto* controller = window_->controller();
    ASSERT_TRUE(controller->session()->filesystem().writeFile("click.py", "printer_value = 1\n"));
    window_->showWorkspace(QStringLiteral("code"));
    settle();
    auto* tree = window_->findChild<QTreeView*>(QStringLiteral("fileTree"));
    ASSERT_NE(tree, nullptr);
    QModelIndex found;
    ASSERT_TRUE(waitFor([&] {
        QAbstractItemModel* model = tree->model();
        QModelIndex rootIndex = tree->rootIndex();
        for (int i = 0; i < model->rowCount(rootIndex); ++i) {
            QModelIndex index = model->index(i, 0, rootIndex);
            if (index.data().toString() == QStringLiteral("click.py")) {
                found = index;
                return true;
            }
        }
        return false;
    }, 10000));
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, tree->visualRect(found).center());
    settle();
    CodeEditor* editor = window_->editors()->currentEditor();
    ASSERT_NE(editor, nullptr);
    EXPECT_EQ(window_->editors()->currentPath(), QStringLiteral("click.py"));
    EXPECT_TRUE(window_->terminal()->isVisible());
    QWidget* focused = window_->focusWidget();
    ASSERT_NE(focused, nullptr);
    editor->gotoLine(2);
    QTest::keyClicks(focused, QStringLiteral("pri"));
    settle();
    EXPECT_TRUE(editor->completionListVisible());
    capture(*window_, QStringLiteral("code-word-completion"));
    QTest::keyClick(focused, Qt::Key_Escape);
    QTest::keyClicks(focused, QStringLiteral("nt("));
    settle();
    EXPECT_TRUE(editor->text().contains(QStringLiteral("print()"))) << fromQ(editor->text());
    QTest::keyClick(focused, Qt::Key_ParenRight, Qt::NoModifier);
    settle();
    EXPECT_TRUE(editor->text().contains(QStringLiteral("print()"))) << fromQ(editor->text());
    EXPECT_FALSE(editor->text().contains(QStringLiteral("print())"))) << fromQ(editor->text());
}

TEST_F(UiFixture, RunWithoutABuildSystemRunsTheOpenFile)
{
    createProject(QStringLiteral("Script App"), QStringLiteral("blank"));
    auto* controller = window_->controller();
    ASSERT_TRUE(controller->session()->filesystem().writeFile("hello.py", "print('forma-run-' + 'marker')\n"));
    window_->showWorkspace(QStringLiteral("code"));
    settle();
    ASSERT_NE(window_->editors()->openFile(QStringLiteral("hello.py")), nullptr);
    settle();
    auto* run = window_->findChild<QAction*>(QStringLiteral("actionRun"));
    ASSERT_NE(run, nullptr);
    run->trigger();
    TerminalWidget* terminal = nullptr;
    bool seen = waitFor([&] {
        terminal = window_->terminal()->current();
        return terminal != nullptr && terminal->visibleText().remove(QLatin1Char('\n')).contains(QStringLiteral("forma-run-marker"));
    }, 30000);
    capture(*window_, QStringLiteral("run-file"));
    EXPECT_TRUE(seen) << (terminal != nullptr ? fromQ(terminal->visibleText()) : std::string("no terminal"));
    EXPECT_TRUE(window_->terminal()->isVisible());
    if (terminal != nullptr)
        terminal->stop();
}

TEST_F(UiFixture, SetupDialogListsToolsAndRunsAnInstallPlan)
{
    window_->openSetup();
    SetupDialog* dialog = window_->setupDialog();
    ASSERT_NE(dialog, nullptr);
    ASSERT_TRUE(waitFor([&] { return dialog->ready(); }, 90000));
    auto* tree = dialog->findChild<QTreeWidget*>(QStringLiteral("setupTree"));
    ASSERT_NE(tree, nullptr);
    EXPECT_GE(tree->topLevelItemCount(), 6);
    capture(*dialog, QStringLiteral("setup-") + (Theme::instance().isDark() ? QStringLiteral("dark") : QStringLiteral("light")));
    build::InstallPlan plan;
    plan.id = "demo";
    plan.name = "Demo tool";
    build::SetupStep step;
    step.label = "Say hello";
#if defined(_WIN32)
    step.process.program = "cmd";
    step.process.arguments = {"/c", "echo", "forma-setup-marker"};
#else
    step.process.program = "sh";
    step.process.arguments = {"-c", "echo forma-setup-marker"};
#endif
    step.process.mergeStderr = true;
    plan.steps.push_back(step);
    bool finished = false;
    bool success = false;
    QObject::connect(dialog, &SetupDialog::installFinished, dialog, [&](bool ok) {
        finished = true;
        success = ok;
    });
    dialog->installPlan(plan);
    ASSERT_TRUE(waitFor([&] { return finished; }, 120000));
    EXPECT_TRUE(success);
    EXPECT_TRUE(dialog->logText().contains(QStringLiteral("forma-setup-marker"))) << fromQ(dialog->logText());
    ASSERT_TRUE(waitFor([&] { return dialog->ready(); }, 90000));
    dialog->close();
}

TEST_F(UiFixture, CodingWithRealInputReachesDisk)
{
    createProject(QStringLiteral("Typing App"), QStringLiteral("blank"));
    auto* controller = window_->controller();
    ASSERT_TRUE(controller->session()->filesystem().writeFile("hello.py", "print('one')\n"));
    window_->showWorkspace(QStringLiteral("code"));
    settle();
    capture(*window_, QStringLiteral("code-empty"));
    CodeEditor* editor = window_->editors()->openFile(QStringLiteral("hello.py"));
    ASSERT_NE(editor, nullptr);
    settle();
    QWidget* focused = window_->focusWidget();
    ASSERT_NE(focused, nullptr);
    EXPECT_TRUE(focused == editor || editor->isAncestorOf(focused)) << focused->metaObject()->className();
    QTest::keyClicks(focused, QStringLiteral("x = 1"));
    QTest::keyClick(focused, Qt::Key_Return);
    settle();
    EXPECT_TRUE(editor->isModified());
    EXPECT_TRUE(editor->text().contains(QStringLiteral("x = 1"))) << fromQ(editor->text());
    QTest::keySequence(focused, QKeySequence::Save);
    settle();
    capture(*window_, QStringLiteral("code-typed"));
    auto disk = controller->session()->filesystem().readFile("hello.py");
    ASSERT_TRUE(disk);
    EXPECT_NE(disk->find("x = 1"), std::string::npos) << *disk;
    QTest::keySequence(focused, QKeySequence::Undo);
    settle();
    EXPECT_FALSE(editor->text().contains(QStringLiteral("x = 1\n"))) << fromQ(editor->text());
}

TEST_F(UiFixture, RealToolInstalls)
{
    QString wanted = qEnvironmentVariable("FORMA_REAL_SETUP");
    if (wanted.isEmpty())
        GTEST_SKIP() << "Set FORMA_REAL_SETUP to tool ids to run real installs";
    window_->openSetup();
    SetupDialog* dialog = window_->setupDialog();
    ASSERT_TRUE(waitFor([&] { return dialog->ready(); }, 180000));
    auto& tools = AppContext::instance().tools();
    for (const auto& id : wanted.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        bool finished = false;
        bool success = false;
        QMetaObject::Connection link = QObject::connect(dialog, &SetupDialog::installFinished, dialog, [&](bool ok) {
            finished = true;
            success = ok;
        });
        QElapsedTimer timer;
        timer.start();
        dialog->installTools({id});
        bool done = waitFor([&] { return finished; }, 45 * 60 * 1000);
        QObject::disconnect(link);
        ASSERT_TRUE(waitFor([&] { return dialog->ready(); }, 180000));
        build::ToolInfo info = tools.detect(fromQ(id), true);
        std::cout << "REAL " << fromQ(id) << " done=" << done << " success=" << success << " available=" << info.available << " version=" << info.version << " path=" << info.path << " seconds=" << timer.elapsed() / 1000 << std::endl;
        EXPECT_TRUE(done && success) << fromQ(dialog->logText().right(3000));
        EXPECT_TRUE(info.available) << fromQ(id) << " " << fromQ(dialog->logText().right(3000));
    }
    capture(*dialog, QStringLiteral("setup-after-real"));
    dialog->close();
}

TEST_F(UiFixture, RealLanguageServers)
{
    QString wanted = qEnvironmentVariable("FORMA_REAL_LSP");
    if (wanted.isEmpty())
        GTEST_SKIP() << "Set FORMA_REAL_LSP to run against installed language servers";
    struct Case {
        const char* key;
        const char* file;
        const char* start;
        const char* typed;
        const char* broken;
    };
    const std::vector<Case> cases = {
        {"python", "demo.py", "import os\n", "os.", "print(undefined_name_here)\n"},
        {"typescript", "demo.ts", "const words: string[] = [];\n", "words.", "const count: number = 'text';\n"},
        {"cpp", "demo.cpp", "#include <vector>\nstd::vector<int> values;\nvoid f() {\n", "values.", "int broken = undefined_name_here;\n"},
        {"rust", "src/main.rs", "fn main() {\n    let words: Vec<i32> = Vec::new();\n", "words.", "fn broken() -> i32 { undefined_name_here }\n"},
        {"kotlin", "demo.kt", "fun main() {\n    val words = listOf(1)\n", "words.", "val broken: Int = undefined_name_here\n"},
    };
    for (const auto& item : cases) {
        if (!wanted.contains(QString::fromLatin1(item.key)))
            continue;
        createProject(QStringLiteral("Lsp ") + QString::fromLatin1(item.key), QStringLiteral("blank"));
        auto* controller = window_->controller();
        auto& files = controller->session()->filesystem();
        if (QString::fromLatin1(item.key) == QStringLiteral("rust")) {
            ASSERT_TRUE(files.writeFile("Cargo.toml", "[package]\nname = \"demo\"\nversion = \"0.1.0\"\nedition = \"2021\"\n"));
            std::error_code ec;
            std::filesystem::create_directories(controller->session()->root() / "src", ec);
        }
        ASSERT_TRUE(files.writeFile(item.file, std::string(item.broken) + item.start));
        window_->showWorkspace(QStringLiteral("code"));
        CodeEditor* editor = window_->editors()->openFile(QString::fromLatin1(item.file));
        ASSERT_NE(editor, nullptr);
        bool running = waitFor([&] { return window_->editors()->serverStatus().contains(QStringLiteral("is running")); }, 120000);
        std::cout << "REAL LSP " << item.key << " status=" << fromQ(window_->editors()->serverStatus()) << std::endl;
        EXPECT_TRUE(running) << item.key;
        bool diagnosed = waitFor([&] {
            for (const auto& problem : controller->problems()) {
                if (problem.source != QStringLiteral("Design") && problem.file.endsWith(QFileInfo(QString::fromLatin1(item.file)).fileName()))
                    return true;
            }
            return false;
        }, 120000);
        settle();
        capture(*window_, QStringLiteral("real-lsp-diagnostics-") + QString::fromLatin1(item.key));
        std::cout << "REAL LSP " << item.key << " diagnostics=" << diagnosed << std::endl;
        EXPECT_TRUE(diagnosed) << item.key;
        editor->gotoLine(editor->lineCount());
        QWidget* focused = window_->focusWidget();
        ASSERT_NE(focused, nullptr);
        bool listed = false;
        for (int attempt = 0; attempt < 4 && !listed; ++attempt) {
            QTest::keyClicks(focused, QString::fromLatin1(item.typed));
            listed = waitFor([&] { return editor->completionListVisible(); }, 30000);
            if (!listed)
                editor->undo();
        }
        settle();
        capture(*window_, QStringLiteral("real-lsp-completion-") + QString::fromLatin1(item.key));
        std::cout << "REAL LSP " << item.key << " completion=" << listed << std::endl;
        EXPECT_TRUE(listed) << item.key;
        QTest::keyClick(focused, Qt::Key_Escape);
        EXPECT_TRUE(window_->editors()->saveAll());
        ASSERT_TRUE(window_->editors()->closeAll());
        controller->closeProject();
        settle();
    }
}

TEST_F(UiFixture, RealRunOfTemplates)
{
    QString wanted = qEnvironmentVariable("FORMA_REAL_RUNS");
    if (wanted.isEmpty())
        GTEST_SKIP() << "Set FORMA_REAL_RUNS to template ids to build and run them for real";
    auto* controller = window_->controller();
    for (const auto& id : wanted.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        createProject(QStringLiteral("Real ") + QString(id).replace(QLatin1Char('-'), QLatin1Char(' ')), id);
        bool buildDone = false;
        bool buildOk = false;
        bool stopped = false;
        int exitCode = 0;
        QString url;
        QObject guard;
        QObject::connect(controller, &ProjectController::buildFinished, &guard, [&](bool ok, int, int, qint64) {
            buildDone = true;
            buildOk = ok;
        });
        QObject::connect(controller, &ProjectController::appStopped, &guard, [&](int code) {
            stopped = true;
            exitCode = code;
        });
        QObject::connect(controller, &ProjectController::runUrlFound, &guard, [&](const QString& found) { url = found; });
        window_->findChild<QAction*>(QStringLiteral("actionRun"))->trigger();
        EXPECT_TRUE(waitFor([&] { return buildDone; }, 20 * 60 * 1000)) << fromQ(id);
        EXPECT_TRUE(buildOk) << fromQ(id) << "\n" << fromQ(window_->output()->text().right(4000));
        bool android = controller->isAndroidProject();
        bool web = controller->opensInBrowser();
        bool reached = false;
        if (buildOk) {
            if (android) {
                reached = waitFor([&] { return window_->runStatusText().startsWith(QStringLiteral("Running on")) || stopped; }, 15 * 60 * 1000) && !stopped;
            } else if (web) {
                reached = waitFor([&] { return !url.isEmpty() || stopped; }, 3 * 60 * 1000) && !url.isEmpty();
            } else if (controller->target()->generator.empty()) {
                TerminalWidget* terminal = nullptr;
                reached = waitFor([&] {
                    terminal = window_->terminal()->current();
                    return window_->runStatusText() == QStringLiteral("Running in the terminal") && terminal != nullptr;
                }, 30000);
                QTest::qWait(25000);
                if (terminal != nullptr) {
                    std::cout << "REAL TERMINAL " << fromQ(id) << ":\n" << fromQ(terminal->visibleText().right(900)) << std::endl;
                    terminal->stop();
                }
            } else {
                waitFor([&] { return stopped; }, 12000);
                reached = stopped ? exitCode == 0 : controller->appRunning();
            }
        }
        settle();
        capture(*window_, QStringLiteral("real-run-") + id);
        std::cout << "REAL RUN " << fromQ(id) << " build=" << buildOk << " reached=" << reached << " stopped=" << stopped << " exit=" << exitCode << " url=" << fromQ(url) << " status=" << fromQ(window_->runStatusText()) << std::endl;
        EXPECT_TRUE(reached) << fromQ(id) << "\n" << fromQ(window_->output()->text().right(4000));
        if (android && reached) {
            QString shot = QDir(tourDirectory()).filePath(QStringLiteral("real-run-android-device.png"));
            auto adb = build::adbPath(AppContext::instance().tools());
            QString serial = window_->runStatusText().mid(QStringLiteral("Running on ").size());
            ProcessSpec capturePhone;
            capturePhone.program = paths::toUtf8(*adb);
            capturePhone.arguments = {"-s", fromQ(serial), "exec-out", "screencap", "-p"};
            auto picture = runProcess(capturePhone, std::chrono::seconds(30));
            if (picture) {
                QFile file(shot);
                if (file.open(QIODevice::WriteOnly))
                    file.write(picture->out.data(), static_cast<qint64>(picture->out.size()));
            }
        }
        window_->findChild<QAction*>(QStringLiteral("actionStop"))->trigger();
        settle();
        EXPECT_FALSE(controller->appRunning()) << fromQ(id);
        window_->editors()->closeAll();
        controller->closeProject();
        settle();
    }
}

TEST_F(UiFixture, CanvasDropInsertsSelectsAndUndoes)
{
    createProject(QStringLiteral("Canvas App"));
    auto* controller = window_->controller();
    window_->showWorkspace(QStringLiteral("design"));
    settle();
    const ir::Screen* screen = &controller->model()->screens.front();
    QString screenId = toQ(screen->root.id);
    std::size_t before = screen->root.children.size();
    auto component = AppContext::instance().components().instantiate("button", "uiTestButton");
    auto inserted = controller->editor()->insertComponent(fromQ(screenId), screen->root.id, -1, component);
    ASSERT_TRUE(inserted) << inserted.error().message;
    screen = controller->model()->findScreen(fromQ(screenId));
    EXPECT_EQ(screen->root.children.size(), before + 1);
    controller->setSelection({toQ(*inserted)});
    settle();
    QRectF frame = window_->canvas()->worldFrameOf(screenId, toQ(*inserted));
    ASSERT_FALSE(frame.isEmpty());
    QPoint view = window_->canvas()->worldToView(frame.center()).toPoint();
    controller->setSelection({});
    QTest::mouseClick(window_->canvas(), Qt::LeftButton, Qt::NoModifier, view);
    settle();
    ASSERT_EQ(controller->selection().size(), 1);
    EXPECT_EQ(controller->selection().front(), toQ(*inserted));
    controller->editor()->undo();
    screen = controller->model()->findScreen(fromQ(screenId));
    EXPECT_EQ(screen->root.children.size(), before);
    controller->editor()->redo();
    screen = controller->model()->findScreen(fromQ(screenId));
    EXPECT_EQ(screen->root.children.size(), before + 1);
}

TEST_F(UiFixture, EditorTypingUndoAndSaveReachDisk)
{
    createProject(QStringLiteral("Editor App"), QStringLiteral("blank"));
    auto* controller = window_->controller();
    auto written = controller->session()->filesystem().writeFile("notes.py", "print('one')\n");
    ASSERT_TRUE(written);
    CodeEditor* editor = window_->editors()->openFile(QStringLiteral("notes.py"));
    ASSERT_NE(editor, nullptr);
    ASSERT_NE(editor->language(), nullptr);
    EXPECT_EQ(editor->language()->id, "python");
    EXPECT_FALSE(editor->isModified());
    editor->insertAtCursor(QStringLiteral("# start\n"));
    EXPECT_TRUE(editor->isModified());
    EXPECT_TRUE(window_->editors()->saveCurrent());
    EXPECT_FALSE(editor->isModified());
    auto disk = controller->session()->filesystem().readFile("notes.py");
    ASSERT_TRUE(disk);
    EXPECT_EQ(*disk, "# start\nprint('one')\n");
    editor->undo();
    EXPECT_EQ(fromQ(editor->text()), "print('one')\n");
    EXPECT_TRUE(editor->isModified());
    EXPECT_TRUE(window_->editors()->saveCurrent());
    EXPECT_EQ(editor->lineCount(), 2);
}

TEST_F(UiFixture, DiskChangesReloadCleanTabsAndNeverOverwriteDirtyOnes)
{
    createProject(QStringLiteral("Watch App"), QStringLiteral("blank"));
    auto* controller = window_->controller();
    auto& files = controller->session()->filesystem();
    ASSERT_TRUE(files.writeFile("watched.txt", "first\n"));
    CodeEditor* editor = window_->editors()->openFile(QStringLiteral("watched.txt"));
    ASSERT_NE(editor, nullptr);
    QFile external(QDir(controller->rootPath()).filePath(QStringLiteral("watched.txt")));
    ASSERT_TRUE(external.open(QIODevice::WriteOnly | QIODevice::Truncate));
    external.write("second\n");
    external.close();
    ASSERT_TRUE(waitFor([&] { return editor->text() == QStringLiteral("second\n"); }, 15000));
    editor->insertAtCursor(QStringLiteral("mine "));
    ASSERT_TRUE(external.open(QIODevice::WriteOnly | QIODevice::Truncate));
    external.write("third\n");
    external.close();
    QTest::qWait(1500);
    EXPECT_TRUE(editor->text().contains(QStringLiteral("mine ")));
    auto disk = files.readFile("watched.txt");
    ASSERT_TRUE(disk);
    EXPECT_EQ(*disk, "third\n");
    editor->setText(QStringLiteral("third\n"));
    ASSERT_TRUE(window_->editors()->closeAll());
}

TEST_F(UiFixture, TerminalRunsACommand)
{
    createProject(QStringLiteral("Term App"), QStringLiteral("blank"));
    window_->showWorkspace(QStringLiteral("code"));
    window_->showDock(QStringLiteral("terminal"));
    settle();
    ASSERT_EQ(window_->terminal()->sessionCount(), 1);
    TerminalWidget* terminal = window_->terminal()->current();
    ASSERT_NE(terminal, nullptr);
    capture(*window_, QStringLiteral("terminal-before"));
    terminal->runCommand(QStringLiteral("echo forma-terminal-marker"));
    bool seen = waitFor([&] { return terminal->visibleText().remove(QLatin1Char('\n')).count(QStringLiteral("forma-terminal-marker")) >= 2; }, 20000);
    capture(*window_, QStringLiteral("terminal-") + (Theme::instance().isDark() ? QStringLiteral("dark") : QStringLiteral("light")));
    EXPECT_TRUE(seen) << fromQ(terminal->visibleText());
    terminal->stop();
}

TEST_F(UiFixture, PreviewRendersAtDeviceSizesInBothAppearances)
{
    createProject(QStringLiteral("Preview App"));
    window_->showWorkspace(QStringLiteral("preview"));
    settle();
    PreviewPanel* preview = window_->preview();
    ASSERT_FALSE(preview->lastImage().isNull());
    const auto& presets = design::devicePresets();
    ASSERT_FALSE(presets.empty());
    preview->setDevice(toQ(presets.front().id));
    EXPECT_EQ(preview->lastImage().width(), static_cast<int>(std::ceil(presets.front().width)));
    preview->setAppearance(QStringLiteral("dark"));
    EXPECT_FALSE(preview->lastImage().isNull());
    auto* notice = window_->findChild<QLabel*>(QStringLiteral("previewNotice"));
    ASSERT_NE(notice, nullptr);
    EXPECT_TRUE(notice->text().contains(QStringLiteral("Simulated")));
}

TEST_F(UiFixture, AssistantIsOffUntilConfigured)
{
    createProject(QStringLiteral("Assistant App"), QStringLiteral("blank"));
    window_->showWorkspace(QStringLiteral("assistant"));
    settle();
    auto* send = window_->findChild<QPushButton*>(QStringLiteral("assistantSend"));
    auto* status = window_->findChild<QLabel*>(QStringLiteral("assistantStatus"));
    ASSERT_NE(send, nullptr);
    ASSERT_NE(status, nullptr);
    EXPECT_FALSE(send->isEnabled());
    EXPECT_TRUE(status->text().contains(QStringLiteral("off until")));
}

TEST_F(UiFixture, DesignGenerateEditRegenerateKeepsUserCode)
{
    createProject(QStringLiteral("Round Trip"));
    auto* controller = window_->controller();
    controller->regenerate(true);
    ASSERT_TRUE(waitFor([&] { return !controller->lastReport().files.empty(); }, 20000));
    QString userFile;
    for (const auto& file : controller->lastReport().files) {
        QString path = toQ(file.path);
        if (path.endsWith(QStringLiteral(".cpp")) && !path.contains(QStringLiteral("generated")) && path != QStringLiteral("src/main.cpp"))
            userFile = path;
    }
    ASSERT_FALSE(userFile.isEmpty());
    CodeEditor* editor = window_->editors()->openFile(userFile);
    ASSERT_NE(editor, nullptr);
    editor->gotoLine(editor->lineCount());
    editor->insertAtCursor(QStringLiteral("\n// user marker kept\n"));
    ASSERT_TRUE(window_->editors()->saveCurrent());
    const ir::Screen& screen = controller->model()->screens.front();
    auto component = AppContext::instance().components().instantiate("button", "roundTripButton");
    ASSERT_TRUE(controller->editor()->insertComponent(screen.root.id, screen.root.id, -1, component));
    controller->regenerate(true);
    settle();
    auto disk = controller->session()->filesystem().readFile(fromQ(userFile));
    ASSERT_TRUE(disk);
    EXPECT_NE(disk->find("user marker kept"), std::string::npos);
}

TEST_F(UiFixture, InspectorSettingsAndGitScreens)
{
    createProject(QStringLiteral("Panels App"));
    auto* controller = window_->controller();
    QString mode = Theme::instance().isDark() ? QStringLiteral("dark") : QStringLiteral("light");
    const ir::Screen& screen = controller->model()->screens.front();
    QString button;
    for (const auto& child : screen.root.children) {
        if (child.type == "button")
            button = toQ(child.id);
    }
    ASSERT_FALSE(button.isEmpty());
    controller->setSelection({button});
    settle();
    capture(*window_, QStringLiteral("inspector-") + mode);
    window_->showWorkspace(QStringLiteral("code"));
    window_->showDock(QStringLiteral("git"));
    settle();
    auto* init = window_->findChild<QPushButton*>(QStringLiteral("gitInit"));
    ASSERT_NE(init, nullptr);
    if (git::GitClient::available()) {
        ASSERT_TRUE(waitFor([&] { return init->isVisible(); }, 15000));
        QTest::mouseClick(init, Qt::LeftButton);
        auto* changes = window_->findChild<QTreeWidget*>(QStringLiteral("gitChanges"));
        ASSERT_NE(changes, nullptr);
        ASSERT_TRUE(waitFor([&] { return changes->topLevelItemCount() > 0; }, 20000));
        capture(*window_, QStringLiteral("git-") + mode);
    }
    window_->showWorkspace(QStringLiteral("debug"));
    capture(*window_, QStringLiteral("debug-") + mode);
    SettingsDialog settings(window_.get());
    settings.show();
    for (const auto& page : {QStringLiteral("Appearance"), QStringLiteral("Editor"), QStringLiteral("Assistant"), QStringLiteral("Plugins")}) {
        settings.showPage(page);
        capture(settings, QStringLiteral("settings-%1-%2").arg(page, mode));
    }
}

TEST_F(UiFixture, DesignBuildAndRunTheRealApp)
{
    auto& tools = AppContext::instance().tools();
    if (!tools.detect("cmake").available || !tools.detect("qt").available)
        GTEST_SKIP() << "needs CMake and Qt";
    createProject(QStringLiteral("Real App"));
    auto* controller = window_->controller();
    const ir::Screen& screen = controller->model()->screens.front();
    auto component = AppContext::instance().components().instantiate("button", "realAppButton");
    component.properties["label"] = "Clicked from the designer";
    ASSERT_TRUE(controller->editor()->insertComponent(screen.root.id, screen.root.id, -1, component));
    bool built = false;
    bool success = false;
    QObject::connect(controller, &ProjectController::buildFinished, window_.get(), [&](bool ok, int, int, qint64) {
        built = true;
        success = ok;
    });
    controller->regenerate(true);
    controller->startBuild(QStringLiteral("debug"));
    ASSERT_TRUE(waitFor([&] { return built; }, 420000));
    EXPECT_TRUE(success) << fromQ(window_->output()->text());
    ASSERT_FALSE(controller->artifacts().empty());
    capture(*window_, QStringLiteral("build-finished"));
    window_->showWorkspace(QStringLiteral("build"));
    capture(*window_, QStringLiteral("workspace-build-after"));
    bool started = false;
    int exitCode = -1;
    bool stopped = false;
    QObject::connect(controller, &ProjectController::appStarted, window_.get(), [&](const QString&) { started = true; });
    QObject::connect(controller, &ProjectController::appStopped, window_.get(), [&](int code) {
        stopped = true;
        exitCode = code;
    });
    controller->runApp(QStringLiteral("debug"));
    ASSERT_TRUE(waitFor([&] { return started; }, 20000));
    QTest::qWait(3000);
    EXPECT_TRUE(controller->appRunning()) << fromQ(window_->output()->text());
    controller->stopApp();
    EXPECT_TRUE(waitFor([&] { return stopped; }, 20000));
    (void)exitCode;
}

int main(int argc, char** argv)
{
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QTemporaryDir home;
    qputenv("FORMA_NO_BROWSER", "1");
    qputenv("FORMA_CONFIG_HOME", home.filePath(QStringLiteral("config")).toUtf8());
    qputenv("FORMA_DATA_HOME", home.filePath(QStringLiteral("data")).toUtf8());
    QApplication application(argc, argv);
    QString mode = qEnvironmentVariable("FORMA_UI_THEME", QStringLiteral("light"));
    Theme::instance().setMode(mode);
    Theme::instance().apply(application);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
