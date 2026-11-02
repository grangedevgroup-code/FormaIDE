#include "design/design_panels.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDrag>
#include <QDropEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyle>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QApplication>
#include <QGridLayout>
#include <QMouseEvent>
#include <tuple>

#include "app/app_context.h"
#include "canvas/canvas_widget.h"
#include "code/code_editor.h"
#include "forma/core/text.h"
#include "forma/design/design_engine.h"
#include "forma/ir/catalog.h"
#include "forma/ir/serialization.h"
#include "ui/theme.h"

namespace forma::ui {

namespace {

QToolButton* iconButton(const QString& iconName, const QString& tip, QWidget* parent)
{
    auto* button = new QToolButton(parent);
    button->setIcon(icon(iconName));
    button->setToolTip(tip);
    button->setIconSize(QSize(16, 16));
    return button;
}

QIcon rowIcon(const QString& name)
{
    return Theme::instance().icon(name, colors().textMuted);
}

QIcon iconForType(const std::string& type)
{
    const ir::ComponentSchema* schema = AppContext::instance().components().find(type);
    if (schema != nullptr)
        return rowIcon(toQ(schema->icon));
    if (type == "screen")
        return rowIcon(QStringLiteral("screens"));
    return rowIcon(QStringLiteral("custom"));
}

class NumberBox : public QDoubleSpinBox {
public:
    using QDoubleSpinBox::QDoubleSpinBox;

protected:
    QString textFromValue(double value) const override
    {
        QString text = QString::number(value, 'f', decimals());
        if (text.contains(QLatin1Char('.'))) {
            while (text.endsWith(QLatin1Char('0')))
                text.chop(1);
            if (text.endsWith(QLatin1Char('.')))
                text.chop(1);
        }
        return text;
    }
};

QString friendlyChoice(const QString& value)
{
    if (value.isEmpty())
        return QObject::tr("Default");
    QString output;
    for (qsizetype i = 0; i < value.size(); ++i) {
        QChar ch = value.at(i);
        if (i == 0) {
            output += ch.toUpper();
        } else if (ch.isUpper()) {
            output += QLatin1Char(' ');
            output += ch.toLower();
        } else {
            output += ch;
        }
    }
    return output;
}

QString friendlyEvent(const QString& event)
{
    return friendlyChoice(event);
}

class ScrubLabel : public QLabel {
public:
    ScrubLabel(const QString& text, QDoubleSpinBox* spin, ProjectController* controller, QWidget* parent) : QLabel(text, parent), spin_(spin), controller_(controller)
    {
        setCursor(Qt::SizeHorCursor);
        setToolTip(QObject::tr("Drag sideways to change this value"));
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton || !spin_->isEnabled())
            return;
        last_ = event->position().x();
        active_ = true;
        if (controller_->editor() != nullptr)
            scope_ = std::make_unique<ir::TransactionScope>(*controller_->editor(), "Change a value");
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!active_)
            return;
        double delta = std::round(event->position().x() - last_);
        if (delta == 0)
            return;
        last_ = event->position().x();
        double step = spin_->singleStep() * (event->modifiers() & Qt::ShiftModifier ? 10 : 1);
        spin_->setValue(spin_->value() + delta * step);
    }

    void mouseReleaseEvent(QMouseEvent*) override
    {
        active_ = false;
        auto* pending = scope_.release();
        QTimer::singleShot(0, qApp, [pending] { delete pending; });
    }

private:
    QDoubleSpinBox* spin_;
    ProjectController* controller_;
    std::unique_ptr<ir::TransactionScope> scope_;
    double last_ = 0;
    bool active_ = false;
};

const ir::Component* rootFor(ProjectController* controller)
{
    if (!controller->hasProject())
        return nullptr;
    const ir::Screen* screen = controller->model()->findScreen(fromQ(controller->currentScreen()));
    return screen != nullptr ? &screen->root : nullptr;
}

}

ScreensPanel::ScreensPanel(ProjectController* controller, QWidget* parent) : QWidget(parent), controller_(controller)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* bar = new QHBoxLayout();
    bar->setContentsMargins(8, 4, 4, 4);
    auto* title = new QLabel(tr("Screens"), this);
    title->setProperty("role", "section");
    bar->addWidget(title, 1);
    auto* add = iconButton(QStringLiteral("plus"), tr("Add a screen"), this);
    add->setObjectName(QStringLiteral("addScreenButton"));
    connect(add, &QToolButton::clicked, this, &ScreensPanel::addScreen);
    bar->addWidget(add);
    layout->addLayout(bar);
    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("screensList"));
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(list_, 1);
    connect(list_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
        if (updating_ || item == nullptr)
            return;
        QString id = item->data(Qt::UserRole).toString();
        controller_->setCurrentScreen(id);
        Q_EMIT screenActivated(id);
    });
    connect(list_, &QWidget::customContextMenuRequested, this, &ScreensPanel::showMenu);
    connect(controller_, &ProjectController::projectOpened, this, &ScreensPanel::refresh);
    connect(controller_, &ProjectController::projectClosed, this, &ScreensPanel::refresh);
    connect(controller_, &ProjectController::screenChanged, this, &ScreensPanel::refresh);
    connect(controller_, &ProjectController::modelChanged, this, [this](const ir::ChangeEvent& event) {
        if (event.kind == ir::ChangeKind::ScreenAdded || event.kind == ir::ChangeKind::ScreenRemoved || event.kind == ir::ChangeKind::Reset || event.kind == ir::ChangeKind::ApplicationChanged || event.key == "name")
            refresh();
    });
}

void ScreensPanel::refresh()
{
    updating_ = true;
    list_->clear();
    if (controller_->hasProject()) {
        for (const auto& screen : controller_->model()->screens) {
            QString name = toQ(screen.root.name.empty() ? screen.root.id : screen.root.name);
            if (screen.root.id == controller_->model()->navigation.start)
                name += tr(" (start)");
            auto* item = new QListWidgetItem(icon(QStringLiteral("device")), name, list_);
            item->setData(Qt::UserRole, toQ(screen.root.id));
            if (toQ(screen.root.id) == controller_->currentScreen())
                list_->setCurrentItem(item);
        }
    }
    updating_ = false;
}

void ScreensPanel::addScreen()
{
    if (!controller_->hasProject())
        return;
    bool accepted = false;
    QString name = QInputDialog::getText(this, tr("New screen"), tr("Name"), QLineEdit::Normal, tr("Settings"), &accepted);
    if (!accepted || name.trimmed().isEmpty())
        return;
    controller_->addScreen(name.trimmed());
}

static void populateScreenMenu(QMenu& menu, ProjectController* controller, const QString& id, QWidget* parent)
{
    ir::ModelEditor* editor = controller->editor();
    menu.addAction(QObject::tr("Set as start screen"), parent, [controller, id, editor] {
        Json navigation = ir::applicationToJson(editor->model())["navigation"];
        navigation["start"] = fromQ(id);
        auto status = editor->setSection("navigation", navigation);
        if (!status)
            controller->reportStatus(toQ(status.error().message));
    });
    menu.addAction(QObject::tr("Rename..."), parent, [parent, id, editor] {
        bool accepted = false;
        const ir::Screen* screen = editor->model().findScreen(fromQ(id));
        QString name = QInputDialog::getText(parent, QObject::tr("Rename screen"), QObject::tr("Name"), QLineEdit::Normal, toQ(screen->root.name), &accepted);
        if (accepted && !name.trimmed().isEmpty()) {
            auto status = editor->setField(fromQ(id), fromQ(id), "name", fromQ(name.trimmed()));
            (void)status;
        }
    });
    auto* sizes = menu.addMenu(QObject::tr("Device size"));
    for (const auto& preset : design::devicePresets()) {
        sizes->addAction(toQ(preset.name) + QStringLiteral("  %1 x %2").arg(preset.width).arg(preset.height), parent, [id, editor, preset] {
            ir::TransactionScope transaction(*editor, "Change device size");
            auto first = editor->setField(fromQ(id), fromQ(id), "width", preset.width);
            auto second = editor->setField(fromQ(id), fromQ(id), "height", preset.height);
            auto third = editor->setScreenField(fromQ(id), "device", preset.id);
            (void)first;
            (void)second;
            (void)third;
        });
    }
    menu.addSeparator();
    menu.addAction(QObject::tr("Delete"), parent, [parent, controller, id, editor] {
        if (QMessageBox::question(parent, QObject::tr("Delete screen"), QObject::tr("Delete this screen? You can undo this.")) != QMessageBox::Yes)
            return;
        auto status = editor->removeScreen(fromQ(id));
        if (!status) {
            controller->reportStatus(toQ(status.error().message));
            return;
        }
        const auto& screens = editor->model().screens;
        controller->setCurrentScreen(screens.empty() ? QString() : toQ(screens.front().root.id));
    });
}

void ScreensPanel::showMenu(const QPoint& position)
{
    QListWidgetItem* item = list_->itemAt(position);
    if (item == nullptr)
        return;
    QMenu menu(this);
    populateScreenMenu(menu, controller_, item->data(Qt::UserRole).toString(), this);
    menu.exec(list_->viewport()->mapToGlobal(position));
}

