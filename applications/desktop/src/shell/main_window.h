#pragma once

#include <QButtonGroup>
#include <QDockWidget>
#include <QMainWindow>
#include <QPointer>
#include <map>

#include "ai/assistant_panel.h"
#include "app/project_controller.h"
#include "build/build_panels.h"
#include "canvas/canvas_widget.h"
#include "code/editor_area.h"
#include "debug/debug_panel.h"
#include "design/design_panels.h"
#include "git/git_panel.h"
#include "preview/preview_panel.h"
#include "project/project_page.h"
#include "shell/dialogs.h"
#include "shell/setup_dialog.h"
#include "terminal/terminal_widget.h"

class QComboBox;
class QLabel;
class QStackedWidget;
class QToolButton;

namespace forma::ui {

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    ProjectController* controller() const { return controller_; }
    CanvasWidget* canvas() const { return canvas_; }
    EditorArea* editors() const { return editors_; }
    TerminalPanel* terminal() const { return terminal_; }
    OutputPanel* output() const { return output_; }
    ProblemsPanel* problems() const { return problems_; }
    BuildPanel* buildPanel() const { return build_; }
    AssistantPanel* assistant() const { return assistant_; }
    PreviewPanel* preview() const { return preview_; }
    ProjectPage* projectPage() const { return projectPage_; }
    GitPanel* git() const { return git_; }
    DebugPanel* debugPanel() const { return debug_; }
    WelcomePage* welcome() const { return welcome_; }
    InspectorPanel* inspector() const { return inspector_; }
    LayersPanel* layers() const { return layers_; }
    SetupDialog* setupDialog() const { return setup_; }
    void openSetup(const QStringList& tools = {}, const QString& reason = {});
    void refreshDevices();
    QString runStatusText() const;
    bool bottomPanelOpen() const;
    void toggleBottomPanel();
    void commandPalette();

    void showWorkspace(const QString& id);
    QString workspace() const { return workspace_; }
    bool openProjectFolder(const QString& path);
    void showDock(const QString& id);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    struct Workspace {
        QString id;
        QString title;
        QString iconName;
        QWidget* center = nullptr;
        QStringList docks;
        QStringList bottom;
        bool bottomOpen = false;
        QAbstractButton* button = nullptr;
    };

    QDockWidget* addDock(const QString& id, const QString& title, QWidget* widget, Qt::DockWidgetArea area);
    void buildCentral();
    void buildDocks();
    void buildToolbar();
    void buildMenus();
    void buildStatusBar();
    void wireController();
    void updateTitle();
    void updateActions();
    void updateTargets();
    void onProjectOpened();
    void onProjectClosed();
    void newProject(const QString& templateId = QString());
    void setRunStatus(const QString& text, const QString& state = QString());
    void setAssistantOpen(bool open);
    void checkToolsSummary();
    void openProject();
    void importFolder();
    void openSettings(const QString& page = {});
    void quickOpen();
    void undo();
    void redo();
    bool saveEverything();
    void saveLayout();
    void restoreLayout();
    void checkRecovery(int buffers, bool design);
    void applyEditorSettings();
    void updateDockTitles();

    ProjectController* controller_;
    QStackedWidget* pages_;
    QStackedWidget* center_;
    QWidget* mainPage_;
    WelcomePage* welcome_;
    CanvasWidget* canvas_;
    EditorArea* editors_;
    PreviewPanel* preview_;
    BuildPanel* build_;
    ProjectPage* projectPage_;
    ScreensPanel* screens_;
    LayersPanel* layers_;
    PalettePanel* palette_;
    InspectorPanel* inspector_;
    FileExplorer* files_;
    SearchPanel* search_;
    GitPanel* git_;
    DiffView* diff_;
    TerminalPanel* terminal_;
    OutputPanel* output_;
    ProblemsPanel* problems_;
    AutomationLog* automation_;
    DebugPanel* debug_;
    AssistantPanel* assistant_;
    std::map<QString, QDockWidget*> docks_;
    std::vector<Workspace> workspaces_;
    QString workspace_;
    QByteArray pendingState_;
    QButtonGroup* modeGroup_;
    QToolBar* toolbar_;
    QLabel* projectLabel_;
    QLabel* runStatus_;
    QComboBox* target_;
    QComboBox* device_;
    QComboBox* configuration_;
    QToolButton* panelButton_;
    QToolButton* assistantButton_;
    QPointer<SetupDialog> setup_;
    bool assistantOpen_ = false;
    bool diffOpen_ = false;
    bool updatingDevices_ = false;
    std::vector<QMenu*> menus_;
    std::vector<std::pair<QAction*, QString>> themedActions_;
    QAction* buildAction_;
    QAction* runAction_;
    QAction* stopAction_;
    QAction* undoAction_;
    QAction* redoAction_;
    QAction* saveAction_;
    QLabel* statusText_;
    QLabel* statusProblems_;
    QLabel* statusBranch_;
    QLabel* statusServer_;
    QLabel* statusCursor_;
    QLabel* statusLanguage_;
    QLabel* statusSaved_;
    bool updatingTargets_ = false;
};

}
