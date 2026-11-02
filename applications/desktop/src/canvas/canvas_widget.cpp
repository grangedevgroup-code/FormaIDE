#include "canvas/canvas_widget.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDrag>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>
#include <QWheelEvent>
#include <cmath>

#include "app/app_context.h"
#include "forma/core/text.h"
#include "forma/ir/catalog.h"
#include "ui/theme.h"

namespace forma::ui {

namespace {

constexpr int kRulerSize = 20;
constexpr double kHandleSize = 7;
constexpr double kSideHandleRoom = 40;

QRectF qrect(const layout::Rect& rect)
{
    return QRectF(rect.x, rect.y, rect.width, rect.height);
}

double sizeOf(const ir::SizeSpec& spec, double fallback)
{
    return spec.mode == ir::SizeMode::Fixed ? spec.value : fallback;
}

}

CanvasWidget::CanvasWidget(ProjectController* controller, QWidget* parent) : QWidget(parent), controller_(controller)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAcceptDrops(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setObjectName(QStringLiteral("designCanvas"));
    connect(controller_, &ProjectController::modelChanged, this, [this](const ir::ChangeEvent& event) {
        invalidate(event.kind == ir::ChangeKind::ApplicationChanged || event.kind == ir::ChangeKind::Reset ? QString() : toQ(event.screen));
    });
    connect(controller_, &ProjectController::selectionChanged, this, QOverload<>::of(&CanvasWidget::update));
    connect(controller_, &ProjectController::screenChanged, this, QOverload<>::of(&CanvasWidget::update));
    connect(controller_, &ProjectController::projectOpened, this, [this] {
        caches_.clear();
        fitPending_ = true;
        update();
    });
    connect(controller_, &ProjectController::projectClosed, this, [this] {
        caches_.clear();
        update();
    });
    connect(&Theme::instance(), &Theme::changed, this, QOverload<>::of(&CanvasWidget::update));
    buildChrome();
}

void CanvasWidget::buildChrome()
{
    toolBar_ = new QFrame(this);
    toolBar_->setObjectName(QStringLiteral("canvasToolbar"));
    toolBar_->setProperty("role", "floating");
    auto* tools = new QHBoxLayout(toolBar_);
    tools->setContentsMargins(5, 5, 5, 5);
    tools->setSpacing(2);
    auto addButton = [this](QFrame* holder, QHBoxLayout* row, const QString& name, const QString& iconName, const QString& tip) {
        auto* button = new QToolButton(holder);
        button->setObjectName(QStringLiteral("tool.") + name);
        button->setIcon(icon(iconName));
        button->setIconSize(QSize(18, 18));
        button->setToolTip(tip);
        button->setFocusPolicy(Qt::NoFocus);
        row->addWidget(button);
        chromeIcons_.emplace_back(button, iconName);
        return button;
    };
    auto addSeparator = [](QFrame* holder, QHBoxLayout* row) {
        auto* line = new QFrame(holder);
        line->setProperty("role", "sep");
        row->addWidget(line);
    };
    auto addTool = [&](const QString& key, const QString& name, const QString& iconName, const QString& tip, std::function<void()> action) {
        QToolButton* button = addButton(toolBar_, tools, name, iconName, tip);
        button->setCheckable(true);
        toolButtons_[key] = button;
        connect(button, &QToolButton::clicked, this, [this, action] {
            action();
            updateChrome();
        });
    };
    addTool(QStringLiteral("@select"), QStringLiteral("move"), QStringLiteral("pointer"), tr("Move and select (V)"), [this] { setTool(Tool::Select); });
    addTool(QStringLiteral("@pan"), QStringLiteral("hand"), QStringLiteral("hand"), tr("Hand: drag to move around (H, or hold Space)"), [this] { setTool(Tool::Pan); });
    addSeparator(toolBar_, tools);
    struct Insertable {
        const char* type;
        const char* name;
        const char* iconName;
        QString tip;
    };
    const std::vector<Insertable> insertables = {
        {"frame", "frame", "frame", tr("Frame (F): click or drag on a screen")},
        {"text", "text", "text", tr("Text (T): click or drag on a screen")},
        {"shape", "shape", "shape", tr("Shape (R): click or drag on a screen")},
        {"image", "image", "image", tr("Image: click or drag on a screen")},
        {"button", "button", "button", tr("Button (B): click or drag on a screen")},
        {"textinput", "input", "input", tr("Text input (I): click or drag on a screen")},
    };
    for (const auto& item : insertables) {
        QString type = QString::fromLatin1(item.type);
        addTool(type, QString::fromLatin1(item.name), QString::fromLatin1(item.iconName), item.tip, [this, type] {
            if (insertType_ == type)
                setTool(Tool::Select);
            else
                beginInsert(type);
        });
    }
    QToolButton* more = addButton(toolBar_, tools, QStringLiteral("more"), QStringLiteral("components"), tr("All components"));
    more->setPopupMode(QToolButton::InstantPopup);
    auto* moreMenu = new QMenu(more);
    connect(moreMenu, &QMenu::aboutToShow, this, [this, moreMenu] {
        moreMenu->clear();
        const auto& catalog = AppContext::instance().components();
        for (const auto& category : catalog.categories()) {
            QMenu* group = moreMenu->addMenu(toQ(category));
            for (const auto& schema : catalog.all()) {
                if (schema.category != category)
                    continue;
                QString type = toQ(schema.type);
                group->addAction(icon(toQ(schema.icon)), toQ(schema.label), this, [this, type] { beginInsert(type); });
            }
        }
    });
    more->setMenu(moreMenu);
    addSeparator(toolBar_, tools);
    QToolButton* addScreenButton = addButton(toolBar_, tools, QStringLiteral("addScreen"), QStringLiteral("plus"), tr("Add a screen"));
    connect(addScreenButton, &QToolButton::clicked, this, [this] {
        if (!controller_->addScreen().isEmpty())
            centerOnScreen(controller_->currentScreen());
    });
    flowsButton_ = addButton(toolBar_, tools, QStringLiteral("flows"), QStringLiteral("link"), tr("Show how screens link to each other"));
    flowsButton_->setCheckable(true);
    connect(flowsButton_, &QToolButton::toggled, this, &CanvasWidget::setFlowsVisible);

    zoomBar_ = new QFrame(this);
    zoomBar_->setObjectName(QStringLiteral("canvasZoomBar"));
    zoomBar_->setProperty("role", "floating");
    auto* zoomRow = new QHBoxLayout(zoomBar_);
    zoomRow->setContentsMargins(5, 5, 5, 5);
    zoomRow->setSpacing(2);
    QToolButton* zoomOutButton = addButton(zoomBar_, zoomRow, QStringLiteral("zoomOut"), QStringLiteral("minus"), tr("Zoom out"));
    connect(zoomOutButton, &QToolButton::clicked, this, &CanvasWidget::zoomOut);
    zoomLabel_ = new QLabel(QStringLiteral("100%"), zoomBar_);
    zoomLabel_->setObjectName(QStringLiteral("canvasZoomLabel"));
    zoomLabel_->setAlignment(Qt::AlignCenter);
    zoomLabel_->setMinimumWidth(42);
    zoomRow->addWidget(zoomLabel_);
    QToolButton* zoomInButton = addButton(zoomBar_, zoomRow, QStringLiteral("zoomIn"), QStringLiteral("plus"), tr("Zoom in"));
    connect(zoomInButton, &QToolButton::clicked, this, &CanvasWidget::zoomIn);
    QToolButton* fitButton = addButton(zoomBar_, zoomRow, QStringLiteral("zoomFit"), QStringLiteral("fit"), tr("Fit everything (Shift+1)"));
    connect(fitButton, &QToolButton::clicked, this, &CanvasWidget::zoomToFit);
    connect(this, &CanvasWidget::zoomChanged, this, [this](double zoom) { zoomLabel_->setText(QStringLiteral("%1%").arg(static_cast<int>(std::lround(zoom * 100)))); });
    connect(&Theme::instance(), &Theme::changed, this, [this] {
        for (const auto& [button, name] : chromeIcons_)
            button->setIcon(icon(name));
    });
    connect(controller_, &ProjectController::projectOpened, this, &CanvasWidget::updateChrome);
    connect(controller_, &ProjectController::projectClosed, this, &CanvasWidget::updateChrome);
    updateChrome();
}

void CanvasWidget::layoutChrome()
{
    toolBar_->adjustSize();
    zoomBar_->adjustSize();
    toolBar_->move((width() - toolBar_->width()) / 2, height() - toolBar_->height() - 14);
    zoomBar_->move(width() - zoomBar_->width() - 14, height() - zoomBar_->height() - 14);
    zoomBar_->setVisible(controller_->hasProject() && toolBar_->geometry().right() + 10 < zoomBar_->x());
    toolBar_->raise();
    zoomBar_->raise();
}

void CanvasWidget::updateChrome()
{
    bool open = controller_->hasProject();
    toolBar_->setVisible(open);
    zoomBar_->setVisible(open);
    for (const auto& [key, button] : toolButtons_) {
        bool on = false;
        if (key == QStringLiteral("@select"))
            on = tool_ == Tool::Select;
        else if (key == QStringLiteral("@pan"))
            on = tool_ == Tool::Pan;
        else
            on = tool_ == Tool::Insert && insertType_ == key;
        button->setChecked(on);
    }
    if (flowsButton_->isChecked() != flows_) {
        QSignalBlocker block(flowsButton_);
        flowsButton_->setChecked(flows_);
    }
    if (open)
        layoutChrome();
}

void CanvasWidget::setTool(Tool tool)
{
    tool_ = tool;
    if (tool != Tool::Insert)
        insertType_.clear();
    setCursor(tool == Tool::Pan ? Qt::OpenHandCursor : tool == Tool::Insert ? Qt::CrossCursor : Qt::ArrowCursor);
    updateChrome();
    Q_EMIT toolChanged();
}

void CanvasWidget::beginInsert(const QString& type)
{
    insertType_ = type;
    tool_ = Tool::Insert;
    setCursor(Qt::CrossCursor);
    updateChrome();
    setFocus();
    Q_EMIT toolChanged();
}

void CanvasWidget::setFlowsVisible(bool on)
{
    if (flows_ == on)
        return;
    flows_ = on;
    updateChrome();
    update();
}

QRectF CanvasWidget::addScreenRect() const
{
    const ir::ApplicationModel* model = controller_->model();
    if (model == nullptr)
        return {};
    if (model->screens.empty())
        return QRectF(0, 0, 390, 844);
    QRectF last;
    for (const auto& screen : model->screens) {
        QRectF artboard = artboardRect(screen);
        if (last.isNull() || artboard.right() > last.right())
            last = artboard;
    }
    return QRectF(last.right() + 120, last.top(), std::min(320.0, last.width()), last.height());
}

QPointF CanvasWidget::worldToView(QPointF world) const
{
    return QPointF(view_.toViewX(world.x()), view_.toViewY(world.y()));
}

QPointF CanvasWidget::viewToWorld(QPointF point) const
{
    return QPointF(view_.toWorldX(point.x()), view_.toWorldY(point.y()));
}

void CanvasWidget::setZoom(double zoom, QPointF anchor)
{
    zoom = std::clamp(zoom, 0.05, 16.0);
    if (anchor.x() < 0)
        anchor = QPointF(width() / 2.0, height() / 2.0);
    QPointF world = viewToWorld(anchor);
    view_.scale = zoom;
    view_.offsetX = anchor.x() - world.x() * zoom;
    view_.offsetY = anchor.y() - world.y() * zoom;
    Q_EMIT zoomChanged(zoom);
    update();
}

void CanvasWidget::zoomIn()
{
    setZoom(view_.scale * 1.25);
}

void CanvasWidget::zoomOut()
{
    setZoom(view_.scale / 1.25);
}

void CanvasWidget::zoomToFit()
{
    const ir::ApplicationModel* model = controller_->model();
    if (model == nullptr || model->screens.empty())
        return;
    QRectF bounds = addScreenRect();
    for (const auto& screen : model->screens)
        bounds = bounds.united(artboardRect(screen));
    double side = 48;
    double top = 48;
    double bottom = 76;
    double available = rulers_ ? kRulerSize : 0;
    double scale = std::min((width() - available - side * 2) / bounds.width(), (height() - available - top - bottom) / bounds.height());
    scale = std::clamp(scale, 0.05, 1.0);
    view_.scale = scale;
    view_.offsetX = available + (width() - available - bounds.width() * scale) / 2 - bounds.x() * scale;
    view_.offsetY = available + top + (height() - available - top - bottom - bounds.height() * scale) / 2 - bounds.y() * scale;
    Q_EMIT zoomChanged(scale);
    update();
}

void CanvasWidget::zoomToSelection()
{
    QRectF bounds = selectionBounds();
    if (bounds.isEmpty()) {
        zoomToFit();
        return;
    }
    double scale = std::clamp(std::min((width() - 160.0) / bounds.width(), (height() - 160.0) / bounds.height()), 0.05, 8.0);
    view_.scale = scale;
    view_.offsetX = width() / 2.0 - bounds.center().x() * scale;
    view_.offsetY = height() / 2.0 - bounds.center().y() * scale;
    Q_EMIT zoomChanged(scale);
    update();
}

void CanvasWidget::centerOnScreen(const QString& id)
{
    const ir::ApplicationModel* model = controller_->model();
    if (model == nullptr)
        return;
    const ir::Screen* screen = model->findScreen(fromQ(id));
    if (screen == nullptr)
        return;
    QRectF rect = artboardRect(*screen);
    view_.offsetX = width() / 2.0 - rect.center().x() * view_.scale;
    view_.offsetY = height() / 2.0 - rect.center().y() * view_.scale;
    update();
}

void CanvasWidget::setGridVisible(bool on)
{
    grid_ = on;
    update();
}

void CanvasWidget::setRulersVisible(bool on)
{
    rulers_ = on;
    update();
}

void CanvasWidget::invalidate(const QString& screen)
{
    if (screen.isEmpty())
        caches_.clear();
    else
        caches_.erase(fromQ(screen));
    update();
}

CanvasWidget::ScreenCache& CanvasWidget::cacheFor(const ir::Screen& screen)
{
    auto it = caches_.find(screen.root.id);
    if (it != caches_.end())
        return it->second;
    layout::LayoutOptions options;
    options.model = controller_->model();
    options.measurer = &measurer_;
    ScreenCache cache;
    cache.layout = layout::computeScreenLayout(screen, options);
    cache.list = canvas::buildDisplayList(cache.layout, *controller_->model());
    return caches_.emplace(screen.root.id, std::move(cache)).first->second;
}

const layout::LayoutResult* CanvasWidget::layoutFor(const QString& id)
{
    const ir::ApplicationModel* model = controller_->model();
    if (model == nullptr)
        return nullptr;
    const ir::Screen* screen = model->findScreen(fromQ(id));
    if (screen == nullptr)
        return nullptr;
    return &cacheFor(*screen).layout;
}

QRectF CanvasWidget::artboardRect(const ir::Screen& screen) const
{
    return QRectF(screen.canvas.x, screen.canvas.y, sizeOf(screen.root.width, 390), sizeOf(screen.root.height, 844));
}

QRectF CanvasWidget::worldFrameOf(const QString& screenId, const QString& component)
{
    const ir::ApplicationModel* model = controller_->model();
    if (model == nullptr)
        return {};
    const ir::Screen* screen = model->findScreen(fromQ(screenId));
    if (screen == nullptr)
        return {};
    const layout::LayoutNode* node = cacheFor(*screen).layout.find(fromQ(component));
    if (node == nullptr)
        return {};
    return qrect(node->frame).translated(screen->canvas.x, screen->canvas.y);
}

QImage CanvasWidget::renderScreen(const QString& id, double scale, bool includeBackground)
{
    const ir::ApplicationModel* model = controller_->model();
    if (model == nullptr)
        return {};
    const ir::Screen* screen = model->findScreen(fromQ(id));
    if (screen == nullptr)
        return {};
    ScreenCache& cache = cacheFor(*screen);
    QSize size(static_cast<int>(std::ceil(cache.layout.size.width * scale)), static_cast<int>(std::ceil(cache.layout.size.height * scale)));
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(includeBackground ? QColor(Qt::white) : QColor(Qt::transparent));
    QPainter painter(&image);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
    painter.scale(scale, scale);
    PainterBackend backend(painter, controller_->rootPath());
    backend.draw(cache.list, {});
    painter.end();
    return image;
}

void CanvasWidget::paintEvent(QPaintEvent*)
{
    QElapsedTimer timer;
    timer.start();
    if (fitPending_ && controller_->hasProject() && width() > 100) {
        fitPending_ = false;
        zoomToFit();
    }
    const ThemeColors& c = colors();
    QPainter painter(this);
    painter.fillRect(rect(), c.canvas);
    const ir::ApplicationModel* model = controller_->model();
    if (model != nullptr)
        paintDots(painter);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    if (model != nullptr) {
        QRectF visible = QRectF(viewToWorld(QPointF(0, 0)), viewToWorld(QPointF(width(), height())));
        QFont titleFont = font();
        titleFont.setPixelSize(12);
        QFont titleBold = titleFont;
        titleBold.setWeight(QFont::DemiBold);
        for (const auto& screen : model->screens) {
            QRectF artboard = artboardRect(screen);
            bool isCurrent = toQ(screen.root.id) == controller_->currentScreen();
            if (artboard.intersects(visible)) {
                painter.save();
                painter.translate(view_.offsetX, view_.offsetY);
                painter.scale(view_.scale, view_.scale);
                painter.fillRect(artboard, Qt::white);
                painter.translate(artboard.topLeft());
                painter.setClipRect(QRectF(QPointF(0, 0), artboard.size()), Qt::IntersectClip);
                ScreenCache& cache = cacheFor(screen);
                PainterBackend backend(painter, controller_->rootPath());
                backend.draw(cache.list, view_);
                painter.setClipping(false);
                if (grid_)
                    paintGrid(painter, QRectF(QPointF(0, 0), artboard.size()));
                painter.restore();
            }
            QRectF frame(worldToView(artboard.topLeft()), worldToView(artboard.bottomRight()));
            painter.setPen(QPen(isCurrent ? c.accent : c.borderStrong, 1));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(frame.adjusted(-0.5, -0.5, 0.5, 0.5));
            QString name = toQ(screen.root.name.empty() ? screen.root.id : screen.root.name);
            if (screen.root.id == model->navigation.start)
                name += tr(" (start)");
            QString size = QStringLiteral("%1 x %2").arg(static_cast<int>(std::lround(artboard.width()))).arg(static_cast<int>(std::lround(artboard.height())));
            painter.setFont(isCurrent ? titleBold : titleFont);
            QFontMetricsF nameMetrics(painter.font());
            double available = std::max(40.0, frame.width());
            QString shown = nameMetrics.elidedText(name, Qt::ElideRight, available);
            painter.setPen(isCurrent ? c.accent : c.textMuted);
            QRectF titleRect(frame.left(), frame.top() - 22, available, 20);
            painter.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, shown);
            double used = nameMetrics.horizontalAdvance(shown) + 8;
            if (used + 60 < available) {
                painter.setFont(titleFont);
                painter.setPen(c.textFaint);
                painter.drawText(titleRect.adjusted(used, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, size);
            }
        }
        QRectF ghostWorld = addScreenRect();
        QRectF ghost(worldToView(ghostWorld.topLeft()), worldToView(ghostWorld.bottomRight()));
        painter.setPen(QPen(c.borderStrong, 1, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(ghost);
        painter.setFont(titleFont);
        painter.setPen(c.textMuted);
        painter.drawText(ghost, Qt::AlignCenter, tr("+ Add screen"));
        if (flows_)
            paintFlows(painter);
        paintOverlays(painter);
    } else {
        painter.setPen(c.textMuted);
        painter.drawText(rect(), Qt::AlignCenter, tr("Open a project to start designing"));
    }
    if (rulers_)
        paintRulers(painter);
    lastPaintMicros_ = static_cast<int>(timer.nsecsElapsed() / 1000);
}

void CanvasWidget::paintDots(QPainter& painter)
{
    double step = 20;
    while (step * view_.scale < 16)
        step *= 2;
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(QPen(Theme::instance().isDark() ? colors().border : colors().borderStrong, 1));
    QVector<QPointF> points;
    double startX = std::floor(view_.toWorldX(0) / step) * step;
    double startY = std::floor(view_.toWorldY(0) / step) * step;
    for (double y = startY; view_.toViewY(y) < height(); y += step) {
        for (double x = startX; view_.toViewX(x) < width(); x += step)
            points.push_back(QPointF(std::round(view_.toViewX(x)), std::round(view_.toViewY(y))));
    }
    painter.drawPoints(points.constData(), static_cast<int>(points.size()));
}

void CanvasWidget::paintFlows(QPainter& painter)
{
    const ir::ApplicationModel* model = controller_->model();
    const ThemeColors& c = colors();
    std::function<void(const ir::Screen&, const ir::Component&)> visit = [&](const ir::Screen& screen, const ir::Component& node) {
        for (const auto& [event, actionId] : node.events) {
            const ir::Action* action = model->findAction(actionId);
            if (action == nullptr || action->kind != "navigate")
                continue;
            const ir::Screen* target = model->findScreen(action->target);
            if (target == nullptr || target == &screen)
                continue;
            QRectF source = worldFrameOf(toQ(screen.root.id), toQ(node.id));
            if (source.isEmpty())
                continue;
            QRectF artboard = artboardRect(*target);
            bool forward = artboard.left() >= source.center().x();
            QPointF from = worldToView(QPointF(forward ? source.right() : source.left(), source.center().y()));
            QPointF to = worldToView(QPointF(forward ? artboard.left() : artboard.right(), artboard.top() + std::min(60.0, artboard.height() / 4)));
            double reach = std::max(40.0, std::abs(to.x() - from.x()) / 2);
            QPainterPath path(from);
            path.cubicTo(from + QPointF(forward ? reach : -reach, 0), to - QPointF(forward ? reach : -reach, 0), to);
            painter.setPen(QPen(c.accent, 1.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(path);
            painter.setPen(Qt::NoPen);
            painter.setBrush(c.accent);
            painter.drawEllipse(from, 3.5, 3.5);
            double direction = forward ? -1 : 1;
            const QPointF head[3] = {to, to + QPointF(direction * 8, -5), to + QPointF(direction * 8, 5)};
            painter.drawPolygon(head, 3);
        }
        for (const auto& child : node.children)
            visit(screen, child);
    };
    for (const auto& screen : model->screens)
        visit(screen, screen.root);
}

void CanvasWidget::paintMeasure(QPainter& painter, const QRectF& from, const QRectF& to)
{
    const ThemeColors& c = colors();
    QFont small = font();
    small.setPixelSize(11);
    painter.setFont(small);
    QFontMetrics metrics(small);
    auto gap = [&](QPointF a, QPointF b, double value) {
        if (value < 0.5)
            return;
        QPointF viewA = worldToView(a);
        QPointF viewB = worldToView(b);
        painter.setPen(QPen(c.danger, 1));
        painter.drawLine(viewA, viewB);
        QString label = QString::number(static_cast<int>(std::lround(value)));
        QPointF middle = (viewA + viewB) / 2;
        QRectF tag(middle.x() - metrics.horizontalAdvance(label) / 2.0 - 4, middle.y() - 8, metrics.horizontalAdvance(label) + 8, 16);
        painter.setPen(Qt::NoPen);
        painter.setBrush(c.danger);
        painter.drawRoundedRect(tag, 3, 3);
        painter.setPen(Qt::white);
        painter.drawText(tag, Qt::AlignCenter, label);
    };
    double midY = from.center().y();
    double midX = from.center().x();
    if (to.right() <= from.left())
        gap(QPointF(to.right(), midY), QPointF(from.left(), midY), from.left() - to.right());
    else if (to.left() >= from.right())
        gap(QPointF(from.right(), midY), QPointF(to.left(), midY), to.left() - from.right());
    else {
        gap(QPointF(to.left(), midY), QPointF(from.left(), midY), from.left() - to.left());
        gap(QPointF(from.right(), midY), QPointF(to.right(), midY), to.right() - from.right());
    }
    if (to.bottom() <= from.top())
        gap(QPointF(midX, to.bottom()), QPointF(midX, from.top()), from.top() - to.bottom());
    else if (to.top() >= from.bottom())
        gap(QPointF(midX, from.bottom()), QPointF(midX, to.top()), to.top() - from.bottom());
    else {
        gap(QPointF(midX, to.top()), QPointF(midX, from.top()), from.top() - to.top());
        gap(QPointF(midX, from.bottom()), QPointF(midX, to.bottom()), to.bottom() - from.bottom());
    }
}

void CanvasWidget::paintGrid(QPainter& painter, const QRectF& artboard)
{
    double step = 8;
    if (view_.scale < 0.5)
        step = 32;
    if (view_.scale < 0.2)
        return;
    QColor line = colors().accent;
    line.setAlpha(36);
    painter.setPen(QPen(line, 0));
    for (double x = 0; x <= artboard.width(); x += step)
        painter.drawLine(QPointF(x, 0), QPointF(x, artboard.height()));
    for (double y = 0; y <= artboard.height(); y += step)
        painter.drawLine(QPointF(0, y), QPointF(artboard.width(), y));
}

void CanvasWidget::paintRulers(QPainter& painter)
{
    const ThemeColors& c = colors();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.fillRect(0, 0, width(), kRulerSize, c.panel);
    painter.fillRect(0, 0, kRulerSize, height(), c.panel);
    painter.setPen(c.border);
    painter.drawLine(0, kRulerSize, width(), kRulerSize);
    painter.drawLine(kRulerSize, 0, kRulerSize, height());
    double worldStep = 100;
    for (double candidate : {1000.0, 500.0, 200.0, 100.0, 50.0, 20.0, 10.0, 5.0}) {
        if (candidate * view_.scale >= 50)
            worldStep = candidate;
    }
    QFont small = font();
    small.setPixelSize(10);
    painter.setFont(small);
    double originX = 0;
    double originY = 0;
    if (const ir::ApplicationModel* model = controller_->model()) {
        if (const ir::Screen* screen = model->findScreen(fromQ(controller_->currentScreen()))) {
            originX = screen->canvas.x;
            originY = screen->canvas.y;
        }
    }
    double startX = std::floor((view_.toWorldX(kRulerSize) - originX) / worldStep) * worldStep;
    for (double x = startX; view_.toViewX(x + originX) < width(); x += worldStep) {
        double vx = view_.toViewX(x + originX);
        painter.setPen(c.borderStrong);
        painter.drawLine(QPointF(vx, kRulerSize - 6), QPointF(vx, kRulerSize));
        painter.setPen(c.textFaint);
        painter.drawText(QPointF(vx + 3, 12), QString::number(static_cast<int>(x)));
    }
    double startY = std::floor((view_.toWorldY(kRulerSize) - originY) / worldStep) * worldStep;
    for (double y = startY; view_.toViewY(y + originY) < height(); y += worldStep) {
        double vy = view_.toViewY(y + originY);
        painter.setPen(c.borderStrong);
        painter.drawLine(QPointF(kRulerSize - 6, vy), QPointF(kRulerSize, vy));
        painter.save();
        painter.setPen(c.textFaint);
        painter.translate(11, vy - 3);
        painter.rotate(-90);
        painter.drawText(QPointF(0, 0), QString::number(static_cast<int>(y)));
        painter.restore();
    }
    painter.fillRect(0, 0, kRulerSize, kRulerSize, c.panel);
    painter.setRenderHint(QPainter::Antialiasing);
}

void CanvasWidget::paintOverlays(QPainter& painter)
{
    const ThemeColors& c = colors();
    const ir::ApplicationModel* model = controller_->model();
    const ir::Screen* current = model->findScreen(fromQ(controller_->currentScreen()));
    QColor guideColor(c.danger);
    for (const auto& [vertical, position] : guides_) {
        painter.setPen(QPen(guideColor, 1));
        if (vertical) {
            double x = view_.toViewX(position);
            painter.drawLine(QPointF(x, 0), QPointF(x, height()));
        } else {
            double y = view_.toViewY(position);
            painter.drawLine(QPointF(0, y), QPointF(width(), y));
        }
    }
    if (drag_ == Drag::Guide) {
        painter.setPen(QPen(guideColor, 1, Qt::DashLine));
        if (guideVertical_) {
            double x = view_.toViewX(guidePosition_);
            painter.drawLine(QPointF(x, 0), QPointF(x, height()));
        } else {
            double y = view_.toViewY(guidePosition_);
            painter.drawLine(QPointF(0, y), QPointF(width(), y));
        }
    }
    if (current == nullptr)
        return;
    auto toView = [&](const QRectF& world) { return QRectF(worldToView(world.topLeft()), worldToView(world.bottomRight())); };
    if (!hoverComponent_.isEmpty() && !controller_->selection().contains(hoverComponent_) && drag_ == Drag::None) {
        QRectF frame = worldFrameOf(hoverScreen_, hoverComponent_);
        if (!frame.isEmpty()) {
            painter.setPen(QPen(c.accent, 1));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(toView(frame));
        }
    }
    bool singleSelection = controller_->selection().size() == 1;
    for (const auto& id : controller_->selection()) {
        QRectF frame = worldFrameOf(controller_->currentScreen(), id);
        if (frame.isEmpty() && frame.width() == 0 && frame.height() == 0)
            continue;
        if (drag_ == Drag::Resize || drag_ == Drag::Move) {
            if (singleSelection && dragMoved_)
                frame = liveFrame_;
        }
        QRectF viewFrame = toView(frame);
        painter.setPen(QPen(c.accent, 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(viewFrame);
        if (singleSelection && id != toQ(current->root.id)) {
            painter.setPen(QPen(c.accent, 1));
            painter.setBrush(Qt::white);
            std::vector<QPointF> points = {viewFrame.topLeft(), viewFrame.topRight(), viewFrame.bottomRight(), viewFrame.bottomLeft()};
            if (viewFrame.width() > kSideHandleRoom) {
                points.emplace_back(viewFrame.center().x(), viewFrame.top());
                points.emplace_back(viewFrame.center().x(), viewFrame.bottom());
            }
            if (viewFrame.height() > kSideHandleRoom) {
                points.emplace_back(viewFrame.right(), viewFrame.center().y());
                points.emplace_back(viewFrame.left(), viewFrame.center().y());
            }
            for (const auto& point : points)
                painter.drawRect(QRectF(point.x() - kHandleSize / 2, point.y() - kHandleSize / 2, kHandleSize, kHandleSize));
            QString label = QStringLiteral("%1 x %2").arg(static_cast<int>(std::lround(frame.width()))).arg(static_cast<int>(std::lround(frame.height())));
            QFont small = font();
            small.setPixelSize(11);
            painter.setFont(small);
            QFontMetrics metrics(small);
            QRectF tag(viewFrame.center().x() - metrics.horizontalAdvance(label) / 2.0 - 5, viewFrame.bottom() + 6, metrics.horizontalAdvance(label) + 10, 18);
            painter.setPen(Qt::NoPen);
            painter.setBrush(c.accent);
            painter.drawRoundedRect(tag, 3, 3);
            painter.setPen(c.accentText);
            painter.drawText(tag, Qt::AlignCenter, label);
        }
    }
    if (drag_ == Drag::Draw) {
        QColor fill = c.accent;
        fill.setAlpha(36);
        painter.setPen(QPen(c.accent, 1));
        painter.setBrush(fill);
        painter.drawRect(QRectF(worldToView(pressWorld_), worldToView(drawEnd_)).normalized());
    }
    if (altHeld_ && singleSelection && !hoverComponent_.isEmpty() && hoverScreen_ == controller_->currentScreen() && !controller_->selection().contains(hoverComponent_)) {
        QRectF from = worldFrameOf(controller_->currentScreen(), controller_->selection().front());
        QRectF to = worldFrameOf(controller_->currentScreen(), hoverComponent_);
        if (!from.isEmpty() && !to.isEmpty())
            paintMeasure(painter, from, to);
    }
    painter.setPen(QPen(c.danger, 1));
    for (const auto& guide : snapGuides_) {
        if (guide.vertical) {
            QPointF from = worldToView(QPointF(guide.position + current->canvas.x, guide.start + current->canvas.y));
            QPointF to = worldToView(QPointF(guide.position + current->canvas.x, guide.end + current->canvas.y));
            painter.drawLine(from, to);
        } else {
            QPointF from = worldToView(QPointF(guide.start + current->canvas.x, guide.position + current->canvas.y));
            QPointF to = worldToView(QPointF(guide.end + current->canvas.x, guide.position + current->canvas.y));
            painter.drawLine(from, to);
        }
    }
    if (!dropTarget_.parent.empty() && (drag_ == Drag::Move || !dropHoverScreen_.isEmpty())) {
        QString screenId = dropHoverScreen_.isEmpty() ? controller_->currentScreen() : dropHoverScreen_;
        QRectF parentFrame = worldFrameOf(screenId, toQ(dropTarget_.parent));
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(c.accent, 1, Qt::DashLine));
        painter.drawRect(toView(parentFrame));
        if (!dropTarget_.insideAbsolute && dropTarget_.indicator.width + dropTarget_.indicator.height > 0) {
            const ir::Screen* target = model->findScreen(fromQ(screenId));
            if (target != nullptr) {
                QRectF bar = qrect(dropTarget_.indicator).translated(target->canvas.x, target->canvas.y);
                painter.fillRect(toView(bar).adjusted(-1, -1, 1, 1), c.accent);
            }
        }
    }
    if (drag_ == Drag::Marquee) {
        QColor fill = c.accent;
        fill.setAlpha(30);
        painter.setPen(QPen(c.accent, 1));
        painter.setBrush(fill);
        painter.drawRect(marquee_);
    }
}

const ir::Screen* CanvasWidget::screenAt(QPointF world) const
{
    const ir::ApplicationModel* model = controller_->model();
    if (model == nullptr)
        return nullptr;
    for (auto it = model->screens.rbegin(); it != model->screens.rend(); ++it) {
        if (artboardRect(*it).contains(world))
            return &*it;
    }
    return nullptr;
}

QPointF CanvasWidget::localToScreen(const ir::Screen& screen, QPointF world) const
{
    return world - QPointF(screen.canvas.x, screen.canvas.y);
}

const ir::Component* CanvasWidget::rootOfCurrent() const
{
    const ir::ApplicationModel* model = controller_->model();
    if (model == nullptr)
        return nullptr;
    const ir::Screen* screen = model->findScreen(fromQ(controller_->currentScreen()));
    return screen != nullptr ? &screen->root : nullptr;
}

QRectF CanvasWidget::selectionBounds() const
{
    QRectF bounds;
    auto* self = const_cast<CanvasWidget*>(this);
    for (const auto& id : controller_->selection())
        bounds = bounds.united(self->worldFrameOf(controller_->currentScreen(), id));
    return bounds;
}

CanvasWidget::Handle CanvasWidget::handleAt(QPointF point) const
{
    if (controller_->selection().size() != 1)
        return Handle::None;
    const ir::Component* root = rootOfCurrent();
    if (root == nullptr || toQ(root->id) == controller_->selection().front())
        return Handle::None;
    QRectF frame = selectionBounds();
    QRectF v(worldToView(frame.topLeft()), worldToView(frame.bottomRight()));
    double tolerance = kHandleSize;
    auto near = [&](QPointF target) { return std::abs(point.x() - target.x()) <= tolerance && std::abs(point.y() - target.y()) <= tolerance; };
    if (near(v.topLeft()))
        return Handle::TopLeft;
    if (near(v.topRight()))
        return Handle::TopRight;
    if (near(v.bottomRight()))
        return Handle::BottomRight;
    if (near(v.bottomLeft()))
        return Handle::BottomLeft;
    if (v.width() > kSideHandleRoom && near(QPointF(v.center().x(), v.top())))
        return Handle::Top;
    if (v.height() > kSideHandleRoom && near(QPointF(v.right(), v.center().y())))
        return Handle::Right;
    if (v.width() > kSideHandleRoom && near(QPointF(v.center().x(), v.bottom())))
        return Handle::Bottom;
    if (v.height() > kSideHandleRoom && near(QPointF(v.left(), v.center().y())))
        return Handle::Left;
    return Handle::None;
}

void CanvasWidget::updateCursor(QPointF point)
{
    if (spaceHeld_ || tool_ == Tool::Pan) {
        setCursor(drag_ == Drag::Pan ? Qt::ClosedHandCursor : Qt::OpenHandCursor);
        return;
    }
    if (tool_ == Tool::Insert) {
        setCursor(Qt::CrossCursor);
        return;
    }
    if (addScreenRect().contains(viewToWorld(point))) {
        setCursor(Qt::PointingHandCursor);
        return;
    }
    switch (handleAt(point)) {
    case Handle::TopLeft:
    case Handle::BottomRight:
        setCursor(Qt::SizeFDiagCursor);
        break;
    case Handle::TopRight:
    case Handle::BottomLeft:
        setCursor(Qt::SizeBDiagCursor);
        break;
    case Handle::Top:
    case Handle::Bottom:
        setCursor(Qt::SizeVerCursor);
        break;
    case Handle::Left:
    case Handle::Right:
        setCursor(Qt::SizeHorCursor);
        break;
    case Handle::None:
        setCursor(Qt::ArrowCursor);
        break;
    }
}

void CanvasWidget::mousePressEvent(QMouseEvent* event)
{
    setFocus();
    commitInlineEdit();
    pressView_ = event->position();
    lastView_ = pressView_;
    pressWorld_ = viewToWorld(pressView_);
    dragMoved_ = false;
    if (!controller_->hasProject())
        return;
    if (event->button() == Qt::MiddleButton || (event->button() == Qt::LeftButton && (spaceHeld_ || tool_ == Tool::Pan))) {
        drag_ = Drag::Pan;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;
    if (tool_ == Tool::Insert) {
        if (screenAt(pressWorld_) == nullptr) {
            controller_->reportStatus(tr("Click inside a screen to place it"));
            return;
        }
        drag_ = Drag::Draw;
        drawEnd_ = pressWorld_;
        return;
    }
    if (rulers_ && (pressView_.x() < kRulerSize || pressView_.y() < kRulerSize)) {
        drag_ = Drag::Guide;
        guideVertical_ = pressView_.x() < kRulerSize;
        guidePosition_ = guideVertical_ ? pressWorld_.x() : pressWorld_.y();
        return;
    }
    Handle handle = handleAt(pressView_);
    if (handle != Handle::None) {
        drag_ = Drag::Resize;
        activeHandle_ = handle;
        dragStartFrame_ = selectionBounds();
        liveFrame_ = dragStartFrame_;
        return;
    }
    const ir::Screen* screen = screenAt(pressWorld_);
    if (screen == nullptr) {
        QPointF title = pressWorld_;
        const ir::ApplicationModel* model = controller_->model();
        for (const auto& candidate : model->screens) {
            QRectF artboard = artboardRect(candidate);
            QRectF titleBar(artboard.x(), artboard.y() - 24 / view_.scale, artboard.width(), 24 / view_.scale);
            if (titleBar.contains(title)) {
                controller_->setCurrentScreen(toQ(candidate.root.id));
                controller_->setSelection({toQ(candidate.root.id)});
                drag_ = Drag::MoveScreen;
                screenStartCanvas_ = QPointF(candidate.canvas.x, candidate.canvas.y);
                return;
            }
        }
        if (addScreenRect().contains(pressWorld_)) {
            controller_->addScreen();
            return;
        }
        if (!(event->modifiers() & Qt::ShiftModifier))
            controller_->setSelection({});
        drag_ = Drag::Marquee;
        marquee_ = QRectF(pressView_, QSizeF(0, 0));
        return;
    }
    controller_->setCurrentScreen(toQ(screen->root.id));
    QPointF local = localToScreen(*screen, pressWorld_);
    ScreenCache& cache = cacheFor(*screen);
    std::string hit = canvas::hitTest(cache.layout, local.x(), local.y());
    QString id = toQ(hit);
    if (id.isEmpty() || id == toQ(screen->root.id)) {
        if (!(event->modifiers() & Qt::ShiftModifier))
            controller_->setSelection({toQ(screen->root.id)});
        drag_ = Drag::Marquee;
        marquee_ = QRectF(pressView_, QSizeF(0, 0));
        return;
    }
    if (id.contains(QStringLiteral("__")))
        id = id.left(id.indexOf(QStringLiteral("__")));
    QStringList selection = controller_->selection();
    if (event->modifiers() & Qt::ShiftModifier) {
        if (selection.contains(id))
            selection.removeAll(id);
        else
            selection.removeAll(toQ(screen->root.id)), selection << id;
        controller_->setSelection(selection);
        return;
    }
    if (!selection.contains(id))
        controller_->setSelection({id});
    drag_ = Drag::Move;
    dragComponent_ = id;
    dragStartFrame_ = worldFrameOf(toQ(screen->root.id), id);
    liveFrame_ = dragStartFrame_;
    dragGrabOffset_ = pressWorld_ - dragStartFrame_.topLeft();
}

void CanvasWidget::mouseMoveEvent(QMouseEvent* event)
{
    QPointF point = event->position();
    QPointF world = viewToWorld(point);
    bool alt = event->modifiers() & Qt::AltModifier;
    if (alt != altHeld_) {
        altHeld_ = alt;
        update();
    }
    const ir::Screen* current = controller_->hasProject() ? controller_->model()->findScreen(fromQ(controller_->currentScreen())) : nullptr;
    switch (drag_) {
    case Drag::Draw:
        drawEnd_ = world;
        update();
        break;
    case Drag::Pan:
        view_.offsetX += point.x() - lastView_.x();
        view_.offsetY += point.y() - lastView_.y();
        update();
        break;
    case Drag::Guide:
        guidePosition_ = guideVertical_ ? world.x() : world.y();
        update();
        break;
    case Drag::Marquee:
        marquee_ = QRectF(pressView_, point).normalized();
        update();
        break;
    case Drag::MoveScreen:
        if (current != nullptr) {
            dragMoved_ = true;
            QPointF delta = world - pressWorld_;
            const_cast<ir::Screen*>(current)->canvas.x = std::round(screenStartCanvas_.x() + delta.x());
            const_cast<ir::Screen*>(current)->canvas.y = std::round(screenStartCanvas_.y() + delta.y());
            update();
        }
        break;
    case Drag::Move: {
        if (!dragMoved_ && (point - pressView_).manhattanLength() < 4)
            break;
        dragMoved_ = true;
        if (current == nullptr || controller_->selection().size() != 1)
            break;
        QRectF frame(world - dragGrabOffset_, dragStartFrame_.size());
        const layout::LayoutResult& layoutResult = cacheFor(*current).layout;
        const ir::Component* parent = ir::findParent(current->root, fromQ(dragComponent_));
        bool freePosition = parent != nullptr && (parent->layout.kind == ir::LayoutKind::Absolute || parent->layout.kind == ir::LayoutKind::Constraint);
        QPointF local = localToScreen(*current, world);
        dropTarget_ = design::findDropTarget(layoutResult, *controller_->model(), local.x(), local.y(), {fromQ(dragComponent_)});
        snapGuides_.clear();
        if (snap_ && freePosition && !(event->modifiers() & Qt::AltModifier)) {
            design::SnapOptions options;
            options.threshold = 6 / view_.scale;
            options.toGrid = grid_;
            for (const auto& [vertical, position] : guides_) {
                if (vertical)
                    options.verticalGuides.push_back(position - current->canvas.x);
                else
                    options.horizontalGuides.push_back(position - current->canvas.y);
            }
            layout::Rect moving{frame.x() - current->canvas.x, frame.y() - current->canvas.y, frame.width(), frame.height()};
            design::SnapResult snapped = design::snapMove(moving, layoutResult, {fromQ(dragComponent_)}, options);
            frame.translate(snapped.dx, snapped.dy);
            snapGuides_ = snapped.guides;
        }
        liveFrame_ = frame;
        update();
        break;
    }
    case Drag::Resize: {
        dragMoved_ = true;
        QRectF frame = dragStartFrame_;
        double dx = world.x() - pressWorld_.x();
        double dy = world.y() - pressWorld_.y();
        switch (activeHandle_) {
        case Handle::TopLeft:
            frame.setTopLeft(frame.topLeft() + QPointF(dx, dy));
            break;
        case Handle::Top:
            frame.setTop(frame.top() + dy);
            break;
        case Handle::TopRight:
            frame.setTopRight(frame.topRight() + QPointF(dx, dy));
            break;
        case Handle::Right:
            frame.setRight(frame.right() + dx);
            break;
        case Handle::BottomRight:
            frame.setBottomRight(frame.bottomRight() + QPointF(dx, dy));
            break;
        case Handle::Bottom:
            frame.setBottom(frame.bottom() + dy);
            break;
        case Handle::BottomLeft:
            frame.setBottomLeft(frame.bottomLeft() + QPointF(dx, dy));
            break;
        case Handle::Left:
            frame.setLeft(frame.left() + dx);
            break;
        case Handle::None:
            break;
        }
        if (frame.width() < 1)
            frame.setWidth(1);
        if (frame.height() < 1)
            frame.setHeight(1);
        liveFrame_ = frame;
        update();
        break;
    }
    case Drag::None: {
        updateCursor(point);
        QString hover;
        QString hoverScreen;
        if (controller_->hasProject() && tool_ != Tool::Insert) {
            if (const ir::Screen* screen = screenAt(world)) {
                QPointF local = localToScreen(*screen, world);
                QString id = toQ(canvas::hitTest(cacheFor(*screen).layout, local.x(), local.y()));
                if (id.contains(QStringLiteral("__")))
                    id = id.left(id.indexOf(QStringLiteral("__")));
                if (id != toQ(screen->root.id)) {
                    hover = id;
                    hoverScreen = toQ(screen->root.id);
                }
            }
        }
        if (hover != hoverComponent_ || hoverScreen != hoverScreen_) {
            hoverComponent_ = hover;
            hoverScreen_ = hoverScreen;
            update();
        }
        break;
    }
    }
    lastView_ = point;
}

void CanvasWidget::mouseReleaseEvent(QMouseEvent* event)
{
    Drag finished = drag_;
    drag_ = Drag::None;
    updateCursor(event->position());
    ir::ModelEditor* editor = controller_->editor();
    const ir::Screen* current = controller_->hasProject() ? controller_->model()->findScreen(fromQ(controller_->currentScreen())) : nullptr;
    QString screenId = controller_->currentScreen();
    switch (finished) {
    case Drag::Draw: {
        QString type = insertType_;
        QPointF start = pressWorld_;
        QRectF area = QRectF(start, drawEnd_).normalized();
        setTool(Tool::Select);
        if (!type.isEmpty())
            insertFromMime(type, start, area.width() * view_.scale > 8 && area.height() * view_.scale > 8 ? area.size() : QSizeF());
        update();
        break;
    }
    case Drag::Guide: {
        double start = guideVertical_ ? event->position().x() : event->position().y();
        if (start > kRulerSize)
            addGuide(guideVertical_, guidePosition_);
        break;
    }
    case Drag::Marquee: {
        if (current != nullptr && marquee_.width() > 3 && marquee_.height() > 3) {
            QPointF topLeft = localToScreen(*current, viewToWorld(marquee_.topLeft()));
            QPointF bottomRight = localToScreen(*current, viewToWorld(marquee_.bottomRight()));
            layout::Rect area{topLeft.x(), topLeft.y(), bottomRight.x() - topLeft.x(), bottomRight.y() - topLeft.y()};
            QStringList ids;
            for (const auto& id : canvas::marqueeTest(cacheFor(*current).layout, area, current->root.id))
                ids << toQ(id);
            if (!ids.isEmpty())
                controller_->setSelection(ids);
        }
        update();
        break;
    }
    case Drag::MoveScreen:
        if (current != nullptr && dragMoved_ && editor != nullptr) {
            double x = current->canvas.x;
            double y = current->canvas.y;
            const_cast<ir::Screen*>(current)->canvas.x = screenStartCanvas_.x();
            const_cast<ir::Screen*>(current)->canvas.y = screenStartCanvas_.y();
            auto status = editor->setScreenField(fromQ(screenId), "canvas", Json{{"x", x}, {"y", y}});
            (void)status;
        }
        break;
    case Drag::Move:
        if (dragMoved_ && current != nullptr && editor != nullptr && controller_->selection().size() == 1) {
            design::DesignOperations operations(*editor);
            const ir::Component* parent = ir::findParent(current->root, fromQ(dragComponent_));
            QPointF topLeft = localToScreen(*current, liveFrame_.topLeft());
            if (parent != nullptr && !dropTarget_.parent.empty()) {
                if (dropTarget_.insideAbsolute) {
                    const layout::LayoutNode* parentNode = cacheFor(*current).layout.find(dropTarget_.parent);
                    if (parentNode != nullptr) {
                        dropTarget_.x = std::round(topLeft.x() - parentNode->contentFrame.x);
                        dropTarget_.y = std::round(topLeft.y() - parentNode->contentFrame.y);
                    }
                }
                auto status = operations.reparentDrop(fromQ(screenId), fromQ(dragComponent_), dropTarget_);
                if (!status)
                    controller_->reportStatus(toQ(status.error().message));
            }
        }
        snapGuides_.clear();
        dropTarget_ = {};
        update();
        break;
    case Drag::Resize:
        if (dragMoved_ && current != nullptr && editor != nullptr) {
            design::DesignOperations operations(*editor);
            QPointF topLeft = localToScreen(*current, liveFrame_.topLeft());
            layout::Rect frame{topLeft.x(), topLeft.y(), liveFrame_.width(), liveFrame_.height()};
            auto status = operations.resize(fromQ(screenId), fromQ(controller_->selection().front()), frame, cacheFor(*current).layout);
            if (!status)
                controller_->reportStatus(toQ(status.error().message));
        }
        update();
        break;
    default:
        break;
    }
}

void CanvasWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !controller_->hasProject())
        return;
    QPointF world = viewToWorld(event->position());
    const ir::Screen* screen = screenAt(world);
    if (screen == nullptr)
        return;
    QPointF local = localToScreen(*screen, world);
    QString id = toQ(canvas::hitTest(cacheFor(*screen).layout, local.x(), local.y()));
    if (id.isEmpty() || id == toQ(screen->root.id))
        return;
    controller_->setSelection({id});
    beginInlineEdit(id);
}

void CanvasWidget::beginInlineEdit(const QString& component)
{
    const ir::Component* root = rootOfCurrent();
    if (root == nullptr)
        return;
    const ir::Component* target = ir::findComponent(*root, fromQ(component));
    if (target == nullptr)
        return;
    std::string key;
    if (target->type == "text")
        key = "content";
    else if (target->type == "button" || target->type == "checkbox" || target->type == "radio" || target->type == "switch")
        key = "label";
    else if (target->type == "textinput")
        key = "placeholder";
    else if (target->type == "navbar" || target->type == "dialog")
        key = "title";
    if (key.empty())
        return;
    QRectF frame = worldFrameOf(controller_->currentScreen(), component);
    QRectF viewFrame(worldToView(frame.topLeft()), worldToView(frame.bottomRight()));
    inlineEditor_ = new QLineEdit(this);
    inlineEditor_->setObjectName(QStringLiteral("inlineEditor"));
    inlineEditor_->setText(toQ(ir::propertyString(*target, key)));
    inlineEditor_->setGeometry(viewFrame.toRect().adjusted(0, 0, 0, std::max(0, 26 - static_cast<int>(viewFrame.height()))));
    inlineEditor_->setProperty("key", toQ(key));
    inlineComponent_ = component;
    inlineEditor_->selectAll();
    inlineEditor_->show();
    inlineEditor_->setFocus();
    connect(inlineEditor_, &QLineEdit::returnPressed, this, [this] { commitInlineEdit(); setFocus(); });
}

void CanvasWidget::commitInlineEdit()
{
    if (!inlineEditor_)
        return;
    QLineEdit* editorWidget = inlineEditor_;
    inlineEditor_ = nullptr;
    QString value = editorWidget->text();
    std::string key = "properties." + fromQ(editorWidget->property("key").toString());
    editorWidget->hide();
    editorWidget->deleteLater();
    if (ir::ModelEditor* editor = controller_->editor()) {
        auto status = editor->setField(fromQ(controller_->currentScreen()), fromQ(inlineComponent_), key, fromQ(value));
        if (!status)
            controller_->reportStatus(toQ(status.error().message));
    }
}

void CanvasWidget::wheelEvent(QWheelEvent* event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        double factor = std::pow(1.0015, event->angleDelta().y());
        setZoom(view_.scale * factor, event->position());
    } else {
        double dx = event->pixelDelta().isNull() ? event->angleDelta().x() / 2.0 : event->pixelDelta().x();
        double dy = event->pixelDelta().isNull() ? event->angleDelta().y() / 2.0 : event->pixelDelta().y();
        if (event->modifiers() & Qt::ShiftModifier)
            std::swap(dx, dy);
        view_.offsetX += dx;
        view_.offsetY += dy;
        update();
    }
    event->accept();
}

void CanvasWidget::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        spaceHeld_ = true;
        setCursor(Qt::OpenHandCursor);
        return;
    }
    ir::ModelEditor* editor = controller_->editor();
    if (editor == nullptr) {
        QWidget::keyPressEvent(event);
        return;
    }
    bool control = event->modifiers() & Qt::ControlModifier;
    if (event->key() == Qt::Key_Alt) {
        altHeld_ = true;
        update();
        return;
    }
    if (!control && !(event->modifiers() & Qt::AltModifier)) {
        static const std::map<int, QString> quick = {{Qt::Key_T, QStringLiteral("text")}, {Qt::Key_F, QStringLiteral("frame")}, {Qt::Key_R, QStringLiteral("shape")}, {Qt::Key_B, QStringLiteral("button")}, {Qt::Key_I, QStringLiteral("textinput")}};
        auto found = quick.find(event->key());
        if (found != quick.end()) {
            beginInsert(found->second);
            return;
        }
    }
    switch (event->key()) {
    case Qt::Key_Exclam:
        zoomToFit();
        return;
    case Qt::Key_At:
        zoomToSelection();
        return;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        deleteSelection();
        return;
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Up:
    case Qt::Key_Down: {
        double step = event->modifiers() & Qt::ShiftModifier ? 10 : 1;
        double dx = event->key() == Qt::Key_Left ? -step : event->key() == Qt::Key_Right ? step : 0;
        double dy = event->key() == Qt::Key_Up ? -step : event->key() == Qt::Key_Down ? step : 0;
        if (const layout::LayoutResult* layoutResult = layoutFor(controller_->currentScreen())) {
            std::vector<std::string> ids;
            for (const auto& id : controller_->selection())
                ids.push_back(fromQ(id));
            design::DesignOperations operations(*editor);
            auto status = operations.nudge(fromQ(controller_->currentScreen()), *layoutResult, ids, dx, dy);
            (void)status;
        }
        return;
    }
    case Qt::Key_Escape:
        if (tool_ == Tool::Insert) {
            setTool(Tool::Select);
            return;
        }
        controller_->setSelection({});
        return;
    case Qt::Key_D:
        if (control) {
            duplicateSelection();
            return;
        }
        break;
    case Qt::Key_G:
        if (control) {
            if (event->modifiers() & Qt::ShiftModifier)
                ungroupSelection();
            else
                groupSelection();
            return;
        }
        break;
    case Qt::Key_C:
        if (control) {
            copySelection();
            return;
        }
        break;
    case Qt::Key_X:
        if (control) {
            cutSelection();
            return;
        }
        break;
    case Qt::Key_V:
        if (control) {
            pasteClipboard();
            return;
        }
        setTool(Tool::Select);
        return;
    case Qt::Key_H:
        setTool(Tool::Pan);
        return;
    case Qt::Key_0:
        if (control) {
            zoomToFit();
            return;
        }
        break;
    case Qt::Key_1:
        if (control) {
            setZoom(1.0);
            return;
        }
        break;
    case Qt::Key_Equal:
    case Qt::Key_Plus:
        if (control) {
            zoomIn();
            return;
        }
        break;
    case Qt::Key_Minus:
        if (control) {
            zoomOut();
            return;
        }
        break;
    case Qt::Key_BracketRight:
        reorderSelection(control ? design::ZOrder::Front : design::ZOrder::Forward);
        return;
    case Qt::Key_BracketLeft:
        reorderSelection(control ? design::ZOrder::Back : design::ZOrder::Backward);
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (controller_->selection().size() == 1)
            beginInlineEdit(controller_->selection().front());
        return;
    default:
        break;
    }
    QWidget::keyPressEvent(event);
}

void CanvasWidget::keyReleaseEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Alt) {
        altHeld_ = false;
        update();
        return;
    }
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        spaceHeld_ = false;
        setCursor(tool_ == Tool::Pan ? Qt::OpenHandCursor : Qt::ArrowCursor);
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void CanvasWidget::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasFormat(QString::fromLatin1(kComponentMimeType)))
        event->acceptProposedAction();
}

