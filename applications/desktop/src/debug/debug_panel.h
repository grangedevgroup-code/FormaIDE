#pragma once

#include <QTreeWidget>
#include <QWidget>
#include <memory>
#include <set>

#include "app/project_controller.h"
#include "code/editor_area.h"
#include "forma/debugger/debug_client.h"

class QComboBox;
class QLineEdit;
class QLabel;
class QToolButton;
class QPlainTextEdit;

namespace forma::ui {

class DebugPanel : public QWidget {
    Q_OBJECT

public:
    DebugPanel(ProjectController* controller, EditorArea* editors, QWidget* parent = nullptr);
    ~DebugPanel() override;

    void start();
    void stop();
    bool active() const;
    QString stateText() const;
    void refreshAdapters();

Q_SIGNALS:
    void stateChanged(const QString& text);

private:
    void onStopped(const debugger::StopEvent& event);
    void loadFrame(int frameId);
    void updateButtons();
    void addWatch();
    void refreshWatches();
    void appendOutput(const QString& text, bool isError = false);
    std::string defaultAdapterFor(const QString& path) const;

    ProjectController* controller_;
    EditorArea* editors_;
    std::unique_ptr<debugger::DebugSession> session_;
    QComboBox* adapter_;
    QLineEdit* program_;
    QLabel* state_;
    QToolButton* startButton_;
    QToolButton* stopButton_;
    QToolButton* continueButton_;
    QToolButton* pauseButton_;
    QToolButton* overButton_;
    QToolButton* intoButton_;
    QToolButton* outButton_;
    QTreeWidget* stack_;
    QTreeWidget* variables_;
    QTreeWidget* watches_;
    QLineEdit* watchInput_;
    QPlainTextEdit* console_;
    int threadId_ = 0;
    int frameId_ = 0;
};

}
