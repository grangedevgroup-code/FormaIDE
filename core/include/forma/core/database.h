#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "forma/core/result.h"

struct sqlite3;
struct sqlite3_stmt;

namespace forma {

class Statement {
public:
    Statement() = default;
    explicit Statement(sqlite3_stmt* handle);
    ~Statement();
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    Statement& bind(int index, std::string_view value);
    Statement& bind(int index, std::int64_t value);
    Statement& bind(int index, double value);
    Statement& bindNull(int index);

    Result<bool> step();
    Status run();
    void reset();

    std::string columnText(int index) const;
    std::int64_t columnInt(int index) const;
    double columnDouble(int index) const;
    bool columnIsNull(int index) const;

private:
    sqlite3_stmt* handle_ = nullptr;
};

struct DatabaseMigration {
    int version = 0;
    std::string sql;
};

class Database {
public:
    static Result<std::unique_ptr<Database>> open(const std::filesystem::path& path,
                                                  const std::vector<DatabaseMigration>& migrations);
    ~Database();

    Status exec(std::string_view sql);
    Result<Statement> prepare(std::string_view sql);
    Result<bool> integrityCheck();
    std::int64_t lastInsertId() const;
    int userVersion();
    bool recoveredFromCorruption() const { return recovered_; }
    const std::filesystem::path& path() const { return path_; }

private:
    Database() = default;
    Status migrate(const std::vector<DatabaseMigration>& migrations);

    sqlite3* handle_ = nullptr;
    std::filesystem::path path_;
    bool recovered_ = false;
};

}
