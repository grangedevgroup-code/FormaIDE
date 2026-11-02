#include "forma/core/file_watcher.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

#include "forma/core/paths.h"
#include "forma/core/text.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#elif defined(__linux__)
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace forma {

namespace {

class PollingWatcher final : public IFileWatcher {
public:
    explicit PollingWatcher(std::chrono::milliseconds interval) : interval_(interval) {}
    ~PollingWatcher() override { stop(); }

    Status start(const fs::path& root, FileChangeCallback callback) override
    {
        stop();
        root_ = root;
        callback_ = std::move(callback);
        snapshot_ = scan();
        stopping_ = false;
        thread_ = std::thread([this] { run(); });
        return {};
    }

    void stop() override
    {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        if (thread_.joinable())
            thread_.join();
    }

    const char* backendName() const override { return "polling"; }

private:
    struct Stamp {
        std::uint64_t size = 0;
        fs::file_time_type time{};
    };

    std::map<std::string, Stamp> scan() const
    {
        std::map<std::string, Stamp> files;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(root_, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            auto name = paths::toUtf8(it->path().filename());
            if (it->is_directory(ec) && (name == ".git" || name == "node_modules" || name == "build")) {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec))
                continue;
            Stamp stamp;
            stamp.size = it->file_size(ec);
            stamp.time = it->last_write_time(ec);
            files[paths::toGeneric(it->path().lexically_relative(root_))] = stamp;
        }
        return files;
    }

    void run()
    {
        while (true) {
            {
                std::unique_lock lock(mutex_);
                if (wake_.wait_for(lock, interval_, [this] { return stopping_; }))
                    return;
            }
            auto current = scan();
            std::vector<FileChange> changes;
            for (const auto& [path, stamp] : current) {
                auto previous = snapshot_.find(path);
                if (previous == snapshot_.end())
                    changes.push_back({FileChangeKind::Added, path, {}});
                else if (previous->second.size != stamp.size || previous->second.time != stamp.time)
                    changes.push_back({FileChangeKind::Modified, path, {}});
            }
            for (const auto& [path, stamp] : snapshot_) {
                if (current.find(path) == current.end())
                    changes.push_back({FileChangeKind::Removed, path, {}});
            }
            snapshot_ = std::move(current);
            if (!changes.empty() && callback_)
                callback_(changes);
        }
    }

    std::chrono::milliseconds interval_;
    fs::path root_;
    FileChangeCallback callback_;
    std::map<std::string, Stamp> snapshot_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
};

#if defined(_WIN32)

class WindowsWatcher final : public IFileWatcher {
public:
    ~WindowsWatcher() override { stop(); }

    Status start(const fs::path& root, FileChangeCallback callback) override
    {
        stop();
        directory_ = CreateFileW(root.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (directory_ == INVALID_HANDLE_VALUE)
            return makeError("watch.open", "Cannot watch " + paths::toUtf8(root));
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        callback_ = std::move(callback);
        thread_ = std::thread([this] { run(); });
        return {};
    }

    void stop() override
    {
        if (stopEvent_ != nullptr)
            SetEvent(stopEvent_);
        if (thread_.joinable())
            thread_.join();
        if (directory_ != INVALID_HANDLE_VALUE) {
            CloseHandle(directory_);
            directory_ = INVALID_HANDLE_VALUE;
        }
        if (stopEvent_ != nullptr) {
            CloseHandle(stopEvent_);
            stopEvent_ = nullptr;
        }
    }

    const char* backendName() const override { return "ReadDirectoryChangesW"; }

private:
    void run()
    {
        std::vector<DWORD> buffer(64 * 1024 / sizeof(DWORD));
        HANDLE readEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        while (true) {
            OVERLAPPED overlapped{};
            overlapped.hEvent = readEvent;
            ResetEvent(readEvent);
            DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE
                | FILE_NOTIFY_CHANGE_SIZE;
            if (!ReadDirectoryChangesW(directory_, buffer.data(), static_cast<DWORD>(buffer.size() * sizeof(DWORD)), TRUE,
                                       filter, nullptr, &overlapped, nullptr))
                break;
            HANDLE handles[2] = {readEvent, stopEvent_};
            DWORD signaled = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (signaled != WAIT_OBJECT_0) {
                CancelIoEx(directory_, &overlapped);
                DWORD ignored = 0;
                GetOverlappedResult(directory_, &overlapped, &ignored, TRUE);
                break;
            }
            DWORD bytes = 0;
            if (!GetOverlappedResult(directory_, &overlapped, &bytes, FALSE))
                break;
            if (bytes == 0)
                continue;
            std::vector<FileChange> changes;
            std::string renameFrom;
            const auto* base = reinterpret_cast<const char*>(buffer.data());
            std::size_t offset = 0;
            while (true) {
                const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(base + offset);
                std::wstring_view name(info->FileName, info->FileNameLength / sizeof(wchar_t));
                std::string path = paths::toGeneric(fs::path(std::wstring(name)));
                switch (info->Action) {
                case FILE_ACTION_ADDED:
                    changes.push_back({FileChangeKind::Added, path, {}});
                    break;
                case FILE_ACTION_REMOVED:
                    changes.push_back({FileChangeKind::Removed, path, {}});
                    break;
                case FILE_ACTION_MODIFIED:
                    changes.push_back({FileChangeKind::Modified, path, {}});
                    break;
                case FILE_ACTION_RENAMED_OLD_NAME:
                    renameFrom = path;
                    break;
                case FILE_ACTION_RENAMED_NEW_NAME:
                    changes.push_back({FileChangeKind::Renamed, path, renameFrom});
                    renameFrom.clear();
                    break;
                default:
                    break;
                }
                if (info->NextEntryOffset == 0)
                    break;
                offset += info->NextEntryOffset;
            }
            if (!changes.empty() && callback_)
                callback_(changes);
        }
        CloseHandle(readEvent);
    }

    HANDLE directory_ = INVALID_HANDLE_VALUE;
    HANDLE stopEvent_ = nullptr;
    FileChangeCallback callback_;
    std::thread thread_;
};

#elif defined(__linux__)

class InotifyWatcher final : public IFileWatcher {
public:
    ~InotifyWatcher() override { stop(); }

    Status start(const fs::path& root, FileChangeCallback callback) override
    {
        stop();
        root_ = root;
        callback_ = std::move(callback);
        inotify_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        wake_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (inotify_ < 0 || wake_ < 0)
            return makeError("watch.open", "Cannot initialize inotify");
        addTree(root_);
        thread_ = std::thread([this] { run(); });
        return {};
    }

    void stop() override
    {
        if (wake_ >= 0) {
            std::uint64_t one = 1;
            auto ignored = ::write(wake_, &one, sizeof one);
            (void)ignored;
        }
        if (thread_.joinable())
            thread_.join();
        if (inotify_ >= 0)
            ::close(inotify_);
        if (wake_ >= 0)
            ::close(wake_);
        inotify_ = -1;
        wake_ = -1;
        watches_.clear();
    }

    const char* backendName() const override { return "inotify"; }

private:
    void addTree(const fs::path& directory)
    {
        addOne(directory);
        std::error_code ec;
        for (fs::recursive_directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (!it->is_directory(ec) || it->is_symlink(ec))
                continue;
            auto name = paths::toUtf8(it->path().filename());
            if (name == ".git" || name == "node_modules" || name == "build") {
                it.disable_recursion_pending();
                continue;
            }
            addOne(it->path());
        }
    }

    void addOne(const fs::path& directory)
    {
        int watch = inotify_add_watch(inotify_, directory.c_str(),
                                      IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO);
        if (watch >= 0)
            watches_[watch] = directory;
    }

    void run()
    {
        std::vector<char> buffer(64 * 1024);
        pollfd items[2] = {{inotify_, POLLIN, 0}, {wake_, POLLIN, 0}};
        std::map<std::uint32_t, std::string> pendingMoves;
        while (true) {
            int ready = ::poll(items, 2, -1);
            if (ready < 0)
                continue;
            if (items[1].revents != 0)
                return;
            auto length = ::read(inotify_, buffer.data(), buffer.size());
            if (length <= 0)
                continue;
            std::vector<FileChange> changes;
            for (char* cursor = buffer.data(); cursor < buffer.data() + length;) {
                auto* event = reinterpret_cast<inotify_event*>(cursor);
                cursor += sizeof(inotify_event) + event->len;
                auto watch = watches_.find(event->wd);
                if (watch == watches_.end() || event->len == 0)
                    continue;
                fs::path full = watch->second / event->name;
                std::string path = paths::toGeneric(full.lexically_relative(root_));
                bool isDirectory = (event->mask & IN_ISDIR) != 0;
                if (event->mask & IN_CREATE) {
                    if (isDirectory)
                        addTree(full);
                    changes.push_back({FileChangeKind::Added, path, {}});
                } else if (event->mask & IN_DELETE) {
                    changes.push_back({FileChangeKind::Removed, path, {}});
                } else if (event->mask & (IN_MODIFY | IN_CLOSE_WRITE)) {
                    changes.push_back({FileChangeKind::Modified, path, {}});
                } else if (event->mask & IN_MOVED_FROM) {
                    pendingMoves[event->cookie] = path;
                } else if (event->mask & IN_MOVED_TO) {
                    auto move = pendingMoves.find(event->cookie);
                    if (move != pendingMoves.end()) {
                        changes.push_back({FileChangeKind::Renamed, path, move->second});
                        pendingMoves.erase(move);
                    } else {
                        changes.push_back({FileChangeKind::Added, path, {}});
                    }
                    if (isDirectory)
                        addTree(full);
                }
            }
            for (const auto& [cookie, path] : pendingMoves)
                changes.push_back({FileChangeKind::Removed, path, {}});
            pendingMoves.clear();
            if (!changes.empty() && callback_)
                callback_(changes);
        }
    }

    fs::path root_;
    FileChangeCallback callback_;
    int inotify_ = -1;
    int wake_ = -1;
    std::map<int, fs::path> watches_;
    std::thread thread_;
};

#elif defined(__APPLE__)

class FsEventsWatcher final : public IFileWatcher {
public:
    ~FsEventsWatcher() override { stop(); }

    Status start(const fs::path& root, FileChangeCallback callback) override
    {
        stop();
        std::error_code ec;
        root_ = fs::weakly_canonical(root, ec);
        callback_ = std::move(callback);
        queue_ = dispatch_queue_create("forma.fileevents", DISPATCH_QUEUE_SERIAL);
        CFStringRef path = CFStringCreateWithCString(nullptr, paths::toUtf8(root_).c_str(), kCFStringEncodingUTF8);
        CFArrayRef pathsToWatch = CFArrayCreate(nullptr, reinterpret_cast<const void**>(&path), 1, &kCFTypeArrayCallBacks);
        FSEventStreamContext context{0, this, nullptr, nullptr, nullptr};
        stream_ = FSEventStreamCreate(nullptr, &FsEventsWatcher::onEvents, &context, pathsToWatch,
                                      kFSEventStreamEventIdSinceNow, 0.2,
                                      kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);
        CFRelease(pathsToWatch);
        CFRelease(path);
        if (stream_ == nullptr)
            return makeError("watch.open", "Cannot create an FSEvents stream");
        FSEventStreamSetDispatchQueue(stream_, queue_);
        if (!FSEventStreamStart(stream_))
            return makeError("watch.open", "Cannot start the FSEvents stream");
        return {};
    }

    void stop() override
    {
        if (stream_ != nullptr) {
            FSEventStreamStop(stream_);
            FSEventStreamInvalidate(stream_);
            FSEventStreamRelease(stream_);
            stream_ = nullptr;
        }
        if (queue_ != nullptr) {
            dispatch_sync(queue_, ^{
                         });
            dispatch_release(queue_);
            queue_ = nullptr;
        }
    }

    const char* backendName() const override { return "FSEvents"; }

private:
    static void onEvents(ConstFSEventStreamRef, void* info, size_t count, void* eventPaths,
                         const FSEventStreamEventFlags flags[], const FSEventStreamEventId[])
    {
        auto* self = static_cast<FsEventsWatcher*>(info);
        auto** names = static_cast<char**>(eventPaths);
        std::vector<FileChange> changes;
        std::error_code ec;
        for (size_t i = 0; i < count; ++i) {
            fs::path full(names[i]);
            std::string path = paths::toGeneric(full.lexically_relative(self->root_));
            bool exists = fs::exists(full, ec);
            if (flags[i] & kFSEventStreamEventFlagItemRenamed)
                changes.push_back({exists ? FileChangeKind::Added : FileChangeKind::Removed, path, {}});
            else if (flags[i] & kFSEventStreamEventFlagItemRemoved)
                changes.push_back({FileChangeKind::Removed, path, {}});
            else if (flags[i] & kFSEventStreamEventFlagItemCreated)
                changes.push_back({FileChangeKind::Added, path, {}});
            else
                changes.push_back({FileChangeKind::Modified, path, {}});
        }
        if (!changes.empty() && self->callback_)
            self->callback_(changes);
    }

    fs::path root_;
    FileChangeCallback callback_;
    FSEventStreamRef stream_ = nullptr;
    dispatch_queue_t queue_ = nullptr;
};

#endif

}

std::unique_ptr<IFileWatcher> createFileWatcher()
{
#if defined(_WIN32)
    return std::make_unique<WindowsWatcher>();
#elif defined(__linux__)
    return std::make_unique<InotifyWatcher>();
#elif defined(__APPLE__)
    return std::make_unique<FsEventsWatcher>();
#else
    return std::make_unique<PollingWatcher>(std::chrono::seconds(2));
#endif
}

std::unique_ptr<IFileWatcher> createPollingFileWatcher(std::chrono::milliseconds interval)
{
    return std::make_unique<PollingWatcher>(interval);
}

}