void CanvasWidget::dragMoveEvent(QDragMoveEvent* event)
{
    QPointF world = viewToWorld(event->position());
    const ir::Screen* screen = screenAt(world);
    if (screen == nullptr) {
        dropTarget_ = {};
        dropHoverScreen_.clear();
        event->ignore();
        update();
        return;
    }
    QPointF local = localToScreen(*screen, world);
    dropHoverScreen_ = toQ(screen->root.id);
    dropTarget_ = design::findDropTarget(cacheFor(*screen).layout, *controller_->model(), local.x(), local.y(), {});
    event->acceptProposedAction();
    update();
}

void CanvasWidget::dragLeaveEvent(QDragLeaveEvent*)
{
    dropTarget_ = {};
    dropHoverScreen_.clear();
    update();
}

void CanvasWidget::dropEvent(QDropEvent* event)
{
    QString type = QString::fromUtf8(event->mimeData()->data(QString::fromLatin1(kComponentMimeType)));
    insertFromMime(type, viewToWorld(event->position()));
    dropTarget_ = {};
    dropHoverScreen_.clear();
    event->acceptProposedAction();
    update();
}

void CanvasWidget::insertFromMime(const QString& type, QPointF world, QSizeF size)
{
    ir::ModelEditor* editor = controller_->editor();
    if (editor == nullptr)
        return;
    const ir::Screen* screen = screenAt(world);
    if (screen == nullptr)
        return;
    QString screenId = toQ(screen->root.id);
    QPointF local = localToScreen(*screen, world);
    design::DropTarget target = design::findDropTarget(cacheFor(*screen).layout, *controller_->model(), local.x(), local.y(), {});
    std::string typeId = fromQ(type);
    ir::Component component;
    if (type.startsWith(QStringLiteral("instance:"))) {
        std::string definition = typeId.substr(9);
        const ir::ComponentDefinition* found = controller_->model()->findDefinition(definition);
        if (found == nullptr)
            return;
        component = ir::makeComponent(editor->uniqueComponentId(fromQ(screenId), definition), found->root.type, found->root.name);
        component.instanceOf = definition;
    } else {
        component = AppContext::instance().components().instantiate(typeId, editor->uniqueComponentId(fromQ(screenId), typeId));
    }
    if (target.insideAbsolute) {
        component.x = std::round(target.x);
        component.y = std::round(target.y);
    }
    ir::TransactionScope transaction(*editor, "Add " + component.name);
    auto inserted = editor->insertComponent(fromQ(screenId), target.parent.empty() ? fromQ(screenId) : target.parent, target.index, component);
    if (!inserted) {
        controller_->reportStatus(toQ(inserted.error().message));
        return;
    }
    if (size.isValid() && !size.isEmpty()) {
        auto first = editor->setField(fromQ(screenId), *inserted, "width", std::round(size.width()));
        auto second = editor->setField(fromQ(screenId), *inserted, "height", std::round(size.height()));
        (void)first;
        (void)second;
    }
    controller_->setCurrentScreen(screenId);
    controller_->setSelection({toQ(*inserted)});
    setFocus();
}

