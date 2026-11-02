#pragma once

#include <QNetworkAccessManager>
#include <QPlainTextEdit>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QWidget>
#include <memory>

#include "app/project_controller.h"
#include "code/editor_area.h"
#include "forma/ai/agent.h"
#include "git/git_panel.h"

class QComboBox;
class QLabel;
class QPushButton;
class QCheckBox;

namespace forma::ui {

class QtHttpTransport final : public ai::IHttpTransport {
public:
    std::shared_ptr<ai::IHttpCall> send(const ai::HttpRequest& request, std::function<void(std::string_view)> onChunk, std::function<void(int, const std::string&)> onDone) override;

private:
    QNetworkAccessManager manager_;
};

class AssistantPanel : public QWidget {
    Q_OBJECT

public:
    AssistantPanel(ProjectController* controller, EditorArea* editors, QWidget* parent = nullptr);

    void setTransport(std::shared_ptr<ai::IHttpTransport> transport) { transport_ = std::move(transport); }
    void reloadProviders();
    void ask(const QString& prompt);
    void askToFixBuild();
    QString transcript() const;
    int stagedCount() const;
    void applyApproved();
    ai::AgentSession* session() const { return session_.get(); }

Q_SIGNALS:
    void setupRequested();

private:
    void ensureSession();
    void addMessage(const QString& who, const QString& text);
    void appendStreaming(const QString& text);
    void refreshContext();
    void showStaged();
    void setBusy(bool busy);
    ai::ProviderConfig currentConfig() const;

    ProjectController* controller_;
    EditorArea* editors_;
    std::shared_ptr<ai::IHttpTransport> transport_;
    std::shared_ptr<ai::AgentSession> session_;
    QComboBox* provider_;
    QComboBox* autonomy_;
    QLabel* status_;
    QPushButton* setup_;
    QTextBrowser* conversation_;
    QPlainTextEdit* input_;
    QPushButton* send_;
    QPushButton* stop_;
    QTreeWidget* context_;
    QTreeWidget* changes_;
    DiffView* diff_;
    QWidget* changesBox_;
    QPushButton* apply_;
    QPushButton* rollback_;
    QString lastCheckpoint_;
    QString streaming_;
    bool streamingOpen_ = false;
};

}
