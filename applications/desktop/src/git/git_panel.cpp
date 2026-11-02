#include "git/git_panel.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSplitter>
#include <QTextBlock>
#include <QToolButton>
#include <QVBoxLayout>
#include <thread>

#include "ui/theme.h"

namespace forma::ui {

DiffView::DiffView(QWidget* parent) : QPlainTextEdit(parent)
{
    setReadOnly(true);
    setFont(Theme::instance().codeFont(9));
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setFrameShape(QFrame::NoFrame);
}

void DiffView::showDiff(const QString& diff)
{
    clear();
    QTextCursor cursor(document());
    QTextCharFormat normal;
    normal.setForeground(colors().text);
    QTextCharFormat added;
    added.setForeground(colors().success);
    QTextCharFormat removed;
    removed.setForeground(colors().danger);
    QTextCharFormat hunk;
    hunk.setForeground(colors().accent);
    bool first = true;
    for (const auto& line : diff.split(QLatin1Char('\n'))) {
        if (!first)
            cursor.insertBlock();
        first = false;
        const QTextCharFormat& format = line.startsWith(QStringLiteral("@@")) ? hunk : line.startsWith(QLatin1Char('+')) ? added : line.startsWith(QLatin1Char('-')) ? removed : normal;
        cursor.insertText(line, format);
    }
    moveCursor(QTextCursor::Start);
}

GitPanel::GitPanel(ProjectController* controller, QWidget* parent) : QWidget(parent), controller_(controller)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 0);
    layout->setSpacing(4);
    initButton_ = new QPushButton(tr("Start a Git repository here"), this);
    initButton_->setObjectName(QStringLiteral("gitInit"));
    layout->addWidget(initButton_);
    repoControls_ = new QWidget(this);
    auto* controls = new QVBoxLayout(repoControls_);
    controls->setContentsMargins(0, 0, 0, 0);
    controls->setSpacing(4);
    auto* top = new QHBoxLayout();
    branch_ = new QComboBox(repoControls_);
    branch_->setObjectName(QStringLiteral("gitBranch"));
    top->addWidget(branch_, 1);
    auto add = [&](const QString& iconName, const QString& tip, std::function<void()> action) {
        auto* button = new QToolButton(repoControls_);
        button->setIcon(icon(iconName));
        button->setToolTip(tip);
        connect(button, &QToolButton::clicked, this, action);
        top->addWidget(button);
        return button;
    };
    add(QStringLiteral("refresh"), tr("Refresh"), [this] { refresh(); });
    auto* more = new QToolButton(repoControls_);
    more->setIcon(icon(QStringLiteral("more")));
    more->setToolTip(tr("Pull, push, branches and more"));
    more->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(more);
    menu->addAction(tr("Fetch"), this, [this] { runAsync(tr("Fetch"), [](git::GitClient& git) { return git.fetch(); }); });
    menu->addAction(tr("Pull"), this, [this] { runAsync(tr("Pull"), [](git::GitClient& git) { return git.pull(); }); });
    menu->addAction(tr("Push"), this, [this] {
        bool noUpstream = branchText_.contains(tr("no upstream"));
        runAsync(tr("Push"), [noUpstream](git::GitClient& git) { return git.push(noUpstream); });
    });
    menu->addSeparator();
    menu->addAction(tr("New branch..."), this, [this] {
        bool accepted = false;
        QString name = QInputDialog::getText(this, tr("New branch"), tr("Name"), QLineEdit::Normal, QString(), &accepted);
        if (accepted && !name.trimmed().isEmpty())
            runAsync(tr("Create branch"), [name](git::GitClient& git) { return git.createBranch(fromQ(name.trimmed())); });
    });
    menu->addAction(tr("Merge a branch into this one..."), this, [this] {
        QStringList names;
        for (int i = 0; i < branch_->count(); ++i)
            names << branch_->itemText(i);
        bool accepted = false;
        QString name = QInputDialog::getItem(this, tr("Merge"), tr("Merge from"), names, 0, false, &accepted);
        if (accepted)
            runAsync(tr("Merge"), [name](git::GitClient& git) { return git.merge(fromQ(name)); });
    });
    menu->addAction(tr("Rebase this branch onto..."), this, [this] {
        QStringList names;
        for (int i = 0; i < branch_->count(); ++i)
            names << branch_->itemText(i);
        bool accepted = false;
        QString name = QInputDialog::getItem(this, tr("Rebase"), tr("Rebase onto"), names, 0, false, &accepted);
        if (accepted)
            runAsync(tr("Rebase"), [name](git::GitClient& git) { return git.rebase(fromQ(name)); });
    });
    menu->addAction(tr("Continue rebase"), this, [this] { runAsync(tr("Continue rebase"), [](git::GitClient& git) { return git.continueRebase(); }); });
    menu->addAction(tr("Abort merge"), this, [this] { runAsync(tr("Abort merge"), [](git::GitClient& git) { return git.abortMerge(); }); });
    menu->addAction(tr("Abort rebase"), this, [this] { runAsync(tr("Abort rebase"), [](git::GitClient& git) { return git.abortRebase(); }); });
    menu->addSeparator();
    menu->addAction(tr("Add to .gitignore..."), this, [this] {
        bool accepted = false;
        QString pattern = QInputDialog::getText(this, tr("Ignore"), tr("Pattern"), QLineEdit::Normal, QString(), &accepted);
        if (accepted && !pattern.isEmpty()) {
            auto status = client().addToIgnore(fromQ(pattern));
            if (!status)
                controller_->reportStatus(toQ(status.error().message));
            refresh();
        }
    });
    more->setMenu(menu);
    top->addWidget(more);
    controls->addLayout(top);
    summary_ = new QLabel(repoControls_);
    summary_->setProperty("role", "muted");
    controls->addWidget(summary_);
    auto* split = new QSplitter(Qt::Vertical, repoControls_);
    auto* changesHolder = new QWidget(split);
    auto* changesLayout = new QVBoxLayout(changesHolder);
    changesLayout->setContentsMargins(0, 0, 0, 0);
    changesLayout->setSpacing(3);
    changes_ = new QTreeWidget(changesHolder);
    changes_->setObjectName(QStringLiteral("gitChanges"));
    changes_->setHeaderHidden(true);
    changes_->setColumnCount(2);
    changes_->setRootIsDecorated(false);
    changes_->setContextMenuPolicy(Qt::CustomContextMenu);
    changes_->header()->setStretchLastSection(false);
    changes_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    changes_->header()->resizeSection(1, 28);
    changesLayout->addWidget(changes_, 1);
    message_ = new QPlainTextEdit(changesHolder);
    message_->setObjectName(QStringLiteral("gitMessage"));
    message_->setPlaceholderText(tr("Commit message"));
    message_->setFixedHeight(54);
    changesLayout->addWidget(message_);
    commitButton_ = new QPushButton(tr("Commit staged changes"), changesHolder);
    commitButton_->setObjectName(QStringLiteral("gitCommit"));
    commitButton_->setProperty("role", "primary");
    changesLayout->addWidget(commitButton_);
    split->addWidget(changesHolder);
    log_ = new QTreeWidget(split);
    log_->setObjectName(QStringLiteral("gitLog"));
    log_->setHeaderLabels({tr("History"), tr("Date")});
    log_->setRootIsDecorated(false);
    log_->setUniformRowHeights(true);
    log_->header()->setStretchLastSection(false);
    log_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    split->addWidget(log_);
    controls->addWidget(split, 1);
    layout->addWidget(repoControls_, 1);

    connect(initButton_, &QPushButton::clicked, this, [this] { runAsync(tr("Create repository"), [](git::GitClient& git) { return git.init(); }); });
    connect(branch_, &QComboBox::currentTextChanged, this, [this](const QString& name) {
        if (!updating_ && !name.isEmpty())
            runAsync(tr("Switch branch"), [name](git::GitClient& git) { return git.checkout(fromQ(name)); });
    });
    connect(commitButton_, &QPushButton::clicked, this, [this] {
        QString text = message_->toPlainText().trimmed();
        if (text.isEmpty()) {
            controller_->reportStatus(tr("Write a commit message first"));
            return;
        }
        message_->clear();
        runAsync(tr("Commit"), [text](git::GitClient& git) { return git.commit(fromQ(text)); });
    });
    connect(changes_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
        QString path = item->data(0, Qt::UserRole).toString();
        bool staged = item->data(0, Qt::UserRole + 1).toBool();
        auto diff = client().diff(fromQ(path), staged);
        if (diff)
            Q_EMIT diffRequested(path, toQ(*diff));
    });
    connect(changes_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) { controller_->openFile(item->data(0, Qt::UserRole).toString()); });
    connect(changes_, &QWidget::customContextMenuRequested, this, &GitPanel::showFileMenu);
    connect(log_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
        auto diff = client().diffCommit(fromQ(item->data(0, Qt::UserRole).toString()));
        if (diff)
            Q_EMIT diffRequested(item->text(0), toQ(*diff));
    });
    connect(controller_, &ProjectController::projectOpened, this, &GitPanel::refresh);
    connect(controller_, &ProjectController::filesChanged, this, [this](const QStringList&) { refresh(); });
}

