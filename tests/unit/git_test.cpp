#include <gtest/gtest.h>

#include "forma/git/git_client.h"
#include "test_support.h"

using namespace forma;
using namespace forma::git;
using namespace std::string_literals;
using forma::testing::TempDir;

namespace {

void write(const std::filesystem::path& path, const char* content)
{
    ASSERT_TRUE(paths::writeFileAtomic(path, content).ok());
}

void configure(GitClient& client)
{
    ASSERT_TRUE(client.run({"config", "user.name", "Test"}).ok());
    ASSERT_TRUE(client.run({"config", "user.email", "test@example.com"}).ok());
    ASSERT_TRUE(client.run({"config", "commit.gpgsign", "false"}).ok());
    ASSERT_TRUE(client.run({"config", "core.autocrlf", "false"}).ok());
}

}

TEST(GitStatus, ParsesPorcelainV2)
{
    std::string input = "# branch.oid abc\0# branch.head main\0# branch.upstream origin/main\0# branch.ab +2 -1\0"
                        "1 .M N... 100644 100644 100644 aaa bbb src/my file.cpp\0"
                        "1 A. N... 000000 100644 100644 000 ccc new.txt\0"
                        "2 R. N... 100644 100644 100644 ddd eee R100 renamed.txt\0old.txt\0"
                        "u UU N... 100644 100644 100644 100644 f g h clash.txt\0"
                        "? untracked.txt\0"s;
    auto status = parsePorcelainStatus(input);
    EXPECT_EQ(status.branch, "main");
    EXPECT_EQ(status.upstream, "origin/main");
    EXPECT_EQ(status.ahead, 2);
    EXPECT_EQ(status.behind, 1);
    ASSERT_EQ(status.files.size(), 5u);
    EXPECT_EQ(status.files[0].path, "src/my file.cpp");
    EXPECT_EQ(status.files[0].unstaged, 'M');
    EXPECT_EQ(status.files[1].staged, 'A');
    EXPECT_EQ(status.files[2].originalPath, "old.txt");
    EXPECT_TRUE(status.files[3].conflicted);
    EXPECT_TRUE(status.files[4].untracked);
}

TEST(GitClient, FullWorkflowOnARealRepository)
{
    if (!GitClient::available())
        GTEST_SKIP() << "git is not installed";
    TempDir dir;
    GitClient client(dir.path());
    auto notRepository = client.status();
    ASSERT_TRUE(notRepository.ok());
    EXPECT_FALSE(notRepository->isRepository);

    ASSERT_TRUE(client.init()->ok());
    configure(client);
    write(dir.path() / "a.txt", "one\n");
    auto status = client.status();
    ASSERT_TRUE(status.ok());
    ASSERT_EQ(status->files.size(), 1u);
    EXPECT_TRUE(status->files[0].untracked);

    ASSERT_TRUE(client.stage({"a.txt"})->ok());
    ASSERT_TRUE(client.commit("First commit")->ok());
    auto log = client.log();
    ASSERT_TRUE(log.ok());
    ASSERT_EQ(log->size(), 1u);
    EXPECT_EQ((*log)[0].subject, "First commit");
    EXPECT_EQ(*client.showFile("HEAD", "a.txt"), "one\n");

    write(dir.path() / "a.txt", "two\n");
    auto diff = client.diff("a.txt", false);
    ASSERT_TRUE(diff.ok());
    EXPECT_NE(diff->find("+two"), std::string::npos);
    ASSERT_TRUE(client.discard({"a.txt"})->ok());
    EXPECT_EQ(*paths::readWholeFile(dir.path() / "a.txt"), "one\n");

    ASSERT_TRUE(client.createBranch("feature")->ok());
    write(dir.path() / "a.txt", "feature\n");
    ASSERT_TRUE(client.stage({"a.txt"})->ok());
    ASSERT_TRUE(client.commit("On feature")->ok());
    ASSERT_TRUE(client.checkout("main")->ok());
    auto branches = client.branches();
    ASSERT_TRUE(branches.ok());
    EXPECT_EQ(branches->size(), 2u);

    write(dir.path() / "a.txt", "main side\n");
    ASSERT_TRUE(client.stage({"a.txt"})->ok());
    ASSERT_TRUE(client.commit("On main")->ok());
    auto merge = client.merge("feature");
    ASSERT_TRUE(merge.ok());
    EXPECT_FALSE(merge->ok());
    status = client.status();
    ASSERT_EQ(status->files.size(), 1u);
    EXPECT_TRUE(status->files[0].conflicted);
    ASSERT_TRUE(client.resolveWithTheirs("a.txt")->ok());
    ASSERT_TRUE(client.commit("Merge feature")->ok());
    EXPECT_EQ(*paths::readWholeFile(dir.path() / "a.txt"), "feature\n");

    ASSERT_TRUE(client.addToIgnore("build/").ok());
    ASSERT_TRUE(client.addToIgnore("build/").ok());
    EXPECT_EQ(*paths::readWholeFile(dir.path() / ".gitignore"), "build/\n");
}

TEST(GitClient, RefusesOptionLikeBranchNames)
{
    TempDir dir;
    GitClient client(dir.path());
    EXPECT_FALSE(client.createBranch("--force").ok());
    EXPECT_FALSE(client.checkout("-b").ok());
    EXPECT_FALSE(GitClient::clone("--upload-pack=evil", dir.path() / "x").ok());
}