class LayerTree : public QTreeWidget {
public:
    explicit LayerTree(ProjectController* controller, QWidget* parent = nullptr) : QTreeWidget(parent), controller_(controller)
    {
        setHeaderHidden(true);
        setColumnCount(3);
        header()->setStretchLastSection(false);
        header()->setSectionResizeMode(0, QHeaderView::Stretch);
        header()->setSectionResizeMode(1, QHeaderView::Fixed);
        header()->setSectionResizeMode(2, QHeaderView::Fixed);
        header()->resizeSection(1, 22);
        header()->resizeSection(2, 22);
        setDragDropMode(QAbstractItemView::InternalMove);
        setSelectionMode(QAbstractItemView::ExtendedSelection);
        setIndentation(14);
        setAnimated(false);
        setUniformRowHeights(true);
    }

protected:
    void dropEvent(QDropEvent* event) override
    {
        QTreeWidgetItem* moved = currentItem();
        QTreeWidgetItem* target = itemAt(event->position().toPoint());
        ir::ModelEditor* editor = controller_->editor();
        if (moved == nullptr || target == nullptr || editor == nullptr || moved == target)
            return;
        DropIndicatorPosition position = dropIndicatorPosition();
        QString movedId = moved->data(0, Qt::UserRole).toString();
        QString targetId = target->data(0, Qt::UserRole).toString();
        if (moved->data(0, Qt::UserRole + 1) != target->data(0, Qt::UserRole + 1)) {
            controller_->reportStatus(tr("Layers can only move within their own screen"));
            event->ignore();
            return;
        }
        const ir::Component* root = rootFor(controller_);
        if (root == nullptr)
            return;
        std::string parent;
        int index = 0;
        if (position == QAbstractItemView::OnItem) {
            parent = fromQ(targetId);
            const ir::Component* parentNode = ir::findComponent(*root, parent);
            const ir::ComponentSchema* schema = parentNode != nullptr ? AppContext::instance().components().find(parentNode->type) : nullptr;
            if (parentNode == nullptr || (parentNode != root && (schema == nullptr || !schema->container))) {
                controller_->reportStatus(tr("That component cannot contain others"));
                return;
            }
            index = static_cast<int>(parentNode->children.size());
        } else {
            const ir::Component* parentNode = ir::findParent(*root, fromQ(targetId));
            if (parentNode == nullptr)
                return;
            parent = parentNode->id;
            index = ir::indexInParent(*parentNode, fromQ(targetId)) + (position == QAbstractItemView::BelowItem ? 1 : 0);
            const ir::Component* movedParent = ir::findParent(*root, fromQ(movedId));
            if (movedParent != nullptr && movedParent->id == parent && ir::indexInParent(*parentNode, fromQ(movedId)) < index)
                --index;
        }
        auto status = editor->moveComponent(fromQ(controller_->currentScreen()), fromQ(movedId), parent, index);
        if (!status)
            controller_->reportStatus(toQ(status.error().message));
        event->ignore();
    }

private:
    ProjectController* controller_;
};

LayersPanel::LayersPanel(ProjectController* controller, QWidget* parent) : QWidget(parent), controller_(controller)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    filter_ = new QLineEdit(this);
    filter_->setPlaceholderText(tr("Find a screen or layer"));
    filter_->setClearButtonEnabled(true);
    auto* add = iconButton(QStringLiteral("plus"), tr("Add a screen"), this);
    add->setObjectName(QStringLiteral("addScreenButton"));
    connect(add, &QToolButton::clicked, this, [this] { controller_->addScreen(); });
    auto* filterHolder = new QHBoxLayout();
    filterHolder->setContentsMargins(8, 6, 6, 6);
    filterHolder->setSpacing(4);
    filterHolder->addWidget(filter_, 1);
    filterHolder->addWidget(add);
    layout->addLayout(filterHolder);
    tree_ = new LayerTree(controller_, this);
    tree_->setObjectName(QStringLiteral("layersTree"));
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(tree_, 1);
    connect(filter_, &QLineEdit::textChanged, this, &LayersPanel::refresh);
    connect(tree_, &QWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        QTreeWidgetItem* item = tree_->itemAt(position);
        if (item == nullptr || item->parent() != nullptr)
            return;
        QMenu menu(this);
        populateScreenMenu(menu, controller_, item->data(0, Qt::UserRole).toString(), this);
        menu.exec(tree_->viewport()->mapToGlobal(position));
    });
    connect(tree_, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) {
        if (updating_ || item->parent() != nullptr)
            return;
        QString id = item->data(0, Qt::UserRole).toString();
        expanded_.insert(id);
        collapsed_.erase(id);
    });
    connect(tree_, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem* item) {
        if (updating_ || item->parent() != nullptr)
            return;
        QString id = item->data(0, Qt::UserRole).toString();
        expanded_.erase(id);
        collapsed_.insert(id);
    });
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (updating_)
            return;
        QList<QTreeWidgetItem*> items = tree_->selectedItems();
        updating_ = true;
        if (items.isEmpty()) {
            controller_->setSelection({});
            updating_ = false;
            return;
        }
        QString screen = items.first()->data(0, Qt::UserRole + 1).toString();
        bool screenRow = false;
        QStringList ids;
        for (auto* item : items) {
            if (item->data(0, Qt::UserRole + 1).toString() != screen)
                continue;
            ids << item->data(0, Qt::UserRole).toString();
            screenRow = screenRow || item->parent() == nullptr;
        }
        controller_->setCurrentScreen(screen);
        controller_->setSelection(ids);
        updating_ = false;
        if (screenRow)
            Q_EMIT screenActivated(screen);
    });
    connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int column) {
        ir::ModelEditor* editor = controller_->editor();
        if (editor == nullptr || column == 0)
            return;
        QString id = item->data(0, Qt::UserRole).toString();
        const ir::Component* root = rootFor(controller_);
        const ir::Component* component = root != nullptr ? ir::findComponent(*root, fromQ(id)) : nullptr;
        if (component == nullptr || component == root)
            return;
        if (column == 1) {
            auto status = editor->setField(fromQ(controller_->currentScreen()), fromQ(id), "visible", component->visible ? Json(false) : Json(nullptr));
            (void)status;
        } else if (column == 2) {
            auto status = editor->setField(fromQ(controller_->currentScreen()), fromQ(id), "locked", component->locked ? Json(nullptr) : Json(true));
            (void)status;
        }
    });
    connect(controller_, &ProjectController::projectOpened, this, &LayersPanel::refresh);
    connect(controller_, &ProjectController::projectClosed, this, &LayersPanel::refresh);
    connect(controller_, &ProjectController::screenChanged, this, &LayersPanel::refresh, Qt::QueuedConnection);
    connect(controller_, &ProjectController::modelChanged, this, &LayersPanel::refresh, Qt::QueuedConnection);
    connect(controller_, &ProjectController::selectionChanged, this, &LayersPanel::syncSelection);
}

QTreeWidget* LayersPanel::tree() const
{
    return tree_;
}

void LayersPanel::refresh()
{
    updating_ = true;
    tree_->clear();
    QString needle = filter_->text().trimmed();
    if (controller_->hasProject()) {
        const ir::ApplicationModel* model = controller_->model();
        for (const auto& screen : model->screens) {
            const ir::Component* root = &screen.root;
            QString screenId = toQ(root->id);
            std::function<bool(const ir::Component&, QTreeWidgetItem*)> addNode = [&](const ir::Component& node, QTreeWidgetItem* parent) -> bool {
                QString name = toQ(node.name.empty() ? node.id : node.name);
                auto* item = parent != nullptr ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree_);
                item->setText(0, name);
                item->setIcon(0, parent == nullptr ? rowIcon(QStringLiteral("device")) : iconForType(node.type));
                item->setData(0, Qt::UserRole, toQ(node.id));
                item->setData(0, Qt::UserRole + 1, screenId);
                item->setToolTip(0, toQ(node.type) + QStringLiteral(" - ") + toQ(node.id) + (node.codeManaged.empty() ? QString() : tr("\nPartly managed in code")));
                if (&node != root) {
                    item->setIcon(1, Theme::instance().icon(node.visible ? QStringLiteral("eye") : QStringLiteral("eye-off"), colors().textFaint));
                    item->setToolTip(1, node.visible ? tr("Hide") : tr("Show"));
                    if (node.locked)
                        item->setIcon(2, rowIcon(QStringLiteral("lock")));
                    else
                        item->setIcon(2, Theme::instance().icon(QStringLiteral("unlock"), colors().textFaint));
                    item->setToolTip(2, node.locked ? tr("Unlock") : tr("Lock"));
                } else {
                    item->setFlags(item->flags() & ~Qt::ItemIsDragEnabled);
                    QFont bold = item->font(0);
                    bold.setWeight(QFont::DemiBold);
                    item->setFont(0, bold);
                    if (screen.root.id == model->navigation.start)
                        item->setText(0, name + tr(" (start)"));
                }
                if (!node.codeManaged.empty())
                    item->setForeground(0, colors().generated);
                if (!node.visible)
                    item->setForeground(0, colors().textFaint);
                bool anyChild = false;
                for (auto child = node.children.rbegin(); child != node.children.rend(); ++child) {
                    if (addNode(*child, item))
                        anyChild = true;
                }
                bool matches = needle.isEmpty() || name.contains(needle, Qt::CaseInsensitive) || toQ(node.type).contains(needle, Qt::CaseInsensitive);
                if (!matches && !anyChild) {
                    delete item;
                    return false;
                }
                return true;
            };
            addNode(*root, nullptr);
        }
        tree_->expandAll();
        for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
            QTreeWidgetItem* item = tree_->topLevelItem(i);
            QString id = item->data(0, Qt::UserRole).toString();
            bool open = !needle.isEmpty() || expanded_.count(id) > 0 || (id == controller_->currentScreen() && collapsed_.count(id) == 0);
            item->setExpanded(open);
        }
    }
    updating_ = false;
    syncSelection();
}

