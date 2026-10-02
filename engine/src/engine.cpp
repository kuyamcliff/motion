#include "mf/engine.hpp"

#include <sstream>
#include <thread>

#include "mf/audio.hpp"
#include "mf/effects.hpp"
#include "mf/gif.hpp"
#include "mf/imageops.hpp"
#include "mf/model.hpp"
#include "mf/scripting.hpp"
#include "mf/text.hpp"

namespace mf {

json OpOutcome::toJson() const { return {{"ok", ok}, {"error", error}, {"label", label}, {"data", data}}; }

Engine::Engine(EngineConfig cfg, MediaProvider* media)
    : cfg_(std::move(cfg)), media_(media), store_(pathJoin(cfg_.dataDir, "projects")), renderer_(media) {
    for (const char* d : {"presets", "capsules", "scripts", "extensions", "templates", "exports"}) makeDirs(pathJoin(cfg_.dataDir, d));
    if (!cfg_.cacheDir.empty()) makeDirs(cfg_.cacheDir);
    doc_.onCommit = [this](const std::string& label, const json& patch, uint64_t) {
        // Continuous journal: every committed change is durable before the next one.
        if (!projectId_.empty()) store_.appendJournal(projectId_, ++journalSeq_, label, patch);
        saveStatus_ = "unsaved";
    };
}

Engine::~Engine() {
    if (isOpen()) closeProject();
}

std::string Engine::createProject(const std::string& name, int w, int h, double fps, double duration, std::string* err) {
    if (w < 16 || h < 16 || w > 16384 || h > 16384) { if (err) *err = "Resolution must be between 16 and 16384 pixels."; return {}; }
    if (fps <= 0 || fps > 240) { if (err) *err = "Frame rate must be between 1 and 240 fps."; return {}; }
    return createProjectFromDoc(newProject(name.empty() ? "Untitled" : name, w, h, fps, duration > 0 ? duration : 10), err);
}

std::string Engine::createProjectFromDoc(const json& doc, std::string* err) { return store_.create(doc, err); }

bool Engine::openProject(const std::string& id, std::string& err, bool recover) {
    if (isOpen()) closeProject();
    json d;
    std::vector<std::string> log;
    if (recover) {
        RecoveryInfo ri;
        if (!store_.recover(id, d, &ri)) { err = "Recovery failed: no readable project state."; return false; }
        try {
            d = migrateProject(d, &log);
        } catch (std::exception& e) {
            err = e.what();
            return false;
        }
    } else if (!store_.load(id, d, err, &log)) {
        return false;
    }
    doc_.reset(d);
    projectId_ = id;
    journalSeq_ = 0;
    store_.markOpen(id);
    lastSave_ = nowSeconds();
    if (recover) {
        // Persist the recovered state as the new checkpoint (known-good file kept as project.prev.json).
        std::string e;
        store_.save(id, d, e, true);
    }
    renderer_.clearCaches();
    renderer_.audio().resetState();
    saveStatus_ = "saved";
    return true;
}

void Engine::closeProject() {
    if (!isOpen()) return;
    std::string err;
    if (doc_.dirty()) save(err);
    writeThumbnail(0.5);
    store_.markClosed(projectId_);
    projectId_.clear();
}

std::string Engine::activeCompId() const {
    auto s = doc_.snapshot();
    const json* c = activeComp(*s);
    return c ? c->value("id", std::string()) : std::string();
}

OpOutcome Engine::apply(const json& op) {
    OpOutcome out;
    std::lock_guard<std::mutex> lk(mutex_);
    try {
        if (inGesture_) {
            doc_.cancelPreview();
            inGesture_ = false;
        }
        OpResult r;
        json next = applyOp(doc_.doc(), op, &r);
        out.label = r.label;
        out.data = r.data;
        doc_.commit(r.label, std::move(next));
        out.ok = true;
    } catch (EditError& e) {
        out.error = e.what();
    } catch (std::exception& e) {
        out.error = std::string("Internal error: ") + e.what();
    }
    lastError_ = out.error;
    return out;
}

OpOutcome Engine::previewOp(const json& op) {
    OpOutcome out;
    std::lock_guard<std::mutex> lk(mutex_);
    try {
        if (!inGesture_) {
            gestureBase_ = doc_.doc();
            inGesture_ = true;
        }
        OpResult r;
        json next = applyOp(gestureBase_, op, &r);
        doc_.preview(std::move(next));
        out.ok = true;
        out.label = r.label;
        out.data = r.data;
    } catch (EditError& e) {
        out.error = e.what();
    } catch (std::exception& e) {
        out.error = e.what();
    }
    return out;
}

bool Engine::commitPreview(const std::string& label) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!inGesture_) return false;
    inGesture_ = false;
    return doc_.commitPreview(label);
}

