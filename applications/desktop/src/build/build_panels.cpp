#include "build/build_panels.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QTextBlock>
#include <QTime>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <thread>

#include "app/app_context.h"
#include "ui/theme.h"

namespace forma::ui {

OutputPanel::OutputPanel(ProjectController* controller, QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    output_ = new QPlainTextEdit(this);
    output_->setObjectName(QStringLiteral("outputLog"));
    output_->setReadOnly(true);
    output_->setFont(Theme::instance().codeFont(9));
    output_->setMaximumBlockCount(20000);
    output_->setLineWrapMode(QPlainTextEdit::NoWrap);
    output_->setFrameShape(QFrame::NoFrame);
    layout->addWidget(output_);
    connect(controller, &ProjectController::buildStarted, this, [this] { clear(); appendLine(tr("Build started at %1").arg(QTime::currentTime().toString())); });
    connect(controller, &ProjectController::buildStep, this, [this](const QString& label) { appendLine(QStringLiteral("== %1 ==").arg(label)); });
    connect(controller, &ProjectController::buildOutput, this, &OutputPanel::appendLine);
    connect(controller, &ProjectController::buildFinished, this, [this](bool success, int errors, int warnings, qint64 millis) {
        appendLine(success ? tr("Build succeeded in %1 s, %2 warning(s)").arg(millis / 1000.0, 0, 'f', 1).arg(warnings) : tr("Build failed: %1 error(s), %2 warning(s)").arg(errors).arg(warnings), !success);
    });
    connect(controller, &ProjectController::appStarted, this, [this](const QString& command) { appendLine(tr("Running %1").arg(command)); });
    connect(controller, &ProjectController::appOutput, this, [this](const QString& text, bool isError) {
        for (const auto& line : text.split(QLatin1Char('\n'))) {
            QString clean = line;
            if (clean.endsWith(QLatin1Char('\r')))
                clean.chop(1);
            if (!clean.isEmpty())
                appendLine(clean, isError);
        }
    });
    connect(controller, &ProjectController::appStopped, this, [this](int code) { appendLine(tr("The program ended with exit code %1").arg(code), code != 0); });
}

QString OutputPanel::text() const
{
    return output_->toPlainText();
}

void OutputPanel::clear()
{
    output_->clear();
}

void OutputPanel::appendLine(const QString& line, bool isError)
{
    QScrollBar* bar = output_->verticalScrollBar();
    bool atBottom = bar->value() >= bar->maximum() - 2;
    QTextCharFormat format;
    format.setForeground(isError ? colors().danger : colors().text);
    QTextCursor cursor(output_->document());
    cursor.movePosition(QTextCursor::End);
    if (!output_->document()->isEmpty())
        cursor.insertBlock();
    cursor.insertText(line, format);
    if (atBottom)
        bar->setValue(bar->maximum());
}

ProblemsPanel::ProblemsPanel(ProjectController* controller, QWidget* parent) : QWidget(parent), controller_(controller)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* bar = new QHBoxLayout();
    bar->setContentsMargins(8, 2, 8, 2);
    showWarnings_ = new QCheckBox(tr("Show warnings"), this);
    showWarnings_->setChecked(true);
    bar->addWidget(showWarnings_);
    bar->addStretch(1);
    layout->addLayout(bar);
    table_ = new QTreeWidget(this);
    table_->setObjectName(QStringLiteral("problemsList"));
    table_->setColumnCount(3);
    table_->setHeaderLabels({tr("Problem"), tr("Where"), tr("From")});
    table_->setRootIsDecorated(false);
    table_->setUniformRowHeights(true);
    table_->header()->setStretchLastSection(false);
    table_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    table_->header()->resizeSection(1, 240);
    table_->header()->resizeSection(2, 90);
    layout->addWidget(table_, 1);
    connect(controller_, &ProjectController::problemsChanged, this, &ProblemsPanel::refresh);
    connect(showWarnings_, &QCheckBox::toggled, this, &ProblemsPanel::refresh);
    connect(table_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        QString file = item->data(0, Qt::UserRole).toString();
        if (!file.isEmpty()) {
            controller_->openFile(file, item->data(0, Qt::UserRole + 1).toInt(), item->data(0, Qt::UserRole + 2).toInt());
            return;
        }
        QString screen = item->data(0, Qt::UserRole + 3).toString();
        QString component = item->data(0, Qt::UserRole + 4).toString();
        if (!screen.isEmpty() && !component.isEmpty())
            controller_->selectComponent(screen, component);
    });
    connect(table_, &QTreeWidget::itemClicked, table_, &QTreeWidget::itemActivated);
}

