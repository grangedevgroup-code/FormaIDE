#include "forma/codegen/pipeline.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "forma/codegen/code_writer.h"
#include "forma/codegen/text_diff.h"
#include "forma/core/hash.h"
#include "forma/core/log.h"
#include "forma/core/text.h"
#include "forma/ir/serialization.h"

namespace forma::codegen {

const char* fileOutcomeName(FileOutcome outcome)
{
    switch (outcome) {
    case FileOutcome::Created:
        return "created";
    case FileOutcome::Updated:
        return "updated";
    case FileOutcome::Unchanged:
        return "unchanged";
    case FileOutcome::Merged:
        return "merged with your edits";
    case FileOutcome::Conflict:
        return "conflict";
    case FileOutcome::KeptUserFile:
        return "kept your file";
    case FileOutcome::Recreated:
        return "recreated";
    case FileOutcome::RemovedObsolete:
        return "removed, no longer generated";
    case FileOutcome::KeptObsolete:
        return "kept, no longer generated";
    }
    return "unchanged";
}

int GenerationReport::count(FileOutcome outcome) const
{
    return static_cast<int>(std::count_if(files.begin(), files.end(), [&](const FileReport& file) { return file.outcome == outcome; }));
}

bool GenerationReport::filesChanged() const
{
    return std::any_of(files.begin(), files.end(), [](const FileReport& file) {
        return file.outcome != FileOutcome::Unchanged && file.outcome != FileOutcome::KeptUserFile && file.outcome != FileOutcome::Conflict
            && file.outcome != FileOutcome::KeptObsolete;
    });
}

std::string targetDirectory(std::string_view target)
{
    return ".forma/generated/" + text::toIdentifier(target);
}

namespace {

std::string basePath(std::string_view target, std::string_view file)
{
    return targetDirectory(target) + "/base/" + normalizeRelativePath(file);
}

std::string joinOutput(std::string_view directory, std::string_view path)
{
    std::string base = normalizeRelativePath(directory);
    if (base.empty() || base == ".")
        return normalizeRelativePath(path);
    return base + "/" + normalizeRelativePath(path);
}

const char* valueKindName(ValueKind kind)
{
    switch (kind) {
    case ValueKind::String:
        return "string";
    case ValueKind::Number:
        return "number";
    case ValueKind::Boolean:
        return "boolean";
    case ValueKind::Color:
        return "color";
    }
    return "string";
}

ValueKind valueKindFromName(const std::string& name)
{
    if (name == "number")
        return ValueKind::Number;
    if (name == "boolean")
        return ValueKind::Boolean;
    if (name == "color")
        return ValueKind::Color;
    return ValueKind::String;
}

const char* literalStyleName(LiteralStyle style)
{
    switch (style) {
    case LiteralStyle::CLike:
        return "clike";
    case LiteralStyle::Xml:
        return "xml";
    case LiteralStyle::Html:
        return "html";
    case LiteralStyle::Python:
        return "python";
    case LiteralStyle::Raw:
        return "raw";
    }
    return "clike";
}

LiteralStyle literalStyleFromName(const std::string& name)
{
    if (name == "xml")
        return LiteralStyle::Xml;
    if (name == "html")
        return LiteralStyle::Html;
    if (name == "python")
        return LiteralStyle::Python;
    if (name == "raw")
        return LiteralStyle::Raw;
    return LiteralStyle::CLike;
}

Json manifestToJson(const Manifest& manifest)
{
    Json files = Json::object();
    for (const auto& [path, file] : manifest.files) {
        Json entry = Json::object();
        entry["hash"] = file.hash;
        entry["category"] = fileCategoryName(file.category);
        if (!file.bindings.empty()) {
            Json bindings = Json::array();
            for (const auto& binding : file.bindings) {
                bindings.push_back(Json{{"line", binding.line}, {"screen", binding.screen}, {"component", binding.component},
                                        {"key", binding.key}, {"prefix", binding.prefix}, {"suffix", binding.suffix},
                                        {"kind", valueKindName(binding.kind)}, {"style", literalStyleName(binding.style)}});
            }
            entry["bindings"] = bindings;
        }
        if (!file.anchors.empty()) {
            Json anchors = Json::array();
            for (const auto& anchor : file.anchors)
                anchors.push_back(Json{{"screen", anchor.screen}, {"component", anchor.component}, {"start", anchor.startLine}, {"end", anchor.endLine}});
            entry["anchors"] = anchors;
        }
        files[path] = entry;
    }
    Json json = Json::object();
    json["formaManifest"] = 1;
    json["target"] = manifest.target;
    json["generator"] = manifest.generator;
    json["files"] = files;
    return json;
}

Manifest manifestFromJson(const Json& json)
{
    Manifest manifest;
    manifest.target = jsonString(json, "target");
    manifest.generator = jsonString(json, "generator");
    auto files = json.find("files");
    if (files == json.end() || !files->is_object())
        return manifest;
    for (auto item = files->begin(); item != files->end(); ++item) {
        ManifestFile file;
        file.path = item.key();
        file.hash = jsonString(*item, "hash");
        file.category = fileCategoryFromName(jsonString(*item, "category"));
        auto bindings = item->find("bindings");
        if (bindings != item->end() && bindings->is_array()) {
            for (const auto& entry : *bindings) {
                SourceBinding binding;
                binding.line = jsonInt(entry, "line");
                binding.screen = jsonString(entry, "screen");
                binding.component = jsonString(entry, "component");
                binding.key = jsonString(entry, "key");
                binding.prefix = jsonString(entry, "prefix");
                binding.suffix = jsonString(entry, "suffix");
                binding.kind = valueKindFromName(jsonString(entry, "kind"));
                binding.style = literalStyleFromName(jsonString(entry, "style"));
                file.bindings.push_back(std::move(binding));
            }
        }
        auto anchors = item->find("anchors");
        if (anchors != item->end() && anchors->is_array()) {
            for (const auto& entry : *anchors) {
                ComponentAnchor anchor;
                anchor.screen = jsonString(entry, "screen");
                anchor.component = jsonString(entry, "component");
                anchor.startLine = jsonInt(entry, "start");
                anchor.endLine = jsonInt(entry, "end");
                file.anchors.push_back(std::move(anchor));
            }
        }
        manifest.files[item.key()] = std::move(file);
    }
    return manifest;
}

std::string normalizeNewlines(std::string_view text)
{
    std::string output;
    output.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
            continue;
        output.push_back(text[i]);
    }
    return output;
}

Status writeBase(IFilesystemService& filesystem, std::string_view target, std::string_view file, std::string_view content)
{
    WriteOptions options;
    options.origin = Origin::System;
    options.backup = false;
    return filesystem.writeFile(basePath(target, file), content, options);
}

}

