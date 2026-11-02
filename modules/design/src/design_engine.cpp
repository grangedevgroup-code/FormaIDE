#include "forma/design/design_engine.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "forma/core/text.h"
#include "forma/ir/catalog.h"
#include "forma/ir/serialization.h"

namespace forma::design {

using namespace forma::ir;
using layout::LayoutNode;
using layout::LayoutResult;
using layout::Rect;

namespace {

bool isFreePositioned(const Component& parent)
{
    return parent.layout.kind == LayoutKind::Absolute;
}

const Component* rootOf(const ApplicationModel& model, std::string_view screen)
{
    if (const Screen* found = model.findScreen(screen))
        return &found->root;
    if (const ComponentDefinition* definition = model.findDefinition(screen))
        return &definition->root;
    return nullptr;
}

}

SnapResult snapMove(const Rect& moving, const LayoutResult& layout, const std::vector<std::string>& excluded, const SnapOptions& options)
{
    SnapResult result;
    double bestX = options.threshold + 1;
    double bestY = options.threshold + 1;
    std::vector<double> xCandidates = options.verticalGuides;
    std::vector<double> yCandidates = options.horizontalGuides;

    if (options.toObjects) {
        for (const auto& node : layout.nodes) {
            if (!node.visible || std::find(excluded.begin(), excluded.end(), node.id) != excluded.end())
                continue;
            xCandidates.push_back(node.frame.x);
            xCandidates.push_back(node.frame.centerX());
            xCandidates.push_back(node.frame.right());
            yCandidates.push_back(node.frame.y);
            yCandidates.push_back(node.frame.centerY());
            yCandidates.push_back(node.frame.bottom());
        }
    }

    const double movingX[3] = {moving.x, moving.centerX(), moving.right()};
    const double movingY[3] = {moving.y, moving.centerY(), moving.bottom()};
    for (double candidate : xCandidates) {
        for (double edge : movingX) {
            double distance = candidate - edge;
            if (std::abs(distance) < std::abs(bestX) && std::abs(distance) <= options.threshold)
                bestX = distance;
        }
    }
    for (double candidate : yCandidates) {
        for (double edge : movingY) {
            double distance = candidate - edge;
            if (std::abs(distance) < std::abs(bestY) && std::abs(distance) <= options.threshold)
                bestY = distance;
        }
    }
    bool snappedX = std::abs(bestX) <= options.threshold;
    bool snappedY = std::abs(bestY) <= options.threshold;

    if (options.toGrid && options.gridSize > 0) {
        if (!snappedX) {
            bestX = std::round(moving.x / options.gridSize) * options.gridSize - moving.x;
            snappedX = true;
        }
        if (!snappedY) {
            bestY = std::round(moving.y / options.gridSize) * options.gridSize - moving.y;
            snappedY = true;
        }
    }
    if (snappedX)
        result.dx = bestX;
    if (snappedY)
        result.dy = bestY;

    Rect snapped{moving.x + result.dx, moving.y + result.dy, moving.width, moving.height};
    const double snappedXs[3] = {snapped.x, snapped.centerX(), snapped.right()};
    const double snappedYs[3] = {snapped.y, snapped.centerY(), snapped.bottom()};
    if (snappedX && std::abs(result.dx) <= options.threshold) {
        for (double edge : snappedXs) {
            if (std::any_of(xCandidates.begin(), xCandidates.end(), [&](double value) { return std::abs(value - edge) < 0.01; }))
                result.guides.push_back({true, edge, std::min(0.0, snapped.y), std::max(layout.size.height, snapped.bottom())});
        }
    }
    if (snappedY && std::abs(result.dy) <= options.threshold) {
        for (double edge : snappedYs) {
            if (std::any_of(yCandidates.begin(), yCandidates.end(), [&](double value) { return std::abs(value - edge) < 0.01; }))
                result.guides.push_back({false, edge, std::min(0.0, snapped.x), std::max(layout.size.width, snapped.right())});
        }
    }
    return result;
}

DropTarget findDropTarget(const LayoutResult& layout, const ApplicationModel& model, double x, double y, const std::vector<std::string>& excluded)
{
    DropTarget target;
    auto isExcluded = [&](const LayoutNode& node) {
        int cursor = static_cast<int>(&node - layout.nodes.data());
        while (cursor >= 0) {
            const auto& current = layout.nodes[static_cast<std::size_t>(cursor)];
            if (std::find(excluded.begin(), excluded.end(), current.id) != excluded.end())
                return true;
            cursor = current.parentIndex;
        }
        return false;
    };

    const LayoutNode* best = nullptr;
    for (auto it = layout.nodes.rbegin(); it != layout.nodes.rend(); ++it) {
        const LayoutNode& node = *it;
        if (!node.visible || node.component == nullptr || isExcluded(node) || !node.frame.contains(x, y) || node.component->locked)
            continue;
        const ComponentSchema* schema = builtinCatalog().find(node.component->type);
        bool container = node.component->type == "screen" || (schema != nullptr && schema->container);
        if (!container || !node.component->instanceOf.empty())
            continue;
        best = &node;
        break;
    }
    if (best == nullptr) {
        if (layout.nodes.empty())
            return target;
        best = &layout.nodes.front();
    }
    (void)model;
    const Component& parent = *best->component;
    target.parent = parent.id;
    target.x = x - best->contentFrame.x;
    target.y = y - best->contentFrame.y;
    if (isFreePositioned(parent) || parent.layout.kind == LayoutKind::Constraint) {
        target.insideAbsolute = true;
        target.index = static_cast<int>(parent.children.size());
        return target;
    }

    bool horizontal = parent.layout.kind == LayoutKind::Horizontal || parent.layout.kind == LayoutKind::Flex
        || (parent.layout.kind == LayoutKind::Scroll && parent.layout.scroll == ScrollAxis::Horizontal);
    int bestIndex = 0;
    int visibleIndex = 0;
    double indicatorPosition = horizontal ? best->contentFrame.x : best->contentFrame.y;
    for (const auto& childComponent : parent.children) {
        if (std::find(excluded.begin(), excluded.end(), childComponent.id) != excluded.end()) {
            ++visibleIndex;
            continue;
        }
        const LayoutNode* child = layout.find(childComponent.id);
        if (child == nullptr || !child->visible) {
            ++visibleIndex;
            continue;
        }
        double mid = horizontal ? child->frame.centerX() : child->frame.centerY();
        double pointer = horizontal ? x : y;
        if (pointer > mid) {
            bestIndex = visibleIndex + 1;
            indicatorPosition = (horizontal ? child->frame.right() : child->frame.bottom()) + parent.layout.spacing / 2;
        }
        ++visibleIndex;
    }
    target.index = bestIndex;
    if (horizontal) {
        target.indicator = Rect{indicatorPosition - 1, best->contentFrame.y, 2, best->contentFrame.height};
        target.indicatorVertical = true;
    } else {
        target.indicator = Rect{best->contentFrame.x, indicatorPosition - 1, best->contentFrame.width, 2};
        target.indicatorVertical = false;
    }
    return target;
}

Status DesignOperations::align(std::string_view screen, const LayoutResult& layout, const std::vector<std::string>& ids, AlignEdge edge)
{
    if (ids.empty())
        return {};
    std::vector<const LayoutNode*> nodes;
    for (const auto& id : ids) {
        if (const LayoutNode* node = layout.find(id))
            nodes.push_back(node);
    }
    if (nodes.empty())
        return {};
    double reference = 0;
    if (nodes.size() == 1) {
        const LayoutNode* parent = nodes[0]->parentIndex >= 0 ? &layout.nodes[static_cast<std::size_t>(nodes[0]->parentIndex)] : nullptr;
        if (parent == nullptr)
            return {};
        const Rect& frame = parent->contentFrame;
        switch (edge) {
        case AlignEdge::Left:
            reference = frame.x;
            break;
        case AlignEdge::CenterX:
            reference = frame.centerX();
            break;
        case AlignEdge::Right:
            reference = frame.right();
            break;
        case AlignEdge::Top:
            reference = frame.y;
            break;
        case AlignEdge::CenterY:
            reference = frame.centerY();
            break;
        case AlignEdge::Bottom:
            reference = frame.bottom();
            break;
        }
    } else {
        double minX = nodes[0]->frame.x;
        double maxX = nodes[0]->frame.right();
        double minY = nodes[0]->frame.y;
        double maxY = nodes[0]->frame.bottom();
        for (const LayoutNode* node : nodes) {
            minX = std::min(minX, node->frame.x);
            maxX = std::max(maxX, node->frame.right());
            minY = std::min(minY, node->frame.y);
            maxY = std::max(maxY, node->frame.bottom());
        }
        switch (edge) {
        case AlignEdge::Left:
            reference = minX;
            break;
        case AlignEdge::CenterX:
            reference = (minX + maxX) / 2;
            break;
        case AlignEdge::Right:
            reference = maxX;
            break;
        case AlignEdge::Top:
            reference = minY;
            break;
        case AlignEdge::CenterY:
            reference = (minY + maxY) / 2;
            break;
        case AlignEdge::Bottom:
            reference = maxY;
            break;
        }
    }
    TransactionScope transaction(editor_, "Align");
    for (const LayoutNode* node : nodes) {
        double dx = 0;
        double dy = 0;
        switch (edge) {
        case AlignEdge::Left:
            dx = reference - node->frame.x;
            break;
        case AlignEdge::CenterX:
            dx = reference - node->frame.centerX();
            break;
        case AlignEdge::Right:
            dx = reference - node->frame.right();
            break;
        case AlignEdge::Top:
            dy = reference - node->frame.y;
            break;
        case AlignEdge::CenterY:
            dy = reference - node->frame.centerY();
            break;
        case AlignEdge::Bottom:
            dy = reference - node->frame.bottom();
            break;
        }
        if (dx != 0) {
            if (auto status = editor_.setField(screen, node->id, "x", node->component->x + dx); !status)
                return status;
        }
        if (dy != 0) {
            if (auto status = editor_.setField(screen, node->id, "y", node->component->y + dy); !status)
                return status;
        }
    }
    return {};
}

Status DesignOperations::distribute(std::string_view screen, const LayoutResult& layout, const std::vector<std::string>& ids, DistributeAxis axis)
{
    std::vector<const LayoutNode*> nodes;
    for (const auto& id : ids) {
        if (const LayoutNode* node = layout.find(id))
            nodes.push_back(node);
    }
    if (nodes.size() < 3)
        return {};
    bool horizontal = axis == DistributeAxis::Horizontal;
    std::sort(nodes.begin(), nodes.end(), [&](const LayoutNode* a, const LayoutNode* b) {
        return horizontal ? a->frame.x < b->frame.x : a->frame.y < b->frame.y;
    });
    double start = horizontal ? nodes.front()->frame.right() : nodes.front()->frame.bottom();
    double end = horizontal ? nodes.back()->frame.x : nodes.back()->frame.y;
    double totalSize = 0;
    for (std::size_t i = 1; i + 1 < nodes.size(); ++i)
        totalSize += horizontal ? nodes[i]->frame.width : nodes[i]->frame.height;
    double gap = (end - start - totalSize) / static_cast<double>(nodes.size() - 1);
    double cursor = start + gap;
    TransactionScope transaction(editor_, "Distribute");
    for (std::size_t i = 1; i + 1 < nodes.size(); ++i) {
        const LayoutNode* node = nodes[i];
        double delta = cursor - (horizontal ? node->frame.x : node->frame.y);
        double current = horizontal ? node->component->x : node->component->y;
        if (auto status = editor_.setField(screen, node->id, horizontal ? "x" : "y", current + delta); !status)
            return status;
        cursor += (horizontal ? node->frame.width : node->frame.height) + gap;
    }
    return {};
}

Result<std::string> DesignOperations::group(std::string_view screen, const LayoutResult& layout, const std::vector<std::string>& ids)
{
    if (ids.empty())
        return makeError("design.group", "Select at least one component to group");
    const Component* root = rootOf(editor_.model(), screen);
    if (root == nullptr)
        return makeError("ir.missing", "No such screen");
    const Component* parent = findParent(*root, ids.front());
    if (parent == nullptr)
        return makeError("design.group", "The root of a screen cannot be grouped");
    double minX = 1e12;
    double minY = 1e12;
    double maxX = -1e12;
    double maxY = -1e12;
    int firstIndex = static_cast<int>(parent->children.size());
    for (const auto& id : ids) {
        if (findParent(*root, id) != parent)
            return makeError("design.group", "Grouped components must share a parent");
        const LayoutNode* node = layout.find(id);
        if (node == nullptr)
            return makeError("ir.missing", "No such component: " + id);
        minX = std::min(minX, node->frame.x);
        minY = std::min(minY, node->frame.y);
        maxX = std::max(maxX, node->frame.right());
        maxY = std::max(maxY, node->frame.bottom());
        firstIndex = std::min(firstIndex, indexInParent(*parent, id));
    }
    const LayoutNode* parentNode = layout.find(parent->id);
    std::string parentId = parent->id;
    Component group = builtinCatalog().instantiate("group", editor_.uniqueComponentId(screen, "group"));
    group.x = minX - parentNode->contentFrame.x;
    group.y = minY - parentNode->contentFrame.y;
    group.width = SizeSpec{SizeMode::Fixed, maxX - minX};
    group.height = SizeSpec{SizeMode::Fixed, maxY - minY};

    TransactionScope transaction(editor_, "Group");
    auto inserted = editor_.insertComponent(screen, parentId, firstIndex, group);
    if (!inserted)
        return inserted.error();
    std::vector<std::string> ordered;
    {
        const Component* currentRoot = rootOf(editor_.model(), screen);
        const Component* currentParent = findComponent(*currentRoot, parentId);
        for (const auto& child : currentParent->children) {
            if (std::find(ids.begin(), ids.end(), child.id) != ids.end())
                ordered.push_back(child.id);
        }
    }
    int position = 0;
    for (const auto& id : ordered) {
        const LayoutNode* node = layout.find(id);
        if (auto status = editor_.moveComponent(screen, id, *inserted, position++); !status)
            return status.error();
        if (auto status = editor_.setField(screen, id, "x", node->frame.x - minX); !status)
            return status.error();
        if (auto status = editor_.setField(screen, id, "y", node->frame.y - minY); !status)
            return status.error();
        Json width = getComponentField(*findComponent(*rootOf(editor_.model(), screen), id), "width");
        if (width.is_null() || width == "fill") {
            if (auto status = editor_.setField(screen, id, "width", node->frame.width); !status)
                return status.error();
        }
        Json height = getComponentField(*findComponent(*rootOf(editor_.model(), screen), id), "height");
        if (height.is_null() || height == "fill") {
            if (auto status = editor_.setField(screen, id, "height", node->frame.height); !status)
                return status.error();
        }
    }
    return inserted;
}

Status DesignOperations::ungroup(std::string_view screen, const LayoutResult& layout, std::string_view id)
{
    const Component* root = rootOf(editor_.model(), screen);
    if (root == nullptr)
        return makeError("ir.missing", "No such screen");
    const Component* group = findComponent(*root, id);
    const Component* parent = findParent(*root, id);
    if (group == nullptr || parent == nullptr)
        return makeError("ir.missing", "No such group");
    if (group->children.empty())
        return makeError("design.ungroup", "This component has nothing to ungroup");
    std::string parentId = parent->id;
    int index = indexInParent(*parent, id);
    const LayoutNode* parentNode = layout.find(parentId);
    std::vector<std::string> childIds;
    for (const auto& child : group->children)
        childIds.push_back(child.id);
    TransactionScope transaction(editor_, "Ungroup");
    for (const auto& child : childIds) {
        const LayoutNode* node = layout.find(child);
        if (auto status = editor_.moveComponent(screen, child, parentId, index++); !status)
            return status;
        if (node != nullptr && parentNode != nullptr) {
            if (auto status = editor_.setField(screen, child, "x", node->frame.x - parentNode->contentFrame.x); !status)
                return status;
            if (auto status = editor_.setField(screen, child, "y", node->frame.y - parentNode->contentFrame.y); !status)
                return status;
        }
    }
    return editor_.removeComponent(screen, id);
}

Status DesignOperations::reorder(std::string_view screen, std::string_view id, ZOrder order)
{
    const Component* root = rootOf(editor_.model(), screen);
    if (root == nullptr)
        return makeError("ir.missing", "No such screen");
    const Component* parent = findParent(*root, id);
    if (parent == nullptr)
        return {};
    int index = indexInParent(*parent, id);
    int last = static_cast<int>(parent->children.size()) - 1;
    int target = index;
    switch (order) {
    case ZOrder::Forward:
        target = std::min(last, index + 1);
        break;
    case ZOrder::Backward:
        target = std::max(0, index - 1);
        break;
    case ZOrder::Front:
        target = last;
        break;
    case ZOrder::Back:
        target = 0;
        break;
    }
    if (target == index)
        return {};
    return editor_.moveComponent(screen, id, parent->id, target);
}

Result<std::vector<std::string>> DesignOperations::duplicate(std::string_view screen, const std::vector<std::string>& ids, double offset)
{
    const Component* root = rootOf(editor_.model(), screen);
    if (root == nullptr)
        return makeError("ir.missing", "No such screen");
    std::vector<std::string> created;
    TransactionScope transaction(editor_, "Duplicate");
    for (const auto& id : ids) {
        root = rootOf(editor_.model(), screen);
        const Component* source = findComponent(*root, id);
        const Component* parent = findParent(*root, id);
        if (source == nullptr || parent == nullptr)
            continue;
        Component copy = *source;
        std::function<void(Component&)> rename = [&](Component& node) {
            node.id.clear();
            for (auto& child : node.children)
                rename(child);
        };
        copy.id = editor_.uniqueComponentId(screen, source->id);
        for (auto& child : copy.children)
            rename(child);
        if (parent->layout.kind == LayoutKind::Absolute) {
            copy.x += offset;
            copy.y += offset;
        }
        int index = indexInParent(*parent, id) + 1;
        std::string parentId = parent->id;
        std::function<void(Component&)> fill = [&](Component& node) {
            for (auto& child : node.children) {
                if (child.id.empty())
                    child.id = child.type;
                fill(child);
            }
        };
        fill(copy);
        auto inserted = editor_.insertComponent(screen, parentId, index, std::move(copy));
        if (!inserted)
            return inserted.error();
        created.push_back(*inserted);
    }
    return created;
}

Status DesignOperations::remove(std::string_view screen, const std::vector<std::string>& ids)
{
    TransactionScope transaction(editor_, ids.size() > 1 ? "Delete components" : "Delete component");
    for (const auto& id : ids) {
        const Component* root = rootOf(editor_.model(), screen);
        if (root == nullptr || findComponent(*root, id) == nullptr)
            continue;
        if (root->id == id)
            continue;
        if (auto status = editor_.removeComponent(screen, id); !status)
            return status;
    }
    return {};
}

Status DesignOperations::nudge(std::string_view screen, const LayoutResult& layout, const std::vector<std::string>& ids, double dx, double dy)
{
    TransactionScope transaction(editor_, "Move");
    const Component* root = rootOf(editor_.model(), screen);
    if (root == nullptr)
        return makeError("ir.missing", "No such screen");
    for (const auto& id : ids) {
        const Component* component = findComponent(*root, id);
        const Component* parent = findParent(*root, id);
        if (component == nullptr || parent == nullptr || component->locked)
            continue;
        if (parent->layout.kind == LayoutKind::Absolute) {
            if (dx != 0) {
                if (auto status = editor_.setField(screen, id, "x", component->x + dx); !status)
                    return status;
            }
            if (dy != 0) {
                if (auto status = editor_.setField(screen, id, "y", component->y + dy); !status)
                    return status;
            }
            root = rootOf(editor_.model(), screen);
        } else if (parent->layout.kind == LayoutKind::Constraint) {
            const LayoutNode* node = layout.find(id);
            const LayoutNode* parentNode = layout.find(parent->id);
            if (node == nullptr || parentNode == nullptr)
                continue;
            Rect frame = node->frame;
            frame.x += dx;
            frame.y += dy;
            if (auto status = moveTo(screen, id, frame.x - parentNode->contentFrame.x, frame.y - parentNode->contentFrame.y); !status)
                return status;
            root = rootOf(editor_.model(), screen);
        }
    }
    return {};
}

Status DesignOperations::moveTo(std::string_view screen, std::string_view id, double x, double y)
{
    const Component* root = rootOf(editor_.model(), screen);
    if (root == nullptr)
        return makeError("ir.missing", "No such screen");
    const Component* component = findComponent(*root, id);
    const Component* parent = findParent(*root, id);
    if (component == nullptr || parent == nullptr)
        return makeError("ir.missing", "No such component");
    TransactionScope transaction(editor_, "Move");
    if (parent->layout.kind == LayoutKind::Constraint && component->pins.any()) {
        if (component->pins.left || component->pins.right || component->pins.centerX) {
            if (!component->pins.left && !component->pins.right) {
                if (auto status = editor_.setField(screen, id, "pins.centerX", nullptr); !status)
                    return status;
            } else if (component->pins.right && !component->pins.left) {
                if (auto status = editor_.setField(screen, id, "pins.right", nullptr); !status)
                    return status;
            }
            if (auto status = editor_.setField(screen, id, "pins.left", x); !status)
                return status;
        }
        root = rootOf(editor_.model(), screen);
        component = findComponent(*root, id);
        if (component->pins.top || component->pins.bottom || component->pins.centerY) {
            if (!component->pins.top && !component->pins.bottom) {
                if (auto status = editor_.setField(screen, id, "pins.centerY", nullptr); !status)
                    return status;
            } else if (component->pins.bottom && !component->pins.top) {
                if (auto status = editor_.setField(screen, id, "pins.bottom", nullptr); !status)
                    return status;
            }
            if (auto status = editor_.setField(screen, id, "pins.top", y); !status)
                return status;
        }
        return {};
    }
    if (auto status = editor_.setField(screen, id, "x", x); !status)
        return status;
    return editor_.setField(screen, id, "y", y);
}

Status DesignOperations::resize(std::string_view screen, std::string_view id, const Rect& frame, const LayoutResult& layout)
{
    const Component* root = rootOf(editor_.model(), screen);
    if (root == nullptr)
        return makeError("ir.missing", "No such screen");
    const Component* parent = findParent(*root, id);
    TransactionScope transaction(editor_, "Resize");
    if (auto status = editor_.setField(screen, id, "width", std::round(frame.width)); !status)
        return status;
    if (auto status = editor_.setField(screen, id, "height", std::round(frame.height)); !status)
        return status;
    if (parent != nullptr && (parent->layout.kind == LayoutKind::Absolute || parent->layout.kind == LayoutKind::Constraint)) {
        const LayoutNode* parentNode = layout.find(parent->id);
        if (parentNode != nullptr) {
            return moveTo(screen, id, std::round(frame.x - parentNode->contentFrame.x), std::round(frame.y - parentNode->contentFrame.y));
        }
    }
    return {};
}

Status DesignOperations::reparentDrop(std::string_view screen, std::string_view id, const DropTarget& target)
{
    const Component* root = rootOf(editor_.model(), screen);
    if (root == nullptr)
        return makeError("ir.missing", "No such screen");
    const Component* parent = findParent(*root, id);
    if (parent == nullptr)
        return makeError("ir.missing", "No such component");
    TransactionScope transaction(editor_, "Move");
    int index = target.index;
    if (parent->id == target.parent && index > indexInParent(*parent, id))
        --index;
    if (parent->id != target.parent || indexInParent(*parent, id) != index) {
        if (auto status = editor_.moveComponent(screen, id, target.parent, index); !status)
            return status;
    }
    if (target.insideAbsolute)
        return moveTo(screen, id, target.x, target.y);
    return {};
}

std::string DesignOperations::copy(std::string_view screen, const std::vector<std::string>& ids) const
{
    Json payload = Json::object();
    payload["formaClipboard"] = 1;
    Json components = Json::array();
    const Component* root = rootOf(editor_.model(), screen);
    if (root != nullptr) {
        for (const auto& id : ids) {
            if (const Component* component = findComponent(*root, id))
                components.push_back(componentToJson(*component));
        }
    }
    payload["components"] = components;
    return dumpJsonCompact(payload);
}

Result<std::vector<std::string>> DesignOperations::paste(std::string_view screen, std::string_view parent, std::string_view payload, double offset)
{
    auto parsed = parseJson(payload);
    if (!parsed)
        return parsed.error();
    if (jsonInt(*parsed, "formaClipboard") != 1)
        return makeError("design.paste", "The clipboard does not hold FormaIDE components");
    std::vector<std::string> created;
    TransactionScope transaction(editor_, "Paste");
    auto items = parsed->find("components");
    if (items == parsed->end() || !items->is_array())
        return created;
    const Component* root = rootOf(editor_.model(), screen);
    if (root == nullptr)
        return makeError("ir.missing", "No such screen");
    const Component* target = findComponent(*root, parent);
    if (target == nullptr)
        return makeError("ir.missing", "No such parent");
    bool absolute = target->layout.kind == LayoutKind::Absolute;
    std::string parentId = target->id;
    for (const auto& item : *items) {
        auto component = componentFromJson(item);
        if (!component)
            return component.error();
        if (absolute) {
            component->x += offset;
            component->y += offset;
        }
        auto inserted = editor_.insertComponent(screen, parentId, -1, std::move(component.value()));
        if (!inserted)
            return inserted.error();
        created.push_back(*inserted);
    }
    return created;
}

Result<std::string> DesignOperations::createDefinition(std::string_view screen, std::string_view id, std::string_view name)
{
    const Component* root = rootOf(editor_.model(), screen);
    if (root == nullptr)
        return makeError("ir.missing", "No such screen");
    const Component* source = findComponent(*root, id);
    const Component* parent = findParent(*root, id);
    if (source == nullptr || parent == nullptr)
        return makeError("design.definition", "Select a component inside the screen");
    std::string definitionId = text::toSnakeCase(name);
    if (editor_.model().findDefinition(definitionId) != nullptr)
        return makeError("design.definition", "A reusable component named " + std::string(name) + " already exists");
    ComponentDefinition definition;
    definition.root = *source;
    definition.root.id = definitionId;
    definition.root.name = std::string(name);
    definition.root.x = 0;
    definition.root.y = 0;
    definition.root.pins = {};
    definition.root.margin = {};
    TransactionScope transaction(editor_, "Create reusable component");
    auto added = editor_.addDefinition(definition);
    if (!added)
        return added.error();
    Component instance = makeComponent(source->id, source->type, source->name);
    instance.instanceOf = definitionId;
    instance.x = source->x;
    instance.y = source->y;
    instance.pins = source->pins;
    instance.margin = source->margin;
    instance.grid = source->grid;
    instance.grow = source->grow;
    instance.width = source->width;
    instance.height = source->height;
    std::string parentId = parent->id;
    int index = indexInParent(*parent, id);
    if (auto status = editor_.removeComponent(screen, id); !status)
        return status.error();
    auto inserted = editor_.insertComponent(screen, parentId, index, std::move(instance));
    if (!inserted)
        return inserted.error();
    return definitionId;
}

Status DesignOperations::detachInstance(std::string_view screen, std::string_view id)
{
    const Component* root = rootOf(editor_.model(), screen);
    if (root == nullptr)
        return makeError("ir.missing", "No such screen");
    const Component* instance = findComponent(*root, id);
    const Component* parent = findParent(*root, id);
    if (instance == nullptr || parent == nullptr || instance->instanceOf.empty())
        return makeError("design.detach", "This component is not an instance");
    layout::LayoutOptions options;
    options.model = &editor_.model();
    Component expanded = layout::resolveComponent(*instance, options);
    expanded.instanceOf.clear();
    expanded.overrides = Json::object();
    std::function<void(Component&)> strip = [&](Component& node) {
        for (auto& child : node.children) {
            auto separator = child.id.find("__");
            if (separator != std::string::npos)
                child.id = child.id.substr(separator + 2);
            strip(child);
        }
    };
    strip(expanded);
    std::string parentId = parent->id;
    int index = indexInParent(*parent, id);
    TransactionScope transaction(editor_, "Detach instance");
    if (auto status = editor_.removeComponent(screen, id); !status)
        return status;
    auto inserted = editor_.insertComponent(screen, parentId, index, std::move(expanded));
    if (!inserted)
        return inserted.error();
    return {};
}

const std::vector<DevicePreset>& devicePresets()
{
    static const std::vector<DevicePreset> presets = {
        {"phone-small", "Small phone", "Phone", 360, 640, 24, 0},
        {"iphone-16", "iPhone 16", "Phone", 393, 852, 59, 34},
        {"iphone-16-pro-max", "iPhone 16 Pro Max", "Phone", 440, 956, 62, 34},
        {"pixel-9", "Pixel 9", "Phone", 412, 924, 48, 24},
        {"galaxy-s25", "Galaxy S25", "Phone", 360, 780, 44, 24},
        {"ipad-11", "iPad 11-inch", "Tablet", 834, 1194, 24, 20},
        {"ipad-13", "iPad 13-inch", "Tablet", 1024, 1366, 24, 20},
        {"android-tablet", "Android tablet", "Tablet", 800, 1280, 24, 24},
        {"desktop-hd", "Desktop 1280 x 720", "Desktop", 1280, 720, 0, 0},
        {"desktop-fhd", "Desktop 1920 x 1080", "Desktop", 1920, 1080, 0, 0},
        {"window-small", "Small window", "Desktop", 800, 600, 0, 0},
        {"web-mobile", "Web mobile", "Web", 375, 667, 0, 0},
        {"web-desktop", "Web desktop", "Web", 1440, 900, 0, 0},
        {"watch-45", "Apple Watch 45 mm", "Watch", 198, 242, 0, 0},
        {"tv-1080", "TV 1080p", "TV", 1920, 1080, 0, 0},
        {"vision-window", "visionOS window", "Vision", 1280, 720, 0, 0},
    };
    return presets;
}

const DevicePreset* findDevicePreset(std::string_view id)
{
    for (const auto& preset : devicePresets()) {
        if (preset.id == id)
            return &preset;
    }
    return nullptr;
}

std::vector<layout::Size> commonDevices()
{
    std::vector<layout::Size> sizes;
    for (const auto& preset : devicePresets())
        sizes.push_back({preset.width, preset.height});
    return sizes;
}

}
