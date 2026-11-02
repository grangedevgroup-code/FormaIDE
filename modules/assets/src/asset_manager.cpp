#include "forma/assets/asset_manager.h"

#include <algorithm>

#include "forma/core/hash.h"
#include "forma/core/paths.h"
#include "forma/core/text.h"
#include "forma/ir/serialization.h"

namespace fs = std::filesystem;

namespace forma::assets {

const char* assetKindForPath(std::string_view path)
{
    std::string lower = text::toLower(path);
    for (const char* suffix : {".png", ".jpg", ".jpeg", ".webp", ".gif", ".bmp"}) {
        if (text::endsWith(lower, suffix))
            return "image";
    }
    if (text::endsWith(lower, ".svg"))
        return "vector";
    for (const char* suffix : {".mp4", ".mov", ".webm", ".mkv", ".avi"}) {
        if (text::endsWith(lower, suffix))
            return "video";
    }
    for (const char* suffix : {".mp3", ".wav", ".ogg", ".flac", ".m4a", ".aac"}) {
        if (text::endsWith(lower, suffix))
            return "audio";
    }
    for (const char* suffix : {".ttf", ".otf", ".woff", ".woff2"}) {
        if (text::endsWith(lower, suffix))
            return "font";
    }
    if (text::endsWith(lower, ".ico") || text::endsWith(lower, ".icns"))
        return "icon";
    return "file";
}

namespace {

std::uint32_t big32(std::string_view data, std::size_t offset)
{
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset])) << 24) | (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 1])) << 16)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 2])) << 8) | static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 3]));
}

std::string folderForKind(const std::string& kind)
{
    if (kind == "image" || kind == "vector")
        return "assets/images";
    if (kind == "icon")
        return "assets/icons";
    if (kind == "font")
        return "assets/fonts";
    if (kind == "video" || kind == "audio")
        return "assets/media";
    return "assets/files";
}

std::string cleanStem(std::string_view input)
{
    std::string output;
    for (char ch : input) {
        auto c = static_cast<unsigned char>(ch);
        output.push_back(std::isalnum(c) ? static_cast<char>(std::tolower(c)) : '_');
    }
    while (!output.empty() && output.front() == '_')
        output.erase(0, 1);
    while (!output.empty() && output.back() == '_')
        output.pop_back();
    if (output.empty() || std::isdigit(static_cast<unsigned char>(output[0])))
        output = "asset_" + output;
    return output;
}

}

ImageInfo sniffImage(std::string_view data)
{
    ImageInfo info;
    if (data.size() > 24 && data.substr(0, 8) == "\x89PNG\r\n\x1a\n") {
        info.format = "png";
        info.width = static_cast<int>(big32(data, 16));
        info.height = static_cast<int>(big32(data, 20));
    } else if (data.size() > 10 && (data.substr(0, 6) == "GIF87a" || data.substr(0, 6) == "GIF89a")) {
        info.format = "gif";
        info.width = static_cast<unsigned char>(data[6]) | (static_cast<unsigned char>(data[7]) << 8);
        info.height = static_cast<unsigned char>(data[8]) | (static_cast<unsigned char>(data[9]) << 8);
    } else if (data.size() > 4 && static_cast<unsigned char>(data[0]) == 0xFF && static_cast<unsigned char>(data[1]) == 0xD8) {
        info.format = "jpeg";
        std::size_t i = 2;
        while (i + 9 < data.size()) {
            if (static_cast<unsigned char>(data[i]) != 0xFF) {
                ++i;
                continue;
            }
            auto marker = static_cast<unsigned char>(data[i + 1]);
            if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
                info.height = (static_cast<unsigned char>(data[i + 5]) << 8) | static_cast<unsigned char>(data[i + 6]);
                info.width = (static_cast<unsigned char>(data[i + 7]) << 8) | static_cast<unsigned char>(data[i + 8]);
                break;
            }
            int length = (static_cast<unsigned char>(data[i + 2]) << 8) | static_cast<unsigned char>(data[i + 3]);
            i += 2 + static_cast<std::size_t>(length);
        }
    } else if (data.size() > 30 && data.substr(0, 4) == "RIFF" && data.substr(8, 4) == "WEBP") {
        info.format = "webp";
    }
    return info;
}

AssetManager::AssetManager(IFilesystemService& filesystem, ir::ModelEditor& editor, const IImageCodec* codec) : filesystem_(filesystem), editor_(editor), codec_(codec) {}

Status AssetManager::updateAssets(std::vector<ir::Asset> assets)
{
    ir::ApplicationModel copy = editor_.model();
    copy.assets = std::move(assets);
    Json json = ir::applicationToJson(copy);
    return editor_.setSection("assets", json.value("assets", Json::array()));
}

