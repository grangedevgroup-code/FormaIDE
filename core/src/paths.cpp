#include "forma/core/paths.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace forma::paths {

std::string toUtf8(const fs::path& path)
{
    auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::string toGeneric(const fs::path& path)
{
    auto text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

fs::path fromUtf8(std::string_view text)
{
    return fs::path(std::u8string(text.begin(), text.end()));
}

std::optional<std::string> env(const char* name)
{
#if defined(_WIN32)
    int wideLength = MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
    std::wstring wideName(static_cast<std::size_t>(wideLength), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wideName.data(), wideLength);
    DWORD size = GetEnvironmentVariableW(wideName.c_str(), nullptr, 0);
    if (size == 0)
        return std::nullopt;
    std::wstring value(size, L'\0');
    DWORD written = GetEnvironmentVariableW(wideName.c_str(), value.data(), size);
    value.resize(written);
    return toUtf8(fs::path(value));
#else
    const char* value = std::getenv(name);
    if (value == nullptr)
        return std::nullopt;
    return std::string(value);
#endif
}

fs::path homeDir()
{
#if defined(_WIN32)
    if (auto value = env("USERPROFILE"))
        return fromUtf8(*value);
#else
    if (auto value = env("HOME"))
        return fromUtf8(*value);
#endif
    return fs::temp_directory_path();
}

fs::path userConfigDir()
{
    if (auto value = env("FORMA_CONFIG_HOME"))
        return fromUtf8(*value);
#if defined(_WIN32)
    if (auto value = env("APPDATA"))
        return fromUtf8(*value) / "FormaIDE";
    return homeDir() / "AppData" / "Roaming" / "FormaIDE";
#elif defined(__APPLE__)
    return homeDir() / "Library" / "Application Support" / "FormaIDE";
#else
    if (auto value = env("XDG_CONFIG_HOME"))
        return fromUtf8(*value) / "formaide";
    return homeDir() / ".config" / "formaide";
#endif
}

fs::path userDataDir()
{
    if (auto value = env("FORMA_DATA_HOME"))
        return fromUtf8(*value);
#if defined(_WIN32)
    if (auto value = env("LOCALAPPDATA"))
        return fromUtf8(*value) / "FormaIDE";
    return homeDir() / "AppData" / "Local" / "FormaIDE";
#elif defined(__APPLE__)
    return homeDir() / "Library" / "Application Support" / "FormaIDE";
#else
    if (auto value = env("XDG_DATA_HOME"))
        return fromUtf8(*value) / "formaide";
    return homeDir() / ".local" / "share" / "formaide";
#endif
}

fs::path userCacheDir()
{
#if defined(_WIN32)
    return userDataDir() / "Cache";
#elif defined(__APPLE__)
    return homeDir() / "Library" / "Caches" / "FormaIDE";
#else
    if (auto value = env("XDG_CACHE_HOME"))
        return fromUtf8(*value) / "formaide";
    return homeDir() / ".cache" / "formaide";
#endif
}

Result<std::string> readWholeFile(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return makeError("fs.read", "Cannot open " + toUtf8(path));
    std::string data;
    stream.seekg(0, std::ios::end);
    auto size = stream.tellg();
    if (size > 0) {
        data.resize(static_cast<std::size_t>(size));
        stream.seekg(0, std::ios::beg);
        stream.read(data.data(), size);
    }
    if (stream.bad())
        return makeError("fs.read", "Cannot read " + toUtf8(path));
    return data;
}

namespace {

std::atomic<unsigned> tempCounter{0};

#if defined(_WIN32)
int currentProcessId()
{
    return static_cast<int>(GetCurrentProcessId());
}
#else
int currentProcessId()
{
    return static_cast<int>(getpid());
}
#endif

}

Status writeFileAtomic(const fs::path& path, std::string_view data, bool executable)
{
    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
        if (ec)
            return makeError("fs.mkdir", "Cannot create " + toUtf8(path.parent_path()) + ": " + ec.message());
    }
    fs::path temp = path;
    temp += ".forma-tmp-" + std::to_string(currentProcessId()) + "-" + std::to_string(tempCounter.fetch_add(1));

#if defined(_WIN32)
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return makeError("fs.write", "Cannot create " + toUtf8(temp));
    std::size_t offset = 0;
    bool wroteAll = true;
    while (offset < data.size()) {
        DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - offset, 1u << 24));
        DWORD written = 0;
        if (!WriteFile(file, data.data() + offset, chunk, &written, nullptr) || written == 0) {
            wroteAll = false;
            break;
        }
        offset += written;
    }
    bool flushed = wroteAll && FlushFileBuffers(file);
    CloseHandle(file);
    if (!flushed) {
        DeleteFileW(temp.c_str());
        return makeError("fs.write", "Cannot write " + toUtf8(path));
    }
    for (int attempt = 0;; ++attempt) {
        if (MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            break;
        DWORD code = GetLastError();
        if ((code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION)
            && attempt < 40) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            continue;
        }
        DeleteFileW(temp.c_str());
        return makeError("fs.write", "Cannot replace " + toUtf8(path) + " (Windows error " + std::to_string(code) + ")");
    }
    (void)executable;
#else
    mode_t mode = executable ? 0755 : 0644;
    struct stat existing {};
    if (::stat(path.c_str(), &existing) == 0)
        mode = existing.st_mode & 07777;
    int file = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
    if (file < 0)
        return makeError("fs.write", "Cannot create " + toUtf8(temp));
    std::size_t offset = 0;
    bool wroteAll = true;
    while (offset < data.size()) {
        auto written = ::write(file, data.data() + offset, data.size() - offset);
        if (written <= 0) {
            wroteAll = false;
            break;
        }
        offset += static_cast<std::size_t>(written);
    }
    bool flushed = wroteAll && ::fsync(file) == 0;
    ::close(file);
    if (!flushed) {
        ::unlink(temp.c_str());
        return makeError("fs.write", "Cannot write " + toUtf8(path));
    }
    if (::rename(temp.c_str(), path.c_str()) != 0) {
        ::unlink(temp.c_str());
        return makeError("fs.write", "Cannot replace " + toUtf8(path));
    }
    if (path.has_parent_path()) {
        int directory = ::open(path.parent_path().c_str(), O_RDONLY | O_CLOEXEC);
        if (directory >= 0) {
            ::fsync(directory);
            ::close(directory);
        }
    }
#endif
    return {};
}

std::int64_t unixMillis()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string nowIso8601()
{
    auto now = std::time(nullptr);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

std::string timestampId()
{
    static std::atomic<unsigned> sequence{0};
    auto now = std::time(nullptr);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof buffer, "%Y%m%d-%H%M%S", &utc);
    char suffix[16];
    std::snprintf(suffix, sizeof suffix, "-%04u", sequence.fetch_add(1) % 10000);
    return std::string(buffer) + suffix;
}

}
