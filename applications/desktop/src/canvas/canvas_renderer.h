#pragma once

#include <QHash>
#include <QImage>
#include <QPainter>
#include <QString>

#include "forma/canvas/display_list.h"

namespace forma::ui {

class QtTextMeasurer final : public layout::ITextMeasurer {
public:
    layout::Size measure(std::string_view text, const layout::ResolvedTextStyle& style, double maxWidth) const override;
};

QFont fontForStyle(const layout::ResolvedTextStyle& style);

class PainterBackend final : public canvas::IRenderBackend {
public:
    PainterBackend(QPainter& painter, QString projectRoot) : painter_(painter), projectRoot_(std::move(projectRoot)) {}
    const char* name() const override { return "QPainter"; }
    void draw(const canvas::DisplayList& list, const canvas::ViewTransform& view) override;

private:
    void drawOp(const canvas::DrawOp& op);
    void drawGlyph(const canvas::DrawOp& op);
    void drawImage(const canvas::DrawOp& op);

    QPainter& painter_;
    QString projectRoot_;
    QVector<qreal> opacityStack_;
};

}