Result<std::string> AssetManager::import(const fs::path& source, std::string_view folder)
{
    auto data = paths::readWholeFile(source);
    if (!data)
        return data.error();
    std::string name = paths::toUtf8(source.filename());
    std::string kind = assetKindForPath(name);
    std::string directory = folder.empty() ? folderForKind(kind) : std::string(folder);
    std::string stem = cleanStem(paths::toUtf8(source.stem()));
    std::string extension = text::toLower(paths::toUtf8(source.extension()));
    std::string id = stem;
    std::string path = directory + "/" + stem + extension;
    int counter = 2;
    auto assets = editor_.model().assets;
    auto taken = [&](const std::string& candidate) {
        return std::any_of(assets.begin(), assets.end(), [&](const ir::Asset& asset) { return asset.id == candidate; });
    };
    while (filesystem_.exists(path) || taken(id)) {
        id = stem + "_" + std::to_string(counter++);
        path = directory + "/" + id + extension;
    }
    WriteOptions options;
    if (auto status = filesystem_.writeFile(path, *data, options); !status)
        return status.error();
    ir::Asset asset;
    asset.id = id;
    asset.path = path;
    asset.kind = kind;
    asset.hash = sha256Hex(*data);
    ImageInfo info = codec_ != nullptr && kind == "image" ? codec_->inspect(*data).valueOr(sniffImage(*data)) : sniffImage(*data);
    asset.width = info.width;
    asset.height = info.height;
    assets.push_back(asset);
    if (auto status = updateAssets(std::move(assets)); !status)
        return status.error();
    return id;
}

std::vector<std::string> AssetManager::references(std::string_view id) const
{
    std::vector<std::string> output;
    const ir::Asset* asset = editor_.model().findAsset(id);
    if (asset == nullptr)
        return output;
    std::string assetId = asset->id;
    std::string assetPath = asset->path;
    auto check = [&](const ir::Component& root, const std::string& owner) {
        ir::visitComponents(root, [&](const ir::Component& component, const ir::Component*) {
            std::string source = ir::propertyString(component, "src");
            if (source == assetId || source == assetPath)
                output.push_back(owner + "/" + component.id);
        });
    };
    for (const auto& screen : editor_.model().screens)
        check(screen.root, screen.root.id);
    for (const auto& definition : editor_.model().components)
        check(definition.root, definition.root.id);
    return output;
}

Status AssetManager::rename(std::string_view id, std::string_view newName)
{
    auto assets = editor_.model().assets;
    auto entry = std::find_if(assets.begin(), assets.end(), [&](const ir::Asset& asset) { return asset.id == id; });
    if (entry == assets.end())
        return makeError("asset.missing", "No such asset");
    std::string clean = cleanStem(newName);
    if (std::any_of(assets.begin(), assets.end(), [&](const ir::Asset& asset) { return asset.id == clean; }))
        return makeError("asset.exists", "Another asset already uses that name");
    std::string oldPath = entry->path;
    std::string oldId = entry->id;
    auto slash = oldPath.find_last_of('/');
    auto dot = oldPath.find_last_of('.');
    std::string newPath = oldPath.substr(0, slash + 1) + clean + (dot != std::string::npos && dot > slash ? oldPath.substr(dot) : std::string());
    if (auto status = filesystem_.rename(oldPath, newPath); !status)
        return status;
    entry->id = clean;
    entry->path = newPath;
    ir::TransactionScope transaction(editor_, "Rename asset");
    auto fix = [&](const ir::Component& root, const std::string& owner, std::vector<std::tuple<std::string, std::string, std::string>>& updates) {
        ir::visitComponents(root, [&](const ir::Component& component, const ir::Component*) {
            std::string source = ir::propertyString(component, "src");
            if (source == oldId)
                updates.emplace_back(owner, component.id, clean);
            else if (source == oldPath)
                updates.emplace_back(owner, component.id, newPath);
        });
    };
    std::vector<std::tuple<std::string, std::string, std::string>> updates;
    for (const auto& screen : editor_.model().screens)
        fix(screen.root, screen.root.id, updates);
    for (const auto& definition : editor_.model().components)
        fix(definition.root, definition.root.id, updates);
    for (const auto& [owner, component, value] : updates) {
        if (auto status = editor_.setField(owner, component, "properties.src", value); !status)
            return status;
    }
    return updateAssets(std::move(assets));
}

Status AssetManager::replace(std::string_view id, const fs::path& source)
{
    auto assets = editor_.model().assets;
    auto entry = std::find_if(assets.begin(), assets.end(), [&](const ir::Asset& asset) { return asset.id == id; });
    if (entry == assets.end())
        return makeError("asset.missing", "No such asset");
    auto data = paths::readWholeFile(source);
    if (!data)
        return data.error();
    if (auto status = filesystem_.writeFile(entry->path, *data); !status)
        return status;
    entry->hash = sha256Hex(*data);
    ImageInfo info = sniffImage(*data);
    entry->width = info.width;
    entry->height = info.height;
    return updateAssets(std::move(assets));
}

