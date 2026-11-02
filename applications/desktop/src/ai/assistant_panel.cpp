#include "ai/assistant_panel.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QVBoxLayout>

#include "app/app_context.h"
#include "forma/codegen/text_diff.h"
#include "forma/core/text.h"
#include "forma/ir/serialization.h"
#include "ui/theme.h"

namespace forma::ui {

namespace {

class QtHttpCall final : public ai::IHttpCall {
public:
    explicit QtHttpCall(QNetworkReply* reply) : reply_(reply) {}
    void cancel() override
    {
        if (reply_)
            reply_->abort();
    }

private:
    QPointer<QNetworkReply> reply_;
};

}

std::shared_ptr<ai::IHttpCall> QtHttpTransport::send(const ai::HttpRequest& request, std::function<void(std::string_view)> onChunk, std::function<void(int, const std::string&)> onDone)
{
    QNetworkRequest networkRequest{QUrl(toQ(request.url))};
    for (const auto& [key, value] : request.headers)
        networkRequest.setRawHeader(QByteArray::fromStdString(key), QByteArray::fromStdString(value));
    networkRequest.setTransferTimeout(600000);
    QNetworkReply* reply = request.method == "GET" ? manager_.get(networkRequest) : manager_.post(networkRequest, QByteArray::fromStdString(request.body));
    QObject::connect(reply, &QNetworkReply::readyRead, reply, [reply, onChunk] {
        QByteArray data = reply->readAll();
        onChunk(std::string_view(data.constData(), static_cast<std::size_t>(data.size())));
    });
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, onChunk, onDone] {
        QByteArray rest = reply->readAll();
        if (!rest.isEmpty())
            onChunk(std::string_view(rest.constData(), static_cast<std::size_t>(rest.size())));
        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        std::string error;
        if (status == 0)
            error = reply->error() == QNetworkReply::OperationCanceledError ? "Stopped" : "Cannot reach the provider: " + reply->errorString().toStdString();
        onDone(status, error);
        reply->deleteLater();
    });
    return std::make_shared<QtHttpCall>(reply);
}