Result<Manifest> loadManifest(const IFilesystemService& filesystem, std::string_view target)
{
    std::string path = targetDirectory(target) + "/manifest.json";
    if (!filesystem.exists(path)) {
        Manifest empty;
        empty.target = std::string(target);
        return empty;
    }
    auto content = filesystem.readFile(path);
    if (!content)
        return content.error();
    auto parsed = parseJson(*content);
    if (!parsed)
        return makeError("manifest.parse", path + " is damaged: " + parsed.error().message);
    Manifest manifest = manifestFromJson(*parsed);
    manifest.target = std::string(target);
    return manifest;
}

Status saveManifest(IFilesystemService& filesystem, const Manifest& manifest)
{
    WriteOptions options;
    options.origin = Origin::System;
    options.backup = false;
    return filesystem.writeFile(targetDirectory(manifest.target) + "/manifest.json", dumpJson(manifestToJson(manifest)), options);
}

GenerationReport runGeneration(IFilesystemService& filesystem, const ICodeGenerator& generator, const ir::ApplicationModel& model,
                               const ProjectConfiguration& config, const GenerationOptions& options)
{
    GenerationReport report;
    report.generator = generator.info().id;
    report.dryRun = options.dryRun;
    report.diagnostics = ir::validateModel(model);
    auto extra = generator.validate(model);
    report.diagnostics.insert(report.diagnostics.end(), extra.begin(), extra.end());
    if (ir::hasErrors(report.diagnostics)) {
        report.blockedByErrors = true;
        return report;
    }

    auto loaded = loadManifest(filesystem, options.target);
    if (!loaded) {
        report.blockedByErrors = true;
        report.diagnostics.push_back({ir::Severity::Error, "manifest.parse", loaded.error().message, {}, {}, {}});
        return report;
    }
    Manifest manifest = std::move(loaded.value());
    manifest.generator = generator.info().id;

    GenerationResult generated = generator.generate(model, config);
    report.diagnostics.insert(report.diagnostics.end(), generated.diagnostics.begin(), generated.diagnostics.end());
    if (ir::hasErrors(generated.diagnostics)) {
        report.blockedByErrors = true;
        return report;
    }

    std::map<std::string, ManifestFile> nextFiles;
    WriteOptions writeOptions;
    writeOptions.origin = Origin::Generator;

    auto recordManifest = [&](const GeneratedFile& file, const std::string& path, const std::string& hash) {
        ManifestFile entry;
        entry.path = path;
        entry.hash = hash;
        entry.category = file.category;
        entry.bindings = file.bindings;
        entry.anchors = file.anchors;
        nextFiles[path] = std::move(entry);
    };

    for (const auto& file : generated.files) {
        std::string path = joinOutput(options.outputDirectory, file.path);
        FileReport item;
        item.path = path;
        item.category = file.category;
        std::string newContent = normalizeNewlines(file.content);
        std::string newHash = sha256Hex(newContent);
        auto existingEntry = manifest.files.find(path);
        bool existsOnDisk = filesystem.exists(path);
        WriteOptions fileOptions = writeOptions;
        fileOptions.executable = file.executable;

        if (file.category == FileCategory::User) {
            if (existsOnDisk) {
                item.outcome = FileOutcome::KeptUserFile;
                if (existingEntry != manifest.files.end())
                    nextFiles[path] = existingEntry->second;
                else
                    recordManifest(file, path, newHash);
            } else if (existingEntry != manifest.files.end()) {
                item.outcome = FileOutcome::KeptUserFile;
                item.detail = "You deleted this file, so it stays deleted";
                nextFiles[path] = existingEntry->second;
            } else {
                if (!options.dryRun) {
                    if (auto status = filesystem.writeFile(path, newContent, fileOptions); !status) {
                        item.outcome = FileOutcome::Conflict;
                        item.detail = status.error().message;
                        report.files.push_back(std::move(item));
                        continue;
                    }
                }
                item.outcome = FileOutcome::Created;
                recordManifest(file, path, newHash);
            }
            report.files.push_back(std::move(item));
            continue;
        }

        if (!existsOnDisk) {
            item.outcome = existingEntry != manifest.files.end() ? FileOutcome::Recreated : FileOutcome::Created;
            if (!options.dryRun) {
                if (auto status = filesystem.writeFile(path, newContent, fileOptions); !status) {
                    item.outcome = FileOutcome::Conflict;
                    item.detail = status.error().message;
                    report.files.push_back(std::move(item));
                    continue;
                }
                auto status = writeBase(filesystem, options.target, path, newContent);
                (void)status;
            }
            recordManifest(file, path, newHash);
            report.files.push_back(std::move(item));
            continue;
        }

        auto disk = filesystem.readFile(path);
        if (!disk) {
            item.outcome = FileOutcome::Conflict;
            item.detail = disk.error().message;
            report.files.push_back(std::move(item));
            continue;
        }
        std::string diskContent = normalizeNewlines(*disk);
        std::string diskHash = sha256Hex(diskContent);

        if (diskHash == newHash) {
            item.outcome = FileOutcome::Unchanged;
            if (!options.dryRun) {
                auto status = writeBase(filesystem, options.target, path, newContent);
                (void)status;
            }
            recordManifest(file, path, newHash);
            report.files.push_back(std::move(item));
            continue;
        }

        if (existingEntry == manifest.files.end()) {
            item.outcome = FileOutcome::Conflict;
            item.detail = "A file that FormaIDE did not create already exists here";
            report.files.push_back(std::move(item));
            continue;
        }

        if (diskHash == existingEntry->second.hash) {
            item.outcome = FileOutcome::Updated;
            if (!options.dryRun) {
                WriteOptions guarded = fileOptions;
                guarded.expectedHash = sha256Hex(*disk);
                if (auto status = filesystem.writeFile(path, newContent, guarded); !status) {
                    item.outcome = FileOutcome::Conflict;
                    item.detail = status.error().message;
                    nextFiles[path] = existingEntry->second;
                    report.files.push_back(std::move(item));
                    continue;
                }
                auto status = writeBase(filesystem, options.target, path, newContent);
                (void)status;
            }
            recordManifest(file, path, newHash);
            report.files.push_back(std::move(item));
            continue;
        }

        auto base = filesystem.readFile(basePath(options.target, path));
        if (!base) {
            item.outcome = FileOutcome::Conflict;
            item.detail = "You edited this file and the earlier generated copy is missing, so it can't be merged safely";
            nextFiles[path] = existingEntry->second;
            report.files.push_back(std::move(item));
            continue;
        }
        MergeResult merged = mergeThreeWay(*base, diskContent, newContent);
        if (!merged.clean()) {
            item.outcome = FileOutcome::Conflict;
            item.detail = "Your edits overlap generated changes in " + std::to_string(merged.conflicts) + " place(s)";
            item.mergedPreview = merged.text;
            nextFiles[path] = existingEntry->second;
            report.files.push_back(std::move(item));
            continue;
        }
        item.outcome = merged.text == diskContent ? FileOutcome::Unchanged : FileOutcome::Merged;
        if (!options.dryRun) {
            if (merged.text != diskContent) {
                WriteOptions guarded = fileOptions;
                guarded.expectedHash = sha256Hex(*disk);
                if (auto status = filesystem.writeFile(path, merged.text, guarded); !status) {
                    item.outcome = FileOutcome::Conflict;
                    item.detail = status.error().message;
                    nextFiles[path] = existingEntry->second;
                    report.files.push_back(std::move(item));
                    continue;
                }
            }
            auto status = writeBase(filesystem, options.target, path, newContent);
            (void)status;
        }
        recordManifest(file, path, newHash);
        report.files.push_back(std::move(item));
    }

    for (const auto& [path, entry] : manifest.files) {
        if (nextFiles.count(path) != 0)
            continue;
        auto stillGenerated = std::any_of(generated.files.begin(), generated.files.end(), [&, p = path](const GeneratedFile& file) {
            return joinOutput(options.outputDirectory, file.path) == p;
        });
        if (stillGenerated) {
            nextFiles[path] = entry;
            continue;
        }
        FileReport item;
        item.path = path;
        item.category = entry.category;
        if (entry.category == FileCategory::User || !filesystem.exists(path)) {
            if (filesystem.exists(path)) {
                item.outcome = FileOutcome::KeptObsolete;
                item.detail = "This file belongs to you and is no longer used by the generated code";
                report.files.push_back(std::move(item));
            }
            continue;
        }
        auto diskHash = filesystem.fileHash(path);
        auto disk = filesystem.readFile(path);
        bool unmodified = disk && sha256Hex(normalizeNewlines(*disk)) == entry.hash;
        (void)diskHash;
        if (unmodified) {
            item.outcome = FileOutcome::RemovedObsolete;
            if (!options.dryRun) {
                auto removed = filesystem.remove(path, Origin::Generator);
                if (removed)
                    item.detail = "Moved to the project's trash: " + *removed;
            }
        } else {
            item.outcome = FileOutcome::KeptObsolete;
            item.detail = "You edited this file, so it was kept even though it is no longer generated";
        }
        report.files.push_back(std::move(item));
    }

    if (!options.dryRun) {
        manifest.files = std::move(nextFiles);
        if (auto status = saveManifest(filesystem, manifest); !status) {
            report.diagnostics.push_back({ir::Severity::Error, "manifest.write", status.error().message, {}, {}, {}});
        }
    }
    log::info("codegen", "generated", Json{{"generator", report.generator}, {"files", report.files.size()}, {"conflicts", report.count(FileOutcome::Conflict)}});
    return report;
}