void LayersPanel::syncSelection()
{
    if (updating_)
        return;
    updating_ = true;
    tree_->clearSelection();
    QStringList ids = controller_->selection();
    QString screen = controller_->currentScreen();
    QTreeWidgetItemIterator it(tree_);
    while (*it) {
        if ((*it)->data(0, Qt::UserRole + 1).toString() == screen && ids.contains((*it)->data(0, Qt::UserRole).toString())) {
            (*it)->setSelected(true);
            tree_->scrollToItem(*it);
        }
        ++it;
    }
    updating_ = false;
}

class PaletteTile : public QToolButton {
public:
    PaletteTile(const QString& type, const QString& label, const QString& iconName, QWidget* parent) : QToolButton(parent), type_(type)
    {
        setObjectName(QStringLiteral("palette.") + type);
        setText(label);
        setIcon(forma::ui::icon(iconName));
        setIconSize(QSize(16, 16));
        setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        setFixedHeight(30);
        setStyleSheet(QStringLiteral("QToolButton { text-align: left; padding: 3px 6px; }"));
        setToolTip(tr("%1. Drag it onto a screen, or click to add it to the selected layer.").arg(label));
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton)
            start_ = event->position().toPoint();
        QToolButton::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if ((event->buttons() & Qt::LeftButton) && (event->position().toPoint() - start_).manhattanLength() >= QApplication::startDragDistance()) {
            setDown(false);
            auto* drag = new QDrag(this);
            auto* data = new QMimeData();
            data->setData(QString::fromLatin1(kComponentMimeType), type_.toUtf8());
            drag->setMimeData(data);
            drag->setPixmap(icon().pixmap(24, 24));
            drag->exec(Qt::CopyAction);
            return;
        }
        QToolButton::mouseMoveEvent(event);
    }

private:
    QString type_;
    QPoint start_;
};

PalettePanel::PalettePanel(ProjectController* controller, QWidget* parent) : QWidget(parent), controller_(controller)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(tr("Search components"));
    search_->setClearButtonEnabled(true);
    auto* holder = new QHBoxLayout();
    holder->setContentsMargins(8, 6, 8, 6);
    holder->addWidget(search_);
    layout->addLayout(holder);
    scroll_ = new QScrollArea(this);
    scroll_->setObjectName(QStringLiteral("paletteScroll"));
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(scroll_, 1);
    connect(search_, &QLineEdit::textChanged, this, &PalettePanel::refresh);
    connect(controller_, &ProjectController::modelChanged, this, [this](const ir::ChangeEvent& event) {
        if (event.key == "components" || event.kind == ir::ChangeKind::Reset)
            refresh();
    });
    connect(controller_, &ProjectController::projectOpened, this, &PalettePanel::refresh);
    refresh();
}

void PalettePanel::refresh()
{
    auto* body = new QWidget();
    auto* column = new QVBoxLayout(body);
    column->setContentsMargins(8, 0, 8, 8);
    column->setSpacing(2);
    QString needle = search_->text().trimmed();
    const auto& catalog = AppContext::instance().components();
    auto addGroup = [&](const QString& title, const std::vector<std::tuple<QString, QString, QString>>& items) {
        if (items.empty())
            return;
        auto* label = new QLabel(title, body);
        label->setProperty("role", "section");
        label->setContentsMargins(4, 8, 0, 2);
        column->addWidget(label);
        auto* grid = new QGridLayout();
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setSpacing(2);
        grid->setColumnStretch(0, 1);
        grid->setColumnStretch(1, 1);
        int index = 0;
        for (const auto& [type, text, iconName] : items) {
            auto* tile = new PaletteTile(type, text, iconName, body);
            QString chosen = type;
            connect(tile, &QToolButton::clicked, this, [this, chosen] { insertIntoSelection(chosen); });
            grid->addWidget(tile, index / 2, index % 2);
            ++index;
        }
        column->addLayout(grid);
    };
    if (controller_->hasProject()) {
        std::vector<std::tuple<QString, QString, QString>> reusable;
        for (const auto& definition : controller_->model()->components) {
            QString label = toQ(definition.root.name);
            if (needle.isEmpty() || label.contains(needle, Qt::CaseInsensitive))
                reusable.emplace_back(QStringLiteral("instance:") + toQ(definition.root.id), label, QStringLiteral("components"));
        }
        addGroup(tr("Reusable"), reusable);
    }
    for (const auto& name : catalog.categories()) {
        std::vector<std::tuple<QString, QString, QString>> items;
        for (const auto& schema : catalog.all()) {
            if (schema.category != name)
                continue;
            QString label = toQ(schema.label);
            if (needle.isEmpty() || label.contains(needle, Qt::CaseInsensitive))
                items.emplace_back(toQ(schema.type), label, toQ(schema.icon));
        }
        addGroup(toQ(name), items);
    }
    column->addStretch(1);
    QWidget* old = scroll_->takeWidget();
    scroll_->setWidget(body);
    if (old != nullptr)
        old->deleteLater();
}

void PalettePanel::insertIntoSelection(const QString& type)
{
    ir::ModelEditor* editor = controller_->editor();
    const ir::Component* root = rootFor(controller_);
    if (editor == nullptr || root == nullptr) {
        controller_->reportStatus(tr("Add a screen first"));
        return;
    }
    QString parent = toQ(root->id);
    if (controller_->selection().size() == 1) {
        QString selected = controller_->selection().front();
        const ir::Component* node = ir::findComponent(*root, fromQ(selected));
        const ir::ComponentSchema* schema = node != nullptr ? AppContext::instance().components().find(node->type) : nullptr;
        if (node != nullptr && schema != nullptr && schema->container)
            parent = selected;
        else if (node != nullptr) {
            const ir::Component* owner = ir::findParent(*root, fromQ(selected));
            if (owner != nullptr)
                parent = toQ(owner->id);
        }
    }
    std::string screen = fromQ(controller_->currentScreen());
    ir::Component component;
    if (type.startsWith(QStringLiteral("instance:"))) {
        std::string definition = fromQ(type.mid(9));
        const ir::ComponentDefinition* found = controller_->model()->findDefinition(definition);
        if (found == nullptr)
            return;
        component = ir::makeComponent(editor->uniqueComponentId(screen, definition), found->root.type, found->root.name);
        component.instanceOf = definition;
    } else {
        component = AppContext::instance().components().instantiate(fromQ(type), editor->uniqueComponentId(screen, fromQ(type)));
    }
    auto inserted = editor->insertComponent(screen, fromQ(parent), -1, std::move(component));
    if (inserted)
        controller_->setSelection({toQ(*inserted)});
    else
        controller_->reportStatus(toQ(inserted.error().message));
}

InspectorPanel::InspectorPanel(ProjectController* controller, QWidget* parent) : QWidget(parent), controller_(controller)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    tabs_ = new QTabBar(this);
    tabs_->setObjectName(QStringLiteral("inspectorTabs"));
    tabs_->setExpanding(false);
    tabs_->setDrawBase(false);
    tabs_->addTab(tr("Design"));
    tabs_->addTab(tr("Prototype"));
    tabs_->addTab(tr("Code"));
    layout->addWidget(tabs_);
    auto* line = new QFrame(this);
    line->setProperty("role", "line");
    layout->addWidget(line);
    pages_ = new QStackedWidget(this);
    auto makeScroll = [this](const QString& name) {
        auto* area = new QScrollArea(pages_);
        area->setObjectName(name);
        area->setWidgetResizable(true);
        area->setFrameShape(QFrame::NoFrame);
        area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        pages_->addWidget(area);
        return area;
    };
    scroll_ = makeScroll(QStringLiteral("inspectorScroll"));
    prototypeScroll_ = makeScroll(QStringLiteral("prototypeScroll"));
    codePage_ = new QWidget(pages_);
    auto* codeLayout = new QVBoxLayout(codePage_);
    codeLayout->setContentsMargins(0, 0, 0, 0);
    codeLayout->setSpacing(0);
    auto* codeHeader = new QHBoxLayout();
    codeHeader->setContentsMargins(10, 6, 6, 6);
    codeLabel_ = new QLabel(codePage_);
    codeLabel_->setObjectName(QStringLiteral("inspectorCodeLabel"));
    codeLabel_->setProperty("role", "muted");
    codeLabel_->setWordWrap(true);
    codeHeader->addWidget(codeLabel_, 1);
    codeOpen_ = new QPushButton(tr("Open"), codePage_);
    codeOpen_->setObjectName(QStringLiteral("inspectorCodeOpen"));
    codeHeader->addWidget(codeOpen_);
    codeLayout->addLayout(codeHeader);
    codeView_ = new CodeEditor(codePage_);
    codeView_->setObjectName(QStringLiteral("inspectorCode"));
    codeView_->setReadOnly(true);
    codeView_->setWordWrap(true);
    codeLayout->addWidget(codeView_, 1);
    pages_->addWidget(codePage_);
    layout->addWidget(pages_, 1);
    connect(tabs_, &QTabBar::currentChanged, this, [this](int index) {
        pages_->setCurrentIndex(index);
        Q_EMIT prototypeShown(index == 1);
        if (index == 2)
            refreshCode();
    });
    connect(codeOpen_, &QPushButton::clicked, this, [this] {
        if (!codeFile_.isEmpty())
            controller_->openFile(codeFile_, codeLine_);
    });
    connect(controller_, &ProjectController::selectionChanged, this, &InspectorPanel::rebuild);
    connect(controller_, &ProjectController::screenChanged, this, &InspectorPanel::rebuild);
    connect(controller_, &ProjectController::projectOpened, this, &InspectorPanel::rebuild);
    connect(controller_, &ProjectController::projectClosed, this, &InspectorPanel::rebuild);
    connect(controller_, &ProjectController::generationFinished, this, [this](const codegen::GenerationReport&) {
        if (!generating_)
            codeStale_ = false;
        if (!generating_ && tabs_->currentIndex() == 2)
            refreshCode();
    });
    connect(controller_, &ProjectController::modelChanged, this, [this](const ir::ChangeEvent& event) {
        codeStale_ = true;
        if (applying_) {
            QMetaObject::invokeMethod(this, &InspectorPanel::refreshValues, Qt::QueuedConnection);
            return;
        }
        if (event.kind == ir::ChangeKind::ComponentChanged && toQ(event.component) == shownComponent_ && event.key != "layout.type" && event.key != "instanceOf")
            refreshValues();
        else
            rebuild();
    });
    openSections_ = {{QStringLiteral("advanced"), false}};
    rebuild();
}

