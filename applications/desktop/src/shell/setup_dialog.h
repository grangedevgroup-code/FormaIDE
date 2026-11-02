#pragma once

#include <QDialog>
#include <QStringList>
#include <deque>
#include <memory>
#include <optional>

#include "forma/build/setup.h"

class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace forma::ui {

class SetupDialog : public QDialog {
    Q_OBJECT

public:
    explicit SetupDialog(QWidget* parent = nullptr);
    ~SetupDialog() override;

    void focusTools(const QStringList& tools, const QString& reason = QString());
    void focusServer(const QString& languageName, const QString& serverCommand, const QString& hint);
    void installTools(const QStringList& tools);
    void installPlan(build::InstallPlan plan);
    bool busy() const;
    QString logText() const;
    int readyStacks() const { return readyStacks_; }
    int totalStacks() const { return totalStacks_; }
    bool ready() const { return ready_; }

Q_SIGNALS:
    void toolsChanged();
    void refreshed();
    void installFinished(bool success);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void refresh();
    void populate();
    void describeCurrent();
    void startNext();
    void log(const QString& text, bool isError = false);
    QStringList missingIn(QTreeWidgetItem* item) const;

    QTreeWidget* tree_;
    QLabel* heading_;
    QLabel* detail_;
    QPlainTextEdit* commands_;
    QPlainTextEdit* log_;
    QProgressBar* progress_;
    QPushButton* install_;
    QPushButton* cancel_;
    QPushButton* refresh_;
    std::unique_ptr<build::SetupJob> job_;
    std::deque<build::InstallPlan> queue_;
    std::optional<build::InstallPlan> pendingPlan_;
    QString current_;
    bool anyFailed_ = false;
    bool ready_ = false;
    QString manualUrl_;
    QStringList pendingTools_;
    QString pendingReason_;
    int readyStacks_ = 0;
    int totalStacks_ = 0;
};

}