int ProblemsPanel::rowCount() const
{
    return table_->topLevelItemCount();
}

void ProblemsPanel::refresh()
{
    table_->clear();
    int errors = 0;
    int warnings = 0;
    auto problems = controller_->problems();
    std::stable_sort(problems.begin(), problems.end(), [](const ProblemItem& a, const ProblemItem& b) { return a.severity > b.severity; });
    for (const auto& problem : problems) {
        errors += problem.severity == 2 ? 1 : 0;
        warnings += problem.severity == 1 ? 1 : 0;
        if (problem.severity < 2 && !showWarnings_->isChecked())
            continue;
        auto* item = new QTreeWidgetItem(table_);
        item->setIcon(0, Theme::instance().icon(problem.severity == 2 ? QStringLiteral("error") : problem.severity == 1 ? QStringLiteral("warning") : QStringLiteral("info"), problem.severity == 2 ? colors().danger : problem.severity == 1 ? colors().warning : colors().textMuted));
        item->setText(0, problem.message);
        item->setToolTip(0, problem.message);
        QString where;
        if (!problem.file.isEmpty())
            where = problem.line > 0 ? QStringLiteral("%1:%2").arg(problem.file).arg(problem.line) : problem.file;
        else if (!problem.component.isEmpty())
            where = problem.screen + QLatin1Char('/') + problem.component;
        else
            where = problem.screen;
        item->setText(1, where);
        item->setToolTip(1, where);
        item->setText(2, problem.source);
        item->setData(0, Qt::UserRole, problem.file);
        item->setData(0, Qt::UserRole + 1, problem.line);
        item->setData(0, Qt::UserRole + 2, problem.column);
        item->setData(0, Qt::UserRole + 3, problem.screen);
        item->setData(0, Qt::UserRole + 4, problem.component);
    }
    Q_EMIT countsChanged(errors, warnings);
}

