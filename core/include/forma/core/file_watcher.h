#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "forma/core/result.h"

namespace forma {

enum class FileChangeKind { Added, Removed, Modified, Renamed };

struct FileChange {
    FileChangeKind kind = FileChangeKind::Modified;
    std::string path;
    std::string previousPath;
};

using FileChangeCallback = std::function<void(const std::vector<FileChange>&)>;

class IFileWatcher {
public:
    virtual ~IFileWatcher() = default;
    virtual Status start(const std::filesystem::path& root, FileChangeCallback callback) = 0;
    virtual void stop() = 0;
    virtual const char* backendName() const = 0;
};

std::unique_ptr<IFileWatcher> createFileWatcher();
std::unique_ptr<IFileWatcher> createPollingFileWatcher(std::chrono::milliseconds interval = std::chrono::seconds(2));

}
