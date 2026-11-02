#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace forma::resources {

namespace detail {

struct EmbeddedFile {
    const char* path;
    const unsigned char* data;
    std::size_t size;
};

extern const EmbeddedFile embeddedFiles[];

}

std::optional<std::string_view> find(std::string_view path);
std::vector<std::string> list(std::string_view directory);

}