void CanvasWidget::contextMenuEvent(QContextMenuEvent* event)
{
    if (!controller_->hasProject())
        return;
    QPointF world = viewToWorld(event->pos());
    if (const ir::Screen* screen = screenAt(world)) {
        controller_->setCurrentScreen(toQ(screen->root.id));
        QPointF local = localToScreen(*screen, world);
        QString id = toQ(canvas::hitTest(cacheFor(*screen).layout, local.x(), local.y()));
        if (id.contains(QStringLiteral("__")))
            id = id.left(id.indexOf(QStringLiteral("__")));
        if (!id.isEmpty() && !controller_->selection().contains(id))
            controller_->setSelection({id});
    }
    QMenu menu(this);
    bool has = !controller_->selection().isEmpty();
    QString first = has ? controller_->selection().front() : QString();
    menu.addAction(tr("Go to code"), this, [this, first] { Q_EMIT openCodeRequested(controller_->currentScreen(), first); })->setEnabled(has);
    menu.addSeparator();
    menu.addAction(tr("Cut"), QKeySequence::Cut, this, &CanvasWidget::cutSelection)->setEnabled(has);
    menu.addAction(tr("Copy"), QKeySequence::Copy, this, &CanvasWidget::copySelection)->setEnabled(has);
    menu.addAction(tr("Paste"), QKeySequence::Paste, this, &CanvasWidget::pasteClipboard);
    menu.addAction(tr("Duplicate"), QKeySequence(Qt::CTRL | Qt::Key_D), this, &CanvasWidget::duplicateSelection)->setEnabled(has);
    menu.addAction(tr("Delete"), QKeySequence::Delete, this, &CanvasWidget::deleteSelection)->setEnabled(has);
    menu.addSeparator();
    menu.addAction(tr("Group"), QKeySequence(Qt::CTRL | Qt::Key_G), this, &CanvasWidget::groupSelection)->setEnabled(has);
    menu.addAction(tr("Ungroup"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G), this, &CanvasWidget::ungroupSelection)->setEnabled(has);
    menu.addSeparator();
    menu.addAction(tr("Bring forward"), this, [this] { reorderSelection(design::ZOrder::Forward); })->setEnabled(has);
    menu.addAction(tr("Send backward"), this, [this] { reorderSelection(design::ZOrder::Backward); })->setEnabled(has);
    menu.addSeparator();
    menu.addAction(tr("Make reusable..."), this, [this] {
        bool accepted = false;
        QString name = QInputDialog::getText(this, tr("Make reusable"), tr("Component name"), QLineEdit::Normal, QString(), &accepted);
        if (accepted && !name.trimmed().isEmpty())
            createReusableFromSelection(name.trimmed());
    })->setEnabled(controller_->selection().size() == 1);
    if (!guides_.empty())
        menu.addAction(tr("Clear guides"), this, &CanvasWidget::clearGuides);
    menu.exec(event->globalPos());
}

void CanvasWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (controller_->hasProject())
        layoutChrome();
    if (fitPending_)
        update();
}

static std::vector<std::string> toStd(const QStringList& list)
{
    std::vector<std::string> output;
    for (const auto& item : list)
        output.push_back(item.toStdString());
    return output;
}

void CanvasWidget::alignSelection(design::AlignEdge edge)
{
    ir::ModelEditor* editor = controller_->editor();
    const layout::LayoutResult* layoutResult = layoutFor(controller_->currentScreen());
    if (editor == nullptr || layoutResult == nullptr)
        return;
    design::DesignOperations operations(*editor);
    auto status = operations.align(fromQ(controller_->currentScreen()), *layoutResult, toStd(controller_->selection()), edge);
    if (!status)
        controller_->reportStatus(toQ(status.error().message));
}

void CanvasWidget::distributeSelection(design::DistributeAxis axis)
{
    ir::ModelEditor* editor = controller_->editor();
    const layout::LayoutResult* layoutResult = layoutFor(controller_->currentScreen());
    if (editor == nullptr || layoutResult == nullptr)
        return;
    design::DesignOperations operations(*editor);
    auto status = operations.distribute(fromQ(controller_->currentScreen()), *layoutResult, toStd(controller_->selection()), axis);
    if (!status)
        controller_->reportStatus(toQ(status.error().message));
}