AssistantPanel::AssistantPanel(ProjectController* controller, EditorArea* editors, QWidget* parent) : QWidget(parent), controller_(controller), editors_(editors)
{
    transport_ = std::make_shared<QtHttpTransport>();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);
    auto* top = new QHBoxLayout();
    provider_ = new QComboBox(this);
    provider_->setObjectName(QStringLiteral("assistantProvider"));
    provider_->setToolTip(tr("Set up providers in Settings, Assistant"));
    top->addWidget(provider_, 1);
    autonomy_ = new QComboBox(this);
    autonomy_->setObjectName(QStringLiteral("assistantAutonomy"));
    autonomy_->addItem(tr("Suggestions only"), 1);
    autonomy_->addItem(tr("Propose changes"), 2);
    autonomy_->addItem(tr("Apply approved files"), 3);
    autonomy_->addItem(tr("Also run builds and tests"), 4);
    autonomy_->setCurrentIndex(1);
    autonomy_->setToolTip(tr("How much the assistant may do. Deleting outside the project, uploads and reading secrets are never allowed."));
    top->addWidget(autonomy_);
    layout->addLayout(top);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("assistantStatus"));
    status_->setProperty("role", "muted");
    status_->setWordWrap(true);
    layout->addWidget(status_);
    setup_ = new QPushButton(tr("Set up the assistant..."), this);
    setup_->setObjectName(QStringLiteral("assistantSetup"));
    connect(setup_, &QPushButton::clicked, this, &AssistantPanel::setupRequested);
    layout->addWidget(setup_);
    auto* split = new QSplitter(Qt::Vertical, this);
    conversation_ = new QTextBrowser(split);
    conversation_->setObjectName(QStringLiteral("assistantConversation"));
    conversation_->setOpenExternalLinks(true);
    conversation_->setFrameShape(QFrame::NoFrame);
    auto* inputHolder = new QWidget(split);
    auto* inputLayout = new QVBoxLayout(inputHolder);
    inputLayout->setContentsMargins(0, 0, 0, 0);
    inputLayout->setSpacing(4);
    auto* contextLabel = new QLabel(tr("Sent with your next message (uncheck anything you don't want to share)"), inputHolder);
    contextLabel->setProperty("role", "muted");
    inputLayout->addWidget(contextLabel);
    context_ = new QTreeWidget(inputHolder);
    context_->setObjectName(QStringLiteral("assistantContext"));
    context_->setHeaderHidden(true);
    context_->setRootIsDecorated(false);
    context_->setMaximumHeight(84);
    inputLayout->addWidget(context_);
    input_ = new QPlainTextEdit(inputHolder);
    input_->setObjectName(QStringLiteral("assistantInput"));
    input_->setPlaceholderText(tr("Describe what you want. For example: add a settings screen with a dark theme switch."));
    input_->setMaximumHeight(80);
    inputLayout->addWidget(input_);
    auto* actions = new QHBoxLayout();
    auto* fix = new QPushButton(tr("Explain build errors"), inputHolder);
    fix->setObjectName(QStringLiteral("assistantFixBuild"));
    actions->addWidget(fix);
    actions->addStretch(1);
    stop_ = new QPushButton(tr("Stop"), inputHolder);
    stop_->setEnabled(false);
    actions->addWidget(stop_);
    send_ = new QPushButton(tr("Send"), inputHolder);
    send_->setObjectName(QStringLiteral("assistantSend"));
    send_->setProperty("role", "primary");
    actions->addWidget(send_);
    inputLayout->addLayout(actions);
    changesBox_ = new QWidget(split);
    auto* changesLayout = new QVBoxLayout(changesBox_);
    changesLayout->setContentsMargins(0, 0, 0, 0);
    changesLayout->setSpacing(3);
    auto* changesLabel = new QLabel(tr("Proposed changes"), changesBox_);
    changesLabel->setProperty("role", "section");
    changesLayout->addWidget(changesLabel);
    auto* changesSplit = new QSplitter(Qt::Horizontal, changesBox_);
    changes_ = new QTreeWidget(changesSplit);
    changes_->setObjectName(QStringLiteral("assistantChanges"));
    changes_->setHeaderLabels({tr("Change"), tr("Lines")});
    changes_->setRootIsDecorated(false);
    diff_ = new DiffView(changesSplit);
    diff_->setObjectName(QStringLiteral("assistantDiff"));
    changesLayout->addWidget(changesSplit, 1);
    auto* changeActions = new QHBoxLayout();
    rollback_ = new QPushButton(tr("Undo last applied changes"), changesBox_);
    rollback_->setEnabled(false);
    changeActions->addWidget(rollback_);
    changeActions->addStretch(1);
    auto* discard = new QPushButton(tr("Discard all"), changesBox_);
    changeActions->addWidget(discard);
    apply_ = new QPushButton(tr("Apply checked changes"), changesBox_);
    apply_->setObjectName(QStringLiteral("assistantApply"));
    apply_->setProperty("role", "primary");
    changeActions->addWidget(apply_);
    changesLayout->addLayout(changeActions);
    changesBox_->hide();
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 0);
    split->setStretchFactor(2, 0);
    layout->addWidget(split, 1);

    connect(send_, &QPushButton::clicked, this, [this] {
        QString text = input_->toPlainText().trimmed();
        if (!text.isEmpty()) {
            input_->clear();
            ask(text);
        }
    });
    connect(stop_, &QPushButton::clicked, this, [this] {
        if (session_)
            session_->cancel();
        setBusy(false);
    });
    connect(fix, &QPushButton::clicked, this, &AssistantPanel::askToFixBuild);
    connect(apply_, &QPushButton::clicked, this, &AssistantPanel::applyApproved);
    connect(discard, &QPushButton::clicked, this, [this] {
        if (session_)
            session_->staged() = {};
        showStaged();
    });
    connect(rollback_, &QPushButton::clicked, this, [this] {
        if (lastCheckpoint_.isEmpty() || !controller_->hasProject())
            return;
        if (QMessageBox::question(this, tr("Undo assistant changes"), tr("Restore the project to how it was before the assistant's last changes?")) != QMessageBox::Yes)
            return;
        auto status = ai::rollbackTo(*controller_->session(), fromQ(lastCheckpoint_));
        controller_->reportStatus(status ? tr("The assistant's changes were undone") : toQ(status.error().message));
        lastCheckpoint_.clear();
        rollback_->setEnabled(false);
    });
    connect(changes_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        if (item == nullptr || !session_)
            return;
        int index = item->data(0, Qt::UserRole).toInt();
        bool isFile = item->data(0, Qt::UserRole + 1).toBool();
        if (isFile && index < static_cast<int>(session_->staged().files.size())) {
            const auto& file = session_->staged().files[static_cast<std::size_t>(index)];
            diff_->showDiff(toQ(codegen::unifiedDiff(file.before, file.after, file.path, file.path)));
        } else if (!isFile && index < static_cast<int>(session_->staged().design.size())) {
            diff_->showDiff(toQ(dumpJson(session_->staged().design[static_cast<std::size_t>(index)].operation)));
        }
    });
    connect(changes_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item) {
        if (!session_)
            return;
        int index = item->data(0, Qt::UserRole).toInt();
        bool on = item->checkState(0) == Qt::Checked;
        if (item->data(0, Qt::UserRole + 1).toBool()) {
            if (index < static_cast<int>(session_->staged().files.size()))
                session_->staged().files[static_cast<std::size_t>(index)].approved = on;
        } else if (index < static_cast<int>(session_->staged().design.size())) {
            session_->staged().design[static_cast<std::size_t>(index)].approved = on;
        }
    });
    connect(context_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem*) {});
    connect(editors_, &EditorArea::currentFileChanged, this, [this](const QString&) { refreshContext(); });
    connect(controller_, &ProjectController::selectionChanged, this, &AssistantPanel::refreshContext);
    connect(controller_, &ProjectController::projectOpened, this, [this] {
        session_.reset();
        conversation_->clear();
        refreshContext();
        reloadProviders();
    });
    connect(controller_, &ProjectController::projectClosed, this, [this] {
        session_.reset();
        conversation_->clear();
    });
    reloadProviders();
}

