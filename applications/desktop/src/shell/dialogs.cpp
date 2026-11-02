#include "shell/dialogs.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QSvgRenderer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "ai/assistant_panel.h"
#include "app/app_context.h"
#include "forma/core/text.h"
#include "forma/resources/resources.h"
#include "forma/terminal/terminal.h"
#include "ui/theme.h"

namespace forma::ui {

namespace {

QPixmap logoPixmap(int height)
{
    auto data = resources::find(Theme::instance().isDark() ? "branding/logo-dark.svg" : "branding/logo-light.svg");
    if (!data)
        return {};
    QSvgRenderer renderer(QByteArray(data->data(), static_cast<qsizetype>(data->size())));
    QSize size = renderer.defaultSize();
    if (size.isEmpty())
        return {};
    qreal ratio = 2.0;
    QPixmap pixmap(QSize(size.width() * height / size.height(), height) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    renderer.render(&painter, QRectF(0, 0, pixmap.width() / ratio, height));
    return pixmap;
}

QString defaultProjectsFolder()
{
    QString saved = toQ(AppContext::instance().settings().getString("projects.location"));
    if (!saved.isEmpty())
        return saved;
    return QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath(QStringLiteral("FormaProjects"));
}

}

WelcomePage::WelcomePage(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("welcomePage"));
    auto* outer = new QHBoxLayout(this);
    outer->addStretch(1);
    auto* column = new QVBoxLayout();
    column->setSpacing(10);
    column->addStretch(1);
    auto* logo = new QLabel(this);
    logo->setObjectName(QStringLiteral("welcomeLogo"));
    logo->setPixmap(logoPixmap(48));
    column->addWidget(logo);
    auto* subtitle = new QLabel(tr("Design screens, generate real source code, edit it, and build it, all on your own computer."), this);
    subtitle->setProperty("role", "muted");
    subtitle->setWordWrap(true);
    column->addWidget(subtitle);
    column->addSpacing(12);
    auto* actions = new QHBoxLayout();
    auto* create = new QPushButton(icon(QStringLiteral("plus")), tr("New project"), this);
    create->setObjectName(QStringLiteral("welcomeNew"));
    create->setProperty("role", "primary");
    auto* open = new QPushButton(icon(QStringLiteral("folder-open")), tr("Open project"), this);
    open->setObjectName(QStringLiteral("welcomeOpen"));
    auto* import = new QPushButton(icon(QStringLiteral("code")), tr("Open an existing code folder"), this);
    import->setObjectName(QStringLiteral("welcomeImport"));
    actions->addWidget(create);
    actions->addWidget(open);
    actions->addWidget(import);
    actions->addStretch(1);
    column->addLayout(actions);
    column->addSpacing(12);
    auto* label = new QLabel(tr("Recent projects"), this);
    label->setProperty("role", "section");
    column->addWidget(label);
    recents_ = new QListWidget(this);
    recents_->setObjectName(QStringLiteral("recentProjects"));
    recents_->setMinimumSize(640, 240);
    recents_->setContextMenuPolicy(Qt::CustomContextMenu);
    column->addWidget(recents_);
    emptyRecents_ = new QLabel(tr("Projects you open will be listed here."), this);
    emptyRecents_->setProperty("role", "muted");
    column->addWidget(emptyRecents_);
    column->addStretch(2);
    outer->addLayout(column);
    outer->addStretch(1);
    connect(create, &QPushButton::clicked, this, &WelcomePage::newRequested);
    connect(open, &QPushButton::clicked, this, &WelcomePage::openRequested);
    connect(import, &QPushButton::clicked, this, &WelcomePage::importRequested);
    connect(recents_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) { Q_EMIT recentRequested(item->data(Qt::UserRole).toString()); });
    connect(recents_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) { Q_EMIT recentRequested(item->data(Qt::UserRole).toString()); });
    connect(recents_, &QWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        QListWidgetItem* item = recents_->itemAt(position);
        if (item == nullptr)
            return;
        QString path = item->data(Qt::UserRole).toString();
        QMenu menu(this);
        menu.addAction(tr("Remove from this list"), this, [this, path] {
            AppContext::instance().recents().remove(paths::fromUtf8(fromQ(path)));
            refresh();
        });
        menu.exec(recents_->viewport()->mapToGlobal(position));
    });
    refresh();
}

