#include "forma/core/recovery.h"

#include <algorithm>

#include "forma/core/hash.h"
#include "forma/core/log.h"
#include "forma/core/paths.h"
#include "forma/core/process.h"
#include "forma/core/text.h"

namespace fs = std::filesystem;

namespace forma {

namespace {

bool isSnapshotExcluded(const std::string& path)
{
    const auto& ignored = defaultIgnoredNames();
    for (const auto& part : text::split(path, '/')) {
        if (std::find(ignored.begin(), ignored.end(), part) != ignored.end())
            return true;
    }
    return false;
}

}

fs::path SnapshotStore::directory() const
{
    return filesystem_.root() / ".forma" / "snapshots";
}

Result<SnapshotInfo> SnapshotStore::create(std::string_view label, std::string_view actor, const std::vector<std::string>& extraPaths)
{
    SnapshotInfo info;
    info.id = paths::timestampId();
    info.label = std::string(label);
    info.actor = std::string(actor);
    info.time = paths::nowIso8601();

    ListOptions options;
    options.recursive = true;
    options.includeHidden = true;
    options.ignoredNames = defaultIgnoredNames();
    auto entries = filesystem_.list("", options);
    if (!entries)
        return entries.error();

    Json files = Json::object();
    fs::path objects = directory() / "objects";
    auto addFile = [&](const std::string& path) -> Status {
        auto content = filesystem_.readFile(path);
        if (!content)
            return content.error();
        std::string hash = sha256Hex(*content);
        fs::path object = objects / hash.substr(0, 2) / hash;
        std::error_code ec;
        if (!fs::exists(object, ec)) {
            if (auto status = paths::writeFileAtomic(object, *content); !status)
                return status;
        }
        files[path] = hash;
        return {};
    };

    for (const auto& entry : *entries) {
        if (entry.directory || isSnapshotExcluded(entry.path))
            continue;
        if (auto status = addFile(entry.path); !status)
            return status.error();
    }
    for (const auto& path : extraPaths) {
        if (filesystem_.exists(path) && !files.contains(path)) {
            if (auto status = addFile(path); !status)
                return status.error();
        }
    }

    Json manifest = Json::object();
    manifest["id"] = info.id;
    manifest["label"] = info.label;
    manifest["actor"] = info.actor;
    manifest["time"] = info.time;
    manifest["files"] = files;
    info.fileCount = files.size();
    if (auto status = paths::writeFileAtomic(directory() / (info.id + ".json"), dumpJson(manifest)); !status)
        return status.error();
    return info;
}

Result<std::vector<SnapshotInfo>> SnapshotStore::list() const
{
    std::vector<SnapshotInfo> snapshots;
    std::error_code ec;
    if (!fs::is_directory(directory(), ec))
        return snapshots;
    for (fs::directory_iterator it(directory(), ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".json")
            continue;
        auto content = paths::readWholeFile(it->path());
        if (!content)
            continue;
        auto manifest = parseJson(*content);
        if (!manifest)
            continue;
        SnapshotInfo info;
        info.id = jsonString(*manifest, "id");
        info.label = jsonString(*manifest, "label");
        info.actor = jsonString(*manifest, "actor");
        info.time = jsonString(*manifest, "time");
        if (manifest->contains("files"))
            info.fileCount = (*manifest)["files"].size();
        snapshots.push_back(std::move(info));
    }
    std::sort(snapshots.begin(), snapshots.end(), [](const auto& a, const auto& b) { return a.id > b.id; });
    return snapshots;
}

Result<Json> SnapshotStore::manifest(std::string_view id) const
{
    auto content = paths::readWholeFile(directory() / (std::string(id) + ".json"));
    if (!content)
        return makeError("snapshot.missing", "No snapshot with id " + std::string(id));
    return parseJson(*content);
}

Result<std::string> SnapshotStore::fileContent(std::string_view id, std::string_view path) const
{
    auto loaded = manifest(id);
    if (!loaded)
        return loaded.error();
    const auto& files = (*loaded)["files"];
    auto it = files.find(std::string(path));
    if (it == files.end())
        return makeError("snapshot.missing", "The snapshot has no file " + std::string(path));
    std::string hash = it->get<std::string>();
    return paths::readWholeFile(directory() / "objects" / hash.substr(0, 2) / hash);
}

Result<std::vector<std::string>> SnapshotStore::restore(std::string_view id, std::vector<std::string>* removedFiles)
{
    auto loaded = manifest(id);
    if (!loaded)
        return loaded.error();

    auto safety = create("Before restoring " + std::string(id), "system");
    if (!safety)
        return safety.error();

    const auto& files = (*loaded)["files"];
    std::vector<std::string> restored;
    for (auto it = files.begin(); it != files.end(); ++it) {
        std::string hash = it.value().get<std::string>();
        auto content = paths::readWholeFile(directory() / "objects" / hash.substr(0, 2) / hash);
        if (!content)
            return content.error();
        auto current = filesystem_.fileHash(it.key());
        if (current && *current == hash)
            continue;
        WriteOptions options;
        options.origin = Origin::System;
        if (auto status = filesystem_.writeFile(it.key(), *content, options); !status)
            return status.error();
        restored.push_back(it.key());
    }

    ListOptions options;
    options.recursive = true;
    options.includeHidden = true;
    options.ignoredNames = defaultIgnoredNames();
    auto entries = filesystem_.list("", options);
    if (entries) {
        for (const auto& entry : *entries) {
            if (entry.directory || isSnapshotExcluded(entry.path) || files.contains(entry.path))
                continue;
            auto removed = filesystem_.remove(entry.path, Origin::System);
            if (removed && removedFiles != nullptr)
                removedFiles->push_back(entry.path);
        }
    }
    return restored;
}

Status SnapshotStore::remove(std::string_view id)
{
    std::error_code ec;
    fs::remove(directory() / (std::string(id) + ".json"), ec);
    if (ec)
        return makeError("snapshot.delete", ec.message());
    return {};
}

Status SnapshotStore::prune(std::size_t keep)
{
    auto all = list();
    if (!all)
        return all.error();
    for (std::size_t i = keep; i < all->size(); ++i) {
        if (auto status = remove((*all)[i].id); !status)
            return status;
    }
    std::vector<std::string> live;
    auto remaining = list();
    if (!remaining)
        return remaining.error();
    for (const auto& info : *remaining) {
        auto loaded = manifest(info.id);
        if (!loaded)
            continue;
        for (const auto& item : (*loaded)["files"])
            live.push_back(item.get<std::string>());
    }
    std::sort(live.begin(), live.end());
    std::error_code ec;
    fs::path objects = directory() / "objects";
    for (fs::recursive_directory_iterator it(objects, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec))
            continue;
        if (!std::binary_search(live.begin(), live.end(), paths::toUtf8(it->path().filename())))
            fs::remove(it->path(), ec);
    }
    return {};
}

