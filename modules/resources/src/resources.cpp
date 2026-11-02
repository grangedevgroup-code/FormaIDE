#include "forma/resources/resources.h"

#include "forma/core/text.h"

namespace forma::resources {

std::optional<std::string_view> find(std::string_view path)
{
    for (const detail::EmbeddedFile* file = detail::embeddedFiles; file->path != nullptr; ++file) {
        if (path == file->path)
            return std::string_view(reinterpret_cast<const char*>(file->data), file->size);
    }
    return std::nullopt;
}

std::vector<std::string> list(std::string_view directory)
{
    std::vector<std::string> paths;
    std::string prefix(directory);
    if (!prefix.empty() && prefix.back() != '/')
        prefix.push_back('/');
    for (const detail::EmbeddedFile* file = detail::embeddedFiles; file->path != nullptr; ++file) {
        if (text::startsWith(file->path, prefix))
            paths.emplace_back(file->path);
    }
    return paths;
}

}