void AssistantPanel::reloadProviders()
{
    QString previous = provider_->currentData().toString();
    provider_->clear();
    Json saved = AppContext::instance().settings().get("assistant.providers", Json::array());
    if (saved.is_array()) {
        for (const auto& item : saved) {
            ai::ProviderConfig config = ai::providerConfigFromJson(item);
            provider_->addItem(toQ(config.label) + (config.model.empty() ? QString() : QStringLiteral(" - ") + toQ(config.model)), toQ(config.id));
        }
    }
    int index = provider_->findData(previous);
    if (index >= 0)
        provider_->setCurrentIndex(index);
    provider_->setVisible(provider_->count() > 0);
    autonomy_->setVisible(provider_->count() > 0);
    setup_->setVisible(provider_->count() == 0);
    if (provider_->count() == 0) {
        status_->setText(tr("The assistant is optional and off until you set it up. In Settings, Assistant, pick a local model or add your own API key. Nothing leaves this computer until then."));
        send_->setEnabled(false);
    } else {
        status_->setText(tr("Only what you check below is sent. The assistant can read project files you allow, and all changes wait for your approval."));
        send_->setEnabled(true);
    }
}

ai::ProviderConfig AssistantPanel::currentConfig() const
{
    Json saved = AppContext::instance().settings().get("assistant.providers", Json::array());
    QString id = provider_->currentData().toString();
    if (saved.is_array()) {
        for (const auto& item : saved) {
            if (jsonString(item, "id") == fromQ(id))
                return ai::providerConfigFromJson(item);
        }
    }
    return {};
}

void AssistantPanel::ensureSession()
{
    ai::ProviderConfig config = currentConfig();
    std::string key;
    if (!config.credentialAccount.empty()) {
        auto stored = AppContext::instance().credentials().get(config.credentialAccount);
        if (stored && stored->has_value())
            key = **stored;
    }
    std::shared_ptr<ai::IAIProvider> provider = ai::createProvider(config, key, transport_);
    if (session_ && session_->busy())
        return;
    ai::ChangeSet staged;
    session_ = std::make_shared<ai::AgentSession>(*controller_->session(), AppContext::instance().generators(), provider);
    session_->setBuildErrorsProvider([this] { return fromQ(controller_->buildErrorsText()); });
    ai::AgentCallbacks callbacks;
    QPointer<AssistantPanel> self(this);
    callbacks.onText = [self](std::string_view text) {
        if (self)
            self->appendStreaming(QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())));
    };
    callbacks.onTool = [self](const std::string& tool, const Json& input) {
        if (!self)
            return;
        QString detail = toQ(jsonString(input, "path", jsonString(input, "description", jsonString(input, "query", jsonString(input, "command")))));
        self->addMessage(self->tr("Assistant"), self->tr("<i>%1 %2</i>").arg(toQ(tool), detail.toHtmlEscaped()));
        if (tool == "run_command")
            Q_EMIT self->controller_->automationLogged(self->tr("Assistant"), detail);
    };
    callbacks.onChangesStaged = [self](const ai::ChangeSet&) {
        if (self)
            self->showStaged();
    };
    callbacks.onFinished = [self](const std::string&) {
        if (self) {
            self->streamingOpen_ = false;
            self->setBusy(false);
            self->showStaged();
        }
    };
    callbacks.onError = [self](const Error& error) {
        if (self) {
            self->streamingOpen_ = false;
            self->addMessage(self->tr("Problem"), toQ(error.message).toHtmlEscaped());
            self->setBusy(false);
        }
    };
    callbacks.onCommandApproval = [self](const std::string& command, const ai::CommandCheck& check, std::function<void(bool)> decide) {
        if (!self) {
            decide(false);
            return;
        }
        QMessageBox box(self);
        box.setIcon(check.dangerous ? QMessageBox::Warning : QMessageBox::Question);
        box.setWindowTitle(self->tr("Run this command?"));
        box.setText(self->tr("The assistant wants to run:\n\n%1").arg(toQ(command)));
        box.setInformativeText(toQ(check.reason));
        box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        box.setDefaultButton(QMessageBox::No);
        decide(box.exec() == QMessageBox::Yes);
    };
    session_->setCallbacks(callbacks);
}