void Engine::cancelPreview() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!inGesture_) return;
    inGesture_ = false;
    doc_.cancelPreview();
}

bool Engine::undo() {
    std::lock_guard<std::mutex> lk(mutex_);
    inGesture_ = false;
    return doc_.undo();
}
bool Engine::redo() {
    std::lock_guard<std::mutex> lk(mutex_);
    inGesture_ = false;
    return doc_.redo();
}
bool Engine::jumpHistory(int index) {
    std::lock_guard<std::mutex> lk(mutex_);
    return doc_.jumpTo(index);
}

json Engine::historyJson() const {
    json h = json::array();
    int i = 0;
    for (auto& e : doc_.history()) h.push_back({{"index", ++i}, {"label", e.label}, {"time", e.timestamp}});
    return {{"entries", h}, {"index", doc_.historyIndex()}};
}

json Engine::stateJson() const {
    return {{"canUndo", doc_.canUndo()},        {"canRedo", doc_.canRedo()},       {"undoLabel", doc_.undoLabel()},
            {"redoLabel", doc_.redoLabel()},    {"dirty", doc_.dirty()},           {"revision", doc_.revision()},
            {"saveStatus", saveStatus()},       {"projectId", projectId_},         {"historyIndex", doc_.historyIndex()},
            {"historySize", doc_.history().size()}};
}

bool Engine::save(std::string& err, bool version) {
    if (!isOpen()) { err = "No project is open."; return false; }
    auto snap = doc_.snapshot();
    if (!store_.save(projectId_, *snap, err, version)) {
        saveStatus_ = "error";
        lastError_ = err;
        return false;
    }
    doc_.markSaved();
    lastSave_ = nowSeconds();
    saveStatus_ = "saved";
    return true;
}

bool Engine::autosaveTick(double now) {
    if (!isOpen() || !doc_.dirty() || doc_.inPreview()) return false;
    if (now - lastSave_ < cfg_.autosaveInterval) return false;
    std::string err;
    return save(err, false);
}

void Engine::checkpoint(const std::string& reason) {
    std::string err;
    if (isOpen()) save(err, true);
    MF_LOGI("checkpoint: " + reason);
}

std::string Engine::saveStatus() const { return doc_.dirty() && saveStatus_ == "saved" ? "unsaved" : saveStatus_; }

Image Engine::render(double t, double scale, RenderSettings rs, RenderStats* stats, const std::string& compId) {
    auto snap = doc_.snapshot();
    rs.scale = scale;
    rs.revision = doc_.revision() + 1;
    return renderer_.renderFrame(*snap, compId.empty() ? activeCompId() : compId, t, rs, stats);
}

std::string Engine::hitTest(double t, double x, double y) {
    auto snap = doc_.snapshot();
    return renderer_.hitTest(*snap, activeCompId(), t, x, y);
}

json Engine::layerQuad(const std::string& layerId, double t) {
    auto snap = doc_.snapshot();
    Vec2 q[4];
    if (!renderer_.layerQuad(*snap, activeCompId(), layerId, t, q)) return json();
    json out = json::array();
    for (auto& p : q) out.push_back({p.x, p.y});
    return out;
}

bool Engine::writeThumbnail(double t) {
    if (!isOpen()) return false;
    auto snap = doc_.snapshot();
    const json* comp = activeComp(*snap);
    if (!comp) return false;
    double scale = 320.0 / std::max(1, comp->value("width", 1920));
    RenderSettings rs;
    rs.scale = scale;
    Image img = renderer_.renderFrame(*snap, comp->value("id", ""), std::min(t, comp->value("duration", 1.0) / 2), rs);
    return savePng(img, store_.thumbnailPath(projectId_));
}

float Engine::mixAudio(double t0, int frames, float* out, int sr) {
    auto snap = doc_.snapshot();
    const json* comp = activeComp(*snap);
    if (!comp) { std::fill(out, out + frames * 2, 0.f); return 0; }
    renderer_.audio().setProject(snap.get());
    return renderer_.audio().mix(*snap, *comp, t0, frames, out, sr);
}

