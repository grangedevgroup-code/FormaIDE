#pragma once

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "forma/ir/model.h"
#include "generator_support.h"

namespace forma::generators {

struct BoxStyle {
    std::string background;
    std::string color;
    std::string borderColor;
    double borderWidth = 0;
    double radius = 0;
    double paddingX = 0;
    double paddingY = 0;
    double fontSize = 15;
    int fontWeight = 400;
    bool italic = false;
    bool underline = false;
    std::string fontFamily;
    std::string textAlign;
    double opacity = 1;
};

BoxStyle resolveBox(const ir::ApplicationModel& model, const ir::Component& component);
std::string rootBackground(const ir::ApplicationModel& model, const ir::Component& root);
std::string numberText(double value);
const std::vector<std::string>& coreComponentTypes();
std::vector<std::string> coreLayoutKinds();

struct ResolvedAction {
    std::string kind;
    std::string target;
    Json value;
    std::string handler;
};

std::vector<ResolvedAction> resolveAction(const ir::ApplicationModel& model, const std::string& actionId);
std::set<std::string> collectHandlers(const ir::ApplicationModel& model);
std::set<std::string> screenIds(const ir::ApplicationModel& model);

std::string colorRgbHex(const std::string& color);
int colorAlpha(const std::string& color);

codegen::GeneratedFile plainFile(std::string path, std::string content, codegen::FileCategory category = codegen::FileCategory::Generated);
codegen::GeneratedFile standardReadme(const ir::ApplicationModel& model, const codegen::ProjectConfiguration& config, const std::string& framework, const std::string& howToRun, const std::string& userFiles);

}
