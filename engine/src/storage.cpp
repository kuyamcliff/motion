#include "mf/storage.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <sstream>

#include "mf/model.hpp"

extern "C" unsigned long mz_crc32(unsigned long crc, const unsigned char* ptr, size_t buf_len);

namespace mf {

uint32_t crc32Of(const std::string& s) { return (uint32_t)mz_crc32(0, (const unsigned char*)s.data(), s.size()); }

bool atomicWriteFile(const std::string& path, const std::string& data, std::string* err) {
    std::string tmp = path + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { if (err) *err = "Cannot write " + pathBasename(path) + " (storage full or read-only?)"; return false; }
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n <= 0) {
            ::close(fd);
            ::unlink(tmp.c_str());
            if (err) *err = "Write failed for " + pathBasename(path) + " (storage full?)";
            return false;
        }
        off += (size_t)n;
    }
    if (::fsync(fd) != 0) { ::close(fd); ::unlink(tmp.c_str()); if (err) *err = "fsync failed"; return false; }
    ::close(fd);
    if (::rename(tmp.c_str(), path.c_str()) != 0) { ::unlink(tmp.c_str()); if (err) *err = "rename failed"; return false; }
    int dfd = ::open(pathDirname(path).c_str(), O_RDONLY);
    if (dfd >= 0) { ::fsync(dfd); ::close(dfd); }
    return true;
}

std::vector<std::string> listDir(const std::string& dir) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n != "." && n != "..") out.push_back(n);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

bool removeTree(const std::string& path) {
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) return true;
    if (S_ISDIR(st.st_mode)) {
        for (auto& n : listDir(path)) removeTree(pathJoin(path, n));
        return ::rmdir(path.c_str()) == 0;
    }
    return ::unlink(path.c_str()) == 0;
}

bool copyFile(const std::string& from, const std::string& to) {
    bool ok;
    auto bytes = readFileBytes(from, &ok);
    if (!ok) return false;
    return atomicWriteFile(to, std::string(bytes.begin(), bytes.end()));
}

int64_t dirSize(const std::string& dir) {
    int64_t total = 0;
    for (auto& n : listDir(dir)) {
        std::string p = pathJoin(dir, n);
        struct stat st;
        if (::stat(p.c_str(), &st) != 0) continue;
        total += S_ISDIR(st.st_mode) ? dirSize(p) : (int64_t)st.st_size;
    }
    return total;
}

json ProjectSummary::toJson() const {
    return {{"id", id}, {"name", name}, {"path", path}, {"thumbnail", thumbnail}, {"modified", modified}, {"duration", duration}, {"fps", fps},
            {"width", width}, {"height", height}, {"bytes", bytes}, {"needsRecovery", needsRecovery}, {"recoveryTime", recoveryTime}};
}

ProjectStore::ProjectStore(std::string root) : root_(std::move(root)) { makeDirs(root_); }

void ProjectStore::writeMeta(const std::string& id, const json& doc) {
    const json* comp = activeComp(doc);
    json meta = {{"id", id},
                 {"name", jobj(doc, "meta").value("name", std::string("Untitled"))},
                 {"modified", nowSeconds()},
                 {"formatVersion", doc.value("formatVersion", kFormatVersion)}};
    if (comp) {
        meta["width"] = comp->value("width", 0);
        meta["height"] = comp->value("height", 0);
        meta["fps"] = comp->value("fps", 0.0);
        meta["duration"] = comp->value("duration", 0.0);
    }
    atomicWriteFile(pathJoin(projectDir(id), "meta.json"), meta.dump(1));
}

std::vector<ProjectSummary> ProjectStore::list() const {
    std::vector<ProjectSummary> out;
    for (auto& id : listDir(root_)) {
        if (id.empty() || id[0] == '.') continue;
        std::string dir = projectDir(id);
        bool ok;
        std::string m = readFileText(pathJoin(dir, "meta.json"), &ok);
        if (!ok) continue;
        json meta = json::parse(m, nullptr, false);
        if (meta.is_discarded()) continue;
        ProjectSummary s;
        s.id = id;
        s.name = meta.value("name", std::string("Untitled"));
        s.path = dir;
        s.modified = meta.value("modified", 0.0);
        s.width = meta.value("width", 0);
        s.height = meta.value("height", 0);
        s.fps = meta.value("fps", 0.0);
        s.duration = meta.value("duration", 0.0);
        s.thumbnail = fileExists(thumbnailPath(id)) ? thumbnailPath(id) : std::string();
        s.bytes = dirSize(dir);
        RecoveryInfo ri = checkRecovery(id);
        s.needsRecovery = ri.available;
        s.recoveryTime = ri.recoveredTime;
        out.push_back(s);
    }
    std::sort(out.begin(), out.end(), [](const ProjectSummary& a, const ProjectSummary& b) { return a.modified > b.modified; });
    return out;
}