BuildPanel::BuildPanel(ProjectController* controller, QWidget* parent) : QWidget(parent), controller_(controller)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(6);
    auto* top = new QHBoxLayout();
    top->addWidget(new QLabel(tr("Configuration"), this));
    configuration_ = new QComboBox(this);
    configuration_->setObjectName(QStringLiteral("buildConfiguration"));
    top->addWidget(configuration_);
    buildButton_ = new QPushButton(tr("Build"), this);
    buildButton_->setObjectName(QStringLiteral("buildPanelBuild"));
    buildButton_->setProperty("role", "primary");
    top->addWidget(buildButton_);
    cancelButton_ = new QPushButton(tr("Cancel"), this);
    cancelButton_->setEnabled(false);
    top->addWidget(cancelButton_);
    status_ = new QLabel(tr("Nothing has been built yet"), this);
    status_->setObjectName(QStringLiteral("buildStatus"));
    status_->setProperty("role", "muted");
    top->addWidget(status_, 1);
    auto* setup = new QPushButton(tr("Install tools..."), this);
    setup->setObjectName(QStringLiteral("buildSetup"));
    connect(setup, &QPushButton::clicked, this, &BuildPanel::setupRequested);
    top->addWidget(setup);
    layout->addLayout(top);
    progress_ = new QProgressBar(this);
    progress_->setRange(0, 0);
    progress_->setFixedHeight(3);
    progress_->setTextVisible(false);
    progress_->hide();
    layout->addWidget(progress_);
    auto* split = new QSplitter(Qt::Horizontal, this);
    split->setChildrenCollapsible(false);
    auto makeList = [&](const QString& title, QTreeWidget*& tree, const QStringList& headers, const QString& name) {
        auto* holder = new QWidget(split);
        auto* column = new QVBoxLayout(holder);
        column->setContentsMargins(0, 0, 6, 0);
        column->setSpacing(3);
        auto* label = new QLabel(title, holder);
        label->setProperty("role", "section");
        column->addWidget(label);
        tree = new QTreeWidget(holder);
        tree->setObjectName(name);
        tree->setHeaderLabels(headers);
        tree->setRootIsDecorated(false);
        tree->setUniformRowHeights(true);
        column->addWidget(tree, 1);
        return holder;
    };
    split->addWidget(makeList(tr("Built files"), artifacts_, {tr("File"), tr("Kind"), tr("Size")}, QStringLiteral("artifactsList")));
    split->addWidget(makeList(tr("Generated code"), generated_, {tr("File"), tr("What happened")}, QStringLiteral("generatedList")));
    split->addWidget(makeList(tr("Toolchains on this computer"), toolchains_, {tr("Tool"), tr("Version"), tr("Status")}, QStringLiteral("toolchainList")));
    toolchains_->setRootIsDecorated(true);
    layout->addWidget(split, 1);

    connect(buildButton_, &QPushButton::clicked, this, [this] { controller_->startBuild(configuration()); });
    connect(cancelButton_, &QPushButton::clicked, controller_, &ProjectController::cancelBuild);
    connect(controller_, &ProjectController::buildStarted, this, [this] {
        status_->setText(tr("Building..."));
        progress_->show();
        buildButton_->setEnabled(false);
        cancelButton_->setEnabled(true);
    });
    connect(controller_, &ProjectController::buildStep, this, [this](const QString& label) { status_->setText(label + QStringLiteral("...")); });
    connect(controller_, &ProjectController::buildFinished, this, [this](bool success, int errors, int warnings, qint64 millis) {
        progress_->hide();
        buildButton_->setEnabled(true);
        cancelButton_->setEnabled(false);
        status_->setText(success ? tr("Built in %1 s, %2 warning(s)").arg(millis / 1000.0, 0, 'f', 1).arg(warnings) : tr("Failed: %1 error(s), %2 warning(s)").arg(errors).arg(warnings));
        refreshArtifacts();
    });
    connect(controller_, &ProjectController::generationFinished, this, [this](const codegen::GenerationReport& report) {
        generated_->clear();
        for (const auto& file : report.files) {
            if (file.outcome == codegen::FileOutcome::Unchanged || file.outcome == codegen::FileOutcome::KeptUserFile)
                continue;
            auto* item = new QTreeWidgetItem(generated_);
            item->setText(0, toQ(file.path));
            item->setText(1, toQ(codegen::fileOutcomeName(file.outcome)) + (file.detail.empty() ? QString() : QStringLiteral(": ") + toQ(file.detail)));
            item->setData(0, Qt::UserRole, toQ(file.path));
            if (file.outcome == codegen::FileOutcome::Conflict)
                item->setForeground(1, colors().danger);
            item->setToolTip(1, item->text(1));
        }
        generated_->resizeColumnToContents(0);
    });
    generated_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(generated_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) { controller_->openFile(item->data(0, Qt::UserRole).toString()); });
    connect(generated_, &QWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        QTreeWidgetItem* item = generated_->itemAt(position);
        if (item == nullptr)
            return;
        QString path = item->data(0, Qt::UserRole).toString();
        QMenu menu(this);
        menu.addAction(tr("Open"), this, [this, path] { controller_->openFile(path); });
        if (item->text(1).startsWith(QStringLiteral("conflict"))) {
            menu.addSeparator();
            menu.addAction(tr("Keep my version"), this, [this, path] { controller_->resolveConflict(path, codegen::ConflictChoice::KeepMine); });
            menu.addAction(tr("Use the generated version"), this, [this, path] {
                if (QMessageBox::question(this, tr("Use generated version"), tr("Replace your edits in %1? A backup is kept in the project.").arg(path)) == QMessageBox::Yes)
                    controller_->resolveConflict(path, codegen::ConflictChoice::TakeGenerated);
            });
            menu.addAction(tr("Merge by hand (adds conflict markers)"), this, [this, path] {
                controller_->resolveConflict(path, codegen::ConflictChoice::WriteMarkers);
                controller_->openFile(path);
            });
        }
        menu.exec(generated_->viewport()->mapToGlobal(position));
    });
    artifacts_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(artifacts_, &QWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        QTreeWidgetItem* item = artifacts_->itemAt(position);
        if (item == nullptr)
            return;
        QString path = item->data(0, Qt::UserRole).toString();
        QMenu menu(this);
        menu.addAction(tr("Show in file manager"), this, [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath())); });
        menu.exec(artifacts_->viewport()->mapToGlobal(position));
    });
    connect(controller_, &ProjectController::projectOpened, this, &BuildPanel::refreshConfigurations);
    connect(controller_, &ProjectController::modelChanged, this, [this](const ir::ChangeEvent& event) {
        if (event.key == "buildConfigurations" || event.kind == ir::ChangeKind::Reset)
            refreshConfigurations();
    });
}

