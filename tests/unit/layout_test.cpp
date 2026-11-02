#include <gtest/gtest.h>

#include "forma/ir/catalog.h"
#include "forma/layout/layout_engine.h"

using namespace forma;
using namespace forma::ir;
using namespace forma::layout;

namespace {

class FixedMeasurer final : public ITextMeasurer {
public:
    Size measure(std::string_view text, const ResolvedTextStyle& style, double maxWidth) const override
    {
        double width = static_cast<double>(text.size()) * 10;
        if (maxWidth > 0 && width > maxWidth)
            return {maxWidth, 20.0 * std::ceil(width / maxWidth)};
        return {width, 20};
    }
};

LayoutOptions options()
{
    static const FixedMeasurer measurer;
    LayoutOptions result;
    result.measurer = &measurer;
    return result;
}

Component box(const char* id, double width, double height)
{
    Component component = makeComponent(id, "shape");
    component.width = SizeSpec{SizeMode::Fixed, width};
    component.height = SizeSpec{SizeMode::Fixed, height};
    return component;
}

Component root(LayoutKind kind, double width = 400, double height = 300)
{
    Component component = makeComponent("root", "container");
    component.width = SizeSpec{SizeMode::Fixed, width};
    component.height = SizeSpec{SizeMode::Fixed, height};
    component.layout.kind = kind;
    return component;
}

Rect frameOf(const LayoutResult& result, const char* id)
{
    const LayoutNode* node = result.find(id);
    EXPECT_NE(node, nullptr) << id;
    return node != nullptr ? node->frame : Rect{};
}

}

TEST(Layout, VerticalStackWithSpacingAndPadding)
{
    Component parent = root(LayoutKind::Vertical);
    parent.layout.spacing = 10;
    parent.layout.padding = EdgeInsets{5, 6, 7, 8};
    parent.children.push_back(box("a", 100, 40));
    parent.children.push_back(box("b", 120, 30));
    auto result = computeLayout(parent, {400, 300}, options());
    EXPECT_EQ(frameOf(result, "a"), (Rect{8, 5, 100, 40}));
    EXPECT_EQ(frameOf(result, "b"), (Rect{8, 55, 120, 30}));
}

TEST(Layout, HorizontalStackJustifyVariants)
{
    Component parent = root(LayoutKind::Horizontal, 300, 100);
    parent.children.push_back(box("a", 50, 20));
    parent.children.push_back(box("b", 50, 20));

    parent.layout.justify = Justify::Center;
    auto centered = computeLayout(parent, {300, 100}, options());
    EXPECT_EQ(frameOf(centered, "a").x, 100);
    EXPECT_EQ(frameOf(centered, "b").x, 150);

    parent.layout.justify = Justify::End;
    auto ended = computeLayout(parent, {300, 100}, options());
    EXPECT_EQ(frameOf(ended, "b").right(), 300);

    parent.layout.justify = Justify::SpaceBetween;
    auto between = computeLayout(parent, {300, 100}, options());
    EXPECT_EQ(frameOf(between, "a").x, 0);
    EXPECT_EQ(frameOf(between, "b").right(), 300);

    parent.layout.justify = Justify::SpaceEvenly;
    auto evenly = computeLayout(parent, {300, 100}, options());
    EXPECT_NEAR(frameOf(evenly, "a").x, 200.0 / 3, 0.01);
}

TEST(Layout, CrossAlignment)
{
    Component parent = root(LayoutKind::Horizontal, 300, 100);
    parent.children.push_back(box("a", 50, 20));
    parent.layout.align = Align::Center;
    EXPECT_EQ(frameOf(computeLayout(parent, {300, 100}, options()), "a").y, 40);
    parent.layout.align = Align::End;
    EXPECT_EQ(frameOf(computeLayout(parent, {300, 100}, options()), "a").y, 80);
}

TEST(Layout, FillSharesRemainingSpaceByGrow)
{
    Component parent = root(LayoutKind::Horizontal, 300, 100);
    Component fixed = box("fixed", 100, 20);
    Component one = box("one", 0, 20);
    one.width = SizeSpec{SizeMode::Fill};
    Component two = box("two", 0, 20);
    two.width = SizeSpec{SizeMode::Fill};
    two.grow = 3;
    parent.children = {fixed, one, two};
    auto result = computeLayout(parent, {300, 100}, options());
    EXPECT_EQ(frameOf(result, "one").width, 50);
    EXPECT_EQ(frameOf(result, "two").width, 150);
    EXPECT_EQ(frameOf(result, "two").right(), 300);
}

