#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>

#include "ILexer.h"
#include "Lexilla.h"
#include "ScintillaEditBase.h"
#include "app/app_context.h"
#include "forma/design/design_engine.h"
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
    for (const auto& workspace : {QStringLiteral("design"), QStringLiteral("code"), QStringLiteral("project"), QStringLiteral("preview"), QStringLiteral("debug"), QStringLiteral("build"), QStringLiteral("assistant")}) {
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

int main(int argc, char** argv)
{
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QTemporaryDir home;
    qputenv("FORMA_CONFIG_HOME", home.filePath(QStringLiteral("config")).toUtf8());
    qputenv("FORMA_DATA_HOME", home.filePath(QStringLiteral("data")).toUtf8());
    QApplication application(argc, argv);
    QString mode = qEnvironmentVariable("FORMA_UI_THEME", QStringLiteral("light"));
    Theme::instance().setMode(mode);
    Theme::instance().apply(application);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
