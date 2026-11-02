#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <memory>
#include <set>

#include "forma/build/build_system.h"
#include "forma/codegen/pipeline.h"
#include "forma/core/file_watcher.h"
#include "forma/ir/editor.h"
#include "forma/project/project.h"

namespace forma::ui {

struct ProblemItem {
    QString source;
    int severity = 2;
    QString file;
    int line = 0;
    int column = 0;
    QString message;
    QString screen;
    QString component;
};

class ProjectController : public QObject {
    Q_OBJECT

public:
    explicit ProjectController(QObject* parent = nullptr);
    ~ProjectController() override;

    bool hasProject() const { return session_ != nullptr; }
    project::ProjectSession* session() const { return session_.get(); }
    ir::ModelEditor* editor() const { return session_ ? &session_->editor() : nullptr; }
    const ir::ApplicationModel* model() const { return session_ ? &session_->model() : nullptr; }
    const project::TargetConfig* target() const { return session_ ? session_->descriptor().active() : nullptr; }
    QString rootPath() const;

    Status createProject(const project::NewProjectOptions& options);
    Status openProject(const QString& directory);
    Status importFolder(const QString& directory);
    void closeProject();
    Status saveAll();

    QString currentScreen() const { return currentScreen_; }
    void setCurrentScreen(const QString& id);
    QStringList selection() const { return selection_; }
    void setSelection(const QStringList& ids);
    void selectComponent(const QString& screen, const QString& component);

    void regenerate(bool quiet = false);
    void reconcileCode();
    void refreshValidation();
    codegen::GenerationReport lastReport() const { return lastReport_; }
    void resolveConflict(const QString& path, codegen::ConflictChoice choice);

    void startBuild(const QString& configuration, std::function<void()> then = {});
    void cancelBuild();
    bool buildRunning() const;
    void runApp(const QString& configuration);
    void stopApp();
    bool appRunning() const;
    void buildAndRun(const QString& configuration);
    std::vector<build::Artifact> artifacts() const { return artifacts_; }

    void setBuildProblems(std::vector<ProblemItem> problems);
    void setEditorProblems(const QString& file, std::vector<ProblemItem> problems);
    std::vector<ProblemItem> problems() const;
    QString buildErrorsText() const;

    void openFile(const QString& relativePath, int line = 0, int column = 0);
    void notifyFilesChanged();

    void reportStatus(const QString& text);

Q_SIGNALS:
    void projectOpened();
    void projectClosed();
    void modelChanged(const forma::ir::ChangeEvent& event);
    void selectionChanged();
    void screenChanged();
    void filesChanged(const QStringList& paths);
    void externalFileChanged(const QString& path);
    void problemsChanged();
    void generationFinished(const forma::codegen::GenerationReport& report);
    void syncFinished(const QString& summary);
    void buildStarted();
    void buildStep(const QString& label);
    void buildOutput(const QString& line, bool isError);
    void buildFinished(bool success, int errors, int warnings, qint64 millis);
    void appStarted(const QString& command);
    void appOutput(const QString& text, bool isError);
    void appStopped(int exitCode);
    void fileOpenRequested(const QString& relativePath, int line, int column);
    void statusMessage(const QString& text);
    void automationLogged(const QString& actor, const QString& command);
    void recoveryAvailable(int buffers, bool design);

private:
    void attachSession();
    void onFilesChangedExternally(const std::vector<FileChange>& changes);
    build::BuildRequest requestFor(const QString& configuration) const;

    std::unique_ptr<project::ProjectSession> session_;
    std::unique_ptr<IFileWatcher> watcher_;
    std::unique_ptr<build::BuildJob> buildJob_;
    std::unique_ptr<Process> runningApp_;
    QTimer autosaveTimer_;
    QTimer watchDebounce_;
    QTimer validationTimer_;
    QStringList pendingChanges_;
    std::set<QString> ownWrites_;
    QString currentScreen_;
    QStringList selection_;
    codegen::GenerationReport lastReport_;
    std::vector<build::Artifact> artifacts_;
    std::vector<ProblemItem> buildProblems_;
    std::vector<ProblemItem> designProblems_;
    std::map<QString, std::vector<ProblemItem>> editorProblems_;
    int listenerId_ = 0;
};

inline QString toQ(const std::string& text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

inline std::string fromQ(const QString& text)
{
    return text.toStdString();
}

}

Q_DECLARE_METATYPE(forma::ir::ChangeEvent)
Q_DECLARE_METATYPE(forma::codegen::GenerationReport)