int InspectorPanel::currentTab() const
{
    return tabs_->currentIndex();
}

void InspectorPanel::setCurrentTab(int index)
{
    tabs_->setCurrentIndex(index);
}

QString InspectorPanel::codeLocation() const
{
    return codeLabel_->text();
}

void InspectorPanel::refreshCode()
{
    codeFile_.clear();
    codeLine_ = 0;
    codeOpen_->setEnabled(false);
    const ir::Component* component = selected();
    auto showText = [this](const QString& text) {
        codeView_->setReadOnly(false);
        codeView_->setText(text);
        codeView_->setReadOnly(true);
    };
    if (!controller_->hasProject() || controller_->target() == nullptr || controller_->target()->generator.empty()) {
        codeLabel_->setText(tr("This project has no code generator, so there's no generated code to show."));
        showText(QString());
        return;
    }
    if (component == nullptr) {
        codeLabel_->setText(tr("Select a layer to see the code it generates."));
        showText(QString());
        return;
    }
    if (codeStale_ && !generating_) {
        generating_ = true;
        codeStale_ = false;
        controller_->regenerate(true);
        generating_ = false;
    }
    auto& files = controller_->session()->filesystem();
    auto location = codegen::locateComponent(files, controller_->target()->id, fromQ(controller_->currentScreen()), component->id);
    if (!location) {
        codeLabel_->setText(tr("This layer has no code of its own. Its parent writes it."));
        showText(QString());
        return;
    }
    auto content = files.readFile(location->file);
    if (!content) {
        codeLabel_->setText(toQ(content.error().message));
        showText(QString());
        return;
    }
    codeFile_ = toQ(location->file);
    codeLine_ = location->line;
    codeOpen_->setEnabled(true);
    codeLabel_->setText(QStringLiteral("%1:%2").arg(codeFile_).arg(codeLine_));
    codeView_->setLanguage(AppContext::instance().languages().forPath(location->file));
    showText(toQ(*content));
    codeView_->gotoLine(codeLine_);
}

const ir::Component* InspectorPanel::selected() const
{
    const ir::Component* root = rootFor(controller_);
    if (root == nullptr || controller_->selection().size() != 1)
        return nullptr;
    return ir::findComponent(*root, fromQ(controller_->selection().front()));
}

void InspectorPanel::rebuild()
{
    refreshers_.clear();
    shownComponent_.clear();
    const ir::Component* root = rootFor(controller_);
    const ir::Component* component = selected();
    if (component != nullptr)
        shownComponent_ = toQ(component->id);
    auto buildPage = [this](QScrollArea* area, const QString& name, const std::function<void()>& fill) {
        content_ = new QWidget();
        content_->setObjectName(name);
        contentLayout_ = new QVBoxLayout(content_);
        contentLayout_->setContentsMargins(0, 0, 0, 0);
        contentLayout_->setSpacing(0);
        fill();
        contentLayout_->addStretch(1);
        QWidget* old = area->takeWidget();
        area->setWidget(content_);
        if (old != nullptr)
            old->deleteLater();
    };
    buildPage(scroll_, QStringLiteral("inspectorContent"), [&] {
        if (component != nullptr)
            buildComponent(*component, ir::findParent(*root, component->id));
        else if (controller_->selection().size() > 1)
            buildMulti();
        else
            buildNothing();
    });
    buildPage(prototypeScroll_, QStringLiteral("prototypeContent"), [&] { buildPrototype(component); });
    refreshValues();
    if (tabs_->currentIndex() == 2)
        refreshCode();
}

QRectF InspectorPanel::localFrame() const
{
    if (!frameProvider_ || !controller_->hasProject() || shownComponent_.isEmpty())
        return {};
    QRectF world = frameProvider_(controller_->currentScreen(), shownComponent_);
    const ir::Screen* screen = controller_->model()->findScreen(fromQ(controller_->currentScreen()));
    if (screen != nullptr)
        world.translate(-screen->canvas.x, -screen->canvas.y);
    return world;
}

void InspectorPanel::refreshValues()
{
    bool previous = applying_;
    applying_ = true;
    for (const auto& refresher : refreshers_)
        refresher();
    applying_ = previous;
}

void InspectorPanel::setValue(const std::string& key, const Json& value)
{
    ir::ModelEditor* editor = controller_->editor();
    if (editor == nullptr || applying_)
        return;
    applying_ = true;
    std::string screen = fromQ(controller_->currentScreen());
    for (const auto& id : controller_->selection()) {
        auto status = editor->setField(screen, fromQ(id), key, value);
        if (!status)
            controller_->reportStatus(toQ(status.error().message));
    }
    applying_ = false;
}

Json InspectorPanel::currentValue(const std::string& key) const
{
    const ir::Component* component = selected();
    if (component == nullptr)
        return nullptr;
    return ir::getComponentField(*component, key);
}

QVBoxLayout* InspectorPanel::section(const QString& title, const QString& key, bool startOpen)
{
    auto it = openSections_.find(key);
    bool open = it != openSections_.end() ? it->second : startOpen;
    auto* header = new QToolButton(content_);
    header->setText(title);
    header->setIcon(icon(open ? QStringLiteral("chevron-down") : QStringLiteral("chevron-right")));
    header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    header->setIconSize(QSize(12, 12));
    header->setCheckable(false);
    header->setStyleSheet(QStringLiteral("QToolButton { font-weight: 600; text-align: left; padding: 7px 6px 5px 6px; border-radius: 0; border-top: 1px solid %1; }").arg(colors().border.name()));
    auto* body = new QWidget(content_);
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(10, 2, 8, 8);
    bodyLayout->setSpacing(4);
    body->setVisible(open);
    connect(header, &QToolButton::clicked, this, [this, body, header, key] {
        bool next = !body->isVisible();
        body->setVisible(next);
        header->setIcon(icon(next ? QStringLiteral("chevron-down") : QStringLiteral("chevron-right")));
        openSections_[key] = next;
    });
    contentLayout_->addWidget(header);
    contentLayout_->addWidget(body);
    return bodyLayout;
}

QFormLayout* InspectorPanel::formSection(const QString& title, const QString& key, bool startOpen)
{
    QVBoxLayout* body = section(title, key, startOpen);
    auto* form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(8);
    form->setVerticalSpacing(4);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    body->addLayout(form);
    return form;
}

void InspectorPanel::addTextRow(QFormLayout* form, const QString& label, const std::string& key, bool multiline)
{
    if (multiline) {
        auto* edit = new QPlainTextEdit(content_);
        edit->setFixedHeight(60);
        edit->setObjectName(toQ("field_" + key));
        form->addRow(label, edit);
        connect(edit, &QPlainTextEdit::textChanged, this, [this, key, edit] {
            if (!applying_) {
                QString value = edit->toPlainText();
                setValue(key, value.isEmpty() ? Json(nullptr) : Json(fromQ(value)));
            }
        });
        refreshers_.push_back([this, key, edit] {
            Json value = currentValue(key);
            QString text = value.is_string() ? toQ(value.get<std::string>()) : QString();
            if (edit->toPlainText() != text)
                edit->setPlainText(text);
        });
        return;
    }
    auto* edit = new QLineEdit(content_);
    edit->setObjectName(toQ("field_" + key));
    form->addRow(label, edit);
    connect(edit, &QLineEdit::editingFinished, this, [this, key, edit] {
        QString value = edit->text();
        setValue(key, value.isEmpty() ? Json(nullptr) : Json(fromQ(value)));
    });
    refreshers_.push_back([this, key, edit] {
        Json value = currentValue(key);
        QString text = value.is_string() ? toQ(value.get<std::string>()) : QString();
        if (edit->text() != text)
            edit->setText(text);
    });
}

QDoubleSpinBox* InspectorPanel::makeNumber(const std::string& key, double minimum, double maximum, double fallback)
{
    auto* spin = new NumberBox(content_);
    spin->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    spin->setMinimumWidth(36);
    spin->setObjectName(toQ("field_" + key));
    bool optional = fallback <= -999999;
    if (optional) {
        minimum = -100000;
        fallback = minimum;
        spin->setSpecialValueText(QStringLiteral(" "));
    }
    spin->setRange(minimum, maximum);
    spin->setDecimals(2);
    spin->setSingleStep(maximum <= 1 ? 0.01 : 1);
    spin->setKeyboardTracking(false);
    connect(spin, &QDoubleSpinBox::valueChanged, this, [this, key, fallback](double value) { setValue(key, std::abs(value - fallback) < 1e-9 ? Json(nullptr) : Json(value)); });
    refreshers_.push_back([this, key, spin, fallback] {
        Json value = currentValue(key);
        double number = value.is_number() ? value.get<double>() : fallback;
        if (spin->value() != number)
            spin->setValue(number);
    });
    return spin;
}

