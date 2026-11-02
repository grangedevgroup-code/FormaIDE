#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "forma/core/result.h"

namespace forma::git {

struct FileStatus {
    std::string path;
    std::string originalPath;
    char staged = ' ';
    char unstaged = ' ';
    bool untracked = false;
    bool conflicted = false;
};

struct RepositoryStatus {
    bool isRepository = false;
    std::string branch;
    std::string upstream;
    int ahead = 0;
    int behind = 0;
    bool detached = false;
    std::vector<FileStatus> files;
};

struct Commit {
    std::string hash;
    std::string shortHash;
    std::string author;
    std::string date;
    std::string subject;
};

struct Branch {
    std::string name;
    bool current = false;
    bool remote = false;
    std::string upstream;
};

struct CommandResult {
    int exitCode = 0;
    std::string output;
    std::string error;
    bool ok() const { return exitCode == 0; }
};

class GitClient {
public:
    explicit GitClient(std::filesystem::path workingDirectory);

    static bool available();
    static Result<CommandResult> clone(std::string_view url, const std::filesystem::path& destination);

    Result<CommandResult> run(const std::vector<std::string>& arguments, std::chrono::milliseconds timeout = std::chrono::minutes(5)) const;

    Result<CommandResult> init(std::string_view branch = "main");
    Result<RepositoryStatus> status() const;
    Result<std::vector<Commit>> log(int limit = 200, std::string_view path = {}) const;
    Result<std::vector<Branch>> branches() const;
    Result<std::string> diff(std::string_view path, bool staged) const;
    Result<std::string> diffCommit(std::string_view hash) const;
    Result<std::string> showFile(std::string_view revision, std::string_view path) const;

    Result<CommandResult> stage(const std::vector<std::string>& paths);
    Result<CommandResult> unstage(const std::vector<std::string>& paths);
    Result<CommandResult> discard(const std::vector<std::string>& paths);
    Result<CommandResult> commit(std::string_view message, bool amend = false);
    Result<CommandResult> createBranch(std::string_view name, bool checkout = true);
    Result<CommandResult> checkout(std::string_view name);
    Result<CommandResult> deleteBranch(std::string_view name, bool force = false);
    Result<CommandResult> merge(std::string_view name);
    Result<CommandResult> rebase(std::string_view onto);
    Result<CommandResult> abortMerge();
    Result<CommandResult> abortRebase();
    Result<CommandResult> continueRebase();
    Result<CommandResult> pull();
    Result<CommandResult> push(bool setUpstream = false);
    Result<CommandResult> fetch();
    Result<CommandResult> resolveWithOurs(std::string_view path);
    Result<CommandResult> resolveWithTheirs(std::string_view path);
    Status addToIgnore(std::string_view pattern);

    const std::filesystem::path& workingDirectory() const { return directory_; }

private:
    std::filesystem::path directory_;
};

RepositoryStatus parsePorcelainStatus(std::string_view output);

}