Status resolveConflict(IFilesystemService& filesystem, const ICodeGenerator& generator, const ir::ApplicationModel& model,
                       const ProjectConfiguration& config, const GenerationOptions& options, std::string_view path, ConflictChoice choice)
{
    GenerationResult generated = generator.generate(model, config);
    auto file = std::find_if(generated.files.begin(), generated.files.end(), [&](const GeneratedFile& item) {
        return joinOutput(options.outputDirectory, item.path) == normalizeRelativePath(path);
    });
    if (file == generated.files.end())
        return makeError("codegen.conflict", "The generator no longer produces " + std::string(path));
    auto loaded = loadManifest(filesystem, options.target);
    if (!loaded)
        return loaded.error();
    std::string newContent = normalizeNewlines(file->content);
    std::string normalized = normalizeRelativePath(path);
    WriteOptions writeOptions;
    writeOptions.origin = Origin::Generator;
    if (choice == ConflictChoice::TakeGenerated) {
        if (auto status = filesystem.writeFile(path, newContent, writeOptions); !status)
            return status;
    } else if (choice == ConflictChoice::WriteMarkers) {
        auto disk = filesystem.readFile(path);
        if (!disk)
            return disk.error();
        auto base = filesystem.readFile(basePath(options.target, normalized));
        std::string baseContent = base ? *base : std::string();
        MergeResult merged = mergeThreeWay(baseContent, normalizeNewlines(*disk), newContent);
        if (auto status = filesystem.writeFile(path, merged.text, writeOptions); !status)
            return status;
    }
    if (auto status = writeBase(filesystem, options.target, normalized, newContent); !status)
        return status;
    ManifestFile entry;
    entry.path = normalized;
    entry.hash = sha256Hex(newContent);
    entry.category = file->category;
    entry.bindings = file->bindings;
    entry.anchors = file->anchors;
    loaded->files[normalized] = std::move(entry);
    loaded->generator = generator.info().id;
    return saveManifest(filesystem, *loaded);
}

