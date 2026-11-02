#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "forma/codegen/generator.h"
#include "forma/core/filesystem_service.h"
#include "forma/ir/editor.h"

namespace forma::codegen {

enum class FileOutcome { Created, Updated, Unchanged, Merged, Conflict, KeptUserFile, Recreated, RemovedObsolete, KeptObsolete };

const char* fileOutcomeName(FileOutcome outcome);

struct FileReport {
    std::string path;
    FileOutcome outcome = FileOutcome::Unchanged;
    FileCategory category = FileCategory::Generated;
    std::string detail;
    std::string mergedPreview;
};

struct GenerationReport {
    std::string generator;
    std::vector<FileReport> files;
    std::vector<ir::Diagnostic> diagnostics;
    bool blockedByErrors = false;
    bool dryRun = false;
    int count(FileOutcome outcome) const;
    bool hasConflicts() const { return count(FileOutcome::Conflict) > 0; }
    bool filesChanged() const;
};

struct ManifestFile {
    std::string path;
    std::string hash;
    FileCategory category = FileCategory::Generated;
    std::vector<SourceBinding> bindings;
    std::vector<ComponentAnchor> anchors;
};

struct Manifest {
    std::string target;
    std::string generator;
    std::map<std::string, ManifestFile> files;
};

std::string targetDirectory(std::string_view target);
Result<Manifest> loadManifest(const IFilesystemService& filesystem, std::string_view target);
Status saveManifest(IFilesystemService& filesystem, const Manifest& manifest);

struct GenerationOptions {
    std::string target = "main";
    std::string outputDirectory = ".";
    bool dryRun = false;
};

GenerationReport runGeneration(IFilesystemService& filesystem, const ICodeGenerator& generator, const ir::ApplicationModel& model,
                               const ProjectConfiguration& config, const GenerationOptions& options);

enum class ConflictChoice { KeepMine, TakeGenerated, WriteMarkers };
Status resolveConflict(IFilesystemService& filesystem, const ICodeGenerator& generator, const ir::ApplicationModel& model,
                       const ProjectConfiguration& config, const GenerationOptions& options, std::string_view path, ConflictChoice choice);

struct PropertyUpdate {
    std::string screen;
    std::string component;
    std::string key;
    Json value;
    std::string file;
    int line = 0;
    std::string note;
};

struct UnmappedEdit {
    std::string file;
    int startLine = 0;
    int endLine = 0;
    std::string screen;
    std::string component;
};

struct SyncConflict {
    std::string screen;
    std::string component;
    std::string key;
    Json designValue;
    Json codeValue;
    std::string file;
    int line = 0;
};

struct SyncAnalysis {
    std::vector<PropertyUpdate> updates;
    std::vector<UnmappedEdit> unmapped;
    std::vector<SyncConflict> conflicts;
    std::vector<std::string> changedFiles;
    bool anythingChanged() const { return !changedFiles.empty(); }
};

SyncAnalysis analyzeCodeChanges(const IFilesystemService& filesystem, const ir::ApplicationModel& model, std::string_view target);
Status applyCodeChanges(ir::ModelEditor& editor, const SyncAnalysis& analysis);

struct SourceLocation {
    std::string file;
    int line = 0;
    int endLine = 0;
};

std::optional<SourceLocation> locateComponent(const IFilesystemService& filesystem, std::string_view target, std::string_view screen, std::string_view component);

struct ComponentReference {
    std::string screen;
    std::string component;
    std::string key;
};

std::optional<ComponentReference> componentAtLine(const IFilesystemService& filesystem, std::string_view target, std::string_view file, int line);
std::vector<std::string> generatedFiles(const IFilesystemService& filesystem, std::string_view target, FileCategory category);

}
