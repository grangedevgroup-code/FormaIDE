#pragma once

#include <QPlainTextEdit>
#include <QTableWidget>
#include <QTreeWidget>
#include <QWidget>

#include "app/project_controller.h"

class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QToolButton;
class QCheckBox;

namespace forma::ui {

class OutputPanel : public QWidget {
    Q_OBJECT

public:
    explicit OutputPanel(ProjectController* controller, QWidget* parent = nullptr);
    QString text() const;
    void appendLine(const QString& line, bool isError = false);
    void clear();

private:
    QPlainTextEdit* output_;
};

class ProblemsPanel : public QWidget {
    Q_OBJECT

public:
    explicit ProblemsPanel(ProjectController* controller, QWidget* parent = nullptr);
    int rowCount() const;

Q_SIGNALS:
    void countsChanged(int errors, int warnings);

private:
    void refresh();
    ProjectController* controller_;
    QTreeWidget* table_;
    QCheckBox* showWarnings_;
};

class BuildPanel : public QWidget {
    Q_OBJECT

public:
    explicit BuildPanel(ProjectController* controller, QWidget* parent = nullptr);
    QString configuration() const;
    void refreshToolchains(bool force = false);

Q_SIGNALS:
    void runRequested();

private:
    void refreshConfigurations();
    void refreshArtifacts();
    ProjectController* controller_;
    QComboBox* configuration_;
    QLabel* status_;
    QProgressBar* progress_;
    QPushButton* buildButton_;
    QPushButton* cancelButton_;
    QTreeWidget* artifacts_;
    QTreeWidget* toolchains_;
    QTreeWidget* generated_;
};

class AutomationLog : public QWidget {
    Q_OBJECT

public:
    explicit AutomationLog(ProjectController* controller, QWidget* parent = nullptr);
    void add(const QString& actor, const QString& command);

private:
    QTreeWidget* list_;
};

}