void WelcomePage::refresh()
{
    recents_->clear();
    for (const auto& recent : AppContext::instance().recents().list()) {
        auto* item = new QListWidgetItem(recents_);
        item->setText(QStringLiteral("%1  -  %2").arg(toQ(recent.name), QDir::toNativeSeparators(toQ(recent.path))));
        item->setData(Qt::UserRole, toQ(recent.path));
        item->setToolTip(toQ(recent.path));
    }
    recents_->setVisible(recents_->count() > 0);
    emptyRecents_->setVisible(recents_->count() == 0);
}

NewProjectDialog::NewProjectDialog(QWidget* parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("newProjectDialog"));
    setWindowTitle(tr("New project"));
    resize(820, 540);
    auto* layout = new QVBoxLayout(this);
    auto* split = new QHBoxLayout();
    templates_ = new QTreeWidget(this);
    templates_->setObjectName(QStringLiteral("templateList"));
    templates_->setHeaderHidden(true);
    templates_->setFixedWidth(300);
    auto& app = AppContext::instance();
    std::map<std::string, QTreeWidgetItem*> groups;
    for (const auto& item : app.templates().all()) {
        if (!item.generator.empty() && app.generators().find(item.generator) == nullptr)
            continue;
        QTreeWidgetItem*& group = groups[item.category];
        if (group == nullptr) {
            group = new QTreeWidgetItem(templates_);
            group->setText(0, toQ(item.category));
            group->setFlags(Qt::ItemIsEnabled);
            QFont bold = group->font(0);
            bold.setWeight(QFont::DemiBold);
            group->setFont(0, bold);
        }
        auto* child = new QTreeWidgetItem(group);
        child->setText(0, toQ(item.name));
        child->setData(0, Qt::UserRole, toQ(item.id));
    }
    templates_->expandAll();
    split->addWidget(templates_);
    auto* right = new QVBoxLayout();
    description_ = new QLabel(this);
    description_->setObjectName(QStringLiteral("templateDescription"));
    description_->setWordWrap(true);
    right->addWidget(description_);
    tools_ = new QLabel(this);
    tools_->setObjectName(QStringLiteral("templateTools"));
    tools_->setWordWrap(true);
    tools_->setTextFormat(Qt::RichText);
    right->addWidget(tools_);
    right->addSpacing(8);
    auto* form = new QFormLayout();
    name_ = new QLineEdit(tr("My App"), this);
    name_->setObjectName(QStringLiteral("projectNameEdit"));
    form->addRow(tr("Name"), name_);
    auto* locationRow = new QHBoxLayout();
    location_ = new QLineEdit(defaultProjectsFolder(), this);
    location_->setObjectName(QStringLiteral("projectLocationEdit"));
    locationRow->addWidget(location_, 1);
    auto* browse = new QPushButton(tr("Browse..."), this);
    locationRow->addWidget(browse);
    form->addRow(tr("Save in"), locationRow);
    bundle_ = new QLineEdit(this);
    bundle_->setObjectName(QStringLiteral("projectBundleEdit"));
    form->addRow(tr("Package id"), bundle_);
    fullPath_ = new QLabel(this);
    fullPath_->setObjectName(QStringLiteral("projectFullPath"));
    fullPath_->setProperty("role", "muted");
    fullPath_->setWordWrap(true);
    form->addRow(tr("Full path"), fullPath_);
    git_ = new QCheckBox(tr("Start a Git repository"), this);
    git_->setChecked(app.settings().getBool("projects.git", true));
    form->addRow(QString(), git_);
    right->addLayout(form);
    right->addStretch(1);
    split->addLayout(right, 1);
    layout->addLayout(split, 1);
    auto* buttons = new QDialogButtonBox(this);
    create_ = buttons->addButton(tr("Create project"), QDialogButtonBox::AcceptRole);
    create_->setObjectName(QStringLiteral("projectCreate"));
    create_->setProperty("role", "primary");
    buttons->addButton(QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &NewProjectDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(browse, &QPushButton::clicked, this, [this] {
        QString folder = QFileDialog::getExistingDirectory(this, tr("Save in"), location_->text());
        if (!folder.isEmpty())
            location_->setText(folder);
    });
    connect(templates_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem*, QTreeWidgetItem*) { updateDetails(); });
    connect(name_, &QLineEdit::textChanged, this, &NewProjectDialog::updateDetails);
    connect(location_, &QLineEdit::textChanged, this, &NewProjectDialog::updateDetails);
    connect(bundle_, &QLineEdit::textEdited, this, [this] { bundleEdited_ = true; });
    selectTemplate(QStringLiteral("qt-desktop"));
    if (templates_->currentItem() == nullptr)
        selectTemplate(QStringLiteral("blank"));
    updateDetails();
}