void InspectorPanel::addNumberRow(QFormLayout* form, const QString& label, const std::string& key, double minimum, double maximum, double fallback)
{
    QDoubleSpinBox* spin = makeNumber(key, minimum, maximum, fallback);
    form->addRow(new ScrubLabel(label, spin, controller_, content_), spin);
}

void InspectorPanel::addNumberCell(QGridLayout* grid, int row, int column, const QString& label, const std::string& key)
{
    QDoubleSpinBox* spin = makeNumber(key, -100000, 100000, 0);
    auto* tag = new ScrubLabel(label, spin, controller_, content_);
    tag->setProperty("role", "fieldTag");
    grid->addWidget(tag, row, column);
    grid->addWidget(spin, row, column + 1);
}

void InspectorPanel::addFrameCell(QGridLayout* grid, int row, int column, const QString& label, bool horizontal)
{
    auto* tag = new QLabel(label, content_);
    tag->setProperty("role", "fieldTag");
    auto* spin = new QDoubleSpinBox(content_);
    spin->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    spin->setMinimumWidth(36);
    spin->setRange(-100000, 100000);
    spin->setDecimals(0);
    spin->setEnabled(false);
    spin->setToolTip(tr("Set by the layout of the parent"));
    spin->setObjectName(horizontal ? QStringLiteral("frame_x") : QStringLiteral("frame_y"));
    grid->addWidget(tag, row, column);
    grid->addWidget(spin, row, column + 1);
    refreshers_.push_back([this, spin, horizontal] {
        QRectF frame = localFrame();
        spin->setValue(horizontal ? frame.x() : frame.y());
    });
}

void InspectorPanel::addDeviceRow(QFormLayout* form)
{
    auto* combo = new QComboBox(content_);
    combo->setObjectName(QStringLiteral("field_device"));
    combo->addItem(tr("Custom size"), QString());
    for (const auto& preset : design::devicePresets())
        combo->addItem(toQ(preset.name), toQ(preset.id));
    form->addRow(tr("Device"), combo);
    connect(combo, &QComboBox::activated, this, [this, combo] {
        QString id = combo->currentData().toString();
        const design::DevicePreset* preset = id.isEmpty() ? nullptr : design::findDevicePreset(fromQ(id));
        ir::ModelEditor* editor = controller_->editor();
        if (preset == nullptr || editor == nullptr)
            return;
        std::string screen = fromQ(controller_->currentScreen());
        ir::TransactionScope transaction(*editor, "Change device size");
        auto first = editor->setField(screen, screen, "width", preset->width);
        auto second = editor->setField(screen, screen, "height", preset->height);
        auto third = editor->setScreenField(screen, "device", preset->id);
        (void)first;
        (void)second;
        (void)third;
    });
    refreshers_.push_back([this, combo] {
        const ir::Screen* screen = controller_->hasProject() ? controller_->model()->findScreen(fromQ(controller_->currentScreen())) : nullptr;
        if (screen != nullptr)
            combo->setCurrentIndex(std::max(0, combo->findData(toQ(screen->device))));
    });
}

void InspectorPanel::addBoolRow(QFormLayout* form, const QString& label, const std::string& key, bool fallback)
{
    auto* box = new QCheckBox(content_);
    box->setObjectName(toQ("field_" + key));
    form->addRow(label, box);
    connect(box, &QCheckBox::toggled, this, [this, key, fallback](bool on) { setValue(key, on == fallback ? Json(nullptr) : Json(on)); });
    refreshers_.push_back([this, key, box, fallback] {
        Json value = currentValue(key);
        bool on = value.is_boolean() ? value.get<bool>() : fallback;
        if (box->isChecked() != on)
            box->setChecked(on);
    });
}

void InspectorPanel::addChoiceRow(QFormLayout* form, const QString& label, const std::string& key, const QStringList& options, const QString& fallback)
{
    auto* combo = new QComboBox(content_);
    combo->setObjectName(toQ("field_" + key));
    for (const auto& option : options)
        combo->addItem(friendlyChoice(option), option);
    form->addRow(label, combo);
    connect(combo, &QComboBox::currentIndexChanged, this, [this, key, fallback, combo] {
        if (applying_)
            return;
        QString value = combo->currentData().toString();
        setValue(key, value == fallback ? Json(nullptr) : Json(fromQ(value)));
    });
    refreshers_.push_back([this, key, combo, fallback] {
        Json value = currentValue(key);
        QString text = value.is_string() ? toQ(value.get<std::string>()) : fallback;
        int index = combo->findData(text);
        if (index >= 0 && combo->currentIndex() != index)
            combo->setCurrentIndex(index);
    });
}

void InspectorPanel::addColorRow(QFormLayout* form, const QString& label, const std::string& key)
{
    auto* holder = new QWidget(content_);
    auto* row = new QHBoxLayout(holder);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(4);
    auto* swatch = new QToolButton(holder);
    swatch->setFixedSize(24, 24);
    swatch->setToolTip(tr("Pick a color"));
    auto* combo = new QComboBox(holder);
    combo->setObjectName(toQ("field_" + key));
    combo->setEditable(true);
    combo->addItem(QString());
    if (const ir::Theme* theme = controller_->hasProject() ? controller_->model()->theme() : nullptr) {
        for (const auto& [name, value] : theme->tokens.colors)
            combo->addItem(toQ("$color." + name));
    }
    combo->setMinimumWidth(40);
    row->addWidget(swatch);
    row->addWidget(combo, 1);
    form->addRow(label, holder);
    auto updateSwatch = [this, swatch, combo] {
        QString value = combo->currentText();
        QString resolved = controller_->hasProject() ? toQ(ir::resolveColor(*controller_->model(), fromQ(value), "")) : value;
        QColor color(resolved);
        swatch->setStyleSheet(QStringLiteral("QToolButton { background: %1; border: 1px solid %2; border-radius: 3px; }").arg(color.isValid() ? color.name(QColor::HexArgb) : QStringLiteral("transparent"), colors().borderStrong.name()));
    };
    connect(combo, &QComboBox::currentTextChanged, this, [this, key, updateSwatch](const QString& value) {
        updateSwatch();
        setValue(key, value.isEmpty() ? Json(nullptr) : Json(fromQ(value)));
    });
    connect(swatch, &QToolButton::clicked, this, [this, combo] {
        QColor chosen = QColorDialog::getColor(QColor(combo->currentText()), this, tr("Pick a color"), QColorDialog::ShowAlphaChannel);
        if (chosen.isValid())
            combo->setCurrentText(chosen.alpha() == 255 ? chosen.name().toUpper() : chosen.name(QColor::HexArgb).toUpper());
    });
    refreshers_.push_back([this, key, combo, updateSwatch] {
        Json value = currentValue(key);
        QString text = value.is_string() ? toQ(value.get<std::string>()) : QString();
        if (combo->currentText() != text)
            combo->setCurrentText(text);
        updateSwatch();
    });
}

void InspectorPanel::addSizeRow(QFormLayout* form, const QString& label, const std::string& key)
{
    auto* holder = new QWidget(content_);
    auto* row = new QHBoxLayout(holder);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(4);
    auto* mode = new QComboBox(holder);
    mode->setObjectName(toQ("mode_" + key));
    mode->addItem(tr("Hug"), QStringLiteral("hug"));
    mode->addItem(tr("Fill"), QStringLiteral("fill"));
    mode->addItem(tr("Fixed"), QStringLiteral("fixed"));
    mode->addItem(tr("Percent"), QStringLiteral("percent"));
    auto* value = new QDoubleSpinBox(holder);
    value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    value->setMinimumWidth(36);
    value->setObjectName(toQ("field_" + key));
    value->setRange(0, 100000);
    value->setDecimals(0);
    value->setKeyboardTracking(false);
    row->addWidget(value, 1);
    row->addWidget(mode, 1);
    form->addRow(new ScrubLabel(label, value, controller_, content_), holder);
    auto apply = [this, key, mode, value] {
        if (applying_)
            return;
        QString chosen = mode->currentData().toString();
        Json current = currentValue(key);
        Json output;
        if (chosen == QStringLiteral("fixed")) {
            output = value->value();
        } else if (chosen == QStringLiteral("percent")) {
            output = fromQ(QString::number(value->value()) + QLatin1Char('%'));
        } else {
            output = fromQ(chosen);
        }
        if (current.is_object()) {
            Json object = current;
            object["mode"] = fromQ(chosen);
            if (chosen == QStringLiteral("fixed") || chosen == QStringLiteral("percent"))
                object["value"] = value->value();
            else
                object.erase("value");
            output = object;
        }
        setValue(key, output);
    };
    connect(mode, &QComboBox::currentIndexChanged, this, apply);
    connect(value, &QDoubleSpinBox::valueChanged, this, apply);
    refreshers_.push_back([this, key, mode, value] {
        const ir::Component* component = selected();
        if (component == nullptr)
            return;
        const ir::SizeSpec& spec = key == "width" ? component->width : component->height;
        mode->setCurrentIndex(mode->findData(toQ(ir::sizeModeName(spec.mode))));
        value->setEnabled(spec.mode == ir::SizeMode::Fixed || spec.mode == ir::SizeMode::Percent);
        if (spec.mode == ir::SizeMode::Fixed || spec.mode == ir::SizeMode::Percent) {
            value->setValue(spec.value);
        } else {
            QRectF frame = localFrame();
            value->setValue(key == "width" ? frame.width() : frame.height());
        }
    });
}

