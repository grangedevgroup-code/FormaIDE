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
#include <QToolButton>
#include <QVBoxLayout>

#include "app/app_context.h"
#include "canvas/canvas_widget.h"
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

QIcon iconForType(const std::string& type)
{
    const ir::ComponentSchema* schema = AppContext::instance().components().find(type);
    if (schema != nullptr)
        return icon(toQ(schema->icon));
    if (type == "screen")
        return icon(QStringLiteral("screens"));
    return icon(QStringLiteral("custom"));
}

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
    double width = 393;
    double height = 852;
    double right = 0;
    const auto& screens = controller_->model()->screens;
    if (!screens.empty()) {
        const auto& first = screens.front();
        if (first.root.width.mode == ir::SizeMode::Fixed)
            width = first.root.width.value;
        if (first.root.height.mode == ir::SizeMode::Fixed)
            height = first.root.height.value;
    }
    for (const auto& screen : screens) {
        double w = screen.root.width.mode == ir::SizeMode::Fixed ? screen.root.width.value : width;
        right = std::max(right, screen.canvas.x + w + 120);
    }
    ir::Screen screen = ir::makeScreen(controller_->editor()->uniqueScreenId(fromQ(name)), fromQ(name.trimmed()), width, height);
    screen.canvas.x = right;
    auto added = controller_->editor()->addScreen(screen);
    if (added)
        controller_->setCurrentScreen(toQ(*added));
    else
        controller_->reportStatus(toQ(added.error().message));
}

