#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
#include <mutex>

#include "forma/core/file_watcher.h"
#include "forma/core/filesystem_service.h"
#include "forma/core/recovery.h"
#include "test_support.h"

using namespace forma;
using forma::testing::TempDir;
using forma::testing::waitUntil;

TEST(PathValidation, RejectsEscapes)
{
    EXPECT_FALSE(validateRelativePath("../outside.txt").ok());
    EXPECT_FALSE(validateRelativePath("src/../../outside.txt").ok());
    EXPECT_FALSE(validateRelativePath("/etc/passwd").ok());
    EXPECT_FALSE(validateRelativePath("C:/Windows/system.ini").ok());
    EXPECT_FALSE(validateRelativePath("src\\..\\..\\x").ok());
    EXPECT_FALSE(validateRelativePath("~/x").ok());
    EXPECT_FALSE(validateRelativePath("").ok());
    EXPECT_FALSE(validateRelativePath("a/CON.txt").ok());
    EXPECT_FALSE(validateRelativePath("a/b:stream").ok());
    EXPECT_FALSE(validateRelativePath("a/name.").ok());
    EXPECT_TRUE(validateRelativePath("src/main.cpp").ok());
    EXPECT_TRUE(validateRelativePath("assets/images/logo.png").ok());
    EXPECT_TRUE(validateRelativePath(".forma/manifest.json").ok());
}

TEST(FilesystemService, WritesReadsAndBacksUp)
{
    TempDir dir;
    auto service = createLocalFilesystemService(dir.path());
    ASSERT_TRUE(service->writeFile("src/a.txt", "one").ok());
    auto content = service->readFile("src/a.txt");
    ASSERT_TRUE(content.ok());
    EXPECT_EQ(*content, "one");

    std::vector<FileOperation> operations;
    service->setOperationListener([&](const FileOperation& op) { operations.push_back(op); });
    ASSERT_TRUE(service->writeFile("src/a.txt", "two").ok());
    ASSERT_EQ(operations.size(), 1u);
    EXPECT_EQ(operations[0].action, "modify");
    EXPECT_FALSE(operations[0].recoveryId.empty());
    auto backup = paths::readWholeFile(dir.path() / ".forma" / "backups" / operations[0].recoveryId / "src" / "a.txt");
    ASSERT_TRUE(backup.ok());
    EXPECT_EQ(*backup, "one");
}

TEST(FilesystemService, ExpectedHashDetectsExternalEdits)
{
    TempDir dir;
    auto service = createLocalFilesystemService(dir.path());
    ASSERT_TRUE(service->writeFile("a.txt", "one").ok());
    auto hash = service->fileHash("a.txt");
    ASSERT_TRUE(hash.ok());
    {
        std::ofstream stream(dir.path() / "a.txt", std::ios::binary);
        stream << "changed outside";
    }
    WriteOptions options;
    options.expectedHash = *hash;
    auto status = service->writeFile("a.txt", "two", options);
    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error().code, "fs.conflict");
    EXPECT_EQ(*service->readFile("a.txt"), "changed outside");
}

TEST(FilesystemService, RemoveGoesToTrashAndRestores)
{
    TempDir dir;
    auto service = createLocalFilesystemService(dir.path());
    ASSERT_TRUE(service->writeFile("dir/file.txt", "keep me").ok());
    auto id = service->remove("dir/file.txt");
    ASSERT_TRUE(id.ok());
    EXPECT_FALSE(service->exists("dir/file.txt"));
    ASSERT_TRUE(service->restoreFromTrash(*id).ok());
    EXPECT_EQ(*service->readFile("dir/file.txt"), "keep me");
}

TEST(FilesystemService, RefusesPathsOutsideTheRoot)
{
    TempDir dir;
    auto service = createLocalFilesystemService(dir.path());
    EXPECT_FALSE(service->writeFile("../evil.txt", "x").ok());
    EXPECT_FALSE(service->readFile("../../etc/passwd").ok());
    EXPECT_FALSE(service->remove("..").ok());
    EXPECT_FALSE(service->rename("a.txt", "../b.txt").ok());
}

TEST(FilesystemService, RefusesLinksThatLeaveTheRoot)
{
    TempDir outside;
    TempDir dir;
    std::error_code ec;
    std::filesystem::create_directory_symlink(outside.path(), dir.path() / "link", ec);
    if (ec)
        GTEST_SKIP() << "This machine cannot create symbolic links";
    auto service = createLocalFilesystemService(dir.path());
    auto status = service->writeFile("link/evil.txt", "x");
    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error().code, "path.escape");
    EXPECT_FALSE(std::filesystem::exists(outside.path() / "evil.txt"));
}

