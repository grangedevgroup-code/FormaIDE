#include "project/project_page.h"

#include <QBuffer>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QVBoxLayout>

#include "ui/theme.h"

namespace forma::ui {

Result<assets::ImageInfo> QtImageCodec::inspect(std::string_view data) const
{
    QByteArray bytes(data.data(), static_cast<qsizetype>(data.size()));
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    QSize size = reader.size();
    if (!size.isValid())
        return Error{"asset.unreadable", "This file is not an image FormaIDE can read"};
    assets::ImageInfo info;
    info.width = size.width();
    info.height = size.height();
    info.format = reader.format().toStdString();
    return info;
}

Result<std::string> QtImageCodec::convert(std::string_view data, std::string_view format, int width, int height, int quality) const
{
    QImage image = QImage::fromData(reinterpret_cast<const uchar*>(data.data()), static_cast<int>(data.size()));
    if (image.isNull())
        return Error{"asset.unreadable", "This file is not an image FormaIDE can read"};
    if (width > 0 && height > 0)
        image = image.scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QByteArray output;
    QBuffer buffer(&output);
    buffer.open(QIODevice::WriteOnly);
    std::string name(format);
    if (!image.save(&buffer, name.c_str(), quality))
        return Error{"asset.convert", "This computer cannot write " + name + " images"};
    return output.toStdString();
}

AssetsPanel::AssetsPanel(ProjectController* controller, QWidget* parent) : QWidget(parent), controller_(controller)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(6);
    auto* top = new QHBoxLayout();
    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("assetSearch"));
    search_->setPlaceholderText(tr("Search assets"));
    search_->setClearButtonEnabled(true);
    top->addWidget(search_, 1);
    auto* import = new QPushButton(tr("Import..."), this);
    import->setObjectName(QStringLiteral("assetImport"));
    top->addWidget(import);
    auto* rescan = new QPushButton(tr("Rescan"), this);
    top->addWidget(rescan);
    layout->addLayout(top);
    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("assetList"));
    list_->setViewMode(QListView::IconMode);
    list_->setIconSize(QSize(72, 72));
    list_->setGridSize(QSize(112, 112));
    list_->setResizeMode(QListView::Adjust);
    list_->setMovement(QListView::Static);
    list_->setWordWrap(true);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(list_, 1);
    details_ = new QLabel(tr("Import images, icons, fonts and other files your app uses."), this);
    details_->setProperty("role", "muted");
    details_->setWordWrap(true);
    layout->addWidget(details_);
    connect(search_, &QLineEdit::textChanged, this, &AssetsPanel::refresh);
    connect(import, &QPushButton::clicked, this, [this] {
        const QStringList files = QFileDialog::getOpenFileNames(this, tr("Import assets"));
        for (const auto& file : files) {
            auto status = importFile(file);
            if (!status)
                controller_->reportStatus(toQ(status.error().message));
        }
    });
    connect(rescan, &QPushButton::clicked, this, [this] {
        if (!controller_->hasProject())
            return;
        auto status = manager().rescan();
        if (!status)
            controller_->reportStatus(toQ(status.error().message));
        refresh();
    });
    connect(list_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
        if (item == nullptr || !controller_->hasProject())
            return;
        const QString id = item->data(Qt::UserRole).toString();
        auto used = manager().references(fromQ(id));
        for (const auto& asset : controller_->model()->assets) {
            if (toQ(asset.id) != id)
                continue;
            QString size = asset.width > 0 ? QStringLiteral(" %1 x %2").arg(asset.width).arg(asset.height) : QString();
            details_->setText(tr("%1 (%2%3), used by %n component(s)", nullptr, static_cast<int>(used.size())).arg(toQ(asset.path), toQ(asset.kind), size));
        }
    });
    connect(list_, &QWidget::customContextMenuRequested, this, &AssetsPanel::showMenu);
    connect(controller_, &ProjectController::projectOpened, this, &AssetsPanel::refresh);
    connect(controller_, &ProjectController::modelChanged, this, [this](const ir::ChangeEvent& event) {
        if (event.key == "assets" || event.kind == ir::ChangeKind::Reset)
            refresh();
    });
}