void ScreensPanel::showMenu(const QPoint& position)
{
    QListWidgetItem* item = list_->itemAt(position);
    if (item == nullptr)
        return;
    QString id = item->data(Qt::UserRole).toString();
    ir::ModelEditor* editor = controller_->editor();
    QMenu menu(this);
    menu.addAction(tr("Set as start screen"), this, [this, id, editor] {
        Json navigation = ir::applicationToJson(editor->model())["navigation"];
        navigation["start"] = fromQ(id);
        auto status = editor->setSection("navigation", navigation);
        if (!status)
            controller_->reportStatus(toQ(status.error().message));
    });
    menu.addAction(tr("Rename..."), this, [this, id, editor] {
        bool accepted = false;
        const ir::Screen* screen = editor->model().findScreen(fromQ(id));
        QString name = QInputDialog::getText(this, tr("Rename screen"), tr("Name"), QLineEdit::Normal, toQ(screen->root.name), &accepted);
        if (accepted && !name.trimmed().isEmpty()) {
            auto status = editor->setField(fromQ(id), fromQ(id), "name", fromQ(name.trimmed()));
            (void)status;
        }
    });
    auto* sizes = menu.addMenu(tr("Device size"));
    for (const auto& preset : design::devicePresets()) {
        sizes->addAction(toQ(preset.name) + QStringLiteral("  %1 x %2").arg(preset.width).arg(preset.height), this, [this, id, editor, preset] {
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
    menu.addAction(tr("Delete"), this, [this, id, editor] {
        if (QMessageBox::question(this, tr("Delete screen"), tr("Delete this screen? You can undo this.")) != QMessageBox::Yes)
            return;
        auto status = editor->removeScreen(fromQ(id));
        if (!status) {
            controller_->reportStatus(toQ(status.error().message));
            return;
        }
        const auto& screens = editor->model().screens;
        controller_->setCurrentScreen(screens.empty() ? QString() : toQ(screens.front().root.id));
    });
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
    filter_->setPlaceholderText(tr("Filter layers"));
    filter_->setClearButtonEnabled(true);
    auto* filterHolder = new QHBoxLayout();
    filterHolder->setContentsMargins(6, 4, 6, 4);
    filterHolder->addWidget(filter_);
    layout->addLayout(filterHolder);
    tree_ = new LayerTree(controller_, this);
    tree_->setObjectName(QStringLiteral("layersTree"));
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(tree_, 1);
    connect(filter_, &QLineEdit::textChanged, this, &LayersPanel::refresh);
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (updating_)
            return;
        QStringList ids;
        for (auto* item : tree_->selectedItems())
            ids << item->data(0, Qt::UserRole).toString();
        updating_ = true;
        controller_->setSelection(ids);
        updating_ = false;
    });
    connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int column) {
        ir::ModelEditor* editor = controller_->editor();
        if (editor == nullptr)
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
    connect(controller_, &ProjectController::screenChanged, this, &LayersPanel::refresh);
    connect(controller_, &ProjectController::modelChanged, this, &LayersPanel::refresh);
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
    const ir::Component* root = rootFor(controller_);
    QString needle = filter_->text().trimmed();
    if (root != nullptr) {
        std::function<bool(const ir::Component&, QTreeWidgetItem*)> addNode = [&](const ir::Component& node, QTreeWidgetItem* parent) -> bool {
            QString name = toQ(node.name.empty() ? node.id : node.name);
            auto* item = parent != nullptr ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree_);
            item->setText(0, name);
            item->setIcon(0, iconForType(node.type));
            item->setData(0, Qt::UserRole, toQ(node.id));
            item->setToolTip(0, toQ(node.type) + QStringLiteral(" - ") + toQ(node.id) + (node.codeManaged.empty() ? QString() : tr("\nPartly managed in code")));
            if (&node != root) {
                item->setIcon(1, icon(node.visible ? QStringLiteral("eye") : QStringLiteral("eye-off")));
                item->setToolTip(1, node.visible ? tr("Hide") : tr("Show"));
                if (node.locked)
                    item->setIcon(2, icon(QStringLiteral("lock")));
                else
                    item->setIcon(2, Theme::instance().icon(QStringLiteral("unlock"), colors().textFaint));
                item->setToolTip(2, node.locked ? tr("Unlock") : tr("Lock"));
            } else {
                item->setFlags(item->flags() & ~Qt::ItemIsDragEnabled);
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
        tree_->expandAll();
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
    QTreeWidgetItemIterator it(tree_);
    while (*it) {
        if (ids.contains((*it)->data(0, Qt::UserRole).toString())) {
            (*it)->setSelected(true);
            tree_->scrollToItem(*it);
        }
        ++it;
    }
    updating_ = false;
}

class PaletteTree : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;

protected:
    QStringList mimeTypes() const override { return {QString::fromLatin1(kComponentMimeType)}; }
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override
    {
        if (items.isEmpty())
            return nullptr;
        QString type = items.first()->data(0, Qt::UserRole).toString();
        if (type.isEmpty())
            return nullptr;
        auto* data = new QMimeData();
        data->setData(QString::fromLatin1(kComponentMimeType), type.toUtf8());
        return data;
    }
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
    holder->setContentsMargins(6, 4, 6, 4);
    holder->addWidget(search_);
    layout->addLayout(holder);
    tree_ = new PaletteTree(this);
    tree_->setObjectName(QStringLiteral("paletteTree"));
    tree_->setHeaderHidden(true);
    tree_->setIndentation(0);
    tree_->setDragEnabled(true);
    tree_->setDragDropMode(QAbstractItemView::DragOnly);
    tree_->setRootIsDecorated(false);
    tree_->setToolTip(tr("Drag a component onto the canvas, or double-click to add it to the selection"));
    layout->addWidget(tree_, 1);
    connect(search_, &QLineEdit::textChanged, this, &PalettePanel::refresh);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        QString type = item->data(0, Qt::UserRole).toString();
        if (!type.isEmpty())
            insertIntoSelection(type);
    });
    connect(controller_, &ProjectController::modelChanged, this, [this](const ir::ChangeEvent& event) {
        if (event.key == "components" || event.kind == ir::ChangeKind::Reset)
            refresh();
    });
    connect(controller_, &ProjectController::projectOpened, this, &PalettePanel::refresh);
    refresh();
}