std::string ProjectStore::create(const json& doc, std::string* err) {
    std::string id;
    for (int i = 0; i < 1000; ++i) {
        id = formatString("p%08x", hash32((uint32_t)(nowSeconds() * 1000) + i * 7919u));
        if (!fileExists(projectDir(id))) break;
    }
    if (!makeDirs(projectDir(id))) { if (err) *err = "Cannot create project folder (storage full or read-only?)."; return {}; }
    makeDirs(pathJoin(projectDir(id), "versions"));
    makeDirs(pathJoin(projectDir(id), "caches"));
    std::string e;
    if (!save(id, doc, e, true)) { if (err) *err = e; return {}; }
    return id;
}

bool ProjectStore::load(const std::string& id, json& out, std::string& err, std::vector<std::string>* log) {
    std::string dir = projectDir(id);
    bool ok;
    std::string text = readFileText(pathJoin(dir, "project.json"), &ok);
    json doc;
    if (ok) doc = json::parse(text, nullptr, false);
    if (!ok || doc.is_discarded()) {
        // Fall back to previous stable state.
        text = readFileText(pathJoin(dir, "project.prev.json"), &ok);
        if (ok) doc = json::parse(text, nullptr, false);
        if (!ok || doc.is_discarded()) { err = "Project file is missing or damaged, and no previous stable copy could be read."; return false; }
        if (log) log->push_back("Current project file was damaged; opened previous stable copy.");
    }
    int ver = doc.value("formatVersion", 1);
    if (ver < kFormatVersion) {
        // Never mutate an old project in place before creating a recovery copy.
        copyFile(pathJoin(dir, "project.json"), pathJoin(dir, formatString("project.v%d.backup.json", ver)));
    }
    try {
        out = migrateProject(doc, log);
    } catch (std::exception& e) {
        err = e.what();
        return false;
    }
    auto problems = validateProject(out);
    if (!problems.empty()) {
        err = "Project failed validation: " + problems[0];
        return false;
    }
    return true;
}

bool ProjectStore::save(const std::string& id, const json& doc, std::string& err, bool addVersion) {
    std::string dir = projectDir(id);
    if (!makeDirs(dir)) { err = "Project folder is not writable."; return false; }
    std::string cur = pathJoin(dir, "project.json"), prev = pathJoin(dir, "project.prev.json");
    std::string data = doc.dump();
    if (fileExists(cur)) {
        // Keep the known-good state as previous stable before replacing it.
        if (::rename(cur.c_str(), prev.c_str()) != 0) { err = "Cannot rotate previous project state."; return false; }
    }
    if (!atomicWriteFile(cur, data, &err)) {
        // Restore previous so we never lose the known-good file.
        if (fileExists(prev)) ::rename(prev.c_str(), cur.c_str());
        return false;
    }
    // Journal entries are now part of the checkpoint.
    std::string jpath = pathJoin(dir, "journal.log");
    atomicWriteFile(jpath, formatString("#checkpoint %.3f\n", nowSeconds()));
    writeMeta(id, doc);
    if (addVersion) {
        std::string vdir = pathJoin(dir, "versions");
        makeDirs(vdir);
        std::string vname = formatString("v%013lld.json", (long long)(nowSeconds() * 1000));
        atomicWriteFile(pathJoin(vdir, vname), data);
        auto vs = listDir(vdir);
        while ((int)vs.size() > keepVersions_) {
            ::unlink(pathJoin(vdir, vs.front()).c_str());
            vs.erase(vs.begin());
        }
    }
    return true;
}

bool ProjectStore::appendJournal(const std::string& id, uint64_t seq, const std::string& label, const json& patch) {
    std::string line = json{{"seq", seq}, {"t", nowSeconds()}, {"label", label}, {"patch", patch}}.dump();
    std::string rec = formatString("%08x ", crc32Of(line)) + line + "\n";
    std::string path = pathJoin(projectDir(id), "journal.log");
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return false;
    bool ok = ::write(fd, rec.data(), rec.size()) == (ssize_t)rec.size();
    ::fsync(fd);
    ::close(fd);
    return ok;
}