void InspectorPanel::addInsetsRow(QFormLayout* form, const QString& label, const std::string& key)
{
    auto* holder = new QWidget(content_);
    auto* row = new QHBoxLayout(holder);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(2);
    std::array<QDoubleSpinBox*, 4> edits{};
    const QStringList tips = {tr("Top"), tr("Right"), tr("Bottom"), tr("Left")};
    for (int i = 0; i < 4; ++i) {
        auto* spin = new QDoubleSpinBox(holder);
        spin->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        spin->setMinimumWidth(36);
        spin->setRange(0, 10000);
        spin->setDecimals(0);
        spin->setKeyboardTracking(false);
        spin->setToolTip(tips[i]);
        spin->setObjectName(toQ("field_" + key + "_" + std::to_string(i)));
        row->addWidget(spin);
        edits[static_cast<std::size_t>(i)] = spin;
    }
    form->addRow(label, holder);
    auto apply = [this, key, edits] {
        if (applying_)
            return;
        double top = edits[0]->value();
        double right = edits[1]->value();
        double bottom = edits[2]->value();
        double left = edits[3]->value();
        if (top == 0 && right == 0 && bottom == 0 && left == 0)
            setValue(key, nullptr);
        else if (top == right && right == bottom && bottom == left)
            setValue(key, top);
        else
            setValue(key, Json::array({top, right, bottom, left}));
    };
    for (auto* spin : edits)
        connect(spin, &QDoubleSpinBox::valueChanged, this, apply);
    refreshers_.push_back([this, key, edits] {
        Json value = currentValue(key);
        std::array<double, 4> numbers{0, 0, 0, 0};
        if (value.is_number())
            numbers.fill(value.get<double>());
        else if (value.is_array() && value.size() == 4) {
            for (std::size_t i = 0; i < 4; ++i)
                numbers[i] = value[i].get<double>();
        }
        for (std::size_t i = 0; i < 4; ++i)
            edits[i]->setValue(numbers[i]);
    });
}

void InspectorPanel::addListRow(QFormLayout* form, const QString& label, const std::string& key)
{
    auto* edit = new QPlainTextEdit(content_);
    edit->setObjectName(toQ("field_" + key));
    edit->setFixedHeight(72);
    edit->setToolTip(tr("One item per line"));
    form->addRow(label, edit);
    connect(edit, &QPlainTextEdit::textChanged, this, [this, key, edit] {
        if (applying_)
            return;
        Json items = Json::array();
        for (const auto& line : edit->toPlainText().split(QLatin1Char('\n'))) {
            if (!line.trimmed().isEmpty())
                items.push_back(fromQ(line.trimmed()));
        }
        setValue(key, items);
    });
    refreshers_.push_back([this, key, edit] {
        Json value = currentValue(key);
        QStringList lines;
        if (value.is_array()) {
            for (const auto& item : value) {
                if (item.is_string())
                    lines << toQ(item.get<std::string>());
            }
        }
        QString text = lines.join(QLatin1Char('\n'));
        if (edit->toPlainText().trimmed() != text)
            edit->setPlainText(text);
    });
}

void InspectorPanel::addAssetRow(QFormLayout* form, const QString& label, const std::string& key)
{
    auto* combo = new QComboBox(content_);
    combo->setObjectName(toQ("field_" + key));
    combo->setEditable(true);
    combo->addItem(QString());
    if (controller_->hasProject()) {
        for (const auto& asset : controller_->model()->assets)
            combo->addItem(toQ(asset.id));
    }
    form->addRow(label, combo);
    connect(combo, &QComboBox::currentTextChanged, this, [this, key](const QString& value) { setValue(key, value.isEmpty() ? Json(nullptr) : Json(fromQ(value))); });
    refreshers_.push_back([this, key, combo] {
        Json value = currentValue(key);
        QString text = value.is_string() ? toQ(value.get<std::string>()) : QString();
        if (combo->currentText() != text)
            combo->setCurrentText(text);
    });
}

QString InspectorPanel::newActionFor(const QString& event)
{
    QStringList kinds = {tr("Go to a screen"), tr("Go back"), tr("Set a state value"), tr("Toggle a state value"), tr("Add to a state value"), tr("Open a web address"), tr("Call your own code")};
    bool accepted = false;
    QString kindLabel = QInputDialog::getItem(this, tr("New action"), tr("When %1 happens").arg(event), kinds, 0, false, &accepted);
    if (!accepted)
        return {};
    int index = kinds.indexOf(kindLabel);
    static const char* kindNames[] = {"navigate", "back", "setState", "toggleState", "incrementState", "openUrl", "handler"};
    Json action = Json::object();
    action["kind"] = kindNames[index];
    std::string base = text::toSnakeCase(shownComponent_.toStdString() + "_" + event.toStdString());
    const ir::ApplicationModel* model = controller_->model();
    std::string id = base;
    int counter = 2;
    while (model->findAction(id) != nullptr)
        id = base + "_" + std::to_string(counter++);
    action["id"] = id;
    if (index == 0) {
        QStringList screens;
        for (const auto& screen : model->screens)
            screens << toQ(screen.root.id);
        QString target = QInputDialog::getItem(this, tr("New action"), tr("Screen"), screens, 0, false, &accepted);
        if (!accepted)
            return {};
        action["target"] = fromQ(target);
    } else if (index >= 2 && index <= 4) {
        QStringList names;
        for (const auto& variable : model->state)
            names << toQ(variable.name);
        QString target = QInputDialog::getItem(this, tr("New action"), tr("State value (type a new name to create one)"), names, 0, true, &accepted);
        if (!accepted || target.isEmpty())
            return {};
        action["target"] = fromQ(target);
        if (model->findState(fromQ(target)) == nullptr) {
            Json variables = ir::applicationToJson(*model).value("state", Json::array());
            std::string type = index == 3 ? "bool" : index == 4 ? "int" : "string";
            Json initial = index == 3 ? Json(false) : index == 4 ? Json(0) : Json("");
            variables.push_back(Json{{"name", fromQ(target)}, {"type", type}, {"initial", initial}});
            auto status = controller_->editor()->setSection("state", variables);
            (void)status;
        }
        if (index == 2) {
            QString value = QInputDialog::getText(this, tr("New action"), tr("Value"), QLineEdit::Normal, QString(), &accepted);
            if (!accepted)
                return {};
            action["value"] = fromQ(value);
        } else if (index == 4) {
            action["value"] = 1;
        }
    } else if (index == 5) {
        QString url = QInputDialog::getText(this, tr("New action"), tr("Web address"), QLineEdit::Normal, QStringLiteral("https://"), &accepted);
        if (!accepted)
            return {};
        action["target"] = fromQ(url);
    } else if (index == 6) {
        action["target"] = id;
    }
    Json actions = ir::applicationToJson(*model).value("actions", Json::array());
    actions.push_back(action);
    auto status = controller_->editor()->setSection("actions", actions);
    if (!status) {
        controller_->reportStatus(toQ(status.error().message));
        return {};
    }
    return toQ(id);
}

QString InspectorPanel::actionForScreen(const QString& screen)
{
    const ir::ApplicationModel* model = controller_->model();
    for (const auto& action : model->actions) {
        if (action.kind == "navigate" && toQ(action.target) == screen)
            return toQ(action.id);
    }
    std::string base = "go_to_" + text::toSnakeCase(fromQ(screen));
    std::string id = base;
    int counter = 2;
    while (model->findAction(id) != nullptr)
        id = base + "_" + std::to_string(counter++);
    Json actions = ir::applicationToJson(*model).value("actions", Json::array());
    actions.push_back(Json{{"id", id}, {"kind", "navigate"}, {"target", fromQ(screen)}});
    auto status = controller_->editor()->setSection("actions", actions);
    if (!status) {
        controller_->reportStatus(toQ(status.error().message));
        return {};
    }
    return toQ(id);
}

void InspectorPanel::addEventRow(QFormLayout* form, const QString& event)
{
    auto* combo = new QComboBox(content_);
    combo->setObjectName(QStringLiteral("event_") + event);
    combo->addItem(tr("Nothing"), QString());
    const ir::ApplicationModel* model = controller_->model();
    std::set<QString> linked;
    for (const auto& action : model->actions) {
        QString id = toQ(action.id);
        if (action.kind == "navigate") {
            const ir::Screen* target = model->findScreen(action.target);
            if (target == nullptr || !linked.insert(toQ(action.target)).second)
                continue;
            combo->addItem(tr("Go to %1").arg(toQ(target->root.name.empty() ? target->root.id : target->root.name)), id);
        } else if (action.kind == "back") {
            combo->addItem(tr("Go back"), id);
        } else {
            combo->addItem(friendlyChoice(id), id);
        }
    }
    for (const auto& screen : model->screens) {
        if (screen.root.id == fromQ(controller_->currentScreen()) || linked.count(toQ(screen.root.id)) > 0)
            continue;
        combo->addItem(tr("Go to %1").arg(toQ(screen.root.name.empty() ? screen.root.id : screen.root.name)), QStringLiteral("@goto:") + toQ(screen.root.id));
    }
    combo->addItem(tr("Something else..."), QStringLiteral("@new"));
    form->addRow(friendlyEvent(event), combo);
    std::string key = "events." + fromQ(event);
    connect(combo, &QComboBox::currentIndexChanged, this, [this, key, combo, event] {
        if (applying_)
            return;
        QString value = combo->currentData().toString();
        if (value == QStringLiteral("@new")) {
            value = newActionFor(event);
            if (value.isEmpty()) {
                refreshValues();
                return;
            }
        } else if (value.startsWith(QStringLiteral("@goto:"))) {
            value = actionForScreen(value.mid(6));
            if (value.isEmpty()) {
                refreshValues();
                return;
            }
        }
        setValue(key, value.isEmpty() ? Json(nullptr) : Json(fromQ(value)));
        QMetaObject::invokeMethod(this, &InspectorPanel::rebuild, Qt::QueuedConnection);
    });
    refreshers_.push_back([this, key, combo] {
        Json value = currentValue(key);
        int index = value.is_string() ? combo->findData(toQ(value.get<std::string>())) : 0;
        combo->setCurrentIndex(std::max(0, index));
    });
}