void NewProjectDialog::selectTemplate(const QString& id)
{
    for (int g = 0; g < templates_->topLevelItemCount(); ++g) {
        QTreeWidgetItem* group = templates_->topLevelItem(g);
        for (int i = 0; i < group->childCount(); ++i) {
            if (group->child(i)->data(0, Qt::UserRole).toString() == id) {
                templates_->setCurrentItem(group->child(i));
                return;
            }
        }
    }
}

void NewProjectDialog::setProjectName(const QString& name)
{
    name_->setText(name);
}

void NewProjectDialog::setLocation(const QString& path)
{
    location_->setText(path);
}

void NewProjectDialog::updateDetails()
{
    auto& app = AppContext::instance();
    QTreeWidgetItem* item = templates_->currentItem();
    const project::ProjectTemplate* chosen = item != nullptr ? app.templates().find(fromQ(item->data(0, Qt::UserRole).toString())) : nullptr;
    if (chosen != nullptr) {
        description_->setText(toQ(chosen->description));
        QStringList parts;
        for (const auto& tool : chosen->tools) {
            auto info = app.tools().detect(tool);
            QColor color = info.available ? colors().success : colors().warning;
            QString status = info.available ? tr("found") : tr("not installed, needed to build this project");
            parts << QStringLiteral("<span style='color:%1'>%2: %3</span>").arg(color.name(), toQ(info.name.empty() ? tool : info.name).toHtmlEscaped(), status);
        }
        tools_->setText(parts.join(QStringLiteral("<br>")));
    }
    QString name = name_->text().trimmed();
    if (!bundleEdited_)
        bundle_->setText(toQ(project::suggestBundleId(fromQ(name))));
    auto valid = project::validateProjectName(fromQ(name));
    QString target = QDir(location_->text()).filePath(name);
    if (!valid) {
        fullPath_->setText(toQ(valid.error().message));
        create_->setEnabled(false);
        return;
    }
    if (QDir(target).exists() && !QDir(target).isEmpty()) {
        fullPath_->setText(tr("%1 already exists and isn't empty").arg(QDir::toNativeSeparators(target)));
        create_->setEnabled(false);
        return;
    }
    fullPath_->setText(QDir::toNativeSeparators(target));
    create_->setEnabled(chosen != nullptr);
}

project::NewProjectOptions NewProjectDialog::options() const
{
    project::NewProjectOptions options;
    options.name = fromQ(name_->text().trimmed());
    options.location = paths::fromUtf8(fromQ(QDir(location_->text()).filePath(name_->text().trimmed())));
    options.bundleId = fromQ(bundle_->text().trimmed());
    if (templates_->currentItem() != nullptr)
        options.templateId = fromQ(templates_->currentItem()->data(0, Qt::UserRole).toString());
    options.initGit = git_->isChecked();
    return options;
}