void PalettePanel::refresh()
{
    tree_->clear();
    QString needle = search_->text().trimmed();
    const auto& catalog = AppContext::instance().components();
    QFont headerFont = font();
    headerFont.setWeight(QFont::DemiBold);
    if (controller_->hasProject() && !controller_->model()->components.empty()) {
        auto* category = new QTreeWidgetItem(tree_);
        category->setText(0, tr("Reusable"));
        category->setFont(0, headerFont);
        category->setFlags(Qt::ItemIsEnabled);
        for (const auto& definition : controller_->model()->components) {
            auto* item = new QTreeWidgetItem(category);
            item->setText(0, toQ(definition.root.name));
            item->setIcon(0, icon(QStringLiteral("components")));
            item->setData(0, Qt::UserRole, QStringLiteral("instance:") + toQ(definition.root.id));
        }
    }
    for (const auto& name : catalog.categories()) {
        QTreeWidgetItem* category = nullptr;
        for (const auto& schema : catalog.all()) {
            if (schema.category != name)
                continue;
            QString label = toQ(schema.label);
            if (!needle.isEmpty() && !label.contains(needle, Qt::CaseInsensitive))
                continue;
            if (category == nullptr) {
                category = new QTreeWidgetItem(tree_);
                category->setText(0, toQ(name));
                category->setFont(0, headerFont);
                category->setFlags(Qt::ItemIsEnabled);
            }
            auto* item = new QTreeWidgetItem(category);
            item->setText(0, label);
            item->setIcon(0, icon(toQ(schema.icon)));
            item->setData(0, Qt::UserRole, toQ(schema.type));
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
        }
    }
    tree_->expandAll();
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
    scroll_ = new QScrollArea(this);
    scroll_->setObjectName(QStringLiteral("inspectorScroll"));
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(scroll_);
    connect(controller_, &ProjectController::selectionChanged, this, &InspectorPanel::rebuild);
    connect(controller_, &ProjectController::screenChanged, this, &InspectorPanel::rebuild);
    connect(controller_, &ProjectController::projectOpened, this, &InspectorPanel::rebuild);
    connect(controller_, &ProjectController::projectClosed, this, &InspectorPanel::rebuild);
    connect(controller_, &ProjectController::modelChanged, this, [this](const ir::ChangeEvent& event) {
        if (applying_)
            return;
        if (event.kind == ir::ChangeKind::ComponentChanged && toQ(event.component) == shownComponent_ && event.key != "layout.type" && event.key != "instanceOf")
            refreshValues();
        else
            rebuild();
    });
    openSections_ = {{QStringLiteral("platform"), false}, {QStringLiteral("variants"), false}, {QStringLiteral("advanced"), false}};
    rebuild();
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
    content_ = new QWidget();
    content_->setObjectName(QStringLiteral("inspectorContent"));
    contentLayout_ = new QVBoxLayout(content_);
    contentLayout_->setContentsMargins(0, 0, 0, 0);
    contentLayout_->setSpacing(0);
    shownComponent_.clear();
    const ir::Component* root = rootFor(controller_);
    const ir::Component* component = selected();
    if (component != nullptr) {
        shownComponent_ = toQ(component->id);
        buildComponent(*component, ir::findParent(*root, component->id));
    } else if (controller_->selection().size() > 1) {
        buildMulti();
    } else {
        buildNothing();
    }
    contentLayout_->addStretch(1);
    scroll_->setWidget(content_);
    refreshValues();
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
    header->setStyleSheet(QStringLiteral("QToolButton { font-weight: 600; text-align: left; padding: 5px 6px; border-radius: 0; border-top: 1px solid %1; }").arg(colors().border.name()));
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

void InspectorPanel::addNumberRow(QFormLayout* form, const QString& label, const std::string& key, double minimum, double maximum, double fallback)
{
    auto* spin = new QDoubleSpinBox(content_);
    spin->setObjectName(toQ("field_" + key));
    spin->setRange(minimum, maximum);
    spin->setDecimals(2);
    spin->setKeyboardTracking(false);
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    form->addRow(label, spin);
    connect(spin, &QDoubleSpinBox::valueChanged, this, [this, key, fallback](double value) { setValue(key, std::abs(value - fallback) < 1e-9 ? Json(nullptr) : Json(value)); });
    refreshers_.push_back([this, key, spin, fallback] {
        Json value = currentValue(key);
        double number = value.is_number() ? value.get<double>() : fallback;
        if (spin->value() != number)
            spin->setValue(number);
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
    combo->addItems(options);
    form->addRow(label, combo);
    connect(combo, &QComboBox::currentTextChanged, this, [this, key, fallback](const QString& value) { setValue(key, value == fallback ? Json(nullptr) : Json(fromQ(value))); });
    refreshers_.push_back([this, key, combo, fallback] {
        Json value = currentValue(key);
        QString text = value.is_string() ? toQ(value.get<std::string>()) : fallback;
        if (combo->currentText() != text)
            combo->setCurrentText(text);
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
    mode->addItem(tr("Hug content"), QStringLiteral("hug"));
    mode->addItem(tr("Fill"), QStringLiteral("fill"));
    mode->addItem(tr("Fixed"), QStringLiteral("fixed"));
    mode->addItem(tr("Percent"), QStringLiteral("percent"));
    auto* value = new QDoubleSpinBox(holder);
    value->setObjectName(toQ("field_" + key));
    value->setRange(0, 100000);
    value->setDecimals(0);
    value->setKeyboardTracking(false);
    value->setButtonSymbols(QAbstractSpinBox::NoButtons);
    value->setMaximumWidth(64);
    row->addWidget(mode, 1);
    row->addWidget(value);
    form->addRow(label, holder);
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
        } else if (const layout::LayoutResult* result = nullptr; result == nullptr) {
            value->setValue(0);
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
        spin->setRange(0, 10000);
        spin->setDecimals(0);
        spin->setKeyboardTracking(false);
        spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
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

void InspectorPanel::addEventRow(QFormLayout* form, const QString& event)
{
    auto* combo = new QComboBox(content_);
    combo->setObjectName(QStringLiteral("event_") + event);
    combo->addItem(tr("Nothing"), QString());
    for (const auto& action : controller_->model()->actions) {
        QString label = toQ(action.id);
        combo->addItem(label, label);
    }
    combo->addItem(tr("New action..."), QStringLiteral("@new"));
    form->addRow(event, combo);
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
        }
        setValue(key, value.isEmpty() ? Json(nullptr) : Json(fromQ(value)));
        rebuild();
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
    auto* label = new QLabel(controller_->hasProject() ? tr("Select a component on the canvas to edit it.") : tr("No project is open."), holder);
    label->setProperty("role", "muted");
    label->setWordWrap(true);
    layout->addWidget(label);
    contentLayout_->addWidget(holder);
}

void InspectorPanel::buildMulti()
{
    auto* body = section(tr("%n selected", nullptr, static_cast<int>(controller_->selection().size())), QStringLiteral("multi"));
    auto* label = new QLabel(tr("Use the toolbar to align or distribute them. Values set here apply to all."), content_);
    label->setProperty("role", "muted");
    label->setWordWrap(true);
    body->addWidget(label);
    QFormLayout* form = formSection(tr("Appearance"), QStringLiteral("appearance"));
    addColorRow(form, tr("Background"), "style.background");
    addNumberRow(form, tr("Radius"), "style.radius", 0, 10000);
    addNumberRow(form, tr("Opacity"), "style.opacity", 0, 1, 1);
}

void InspectorPanel::buildComponent(const ir::Component& component, const ir::Component* parent)
{
    const ir::ComponentSchema* schema = AppContext::instance().components().find(component.type);
    bool isRoot = component.type == "screen";
    bool isContainer = isRoot || (schema != nullptr && schema->container);

    auto* top = new QWidget(content_);
    auto* topLayout = new QHBoxLayout(top);
    topLayout->setContentsMargins(10, 8, 6, 8);
    auto* typeIcon = new QLabel(top);
    typeIcon->setPixmap(Theme::instance().pixmap(schema != nullptr ? toQ(schema->icon) : QStringLiteral("screens"), 16, QColor(), devicePixelRatioF()));
    topLayout->addWidget(typeIcon);
    auto* typeLabel = new QLabel(schema != nullptr ? toQ(schema->label) : tr("Screen"), top);
    typeLabel->setProperty("role", "subheading");
    topLayout->addWidget(typeLabel, 1);
    auto* code = iconButton(QStringLiteral("code"), tr("Go to the code for this component"), top);
    code->setObjectName(QStringLiteral("goToCodeButton"));
    connect(code, &QToolButton::clicked, this, [this] { Q_EMIT openCodeRequested(controller_->currentScreen(), shownComponent_); });
    topLayout->addWidget(code);
    contentLayout_->addWidget(top);

    if (!component.codeManaged.empty()) {
        auto* banner = new QFrame(content_);
        banner->setProperty("role", "bannerWarning");
        auto* bannerLayout = new QHBoxLayout(banner);
        bannerLayout->setContentsMargins(10, 6, 6, 6);
        auto* note = new QLabel(tr("Part of this component is managed in code. Your code is kept, and the canvas may not show everything it does."), banner);
        note->setWordWrap(true);
        bannerLayout->addWidget(note, 1);
        auto* clear = new QPushButton(tr("Dismiss"), banner);
        connect(clear, &QPushButton::clicked, this, [this] { setValue("codeManaged", nullptr); rebuild(); });
        bannerLayout->addWidget(clear);
        contentLayout_->addWidget(banner);
    }

    QFormLayout* identity = formSection(tr("Identity"), QStringLiteral("identity"));
    addTextRow(identity, tr("Name"), "name");
    auto* idLabel = new QLineEdit(toQ(component.id), content_);
    idLabel->setReadOnly(true);
    idLabel->setObjectName(QStringLiteral("field_id"));
    identity->addRow(tr("Identifier"), idLabel);

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

    QFormLayout* layoutForm = formSection(tr("Layout"), QStringLiteral("layout"));
    if (!isRoot) {
        if (parent != nullptr && parent->layout.kind == ir::LayoutKind::Absolute) {
            addNumberRow(layoutForm, QStringLiteral("X"), "x");
            addNumberRow(layoutForm, QStringLiteral("Y"), "y");
        }
        if (parent != nullptr && parent->layout.kind == ir::LayoutKind::Constraint) {
            addNumberRow(layoutForm, tr("Pin left"), "pins.left", -100000, 100000, -999999);
            addNumberRow(layoutForm, tr("Pin right"), "pins.right", -100000, 100000, -999999);
            addNumberRow(layoutForm, tr("Pin top"), "pins.top", -100000, 100000, -999999);
            addNumberRow(layoutForm, tr("Pin bottom"), "pins.bottom", -100000, 100000, -999999);
        }
        addSizeRow(layoutForm, tr("Width"), "width");
        addSizeRow(layoutForm, tr("Height"), "height");
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
        addNumberRow(layoutForm, tr("Width"), "width", 1, 100000);
        addNumberRow(layoutForm, tr("Height"), "height", 1, 100000);
    }

    if (isContainer) {
        QFormLayout* children = formSection(tr("Children"), QStringLiteral("children"));
        addChoiceRow(children, tr("Arrange"), "layout.type", {QStringLiteral("vertical"), QStringLiteral("horizontal"), QStringLiteral("grid"), QStringLiteral("flex"), QStringLiteral("scroll"), QStringLiteral("absolute"), QStringLiteral("constraint")}, QStringLiteral("absolute"));
        addNumberRow(children, tr("Spacing"), "layout.spacing", 0, 10000);
        addInsetsRow(children, tr("Padding"), "layout.padding");
        addChoiceRow(children, tr("Align"), "layout.align", {QStringLiteral("start"), QStringLiteral("center"), QStringLiteral("end"), QStringLiteral("stretch")}, QStringLiteral("start"));
        addChoiceRow(children, tr("Distribute"), "layout.justify", {QStringLiteral("start"), QStringLiteral("center"), QStringLiteral("end"), QStringLiteral("spaceBetween"), QStringLiteral("spaceAround"), QStringLiteral("spaceEvenly")}, QStringLiteral("start"));
        if (component.layout.kind == ir::LayoutKind::Flex)
            addBoolRow(children, tr("Wrap"), "layout.wrap");
        if (component.layout.kind == ir::LayoutKind::Grid)
            addJsonRow(children, tr("Columns"), "layout.columns");
    }

    QFormLayout* appearance = formSection(tr("Appearance"), QStringLiteral("appearance"));
    addColorRow(appearance, tr("Background"), "style.background");
    addColorRow(appearance, tr("Border"), "style.borderColor");
    addNumberRow(appearance, tr("Border width"), "style.borderWidth", 0, 1000);
    addNumberRow(appearance, tr("Radius"), "style.radius", 0, 10000);
    addNumberRow(appearance, tr("Opacity"), "style.opacity", 0, 1, 1);
    addJsonRow(appearance, tr("Shadow"), "style.shadow");

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

    if (schema != nullptr && !schema->events.empty()) {
        QFormLayout* events = formSection(tr("Interactions"), QStringLiteral("events"));
        for (const auto& event : schema->events)
            addEventRow(events, toQ(event));
    }

    std::string bindKey = component.type == "text" ? "properties.content" : (component.type == "button" || component.type == "checkbox" || component.type == "switch") ? "properties.label" : component.type == "textinput" ? "properties.value" : "";
    if (!bindKey.empty() || component.type == "list") {
        QFormLayout* data = formSection(tr("Data"), QStringLiteral("data"), !component.bindings.empty());
        if (!bindKey.empty())
            addBindingRow(data, tr("Show value of"), bindKey);
        if (component.type == "checkbox" || component.type == "switch")
            addBindingRow(data, tr("Checked follows"), "properties.checked");
        if (component.type == "list")
            addBindingRow(data, tr("Items from"), "properties.items");
        addBindingRow(data, tr("Visible follows"), "visible");
    }

    QFormLayout* variants = formSection(tr("Screen sizes"), QStringLiteral("variants"), false);
    for (const auto& breakpoint : controller_->model()->breakpoints)
        addJsonRow(variants, toQ(breakpoint.name), "variants." + breakpoint.id);

    QFormLayout* platform = formSection(tr("Platform overrides"), QStringLiteral("platform"), false);
    for (const char* name : {"ios", "android", "macos", "windows", "linux", "web"})
        addJsonRow(platform, QString::fromLatin1(name), std::string("platform.") + name);
}

}