void InspectorPanel::addBindingRow(QFormLayout* form, const QString& label, const std::string& key)
{
    auto* combo = new QComboBox(content_);
    combo->addItem(tr("Not bound"), QString());
    for (const auto& variable : controller_->model()->state)
        combo->addItem(toQ(variable.name), toQ(variable.name));
    form->addRow(label, combo);
    std::string bindingKey = "bindings." + key;
    connect(combo, &QComboBox::currentIndexChanged, this, [this, combo, key] {
        if (applying_)
            return;
        QString value = combo->currentData().toString();
        const ir::Component* component = selected();
        if (component == nullptr)
            return;
        Json bindings = ir::getComponentField(*component, "bindings");
        if (!bindings.is_object())
            bindings = Json::object();
        if (value.isEmpty())
            bindings.erase(key);
        else
            bindings[key] = fromQ(value);
        setValue("bindings", bindings.empty() ? Json(nullptr) : bindings);
    });
    refreshers_.push_back([this, combo, key] {
        const ir::Component* component = selected();
        if (component == nullptr)
            return;
        auto it = component->bindings.find(key);
        combo->setCurrentIndex(it == component->bindings.end() ? 0 : std::max(0, combo->findData(toQ(it->second.source))));
    });
}

void InspectorPanel::addJsonRow(QFormLayout* form, const QString& label, const std::string& key)
{
    auto* edit = new QPlainTextEdit(content_);
    edit->setObjectName(toQ("field_" + key));
    edit->setFixedHeight(64);
    edit->setFont(Theme::instance().codeFont());
    edit->setPlaceholderText(QStringLiteral("{ }"));
    form->addRow(label, edit);
    auto* apply = new QPushButton(tr("Apply"), content_);
    form->addRow(QString(), apply);
    connect(apply, &QPushButton::clicked, this, [this, key, edit] {
        QString value = edit->toPlainText().trimmed();
        if (value.isEmpty()) {
            setValue(key, nullptr);
            return;
        }
        auto parsed = parseJson(fromQ(value));
        if (!parsed) {
            controller_->reportStatus(tr("That is not valid JSON: %1").arg(toQ(parsed.error().message)));
            return;
        }
        setValue(key, *parsed);
    });
    refreshers_.push_back([this, key, edit] {
        Json value = currentValue(key);
        QString text = value.is_null() ? QString() : toQ(dumpJsonCompact(value));
        if (!edit->hasFocus() && edit->toPlainText() != text)
            edit->setPlainText(text);
    });
}

void InspectorPanel::buildNothing()
{
    auto* holder = new QWidget(content_);
    auto* layout = new QVBoxLayout(holder);
    layout->setContentsMargins(12, 12, 12, 12);
    auto* label = new QLabel(controller_->hasProject() ? tr("Select a layer on the canvas to edit it. Click a screen's name to edit the screen.") : tr("No project is open."), holder);
    label->setProperty("role", "muted");
    label->setWordWrap(true);
    layout->addWidget(label);
    contentLayout_->addWidget(holder);
}

void InspectorPanel::buildMulti()
{
    auto* body = section(tr("%n selected", nullptr, static_cast<int>(controller_->selection().size())), QStringLiteral("multi"));
    auto* align = new QHBoxLayout();
    align->setSpacing(2);
    const std::vector<std::tuple<QString, QString, design::AlignEdge>> edges = {
        {QStringLiteral("align-left"), tr("Align left edges"), design::AlignEdge::Left},
        {QStringLiteral("align-center-x"), tr("Align centers across"), design::AlignEdge::CenterX},
        {QStringLiteral("align-right"), tr("Align right edges"), design::AlignEdge::Right},
        {QStringLiteral("align-top"), tr("Align top edges"), design::AlignEdge::Top},
        {QStringLiteral("align-center-y"), tr("Align centers down"), design::AlignEdge::CenterY},
        {QStringLiteral("align-bottom"), tr("Align bottom edges"), design::AlignEdge::Bottom},
    };
    for (const auto& [name, tip, edge] : edges) {
        auto* button = iconButton(name, tip, content_);
        int value = static_cast<int>(edge);
        connect(button, &QToolButton::clicked, this, [this, value] { Q_EMIT alignRequested(value); });
        align->addWidget(button);
    }
    auto* across = iconButton(QStringLiteral("distribute-h"), tr("Space evenly across"), content_);
    connect(across, &QToolButton::clicked, this, [this] { Q_EMIT distributeRequested(static_cast<int>(design::DistributeAxis::Horizontal)); });
    align->addWidget(across);
    auto* down = iconButton(QStringLiteral("distribute-v"), tr("Space evenly down"), content_);
    connect(down, &QToolButton::clicked, this, [this] { Q_EMIT distributeRequested(static_cast<int>(design::DistributeAxis::Vertical)); });
    align->addWidget(down);
    align->addStretch(1);
    body->addLayout(align);
    QFormLayout* form = formSection(tr("Fill and border"), QStringLiteral("appearance"));
    addColorRow(form, tr("Fill"), "style.background");
    addNumberRow(form, tr("Radius"), "style.radius", 0, 10000);
    addNumberRow(form, tr("Opacity"), "style.opacity", 0, 1, 1);
}

void InspectorPanel::buildPrototype(const ir::Component* component)
{
    if (component == nullptr) {
        auto* holder = new QWidget(content_);
        auto* layout = new QVBoxLayout(holder);
        layout->setContentsMargins(12, 12, 12, 12);
        auto* label = new QLabel(controller_->hasProject() ? tr("Select a button or other control, then choose what happens when someone uses it. Lines on the canvas show how screens connect.") : tr("No project is open."), holder);
        label->setObjectName(QStringLiteral("prototypeHint"));
        label->setProperty("role", "muted");
        label->setWordWrap(true);
        layout->addWidget(label);
        contentLayout_->addWidget(holder);
        return;
    }
    const ir::ComponentSchema* schema = AppContext::instance().components().find(component->type);
    if (component->type == "screen") {
        auto* body = section(tr("Screen"), QStringLiteral("flowScreen"));
        bool isStart = controller_->model()->navigation.start == component->id;
        auto* start = new QPushButton(isStart ? tr("This is the start screen") : tr("Make this the start screen"), content_);
        start->setObjectName(QStringLiteral("prototypeStart"));
        start->setEnabled(!isStart);
        std::string id = component->id;
        connect(start, &QPushButton::clicked, this, [this, id] {
            ir::ModelEditor* editor = controller_->editor();
            Json navigation = ir::applicationToJson(editor->model())["navigation"];
            navigation["start"] = id;
            auto status = editor->setSection("navigation", navigation);
            if (!status)
                controller_->reportStatus(toQ(status.error().message));
        });
        body->addWidget(start);
    }
    if (schema != nullptr && !schema->events.empty()) {
        QFormLayout* events = formSection(tr("When someone uses it"), QStringLiteral("events"));
        for (const auto& event : schema->events)
            addEventRow(events, toQ(event));
    }
    std::string bindKey = component->type == "text" ? "properties.content" : (component->type == "button" || component->type == "checkbox" || component->type == "switch") ? "properties.label" : component->type == "textinput" ? "properties.value" : "";
    if (!bindKey.empty() || component->type == "list") {
        QFormLayout* data = formSection(tr("Data"), QStringLiteral("data"), !component->bindings.empty());
        if (!bindKey.empty())
            addBindingRow(data, tr("Show value of"), bindKey);
        if (component->type == "checkbox" || component->type == "switch")
            addBindingRow(data, tr("Checked follows"), "properties.checked");
        if (component->type == "list")
            addBindingRow(data, tr("Items from"), "properties.items");
        addBindingRow(data, tr("Visible follows"), "visible");
    }
}

