#pragma once

#include <QDialog>
#include <QWidget>

#include "app/project_controller.h"
#include "forma/ai/provider.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTreeWidget;

namespace forma::ui {

class WelcomePage : public QWidget {
    Q_OBJECT

public:
    explicit WelcomePage(QWidget* parent = nullptr);
    void refresh();
    void setToolsSummary(int ready, int total);

Q_SIGNALS:
    void newRequested();
    void templateRequested(const QString& id);
    void toolsRequested();
    void settingsRequested();
    void openRequested();
    void importRequested();
    void recentRequested(const QString& path);

private:
    QListWidget* recents_;
    QLabel* emptyRecents_;
    QLabel* toolsSummary_;
};

class NewProjectDialog : public QDialog {
    Q_OBJECT

public:
    explicit NewProjectDialog(QWidget* parent = nullptr);
    project::NewProjectOptions options() const;
    void selectTemplate(const QString& id);
    void setProjectName(const QString& name);
    void setLocation(const QString& path);
    QStringList toolsToInstall() const;

protected:
    void accept() override;

private:
    void updateDetails();
    QTreeWidget* templates_;
    QLabel* description_;
    QLabel* tools_;
    QLineEdit* name_;
    QLineEdit* location_;
    QLineEdit* bundle_;
    QLabel* fullPath_;
    QCheckBox* git_;
    QCheckBox* installMissing_;
    QStringList missingTools_;
    QPushButton* create_;
    bool bundleEdited_ = false;
};

class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget* parent = nullptr);
    void showPage(const QString& name);

Q_SIGNALS:
    void settingsApplied();

private:
    QWidget* buildAppearancePage();
    QWidget* buildEditorPage();
    QWidget* buildAssistantPage();
    QWidget* buildPluginsPage();
    void reloadProviderList();
    void loadProvider(int row);
    void saveProviders();
    void reloadPlugins();

    QListWidget* pages_;
    QStackedWidget* stack_;
    QComboBox* themeMode_;
    QSpinBox* fontSize_;
    QCheckBox* minimap_;
    QCheckBox* languageServers_;
    QComboBox* shell_;
    QListWidget* providers_;
    QLineEdit* providerLabel_;
    QLineEdit* providerUrl_;
    QLineEdit* providerModel_;
    QLineEdit* providerKey_;
    QLabel* providerStatus_;
    QLabel* providerNote_;
    std::vector<ai::ProviderConfig> providerList_;
    int providerRow_ = -1;
    QTreeWidget* plugins_;
};

}