void NewProjectDialog::accept()
{
    auto& app = AppContext::instance();
    app.settings().set("projects.location", fromQ(location_->text()));
    app.settings().set("projects.git", git_->isChecked());
    app.saveSettings();
    QDialog::accept();
}

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("settingsDialog"));
    setWindowTitle(tr("Settings"));
    resize(820, 540);
    auto* layout = new QVBoxLayout(this);
    auto* row = new QHBoxLayout();
    pages_ = new QListWidget(this);
    pages_->setObjectName(QStringLiteral("settingsPages"));
    pages_->setFixedWidth(160);
    stack_ = new QStackedWidget(this);
    row->addWidget(pages_);
    row->addWidget(stack_, 1);
    layout->addLayout(row, 1);
    auto addPage = [&](const QString& title, QWidget* page) {
        pages_->addItem(title);
        stack_->addWidget(page);
    };
    addPage(tr("Appearance"), buildAppearancePage());
    addPage(tr("Editor"), buildEditorPage());
    addPage(tr("Assistant"), buildAssistantPage());
    addPage(tr("Plugins"), buildPluginsPage());
    connect(pages_, &QListWidget::currentRowChanged, stack_, &QStackedWidget::setCurrentIndex);
    pages_->setCurrentRow(0);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    layout->addWidget(buttons);
}

void SettingsDialog::showPage(const QString& name)
{
    auto found = pages_->findItems(name, Qt::MatchExactly);
    if (!found.isEmpty())
        pages_->setCurrentItem(found.front());
}

QWidget* SettingsDialog::buildAppearancePage()
{
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);
    themeMode_ = new QComboBox(page);
    themeMode_->setObjectName(QStringLiteral("settingsTheme"));
    themeMode_->addItem(tr("Same as the system"), QStringLiteral("system"));
    themeMode_->addItem(tr("Light"), QStringLiteral("light"));
    themeMode_->addItem(tr("Dark"), QStringLiteral("dark"));
    themeMode_->setCurrentIndex(std::max(0, themeMode_->findData(Theme::instance().mode())));
    form->addRow(tr("Theme"), themeMode_);
    connect(themeMode_, &QComboBox::currentIndexChanged, this, [this](int) {
        QString mode = themeMode_->currentData().toString();
        Theme::instance().setMode(mode);
        AppContext::instance().settings().set("appearance.theme", fromQ(mode));
        AppContext::instance().saveSettings();
        Q_EMIT settingsApplied();
    });
    return page;
}

QWidget* SettingsDialog::buildEditorPage()
{
    auto& settings = AppContext::instance().settings();
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);
    fontSize_ = new QSpinBox(page);
    fontSize_->setRange(6, 36);
    fontSize_->setValue(settings.getInt("editor.fontSize", 10));
    form->addRow(tr("Font size"), fontSize_);
    minimap_ = new QCheckBox(tr("Show a minimap beside the code"), page);
    minimap_->setChecked(settings.getBool("editor.minimap", false));
    form->addRow(QString(), minimap_);
    languageServers_ = new QCheckBox(tr("Start language servers for completion and errors"), page);
    languageServers_->setChecked(settings.getBool("editor.languageServers", true));
    form->addRow(QString(), languageServers_);
    shell_ = new QComboBox(page);
    shell_->setObjectName(QStringLiteral("settingsShell"));
    for (const auto& item : terminal::availableShells())
        shell_->addItem(toQ(item.name), toQ(item.id));
    shell_->setCurrentIndex(std::max(0, shell_->findData(toQ(settings.getString("terminal.shell")))));
    form->addRow(tr("Terminal shell"), shell_);
    auto store = [this] {
        auto& s = AppContext::instance().settings();
        s.set("editor.fontSize", fontSize_->value());
        s.set("editor.minimap", minimap_->isChecked());
        s.set("editor.languageServers", languageServers_->isChecked());
        s.set("terminal.shell", fromQ(shell_->currentData().toString()));
        AppContext::instance().saveSettings();
        Q_EMIT settingsApplied();
    };
    connect(fontSize_, &QSpinBox::valueChanged, this, store);
    connect(minimap_, &QCheckBox::toggled, this, store);
    connect(languageServers_, &QCheckBox::toggled, this, store);
    connect(shell_, &QComboBox::currentIndexChanged, this, store);
    return page;
}