TEST(FilesystemService, ListSkipsIgnoredNames)
{
    TempDir dir;
    auto service = createLocalFilesystemService(dir.path());
    ASSERT_TRUE(service->writeFile("src/a.cpp", "a").ok());
    ASSERT_TRUE(service->writeFile("build/x.o", "x").ok());
    ASSERT_TRUE(service->writeFile("README.md", "r").ok());
    ListOptions options;
    options.recursive = true;
    options.ignoredNames = defaultIgnoredNames();
    auto entries = service->list("", options);
    ASSERT_TRUE(entries.ok());
    std::vector<std::string> paths;
    for (const auto& entry : *entries)
        paths.push_back(entry.path);
    EXPECT_NE(std::find(paths.begin(), paths.end(), "src/a.cpp"), paths.end());
    EXPECT_NE(std::find(paths.begin(), paths.end(), "README.md"), paths.end());
    EXPECT_EQ(std::find(paths.begin(), paths.end(), "build/x.o"), paths.end());
}

TEST(FileWatcher, ReportsCreateModifyRemove)
{
    TempDir dir;
    auto watcher = createFileWatcher();
    std::mutex mutex;
    std::vector<FileChange> seen;
    ASSERT_TRUE(watcher->start(dir.path(), [&](const std::vector<FileChange>& changes) {
        std::lock_guard lock(mutex);
        seen.insert(seen.end(), changes.begin(), changes.end());
    }).ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    {
        std::ofstream stream(dir.path() / "watched.txt");
        stream << "hello";
    }
    auto sawChange = [&](const char* name) {
        std::lock_guard lock(mutex);
        return std::any_of(seen.begin(), seen.end(), [&](const FileChange& change) { return change.path == name; });
    };
    EXPECT_TRUE(waitUntil([&] { return sawChange("watched.txt"); }));

    {
        std::lock_guard lock(mutex);
        seen.clear();
    }
    std::filesystem::remove(dir.path() / "watched.txt");
    EXPECT_TRUE(waitUntil([&] {
        std::lock_guard lock(mutex);
        return std::any_of(seen.begin(), seen.end(), [](const FileChange& change) {
            return change.path == "watched.txt" && change.kind == FileChangeKind::Removed;
        });
    }));
    watcher->stop();
}

TEST(FileWatcher, PollingBackendReportsChanges)
{
    TempDir dir;
    auto watcher = createPollingFileWatcher(std::chrono::milliseconds(50));
    std::atomic<int> count{0};
    ASSERT_TRUE(watcher->start(dir.path(), [&](const std::vector<FileChange>& changes) { count += static_cast<int>(changes.size()); }).ok());
    {
        std::ofstream stream(dir.path() / "p.txt");
        stream << "hello";
    }
    EXPECT_TRUE(waitUntil([&] { return count.load() > 0; }));
    watcher->stop();
}

TEST(Snapshots, CreateRestoreAndDeduplicate)
{
    TempDir dir;
    auto service = createLocalFilesystemService(dir.path());
    ASSERT_TRUE(service->writeFile("src/a.txt", "one").ok());
    ASSERT_TRUE(service->writeFile("design/screen.json", "{}").ok());
    SnapshotStore store(*service);
    auto first = store.create("first", "user");
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first->fileCount, 2u);

    ASSERT_TRUE(service->writeFile("src/a.txt", "two").ok());
    ASSERT_TRUE(service->writeFile("src/new.txt", "added later").ok());

    std::vector<std::string> removed;
    auto restored = store.restore(first->id, &removed);
    ASSERT_TRUE(restored.ok());
    EXPECT_EQ(*service->readFile("src/a.txt"), "one");
    EXPECT_FALSE(service->exists("src/new.txt"));
    ASSERT_EQ(removed.size(), 1u);
    EXPECT_EQ(removed[0], "src/new.txt");

    auto all = store.list();
    ASSERT_TRUE(all.ok());
    EXPECT_EQ(all->size(), 2u);
}

TEST(Recovery, DetectsCrashedSessionAndKeepsBuffers)
{
    TempDir dir;
    {
        RecoveryStore store(dir.path());
        EXPECT_FALSE(store.beginSession());
        ASSERT_TRUE(store.saveBuffer("src/a.txt", "unsaved text", "abc").ok());
        ASSERT_TRUE(store.saveDesignState(Json{{"screens", 1}}).ok());
    }
    {
        auto sessionFile = dir.path() / ".forma" / "recovery" / "session.json";
        ASSERT_TRUE(paths::writeFileAtomic(sessionFile, "{\"pid\": 999999}").ok());
        RecoveryStore store(dir.path());
        EXPECT_TRUE(store.beginSession());
        auto buffers = store.buffers();
        ASSERT_TRUE(buffers.ok());
        ASSERT_EQ(buffers->size(), 1u);
        EXPECT_EQ((*buffers)[0].content, "unsaved text");
        auto design = store.designState();
        ASSERT_TRUE(design.has_value());
        EXPECT_EQ((*design)["screens"], 1);
        store.endSession();
    }
    RecoveryStore store(dir.path());
    EXPECT_FALSE(store.beginSession());
}