QString BuildPanel::configuration() const
{
    QString value = configuration_->currentData().toString();
    return value.isEmpty() ? QStringLiteral("debug") : value;
}

void BuildPanel::refreshConfigurations()
{
    configuration_->clear();
    if (!controller_->hasProject())
        return;
    for (const auto& item : controller_->model()->buildConfigurations)
        configuration_->addItem(toQ(item.name), toQ(item.id));
    if (configuration_->count() == 0) {
        configuration_->addItem(tr("Debug"), QStringLiteral("debug"));
        configuration_->addItem(tr("Release"), QStringLiteral("release"));
    }
}

void BuildPanel::refreshArtifacts()
{
    artifacts_->clear();
    if (!controller_->hasProject() || controller_->target() == nullptr)
        return;
    QString root = toQ(paths::toUtf8(controller_->session()->targetDirectory(*controller_->target())));
    for (const auto& artifact : controller_->artifacts()) {
        auto* item = new QTreeWidgetItem(artifacts_);
        item->setText(0, toQ(artifact.path));
        item->setText(1, toQ(artifact.kind));
        item->setText(2, QLocale().formattedDataSize(static_cast<qint64>(artifact.size)));
        item->setData(0, Qt::UserRole, QDir(root).filePath(toQ(artifact.path)));
    }
    artifacts_->resizeColumnToContents(0);
}

void BuildPanel::refreshToolchains(bool force)
{
    toolchains_->clear();
    auto* wait = new QTreeWidgetItem(toolchains_);
    wait->setText(0, tr("Looking for tools..."));
    QPointer<BuildPanel> self(this);
    std::thread([self, force] {
        auto tools = AppContext::instance().tools().detectAll(force);
        QMetaObject::invokeMethod(qApp, [self, tools] {
            if (!self)
                return;
            self->toolchains_->clear();
            std::map<std::string, QTreeWidgetItem*> groups;
            for (const auto& tool : tools) {
                QTreeWidgetItem*& group = groups[tool.category];
                if (group == nullptr) {
                    group = new QTreeWidgetItem(self->toolchains_);
                    group->setText(0, toQ(tool.category));
                    group->setFirstColumnSpanned(true);
                    QFont bold = group->font(0);
                    bold.setWeight(QFont::DemiBold);
                    group->setFont(0, bold);
                }
                auto* item = new QTreeWidgetItem(group);
                item->setText(0, toQ(tool.name));
                item->setText(1, toQ(tool.version));
                item->setText(2, tool.available ? tr("Found") : tr("Not installed"));
                item->setForeground(2, tool.available ? colors().success : colors().textFaint);
                item->setToolTip(0, tool.available ? toQ(tool.path) : toQ(tool.installHint));
                item->setToolTip(2, tool.available ? toQ(tool.path) : toQ(tool.installHint));
            }
            self->toolchains_->expandAll();
            self->toolchains_->resizeColumnToContents(0);
        }, Qt::QueuedConnection);
    }).detach();
}

AutomationLog::AutomationLog(ProjectController* controller, QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    list_ = new QTreeWidget(this);
    list_->setObjectName(QStringLiteral("automationLog"));
    list_->setHeaderLabels({tr("Time"), tr("Started by"), tr("Command")});
    list_->setRootIsDecorated(false);
    list_->setUniformRowHeights(true);
    layout->addWidget(list_);
    connect(controller, &ProjectController::automationLogged, this, &AutomationLog::add);
}

void AutomationLog::add(const QString& actor, const QString& command)
{
    auto* item = new QTreeWidgetItem(list_);
    item->setText(0, QTime::currentTime().toString());
    item->setText(1, actor);
    item->setText(2, command);
    item->setToolTip(2, command);
    list_->scrollToItem(item);
}

}
