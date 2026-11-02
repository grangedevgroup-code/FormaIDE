#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "forma/ir/model.h"

namespace forma::layout {

struct Size {
    double width = 0;
    double height = 0;
    bool operator==(const Size&) const = default;
};

struct Rect {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
    bool operator==(const Rect&) const = default;
    double right() const { return x + width; }
    double bottom() const { return y + height; }
    double centerX() const { return x + width / 2; }
    double centerY() const { return y + height / 2; }
    bool contains(double px, double py) const { return px >= x && py >= y && px <= right() && py <= bottom(); }
    bool intersects(const Rect& other) const
    {
        return x < other.right() && other.x < right() && y < other.bottom() && other.y < bottom();
    }
};

struct ResolvedTextStyle {
    std::string font;
    double size = 15;
    int weight = 400;
    bool italic = false;
    bool underline = false;
    std::string color;
    std::string align = "left";
    double lineHeight = 0;
    double letterSpacing = 0;
};

class ITextMeasurer {
public:
    virtual ~ITextMeasurer() = default;
    virtual Size measure(std::string_view text, const ResolvedTextStyle& style, double maxWidth) const = 0;
};

class ApproximateTextMeasurer final : public ITextMeasurer {
public:
    Size measure(std::string_view text, const ResolvedTextStyle& style, double maxWidth) const override;
};

struct LayoutNode {
    std::string id;
    Rect frame;
    Rect contentFrame;
    const ir::Component* component = nullptr;
    int depth = 0;
    int parentIndex = -1;
    bool visible = true;
};

struct LayoutResult {
    Size size;
    std::vector<LayoutNode> nodes;
    std::map<std::string, int> index;

    const LayoutNode* find(std::string_view id) const;
};

struct LayoutOptions {
    const ir::ApplicationModel* model = nullptr;
    const ITextMeasurer* measurer = nullptr;
    std::string platform;
    std::string breakpoint;
    std::string state;
};

LayoutResult computeLayout(const ir::Component& root, Size viewport, const LayoutOptions& options);
LayoutResult computeScreenLayout(const ir::Screen& screen, const LayoutOptions& options);

ir::Component resolveComponent(const ir::Component& component, const LayoutOptions& options);
ResolvedTextStyle resolveTextStyle(const ir::Component& component, const ir::ApplicationModel* model);
Size intrinsicSize(const ir::Component& component, const LayoutOptions& options, double maxWidth);
std::string breakpointForWidth(const ir::ApplicationModel& model, double width);

}
