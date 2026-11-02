#include "shell/setup_dialog.h"

#include <QCloseEvent>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "app/app_context.h"
#include "app/project_controller.h"
#include "ui/theme.h"

namespace forma::ui {

namespace {

constexpr int kToolRole = Qt::UserRole;
constexpr int kStackRole = Qt::UserRole + 1;

}

SetupDialog::SetupDialog(QWidget* parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("setupDialog"));
    setWindowTitle(tr("Tools and toolchains"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    resize(920, 620);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 12);
    layout->setSpacing(10);
    auto* intro = new QLabel(tr("FormaIDE uses the same compilers and SDKs you would use by hand. Pick what you want to build and it installs what's missing. Nothing is installed until you press Install."), this);
    intro->setWordWrap(true);
    intro->setProperty("role", "muted");
    layout->addWidget(intro);
    auto* split = new QSplitter(Qt::Horizontal, this);
    tree_ = new QTreeWidget(split);
    tree_->setObjectName(QStringLiteral("setupTree"));
    tree_->setColumnCount(2);
    tree_->setHeaderHidden(true);
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree_->setIndentation(16);
    tree_->setRootIsDecorated(false);
    auto* right = new QWidget(split);
    auto* column = new QVBoxLayout(right);
    column->setContentsMargins(12, 0, 0, 0);
    column->setSpacing(8);
    heading_ = new QLabel(right);
    heading_->setObjectName(QStringLiteral("setupHeading"));
    heading_->setProperty("role", "subheading");
    column->addWidget(heading_);
    detail_ = new QLabel(right);
    detail_->setObjectName(QStringLiteral("setupDetail"));
    detail_->setWordWrap(true);
    detail_->setProperty("role", "muted");
    column->addWidget(detail_);
    auto* row = new QHBoxLayout();
    install_ = new QPushButton(tr("Install"), right);
    install_->setObjectName(QStringLiteral("setupInstall"));
    install_->setProperty("role", "primary");
    cancel_ = new QPushButton(tr("Cancel install"), right);
    cancel_->setObjectName(QStringLiteral("setupCancel"));
    refresh_ = new QPushButton(tr("Check again"), right);
    refresh_->setObjectName(QStringLiteral("setupRefresh"));
    row->addWidget(install_);
    row->addWidget(cancel_);
    row->addStretch(1);
    row->addWidget(refresh_);
    column->addLayout(row);
    progress_ = new QProgressBar(right);
    progress_->setRange(0, 0);
    progress_->setTextVisible(false);
    progress_->setFixedHeight(4);
    progress_->hide();
    column->addWidget(progress_);
    auto* commandsLabel = new QLabel(tr("What will run"), right);
    commandsLabel->setProperty("role", "section");
    column->addWidget(commandsLabel);
    commands_ = new QPlainTextEdit(right);
    commands_->setObjectName(QStringLiteral("setupCommands"));
    commands_->setReadOnly(true);
    commands_->setFont(Theme::instance().codeFont(9));
    commands_->setMaximumHeight(110);
    commands_->setLineWrapMode(QPlainTextEdit::NoWrap);
    column->addWidget(commands_);
    auto* logLabel = new QLabel(tr("Progress"), right);
    logLabel->setProperty("role", "section");
    column->addWidget(logLabel);
    log_ = new QPlainTextEdit(right);
    log_->setObjectName(QStringLiteral("setupLog"));
    log_->setReadOnly(true);
    log_->setFont(Theme::instance().codeFont(9));
    log_->setMaximumBlockCount(5000);
    log_->setLineWrapMode(QPlainTextEdit::NoWrap);
    column->addWidget(log_, 1);
    split->setStretchFactor(0, 4);
    split->setStretchFactor(1, 6);
    split->setSizes({360, 540});
    layout->addWidget(split, 1);
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this] {
        pendingPlan_.reset();
        describeCurrent();
    });
    connect(refresh_, &QPushButton::clicked, this, [this] { refresh(); });
    connect(cancel_, &QPushButton::clicked, this, [this] {
        queue_.clear();
        if (job_)
            job_->cancel();
    });
    connect(install_, &QPushButton::clicked, this, [this] {
        if (pendingPlan_) {
            build::InstallPlan plan = *pendingPlan_;
            installPlan(std::move(plan));
            return;
        }
        installTools(missingIn(tree_->currentItem()));
    });
    build::refreshEnvironmentFromSystem();
    refresh();
    cancel_->hide();
}