git::GitClient GitPanel::client() const
{
    return git::GitClient(controller_->hasProject() ? controller_->session()->root() : std::filesystem::path());
}

void GitPanel::runAsync(const QString& label, std::function<Result<git::CommandResult>(git::GitClient&)> work)
{
    if (!controller_->hasProject())
        return;
    git::GitClient git = client();
    QPointer<GitPanel> self(this);
    controller_->reportStatus(tr("%1...").arg(label));
    Q_EMIT controller_->automationLogged(tr("You"), QStringLiteral("git: ") + label);
    std::thread([self, git, label, work]() mutable {
        auto result = work(git);
        QString message;
        if (!result)
            message = toQ(result.error().message);
        else if (!result->ok())
            message = toQ(result->error.empty() ? result->output : result->error);
        QMetaObject::invokeMethod(qApp, [self, label, message] {
            if (!self)
                return;
            self->controller_->reportStatus(message.isEmpty() ? tr("%1 finished").arg(label) : tr("%1: %2").arg(label, message.trimmed().left(300)));
            self->refresh();
        }, Qt::QueuedConnection);
    }).detach();
}

void GitPanel::refresh()
{
    if (!controller_->hasProject() || !git::GitClient::available()) {
        initButton_->setVisible(false);
        repoControls_->setVisible(false);
        return;
    }
    git::GitClient git = client();
    QPointer<GitPanel> self(this);
    std::thread([self, git] {
        auto status = git.status();
        auto branches = git.branches();
        auto log = git.log(100);
        QMetaObject::invokeMethod(qApp, [self, status, branches, log] {
            if (!self)
                return;
            bool isRepository = status && status->isRepository;
            self->initButton_->setVisible(!isRepository);
            self->repoControls_->setVisible(isRepository);
            if (!isRepository) {
                self->branchText_.clear();
                Q_EMIT self->branchChanged(QString());
                return;
            }
            self->updating_ = true;
            self->branch_->clear();
            if (branches) {
                for (const auto& branch : *branches) {
                    if (!branch.remote)
                        self->branch_->addItem(toQ(branch.name));
                }
            }
            QString current = toQ(status->branch);
            if (self->branch_->findText(current) < 0)
                self->branch_->addItem(current);
            self->branch_->setCurrentText(current);
            self->updating_ = false;
            QString text = current;
            if (status->upstream.empty())
                text += self->tr(" (no upstream)");
            else if (status->ahead > 0 || status->behind > 0)
                text += QStringLiteral(" +%1 -%2").arg(status->ahead).arg(status->behind);
            self->branchText_ = text;
            Q_EMIT self->branchChanged(text);
            self->summary_->setText(self->tr("%n changed file(s)", nullptr, static_cast<int>(status->files.size())));
            self->changes_->clear();
            for (const auto& file : status->files) {
                auto* item = new QTreeWidgetItem(self->changes_);
                item->setText(0, toQ(file.path));
                item->setToolTip(0, toQ(file.path));
                QString mark = file.conflicted ? QStringLiteral("!") : file.untracked ? QStringLiteral("U") : file.staged != ' ' ? QString(QLatin1Char(file.staged)) : QString(QLatin1Char(file.unstaged));
                item->setText(1, mark);
                bool staged = file.staged != ' ' && !file.conflicted;
                item->setData(0, Qt::UserRole, toQ(file.path));
                item->setData(0, Qt::UserRole + 1, staged);
                item->setData(0, Qt::UserRole + 2, file.conflicted);
                item->setCheckState(0, staged ? Qt::Checked : Qt::Unchecked);
                if (file.conflicted)
                    item->setForeground(0, colors().danger);
            }
            self->log_->clear();
            if (log) {
                for (const auto& commit : *log) {
                    auto* item = new QTreeWidgetItem(self->log_);
                    item->setText(0, toQ(commit.subject));
                    item->setText(1, toQ(commit.date));
                    item->setToolTip(0, toQ(commit.shortHash + " " + commit.author));
                    item->setData(0, Qt::UserRole, toQ(commit.hash));
                }
            }
        }, Qt::QueuedConnection);
    }).detach();
    disconnect(changes_, &QTreeWidget::itemChanged, this, nullptr);
    connect(changes_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int) {
        QString path = item->data(0, Qt::UserRole).toString();
        bool wantStaged = item->checkState(0) == Qt::Checked;
        if (wantStaged == item->data(0, Qt::UserRole + 1).toBool())
            return;
        runAsync(wantStaged ? tr("Stage") : tr("Unstage"), [path, wantStaged](git::GitClient& git) {
            return wantStaged ? git.stage({fromQ(path)}) : git.unstage({fromQ(path)});
        });
    });
}