static std::vector<json> readJournal(const std::string& path, int& corrupt, double& lastT) {
    std::vector<json> out;
    corrupt = 0;
    lastT = 0;
    bool ok;
    std::string text = readFileText(path, &ok);
    if (!ok) return out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (line.size() < 10 || line[8] != ' ') { ++corrupt; break; }
        uint32_t crc = (uint32_t)std::strtoul(line.substr(0, 8).c_str(), nullptr, 16);
        std::string body = line.substr(9);
        if (crc32Of(body) != crc) { ++corrupt; break; }  // torn write: stop at first bad record
        json j = json::parse(body, nullptr, false);
        if (j.is_discarded()) { ++corrupt; break; }
        lastT = j.value("t", 0.0);
        out.push_back(std::move(j));
    }
    return out;
}

RecoveryInfo ProjectStore::checkRecovery(const std::string& id) const {
    RecoveryInfo ri;
    std::string dir = projectDir(id);
    ri.abnormalExit = fileExists(pathJoin(dir, "session.lock"));
    double lastT;
    auto entries = readJournal(pathJoin(dir, "journal.log"), ri.corruptEntries, lastT);
    ri.journalEntries = (int)entries.size();
    bool ok;
    json meta = json::parse(readFileText(pathJoin(dir, "meta.json"), &ok), nullptr, false);
    ri.checkpointTime = ok && !meta.is_discarded() ? meta.value("modified", 0.0) : 0.0;
    ri.recoveredTime = entries.empty() ? ri.checkpointTime : lastT;
    ri.available = ri.journalEntries > 0;
    if (ri.available)
        ri.message = formatString("%d unsaved change(s) can be recovered%s.", ri.journalEntries, ri.abnormalExit ? " after an unexpected exit" : "");
    else if (ri.abnormalExit)
        ri.message = "The app closed unexpectedly; the last saved state is intact.";
    return ri;
}

bool ProjectStore::recover(const std::string& id, json& out, RecoveryInfo* info) const {
    std::string err;
    std::string dir = projectDir(id);
    bool ok;
    std::string text = readFileText(pathJoin(dir, "project.json"), &ok);
    json base = ok ? json::parse(text, nullptr, false) : json();
    if (!ok || base.is_discarded()) {
        text = readFileText(pathJoin(dir, "project.prev.json"), &ok);
        base = ok ? json::parse(text, nullptr, false) : json();
        if (!ok || base.is_discarded()) return false;
    }
    int corrupt;
    double lastT;
    auto entries = readJournal(pathJoin(dir, "journal.log"), corrupt, lastT);
    json cur = base;
    int applied = 0;
    for (auto& e : entries) {
        try {
            json next = cur.patch(e["patch"]);
            if (!validateProject(next).empty()) break;
            cur = std::move(next);
            ++applied;
        } catch (...) {
            break;  // stop at the first entry that does not apply cleanly
        }
    }
    if (info) {
        *info = checkRecovery(id);
        info->journalEntries = applied;
    }
    out = cur;
    return true;
}

void ProjectStore::markOpen(const std::string& id) { atomicWriteFile(pathJoin(projectDir(id), "session.lock"), formatString("%.3f", nowSeconds())); }
void ProjectStore::markClosed(const std::string& id) { ::unlink(pathJoin(projectDir(id), "session.lock").c_str()); }

std::string ProjectStore::duplicate(const std::string& id, const std::string& newName, std::string* err) {
    json doc;
    std::string e;
    if (!load(id, doc, e)) { if (err) *err = e; return {}; }
    doc["meta"]["name"] = newName;
    doc["meta"]["created"] = nowSeconds();
    std::string nid = create(doc, err);
    if (!nid.empty() && fileExists(thumbnailPath(id))) copyFile(thumbnailPath(id), thumbnailPath(nid));
    return nid;
}

bool ProjectStore::rename(const std::string& id, const std::string& name) {
    json doc;
    std::string e;
    if (!load(id, doc, e) || name.empty()) return false;
    doc["meta"]["name"] = name;
    return save(id, doc, e);
}