void CanvasWidget::groupSelection()
{
    ir::ModelEditor* editor = controller_->editor();
    const layout::LayoutResult* layoutResult = layoutFor(controller_->currentScreen());
    if (editor == nullptr || layoutResult == nullptr)
        return;
    design::DesignOperations operations(*editor);
    auto group = operations.group(fromQ(controller_->currentScreen()), *layoutResult, toStd(controller_->selection()));
    if (group)
        controller_->setSelection({toQ(*group)});
    else
        controller_->reportStatus(toQ(group.error().message));
}

void CanvasWidget::ungroupSelection()
{
    ir::ModelEditor* editor = controller_->editor();
    const layout::LayoutResult* layoutResult = layoutFor(controller_->currentScreen());
    if (editor == nullptr || layoutResult == nullptr || controller_->selection().size() != 1)
        return;
    design::DesignOperations operations(*editor);
    auto status = operations.ungroup(fromQ(controller_->currentScreen()), *layoutResult, fromQ(controller_->selection().front()));
    if (status)
        controller_->setSelection({});
    else
        controller_->reportStatus(toQ(status.error().message));
}

void CanvasWidget::duplicateSelection()
{
    ir::ModelEditor* editor = controller_->editor();
    if (editor == nullptr)
        return;
    design::DesignOperations operations(*editor);
    auto created = operations.duplicate(fromQ(controller_->currentScreen()), toStd(controller_->selection()));
    if (created) {
        QStringList ids;
        for (const auto& id : *created)
            ids << toQ(id);
        controller_->setSelection(ids);
    } else {
        controller_->reportStatus(toQ(created.error().message));
    }
}