Status AssetManager::remove(std::string_view id)
{
    auto assets = editor_.model().assets;
    auto entry = std::find_if(assets.begin(), assets.end(), [&](const ir::Asset& asset) { return asset.id == id; });
    if (entry == assets.end())
        return makeError("asset.missing", "No such asset");
    auto used = references(id);
    if (!used.empty())
        return makeError("asset.used", "This asset is still used by " + used.front());
    auto removed = filesystem_.remove(entry->path);
    if (!removed)
        return removed.error();
    assets.erase(entry);
    return updateAssets(std::move(assets));
}

Result<std::string> AssetManager::optimize(std::string_view id, int quality)
{
    if (codec_ == nullptr)
        return makeError("asset.codec", "Image conversion is not available here");
    const ir::Asset* asset = editor_.model().findAsset(id);
    if (asset == nullptr)
        return makeError("asset.missing", "No such asset");
    auto data = filesystem_.readFile(asset->path);
    if (!data)
        return data.error();
    std::string format = text::toLower(paths::toUtf8(paths::fromUtf8(asset->path).extension()));
    if (!format.empty())
        format.erase(0, 1);
    auto output = codec_->convert(*data, format, 0, 0, quality);
    if (!output)
        return output.error();
    if (output->size() >= data->size())
        return std::string("Already as small as it can get");
    std::size_t saved = data->size() - output->size();
    if (auto status = filesystem_.writeFile(asset->path, *output); !status)
        return status.error();
    auto assets = editor_.model().assets;
    for (auto& item : assets) {
        if (item.id == id)
            item.hash = sha256Hex(*output);
    }
    if (auto status = updateAssets(std::move(assets)); !status)
        return status.error();
    return "Saved " + std::to_string(saved / 1024) + " KB";
}

Result<std::string> AssetManager::convert(std::string_view id, std::string_view format)
{
    if (codec_ == nullptr)
        return makeError("asset.codec", "Image conversion is not available here");
    const ir::Asset* asset = editor_.model().findAsset(id);
    if (asset == nullptr)
        return makeError("asset.missing", "No such asset");
    auto data = filesystem_.readFile(asset->path);
    if (!data)
        return data.error();
    auto output = codec_->convert(*data, format, 0, 0, 90);
    if (!output)
        return output.error();
    std::string base = asset->path.substr(0, asset->path.find_last_of('.'));
    std::string target = base + "." + std::string(format);
    if (filesystem_.exists(target))
        return makeError("asset.exists", "A file already exists at " + target);
    if (auto status = filesystem_.writeFile(target, *output); !status)
        return status.error();
    auto assets = editor_.model().assets;
    ir::Asset created = *asset;
    created.id = asset->id + "_" + std::string(format);
    created.path = target;
    created.hash = sha256Hex(*output);
    assets.push_back(created);
    if (auto status = updateAssets(std::move(assets)); !status)
        return status.error();
    return created.id;
}

std::vector<ir::Asset> AssetManager::search(std::string_view query) const
{
    std::vector<ir::Asset> output;
    for (const auto& asset : editor_.model().assets) {
        if (query.empty() || text::containsIgnoreCase(asset.id, query) || text::containsIgnoreCase(asset.path, query) || text::containsIgnoreCase(asset.kind, query))
            output.push_back(asset);
    }
    return output;
}

Status AssetManager::rescan()
{
    auto assets = editor_.model().assets;
    bool changed = false;
    for (auto& asset : assets) {
        auto data = filesystem_.readFile(asset.path);
        if (!data)
            continue;
        std::string hash = sha256Hex(*data);
        if (hash != asset.hash) {
            asset.hash = hash;
            ImageInfo info = sniffImage(*data);
            asset.width = info.width;
            asset.height = info.height;
            changed = true;
        }
    }
    if (!filesystem_.exists("assets"))
        return changed ? updateAssets(std::move(assets)) : Status{};
    ListOptions options;
    options.recursive = true;
    auto entries = filesystem_.list("assets", options);
    if (entries) {
        for (const auto& entry : *entries) {
            if (entry.directory)
                continue;
            bool known = std::any_of(assets.begin(), assets.end(), [&](const ir::Asset& asset) { return asset.path == entry.path; });
            if (known)
                continue;
            auto data = filesystem_.readFile(entry.path);
            if (!data)
                continue;
            ir::Asset asset;
            std::string stem = paths::toUtf8(paths::fromUtf8(entry.path).stem());
            asset.id = cleanStem(stem);
            int counter = 2;
            while (std::any_of(assets.begin(), assets.end(), [&](const ir::Asset& item) { return item.id == asset.id; }))
                asset.id = cleanStem(stem) + "_" + std::to_string(counter++);
            asset.path = entry.path;
            asset.kind = assetKindForPath(entry.path);
            asset.hash = sha256Hex(*data);
            ImageInfo info = sniffImage(*data);
            asset.width = info.width;
            asset.height = info.height;
            assets.push_back(std::move(asset));
            changed = true;
        }
    }
    return changed ? updateAssets(std::move(assets)) : Status{};
}