RecoveryStore::RecoveryStore(fs::path projectRoot) : root_(std::move(projectRoot) / ".forma" / "recovery") {}

bool RecoveryStore::beginSession()
{
    fs::path lock = root_ / "session.json";
    auto existing = paths::readWholeFile(lock);
    crashed_ = false;
    if (existing) {
        auto parsed = parseJson(*existing);
        if (parsed) {
            int pid = jsonInt(*parsed, "pid", 0);
            if (pid != 0 && pid != currentProcessId() && !isProcessAlive(pid))
                crashed_ = true;
        }
    }
    Json session = Json::object();
    session["pid"] = currentProcessId();
    session["started"] = paths::nowIso8601();
    if (auto status = paths::writeFileAtomic(lock, dumpJson(session)); !status)
        log::warning("recovery", "cannot write the session marker");
    return crashed_;
}

void RecoveryStore::endSession()
{
    std::error_code ec;
    fs::remove(root_ / "session.json", ec);
}

Status RecoveryStore::saveBuffer(std::string_view relativePath, std::string_view content, std::string_view baseHash)
{
    Json record = Json::object();
    record["path"] = std::string(relativePath);
    record["baseHash"] = std::string(baseHash);
    record["savedAt"] = paths::nowIso8601();
    record["content"] = std::string(content);
    return paths::writeFileAtomic(root_ / "buffers" / (sha256Hex(relativePath) + ".json"), dumpJsonCompact(record));
}

Status RecoveryStore::clearBuffer(std::string_view relativePath)
{
    std::error_code ec;
    fs::remove(root_ / "buffers" / (sha256Hex(relativePath) + ".json"), ec);
    return {};
}

Result<std::vector<RecoveredBuffer>> RecoveryStore::buffers() const
{
    std::vector<RecoveredBuffer> result;
    std::error_code ec;
    fs::path directory = root_ / "buffers";
    if (!fs::is_directory(directory, ec))
        return result;
    for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        auto content = paths::readWholeFile(it->path());
        if (!content)
            continue;
        auto parsed = parseJson(*content);
        if (!parsed)
            continue;
        RecoveredBuffer buffer;
        buffer.path = jsonString(*parsed, "path");
        buffer.baseHash = jsonString(*parsed, "baseHash");
        buffer.savedAt = jsonString(*parsed, "savedAt");
        buffer.content = jsonString(*parsed, "content");
        result.push_back(std::move(buffer));
    }
    return result;
}

Status RecoveryStore::saveDesignState(const Json& state)
{
    std::vector<std::uint8_t> bytes = toCbor(state);
    return paths::writeFileAtomic(root_ / "design.cbor", std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

std::optional<Json> RecoveryStore::designState() const
{
    auto content = paths::readWholeFile(root_ / "design.cbor");
    if (!content)
        return std::nullopt;
    std::vector<std::uint8_t> bytes(content->begin(), content->end());
    auto parsed = fromCbor(bytes);
    if (!parsed)
        return std::nullopt;
    return std::move(parsed.value());
}

Status RecoveryStore::clearDesignState()
{
    std::error_code ec;
    fs::remove(root_ / "design.cbor", ec);
    return {};
}

}
