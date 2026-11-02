#include "forma/core/filesystem_service.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <mutex>

#include "forma/core/hash.h"
#include "forma/core/log.h"
#include "forma/core/paths.h"
#include "forma/core/text.h"

namespace fs = std::filesystem;

namespace forma {

const char* originName(Origin origin)
{
    switch (origin) {
    case Origin::User:
        return "user";
    case Origin::Generator:
        return "generator";
    case Origin::Build:
        return "build";
    case Origin::AI:
        return "ai";
    case Origin::Plugin:
        return "plugin";
    case Origin::System:
        return "system";
    }
    return "user";
}

const std::vector<std::string>& defaultIgnoredNames()
{
    static const std::vector<std::string> names = {
        ".git", ".hg", ".svn", ".forma", "node_modules", "build", "out", "dist", "bin", "obj", ".gradle",
        ".idea", ".vs", ".vscode", ".dart_tool", "target", "DerivedData", "__pycache__", ".venv", ".cache",
        "Pods", ".swiftpm", ".next", ".nuxt", "vendor", ".DS_Store", "Thumbs.db"};
    return names;
}

std::string normalizeRelativePath(std::string_view relativePath)
{
    std::string output = text::replaceAll(std::string(relativePath), "\\", "/");
    while (text::startsWith(output, "./"))
        output.erase(0, 2);
    while (output.size() > 1 && output.back() == '/')
        output.pop_back();
    return output;
}

Status validateRelativePath(std::string_view relativePath)
{
    std::string path = normalizeRelativePath(relativePath);
    if (path.empty())
        return makeError("path.empty", "The path is empty");
    if (path.find('\0') != std::string::npos)
        return makeError("path.invalid", "The path contains a NUL character");
    if (path[0] == '/' || path[0] == '~')
        return makeError("path.absolute", "Expected a path inside the project: " + path);
    if (path.size() >= 2 && path[1] == ':')
        return makeError("path.absolute", "Expected a path inside the project: " + path);

    static const std::array<const char*, 22> reserved = {
        "con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7",
        "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};

    for (const auto& part : text::split(path, '/')) {
        if (part.empty() || part == "..")
            return makeError("path.traversal", "The path leaves the project: " + path);
        if (part == ".")
            continue;
        for (char ch : part) {
            auto c = static_cast<unsigned char>(ch);
            if (c < 0x20 || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|')
                return makeError("path.invalid", "The path contains an invalid character: " + path);
        }
        if (part.back() == ' ' || part.back() == '.')
            return makeError("path.invalid", "A path part cannot end with a space or dot: " + path);
        std::string stem = text::toLower(part.substr(0, part.find('.')));
        for (const char* name : reserved) {
            if (stem == name)
                return makeError("path.reserved", "The name is reserved by the operating system: " + part);
        }
    }
    return {};
}

namespace {

class LocalFilesystemService final : public IFilesystemService {
public:
    explicit LocalFilesystemService(fs::path root)
    {
        std::error_code ec;
        fs::create_directories(root, ec);
        root_ = fs::weakly_canonical(root, ec);
        if (ec)
            root_ = std::move(root);
    }

    const fs::path& root() const override { return root_; }

    Result<fs::path> resolve(std::string_view relativePath) const override
    {
        if (auto status = validateRelativePath(relativePath); !status)
            return status.error();
        fs::path full = root_ / paths::fromUtf8(normalizeRelativePath(relativePath));
        std::error_code ec;
        fs::path canonical = fs::weakly_canonical(full, ec);
        if (ec)
            return makeError("path.resolve", "Cannot resolve " + paths::toUtf8(full) + ": " + ec.message());
        auto relative = canonical.lexically_relative(root_);
        if (relative.empty() || *relative.begin() == "..")
            return makeError("path.escape", "The path leaves the project through a link: " + std::string(relativePath));
        return full;
    }

    bool exists(std::string_view relativePath) const override
    {
        auto full = resolve(relativePath);
        if (!full)
            return false;
        std::error_code ec;
        return fs::exists(*full, ec);
    }

    Result<std::string> readFile(std::string_view relativePath) const override
    {
        auto full = resolve(relativePath);
        if (!full)
            return full.error();
        return paths::readWholeFile(*full);
    }

    Result<std::string> fileHash(std::string_view relativePath) const override
    {
        auto content = readFile(relativePath);
        if (!content)
            return content.error();
        return sha256Hex(*content);
    }

    Status writeFile(std::string_view relativePath, std::string_view data, const WriteOptions& options) override
    {
        auto full = resolve(relativePath);
        if (!full)
            return full.error();
        std::error_code ec;
        bool existed = fs::exists(*full, ec);
        if (existed && fs::is_directory(*full, ec))
            return makeError("fs.isdir", "A folder already exists at " + std::string(relativePath));
        std::string previousHash;
        std::string recoveryId;
        if (existed) {
            auto previous = paths::readWholeFile(*full);
            if (!previous)
                return previous.error();
            previousHash = sha256Hex(*previous);
            if (options.expectedHash && previousHash != *options.expectedHash)
                return makeError("fs.conflict", "The file changed on disk: " + std::string(relativePath));
            if (previous->size() == data.size() && *previous == data)
                return {};
            if (options.backup)
                recoveryId = storeCopy("backups", relativePath, *previous);
        } else if (options.expectedHash && !options.expectedHash->empty()) {
            return makeError("fs.conflict", "The file was removed on disk: " + std::string(relativePath));
        }
        if (auto status = paths::writeFileAtomic(*full, data, options.executable); !status)
            return status;
        notify({existed ? "modify" : "create", normalizeRelativePath(relativePath), {}, options.origin,
                sha256Hex(data), recoveryId});
        return {};
    }

    Status createDirectories(std::string_view relativePath) override
    {
        auto full = resolve(relativePath);
        if (!full)
            return full.error();
        std::error_code ec;
        fs::create_directories(*full, ec);
        if (ec)
            return makeError("fs.mkdir", "Cannot create " + std::string(relativePath) + ": " + ec.message());
        return {};
    }

    Result<std::string> remove(std::string_view relativePath, Origin origin) override
    {
        auto full = resolve(relativePath);
        if (!full)
            return full.error();
        std::error_code ec;
        if (!fs::exists(*full, ec))
            return makeError("fs.missing", "No such file: " + std::string(relativePath));
        std::string id = paths::timestampId();
        fs::path destination = root_ / ".forma" / "trash" / id / paths::fromUtf8(normalizeRelativePath(relativePath));
        fs::create_directories(destination.parent_path(), ec);
        if (ec)
            return makeError("fs.trash", "Cannot prepare the trash folder: " + ec.message());
        fs::rename(*full, destination, ec);
        if (ec) {
            ec.clear();
            fs::copy(*full, destination, fs::copy_options::recursive, ec);
            if (ec)
                return makeError("fs.trash", "Cannot move to trash: " + ec.message());
            fs::remove_all(*full, ec);
            if (ec)
                return makeError("fs.delete", "Cannot delete " + std::string(relativePath) + ": " + ec.message());
        }
        notify({"delete", normalizeRelativePath(relativePath), {}, origin, {}, id});
        return id;
    }

    Status rename(std::string_view from, std::string_view to, Origin origin) override
    {
        auto source = resolve(from);
        if (!source)
            return source.error();
        auto destination = resolve(to);
        if (!destination)
            return destination.error();
        std::error_code ec;
        if (!fs::exists(*source, ec))
            return makeError("fs.missing", "No such file: " + std::string(from));
        if (fs::exists(*destination, ec) && !fs::equivalent(*source, *destination, ec))
            return makeError("fs.exists", "A file already exists at " + std::string(to));
        fs::create_directories(destination->parent_path(), ec);
        fs::rename(*source, *destination, ec);
        if (ec)
            return makeError("fs.rename", "Cannot rename " + std::string(from) + ": " + ec.message());
        notify({"rename", normalizeRelativePath(to), normalizeRelativePath(from), origin, {}, {}});
        return {};
    }

    Status copyFile(std::string_view from, std::string_view to, Origin origin) override
    {
        auto source = resolve(from);
        if (!source)
            return source.error();
        auto destination = resolve(to);
        if (!destination)
            return destination.error();
        std::error_code ec;
        if (fs::exists(*destination, ec))
            return makeError("fs.exists", "A file already exists at " + std::string(to));
        fs::create_directories(destination->parent_path(), ec);
        fs::copy(*source, *destination, fs::copy_options::recursive, ec);
        if (ec)
            return makeError("fs.copy", "Cannot copy " + std::string(from) + ": " + ec.message());
        notify({"create", normalizeRelativePath(to), normalizeRelativePath(from), origin, {}, {}});
        return {};
    }

    Status importFile(const fs::path& source, std::string_view to, Origin origin) override
    {
        auto destination = resolve(to);
        if (!destination)
            return destination.error();
        auto data = paths::readWholeFile(source);
        if (!data)
            return data.error();
        WriteOptions options;
        options.origin = origin;
        return writeFile(to, *data, options);
    }

    Result<std::vector<FileEntry>> list(std::string_view relativePath, const ListOptions& options) const override
    {
        fs::path start = root_;
        std::string prefix;
        std::string normalized = normalizeRelativePath(relativePath);
        if (!normalized.empty() && normalized != ".") {
            auto full = resolve(normalized);
            if (!full)
                return full.error();
            start = *full;
            prefix = normalized + "/";
        }
        std::vector<FileEntry> entries;
        std::error_code ec;
        if (!fs::is_directory(start, ec))
            return makeError("fs.notdir", "Not a folder: " + normalized);
        collect(start, prefix, options, entries);
        std::sort(entries.begin(), entries.end(), [](const FileEntry& a, const FileEntry& b) {
            if (a.directory != b.directory && !a.path.empty() && !b.path.empty()) {
                auto parentA = a.path.substr(0, a.path.find_last_of('/') + 1);
                auto parentB = b.path.substr(0, b.path.find_last_of('/') + 1);
                if (parentA == parentB)
                    return a.directory;
            }
            return text::toLower(a.path) < text::toLower(b.path);
        });
        return entries;
    }

    Status restoreFromTrash(std::string_view recoveryId) override
    {
        fs::path trash = root_ / ".forma" / "trash" / paths::fromUtf8(recoveryId);
        std::error_code ec;
        if (!fs::is_directory(trash, ec))
            return makeError("fs.missing", "Nothing in the trash with id " + std::string(recoveryId));
        for (auto it = fs::recursive_directory_iterator(trash, ec); it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            if (ec)
                return makeError("fs.restore", ec.message());
            if (!it->is_regular_file(ec))
                continue;
            auto relative = paths::toGeneric(it->path().lexically_relative(trash));
            auto destination = resolve(relative);
            if (!destination)
                return destination.error();
            if (fs::exists(*destination, ec))
                return makeError("fs.exists", "Restoring would overwrite " + relative);
            fs::create_directories(destination->parent_path(), ec);
            fs::copy_file(it->path(), *destination, ec);
            if (ec)
                return makeError("fs.restore", "Cannot restore " + relative + ": " + ec.message());
            notify({"create", relative, {}, Origin::User, {}, std::string(recoveryId)});
        }
        fs::remove_all(trash, ec);
        return {};
    }

    void setOperationListener(FileOperationListener listener) override
    {
        std::lock_guard lock(mutex_);
        listener_ = std::move(listener);
    }

private:
    void notify(FileOperation operation)
    {
        FileOperationListener listener;
        {
            std::lock_guard lock(mutex_);
            listener = listener_;
        }
        log::debug("filesystem", operation.action,
                   Json{{"path", operation.path}, {"origin", originName(operation.origin)}});
        if (listener)
            listener(operation);
    }

    std::string storeCopy(const char* area, std::string_view relativePath, std::string_view data)
    {
        if (text::startsWith(normalizeRelativePath(relativePath), ".forma/"))
            return {};
        std::string id = paths::timestampId();
        fs::path destination = root_ / ".forma" / area / id / paths::fromUtf8(normalizeRelativePath(relativePath));
        if (auto status = paths::writeFileAtomic(destination, data); !status) {
            log::warning("filesystem", "backup failed", Json{{"path", relativePath}});
            return {};
        }
        return id;
    }

    void collect(const fs::path& directory, const std::string& prefix, const ListOptions& options,
                 std::vector<FileEntry>& entries) const
    {
        std::error_code ec;
        for (fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            std::string name = paths::toUtf8(it->path().filename());
            if (!options.includeHidden && !name.empty() && name[0] == '.')
                continue;
            if (std::find(options.ignoredNames.begin(), options.ignoredNames.end(), name) != options.ignoredNames.end())
                continue;
            FileEntry entry;
            entry.path = prefix + name;
            std::error_code statError;
            entry.directory = it->is_directory(statError);
            if (!entry.directory) {
                entry.size = it->file_size(statError);
                auto time = it->last_write_time(statError);
                if (!statError) {
                    auto system = std::chrono::clock_cast<std::chrono::system_clock>(time);
                    entry.modifiedMillis
                        = std::chrono::duration_cast<std::chrono::milliseconds>(system.time_since_epoch()).count();
                }
            }
            entries.push_back(entry);
            bool isLink = it->is_symlink(statError);
            if (options.recursive && entry.directory && !isLink)
                collect(it->path(), entry.path + "/", options, entries);
        }
    }

    fs::path root_;
    std::mutex mutex_;
    FileOperationListener listener_;
};

}

std::unique_ptr<IFilesystemService> createLocalFilesystemService(fs::path root)
{
    return std::make_unique<LocalFilesystemService>(std::move(root));
}

}