Result<std::vector<AssetManager::PlatformFile>> AssetManager::androidIcon(std::string_view id, std::string_view resourceRoot) const
{
    if (codec_ == nullptr)
        return makeError("asset.codec", "Image conversion is not available here");
    const ir::Asset* asset = editor_.model().findAsset(id);
    if (asset == nullptr)
        return makeError("asset.missing", "No such asset");
    auto data = filesystem_.readFile(asset->path);
    if (!data)
        return data.error();
    std::vector<PlatformFile> files;
    const std::pair<const char*, int> densities[] = {{"mdpi", 48}, {"hdpi", 72}, {"xhdpi", 96}, {"xxhdpi", 144}, {"xxxhdpi", 192}};
    for (const auto& [density, size] : densities) {
        auto png = codec_->convert(*data, "png", size, size, 100);
        if (!png)
            return png.error();
        files.push_back({std::string(resourceRoot) + "/mipmap-" + density + "/ic_launcher.png", *png});
    }
    return files;
}

Result<std::vector<AssetManager::PlatformFile>> AssetManager::appleIconSet(std::string_view id, std::string_view catalogRoot) const
{
    if (codec_ == nullptr)
        return makeError("asset.codec", "Image conversion is not available here");
    const ir::Asset* asset = editor_.model().findAsset(id);
    if (asset == nullptr)
        return makeError("asset.missing", "No such asset");
    auto data = filesystem_.readFile(asset->path);
    if (!data)
        return data.error();
    auto png = codec_->convert(*data, "png", 1024, 1024, 100);
    if (!png)
        return png.error();
    std::string folder = std::string(catalogRoot) + "/AppIcon.appiconset";
    Json contents = Json{{"images", Json::array({Json{{"filename", "icon-1024.png"}, {"idiom", "universal"}, {"platform", "ios"}, {"size", "1024x1024"}}})}, {"info", {{"author", "xcode"}, {"version", 1}}}};
    return std::vector<PlatformFile>{{folder + "/icon-1024.png", *png}, {folder + "/Contents.json", dumpJson(contents)}};
}

std::string buildIco(const std::vector<std::pair<int, std::string>>& images)
{
    std::string output;
    auto put16 = [&](int value) {
        output.push_back(static_cast<char>(value & 255));
        output.push_back(static_cast<char>((value >> 8) & 255));
    };
    auto put32 = [&](std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8)
            output.push_back(static_cast<char>((value >> shift) & 255));
    };
    put16(0);
    put16(1);
    put16(static_cast<int>(images.size()));
    std::uint32_t offset = 6 + 16 * static_cast<std::uint32_t>(images.size());
    for (const auto& [size, png] : images) {
        output.push_back(static_cast<char>(size >= 256 ? 0 : size));
        output.push_back(static_cast<char>(size >= 256 ? 0 : size));
        output.push_back(0);
        output.push_back(0);
        put16(1);
        put16(32);
        put32(static_cast<std::uint32_t>(png.size()));
        put32(offset);
        offset += static_cast<std::uint32_t>(png.size());
    }
    for (const auto& [size, png] : images)
        output += png;
    return output;
}

Result<std::vector<AssetManager::PlatformFile>> AssetManager::desktopIcons(std::string_view id, std::string_view root) const
{
    if (codec_ == nullptr)
        return makeError("asset.codec", "Image conversion is not available here");
    const ir::Asset* asset = editor_.model().findAsset(id);
    if (asset == nullptr)
        return makeError("asset.missing", "No such asset");
    auto data = filesystem_.readFile(asset->path);
    if (!data)
        return data.error();
    std::vector<PlatformFile> files;
    std::vector<std::pair<int, std::string>> icoImages;
    for (int size : {16, 32, 48, 256}) {
        auto png = codec_->convert(*data, "png", size, size, 100);
        if (!png)
            return png.error();
        icoImages.emplace_back(size, *png);
        files.push_back({std::string(root) + "/icon-" + std::to_string(size) + ".png", *png});
    }
    files.push_back({std::string(root) + "/app.ico", buildIco(icoImages)});
    return files;
}

}
