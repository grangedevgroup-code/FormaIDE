#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

#include "forma/core/paths.h"

namespace forma::testing {

class TempDir {
public:
    TempDir()
    {
        static int counter = 0;
        path_ = std::filesystem::temp_directory_path()
            / ("forma-test-" + paths::timestampId() + "-" + std::to_string(++counter));
        std::filesystem::create_directories(path_);
        path_ = std::filesystem::weakly_canonical(path_);
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

inline bool waitUntil(const std::function<bool()>& condition, std::chrono::milliseconds timeout = std::chrono::seconds(10))
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return condition();
}

}
