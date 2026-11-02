#include <gtest/gtest.h>

#include <atomic>
#include <mutex>

#include "forma/core/credentials.h"
#include "forma/core/database.h"
#include "forma/core/history.h"
#include "forma/core/process.h"
#include "forma/core/settings.h"
#include "test_support.h"

using namespace forma;
using forma::testing::TempDir;

namespace {

ProcessSpec shell(const std::string& script)
{
    ProcessSpec spec;
#if defined(_WIN32)
    spec.program = "cmd.exe";
    spec.arguments = {"/d", "/c", script};
#else
    spec.program = "/bin/sh";
    spec.arguments = {"-c", script};
#endif
    return spec;
}

}

TEST(Process, CapturesOutputAndExitCode)
{
    auto result = runProcess(shell("echo hello&& exit 3"));
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result->exitCode, 3);
    EXPECT_NE(result->out.find("hello"), std::string::npos);
}

TEST(Process, ReportsMissingProgram)
{
    ProcessSpec spec;
    spec.program = "forma-program-that-does-not-exist";
    auto result = runProcess(spec);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, "process.notfound");
}

TEST(Process, PassesEnvironmentAndWorkingDirectory)
{
    TempDir dir;
#if defined(_WIN32)
    auto spec = shell("echo %FORMA_TEST_VALUE%&& cd");
#else
    auto spec = shell("echo $FORMA_TEST_VALUE; pwd");
#endif
    spec.environment["FORMA_TEST_VALUE"] = "from-test";
    spec.workingDirectory = dir.path();
    auto result = runProcess(spec);
    ASSERT_TRUE(result.ok());
    EXPECT_NE(result->out.find("from-test"), std::string::npos);
    EXPECT_NE(result->out.find(dir.path().filename().string()), std::string::npos);
}

TEST(Process, ArgumentsWithSpacesAndQuotesSurvive)
{
    TempDir dir;
#if defined(_WIN32)
    std::string program = "findstr";
    std::string flag = "/n";
    std::string pattern = "x";
#else
    std::string program = "grep";
    std::string flag = "-n";
    std::string pattern = "x";
#endif
    auto file = dir.path() / "name with spaces.txt";
    ASSERT_TRUE(paths::writeFileAtomic(file, "x marks\n").ok());
    ProcessSpec spec;
    spec.program = program;
    spec.arguments = {flag, pattern, paths::toUtf8(file)};
    auto result = runProcess(spec);
    ASSERT_TRUE(result.ok());
    EXPECT_NE(result->out.find("x marks"), std::string::npos);
}

TEST(Process, TerminateStopsTheWholeTree)
{
#if defined(_WIN32)
    auto spec = shell("ping -n 60 127.0.0.1 >nul");
#else
    auto spec = shell("sleep 60");
#endif
    auto started = Process::start(spec, {});
    ASSERT_TRUE(started.ok());
    EXPECT_TRUE((*started)->running());
    (*started)->terminate();
    EXPECT_TRUE((*started)->waitFor(std::chrono::seconds(10)));
    EXPECT_FALSE((*started)->running());
}

TEST(Process, TimeoutKillsAndReports)
{
#if defined(_WIN32)
    auto spec = shell("ping -n 60 127.0.0.1 >nul");
#else
    auto spec = shell("sleep 60");
#endif
    auto result = runProcess(spec, std::chrono::milliseconds(300));
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result->timedOut);
}

TEST(Process, StdinReachesTheChild)
{
#if defined(_WIN32)
    auto spec = shell("findstr hello");
#else
    auto spec = shell("cat");
#endif
    spec.keepStdinOpen = true;
    std::mutex mutex;
    std::string output;
    auto started = Process::start(spec, [&](std::string_view data) {
        std::lock_guard lock(mutex);
        output.append(data);
    });
    ASSERT_TRUE(started.ok());
    ASSERT_TRUE((*started)->writeStdin("hello there\n").ok());
    (*started)->closeStdin();
    (*started)->wait();
    EXPECT_NE(output.find("hello there"), std::string::npos);
}

#if defined(_WIN32)
TEST(Process, BatchArgumentsCannotInjectCommands)
{
    TempDir dir;
    auto script = dir.path() / "echoargs.cmd";
    ASSERT_TRUE(paths::writeFileAtomic(script, "@echo off\r\necho ARG:%1\r\n").ok());
    auto marker = dir.path() / "injected.txt";
    ProcessSpec spec;
    spec.program = paths::toUtf8(script);
    spec.arguments = {"safe\"&echo pwned>\"" + paths::toUtf8(marker)};
    auto result = runProcess(spec);
    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(std::filesystem::exists(marker));

    ProcessSpec newline;
    newline.program = paths::toUtf8(script);
    newline.arguments = {"line\r\necho pwned"};
    EXPECT_FALSE(runProcess(newline).ok());
}

TEST(Process, BatchArgumentsKeepTheirDelimiters)
{
    std::string line = buildWindowsBatchCommand("tool.bat", {"system-images;android-36;x86_64", "--root=C:\\Sdk\\", "plain"});
    EXPECT_NE(line.find("^\"system-images^;android-36^;x86_64^\""), std::string::npos) << line;
    EXPECT_NE(line.find("^\"--root^=C:\\Sdk\\\\^\""), std::string::npos) << line;
    EXPECT_NE(line.find(" plain"), std::string::npos) << line;
}