void CanvasWidget::deleteSelection()
{
    ir::ModelEditor* editor = controller_->editor();
    if (editor == nullptr)
        return;
    design::DesignOperations operations(*editor);
    QStringList selection = controller_->selection();
    controller_->setSelection({});
    auto status = operations.remove(fromQ(controller_->currentScreen()), toStd(selection));
    if (!status)
        controller_->reportStatus(toQ(status.error().message));
}

void CanvasWidget::copySelection()
{
    ir::ModelEditor* editor = controller_->editor();
    if (editor == nullptr || controller_->selection().isEmpty())
        return;
    design::DesignOperations operations(*editor);
    QApplication::clipboard()->setText(toQ(operations.copy(fromQ(controller_->currentScreen()), toStd(controller_->selection()))));
}

void CanvasWidget::cutSelection()
{
    copySelection();
    deleteSelection();
}

void CanvasWidget::pasteClipboard()
{
    ir::ModelEditor* editor = controller_->editor();
    const ir::Component* root = rootOfCurrent();
    if (editor == nullptr || root == nullptr)
        return;
    QString parent = toQ(root->id);
    if (controller_->selection().size() == 1) {
        const ir::Component* selected = ir::findComponent(*root, fromQ(controller_->selection().front()));
        const ir::ComponentSchema* schema = selected != nullptr ? AppContext::instance().components().find(selected->type) : nullptr;
        if (schema != nullptr && schema->container)
            parent = toQ(selected->id);
    }
    design::DesignOperations operations(*editor);
    auto created = operations.paste(fromQ(controller_->currentScreen()), fromQ(parent), fromQ(QApplication::clipboard()->text()));
    if (created) {
        QStringList ids;
        for (const auto& id : *created)
            ids << toQ(id);
        controller_->setSelection(ids);
    } else {
        controller_->reportStatus(toQ(created.error().message));
    }
}

void CanvasWidget::reorderSelection(design::ZOrder order)
{
    ir::ModelEditor* editor = controller_->editor();
    if (editor == nullptr)
        return;
    design::DesignOperations operations(*editor);
    for (const auto& id : controller_->selection()) {
        auto status = operations.reorder(fromQ(controller_->currentScreen()), fromQ(id), order);
        (void)status;
    }
}

void CanvasWidget::createReusableFromSelection(const QString& name)
{
    ir::ModelEditor* editor = controller_->editor();
    if (editor == nullptr || controller_->selection().size() != 1)
        return;
    design::DesignOperations operations(*editor);
    auto created = operations.createDefinition(fromQ(controller_->currentScreen()), fromQ(controller_->selection().front()), fromQ(name));
    if (!created)
        controller_->reportStatus(toQ(created.error().message));
    else
        controller_->reportStatus(tr("Created the reusable component %1").arg(name));
}

void CanvasWidget::addGuide(bool vertical, double position)
{
    guides_.emplace_back(vertical, position);
    update();
}

void CanvasWidget::clearGuides()
{
    guides_.clear();
    update();
}

}