TEST(Layout, FillRespectsMinAndMax)
{
    Component parent = root(LayoutKind::Horizontal, 300, 100);
    Component limited = box("limited", 0, 20);
    limited.width = SizeSpec{SizeMode::Fill, 0, std::nullopt, 120.0};
    Component rest = box("rest", 0, 20);
    rest.width = SizeSpec{SizeMode::Fill};
    parent.children = {limited, rest};
    auto result = computeLayout(parent, {300, 100}, options());
    EXPECT_EQ(frameOf(result, "limited").width, 120);
}

TEST(Layout, HugFollowsContent)
{
    Component hug = makeComponent("hug", "vstack");
    hug.layout.kind = LayoutKind::Vertical;
    hug.layout.spacing = 4;
    hug.layout.padding = EdgeInsets{10, 10, 10, 10};
    hug.children.push_back(box("a", 80, 30));
    hug.children.push_back(box("b", 100, 30));
    Component parent = root(LayoutKind::Vertical);
    parent.children.push_back(hug);
    auto result = computeLayout(parent, {400, 300}, options());
    EXPECT_EQ(frameOf(result, "hug").width, 120);
    EXPECT_EQ(frameOf(result, "hug").height, 84);
    EXPECT_EQ(frameOf(result, "b").y, 44);
}

TEST(Layout, TextHugsThenWraps)
{
    Component text = builtinCatalog().instantiate("text", "t");
    text.properties["content"] = "Hello";
    Component parent = root(LayoutKind::Vertical, 30, 100);
    parent.children.push_back(text);
    auto result = computeLayout(parent, {30, 100}, options());
    EXPECT_EQ(frameOf(result, "t").width, 30);
    EXPECT_EQ(frameOf(result, "t").height, 40);
}

TEST(Layout, PercentSizes)
{
    Component parent = root(LayoutKind::Vertical, 200, 100);
    Component half = box("half", 0, 10);
    half.width = SizeSpec{SizeMode::Percent, 50};
    parent.children.push_back(half);
    EXPECT_EQ(frameOf(computeLayout(parent, {200, 100}, options()), "half").width, 100);
}

TEST(Layout, AbsoluteUsesFrames)
{
    Component parent = root(LayoutKind::Absolute);
    Component child = box("a", 50, 60);
    child.x = 11;
    child.y = 22;
    parent.children.push_back(child);
    EXPECT_EQ(frameOf(computeLayout(parent, {400, 300}, options()), "a"), (Rect{11, 22, 50, 60}));
}

TEST(Layout, ConstraintPins)
{
    Component parent = root(LayoutKind::Constraint, 400, 300);
    Component stretched = box("stretched", 10, 40);
    stretched.pins.left = 10;
    stretched.pins.right = 20;
    stretched.pins.top = 5;
    stretched.width = SizeSpec{SizeMode::Hug};
    Component corner = box("corner", 50, 50);
    corner.pins.right = 8;
    corner.pins.bottom = 9;
    Component center = box("center", 100, 20);
    center.pins.centerX = 0;
    center.pins.centerY = 0;
    parent.children = {stretched, corner, center};
    auto result = computeLayout(parent, {400, 300}, options());
    EXPECT_EQ(frameOf(result, "stretched"), (Rect{10, 5, 370, 40}));
    EXPECT_EQ(frameOf(result, "corner"), (Rect{342, 241, 50, 50}));
    EXPECT_EQ(frameOf(result, "center"), (Rect{150, 140, 100, 20}));
}

TEST(Layout, GridAutoPlacesChildrenIntoTracks)
{
    Component parent = root(LayoutKind::Grid, 310, 200);
    parent.layout.spacing = 10;
    parent.layout.columns = {GridTrack{TrackKind::Fixed, 100}, GridTrack{TrackKind::Fraction, 1}};
    for (const char* id : {"a", "b", "c"}) {
        Component child = box(id, 0, 30);
        child.width = SizeSpec{SizeMode::Fill};
        parent.children.push_back(child);
    }
    auto result = computeLayout(parent, {310, 200}, options());
    EXPECT_EQ(frameOf(result, "a"), (Rect{0, 0, 100, 30}));
    EXPECT_EQ(frameOf(result, "b"), (Rect{110, 0, 200, 30}));
    EXPECT_EQ(frameOf(result, "c"), (Rect{0, 40, 100, 30}));
}

TEST(Layout, GridSpans)
{
    Component parent = root(LayoutKind::Grid, 300, 200);
    parent.layout.columns = {GridTrack{TrackKind::Fraction, 1}, GridTrack{TrackKind::Fraction, 1}, GridTrack{TrackKind::Fraction, 1}};
    Component wide = box("wide", 0, 30);
    wide.width = SizeSpec{SizeMode::Fill};
    wide.grid = GridPlacement{1, 0, 2, 1};
    parent.children = {wide};
    EXPECT_EQ(frameOf(computeLayout(parent, {300, 200}, options()), "wide"), (Rect{100, 0, 200, 30}));
}

