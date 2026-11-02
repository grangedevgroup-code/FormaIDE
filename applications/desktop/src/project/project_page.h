#pragma once

#include <QListWidget>
#include <QTreeWidget>
#include <QWidget>
#include <memory>

#include "app/project_controller.h"
#include "forma/assets/asset_manager.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace forma::ui {

class QtImageCodec final : public assets::IImageCodec {
public:
    Result<assets::ImageInfo> inspect(std::string_view data) const override;
    Result<std::string> convert(std::string_view data, std::string_view format, int width, int height, int quality) const override;
};

class AssetsPanel : public QWidget {
    Q_OBJECT

public:
    explicit AssetsPanel(ProjectController* controller, QWidget* parent = nullptr);
    void refresh();
    Status importFile(const QString& path);
    int assetCount() const;

private:
    void showMenu(const QPoint& position);
    assets::AssetManager manager() const;

    ProjectController* controller_;
    QtImageCodec codec_;
    QLineEdit* search_;
    QListWidget* list_;
    QLabel* details_;
};

class HistoryPanel : public QWidget {
    Q_OBJECT

public:
    explicit HistoryPanel(ProjectController* controller, QWidget* parent = nullptr);
    void refresh();

private:
    void restoreSelected();
    void snapshotNow();
    ProjectController* controller_;
    QComboBox* kind_;
    QTreeWidget* entries_;
    QTreeWidget* snapshots_;
};

class ProjectSettingsPanel : public QWidget {
    Q_OBJECT

public:
    explicit ProjectSettingsPanel(ProjectController* controller, QWidget* parent = nullptr);
    void refresh();

private:
    void apply();
    ProjectController* controller_;
    QLineEdit* name_;
    QLineEdit* bundle_;
    QLineEdit* version_;
    QLabel* location_;
    QTreeWidget* targets_;
    QPushButton* save_;
};

class ProjectPage : public QWidget {
    Q_OBJECT

public:
    explicit ProjectPage(ProjectController* controller, QWidget* parent = nullptr);
    AssetsPanel* assets() const { return assets_; }
    HistoryPanel* history() const { return history_; }

private:
    AssetsPanel* assets_;
    HistoryPanel* history_;
    ProjectSettingsPanel* settings_;
};

}