void AssistantPanel::addMessage(const QString& who, const QString& text)
{
    streamingOpen_ = false;
    conversation_->append(QStringLiteral("<p><b>%1</b><br>%2</p>").arg(who.toHtmlEscaped(), text));
}

void AssistantPanel::appendStreaming(const QString& text)
{
    QTextCursor cursor(conversation_->document());
    cursor.movePosition(QTextCursor::End);
    if (!streamingOpen_) {
        conversation_->append(QStringLiteral("<p><b>%1</b></p>").arg(tr("Assistant")));
        cursor.movePosition(QTextCursor::End);
        cursor.insertBlock();
        streamingOpen_ = true;
    }
    QTextCharFormat plain;
    cursor.insertText(text, plain);
    conversation_->verticalScrollBar()->setValue(conversation_->verticalScrollBar()->maximum());
}

void AssistantPanel::refreshContext()
{
    QSignalBlocker blocker(context_);
    std::map<QString, bool> previous;
    for (int i = 0; i < context_->topLevelItemCount(); ++i)
        previous[context_->topLevelItem(i)->data(0, Qt::UserRole).toString()] = context_->topLevelItem(i)->checkState(0) == Qt::Checked;
    context_->clear();
    auto add = [&](const QString& id, const QString& label, bool fallback) {
        auto* item = new QTreeWidgetItem(context_);
        item->setText(0, label);
        item->setData(0, Qt::UserRole, id);
        auto it = previous.find(id);
        item->setCheckState(0, (it != previous.end() ? it->second : fallback) ? Qt::Checked : Qt::Unchecked);
    };
    if (!controller_->hasProject())
        return;
    QString path = editors_->currentPath();
    if (!path.isEmpty())
        add(QStringLiteral("file"), tr("Current file: %1").arg(path), true);
    if (editors_->currentEditor() != nullptr && !editors_->currentEditor()->selectedText().isEmpty())
        add(QStringLiteral("selection"), tr("Selected text"), true);
    if (!controller_->currentScreen().isEmpty())
        add(QStringLiteral("screen"), tr("Current screen: %1").arg(controller_->currentScreen()), true);
    if (!controller_->buildErrorsText().isEmpty())
        add(QStringLiteral("errors"), tr("Build errors"), true);
    add(QStringLiteral("files"), tr("List of project files"), false);
}

