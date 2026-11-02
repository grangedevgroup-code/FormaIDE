#include "canvas/canvas_renderer.h"

#include <QFontMetricsF>
#include <QImageReader>
#include <QPainterPath>
#include <QPixmapCache>
#include <QSvgRenderer>
#include <QTextLayout>
#include <QTextOption>
#include <cmath>

#include "app/project_controller.h"

namespace forma::ui {

namespace {

QColor qcolor(const canvas::Color& color)
{
    return QColor(color.r, color.g, color.b, color.a);
}

QRectF qrect(const layout::Rect& rect)
{
    return QRectF(rect.x, rect.y, rect.width, rect.height);
}

}

QFont fontForStyle(const layout::ResolvedTextStyle& style)
{
    QFont font;
    if (!style.font.empty())
        font.setFamily(toQ(style.font));
    font.setPixelSize(std::max(1, static_cast<int>(std::lround(style.size))));
    font.setWeight(static_cast<QFont::Weight>(std::clamp(style.weight, 100, 900)));
    font.setItalic(style.italic);
    font.setUnderline(style.underline);
    if (style.letterSpacing != 0)
        font.setLetterSpacing(QFont::AbsoluteSpacing, style.letterSpacing);
    font.setHintingPreference(QFont::PreferNoHinting);
    return font;
}

layout::Size QtTextMeasurer::measure(std::string_view input, const layout::ResolvedTextStyle& style, double maxWidth) const
{
    QFontMetricsF metrics(fontForStyle(style));
    QString text = QString::fromUtf8(input.data(), static_cast<qsizetype>(input.size()));
    double lineHeight = style.lineHeight > 0 ? style.lineHeight : metrics.height();
    if (text.isEmpty())
        return {0, lineHeight};
    int flags = Qt::TextExpandTabs;
    QRectF area(0, 0, maxWidth > 0 ? maxWidth : 100000, 100000);
    if (maxWidth > 0)
        flags |= Qt::TextWordWrap;
    QRectF bounds = metrics.boundingRect(area, flags, text);
    int lines = std::max(1, static_cast<int>(std::lround(bounds.height() / metrics.lineSpacing())));
    return {std::ceil(bounds.width()), lineHeight * lines};
}

void PainterBackend::draw(const canvas::DisplayList& list, const canvas::ViewTransform&)
{
    for (const auto& op : list.ops)
        drawOp(op);
}

void PainterBackend::drawOp(const canvas::DrawOp& op)
{
    using canvas::OpKind;
    switch (op.kind) {
    case OpKind::Rect:
    case OpKind::Ellipse: {
        QRectF rect = qrect(op.rect);
        if (op.shadowColor.visible() && (op.shadowBlur > 0 || op.shadowX != 0 || op.shadowY != 0)) {
            QColor shadow = qcolor(op.shadowColor);
            int steps = std::clamp(static_cast<int>(op.shadowBlur / 2), 1, 8);
            painter_.save();
            painter_.setPen(Qt::NoPen);
            QColor layer = shadow;
            layer.setAlphaF(shadow.alphaF() / steps);
            painter_.setBrush(layer);
            for (int i = 0; i < steps; ++i) {
                double spread = op.shadowBlur * (1.0 - static_cast<double>(i) / steps) / 2;
                QRectF expanded = rect.translated(op.shadowX, op.shadowY).adjusted(-spread, -spread, spread, spread);
                if (op.kind == OpKind::Ellipse)
                    painter_.drawEllipse(expanded);
                else
                    painter_.drawRoundedRect(expanded, op.radius + spread, op.radius + spread);
            }
            painter_.restore();
        }
        painter_.save();
        if (op.strokeWidth > 0 && op.stroke.visible()) {
            QPen pen(qcolor(op.stroke), op.strokeWidth);
            pen.setJoinStyle(Qt::MiterJoin);
            if (op.dashed)
                pen.setStyle(Qt::DashLine);
            painter_.setPen(pen);
            rect.adjust(op.strokeWidth / 2, op.strokeWidth / 2, -op.strokeWidth / 2, -op.strokeWidth / 2);
        } else {
            painter_.setPen(Qt::NoPen);
        }
        painter_.setBrush(op.fill.visible() ? QBrush(qcolor(op.fill)) : QBrush(Qt::NoBrush));
        if (op.kind == OpKind::Ellipse)
            painter_.drawEllipse(rect);
        else if (op.radius > 0)
            painter_.drawRoundedRect(rect, op.radius, op.radius);
        else
            painter_.drawRect(rect);
        painter_.restore();
        break;
    }
    case OpKind::Line:
        painter_.save();
        painter_.setPen(QPen(qcolor(op.stroke), op.strokeWidth));
        painter_.drawLine(QPointF(op.rect.x, op.rect.centerY()), QPointF(op.rect.right(), op.rect.centerY()));
        painter_.restore();
        break;
    case OpKind::Text: {
        painter_.save();
        painter_.setFont(fontForStyle(op.textStyle));
        painter_.setPen(QColor(toQ(op.textStyle.color)));
        int flags = Qt::TextExpandTabs;
        flags |= op.textHAlign == 1 ? Qt::AlignHCenter : op.textHAlign == 2 ? Qt::AlignRight : Qt::AlignLeft;
        flags |= op.textVAlign == 1 ? Qt::AlignVCenter : Qt::AlignTop;
        if (op.wrap)
            flags |= Qt::TextWordWrap;
        painter_.setClipRect(qrect(op.rect).adjusted(-1, -1, 1, 1), Qt::IntersectClip);
        painter_.drawText(qrect(op.rect), flags, toQ(op.text));
        painter_.restore();
        break;
    }
    case OpKind::Image:
        drawImage(op);
        break;
    case OpKind::Glyph:
        drawGlyph(op);
        break;
    case OpKind::PushClip: {
        painter_.save();
        QPainterPath path;
        path.addRoundedRect(qrect(op.rect), op.radius, op.radius);
        painter_.setClipPath(path, Qt::IntersectClip);
        break;
    }
    case OpKind::PopClip:
        painter_.restore();
        break;
    case OpKind::PushOpacity:
        opacityStack_.push_back(painter_.opacity());
        painter_.setOpacity(painter_.opacity() * op.opacity);
        break;
    case OpKind::PopOpacity:
        if (!opacityStack_.isEmpty()) {
            painter_.setOpacity(opacityStack_.back());
            opacityStack_.pop_back();
        }
        break;
    }
}

void PainterBackend::drawImage(const canvas::DrawOp& op)
{
    QString path = toQ(op.imageSource);
    QRectF rect = qrect(op.rect);
    QPixmap pixmap;
    QString key = projectRoot_ + QLatin1Char('|') + path;
    if (!QPixmapCache::find(key, &pixmap)) {
        QString full = path.startsWith(QStringLiteral("http")) ? QString() : projectRoot_ + QLatin1Char('/') + path;
        if (full.endsWith(QStringLiteral(".svg"), Qt::CaseInsensitive)) {
            QSvgRenderer renderer(full);
            if (renderer.isValid()) {
                QPixmap rendered(512, 512);
                rendered.fill(Qt::transparent);
                QPainter imagePainter(&rendered);
                renderer.render(&imagePainter);
                imagePainter.end();
                pixmap = rendered;
            }
        } else if (!full.isEmpty()) {
            pixmap.load(full);
        }
        if (!pixmap.isNull())
            QPixmapCache::insert(key, pixmap);
    }
    painter_.save();
    QPainterPath clip;
    clip.addRoundedRect(rect, op.radius, op.radius);
    painter_.setClipPath(clip, Qt::IntersectClip);
    if (pixmap.isNull()) {
        painter_.fillRect(rect, QColor(200, 203, 210));
        painter_.setPen(QColor(90, 95, 105));
        painter_.drawText(rect, Qt::AlignCenter, QObject::tr("Missing image"));
    } else {
        QSizeF size = pixmap.size();
        QRectF target = rect;
        if (op.imageFit == "contain" || op.imageFit == "cover" || op.imageFit == "none") {
            QSizeF scaled = size;
            if (op.imageFit == "contain")
                scaled = size.scaled(rect.size(), Qt::KeepAspectRatio);
            else if (op.imageFit == "cover")
                scaled = size.scaled(rect.size(), Qt::KeepAspectRatioByExpanding);
            target = QRectF(rect.center().x() - scaled.width() / 2, rect.center().y() - scaled.height() / 2, scaled.width(), scaled.height());
        }
        painter_.setRenderHint(QPainter::SmoothPixmapTransform);
        painter_.drawPixmap(target, pixmap, QRectF(QPointF(0, 0), size));
    }
    painter_.restore();
}

void PainterBackend::drawGlyph(const canvas::DrawOp& op)
{
    QRectF r = qrect(op.rect);
    QColor tint = qcolor(op.stroke);
    painter_.save();
    QPen pen(tint, op.strokeWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter_.setPen(pen);
    painter_.setBrush(Qt::NoBrush);
    auto at = [&](double x, double y) { return QPointF(r.x() + r.width() * x, r.y() + r.height() * y); };
    switch (op.glyph) {
    case canvas::Glyph::Check: {
        QPolygonF line;
        line << at(0.12, 0.55) << at(0.4, 0.8) << at(0.88, 0.22);
        painter_.drawPolyline(line);
        break;
    }
    case canvas::Glyph::Chevron: {
        QPolygonF line;
        line << at(0.15, 0.35) << at(0.5, 0.7) << at(0.85, 0.35);
        painter_.drawPolyline(line);
        break;
    }
    case canvas::Glyph::Back: {
        QPolygonF line;
        line << at(0.65, 0.15) << at(0.3, 0.5) << at(0.65, 0.85);
        painter_.drawPolyline(line);
        break;
    }
    case canvas::Glyph::Play: {
        QPolygonF shape;
        shape << at(0.25, 0.15) << at(0.85, 0.5) << at(0.25, 0.85);
        painter_.setBrush(tint);
        painter_.drawPolygon(shape);
        break;
    }
    case canvas::Glyph::Star: {
        QPolygonF shape;
        for (int i = 0; i < 10; ++i) {
            double angle = -M_PI / 2 + i * M_PI / 5;
            double radius = i % 2 == 0 ? 0.48 : 0.2;
            shape << at(0.5 + radius * std::cos(angle), 0.5 + radius * std::sin(angle));
        }
        painter_.drawPolygon(shape);
        break;
    }
    case canvas::Glyph::Map: {
        for (double x : {0.25, 0.5, 0.75})
            painter_.drawLine(at(x, 0), at(x, 1));
        for (double y : {0.33, 0.66})
            painter_.drawLine(at(0, y), at(1, y));
        painter_.setBrush(tint);
        painter_.drawEllipse(at(0.5, 0.5), 4, 4);
        break;
    }
    case canvas::Glyph::BarChart: {
        painter_.setPen(Qt::NoPen);
        painter_.setBrush(tint);
        const double values[] = {0.5, 0.8, 0.35, 0.95, 0.6, 0.72};
        int index = 0;
        for (double value : values) {
            double x = index * (1.0 / 6) + 0.02;
            painter_.drawRect(QRectF(at(x, 1 - value), at(x + 1.0 / 6 - 0.04, 1)));
            ++index;
        }
        break;
    }
    case canvas::Glyph::LineChart: {
        QPolygonF line;
        const double values[] = {0.3, 0.55, 0.42, 0.8, 0.62, 0.9};
        for (int i = 0; i < 6; ++i)
            line << at(i / 5.0, 1 - values[i]);
        painter_.drawPolyline(line);
        break;
    }
    case canvas::Glyph::PieChart: {
        double size = std::min(r.width(), r.height());
        QRectF circle(r.center().x() - size / 2, r.center().y() - size / 2, size, size);
        painter_.setPen(Qt::NoPen);
        QColor slice = tint;
        painter_.setBrush(slice);
        painter_.drawPie(circle, 90 * 16, 160 * 16);
        slice.setAlphaF(0.6);
        painter_.setBrush(slice);
        painter_.drawPie(circle, 250 * 16, 120 * 16);
        slice.setAlphaF(0.3);
        painter_.setBrush(slice);
        painter_.drawPie(circle, 370 * 16, 80 * 16);
        break;
    }
    case canvas::Glyph::Placeholder: {
        painter_.drawRect(r.adjusted(1, 3, -1, -3));
        QPolygonF hill;
        hill << at(0.1, 0.8) << at(0.4, 0.45) << at(0.6, 0.65) << at(0.72, 0.55) << at(0.92, 0.8);
        painter_.drawPolyline(hill);
        break;
    }
    }
    painter_.restore();
}

}