std::string Engine::defaultModelPath() const {
    for (auto& n : listDir(cfg_.modelsDir))
        if (pathExtensionLower(n) == "bin" && n.find("ggml") != std::string::npos) return pathJoin(cfg_.modelsDir, n);
    return {};
}

AsrResult Engine::transcribePcm(const std::vector<float>& pcm, double offset, const AsrOptions& opt, const SegmentOptions& seg) {
    AsrResult r = transcribe(pcm, opt, seg);
    if (offset != 0) {
        for (auto& w : r.words) { w.s += offset; w.e += offset; }
        for (auto& c : r.captions) {
            c.start += offset;
            c.end += offset;
            for (auto& w : c.words) { w.s += offset; w.e += offset; }
        }
    }
    return r;
}

AsrResult Engine::transcribeLayer(const std::string& layerId, const AsrOptions& opt0, const SegmentOptions& seg) {
    AsrResult r;
    auto snap = doc_.snapshot();
    const json* comp = activeComp(*snap);
    const json* L = comp ? findLayer(*comp, layerId) : nullptr;
    if (!L) { r.error = "Choose a video or audio layer that contains speech."; return r; }
    const json* a = findAsset(*snap, L->value("asset", ""));
    if (!a) { r.error = "The layer has no media."; return r; }
    auto buf = media_->audio(*a);
    if (!buf || buf->samples.empty()) { r.error = "No audio track could be decoded from '" + a->value("name", std::string()) + "'."; return r; }
    AsrOptions opt = opt0;
    if (opt.modelPath.empty()) opt.modelPath = defaultModelPath();
    if (opt.modelPath.empty()) { r.error = "No offline speech model is installed. Open Settings > AI Models to install one."; return r; }
    // Use the layer's visible source range; map results back to comp time (speed 1, no remap supported for captions).
    double in = L->value("in", 0.0), out = L->value("out", 0.0);
    double s0 = layerSourceTime(*L, in), s1 = layerSourceTime(*L, out - 1e-6);
    if (s1 < s0) std::swap(s0, s1);
    std::vector<float> mono = resampleMono(*buf, 16000);
    size_t a0 = (size_t)std::max(0.0, s0 * 16000), a1 = std::min(mono.size(), (size_t)(s1 * 16000));
    if (a1 <= a0) { r.error = "The layer's visible range contains no audio."; return r; }
    std::vector<float> slice(mono.begin() + a0, mono.begin() + a1);
    double speed = L->value("speed", 1.0);
    r = transcribe(slice, opt, seg);
    if (!r.ok) return r;
    auto mapT = [&](double t) { return in + t / std::max(1e-6, speed); };
    for (auto& w : r.words) { w.s = mapT(w.s); w.e = mapT(w.e); }
    for (auto& c : r.captions) {
        c.start = mapT(c.start);
        c.end = mapT(c.end);
        for (auto& w : c.words) { w.s = mapT(w.s); w.e = mapT(w.e); }
    }
    return r;
}

json Engine::diagnostics() {
    auto snap = doc_.snapshot();
    auto ds = diagnoseProject(*snap, FontManager::instance().names(), [this](const json& a) { return media_->available(a); });
    json out = json::array();
    for (auto& d : ds) out.push_back({{"severity", d.severity}, {"code", d.code}, {"message", d.message}, {"target", d.target}, {"recommendation", d.recommendation}});
    for (auto& [k, e] : renderer_.expressions()->errors())
        out.push_back({{"severity", "warning"}, {"code", "EXPRESSION_ERROR"}, {"message", e}, {"target", k}, {"recommendation", "Fix the expression; the keyframed value is used meanwhile."}});
    return out;
}

json Engine::capabilities() const {
    return {{"engine", kEngineVersion},
            {"formatVersion", kFormatVersion},
            {"renderer", "cpu"},
            {"threads", (int)std::thread::hardware_concurrency()},
            {"asr", asrAvailable()},
            {"expressions", "quickjs"},
            {"effects", (int)effectRegistry().size()},
            {"models3d", json::array({"glb", "gltf", "obj"})},
            {"lut", json::array({"cube", "3dl"})},
            {"subtitles", json::array({"srt", "vtt", "ass"})}};
}

