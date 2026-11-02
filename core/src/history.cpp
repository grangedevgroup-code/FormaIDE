#include "forma/core/history.h"

#include "forma/core/paths.h"

namespace forma {

Result<std::unique_ptr<ChangeHistory>> ChangeHistory::open(const std::filesystem::path& databasePath)
{
    std::vector<DatabaseMigration> migrations = {
        {1, "CREATE TABLE changes (id INTEGER PRIMARY KEY AUTOINCREMENT, time TEXT NOT NULL, kind TEXT NOT NULL, "
            "actor TEXT NOT NULL, summary TEXT NOT NULL, details TEXT NOT NULL);"
            "CREATE INDEX changes_kind ON changes(kind, id);"},
        {2, "CREATE TABLE file_index (path TEXT PRIMARY KEY, size INTEGER, modified INTEGER, kind TEXT);"
            "CREATE TABLE builds (id INTEGER PRIMARY KEY AUTOINCREMENT, started TEXT, finished TEXT, target TEXT, "
            "configuration TEXT, exit_code INTEGER, errors INTEGER, warnings INTEGER, artifacts TEXT);"
            "CREATE TABLE conversations (id INTEGER PRIMARY KEY AUTOINCREMENT, started TEXT, provider TEXT, model TEXT, "
            "title TEXT, messages INTEGER);"},
    };
    auto database = Database::open(databasePath, migrations);
    if (!database)
        return database.error();
    return std::unique_ptr<ChangeHistory>(new ChangeHistory(std::move(database.value())));
}

Status ChangeHistory::record(std::string_view kind, std::string_view actor, std::string_view summary, const Json& details)
{
    auto statement = database_->prepare("INSERT INTO changes (time, kind, actor, summary, details) VALUES (?,?,?,?,?)");
    if (!statement)
        return statement.error();
    std::string time = paths::nowIso8601();
    std::string payload = dumpJsonCompact(details);
    statement->bind(1, time).bind(2, kind).bind(3, actor).bind(4, summary).bind(5, payload);
    return statement->run();
}

Result<std::vector<ChangeEntry>> ChangeHistory::list(int limit, std::string_view kind) const
{
    std::string sql = "SELECT id, time, kind, actor, summary, details FROM changes";
    if (!kind.empty())
        sql += " WHERE kind = ?";
    sql += " ORDER BY id DESC LIMIT ?";
    auto statement = database_->prepare(sql);
    if (!statement)
        return statement.error();
    int index = 1;
    if (!kind.empty())
        statement->bind(index++, kind);
    statement->bind(index, static_cast<std::int64_t>(limit));
    std::vector<ChangeEntry> entries;
    while (true) {
        auto row = statement->step();
        if (!row)
            return row.error();
        if (!*row)
            break;
        ChangeEntry entry;
        entry.id = statement->columnInt(0);
        entry.time = statement->columnText(1);
        entry.kind = statement->columnText(2);
        entry.actor = statement->columnText(3);
        entry.summary = statement->columnText(4);
        auto parsed = parseJson(statement->columnText(5));
        if (parsed)
            entry.details = std::move(parsed.value());
        entries.push_back(std::move(entry));
    }
    return entries;
}

}
