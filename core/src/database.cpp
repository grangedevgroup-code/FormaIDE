#include "forma/core/database.h"

#include <sqlite3.h>

#include <algorithm>

#include "forma/core/log.h"
#include "forma/core/paths.h"

namespace fs = std::filesystem;

namespace forma {

Statement::Statement(sqlite3_stmt* handle) : handle_(handle) {}

Statement::~Statement()
{
    if (handle_ != nullptr)
        sqlite3_finalize(handle_);
}

Statement::Statement(Statement&& other) noexcept : handle_(other.handle_)
{
    other.handle_ = nullptr;
}

Statement& Statement::operator=(Statement&& other) noexcept
{
    if (this != &other) {
        if (handle_ != nullptr)
            sqlite3_finalize(handle_);
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

Statement& Statement::bind(int index, std::string_view value)
{
    sqlite3_bind_text(handle_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
    return *this;
}

Statement& Statement::bind(int index, std::int64_t value)
{
    sqlite3_bind_int64(handle_, index, value);
    return *this;
}

Statement& Statement::bind(int index, double value)
{
    sqlite3_bind_double(handle_, index, value);
    return *this;
}

Statement& Statement::bindNull(int index)
{
    sqlite3_bind_null(handle_, index);
    return *this;
}

Result<bool> Statement::step()
{
    int code = sqlite3_step(handle_);
    if (code == SQLITE_ROW)
        return true;
    if (code == SQLITE_DONE)
        return false;
    return makeError("db.step", sqlite3_errmsg(sqlite3_db_handle(handle_)));
}

Status Statement::run()
{
    auto stepped = step();
    if (!stepped)
        return stepped.error();
    reset();
    return {};
}

void Statement::reset()
{
    sqlite3_reset(handle_);
    sqlite3_clear_bindings(handle_);
}

std::string Statement::columnText(int index) const
{
    const auto* text = sqlite3_column_text(handle_, index);
    if (text == nullptr)
        return {};
    return std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(sqlite3_column_bytes(handle_, index)));
}

std::int64_t Statement::columnInt(int index) const
{
    return sqlite3_column_int64(handle_, index);
}

double Statement::columnDouble(int index) const
{
    return sqlite3_column_double(handle_, index);
}

bool Statement::columnIsNull(int index) const
{
    return sqlite3_column_type(handle_, index) == SQLITE_NULL;
}

Result<std::unique_ptr<Database>> Database::open(const fs::path& path, const std::vector<DatabaseMigration>& migrations)
{
    std::error_code ec;
    if (path.has_parent_path())
        fs::create_directories(path.parent_path(), ec);

    auto attempt = [&](bool& corrupt) -> Result<std::unique_ptr<Database>> {
        std::unique_ptr<Database> database(new Database());
        database->path_ = path;
        int code = sqlite3_open_v2(paths::toUtf8(path).c_str(), &database->handle_,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
        if (code != SQLITE_OK)
            return makeError("db.open", "Cannot open " + paths::toUtf8(path));
        sqlite3_busy_timeout(database->handle_, 3000);
        auto quick = database->prepare("PRAGMA quick_check");
        if (quick) {
            auto row = quick->step();
            if (!row || !*row || quick->columnText(0) != "ok")
                corrupt = true;
        } else if (quick.error().message.find("not a database") != std::string::npos
                   || quick.error().message.find("malformed") != std::string::npos) {
            corrupt = true;
        }
        if (corrupt)
            return makeError("db.corrupt", "The database is damaged: " + paths::toUtf8(path));
        if (auto status = database->exec("PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON;"); !status)
            return status.error();
        if (auto status = database->migrate(migrations); !status)
            return status.error();
        return database;
    };

    bool corrupt = false;
    auto opened = attempt(corrupt);
    if (opened || !corrupt)
        return opened;

    fs::path aside = path;
    aside += ".damaged-" + paths::timestampId();
    fs::rename(path, aside, ec);
    for (const char* suffix : {"-wal", "-shm"}) {
        fs::path extra = path;
        extra += suffix;
        fs::remove(extra, ec);
    }
    log::warning("database", "moved a damaged database aside", Json{{"path", paths::toUtf8(aside)}});
    corrupt = false;
    auto reopened = attempt(corrupt);
    if (reopened)
        reopened.value()->recovered_ = true;
    return reopened;
}

Database::~Database()
{
    if (handle_ != nullptr)
        sqlite3_close_v2(handle_);
}

Status Database::exec(std::string_view sql)
{
    char* message = nullptr;
    std::string text(sql);
    int code = sqlite3_exec(handle_, text.c_str(), nullptr, nullptr, &message);
    if (code != SQLITE_OK) {
        std::string detail = message != nullptr ? message : "unknown error";
        sqlite3_free(message);
        return makeError("db.exec", detail);
    }
    return {};
}

Result<Statement> Database::prepare(std::string_view sql)
{
    sqlite3_stmt* handle = nullptr;
    int code = sqlite3_prepare_v2(handle_, sql.data(), static_cast<int>(sql.size()), &handle, nullptr);
    if (code != SQLITE_OK)
        return makeError("db.prepare", sqlite3_errmsg(handle_));
    return Statement(handle);
}

Result<bool> Database::integrityCheck()
{
    auto statement = prepare("PRAGMA integrity_check");
    if (!statement)
        return statement.error();
    auto row = statement->step();
    if (!row)
        return row.error();
    return *row && statement->columnText(0) == "ok";
}

std::int64_t Database::lastInsertId() const
{
    return sqlite3_last_insert_rowid(handle_);
}

int Database::userVersion()
{
    auto statement = prepare("PRAGMA user_version");
    if (!statement)
        return 0;
    auto row = statement->step();
    if (!row || !*row)
        return 0;
    return static_cast<int>(statement->columnInt(0));
}

Status Database::migrate(const std::vector<DatabaseMigration>& migrations)
{
    auto ordered = migrations;
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.version < b.version; });
    int current = userVersion();
    for (const auto& migration : ordered) {
        if (migration.version <= current)
            continue;
        std::string script = "BEGIN;" + migration.sql + ";PRAGMA user_version=" + std::to_string(migration.version) + ";COMMIT;";
        if (auto status = exec(script); !status) {
            auto ignored = exec("ROLLBACK;");
            (void)ignored;
            return status;
        }
        current = migration.version;
    }
    return {};
}

}
