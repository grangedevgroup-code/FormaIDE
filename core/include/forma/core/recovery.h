#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "forma/core/filesystem_service.h"
#include "forma/core/json.h"

namespace forma {

struct SnapshotInfo {
    std::string id;
    std::string label;
    std::string time;
    std::string actor;
    std::size_t fileCount = 0;
};

class SnapshotStore {
public:
    explicit SnapshotStore(IFilesystemService& filesystem) : filesystem_(filesystem) {}

    Result<SnapshotInfo> create(std::string_view label, std::string_view actor, const std::vector<std::string>& extraPaths = {});
    Result<std::vector<SnapshotInfo>> list() const;
    Result<std::vector<std::string>> restore(std::string_view id, std::vector<std::string>* removedFiles = nullptr);
    Result<Json> manifest(std::string_view id) const;
    Result<std::string> fileContent(std::string_view id, std::string_view path) const;
    Status remove(std::string_view id);
    Status prune(std::size_t keep);

private:
    std::filesystem::path directory() const;
    IFilesystemService& filesystem_;
};

struct RecoveredBuffer {
    std::string path;
    std::string content;
    std::string baseHash;
    std::string savedAt;
};

class RecoveryStore {
public:
    explicit RecoveryStore(std::filesystem::path projectRoot);

    bool beginSession();
    void endSession();
    bool previousSessionCrashed() const { return crashed_; }

    Status saveBuffer(std::string_view relativePath, std::string_view content, std::string_view baseHash);
    Status clearBuffer(std::string_view relativePath);
    Result<std::vector<RecoveredBuffer>> buffers() const;

    Status saveDesignState(const Json& state);
    std::optional<Json> designState() const;
    Status clearDesignState();

private:
    std::filesystem::path root_;
    bool crashed_ = false;
};

}
