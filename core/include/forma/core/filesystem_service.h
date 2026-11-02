#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "forma/core/result.h"

namespace forma {

enum class Origin { User, Generator, Build, AI, Plugin, System };

const char* originName(Origin origin);

struct WriteOptions {
    Origin origin = Origin::User;
    std::optional<std::string> expectedHash;
    bool backup = true;
    bool executable = false;
};

struct FileEntry {
    std::string path;
    bool directory = false;
    std::uint64_t size = 0;
    std::int64_t modifiedMillis = 0;
};

struct ListOptions {
    bool recursive = false;
    bool includeHidden = false;
    std::vector<std::string> ignoredNames;
};

struct FileOperation {
    std::string action;
    std::string path;
    std::string previousPath;
    Origin origin = Origin::User;
    std::string hash;
    std::string recoveryId;
};

using FileOperationListener = std::function<void(const FileOperation&)>;

class IFilesystemService {
public:
    virtual ~IFilesystemService() = default;

    virtual const std::filesystem::path& root() const = 0;
    virtual Result<std::filesystem::path> resolve(std::string_view relativePath) const = 0;
    virtual bool exists(std::string_view relativePath) const = 0;
    virtual Result<std::string> readFile(std::string_view relativePath) const = 0;
    virtual Result<std::string> fileHash(std::string_view relativePath) const = 0;
    virtual Status writeFile(std::string_view relativePath, std::string_view data, const WriteOptions& options = {}) = 0;
    virtual Status createDirectories(std::string_view relativePath) = 0;
    virtual Result<std::string> remove(std::string_view relativePath, Origin origin = Origin::User) = 0;
    virtual Status rename(std::string_view from, std::string_view to, Origin origin = Origin::User) = 0;
    virtual Status copyFile(std::string_view from, std::string_view to, Origin origin = Origin::User) = 0;
    virtual Status importFile(const std::filesystem::path& source, std::string_view to, Origin origin = Origin::User) = 0;
    virtual Result<std::vector<FileEntry>> list(std::string_view relativePath, const ListOptions& options = {}) const = 0;
    virtual Status restoreFromTrash(std::string_view recoveryId) = 0;
    virtual void setOperationListener(FileOperationListener listener) = 0;
};

Status validateRelativePath(std::string_view relativePath);
std::string normalizeRelativePath(std::string_view relativePath);
const std::vector<std::string>& defaultIgnoredNames();

std::unique_ptr<IFilesystemService> createLocalFilesystemService(std::filesystem::path root);

}
