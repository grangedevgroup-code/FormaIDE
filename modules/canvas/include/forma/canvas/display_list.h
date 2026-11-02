#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "forma/layout/layout_engine.h"

namespace forma::canvas {

struct Color {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
    bool operator==(const Color&) const = default;
    bool visible() const { return a > 0; }
    static Color parse(std::string_view text, Color fallback = {0, 0, 0, 0});
};

enum class OpKind { Rect, Ellipse, Line, Text, Image, Glyph, PushClip, PopClip, PushOpacity, PopOpacity };

enum class Glyph {
    Check, Chevron, Back, Play, Star, Map, BarChart, LineChart, PieChart, Placeholder
};

struct DrawOp {
    OpKind kind = OpKind::Rect;
    layout::Rect rect;
    layout::Rect rectEnd;
    Color fill{0, 0, 0, 0};
    Color stroke{0, 0, 0, 0};
    double strokeWidth = 0;
    double radius = 0;
    bool dashed = false;
    std::string text;
    layout::ResolvedTextStyle textStyle;
    int textHAlign = 0;
    int textVAlign = 0;
    bool wrap = true;
    std::string imageSource;
    std::string imageFit = "cover";
    Glyph glyph = Glyph::Placeholder;
    double opacity = 1;
    double shadowX = 0;
    double shadowY = 0;
    double shadowBlur = 0;
    Color shadowColor{0, 0, 0, 0};
    std::string component;
};

struct DisplayList {
    std::vector<DrawOp> ops;
    layout::Rect bounds;
};

struct BuildOptions {
    bool showPlaceholders = true;
    std::string selectedTab;
};

DisplayList buildDisplayList(const layout::LayoutResult& layout, const ir::ApplicationModel& model, const BuildOptions& options = {});

struct ViewTransform {
    double scale = 1;
    double offsetX = 0;
    double offsetY = 0;
    double toViewX(double x) const { return x * scale + offsetX; }
    double toViewY(double y) const { return y * scale + offsetY; }
    double toWorldX(double x) const { return (x - offsetX) / scale; }
    double toWorldY(double y) const { return (y - offsetY) / scale; }
};

class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;
    virtual const char* name() const = 0;
    virtual void draw(const DisplayList& list, const ViewTransform& view) = 0;
};

std::string hitTest(const layout::LayoutResult& layout, double x, double y, bool includeLocked = false);
std::vector<std::string> marqueeTest(const layout::LayoutResult& layout, const layout::Rect& area, const std::string& rootId);
std::vector<std::string> ancestorsOf(const layout::LayoutResult& layout, const std::string& id);

}
