#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "forma/core/database.h"
#include "forma/core/json.h"

namespace forma {

struct ChangeEntry {
    std::int64_t id = 0;
    std::string time;
    std::string kind;
    std::string actor;
    std::string summary;
    Json details = Json::object();
};

class ChangeHistory {
public:
    static Result<std::unique_ptr<ChangeHistory>> open(const std::filesystem::path& databasePath);

    Status record(std::string_view kind, std::string_view actor, std::string_view summary, const Json& details = Json::object());
    Result<std::vector<ChangeEntry>> list(int limit = 200, std::string_view kind = {}) const;

    Database& database() { return *database_; }

private:
    explicit ChangeHistory(std::unique_ptr<Database> database) : database_(std::move(database)) {}
    std::unique_ptr<Database> database_;
};

}
