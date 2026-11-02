#pragma once

#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QRectF>
#include <QScrollArea>
#include <QTreeWidget>
#include <QWidget>
#include <functional>
#include <set>

#include "app/project_controller.h"

class QFormLayout;
class QGridLayout;
class QVBoxLayout;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QStackedWidget;
class QTabBar;

namespace forma::ui {

class ScreensPanel : public QWidget {
    Q_OBJECT

public:
    explicit ScreensPanel(ProjectController* controller, QWidget* parent = nullptr);
    void refresh();
    void addScreen();

Q_SIGNALS:
    void screenActivated(const QString& id);

private:
    void showMenu(const QPoint& position);

    ProjectController* controller_;
    QListWidget* list_;
    bool updating_ = false;
};

class LayerTree;

class LayersPanel : public QWidget {
    Q_OBJECT

public:
    explicit LayersPanel(ProjectController* controller, QWidget* parent = nullptr);
    void refresh();
    QTreeWidget* tree() const;

Q_SIGNALS:
    void screenActivated(const QString& id);

private:
    void syncSelection();
    ProjectController* controller_;
    LayerTree* tree_;
    QLineEdit* filter_;
    bool updating_ = false;
    std::set<QString> expanded_;
    std::set<QString> collapsed_;
};

class PalettePanel : public QWidget {
    Q_OBJECT

public:
    explicit PalettePanel(ProjectController* controller, QWidget* parent = nullptr);
    void refresh();

private:
    void insertIntoSelection(const QString& type);
    ProjectController* controller_;
    QScrollArea* scroll_;
    QLineEdit* search_;
};

class InspectorPanel : public QWidget {
    Q_OBJECT

public:
    explicit InspectorPanel(ProjectController* controller, QWidget* parent = nullptr);
    void rebuild();
    void refreshValues();
    void setFrameProvider(std::function<QRectF(const QString&, const QString&)> provider) { frameProvider_ = std::move(provider); }
    int currentTab() const;
    void setCurrentTab(int index);
    QString codeLocation() const;

Q_SIGNALS:
    void openCodeRequested(const QString& screen, const QString& component);
    void alignRequested(int edge);
    void distributeRequested(int axis);
    void prototypeShown(bool shown);

private:
    using Refresher = std::function<void()>;

    QVBoxLayout* section(const QString& title, const QString& key, bool startOpen = true);
    QFormLayout* formSection(const QString& title, const QString& key, bool startOpen = true);
    void addTextRow(QFormLayout* form, const QString& label, const std::string& key, bool multiline = false);
    void addNumberRow(QFormLayout* form, const QString& label, const std::string& key, double minimum = -100000, double maximum = 100000, double fallback = 0);
    void addBoolRow(QFormLayout* form, const QString& label, const std::string& key, bool fallback = false);
    void addChoiceRow(QFormLayout* form, const QString& label, const std::string& key, const QStringList& options, const QString& fallback);
    void addColorRow(QFormLayout* form, const QString& label, const std::string& key);
    void addSizeRow(QFormLayout* form, const QString& label, const std::string& key);
    void addInsetsRow(QFormLayout* form, const QString& label, const std::string& key);
    void addListRow(QFormLayout* form, const QString& label, const std::string& key);
    void addAssetRow(QFormLayout* form, const QString& label, const std::string& key);
    void addEventRow(QFormLayout* form, const QString& event);
    void addBindingRow(QFormLayout* form, const QString& label, const std::string& key);
    void addJsonRow(QFormLayout* form, const QString& label, const std::string& key);
    QDoubleSpinBox* makeNumber(const std::string& key, double minimum, double maximum, double fallback);
    void addNumberCell(QGridLayout* grid, int row, int column, const QString& label, const std::string& key);
    void addFrameCell(QGridLayout* grid, int row, int column, const QString& label, bool horizontal);
    void addDeviceRow(QFormLayout* form);
    void buildPrototype(const ir::Component* component);
    void refreshCode();
    QRectF localFrame() const;
    QString actionForScreen(const QString& screen);
    void buildComponent(const ir::Component& component, const ir::Component* parent);
    void buildMulti();
    void buildNothing();
    void setValue(const std::string& key, const Json& value);
    Json currentValue(const std::string& key) const;
    const ir::Component* selected() const;
    QString newActionFor(const QString& event);

    ProjectController* controller_;
    QTabBar* tabs_;
    QStackedWidget* pages_;
    QScrollArea* scroll_;
    QScrollArea* prototypeScroll_;
    QWidget* codePage_;
    QLabel* codeLabel_;
    class CodeEditor* codeView_;
    QPushButton* codeOpen_;
    QString codeFile_;
    int codeLine_ = 0;
    bool codeStale_ = true;
    bool generating_ = false;
    std::function<QRectF(const QString&, const QString&)> frameProvider_;
    QWidget* content_ = nullptr;
    QVBoxLayout* contentLayout_ = nullptr;
    std::vector<Refresher> refreshers_;
    QString shownComponent_;
    bool applying_ = false;
    std::map<QString, bool> openSections_;
};

}