bool ProjectStore::remove(const std::string& id) {
    std::string trash = pathJoin(root_, ".trash");
    makeDirs(trash);
    return ::rename(projectDir(id).c_str(), pathJoin(trash, id + formatString("-%lld", (long long)nowSeconds())).c_str()) == 0;
}

std::vector<json> ProjectStore::versions(const std::string& id) const {
    std::vector<json> out;
    std::string vdir = pathJoin(projectDir(id), "versions");
    for (auto& n : listDir(vdir)) {
        double t = std::atof(n.substr(1, 13).c_str()) / 1000.0;
        out.push_back({{"name", n}, {"time", t}, {"bytes", fileSize(pathJoin(vdir, n))}});
    }
    std::reverse(out.begin(), out.end());
    return out;
}

bool ProjectStore::loadVersion(const std::string& id, const std::string& version, json& out, std::string& err) const {
    if (version.find('/') != std::string::npos) { err = "Invalid version name."; return false; }
    bool ok;
    std::string text = readFileText(pathJoin(pathJoin(projectDir(id), "versions"), version), &ok);
    if (!ok) { err = "Version not found."; return false; }
    json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded()) { err = "Version file is damaged."; return false; }
    out = migrateProject(doc, nullptr);
    return true;
}

int64_t ProjectStore::footprint(const std::string& id) const { return dirSize(projectDir(id)); }

// ====================================================================== migrations
json migrate_v1_to_v2(const json& in) {
    // v1: opacity stored 0..1, no rotationX/Y, comp-level "captions" array, "layers[].hidden" flag.
    json doc = in;
    for (auto& c : doc["comps"]) {
        for (auto& L : c["layers"]) {
            json& T = L["transform"];
            if (T.contains("opacity")) {
                json& o = T["opacity"];
                if (o.contains("v") && o["v"].is_number()) o["v"] = o["v"].get<double>() * 100.0;
                if (o.contains("k"))
                    for (auto& k : o["k"])
                        if (k["v"].is_number()) k["v"] = k["v"].get<double>() * 100.0;
            }
            if (!T.contains("rotationX")) T["rotationX"] = makeProp(0.0);
            if (!T.contains("rotationY")) T["rotationY"] = makeProp(0.0);
            if (L.contains("hidden")) {
                L["enabled"] = !L["hidden"].get<bool>();
                L.erase("hidden");
            }
            for (const char* k : {"effects", "masks", "behaviors", "markers"})
                if (!L.contains(k)) L[k] = json::array();
            if (!L.contains("matte")) L["matte"] = nullptr;
            if (!L.contains("parent")) L["parent"] = nullptr;
        }
        if (c.contains("captions") && c["captions"].is_array()) {
            json items = c["captions"];
            c.erase("captions");
            if (!items.empty()) {
                std::string id = newId(doc, "L");
                json L = makeLayer("captions", id, c, doc, json::object());
                int qi = 0;
                for (auto& it : items) it["id"] = "Q" + std::to_string(++qi) + "m";
                L["captions"]["items"] = items;
                c["layers"].insert(c["layers"].begin(), L);
            }
        }
        if (!c.contains("audio")) c["audio"] = newComp("x", "x", 16, 16, 30, 1)["audio"];
        if (!c.contains("motionBlur")) c["motionBlur"] = {{"enabled", false}, {"samples", 8}, {"shutter", 180}};
        if (!c.contains("guides")) c["guides"] = json::array();
        if (!c.contains("markers")) c["markers"] = json::array();
    }
    if (!doc.contains("captionStyles")) doc["captionStyles"] = json::array({defaultCaptionStyle()});
    if (!doc.contains("capsules")) doc["capsules"] = json::array();
    doc["formatVersion"] = 2;
    return doc;
}

json migrateProject(const json& in, std::vector<std::string>* log) {
    int v = in.value("formatVersion", 1);
    if (v > kFormatVersion)
        throw std::runtime_error(formatString("This project was saved by a newer MOTIONFORGE (format %d; this app supports up to %d). Update the app to open it.", v, kFormatVersion));
    json doc = in;
    if (v < 2) {
        doc = migrate_v1_to_v2(doc);
        if (log) log->push_back("Migrated project format v1 -> v2 (opacity scale, 3D rotation, caption track).");
    }
    doc["engineVersion"] = kEngineVersion;
    return doc;
}

}  // namespace mf