namespace {

std::optional<Json> decodeValue(const SourceBinding& binding, std::string_view literal)
{
    switch (binding.kind) {
    case ValueKind::String: {
        auto decoded = unescapeLiteral(literal, binding.style);
        if (!decoded)
            return std::nullopt;
        return Json(*decoded);
    }
    case ValueKind::Number: {
        std::string value = text::trim(literal);
        if (value.empty())
            return std::nullopt;
        try {
            std::size_t consumed = 0;
            double number = std::stod(value, &consumed);
            if (consumed != value.size())
                return std::nullopt;
            return Json(number);
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
    case ValueKind::Boolean: {
        std::string value = text::toLower(text::trim(literal));
        if (value == "true")
            return Json(true);
        if (value == "false")
            return Json(false);
        return std::nullopt;
    }
    case ValueKind::Color: {
        std::string value = text::trim(literal);
        if (value.size() >= 2 && value[0] == '#' && std::all_of(value.begin() + 1, value.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }))
            return Json(value);
        return std::nullopt;
    }
    }
    return std::nullopt;
}

std::optional<Json> parseBoundLine(const SourceBinding& binding, const std::string& line)
{
    std::string trimmed = text::trim(line);
    std::string prefix = text::trim(binding.prefix);
    std::string suffix = text::trim(binding.suffix);
    if (!text::startsWith(trimmed, prefix) || trimmed.size() < prefix.size() + suffix.size() || !text::endsWith(trimmed, suffix))
        return std::nullopt;
    return decodeValue(binding, std::string_view(trimmed).substr(prefix.size(), trimmed.size() - prefix.size() - suffix.size()));
}

Json currentModelValue(const ir::ApplicationModel& model, const SourceBinding& binding)
{
    const ir::Component* root = nullptr;
    if (const ir::Screen* screen = model.findScreen(binding.screen))
        root = &screen->root;
    else if (const ir::ComponentDefinition* definition = model.findDefinition(binding.screen))
        root = &definition->root;
    if (root == nullptr)
        return nullptr;
    const ir::Component* component = ir::findComponent(*root, binding.component);
    if (component == nullptr)
        return nullptr;
    return ir::getComponentField(*component, binding.key);
}

bool valuesMatch(const Json& a, const Json& b)
{
    if (a.is_number() && b.is_number())
        return std::abs(a.get<double>() - b.get<double>()) < 1e-6;
    return a == b;
}

}

SyncAnalysis analyzeCodeChanges(const IFilesystemService& filesystem, const ir::ApplicationModel& model, std::string_view target)
{
    SyncAnalysis analysis;
    auto manifest = loadManifest(filesystem, target);
    if (!manifest)
        return analysis;
    for (const auto& [path, file] : manifest->files) {
        if (file.category == FileCategory::User || !filesystem.exists(path))
            continue;
        auto disk = filesystem.readFile(path);
        if (!disk)
            continue;
        std::string diskContent = normalizeNewlines(*disk);
        if (sha256Hex(diskContent) == file.hash)
            continue;
        auto base = filesystem.readFile(basePath(target, path));
        if (!base)
            continue;
        analysis.changedFiles.push_back(path);
        auto baseLines = text::splitLines(*base);
        auto diskLines = text::splitLines(diskContent);
        auto hunks = diffLines(baseLines, diskLines);
        for (const auto& hunk : hunks) {
            bool oneToOne = hunk.baseCount == hunk.otherCount && hunk.baseCount > 0;
            std::vector<int> unresolved;
            for (int offset = 0; offset < std::max(hunk.baseCount, hunk.otherCount); ++offset)
                unresolved.push_back(offset);
            if (oneToOne) {
                for (int offset = 0; offset < hunk.baseCount; ++offset) {
                    int baseLineNumber = hunk.baseStart + offset + 1;
                    auto binding = std::find_if(file.bindings.begin(), file.bindings.end(), [&](const SourceBinding& item) { return item.line == baseLineNumber; });
                    if (binding == file.bindings.end())
                        continue;
                    const std::string& edited = diskLines[static_cast<std::size_t>(hunk.otherStart + offset)];
                    auto value = parseBoundLine(*binding, edited);
                    if (!value)
                        continue;
                    auto baseValue = parseBoundLine(*binding, baseLines[static_cast<std::size_t>(hunk.baseStart + offset)]);
                    Json current = currentModelValue(model, *binding);
                    bool designChanged = baseValue && !current.is_null() && !valuesMatch(current, *baseValue);
                    if (designChanged && !valuesMatch(current, *value)) {
                        analysis.conflicts.push_back({binding->screen, binding->component, binding->key, current, *value, path, hunk.otherStart + offset + 1});
                    } else if (current.is_null() || !valuesMatch(current, *value)) {
                        PropertyUpdate update;
                        update.screen = binding->screen;
                        update.component = binding->component;
                        update.key = binding->key;
                        update.value = *value;
                        update.file = path;
                        update.line = hunk.otherStart + offset + 1;
                        analysis.updates.push_back(std::move(update));
                    }
                    unresolved.erase(std::remove(unresolved.begin(), unresolved.end(), offset), unresolved.end());
                }
            }
            if (!unresolved.empty()) {
                UnmappedEdit edit;
                edit.file = path;
                edit.startLine = hunk.otherStart + 1;
                edit.endLine = hunk.otherStart + std::max(1, hunk.otherCount);
                int baseLine = hunk.baseStart + 1;
                const ComponentAnchor* best = nullptr;
                for (const auto& anchor : file.anchors) {
                    if (baseLine >= anchor.startLine && baseLine <= anchor.endLine) {
                        if (best == nullptr || anchor.endLine - anchor.startLine < best->endLine - best->startLine)
                            best = &anchor;
                    }
                }
                if (best != nullptr) {
                    edit.screen = best->screen;
                    edit.component = best->component;
                }
                analysis.unmapped.push_back(std::move(edit));
            }
        }
    }
    return analysis;
}

Status applyCodeChanges(ir::ModelEditor& editor, const SyncAnalysis& analysis)
{
    if (analysis.updates.empty() && analysis.unmapped.empty())
        return {};
    ir::TransactionScope transaction(editor, "Update design from code");
    for (const auto& update : analysis.updates) {
        if (auto status = editor.setField(update.screen, update.component, update.key, update.value); !status)
            return status;
    }
    for (const auto& edit : analysis.unmapped) {
        if (edit.component.empty() || edit.screen.empty())
            continue;
        const ir::Screen* screen = editor.model().findScreen(edit.screen);
        if (screen == nullptr)
            continue;
        const ir::Component* component = ir::findComponent(screen->root, edit.component);
        if (component == nullptr || component->codeManaged == "partial")
            continue;
        if (auto status = editor.setField(edit.screen, edit.component, "codeManaged", "partial"); !status)
            return status;
    }
    return {};
}

std::optional<SourceLocation> locateComponent(const IFilesystemService& filesystem, std::string_view target, std::string_view screen, std::string_view component)
{
    auto manifest = loadManifest(filesystem, target);
    if (!manifest)
        return std::nullopt;
    for (const auto& [path, file] : manifest->files) {
        for (const auto& anchor : file.anchors) {
            if (anchor.screen != screen || anchor.component != component)
                continue;
            SourceLocation location{path, anchor.startLine, anchor.endLine};
            auto disk = filesystem.readFile(path);
            auto base = filesystem.readFile(basePath(target, path));
            if (disk && base) {
                auto hunks = diffLines(text::splitLines(*base), text::splitLines(*disk));
                int mappedStart = mapLineThroughDiff(hunks, anchor.startLine - 1);
                int mappedEnd = mapLineThroughDiff(hunks, anchor.endLine - 1);
                if (mappedStart >= 0)
                    location.line = mappedStart + 1;
                if (mappedEnd >= 0)
                    location.endLine = mappedEnd + 1;
            }
            return location;
        }
    }
    return std::nullopt;
}

std::optional<ComponentReference> componentAtLine(const IFilesystemService& filesystem, std::string_view target, std::string_view file, int line)
{
    auto manifest = loadManifest(filesystem, target);
    if (!manifest)
        return std::nullopt;
    auto it = manifest->files.find(normalizeRelativePath(file));
    if (it == manifest->files.end())
        return std::nullopt;
    int baseLine = line;
    auto disk = filesystem.readFile(file);
    auto base = filesystem.readFile(basePath(target, file));
    if (disk && base) {
        auto hunks = diffLines(text::splitLines(*disk), text::splitLines(*base));
        int mapped = mapLineThroughDiff(hunks, line - 1);
        if (mapped >= 0)
            baseLine = mapped + 1;
    }
    for (const auto& binding : it->second.bindings) {
        if (binding.line == baseLine)
            return ComponentReference{binding.screen, binding.component, binding.key};
    }
    const ComponentAnchor* best = nullptr;
    for (const auto& anchor : it->second.anchors) {
        if (baseLine >= anchor.startLine && baseLine <= anchor.endLine) {
            if (best == nullptr || anchor.endLine - anchor.startLine < best->endLine - best->startLine)
                best = &anchor;
        }
    }
    if (best == nullptr)
        return std::nullopt;
    return ComponentReference{best->screen, best->component, {}};
}

std::vector<std::string> generatedFiles(const IFilesystemService& filesystem, std::string_view target, FileCategory category)
{
    std::vector<std::string> output;
    auto manifest = loadManifest(filesystem, target);
    if (!manifest)
        return output;
    for (const auto& [path, file] : manifest->files) {
        if (file.category == category)
            output.push_back(path);
    }
    return output;
}

}
