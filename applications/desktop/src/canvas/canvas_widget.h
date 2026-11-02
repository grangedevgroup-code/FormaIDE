#pragma once

#include <QFrame>
#include <QHash>
#include <QLabel>
#include <QToolButton>
#include <QLineEdit>
#include <QPointer>
#include <QWidget>
#include <map>

#include "app/project_controller.h"
#include "canvas/canvas_renderer.h"
#include "forma/canvas/display_list.h"
#include "forma/design/design_engine.h"

namespace forma::ui {

constexpr const char* kComponentMimeType = "application/x-forma-component";

class CanvasWidget : public QWidget {
    Q_OBJECT

public:
    enum class Tool { Select, Pan, Insert };

    explicit CanvasWidget(ProjectController* controller, QWidget* parent = nullptr);

    void setTool(Tool tool);
    Tool tool() const { return tool_; }
    void beginInsert(const QString& type);
    QString insertType() const { return insertType_; }
    bool flowsVisible() const { return flows_; }
    void setFlowsVisible(bool on);
    QRectF addScreenRect() const;
    double zoom() const { return view_.scale; }
    void setZoom(double zoom, QPointF anchor = QPointF(-1, -1));
    void zoomIn();
    void zoomOut();
    void zoomToFit();
    void zoomToSelection();
    void centerOnScreen(const QString& screen);

    bool snapEnabled() const { return snap_; }
    void setSnapEnabled(bool on) { snap_ = on; }
    bool gridVisible() const { return grid_; }
    void setGridVisible(bool on);
    bool rulersVisible() const { return rulers_; }
    void setRulersVisible(bool on);

    void alignSelection(design::AlignEdge edge);
    void distributeSelection(design::DistributeAxis axis);
    void groupSelection();
    void ungroupSelection();
    void duplicateSelection();
    void deleteSelection();
    void copySelection();
    void cutSelection();
    void pasteClipboard();
    void reorderSelection(design::ZOrder order);
    void createReusableFromSelection(const QString& name);
    void addGuide(bool vertical, double position);
    void clearGuides();

    const layout::LayoutResult* layoutFor(const QString& screen);
    QRectF worldFrameOf(const QString& screen, const QString& component);
    QPointF worldToView(QPointF world) const;
    QPointF viewToWorld(QPointF view) const;
    QImage renderScreen(const QString& screen, double scale, bool includeBackground = true);
    int lastPaintMicroseconds() const { return lastPaintMicros_; }

Q_SIGNALS:
    void zoomChanged(double zoom);
    void toolChanged();
    void openCodeRequested(const QString& screen, const QString& component);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    struct ScreenCache {
        layout::LayoutResult layout;
        canvas::DisplayList list;
        std::uint64_t revision = 0;
    };

    enum class Drag { None, Pan, Move, Resize, Marquee, MoveScreen, Guide, Draw };
    enum class Handle { None, TopLeft, Top, TopRight, Right, BottomRight, Bottom, BottomLeft, Left };

    ScreenCache& cacheFor(const ir::Screen& screen);
    void invalidate(const QString& screen = {});
    void paintRulers(QPainter& painter);
    void paintGrid(QPainter& painter, const QRectF& artboard);
    void paintOverlays(QPainter& painter);
    void paintDots(QPainter& painter);
    void paintFlows(QPainter& painter);
    void paintMeasure(QPainter& painter, const QRectF& from, const QRectF& to);
    void buildChrome();
    void layoutChrome();
    void updateChrome();
    const ir::Screen* screenAt(QPointF world) const;
    QRectF artboardRect(const ir::Screen& screen) const;
    Handle handleAt(QPointF viewPoint) const;
    QRectF selectionBounds() const;
    void beginInlineEdit(const QString& component);
    void commitInlineEdit();
    void insertFromMime(const QString& type, QPointF world, QSizeF size = QSizeF());
    void updateCursor(QPointF viewPoint);
    QPointF localToScreen(const ir::Screen& screen, QPointF world) const;
    const ir::Component* rootOfCurrent() const;

    ProjectController* controller_;
    canvas::ViewTransform view_;
    Tool tool_ = Tool::Select;
    std::map<std::string, ScreenCache> caches_;
    QtTextMeasurer measurer_;
    bool snap_ = true;
    bool grid_ = false;
    bool rulers_ = false;
    bool flows_ = false;
    bool altHeld_ = false;
    QString insertType_;
    QPointF drawEnd_;
    QString hoverScreen_;
    QFrame* toolBar_ = nullptr;
    QFrame* zoomBar_ = nullptr;
    QLabel* zoomLabel_ = nullptr;
    QToolButton* flowsButton_ = nullptr;
    std::map<QString, QToolButton*> toolButtons_;
    std::vector<std::pair<QToolButton*, QString>> chromeIcons_;
    bool spaceHeld_ = false;

    Drag drag_ = Drag::None;
    Handle activeHandle_ = Handle::None;
    QPointF pressView_;
    QPointF pressWorld_;
    QPointF lastView_;
    QRectF dragStartFrame_;
    QRectF liveFrame_;
    bool dragMoved_ = false;
    std::vector<design::SnapGuide> snapGuides_;
    QRectF marquee_;
    design::DropTarget dropTarget_;
    QString dragComponent_;
    QPointF dragGrabOffset_;
    QString hoverComponent_;
    QString dropHoverScreen_;
    QPointF screenStartCanvas_;
    bool guideVertical_ = true;
    double guidePosition_ = 0;

    std::vector<std::pair<bool, double>> guides_;
    QPointer<QLineEdit> inlineEditor_;
    QString inlineComponent_;
    int lastPaintMicros_ = 0;
    bool fitPending_ = true;
};

}