QWidget* SettingsDialog::buildAssistantPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    auto* intro = new QLabel(tr("The assistant is optional. A local model keeps everything on this computer. With a cloud provider, the code and context you approve is sent to that provider. API keys are kept in the system's credential store, never in project files."), page);
    intro->setWordWrap(true);
    intro->setProperty("role", "muted");
    layout->addWidget(intro);
    auto* row = new QHBoxLayout();
    auto* left = new QVBoxLayout();
    providers_ = new QListWidget(page);
    providers_->setObjectName(QStringLiteral("providerList"));
    providers_->setFixedWidth(220);
    left->addWidget(providers_, 1);
    auto* buttons = new QHBoxLayout();
    auto* add = new QPushButton(tr("Add..."), page);
    add->setObjectName(QStringLiteral("providerAdd"));
    auto* addMenu = new QMenu(add);
    auto presets = ai::providerPresets();
    for (const auto& preset : presets) {
        addMenu->addAction(toQ(preset.label), this, [this, preset] {
            ai::ProviderConfig config = preset;
            config.id = preset.id + "-" + std::to_string(providerList_.size() + 1);
            if (!preset.local)
                config.credentialAccount = "assistant." + config.id;
            providerList_.push_back(config);
            saveProviders();
            reloadProviderList();
            providers_->setCurrentRow(static_cast<int>(providerList_.size()) - 1);
        });
    }
    add->setMenu(addMenu);
    auto* remove = new QPushButton(tr("Remove"), page);
    buttons->addWidget(add);
    buttons->addWidget(remove);
    left->addLayout(buttons);
    row->addLayout(left);
    auto* right = new QVBoxLayout();
    auto* form = new QFormLayout();
    providerLabel_ = new QLineEdit(page);
    providerUrl_ = new QLineEdit(page);
    providerUrl_->setObjectName(QStringLiteral("providerUrl"));
    providerModel_ = new QLineEdit(page);
    providerModel_->setObjectName(QStringLiteral("providerModel"));
    providerKey_ = new QLineEdit(page);
    providerKey_->setObjectName(QStringLiteral("providerKey"));
    providerKey_->setEchoMode(QLineEdit::Password);
    providerKey_->setPlaceholderText(tr("Leave empty to keep the saved key"));
    form->addRow(tr("Name"), providerLabel_);
    form->addRow(tr("Address"), providerUrl_);
    form->addRow(tr("Model"), providerModel_);
    form->addRow(tr("API key"), providerKey_);
    right->addLayout(form);
    auto* actions = new QHBoxLayout();
    auto* test = new QPushButton(tr("Test connection"), page);
    test->setObjectName(QStringLiteral("providerTest"));
    auto* save = new QPushButton(tr("Save"), page);
    save->setObjectName(QStringLiteral("providerSave"));
    save->setProperty("role", "primary");
    actions->addWidget(test);
    actions->addStretch(1);
    actions->addWidget(save);
    right->addLayout(actions);
    providerStatus_ = new QLabel(page);
    providerStatus_->setObjectName(QStringLiteral("providerStatus"));
    providerStatus_->setWordWrap(true);
    right->addWidget(providerStatus_);
    providerNote_ = new QLabel(page);
    providerNote_->setProperty("role", "muted");
    providerNote_->setWordWrap(true);
    right->addWidget(providerNote_);
    right->addStretch(1);
    row->addLayout(right, 1);
    layout->addLayout(row, 1);
    Json saved = AppContext::instance().settings().get("assistant.providers", Json::array());
    if (saved.is_array()) {
        for (const auto& item : saved)
            providerList_.push_back(ai::providerConfigFromJson(item));
    }
    connect(providers_, &QListWidget::currentRowChanged, this, &SettingsDialog::loadProvider);
    connect(remove, &QPushButton::clicked, this, [this] {
        int row = providers_->currentRow();
        if (row < 0 || row >= static_cast<int>(providerList_.size()))
            return;
        const auto& config = providerList_[static_cast<std::size_t>(row)];
        if (!config.credentialAccount.empty()) {
            auto removed = AppContext::instance().credentials().remove(config.credentialAccount);
            (void)removed;
        }
        providerList_.erase(providerList_.begin() + row);
        providerRow_ = -1;
        saveProviders();
        reloadProviderList();
    });
    connect(save, &QPushButton::clicked, this, [this] {
        if (providerRow_ < 0 || providerRow_ >= static_cast<int>(providerList_.size()))
            return;
        auto& config = providerList_[static_cast<std::size_t>(providerRow_)];
        config.label = fromQ(providerLabel_->text());
        config.baseUrl = fromQ(providerUrl_->text());
        config.model = fromQ(providerModel_->text());
        if (!providerKey_->text().isEmpty() && !config.credentialAccount.empty()) {
            auto stored = AppContext::instance().credentials().set(config.credentialAccount, fromQ(providerKey_->text()));
            providerKey_->clear();
            if (!stored) {
                providerStatus_->setText(toQ(stored.error().message));
                return;
            }
        }
        saveProviders();
        int row = providerRow_;
        reloadProviderList();
        providers_->setCurrentRow(row);
        providerStatus_->setText(tr("Saved"));
    });
    connect(test, &QPushButton::clicked, this, [this] {
        if (providerRow_ < 0 || providerRow_ >= static_cast<int>(providerList_.size()))
            return;
        ai::ProviderConfig config = providerList_[static_cast<std::size_t>(providerRow_)];
        config.baseUrl = fromQ(providerUrl_->text());
        std::string key = fromQ(providerKey_->text());
        if (key.empty() && !config.credentialAccount.empty()) {
            auto stored = AppContext::instance().credentials().get(config.credentialAccount);
            if (stored && stored->has_value())
                key = **stored;
        }
        auto transport = std::make_shared<QtHttpTransport>();
        std::shared_ptr<ai::IAIProvider> provider = ai::createProvider(config, key, transport);
        providerStatus_->setText(tr("Connecting..."));
        QPointer<QLabel> status(providerStatus_);
        provider->listModels([status, provider, transport](Result<std::vector<std::string>> models) {
            if (!status)
                return;
            status->setText(models ? tr("Connected. %n model(s) available.", nullptr, static_cast<int>(models->size())) : toQ(models.error().message));
        });
    });
    reloadProviderList();
    return page;
}