SetupDialog::~SetupDialog() = default;

bool SetupDialog::busy() const
{
    return job_ && job_->running();
}

QString SetupDialog::logText() const
{
    return log_->toPlainText();
}

void SetupDialog::refresh()
{
    auto& tools = AppContext::instance().tools();
    QString selectedStack;
    QString selectedTool;
    if (QTreeWidgetItem* item = tree_->currentItem()) {
        selectedStack = item->data(0, kStackRole).toString();
        selectedTool = item->data(0, kToolRole).toString();
    }
    tree_->clear();
    readyStacks_ = 0;
    totalStacks_ = 0;
    const ThemeColors& c = colors();
    QTreeWidgetItem* restore = nullptr;
    for (const auto& stack : build::toolStacksForHost()) {
        auto* parent = new QTreeWidgetItem(tree_);
        parent->setText(0, toQ(stack.name));
        parent->setData(0, kStackRole, toQ(stack.id));
        QFont bold = parent->font(0);
        bold.setWeight(QFont::DemiBold);
        parent->setFont(0, bold);
        bool ready = true;
        auto addTool = [&](const std::string& id, bool required) {
            build::ToolInfo info = tools.detect(id, true);
            auto* child = new QTreeWidgetItem(parent);
            child->setText(0, toQ(info.name.empty() ? id : info.name) + (required ? QString() : tr("  (optional)")));
            child->setData(0, kToolRole, toQ(id));
            child->setData(0, kStackRole, toQ(stack.id));
            child->setText(1, info.available ? (info.version.empty() ? tr("Installed") : toQ(info.version)) : tr("Not installed"));
            child->setForeground(1, info.available ? c.success : (required ? c.warning : c.textFaint));
            child->setIcon(0, Theme::instance().icon(info.available ? QStringLiteral("check") : QStringLiteral("minus"), info.available ? c.success : c.textFaint));
            if (required && !info.available)
                ready = false;
            if (toQ(id) == selectedTool && toQ(stack.id) == selectedStack)
                restore = child;
        };
        for (const auto& id : stack.required)
            addTool(id, true);
        for (const auto& id : stack.optional)
            addTool(id, false);
        parent->setText(1, ready ? tr("Ready") : tr("Needs setup"));
        parent->setForeground(1, ready ? c.success : c.warning);
        ++totalStacks_;
        readyStacks_ += ready ? 1 : 0;
        if (selectedTool.isEmpty() && toQ(stack.id) == selectedStack)
            restore = parent;
    }
    tree_->expandAll();
    tree_->setCurrentItem(restore != nullptr ? restore : tree_->topLevelItem(0));
    describeCurrent();
}

QStringList SetupDialog::missingIn(QTreeWidgetItem* item) const
{
    QStringList missing;
    if (item == nullptr)
        return missing;
    auto& tools = AppContext::instance().tools();
    QString tool = item->data(0, kToolRole).toString();
    if (!tool.isEmpty()) {
        missing << tool;
        return missing;
    }
    const build::ToolStack* stack = build::findToolStack(fromQ(item->data(0, kStackRole).toString()));
    if (stack == nullptr)
        return missing;
    for (const auto& id : stack->required) {
        if (!tools.isAvailable(id))
            missing << toQ(id);
    }
    if (missing.isEmpty()) {
        for (const auto& id : stack->optional) {
            if (!tools.isAvailable(id))
                missing << toQ(id);
        }
    }
    return missing;
}

