#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "forma/core/filesystem_service.h"
#include "forma/ir/editor.h"

namespace forma::assets {

struct ImageInfo {
    int width = 0;
    int height = 0;
    std::string format;
};

class IImageCodec {
public:
    virtual ~IImageCodec() = default;
    virtual Result<ImageInfo> inspect(std::string_view data) const = 0;
    virtual Result<std::string> convert(std::string_view data, std::string_view format, int width, int height, int quality) const = 0;
};

const char* assetKindForPath(std::string_view path);
ImageInfo sniffImage(std::string_view data);

class AssetManager {
public:
    AssetManager(IFilesystemService& filesystem, ir::ModelEditor& editor, const IImageCodec* codec = nullptr);

    Result<std::string> import(const std::filesystem::path& source, std::string_view folder = {});
    Status rename(std::string_view id, std::string_view newName);
    Status replace(std::string_view id, const std::filesystem::path& source);
    Status remove(std::string_view id);
    Result<std::string> optimize(std::string_view id, int quality = 85);
    Result<std::string> convert(std::string_view id, std::string_view format);

    std::vector<ir::Asset> search(std::string_view query) const;
    std::vector<std::string> references(std::string_view id) const;
    Status rescan();

    struct PlatformFile {
        std::string path;
        std::string content;
    };

    Result<std::vector<PlatformFile>> androidIcon(std::string_view id, std::string_view resourceRoot) const;
    Result<std::vector<PlatformFile>> appleIconSet(std::string_view id, std::string_view catalogRoot) const;
    Result<std::vector<PlatformFile>> desktopIcons(std::string_view id, std::string_view root) const;

private:
    Status updateAssets(std::vector<ir::Asset> assets);

    IFilesystemService& filesystem_;
    ir::ModelEditor& editor_;
    const IImageCodec* codec_;
};

std::string buildIco(const std::vector<std::pair<int, std::string>>& pngImages);

}