void SettingsDialog::reloadProviderList()
{
    QSignalBlocker blocker(providers_);
    providers_->clear();
    for (const auto& config : providerList_)
        providers_->addItem(toQ(config.label));
    providerRow_ = -1;
    providerLabel_->clear();
    providerUrl_->clear();
    providerModel_->clear();
    providerKey_->clear();
    providerStatus_->clear();
    providerNote_->clear();
}

void SettingsDialog::loadProvider(int row)
{
    providerRow_ = row;
    if (row < 0 || row >= static_cast<int>(providerList_.size()))
        return;
    const auto& config = providerList_[static_cast<std::size_t>(row)];
    providerLabel_->setText(toQ(config.label));
    providerUrl_->setText(toQ(config.baseUrl));
    providerModel_->setText(toQ(config.model));
    providerKey_->setEnabled(!config.credentialAccount.empty());
    providerKey_->clear();
    providerStatus_->clear();
    providerNote_->setText(config.kind == "anthropic" ? tr("On api.anthropic.com, FormaIDE turns on server-side model fallback for the newest models, so a request the chosen model can't serve may be answered by another Claude model.") : QString());
}

void SettingsDialog::saveProviders()
{
    Json list = Json::array();
    for (const auto& config : providerList_)
        list.push_back(ai::providerConfigToJson(config));
    AppContext::instance().settings().set("assistant.providers", list);
    AppContext::instance().saveSettings();
    Q_EMIT settingsApplied();
}