void SetupDialog::describeCurrent()
{
    QTreeWidgetItem* item = tree_->currentItem();
    if (pendingPlan_) {
        heading_->setText(toQ(pendingPlan_->name));
        detail_->setText(toQ(pendingPlan_->note));
        commands_->setPlainText(toQ(pendingPlan_->preview()));
        install_->setText(tr("Install %1").arg(toQ(pendingPlan_->name)));
        install_->setEnabled(pendingPlan_->automatic() && !busy());
        return;
    }
    if (item == nullptr)
        return;
    auto& tools = AppContext::instance().tools();
    QString tool = item->data(0, kToolRole).toString();
    const build::ToolStack* stack = build::findToolStack(fromQ(item->data(0, kStackRole).toString()));
    QStringList missing = missingIn(item);
    QStringList preview;
    QStringList notes;
    bool anyAutomatic = false;
    std::string manual;
    std::vector<std::string> wanted;
    for (const auto& id : missing)
        wanted.push_back(fromQ(id));
    for (const auto& id : build::installOrder(wanted, tools)) {
        build::InstallPlan plan = build::planToolInstall(id, tools);
        anyAutomatic = anyAutomatic || plan.automatic();
        preview << toQ(plan.preview()).trimmed();
        if (!plan.note.empty())
            notes << toQ(plan.note);
        if (!plan.manualUrl.empty())
            manual = plan.manualUrl;
    }
    if (!tool.isEmpty()) {
        build::ToolInfo info = tools.detect(fromQ(tool));
        heading_->setText(toQ(info.name.empty() ? fromQ(tool) : info.name));
        QString status = info.available ? tr("Installed at %1.").arg(toQ(info.path)) : toQ(info.note);
        if (!info.available)
            status = (status + QLatin1Char(' ') + notes.join(QLatin1Char(' '))).trimmed();
        detail_->setText(status);
        install_->setText(info.available ? tr("Reinstall") : tr("Install"));
    } else if (stack != nullptr) {
        heading_->setText(toQ(stack->name));
        QString text = toQ(stack->description) + QStringLiteral(". ") + notes.join(QLatin1Char(' '));
        detail_->setText(text.trimmed());
        install_->setText(missing.isEmpty() ? tr("Everything is installed") : tr("Install what's missing"));
    }
    commands_->setPlainText(preview.join(QLatin1Char('\n')).trimmed());
    install_->setEnabled(!busy() && !missing.isEmpty() && anyAutomatic);
    if (!manual.empty() && !anyAutomatic) {
        install_->setText(tr("Open the download page"));
        install_->setEnabled(true);
        disconnect(install_, nullptr, this, nullptr);
        QString url = toQ(manual);
        connect(install_, &QPushButton::clicked, this, [url] { QDesktopServices::openUrl(QUrl(url)); });
    }
}

void SetupDialog::focusTools(const QStringList& tools, const QString& reason)
{
    pendingPlan_.reset();
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* parent = tree_->topLevelItem(i);
        for (int c = 0; c < parent->childCount(); ++c) {
            if (tools.contains(parent->child(c)->data(0, kToolRole).toString())) {
                tree_->setCurrentItem(tools.size() > 1 ? parent : parent->child(c));
                describeCurrent();
                if (!reason.isEmpty())
                    detail_->setText(reason + QLatin1Char(' ') + detail_->text());
                return;
            }
        }
    }
    describeCurrent();
}

void SetupDialog::focusServer(const QString& languageName, const QString& serverCommand, const QString& hint)
{
    pendingPlan_ = build::planServerInstall(fromQ(languageName), fromQ(serverCommand), fromQ(hint));
    describeCurrent();
}

void SetupDialog::log(const QString& text, bool isError)
{
    Q_UNUSED(isError);
    log_->appendPlainText(text);
}

