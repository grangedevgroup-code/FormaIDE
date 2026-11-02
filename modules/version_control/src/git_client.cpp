#include "forma/git/git_client.h"

#include <chrono>

#include "forma/core/paths.h"
#include "forma/core/process.h"
#include "forma/core/text.h"

namespace fs = std::filesystem;

namespace forma::git {

GitClient::GitClient(fs::path workingDirectory) : directory_(std::move(workingDirectory)) {}

bool GitClient::available()
{
    return findExecutable("git").has_value();
}

namespace {

Result<CommandResult> execute(const fs::path& directory, const std::vector<std::string>& arguments, std::chrono::milliseconds timeout)
{
    ProcessSpec spec;
    spec.program = "git";
    spec.arguments = {"-c", "core.quotepath=false", "-c", "color.ui=false", "-c", "core.pager="};
    spec.arguments.insert(spec.arguments.end(), arguments.begin(), arguments.end());
    spec.workingDirectory = directory;
    spec.environment["GIT_TERMINAL_PROMPT"] = "0";
    spec.environment["GIT_EDITOR"] = "true";
    spec.origin = Origin::User;
    auto result = runProcess(spec, timeout);
    if (!result)
        return result.error();
    if (result->timedOut)
        return makeError("git.timeout", "git took too long and was stopped");
    return CommandResult{result->exitCode, std::move(result->out), std::move(result->err)};
}

}

Result<CommandResult> GitClient::clone(std::string_view url, const fs::path& destination)
{
    if (text::startsWith(url, "-"))
        return makeError("git.url", "That is not a repository address");
    return execute(destination.parent_path(), {"clone", "--", std::string(url), paths::toUtf8(destination)}, std::chrono::minutes(30));
}

Result<CommandResult> GitClient::run(const std::vector<std::string>& arguments, std::chrono::milliseconds timeout) const
{
    return execute(directory_, arguments, timeout);
}

Result<CommandResult> GitClient::init(std::string_view branch)
{
    return run({"init", "-b", std::string(branch)});
}

RepositoryStatus parsePorcelainStatus(std::string_view output)
{
    RepositoryStatus status;
    status.isRepository = true;
    std::size_t position = 0;
    std::vector<std::string> records;
    while (position < output.size()) {
        auto end = output.find('\0', position);
        if (end == std::string_view::npos)
            end = output.size();
        records.emplace_back(output.substr(position, end - position));
        position = end + 1;
    }
    for (std::size_t i = 0; i < records.size(); ++i) {
        const std::string& record = records[i];
        if (record.empty())
            continue;
        if (text::startsWith(record, "# branch.head ")) {
            status.branch = record.substr(14);
            if (status.branch == "(detached)")
                status.detached = true;
        } else if (text::startsWith(record, "# branch.upstream ")) {
            status.upstream = record.substr(18);
        } else if (text::startsWith(record, "# branch.ab ")) {
            auto parts = text::split(record.substr(12), ' ');
            if (parts.size() == 2) {
                status.ahead = std::atoi(parts[0].c_str() + 1);
                status.behind = std::atoi(parts[1].c_str() + 1);
            }
        } else if (record[0] == '1' && record.size() > 4) {
            auto parts = text::split(record, ' ');
            if (parts.size() < 9)
                continue;
            FileStatus file;
            file.staged = parts[1][0] == '.' ? ' ' : parts[1][0];
            file.unstaged = parts[1][1] == '.' ? ' ' : parts[1][1];
            std::string path;
            for (std::size_t k = 8; k < parts.size(); ++k) {
                if (k > 8)
                    path.push_back(' ');
                path += parts[k];
            }
            file.path = path;
            status.files.push_back(std::move(file));
        } else if (record[0] == '2' && record.size() > 4) {
            auto parts = text::split(record, ' ');
            if (parts.size() < 10)
                continue;
            FileStatus file;
            file.staged = parts[1][0] == '.' ? ' ' : parts[1][0];
            file.unstaged = parts[1][1] == '.' ? ' ' : parts[1][1];
            std::string path;
            for (std::size_t k = 9; k < parts.size(); ++k) {
                if (k > 9)
                    path.push_back(' ');
                path += parts[k];
            }
            file.path = path;
            if (i + 1 < records.size())
                file.originalPath = records[++i];
            status.files.push_back(std::move(file));
        } else if (record[0] == 'u') {
            auto parts = text::split(record, ' ');
            if (parts.size() < 11)
                continue;
            FileStatus file;
            file.conflicted = true;
            file.staged = 'U';
            file.unstaged = 'U';
            std::string path;
            for (std::size_t k = 10; k < parts.size(); ++k) {
                if (k > 10)
                    path.push_back(' ');
                path += parts[k];
            }
            file.path = path;
            status.files.push_back(std::move(file));
        } else if (record[0] == '?') {
            FileStatus file;
            file.untracked = true;
            file.unstaged = '?';
            file.path = record.substr(2);
            status.files.push_back(std::move(file));
        }
    }
    return status;
}

Result<RepositoryStatus> GitClient::status() const
{
    auto result = run({"status", "--porcelain=v2", "--branch", "-z"}, std::chrono::seconds(60));
    if (!result)
        return result.error();
    if (!result->ok()) {
        RepositoryStatus status;
        status.isRepository = false;
        return status;
    }
    return parsePorcelainStatus(result->output);
}

Result<std::vector<Commit>> GitClient::log(int limit, std::string_view path) const
{
    std::vector<std::string> arguments = {"log", "-n", std::to_string(limit), "--date=short", "--format=%H%x1f%h%x1f%an%x1f%ad%x1f%s"};
    if (!path.empty()) {
        arguments.push_back("--");
        arguments.emplace_back(path);
    }
    auto result = run(arguments);
    if (!result)
        return result.error();
    std::vector<Commit> commits;
    if (!result->ok())
        return commits;
    for (const auto& line : text::splitLines(result->output)) {
        auto parts = text::split(line, '\x1f');
        if (parts.size() == 5)
            commits.push_back({parts[0], parts[1], parts[2], parts[3], parts[4]});
    }
    return commits;
}

Result<std::vector<Branch>> GitClient::branches() const
{
    auto result = run({"for-each-ref", "--format=%(refname)%1f%(HEAD)%1f%(upstream:short)", "refs/heads", "refs/remotes"});
    if (!result)
        return result.error();
    std::vector<Branch> list;
    for (const auto& line : text::splitLines(result->output)) {
        auto parts = text::split(line, '\x1f');
        if (parts.size() != 3)
            continue;
        Branch branch;
        branch.current = parts[1] == "*";
        branch.upstream = parts[2];
        if (text::startsWith(parts[0], "refs/heads/")) {
            branch.name = parts[0].substr(11);
        } else {
            branch.name = parts[0].substr(13);
            branch.remote = true;
            if (text::endsWith(branch.name, "/HEAD"))
                continue;
        }
        list.push_back(std::move(branch));
    }
    return list;
}

Result<std::string> GitClient::diff(std::string_view path, bool staged) const
{
    std::vector<std::string> arguments = {"diff"};
    if (staged)
        arguments.push_back("--cached");
    arguments.push_back("--");
    arguments.emplace_back(path);
    auto result = run(arguments);
    if (!result)
        return result.error();
    return result->output;
}

Result<std::string> GitClient::diffCommit(std::string_view hash) const
{
    auto result = run({"show", "--format=%H%n%an <%ae>%n%ad%n%n%B", std::string(hash)});
    if (!result)
        return result.error();
    return result->output;
}

Result<std::string> GitClient::showFile(std::string_view revision, std::string_view path) const
{
    auto result = run({"show", std::string(revision) + ":" + std::string(path)});
    if (!result)
        return result.error();
    if (!result->ok())
        return makeError("git.show", result->error);
    return result->output;
}

Result<CommandResult> GitClient::stage(const std::vector<std::string>& paths)
{
    std::vector<std::string> arguments = {"add", "--"};
    arguments.insert(arguments.end(), paths.begin(), paths.end());
    return run(arguments);
}

Result<CommandResult> GitClient::unstage(const std::vector<std::string>& paths)
{
    std::vector<std::string> arguments = {"restore", "--staged", "--"};
    arguments.insert(arguments.end(), paths.begin(), paths.end());
    auto result = run(arguments);
    if (result && !result->ok()) {
        std::vector<std::string> fallback = {"rm", "--cached", "-q", "--"};
        fallback.insert(fallback.end(), paths.begin(), paths.end());
        return run(fallback);
    }
    return result;
}

Result<CommandResult> GitClient::discard(const std::vector<std::string>& paths)
{
    std::vector<std::string> arguments = {"restore", "--"};
    arguments.insert(arguments.end(), paths.begin(), paths.end());
    return run(arguments);
}

Result<CommandResult> GitClient::commit(std::string_view message, bool amend)
{
    std::vector<std::string> arguments = {"commit", "-m", std::string(message)};
    if (amend)
        arguments.push_back("--amend");
    return run(arguments);
}

Result<CommandResult> GitClient::createBranch(std::string_view name, bool checkout)
{
    if (text::startsWith(name, "-"))
        return makeError("git.branch", "A branch name cannot start with a dash");
    if (checkout)
        return run({"switch", "-c", std::string(name)});
    return run({"branch", std::string(name)});
}

Result<CommandResult> GitClient::checkout(std::string_view name)
{
    if (text::startsWith(name, "-"))
        return makeError("git.branch", "A branch name cannot start with a dash");
    return run({"switch", std::string(name)});
}

Result<CommandResult> GitClient::deleteBranch(std::string_view name, bool force)
{
    if (text::startsWith(name, "-"))
        return makeError("git.branch", "A branch name cannot start with a dash");
    return run({"branch", force ? "-D" : "-d", std::string(name)});
}

Result<CommandResult> GitClient::merge(std::string_view name)
{
    return run({"merge", "--no-edit", "--", std::string(name)});
}

Result<CommandResult> GitClient::rebase(std::string_view onto)
{
    if (text::startsWith(onto, "-"))
        return makeError("git.branch", "A branch name cannot start with a dash");
    return run({"rebase", std::string(onto)});
}

Result<CommandResult> GitClient::abortMerge()
{
    return run({"merge", "--abort"});
}

Result<CommandResult> GitClient::abortRebase()
{
    return run({"rebase", "--abort"});
}

Result<CommandResult> GitClient::continueRebase()
{
    return run({"rebase", "--continue"});
}

Result<CommandResult> GitClient::pull()
{
    return run({"pull", "--no-edit"}, std::chrono::minutes(10));
}

Result<CommandResult> GitClient::push(bool setUpstream)
{
    if (setUpstream)
        return run({"push", "-u", "origin", "HEAD"}, std::chrono::minutes(10));
    return run({"push"}, std::chrono::minutes(10));
}

Result<CommandResult> GitClient::fetch()
{
    return run({"fetch", "--all", "--prune"}, std::chrono::minutes(10));
}

Result<CommandResult> GitClient::resolveWithOurs(std::string_view path)
{
    auto result = run({"checkout", "--ours", "--", std::string(path)});
    if (!result || !result->ok())
        return result;
    return stage({std::string(path)});
}

Result<CommandResult> GitClient::resolveWithTheirs(std::string_view path)
{
    auto result = run({"checkout", "--theirs", "--", std::string(path)});
    if (!result || !result->ok())
        return result;
    return stage({std::string(path)});
}

Status GitClient::addToIgnore(std::string_view pattern)
{
    fs::path file = directory_ / ".gitignore";
    std::string content;
    auto existing = paths::readWholeFile(file);
    if (existing)
        content = *existing;
    for (const auto& line : text::splitLines(content)) {
        if (line == pattern)
            return {};
    }
    if (!content.empty() && content.back() != '\n')
        content.push_back('\n');
    content += std::string(pattern) + "\n";
    return paths::writeFileAtomic(file, content);
}

}