TEST(Process, QuotesFollowWindowsRules)
{
    EXPECT_EQ(quoteWindowsArgument("plain"), "plain");
    EXPECT_EQ(quoteWindowsArgument("two words"), "\"two words\"");
    EXPECT_EQ(quoteWindowsArgument("say \"hi\""), "\"say \\\"hi\\\"\"");
    EXPECT_EQ(quoteWindowsArgument("trailing\\ slash\\"), "\"trailing\\ slash\\\\\"");
    EXPECT_EQ(quoteWindowsArgument(""), "\"\"");
}
#endif

TEST(Database, MigratesAndQueries)
{
    TempDir dir;
    auto path = dir.path() / "test.db";
    std::vector<DatabaseMigration> migrations = {
        {1, "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)"},
        {2, "ALTER TABLE items ADD COLUMN size INTEGER DEFAULT 7"},
    };
    {
        auto database = Database::open(path, migrations);
        ASSERT_TRUE(database.ok());
        EXPECT_EQ((*database)->userVersion(), 2);
        auto insert = (*database)->prepare("INSERT INTO items (name) VALUES (?)");
        ASSERT_TRUE(insert.ok());
        insert->bind(1, std::string_view("first"));
        ASSERT_TRUE(insert->run().ok());
        auto integrity = (*database)->integrityCheck();
        ASSERT_TRUE(integrity.ok());
        EXPECT_TRUE(*integrity);
    }
    auto reopened = Database::open(path, migrations);
    ASSERT_TRUE(reopened.ok());
    auto select = (*reopened)->prepare("SELECT name, size FROM items");
    ASSERT_TRUE(select.ok());
    auto row = select->step();
    ASSERT_TRUE(row.ok() && *row);
    EXPECT_EQ(select->columnText(0), "first");
    EXPECT_EQ(select->columnInt(1), 7);
}

TEST(Database, MovesDamagedFileAsideAndRecovers)
{
    TempDir dir;
    auto path = dir.path() / "bad.db";
    ASSERT_TRUE(paths::writeFileAtomic(path, std::string(4096, 'x')).ok());
    auto database = Database::open(path, {{1, "CREATE TABLE t (id INTEGER)"}});
    ASSERT_TRUE(database.ok()) << database.error().message;
    EXPECT_TRUE((*database)->recoveredFromCorruption());
    EXPECT_EQ((*database)->userVersion(), 1);
}

TEST(ChangeHistory, RecordsAndFilters)
{
    TempDir dir;
    auto history = ChangeHistory::open(dir.path() / "history.db");
    ASSERT_TRUE(history.ok());
    ASSERT_TRUE((*history)->record("design", "user", "Moved title", Json{{"id", "title"}}).ok());
    ASSERT_TRUE((*history)->record("ai", "ai", "Added settings screen").ok());
    auto all = (*history)->list();
    ASSERT_TRUE(all.ok());
    ASSERT_EQ(all->size(), 2u);
    EXPECT_EQ((*all)[0].kind, "ai");
    auto design = (*history)->list(10, "design");
    ASSERT_TRUE(design.ok());
    ASSERT_EQ(design->size(), 1u);
    EXPECT_EQ((*design)[0].details["id"], "title");
}

TEST(Settings, SavesAndSurvivesDamage)
{
    TempDir dir;
    auto file = dir.path() / "settings.json";
    {
        Settings settings(file);
        settings.set("editor.fontSize", 13);
        settings.set("ui.theme", "dark");
        ASSERT_TRUE(settings.save().ok());
    }
    {
        Settings settings(file);
        ASSERT_TRUE(settings.load().ok());
        EXPECT_EQ(settings.getInt("editor.fontSize"), 13);
        EXPECT_EQ(settings.getString("ui.theme"), "dark");
    }
    ASSERT_TRUE(paths::writeFileAtomic(file, "{ broken").ok());
    Settings settings(file);
    ASSERT_TRUE(settings.load().ok());
    EXPECT_EQ(settings.getInt("editor.fontSize", 9), 9);
}

TEST(Credentials, MemoryStoreRoundTrip)
{
    auto store = createMemoryCredentialStore();
    ASSERT_TRUE(store->set("provider.key", "secret").ok());
    auto value = store->get("provider.key");
    ASSERT_TRUE(value.ok());
    ASSERT_TRUE(value->has_value());
    EXPECT_EQ(**value, "secret");
    ASSERT_TRUE(store->remove("provider.key").ok());
    auto gone = store->get("provider.key");
    ASSERT_TRUE(gone.ok());
    EXPECT_FALSE(gone->has_value());
    EXPECT_FALSE(store->persistent());
}

TEST(Credentials, PlatformStoreRoundTrip)
{
    auto store = createPlatformCredentialStore();
    std::string account = "forma-unit-test-" + paths::timestampId();
    auto status = store->set(account, "secret value");
    if (!status.ok())
        GTEST_SKIP() << "No usable credential store here: " << status.error().message;
    auto value = store->get(account);
    ASSERT_TRUE(value.ok());
    ASSERT_TRUE(value->has_value());
    EXPECT_EQ(**value, "secret value");
    ASSERT_TRUE(store->remove(account).ok());
    auto gone = store->get(account);
    ASSERT_TRUE(gone.ok());
    EXPECT_FALSE(gone->has_value());
}