void InspectorPanel::buildComponent(const ir::Component& component, const ir::Component* parent)
{
    const ir::ComponentSchema* schema = AppContext::instance().components().find(component.type);
    bool isRoot = component.type == "screen";
    bool isContainer = isRoot || (schema != nullptr && schema->container);

    auto* top = new QWidget(content_);
    auto* topLayout = new QHBoxLayout(top);
    topLayout->setContentsMargins(10, 8, 6, 4);
    auto* typeIcon = new QLabel(top);
    typeIcon->setPixmap(Theme::instance().pixmap(schema != nullptr ? toQ(schema->icon) : QStringLiteral("device"), 16, QColor(), devicePixelRatioF()));
    topLayout->addWidget(typeIcon);
    auto* typeLabel = new QLabel(schema != nullptr ? toQ(schema->label) : tr("Screen"), top);
    typeLabel->setProperty("role", "subheading");
    topLayout->addWidget(typeLabel, 1);
    auto* code = iconButton(QStringLiteral("code"), tr("Go to the code for this layer"), top);
    code->setObjectName(QStringLiteral("goToCodeButton"));
    connect(code, &QToolButton::clicked, this, [this] { Q_EMIT openCodeRequested(controller_->currentScreen(), shownComponent_); });
    topLayout->addWidget(code);
    contentLayout_->addWidget(top);

    auto* nameHolder = new QWidget(content_);
    auto* nameForm = new QFormLayout(nameHolder);
    nameForm->setContentsMargins(10, 0, 8, 8);
    nameForm->setHorizontalSpacing(8);
    nameForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    addTextRow(nameForm, tr("Name"), "name");
    contentLayout_->addWidget(nameHolder);

    if (!component.codeManaged.empty()) {
        auto* banner = new QFrame(content_);
        banner->setProperty("role", "bannerWarning");
        auto* bannerLayout = new QHBoxLayout(banner);
        bannerLayout->setContentsMargins(10, 6, 6, 6);
        auto* note = new QLabel(tr("Part of this layer is managed in code. Your code is kept, and the canvas may not show everything it does."), banner);
        note->setWordWrap(true);
        bannerLayout->addWidget(note, 1);
        auto* clear = new QPushButton(tr("Dismiss"), banner);
        connect(clear, &QPushButton::clicked, this, [this] { setValue("codeManaged", nullptr); rebuild(); });
        bannerLayout->addWidget(clear);
        contentLayout_->addWidget(banner);
    }

    if (!component.instanceOf.empty()) {
        auto* body = section(tr("Reusable component"), QStringLiteral("instance"));
        auto* note = new QLabel(tr("An instance of %1.").arg(toQ(component.instanceOf)), content_);
        note->setProperty("role", "muted");
        body->addWidget(note);
        auto* detach = new QPushButton(tr("Detach from component"), content_);
        connect(detach, &QPushButton::clicked, this, [this] {
            design::DesignOperations operations(*controller_->editor());
            auto status = operations.detachInstance(fromQ(controller_->currentScreen()), fromQ(shownComponent_));
            if (!status)
                controller_->reportStatus(toQ(status.error().message));
        });
        body->addWidget(detach);
    }

    if (schema != nullptr && !schema->properties.empty()) {
        std::map<std::string, QFormLayout*> forms;
        for (const auto& property : schema->properties) {
            QFormLayout*& form = forms[property.group];
            if (form == nullptr)
                form = formSection(property.group == "Content" ? tr("Content") : tr("Behavior"), toQ(property.group));
            std::string key = "properties." + property.key;
            QString label = toQ(property.label);
            switch (property.type) {
            case ir::PropertyType::Text:
                addTextRow(form, label, key);
                break;
            case ir::PropertyType::MultilineText:
                addTextRow(form, label, key, true);
                break;
            case ir::PropertyType::Number:
                addNumberRow(form, label, key, -100000, 100000, property.defaultValue.is_number() ? -999999 : 0);
                break;
            case ir::PropertyType::Boolean:
                addBoolRow(form, label, key, false);
                break;
            case ir::PropertyType::Color:
                addColorRow(form, label, key);
                break;
            case ir::PropertyType::Enum: {
                QStringList options;
                for (const auto& option : property.options)
                    options << toQ(option);
                addChoiceRow(form, label, key, options, QString());
                break;
            }
            case ir::PropertyType::StringList:
                addListRow(form, label, key);
                break;
            case ir::PropertyType::Asset:
                addAssetRow(form, label, key);
                break;
            case ir::PropertyType::Json:
                addJsonRow(form, label, key);
                break;
            }
        }
    }

    QFormLayout* layoutForm = formSection(isRoot ? tr("Size") : tr("Position and size"), QStringLiteral("layout"));
    if (!isRoot) {
        bool freeParent = parent != nullptr && parent->layout.kind == ir::LayoutKind::Absolute;
        auto* positions = new QGridLayout();
        positions->setContentsMargins(0, 0, 0, 0);
        positions->setHorizontalSpacing(6);
        positions->setColumnStretch(1, 1);
        positions->setColumnStretch(3, 1);
        if (freeParent) {
            addNumberCell(positions, 0, 0, QStringLiteral("X"), "x");
            addNumberCell(positions, 0, 2, QStringLiteral("Y"), "y");
        } else {
            addFrameCell(positions, 0, 0, QStringLiteral("X"), true);
            addFrameCell(positions, 0, 2, QStringLiteral("Y"), false);
        }
        layoutForm->addRow(positions);
        if (parent != nullptr && parent->layout.kind == ir::LayoutKind::Constraint) {
            addNumberRow(layoutForm, tr("Pin left"), "pins.left", -100000, 100000, -999999);
            addNumberRow(layoutForm, tr("Pin right"), "pins.right", -100000, 100000, -999999);
            addNumberRow(layoutForm, tr("Pin top"), "pins.top", -100000, 100000, -999999);
            addNumberRow(layoutForm, tr("Pin bottom"), "pins.bottom", -100000, 100000, -999999);
        }
        addSizeRow(layoutForm, QStringLiteral("W"), "width");
        addSizeRow(layoutForm, QStringLiteral("H"), "height");
        addInsetsRow(layoutForm, tr("Margin"), "margin");
        if (parent != nullptr && parent->layout.kind != ir::LayoutKind::Absolute && parent->layout.kind != ir::LayoutKind::Constraint) {
            addChoiceRow(layoutForm, tr("Align self"), "alignSelf", {QStringLiteral(""), QStringLiteral("start"), QStringLiteral("center"), QStringLiteral("end"), QStringLiteral("stretch")}, QString());
            addNumberRow(layoutForm, tr("Grow"), "grow", 0, 1000);
        }
        if (parent != nullptr && parent->layout.kind == ir::LayoutKind::Grid) {
            addNumberRow(layoutForm, tr("Column"), "grid.column", 0, 1000);
            addNumberRow(layoutForm, tr("Row"), "grid.row", 0, 1000);
            addNumberRow(layoutForm, tr("Column span"), "grid.columnSpan", 1, 1000, 1);
            addNumberRow(layoutForm, tr("Row span"), "grid.rowSpan", 1, 1000, 1);
        }
    } else {
        addDeviceRow(layoutForm);
        addNumberRow(layoutForm, QStringLiteral("W"), "width", 1, 100000);
        addNumberRow(layoutForm, QStringLiteral("H"), "height", 1, 100000);
    }

    if (isContainer) {
        QFormLayout* children = formSection(tr("Auto layout"), QStringLiteral("children"));
        addChoiceRow(children, tr("Arrange"), "layout.type", {QStringLiteral("vertical"), QStringLiteral("horizontal"), QStringLiteral("grid"), QStringLiteral("flex"), QStringLiteral("scroll"), QStringLiteral("absolute"), QStringLiteral("constraint")}, QStringLiteral("absolute"));
        addNumberRow(children, tr("Gap"), "layout.spacing", 0, 10000);
        addInsetsRow(children, tr("Padding"), "layout.padding");
        addChoiceRow(children, tr("Align"), "layout.align", {QStringLiteral("start"), QStringLiteral("center"), QStringLiteral("end"), QStringLiteral("stretch")}, QStringLiteral("start"));
        addChoiceRow(children, tr("Distribute"), "layout.justify", {QStringLiteral("start"), QStringLiteral("center"), QStringLiteral("end"), QStringLiteral("spaceBetween"), QStringLiteral("spaceAround"), QStringLiteral("spaceEvenly")}, QStringLiteral("start"));
        if (component.layout.kind == ir::LayoutKind::Flex)
            addBoolRow(children, tr("Wrap"), "layout.wrap");
        if (component.layout.kind == ir::LayoutKind::Grid)
            addJsonRow(children, tr("Columns"), "layout.columns");
    }

    QFormLayout* appearance = formSection(tr("Fill and border"), QStringLiteral("appearance"));
    addColorRow(appearance, tr("Fill"), "style.background");
    addColorRow(appearance, tr("Border"), "style.borderColor");
    addNumberRow(appearance, tr("Width"), "style.borderWidth", 0, 1000);
    addNumberRow(appearance, tr("Radius"), "style.radius", 0, 10000);
    addNumberRow(appearance, tr("Opacity"), "style.opacity", 0, 1, 1);

    QFormLayout* typography = formSection(tr("Typography"), QStringLiteral("typography"), component.type == "text" || component.type == "button");
    QStringList styles = {QString()};
    if (const ir::Theme* theme = controller_->model()->theme()) {
        for (const auto& [name, style] : theme->tokens.typography)
            styles << toQ(name);
    }
    addChoiceRow(typography, tr("Style"), "typography.ref", styles, QString());
    addTextRow(typography, tr("Font"), "typography.font");
    addNumberRow(typography, tr("Size"), "typography.size", 0, 1000);
    addNumberRow(typography, tr("Weight"), "typography.weight", 0, 900);
    addColorRow(typography, tr("Color"), "typography.color");
    addChoiceRow(typography, tr("Align"), "typography.align", {QString(), QStringLiteral("left"), QStringLiteral("center"), QStringLiteral("right")}, QString());
    addBoolRow(typography, tr("Italic"), "typography.italic");

    QFormLayout* advanced = formSection(tr("Advanced"), QStringLiteral("advanced"), false);
    auto* idLabel = new QLineEdit(toQ(component.id), content_);
    idLabel->setReadOnly(true);
    idLabel->setObjectName(QStringLiteral("field_id"));
    advanced->addRow(tr("Identifier"), idLabel);
    addJsonRow(advanced, tr("Shadow"), "style.shadow");
    for (const auto& breakpoint : controller_->model()->breakpoints)
        addJsonRow(advanced, toQ(breakpoint.name), "variants." + breakpoint.id);
    for (const char* name : {"ios", "android", "macos", "windows", "linux", "web"})
        addJsonRow(advanced, QString::fromLatin1(name), std::string("platform.") + name);
}

}