void Engine::clearCaches() {
    renderer_.clearCaches();
    if (!cfg_.cacheDir.empty()) {
        for (auto& n : listDir(cfg_.cacheDir)) removeTree(pathJoin(cfg_.cacheDir, n));
    }
}

// ====================================================================== scenarios (.mftest)
namespace {

std::vector<std::string> tokenizeLine(const std::string& line) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace((unsigned char)line[i])) ++i;
        if (i >= line.size()) break;
        if (line[i] == '"') {
            size_t e = line.find('"', i + 1);
            if (e == std::string::npos) e = line.size();
            out.push_back(line.substr(i + 1, e - i - 1));
            i = e + 1;
        } else if (line[i] == '[' || line[i] == '{') {
            char open = line[i], close = open == '[' ? ']' : '}';
            int depth = 0;
            size_t s = i;
            for (; i < line.size(); ++i) {
                if (line[i] == open) ++depth;
                if (line[i] == close && --depth == 0) { ++i; break; }
            }
            out.push_back(line.substr(s, i - s));
        } else {
            size_t s = i;
            while (i < line.size() && !std::isspace((unsigned char)line[i])) ++i;
            out.push_back(line.substr(s, i - s));
        }
    }
    return out;
}

std::string upper(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

double parseTimeArg(const std::string& s, double fps) {
    std::string v = s;
    if (v.rfind("t=", 0) == 0) v = v.substr(2);
    if (!v.empty() && v.back() == 'f') return std::atof(v.c_str()) / fps;
    if (v.size() > 1 && v.back() == 's') v.pop_back();
    return std::atof(v.c_str());
}

std::string propAlias(const std::string& p) {
    static const std::map<std::string, std::string> a = {{"position", "transform.position"}, {"scale", "transform.scale"}, {"rotation", "transform.rotation"},
                                                         {"opacity", "transform.opacity"}, {"anchor", "transform.anchor"}, {"text", "text.content"}};
    auto it = a.find(p);
    return it == a.end() ? p : it->second;
}

std::string resolveEffect(const std::string& q) {
    if (findEffect(q)) return q;
    std::string lq = q;
    for (auto& c : lq) c = (char)std::tolower((unsigned char)c);
    for (auto& e : effectRegistry()) {
        std::string t = e.type, n = e.name;
        for (auto& c : n) c = (char)std::tolower((unsigned char)c);
        if (t.size() > lq.size() && t.compare(t.size() - lq.size(), lq.size(), lq) == 0) return e.type;
        if (n == lq) return e.type;
    }
    for (auto& e : effectRegistry()) {
        std::string n = e.name;
        for (auto& c : n) c = (char)std::tolower((unsigned char)c);
        if (n.find(lq) != std::string::npos) return e.type;
    }
    return {};
}

}  // namespace

