#pragma once

#include <QPlainTextEdit>
#include <QTreeWidget>
#include <QWidget>

#include "app/project_controller.h"
#include "forma/git/git_client.h"

class QComboBox;
class QLabel;
class QPushButton;

namespace forma::ui {

class GitPanel : public QWidget {
    Q_OBJECT

public:
    explicit GitPanel(ProjectController* controller, QWidget* parent = nullptr);
    void refresh();
    QString branchText() const { return branchText_; }

Q_SIGNALS:
    void branchChanged(const QString& text);
    void diffRequested(const QString& title, const QString& diff);

private:
    void runAsync(const QString& label, std::function<Result<git::CommandResult>(git::GitClient&)> work);
    void showFileMenu(const QPoint& position);
    git::GitClient client() const;

    ProjectController* controller_;
    QComboBox* branch_;
    QLabel* summary_;
    QPushButton* initButton_;
    QWidget* repoControls_;
    QTreeWidget* changes_;
    QPlainTextEdit* message_;
    QPushButton* commitButton_;
    QTreeWidget* log_;
    QString branchText_;
    bool updating_ = false;
};

class DiffView : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit DiffView(QWidget* parent = nullptr);
    void showDiff(const QString& diff);
};

}