assets::AssetManager AssetsPanel::manager() const
{
    return assets::AssetManager(controller_->session()->filesystem(), *controller_->editor(), &codec_);
}

int AssetsPanel::assetCount() const
{
    return list_->count();
}

Status AssetsPanel::importFile(const QString& path)
{
    if (!controller_->hasProject())
        return Error{"asset.project", "Open a project first"};
    auto imported = manager().import(paths::fromUtf8(fromQ(path)));
    if (!imported)
        return imported.error();
    refresh();
    return {};
}

void AssetsPanel::refresh()
{
    list_->clear();
    if (!controller_->hasProject())
        return;
    for (const auto& asset : manager().search(fromQ(search_->text()))) {
        auto* item = new QListWidgetItem(list_);
        item->setText(toQ(asset.id));
        item->setToolTip(toQ(asset.path));
        item->setData(Qt::UserRole, toQ(asset.id));
        QIcon thumbnail;
        if (asset.kind == "image" || asset.kind == "icon") {
            QImage image(QDir(controller_->rootPath()).filePath(toQ(asset.path)));
            if (!image.isNull())
                thumbnail = QIcon(QPixmap::fromImage(image.scaled(72, 72, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        }
        if (thumbnail.isNull())
            thumbnail = icon(asset.kind == "image" ? QStringLiteral("image") : QStringLiteral("file"));
        item->setIcon(thumbnail);
    }
}

void AssetsPanel::showMenu(const QPoint& position)
{
    QListWidgetItem* item = list_->itemAt(position);
    if (item == nullptr)
        return;
    QString id = item->data(Qt::UserRole).toString();
    auto report = [this](const Status& status) {
        if (!status)
            controller_->reportStatus(toQ(status.error().message));
        refresh();
    };
    QMenu menu(this);
    menu.addAction(tr("Rename..."), this, [this, id, report] {
        bool accepted = false;
        QString name = QInputDialog::getText(this, tr("Rename asset"), tr("New name"), QLineEdit::Normal, id, &accepted);
        if (accepted && !name.isEmpty())
            report(manager().rename(fromQ(id), fromQ(name)));
    });
    menu.addAction(tr("Replace with..."), this, [this, id, report] {
        QString file = QFileDialog::getOpenFileName(this, tr("Replace asset"));
        if (!file.isEmpty())
            report(manager().replace(fromQ(id), paths::fromUtf8(fromQ(file))));
    });
    menu.addAction(tr("Optimize"), this, [this, id] {
        auto result = manager().optimize(fromQ(id));
        controller_->reportStatus(result ? toQ(*result) : toQ(result.error().message));
        refresh();
    });
    menu.addAction(tr("Convert to PNG"), this, [this, id] {
        auto result = manager().convert(fromQ(id), "png");
        controller_->reportStatus(result ? tr("Converted to %1").arg(toQ(*result)) : toQ(result.error().message));
        refresh();
    });
    menu.addSeparator();
    menu.addAction(icon(QStringLiteral("trash")), tr("Delete"), this, [this, id, report] {
        auto used = manager().references(fromQ(id));
        QString question = used.empty() ? tr("Delete %1?").arg(id) : tr("%1 is used by %n component(s). Delete it anyway?", nullptr, static_cast<int>(used.size())).arg(id);
        if (QMessageBox::question(this, tr("Delete asset"), question) == QMessageBox::Yes)
            report(manager().remove(fromQ(id)));
    });
    menu.exec(list_->viewport()->mapToGlobal(position));
}

HistoryPanel::HistoryPanel(ProjectController* controller, QWidget* parent) : QWidget(parent), controller_(controller)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(6);
    auto* top = new QHBoxLayout();
    top->addWidget(new QLabel(tr("Show"), this));
    kind_ = new QComboBox(this);
    kind_->addItem(tr("Everything"), QString());
    kind_->addItem(tr("Generated code"), QStringLiteral("generate"));
    kind_->addItem(tr("Files"), QStringLiteral("file"));
    kind_->addItem(tr("Assistant"), QStringLiteral("assistant"));
    top->addWidget(kind_);
    top->addStretch(1);
    auto* snapshot = new QPushButton(tr("Save a snapshot"), this);
    snapshot->setObjectName(QStringLiteral("historySnapshot"));
    top->addWidget(snapshot);
    auto* restore = new QPushButton(tr("Restore selected snapshot"), this);
    restore->setObjectName(QStringLiteral("historyRestore"));
    top->addWidget(restore);
    layout->addLayout(top);
    auto* split = new QSplitter(Qt::Vertical, this);
    entries_ = new QTreeWidget(split);
    entries_->setObjectName(QStringLiteral("historyEntries"));
    entries_->setHeaderLabels({tr("Time"), tr("Who"), tr("What")});
    entries_->setRootIsDecorated(false);
    entries_->setUniformRowHeights(true);
    snapshots_ = new QTreeWidget(split);
    snapshots_->setObjectName(QStringLiteral("historySnapshots"));
    snapshots_->setHeaderLabels({tr("Snapshot"), tr("Time"), tr("Who"), tr("Files")});
    snapshots_->setRootIsDecorated(false);
    snapshots_->setUniformRowHeights(true);
    layout->addWidget(split, 1);
    connect(kind_, &QComboBox::currentIndexChanged, this, &HistoryPanel::refresh);
    connect(snapshot, &QPushButton::clicked, this, &HistoryPanel::snapshotNow);
    connect(restore, &QPushButton::clicked, this, &HistoryPanel::restoreSelected);
    connect(controller_, &ProjectController::projectOpened, this, &HistoryPanel::refresh);
    connect(controller_, &ProjectController::generationFinished, this, [this](const codegen::GenerationReport&) { refresh(); });
    connect(controller_, &ProjectController::syncFinished, this, [this](const QString&) { refresh(); });
}

void HistoryPanel::refresh()
{
    entries_->clear();
    snapshots_->clear();
    if (!controller_->hasProject())
        return;
    auto list = controller_->session()->history().list(300, fromQ(kind_->currentData().toString()));
    if (list) {
        for (const auto& entry : *list) {
            auto* item = new QTreeWidgetItem(entries_);
            item->setText(0, toQ(entry.time));
            item->setText(1, toQ(entry.actor));
            item->setText(2, toQ(entry.summary));
            item->setToolTip(2, toQ(entry.summary));
        }
    }
    auto saved = controller_->session()->snapshots().list();
    if (saved) {
        for (const auto& info : *saved) {
            auto* item = new QTreeWidgetItem(snapshots_);
            item->setText(0, toQ(info.label));
            item->setText(1, toQ(info.time));
            item->setText(2, toQ(info.actor));
            item->setText(3, QString::number(info.fileCount));
            item->setData(0, Qt::UserRole, toQ(info.id));
        }
    }
    entries_->resizeColumnToContents(0);
    snapshots_->resizeColumnToContents(0);
}

void HistoryPanel::snapshotNow()
{
    if (!controller_->hasProject())
        return;
    bool accepted = false;
    QString label = QInputDialog::getText(this, tr("Save a snapshot"), tr("Name"), QLineEdit::Normal, tr("Manual snapshot"), &accepted);
    if (!accepted)
        return;
    auto saved = controller_->saveAll();
    (void)saved;
    auto created = controller_->session()->snapshots().create(fromQ(label), "user");
    controller_->reportStatus(created ? tr("Snapshot saved") : toQ(created.error().message));
    refresh();
}

void HistoryPanel::restoreSelected()
{
    QTreeWidgetItem* item = snapshots_->currentItem();
    if (item == nullptr || !controller_->hasProject())
        return;
    if (QMessageBox::question(this, tr("Restore snapshot"), tr("Put the project's files back to \"%1\"? A new snapshot of the current state is saved first.").arg(item->text(0))) != QMessageBox::Yes)
        return;
    auto saved = controller_->saveAll();
    (void)saved;
    auto before = controller_->session()->snapshots().create("Before restore", "user");
    (void)before;
    QString root = controller_->rootPath();
    auto restored = controller_->session()->snapshots().restore(fromQ(item->data(0, Qt::UserRole).toString()));
    if (!restored) {
        controller_->reportStatus(toQ(restored.error().message));
        return;
    }
    controller_->closeProject();
    auto reopened = controller_->openProject(root);
    controller_->reportStatus(reopened ? tr("Snapshot restored") : toQ(reopened.error().message));
}

ProjectSettingsPanel::ProjectSettingsPanel(ProjectController* controller, QWidget* parent) : QWidget(parent), controller_(controller)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    auto* form = new QFormLayout();
    name_ = new QLineEdit(this);
    name_->setObjectName(QStringLiteral("projectName"));
    bundle_ = new QLineEdit(this);
    version_ = new QLineEdit(this);
    location_ = new QLabel(this);
    location_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    location_->setWordWrap(true);
    form->addRow(tr("Name"), name_);
    form->addRow(tr("Bundle or package id"), bundle_);
    form->addRow(tr("Version"), version_);
    form->addRow(tr("Folder"), location_);
    layout->addLayout(form);
    save_ = new QPushButton(tr("Save"), this);
    save_->setObjectName(QStringLiteral("projectSave"));
    auto* row = new QHBoxLayout();
    row->addStretch(1);
    row->addWidget(save_);
    layout->addLayout(row);
    auto* label = new QLabel(tr("Targets"), this);
    label->setProperty("role", "section");
    layout->addWidget(label);
    targets_ = new QTreeWidget(this);
    targets_->setObjectName(QStringLiteral("projectTargets"));
    targets_->setHeaderLabels({tr("Target"), tr("Generator"), tr("Build with"), tr("Folder")});
    targets_->setRootIsDecorated(false);
    layout->addWidget(targets_, 1);
    connect(save_, &QPushButton::clicked, this, &ProjectSettingsPanel::apply);
    connect(controller_, &ProjectController::projectOpened, this, &ProjectSettingsPanel::refresh);
}

void ProjectSettingsPanel::refresh()
{
    targets_->clear();
    if (!controller_->hasProject())
        return;
    const auto& descriptor = controller_->session()->descriptor();
    name_->setText(toQ(descriptor.name));
    bundle_->setText(toQ(descriptor.bundleId));
    version_->setText(toQ(descriptor.version));
    location_->setText(QDir::toNativeSeparators(controller_->rootPath()));
    for (const auto& target : descriptor.targets) {
        auto* item = new QTreeWidgetItem(targets_);
        item->setText(0, toQ(target.label));
        item->setText(1, toQ(target.generator));
        item->setText(2, toQ(target.buildProvider));
        item->setText(3, toQ(target.outputDirectory));
    }
}

void ProjectSettingsPanel::apply()
{
    if (!controller_->hasProject())
        return;
    auto& descriptor = controller_->session()->descriptor();
    auto valid = project::validateProjectName(fromQ(name_->text()));
    if (!valid) {
        controller_->reportStatus(toQ(valid.error().message));
        return;
    }
    descriptor.name = fromQ(name_->text());
    descriptor.bundleId = fromQ(bundle_->text());
    descriptor.version = fromQ(version_->text());
    auto saved = controller_->session()->saveDescriptor();
    controller_->reportStatus(saved ? tr("Project details saved") : toQ(saved.error().message));
}

ProjectPage::ProjectPage(ProjectController* controller, QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* tabs = new QTabWidget(this);
    tabs->setObjectName(QStringLiteral("projectTabs"));
    tabs->setDocumentMode(true);
    settings_ = new ProjectSettingsPanel(controller, tabs);
    assets_ = new AssetsPanel(controller, tabs);
    history_ = new HistoryPanel(controller, tabs);
    tabs->addTab(settings_, tr("Details"));
    tabs->addTab(assets_, tr("Assets"));
    tabs->addTab(history_, tr("History"));
    layout->addWidget(tabs);
}

}