ScenarioResult runScenario(Engine& E, const std::string& script, const std::string& workDir) {
    ScenarioResult R;
    std::istringstream in(script);
    std::string line;
    std::string last;  // last created / selected layer
    double t = 0;
    int ln = 0;
    makeDirs(workDir);
    auto fail = [&](const std::string& m) {
        R.error = formatString("line %d: ", ln) + m;
        R.line = ln;
        R.log.push_back("FAIL " + R.error);
        return R;
    };
    auto fps = [&]() {
        const json* c = activeComp(*E.snapshot());
        return c ? compFps(*c) : 30.0;
    };
    auto applyOrFail = [&](const json& op, OpOutcome& o) {
        o = E.apply(op);
        return o.ok;
    };
    while (std::getline(in, line)) {
        ++ln;
        std::string trimmed = line;
        while (!trimmed.empty() && std::isspace((unsigned char)trimmed.back())) trimmed.pop_back();
        size_t p0 = trimmed.find_first_not_of(" \t");
        if (p0 == std::string::npos || trimmed[p0] == '#') continue;
        auto tok = tokenizeLine(trimmed.substr(p0));
        if (tok.empty()) continue;
        std::string cmd = upper(tok[0]);
        R.log.push_back(trimmed.substr(p0));
        OpOutcome o;
        if (cmd == "CREATE") {
            int w = 1920, h = 1080;
            double f = 30, dur = 10;
            for (size_t i = 1; i < tok.size(); ++i) {
                if (std::sscanf(tok[i].c_str(), "%dx%d", &w, &h) == 2) continue;
                if (tok[i].size() > 3 && tok[i].substr(tok[i].size() - 3) == "fps") f = std::atof(tok[i].c_str());
                if (upper(tok[i]) == "DURATION" && i + 1 < tok.size()) dur = std::atof(tok[++i].c_str());
            }
            std::string err;
            std::string id = E.createProject("Scenario", w, h, f, dur, &err);
            if (id.empty() || !E.openProject(id, err)) return fail("CREATE failed: " + err);
            R.outputs["projectId"] = id;
        } else if (cmd == "IMPORT") {
            if (tok.size() < 3) return fail("IMPORT <video|image|audio|model|lut> <path|generator:name|tone:hz> [duration N]");
            std::string kind = tok[1], src = tok[2];
            json asset = {{"type", kind}, {"name", pathBasename(src)}};
            double dur = 5;
            for (size_t i = 3; i + 1 < tok.size(); ++i)
                if (upper(tok[i]) == "DURATION") dur = std::atof(tok[i + 1].c_str());
            if (src.rfind("generator:", 0) == 0) {
                asset["generator"] = src.substr(10);
                asset["width"] = 1280; asset["height"] = 720; asset["fps"] = fps(); asset["duration"] = dur; asset["hasAudio"] = false;
            } else if (src.rfind("tone:", 0) == 0) {
                asset["toneHz"] = std::atof(src.c_str() + 5);
                asset["duration"] = dur; asset["hasAudio"] = true; asset["path"] = "tone:" + src.substr(5);
            } else {
                std::string path = src[0] == '/' ? src : pathJoin(workDir, src);
                if (!fileExists(path)) return fail("IMPORT: file not found " + path);
                asset["path"] = path;
                if (kind == "image") {
                    Image img;
                    if (!loadImageFile(path, img)) return fail("IMPORT: unreadable image");
                    asset["width"] = img.w; asset["height"] = img.h;
                } else if (kind == "audio") {
                    AudioBuffer b;
                    if (!loadWav(path, b)) return fail("IMPORT: only WAV audio is supported by the host runner");
                    asset["duration"] = b.duration(); asset["hasAudio"] = true; asset["sampleRate"] = b.sampleRate; asset["channels"] = b.channels;
                }
            }
            if (!applyOrFail({{"op", "addAsset"}, {"asset", asset}}, o)) return fail(o.error);
            std::string aid = o.data["asset"];
            std::string lk = kind == "model" ? "model3d" : kind;
            if (kind == "lut") { R.outputs["lastAsset"] = aid; continue; }
            json opts = kind == "model" ? json{{"asset", aid}} : json{{"asset", aid}};
            if (!applyOrFail({{"op", "addLayer"}, {"kind", lk}, {"options", opts}, {"at", t}, {"extendComp", true}}, o)) return fail(o.error);
            last = o.data["layer"];
            R.outputs["lastAsset"] = aid;
        } else if (cmd == "ADD") {
            if (tok.size() < 2) return fail("ADD <kind> ...");
            std::string what = tok[1];
            std::string lw = what;
            for (auto& c : lw) c = (char)std::tolower((unsigned char)c);
            if (lw == "keyframe") {
                if (last.empty() || tok.size() < 3) return fail("ADD keyframe <prop> [t=sec]");
                double kt = tok.size() > 3 ? parseTimeArg(tok[3], fps()) : t;
                if (!applyOrFail({{"op", "addKeyframe"}, {"layer", last}, {"path", propAlias(tok[2])}, {"t", kt}}, o)) return fail(o.error);
                t = kt;
                continue;
            }
            if (lw == "marker") {
                if (!applyOrFail({{"op", "addMarker"}, {"t", t}, {"title", tok.size() > 2 ? tok[2] : ""}}, o)) return fail(o.error);
                continue;
            }
            json opts = json::object();
            std::string kind = lw;
            if (lw == "text") opts["text"] = tok.size() > 2 ? tok[2] : "Text";
            if (lw == "shape") opts["shape"] = tok.size() > 2 ? tok[2] : "rect";
            if (lw == "light") opts["light"] = tok.size() > 2 ? tok[2] : "point";
            if (lw == "particles") opts["preset"] = tok.size() > 2 ? tok[2] : "sparks";
            if (lw == "model") { kind = "model3d"; opts["primitive"] = tok.size() > 2 ? tok[2] : "cube"; }
            if (lw == "solid" && tok.size() > 2) opts["color"] = json::parse(tok[2], nullptr, false);
            if (!applyOrFail({{"op", "addLayer"}, {"kind", kind}, {"options", opts}, {"at", t}}, o)) return fail(o.error);
            last = o.data["layer"];
        } else if (cmd == "SELECT") {
            if (tok.size() < 2) return fail("SELECT <name|id>");
            const json* comp = activeComp(*E.snapshot());
            last.clear();
            for (auto& L : jarr(*comp, "layers"))
                if (L.value("id", "") == tok[1] || L.value("name", "") == tok[1]) last = L.value("id", "");
            if (last.empty()) return fail("SELECT: no layer named " + tok[1]);
        } else if (cmd == "TIME") {
            if (tok.size() < 2) return fail("TIME <seconds|Nf>");
            t = parseTimeArg(tok[1], fps());
        } else if (cmd == "SET") {
            // SET <prop> = <value>
            if (tok.size() < 4 || tok[2] != "=") return fail("SET <property> = <value>");
            if (last.empty()) return fail("SET: no layer selected");
            std::string path = propAlias(tok[1]);
            json v = json::parse(tok[3], nullptr, false);
            if (v.is_discarded()) v = tok[3];
            if (path == "text.content") {
                if (!applyOrFail({{"op", "setText"}, {"layer", last}, {"content", v.is_string() ? v.get<std::string>() : v.dump()}, {"t", t}}, o)) return fail(o.error);
                continue;
            }
            // Pad 2D positions to 3D.
            const json* comp = activeComp(*E.snapshot());
            const json* L = findLayer(*comp, last);
            if (L && v.is_array()) {
                const json* P = resolvePath(*L, path);
                Value cur = P ? evalRaw(*P, 0) : Value();
                while (v.size() < cur.n.size()) v.push_back(cur.n[v.size()]);
            }
            if (!applyOrFail({{"op", "setProp"}, {"layer", last}, {"path", path}, {"value", v}, {"t", t}}, o)) return fail(o.error);
        } else if (cmd == "APPLY") {
            if (tok.size() < 2 || last.empty()) return fail("APPLY <effect> (needs a selected layer)");
            std::string type = resolveEffect(tok[1]);
            if (type.empty()) return fail("APPLY: unknown effect " + tok[1]);
            if (!applyOrFail({{"op", "addEffect"}, {"layer", last}, {"type", type}}, o)) return fail(o.error);
        } else if (cmd == "SPLIT") {
            double st = tok.size() > 1 ? parseTimeArg(tok[1], fps()) : t;
            json op = {{"op", "split"}, {"t", st}};
            if (!last.empty()) op["layers"] = {last};
            if (!applyOrFail(op, o)) return fail(o.error);
        } else if (cmd == "OP") {
            if (tok.size() < 2) return fail("OP {json}");
            json op = json::parse(tok[1], nullptr, false);
            if (op.is_discarded()) return fail("OP: invalid JSON");
            if (!op.contains("layer") && !last.empty() && op.value("op", "") != "addLayer") op["layer"] = last;
            if (!op.contains("t")) op["t"] = t;
            if (!applyOrFail(op, o)) return fail(o.error);
            if (o.data.contains("layer")) last = o.data["layer"];
        } else if (cmd == "UNDO") {
            if (!E.undo()) return fail("UNDO: nothing to undo");
        } else if (cmd == "REDO") {
            if (!E.redo()) return fail("REDO: nothing to redo");
        } else if (cmd == "SAVE") {
            std::string err;
            if (!E.save(err, true)) return fail("SAVE failed: " + err);
        } else if (cmd == "REOPEN") {
            json before = *E.snapshot();
            std::string id = E.projectId(), err;
            E.closeProject();
            if (!E.openProject(id, err)) return fail("REOPEN failed: " + err);
            json after = *E.snapshot();
            before["meta"].erase("modified");
            after["meta"].erase("modified");
            before.erase("engineVersion");
            after.erase("engineVersion");
            if (json::diff(before, after).size() > 0) return fail("REOPEN: project differs after reopen: " + json::diff(before, after).dump().substr(0, 300));
        } else if (cmd == "RENDER") {
            if (tok.size() < 2) return fail("RENDER <file.png> [t]");
            double rt = tok.size() > 2 ? parseTimeArg(tok[2], fps()) : t;
            RenderStats st;
            Image img = E.render(rt, 1.0, RenderSettings(), &st);
            std::string path = pathJoin(workDir, tok[1]);
            if (!savePng(img, path)) return fail("RENDER: cannot write " + path);
            R.outputs["lastRender"] = path;
            R.outputs["lastRenderHash"] = std::to_string(imageHash(img));
        } else if (cmd == "EXPORT") {
            if (tok.size() < 3) return fail("EXPORT <gif|png|wav|srt|vtt|ass|frames> <file>");
            std::string fmt = tok[1];
            std::string path = pathJoin(workDir, tok[2]);
            const json* comp = activeComp(*E.snapshot());
            double dur = comp->value("duration", 1.0), f = compFps(*comp);
            if (fmt == "gif" || fmt == "frames") {
                double gfps = f;
                double scale = 0.5;
                for (size_t i = 3; i + 1 < tok.size(); ++i) {
                    if (upper(tok[i]) == "FPS") gfps = std::atof(tok[i + 1].c_str());
                    if (upper(tok[i]) == "SCALE") scale = std::atof(tok[i + 1].c_str());
                }
                int n = (int)std::floor(dur * gfps + 1e-6);
                GifWriter gw;
                int w = (int)std::lround(comp->value("width", 1920) * scale), h = (int)std::lround(comp->value("height", 1080) * scale);
                if (fmt == "gif" && !gw.open(path, w, h)) return fail("EXPORT gif: cannot write");
                if (fmt == "frames") makeDirs(path);
                RenderSettings rs;
                rs.exportMode = true;
                for (int i = 0; i < n; ++i) {
                    Image img = E.render(i / gfps, scale, rs);
                    if (fmt == "gif") gw.addFrame(img, (int)std::lround(100.0 / gfps));
                    else savePng(img, pathJoin(path, formatString("frame_%05d.png", i)));
                }
                if (fmt == "gif") gw.close();
                R.outputs["exportFrames"] = n;
            } else if (fmt == "wav") {
                int sr = 48000;
                size_t frames = (size_t)(dur * sr);
                std::vector<float> buf(frames * 2);
                E.renderer().audio().resetState();
                for (size_t off = 0; off < frames; off += 4800) {
                    int n = (int)std::min<size_t>(4800, frames - off);
                    E.mixAudio(off / (double)sr, n, buf.data() + off * 2, sr);
                }
                if (!saveWav(path, buf.data(), frames, 2, sr)) return fail("EXPORT wav: cannot write");
            } else if (fmt == "srt" || fmt == "vtt" || fmt == "ass") {
                std::vector<CaptionItem> items;
                json style = defaultCaptionStyle();
                for (auto& L : jarr(*comp, "layers"))
                    if (L.value("type", "") == "captions") {
                        items = captionsFromJson(jarr(L["captions"], "items"));
                        style = jobj(L["captions"], "style");
                    }
                std::string text = fmt == "srt" ? toSrt(items) : fmt == "vtt" ? toVtt(items) : toAss(items, style, comp->value("width", 1920), comp->value("height", 1080));
                if (!atomicWriteFile(path, text)) return fail("EXPORT subtitles: cannot write");
            } else {
                return fail("EXPORT: format must be gif, frames, wav, srt, vtt or ass (video encoding runs on device)");
            }
            R.outputs["lastExport"] = path;
        } else if (cmd == "VALIDATE") {
            auto problems = validateProject(*E.snapshot());
            if (!problems.empty()) return fail("VALIDATE: " + problems[0]);
            if (R.outputs.contains("lastExport")) {
                std::string p = R.outputs["lastExport"];
                if (!fileExists(p)) return fail("VALIDATE: export missing");
                if (pathExtensionLower(p) == "gif") {
                    int w, h, n;
                    if (!inspectGif(p, w, h, n) || n != R.outputs.value("exportFrames", -1)) return fail("VALIDATE: GIF frame count mismatch");
                }
            }
        } else if (cmd == "EXPECT") {
            // EXPECT layers == N | EXPECT <prop> == <json> | EXPECT pixel x y ~ [r,g,b] tol | EXPECT undo | EXPECT !redo
            if (tok.size() >= 2 && (tok[1] == "undo" || tok[1] == "!undo" || tok[1] == "redo" || tok[1] == "!redo")) {
                json s = E.stateJson();
                bool want = tok[1][0] != '!';
                std::string k = want ? tok[1] : tok[1].substr(1);
                if (s[k == "undo" ? "canUndo" : "canRedo"].get<bool>() != want) return fail("EXPECT " + tok[1] + " failed");
                continue;
            }
            if (tok.size() < 4) return fail("EXPECT <what> == <value>");
            const json* comp = activeComp(*E.snapshot());
            if (tok[1] == "layers") {
                int want = std::atoi(tok[3].c_str());
                int have = (int)jarr(*comp, "layers").size();
                if (have != want) return fail(formatString("EXPECT layers == %d, have %d", want, have));
            } else if (tok[1] == "pixel") {
                if (tok.size() < 6) return fail("EXPECT pixel x y ~ [r,g,b] [tol]");
                int x = std::atoi(tok[2].c_str()), y = std::atoi(tok[3].c_str());
                json c = json::parse(tok[5], nullptr, false);
                int tol = tok.size() > 6 ? std::atoi(tok[6].c_str()) : 8;
                Image img = E.render(t, 1.0);
                if (x < 0 || y < 0 || x >= img.w || y >= img.h) return fail("EXPECT pixel out of range");
                const uint8_t* p = img.at(x, y);
                for (int k = 0; k < 3; ++k)
                    if (std::abs((int)p[k] - c[k].get<int>()) > tol) return fail(formatString("EXPECT pixel %d,%d = [%d,%d,%d]", x, y, p[0], p[1], p[2]));
            } else if (tok[1] == "duration") {
                if (std::fabs(comp->value("duration", 0.0) - std::atof(tok[3].c_str())) > 1e-6) return fail("EXPECT duration mismatch");
            } else {
                if (last.empty()) return fail("EXPECT <prop>: no layer selected");
                const json* L = findLayer(*comp, last);
                if (!L) return fail("EXPECT: selected layer no longer exists");
                std::string path = propAlias(tok[1]);
                const json* P = resolvePath(*L, path);
                if (!P) return fail("EXPECT: no property " + path);
                json want = json::parse(tok[3], nullptr, false);
                if (want.is_discarded()) want = tok[3];
                Value have = (P->is_object() && (P->contains("v") || P->contains("k"))) ? evalRaw(*P, layerLocalTime(*L, t)) : Value::fromJson(*P);
                Value w = Value::fromJson(want);
                bool okv = true;
                if (w.kind == Value::Kind::String) okv = have.s == w.s;
                else
                    for (size_t k = 0; k < w.n.size(); ++k)
                        if (k >= have.n.size() || std::fabs(have.n[k] - w.n[k]) > 1e-6) okv = false;
                if (!okv) return fail("EXPECT " + path + " == " + tok[3] + " but was " + have.toJson().dump());
            }
        } else if (cmd == "SCRIPT") {
            if (tok.size() < 2) return fail("SCRIPT <file.js>");
            bool ok;
            std::string src = readFileText(pathJoin(workDir, tok[1]), &ok);
            if (!ok) return fail("SCRIPT: file not found");
            ScriptRequest req;
            req.source = src;
            req.permissions = {"PROJECT_READ", "PROJECT_WRITE", "TIMELINE_WRITE"};
            req.project = *E.snapshot();
            req.compId = E.activeCompId();
            req.playhead = t;
            ScriptResult sr = runScript(req);
            for (auto& l : sr.logs) R.log.push_back("  script: " + l);
            if (!sr.ok) return fail("SCRIPT error: " + sr.error);
            if (sr.changed) E.doc().commit("Run Script: " + tok[1], sr.project);
        } else if (cmd == "CAPTIONS") {
            if (last.empty()) return fail("CAPTIONS: select an audio/video layer first");
            AsrOptions ao;
            ao.language = "en";
            AsrResult ar = E.transcribeLayer(last, ao);
            if (!ar.ok) return fail("CAPTIONS: " + ar.error);
            if (!applyOrFail({{"op", "setCaptions"}, {"items", captionsToJson(ar.captions)}, {"language", ar.language}, {"source", "whisper"}}, o)) return fail(o.error);
            std::string text;
            for (auto& c : ar.captions) text += c.text + " ";
            R.outputs["transcript"] = text;
        } else {
            return fail("Unknown command " + tok[0]);
        }
    }
    R.ok = true;
    R.log.push_back("PASS");
    return R;
}

}  // namespace mf
