#pragma once

#include <QFileSystemModel>
#include <QLineEdit>
#include <QSortFilterProxyModel>
#include <QTabWidget>
#include <QTimer>
#include <QTreeView>
#include <QTreeWidget>
#include <QWidget>
#include <atomic>
#include <map>
#include <memory>
#include <set>

#include "app/project_controller.h"
#include "code/code_editor.h"

class QCheckBox;
class QFrame;
class QLabel;
class QPushButton;

namespace forma::ui {

class EditorArea : public QWidget {
    Q_OBJECT

public:
    explicit EditorArea(ProjectController* controller, QWidget* parent = nullptr);
    ~EditorArea() override;

    CodeEditor* openFile(const QString& relativePath, int line = 0, int column = 0);
    CodeEditor* currentEditor() const;
    QString currentPath() const;
    QString currentAbsolutePath() const;
    bool saveCurrent();
    bool saveAll();
    void closeCurrent();
    bool closeAll();
    void reloadIfUnmodified(const QString& relativePath);
    void goToDefinition();
    void findReferences();
    void renameSymbol();
    void formatDocument();
    void runCurrentFile();
    int openCount() const;
    QStringList openPaths() const;
    QString serverStatus() const { return serverStatus_; }
    bool restoreBuffer(const QString& path, const QString& content);
    std::map<QString, std::set<int>> allBreakpoints() const { return breakpoints_; }
    void setExecutionLocation(const QString& path, int line);
    void clearExecutionLocation();
    void gotoComponentAtCursor();
    void retryLanguageServers();
    QString serverNotice() const;

Q_SIGNALS:
    void cursorChanged(int line, int column, const QString& language);
    void currentFileChanged(const QString& path);
    void breakpointsChanged(const QString& path, const std::set<int>& lines);
    void referencesFound(const QString& title, const std::vector<lsp::Location>& locations);
    void serverStatusChanged(const QString& text);
    void automationRequested(const QString& command);
    void componentFocusRequested(const QString& screen, const QString& component);
    void serverInstallRequested(const QString& language, const QString& server, const QString& hint);

private:
    struct Tab {
        CodeEditor* editor = nullptr;
        QString path;
        std::string baseHash;
        bool externalChange = false;
        QTimer* recoveryTimer = nullptr;
        QTimer* syncTimer = nullptr;
    };

    Tab* tabFor(CodeEditor* editor);
    Tab* currentTab();
    int indexOfPath(const QString& path) const;
    bool closeTab(int index);
    void updateTabTitle(Tab& tab);
    void attachLanguageServer(Tab& tab);
    lsp::LanguageClient* clientFor(const editor::LanguageDefinition* language);
    bool saveTab(Tab& tab);
    void stopServers();
    void updateServerNotice();

    ProjectController* controller_;
    QTabWidget* tabs_;
    QWidget* empty_;
    QFrame* notice_;
    QLabel* noticeText_;
    QPushButton* noticeInstall_;
    QString noticeLanguageName_;
    QString noticeServer_;
    QString noticeHint_;
    std::set<std::string> dismissedNotices_;
    std::vector<std::unique_ptr<Tab>> tabList_;
    std::map<std::string, std::unique_ptr<lsp::LanguageClient>> clients_;
    std::map<QString, std::vector<lsp::Diagnostic>> diagnostics_;
    std::map<QString, std::set<int>> breakpoints_;
    QString serverStatus_;
    QString executionPath_;
};

class FileExplorer : public QWidget {
    Q_OBJECT

public:
    explicit FileExplorer(ProjectController* controller, QWidget* parent = nullptr);
    void setRoot(const QString& path);
    QTreeView* view() const { return view_; }

private:
    void showMenu(const QPoint& position);
    void newFile(const QString& directory);
    void newFolder(const QString& directory);

    ProjectController* controller_;
    QFileSystemModel* model_;
    QSortFilterProxyModel* proxy_;
    QTreeView* view_;
    QLineEdit* filter_;
};

class SearchPanel : public QWidget {
    Q_OBJECT

public:
    explicit SearchPanel(ProjectController* controller, QWidget* parent = nullptr);
    void focusSearch();
    void showLocations(const QString& title, const std::vector<lsp::Location>& locations);

private:
    void runSearch();
    ProjectController* controller_;
    QLineEdit* query_;
    QCheckBox* matchCase_;
    QCheckBox* regex_;
    QTreeWidget* results_;
    QLabel* status_;
    std::shared_ptr<std::atomic<bool>> cancel_;
};

}

Q_DECLARE_METATYPE(std::set<int>)