void AssistantPanel::ask(const QString& prompt)
{
    if (!controller_->hasProject())
        return;
    if (provider_->count() == 0) {
        addMessage(tr("Problem"), tr("Set up a provider in Settings, Assistant first."));
        return;
    }
    if (session_ && session_->busy())
        return;
    ensureSession();
    ai::AgentOptions options;
    options.autonomy = static_cast<ai::AutonomyLevel>(autonomy_->currentData().toInt());
    session_->setOptions(options);
    auto& context = session_->context();
    context.clear();
    for (int i = 0; i < context_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = context_->topLevelItem(i);
        if (item->checkState(0) != Qt::Checked)
            continue;
        QString id = item->data(0, Qt::UserRole).toString();
        if (id == QStringLiteral("file") && editors_->currentEditor() != nullptr && !ai::isSensitivePath(fromQ(editors_->currentPath()))) {
            context.add({ai::ContextItem::Kind::File, fromQ(editors_->currentPath()), fromQ(editors_->currentEditor()->text()), true});
        } else if (id == QStringLiteral("selection") && editors_->currentEditor() != nullptr) {
            context.add({ai::ContextItem::Kind::Selection, "selected text", fromQ(editors_->currentEditor()->selectedText()), true});
        } else if (id == QStringLiteral("screen")) {
            if (const ir::Screen* screen = controller_->model()->findScreen(fromQ(controller_->currentScreen())))
                context.add({ai::ContextItem::Kind::Design, "screen " + screen->root.id, dumpJson(ir::screenToJson(*screen)), true});
        } else if (id == QStringLiteral("errors")) {
            context.add({ai::ContextItem::Kind::Diagnostics, "build errors", fromQ(controller_->buildErrorsText()), true});
        } else if (id == QStringLiteral("files")) {
            ListOptions listOptions;
            listOptions.recursive = true;
            listOptions.ignoredNames = defaultIgnoredNames();
            auto entries = controller_->session()->filesystem().list("", listOptions);
            std::string tree;
            if (entries) {
                for (const auto& entry : *entries) {
                    if (!entry.directory)
                        tree += entry.path + "\n";
                }
            }
            context.add({ai::ContextItem::Kind::FileTree, "project files", tree, true});
        }
    }
    addMessage(tr("You"), prompt.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
    setBusy(true);
    session_->send(fromQ(prompt));
}

void AssistantPanel::askToFixBuild()
{
    refreshContext();
    QString errors = controller_->buildErrorsText();
    if (errors.isEmpty()) {
        controller_->reportStatus(tr("The last build has no errors to look at"));
        return;
    }
    ask(tr("Explain these build errors and propose a fix."));
}

QString AssistantPanel::transcript() const
{
    return conversation_->toPlainText();
}

int AssistantPanel::stagedCount() const
{
    return session_ ? static_cast<int>(session_->staged().files.size() + session_->staged().design.size()) : 0;
}

void AssistantPanel::setBusy(bool busy)
{
    send_->setEnabled(!busy && provider_->count() > 0);
    stop_->setEnabled(busy);
    if (busy)
        status_->setText(tr("Working..."));
    else if (provider_->count() > 0)
        status_->setText(tr("Only what you check below is sent. All changes wait for your approval."));
}

void AssistantPanel::showStaged()
{
    QSignalBlocker blocker(changes_);
    changes_->clear();
    diff_->clear();
    bool any = session_ && !session_->staged().empty();
    changesBox_->setVisible(any);
    if (!any)
        return;
    const ai::ChangeSet& staged = session_->staged();
    int index = 0;
    for (const auto& file : staged.files) {
        auto* item = new QTreeWidgetItem(changes_);
        QString kind = file.kind == ai::StagedFile::Kind::Create ? tr("New") : file.kind == ai::StagedFile::Kind::Delete ? tr("Remove") : tr("Edit");
        item->setText(0, QStringLiteral("%1: %2").arg(kind, toQ(file.path)));
        auto stats = codegen::diffStats(file.before, file.after);
        item->setText(1, QStringLiteral("+%1 -%2").arg(stats.added).arg(stats.removed));
        item->setCheckState(0, file.approved ? Qt::Checked : Qt::Unchecked);
        item->setData(0, Qt::UserRole, index++);
        item->setData(0, Qt::UserRole + 1, true);
    }
    index = 0;
    for (const auto& edit : staged.design) {
        auto* item = new QTreeWidgetItem(changes_);
        item->setText(0, tr("Design: %1").arg(toQ(edit.description)));
        item->setCheckState(0, edit.approved ? Qt::Checked : Qt::Unchecked);
        item->setData(0, Qt::UserRole, index++);
        item->setData(0, Qt::UserRole + 1, false);
    }
    changes_->resizeColumnToContents(0);
    if (changes_->topLevelItemCount() > 0)
        changes_->setCurrentItem(changes_->topLevelItem(0));
}

void AssistantPanel::applyApproved()
{
    if (!session_ || !controller_->hasProject())
        return;
    auto result = ai::applyChangeSet(*controller_->session(), session_->staged(), &AppContext::instance().generators());
    if (!result) {
        addMessage(tr("Problem"), toQ(result.error().message).toHtmlEscaped());
        return;
    }
    lastCheckpoint_ = toQ(result->checkpoint);
    rollback_->setEnabled(true);
    session_->staged() = {};
    QString summary = tr("Applied %n file(s).", nullptr, static_cast<int>(result->written.size()));
    for (const auto& failure : result->failures)
        summary += QStringLiteral("<br>") + toQ(failure).toHtmlEscaped();
    addMessage(tr("FormaIDE"), summary);
    changes_->clear();
    diff_->clear();
    changesBox_->setVisible(false);
    rollback_->setParent(rollback_->parentWidget());
    controller_->reportStatus(tr("Assistant changes applied"));
    controller_->notifyFilesChanged();
}

}
