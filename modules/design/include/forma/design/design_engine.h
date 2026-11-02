#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "forma/core/json.h"
#include "forma/ir/editor.h"
#include "forma/layout/layout_engine.h"

namespace forma::design {

enum class AlignEdge { Left, CenterX, Right, Top, CenterY, Bottom };
enum class DistributeAxis { Horizontal, Vertical };
enum class ZOrder { Forward, Backward, Front, Back };

struct SnapGuide {
    bool vertical = true;
    double position = 0;
    double start = 0;
    double end = 0;
};

struct SnapResult {
    double dx = 0;
    double dy = 0;
    std::vector<SnapGuide> guides;
};

struct SnapOptions {
    double threshold = 6;
    bool toGrid = false;
    double gridSize = 8;
    bool toObjects = true;
    std::vector<double> verticalGuides;
    std::vector<double> horizontalGuides;
};

SnapResult snapMove(const layout::Rect& moving, const layout::LayoutResult& layout, const std::vector<std::string>& excluded, const SnapOptions& options);

struct ManualGuide {
    bool vertical = true;
    double position = 0;
};

struct DropTarget {
    std::string parent;
    int index = -1;
    double x = 0;
    double y = 0;
    bool insideAbsolute = false;
    layout::Rect indicator;
    bool indicatorVertical = false;
};

DropTarget findDropTarget(const layout::LayoutResult& layout, const ir::ApplicationModel& model, double x, double y, const std::vector<std::string>& excluded);

class DesignOperations {
public:
    explicit DesignOperations(ir::ModelEditor& editor) : editor_(editor) {}

    Status align(std::string_view screen, const layout::LayoutResult& layout, const std::vector<std::string>& ids, AlignEdge edge);
    Status distribute(std::string_view screen, const layout::LayoutResult& layout, const std::vector<std::string>& ids, DistributeAxis axis);
    Result<std::string> group(std::string_view screen, const layout::LayoutResult& layout, const std::vector<std::string>& ids);
    Status ungroup(std::string_view screen, const layout::LayoutResult& layout, std::string_view id);
    Status reorder(std::string_view screen, std::string_view id, ZOrder order);
    Result<std::vector<std::string>> duplicate(std::string_view screen, const std::vector<std::string>& ids, double offset = 16);
    Status remove(std::string_view screen, const std::vector<std::string>& ids);
    Status nudge(std::string_view screen, const layout::LayoutResult& layout, const std::vector<std::string>& ids, double dx, double dy);
    Status moveTo(std::string_view screen, std::string_view id, double x, double y);
    Status resize(std::string_view screen, std::string_view id, const layout::Rect& frame, const layout::LayoutResult& layout);
    Status reparentDrop(std::string_view screen, std::string_view id, const DropTarget& target);

    std::string copy(std::string_view screen, const std::vector<std::string>& ids) const;
    Result<std::vector<std::string>> paste(std::string_view screen, std::string_view parent, std::string_view payload, double offset = 16);

    Result<std::string> createDefinition(std::string_view screen, std::string_view id, std::string_view name);
    Status detachInstance(std::string_view screen, std::string_view id);

private:
    ir::ModelEditor& editor_;
};

std::vector<layout::Size> commonDevices();

struct DevicePreset {
    std::string id;
    std::string name;
    std::string category;
    double width = 0;
    double height = 0;
    double safeTop = 0;
    double safeBottom = 0;
};

const std::vector<DevicePreset>& devicePresets();
const DevicePreset* findDevicePreset(std::string_view id);

}
