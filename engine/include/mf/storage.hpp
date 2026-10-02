// Project storage: atomic checkpoints, continuous journal, crash recovery, rolling versions, migrations.
#pragma once
#include "common.hpp"

namespace mf {

bool atomicWriteFile(const std::string& path, const std::string& data, std::string* err = nullptr);
uint32_t crc32Of(const std::string& s);

struct ProjectSummary {
    std::string id, name, path, thumbnail;
    double modified = 0, duration = 0, fps = 0;
    int width = 0, height = 0;
    int64_t bytes = 0;
    bool needsRecovery = false;
    double recoveryTime = 0;
    json toJson() const;
};

struct RecoveryInfo {
    bool available = false;
    bool abnormalExit = false;
    int journalEntries = 0;
    int corruptEntries = 0;
    double checkpointTime = 0, recoveredTime = 0;
    std::string message;
};

class ProjectStore {
   public:
    explicit ProjectStore(std::string root);
    const std::string& root() const { return root_; }
    std::string projectDir(const std::string& id) const { return pathJoin(root_, id); }
    std::vector<ProjectSummary> list() const;
    std::string create(const json& doc, std::string* err = nullptr);
    bool load(const std::string& id, json& out, std::string& err, std::vector<std::string>* migrationLog = nullptr);
    // Atomic checkpoint: current -> prev, write new current, truncate journal, add rolling version.
    bool save(const std::string& id, const json& doc, std::string& err, bool addVersion = false);
    bool appendJournal(const std::string& id, uint64_t seq, const std::string& label, const json& patch);
    RecoveryInfo checkRecovery(const std::string& id) const;
    // Rebuild the newest valid state from checkpoint + journal (does not overwrite files).
    bool recover(const std::string& id, json& out, RecoveryInfo* info = nullptr) const;
    void markOpen(const std::string& id);
    void markClosed(const std::string& id);
    std::string duplicate(const std::string& id, const std::string& newName, std::string* err = nullptr);
    bool rename(const std::string& id, const std::string& name);
    bool remove(const std::string& id);  // moves to .trash (recoverable)
    std::vector<json> versions(const std::string& id) const;
    bool loadVersion(const std::string& id, const std::string& version, json& out, std::string& err) const;
    int64_t footprint(const std::string& id) const;
    void setVersionRetention(int n) { keepVersions_ = n; }
    std::string thumbnailPath(const std::string& id) const { return pathJoin(projectDir(id), "thumbnail.png"); }

   private:
    std::string root_;
    int keepVersions_ = 8;
    void writeMeta(const std::string& id, const json& doc);
};

// Format migrations. Returns migrated doc; log describes steps. Throws on unsupported future versions.
json migrateProject(const json& doc, std::vector<std::string>* log = nullptr);
json migrate_v1_to_v2(const json& doc);

std::vector<std::string> listDir(const std::string& dir);
bool removeTree(const std::string& path);
bool copyFile(const std::string& from, const std::string& to);
int64_t dirSize(const std::string& dir);

}  // namespace mf