TEST(Layout, FlexWrapsToNewRows)
{
    Component parent = root(LayoutKind::Flex, 220, 200);
    parent.layout.wrap = true;
    parent.layout.spacing = 10;
    for (const char* id : {"a", "b", "c"})
        parent.children.push_back(box(id, 100, 30));
    auto result = computeLayout(parent, {220, 200}, options());
    EXPECT_EQ(frameOf(result, "a"), (Rect{0, 0, 100, 30}));
    EXPECT_EQ(frameOf(result, "b"), (Rect{110, 0, 100, 30}));
    EXPECT_EQ(frameOf(result, "c"), (Rect{0, 40, 100, 30}));
}

TEST(Layout, HiddenChildrenTakeNoSpace)
{
    Component parent = root(LayoutKind::Vertical);
    parent.layout.spacing = 10;
    Component hidden = box("hidden", 100, 50);
    hidden.visible = false;
    parent.children = {hidden, box("shown", 100, 50)};
    auto result = computeLayout(parent, {400, 300}, options());
    EXPECT_EQ(frameOf(result, "shown").y, 0);
    EXPECT_FALSE(result.find("hidden")->visible);
}

TEST(Layout, OverflowShrinksChildren)
{
    Component parent = root(LayoutKind::Horizontal, 100, 50);
    parent.children = {box("a", 100, 20), box("b", 100, 20)};
    auto result = computeLayout(parent, {100, 50}, options());
    EXPECT_EQ(frameOf(result, "a").width, 50);
    EXPECT_EQ(frameOf(result, "b").width, 50);
}

TEST(Layout, ScrollContainerDoesNotShrink)
{
    Component parent = root(LayoutKind::Scroll, 100, 50);
    parent.children = {box("a", 40, 40), box("b", 40, 40)};
    auto result = computeLayout(parent, {100, 50}, options());
    EXPECT_EQ(frameOf(result, "b").y, 40);
    EXPECT_EQ(frameOf(result, "b").height, 40);
}

TEST(Layout, VariantsAndPlatformOverridesApply)
{
    ApplicationModel model = makeEmptyApplication("App", "com.example.app");
    Component parent = root(LayoutKind::Vertical);
    Component child = box("a", 100, 20);
    child.variants["wide"] = Json{{"width", 300}};
    child.platform["ios"] = Json{{"height", 44}};
    parent.children = {child};

    LayoutOptions base = options();
    base.model = &model;
    EXPECT_EQ(frameOf(computeLayout(parent, {400, 300}, base), "a").width, 100);

    LayoutOptions wide = base;
    wide.breakpoint = "wide";
    EXPECT_EQ(frameOf(computeLayout(parent, {400, 300}, wide), "a").width, 300);

    LayoutOptions ios = base;
    ios.platform = "ios";
    EXPECT_EQ(frameOf(computeLayout(parent, {400, 300}, ios), "a").height, 44);
}

TEST(Layout, InstancesExpandDefinitionsWithOverrides)
{
    ApplicationModel model = makeEmptyApplication("App", "com.example.app");
    ComponentDefinition definition;
    definition.root = builtinCatalog().instantiate("card", "user_card");
    Component label = builtinCatalog().instantiate("text", "label");
    label.properties["content"] = "Name";
    definition.root.children.push_back(label);
    model.components.push_back(definition);

    Component parent = root(LayoutKind::Vertical);
    Component instance = makeComponent("first", "card");
    instance.instanceOf = "user_card";
    instance.overrides["label"] = Json{{"properties.content", "Alice"}};
    parent.children = {instance};

    LayoutOptions base = options();
    base.model = &model;
    auto result = computeLayout(parent, {400, 300}, base);
    const LayoutNode* expanded = result.find("first__label");
    ASSERT_NE(expanded, nullptr);
    EXPECT_EQ(propertyString(*expanded->component, "content"), "Alice");
}

TEST(Layout, TokensResolveToValues)
{
    ApplicationModel model = makeEmptyApplication("App", "com.example.app");
    Component parent = root(LayoutKind::Vertical);
    parent.tokens["layout.spacing"] = "spacing.lg";
    parent.children = {box("a", 10, 10), box("b", 10, 10)};
    LayoutOptions base = options();
    base.model = &model;
    EXPECT_EQ(frameOf(computeLayout(parent, {400, 300}, base), "b").y, 34);
}

TEST(Layout, ResolvedTextStyleMergesTokensAndOverrides)
{
    ApplicationModel model = makeEmptyApplication("App", "com.example.app");
    Component text = builtinCatalog().instantiate("text", "t");
    text.typography.ref = "heading";
    text.typography.color = "$color.primary";
    auto style = resolveTextStyle(text, &model);
    EXPECT_EQ(style.size, 28);
    EXPECT_EQ(style.weight, 700);
    EXPECT_EQ(style.color, "#2B6CD4");
}