QWidget* SettingsDialog::buildPluginsPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    auto* intro = new QLabel(tr("Plugins add generators, templates, languages and build tools. They run as separate programs. FormaIDE shows what a plugin asks for before you install it."), page);
    intro->setWordWrap(true);
    intro->setProperty("role", "muted");
    layout->addWidget(intro);
    plugins_ = new QTreeWidget(page);
    plugins_->setObjectName(QStringLiteral("pluginList"));
    plugins_->setHeaderLabels({tr("Plugin"), tr("Version"), tr("Publisher"), tr("Status")});
    plugins_->setRootIsDecorated(false);
    layout->addWidget(plugins_, 1);
    auto* row = new QHBoxLayout();
    auto* install = new QPushButton(tr("Install from folder..."), page);
    auto* toggle = new QPushButton(tr("Enable or disable"), page);
    auto* remove = new QPushButton(tr("Uninstall"), page);
    row->addWidget(install);
    row->addStretch(1);
    row->addWidget(toggle);
    row->addWidget(remove);
    layout->addLayout(row);
    auto selectedId = [this]() -> std::string {
        QTreeWidgetItem* item = plugins_->currentItem();
        return item != nullptr ? fromQ(item->data(0, Qt::UserRole).toString()) : std::string();
    };
    connect(install, &QPushButton::clicked, this, [this] {
        QString folder = QFileDialog::getExistingDirectory(this, tr("Plugin folder"));
        if (folder.isEmpty())
            return;
        auto& manager = AppContext::instance().plugins();
        auto manifest = manager.inspect(paths::fromUtf8(fromQ(folder)));
        if (!manifest) {
            QMessageBox::warning(this, tr("Plugin"), toQ(manifest.error().message));
            return;
        }
        QStringList lines;
        for (const auto& permission : manifest->permissions)
            lines << toQ(plugins::describePermission(permission));
        QString text = tr("Install %1 %2 from %3?").arg(toQ(manifest->name), toQ(manifest->version), toQ(manifest->publisher));
        QString detail = lines.isEmpty() ? tr("It asks for no special permissions.") : tr("It asks to:\n%1").arg(lines.join(QLatin1Char('\n')));
        if (QMessageBox::question(this, tr("Install plugin"), text + QStringLiteral("\n\n") + detail) != QMessageBox::Yes)
            return;
        auto status = manager.install(paths::fromUtf8(fromQ(folder)), manifest->permissions);
        if (!status)
            QMessageBox::warning(this, tr("Plugin"), toQ(status.error().message));
        AppContext::instance().reloadPlugins();
        reloadPlugins();
    });
    connect(toggle, &QPushButton::clicked, this, [this, selectedId] {
        std::string id = selectedId();
        auto& manager = AppContext::instance().plugins();
        const plugins::InstalledPlugin* plugin = manager.find(id);
        if (plugin == nullptr)
            return;
        auto status = manager.setEnabled(id, !plugin->enabled);
        if (!status)
            QMessageBox::warning(this, tr("Plugin"), toQ(status.error().message));
        AppContext::instance().reloadPlugins();
        reloadPlugins();
    });
    connect(remove, &QPushButton::clicked, this, [this, selectedId] {
        std::string id = selectedId();
        if (id.empty() || QMessageBox::question(this, tr("Uninstall plugin"), tr("Uninstall %1?").arg(toQ(id))) != QMessageBox::Yes)
            return;
        auto status = AppContext::instance().plugins().uninstall(id);
        if (!status)
            QMessageBox::warning(this, tr("Plugin"), toQ(status.error().message));
        AppContext::instance().reloadPlugins();
        reloadPlugins();
    });
    reloadPlugins();
    return page;
}

void SettingsDialog::reloadPlugins()
{
    plugins_->clear();
    for (const auto& plugin : AppContext::instance().plugins().plugins()) {
        auto* item = new QTreeWidgetItem(plugins_);
        item->setText(0, toQ(plugin.manifest.name));
        item->setText(1, toQ(plugin.manifest.version));
        item->setText(2, toQ(plugin.manifest.publisher));
        QString status = plugin.tampered ? tr("Files changed since install, disabled") : !plugin.loadError.empty() ? toQ(plugin.loadError) : plugin.enabled ? tr("Enabled") : tr("Disabled");
        item->setText(3, status);
        item->setToolTip(3, status);
        item->setData(0, Qt::UserRole, toQ(plugin.manifest.id));
    }
}

}