void SetupDialog::installPlan(build::InstallPlan plan)
{
    pendingPlan_.reset();
    anyFailed_ = false;
    auto& tools = AppContext::instance().tools();
    std::vector<std::string> dependencies;
    for (const auto& id : plan.dependencies) {
        if (!tools.isAvailable(id))
            dependencies.push_back(id);
    }
    for (const auto& id : build::installOrder(dependencies, tools))
        queue_.push_back(build::planToolInstall(id, tools));
    queue_.push_back(std::move(plan));
    if (!busy())
        startNext();
}

void SetupDialog::installTools(const QStringList& tools)
{
    auto& detector = AppContext::instance().tools();
    anyFailed_ = false;
    std::vector<std::string> wanted;
    for (const auto& id : tools)
        wanted.push_back(fromQ(id));
    for (const auto& id : build::installOrder(wanted, detector)) {
        build::InstallPlan plan = build::planToolInstall(id, detector);
        if (plan.automatic())
            queue_.push_back(std::move(plan));
        else
            log(tr("%1 has to be installed by hand: %2").arg(toQ(plan.name), toQ(plan.note)));
    }
    if (!busy())
        startNext();
}

void SetupDialog::startNext()
{
    if (queue_.empty()) {
        progress_->hide();
        cancel_->hide();
        refresh();
        Q_EMIT toolsChanged();
        Q_EMIT installFinished(!anyFailed_);
        return;
    }
    build::InstallPlan plan = std::move(queue_.front());
    queue_.pop_front();
    auto& tools = AppContext::instance().tools();
    bool hasStep = plan.automatic();
    if (!plan.id.empty() && !plan.dependencies.empty()) {
        build::InstallPlan fresh = build::planToolInstall(plan.id, tools);
        if (fresh.automatic() && fresh.name == plan.name)
            plan = std::move(fresh);
    }
    if (!hasStep) {
        startNext();
        return;
    }
    current_ = toQ(plan.name);
    log(tr("Installing %1").arg(current_));
    if (plan.mayAskForAdmin)
        log(tr("Windows may ask for permission. Look for the prompt if nothing seems to happen."));
    progress_->show();
    cancel_->show();
    install_->setEnabled(false);
    job_ = std::make_unique<build::SetupJob>();
    QPointer<SetupDialog> self(this);
    build::SetupCallbacks callbacks;
    callbacks.onStep = [self](std::string_view label) {
        QString text = QString::fromUtf8(label.data(), static_cast<qsizetype>(label.size()));
        QMetaObject::invokeMethod(qApp, [self, text] { if (self) self->log(QStringLiteral("== %1 ==").arg(text)); }, Qt::QueuedConnection);
    };
    callbacks.onOutput = [self](std::string_view data, bool isError) {
        QString text = QString::fromUtf8(data.data(), static_cast<qsizetype>(data.size()));
        QMetaObject::invokeMethod(qApp, [self, text, isError] {
            if (!self)
                return;
            for (const auto& line : text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts)) {
                if (!line.trimmed().isEmpty() && !line.contains(QStringLiteral("##")))
                    self->log(line.trimmed(), isError);
            }
        }, Qt::QueuedConnection);
    };
    callbacks.onFinished = [self](bool success, std::string_view message) {
        QString text = QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size()));
        QMetaObject::invokeMethod(qApp, [self, success, text] {
            if (!self)
                return;
            self->job_->wait();
            if (success) {
                self->log(tr("%1 is installed.").arg(self->current_));
            } else {
                self->anyFailed_ = true;
                self->queue_.clear();
                self->log(tr("%1 was not installed: %2").arg(self->current_, text), true);
            }
            self->startNext();
        }, Qt::QueuedConnection);
    };
    auto started = job_->start(std::move(plan), callbacks);
    if (!started) {
        anyFailed_ = true;
        log(toQ(started.error().message), true);
        startNext();
    }
}

void SetupDialog::closeEvent(QCloseEvent* event)
{
    if (busy()) {
        if (QMessageBox::question(this, tr("Stop installing?"), tr("An install is still running. Cancel it and close?")) != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        queue_.clear();
        job_->cancel();
        job_->wait();
    }
    event->accept();
}

}