void GitPanel::showFileMenu(const QPoint& position)
{
    QTreeWidgetItem* item = changes_->itemAt(position);
    if (item == nullptr)
        return;
    QString path = item->data(0, Qt::UserRole).toString();
    bool conflicted = item->data(0, Qt::UserRole + 2).toBool();
    QMenu menu(this);
    menu.addAction(tr("Open"), this, [this, path] { controller_->openFile(path); });
    if (conflicted) {
        menu.addSeparator();
        menu.addAction(tr("Use my version"), this, [this, path] { runAsync(tr("Resolve"), [path](git::GitClient& git) { return git.resolveWithOurs(fromQ(path)); }); });
        menu.addAction(tr("Use their version"), this, [this, path] { runAsync(tr("Resolve"), [path](git::GitClient& git) { return git.resolveWithTheirs(fromQ(path)); }); });
        menu.addAction(tr("Mark as resolved"), this, [this, path] { runAsync(tr("Resolve"), [path](git::GitClient& git) { return git.stage({fromQ(path)}); }); });
    } else {
        menu.addSeparator();
        menu.addAction(tr("Discard changes"), this, [this, path] {
            if (QMessageBox::question(this, tr("Discard changes"), tr("Throw away your changes to %1? Git cannot bring them back.").arg(path)) == QMessageBox::Yes)
                runAsync(tr("Discard"), [path](git::GitClient& git) { return git.discard({fromQ(path)}); });
        });
    }
    menu.exec(changes_->viewport()->mapToGlobal(position));
}

}
