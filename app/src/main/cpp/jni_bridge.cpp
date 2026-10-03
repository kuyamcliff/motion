// JNI bridge: Kotlin <-> native engine. Most calls go through a JSON dispatcher (nativeCall) so the
// Kotlin API stays small and every edit flows through the engine's command layer.
#include <android/bitmap.h>
#include <android/log.h>
#include <jni.h>

#include <atomic>
#include <set>

#include "android_media.h"
#include "mf/audio.hpp"
#include "mf/behaviors.hpp"
#include "mf/captions.hpp"
#include "mf/easing.hpp"
#include "mf/effects.hpp"
#include "mf/engine.hpp"
#include "mf/gif.hpp"
#include "mf/imageops.hpp"
#include "mf/model.hpp"
#include "mf/package.hpp"
#include "mf/scripting.hpp"
#include "mf/storage.hpp"
#include "mf/text.hpp"
#include "mf/threadpool.hpp"
#include "mf/tracking.hpp"

using namespace mf;

namespace {
std::atomic<bool> g_useProxies{true};  // preview reads asset proxies (Settings → "Use proxies in preview")

std::unique_ptr<mfa::AndroidMediaProvider> g_media;
std::unique_ptr<Engine> g_engine;
std::mutex g_initMutex;
EngineConfig g_cfg;
// Frozen document used while an export runs so later edits never change a running export.
std::shared_ptr<const json> g_exportDoc;
std::mutex g_exportMutex;
std::shared_ptr<const json> exportDoc() {
    std::lock_guard<std::mutex> lk(g_exportMutex);
    return g_exportDoc;
}

std::string jstr(JNIEnv* e, jstring s) {
    if (!s) return {};
    const char* c = e->GetStringUTFChars(s, nullptr);
    std::string out = c ? c : "";
    e->ReleaseStringUTFChars(s, c);
    return out;
}

jstring jout(JNIEnv* e, const std::string& s) { return e->NewStringUTF(s.c_str()); }

json err(const std::string& m) { return {{"ok", false}, {"error", m}}; }
json okj(json extra = json::object()) {
    extra["ok"] = true;
    return extra;
}

std::string safeName(const std::string& n) {
    std::string out;
    for (char c : n) out += (std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == ' ') ? c : '_';
    if (out.empty()) out = "untitled";
    return out.substr(0, 64);
}

std::string dataPath(const char* sub) { return pathJoin(g_cfg.dataDir, sub); }

json readJsonFile(const std::string& p) {
    bool ok;
    std::string t = readFileText(p, &ok);
    if (!ok) return json();
    json j = json::parse(t, nullptr, false);
    return j.is_discarded() ? json() : j;
}

json registries() {
    json fx = json::array();
    auto params = [](const std::vector<ParamInfo>& ps) {
        json a = json::array();
        for (auto& p : ps)
            a.push_back({{"name", p.name}, {"label", p.label}, {"kind", p.kind}, {"def", p.def}, {"min", p.min}, {"max", p.max}, {"options", p.options}, {"help", p.help}});
        return a;
    };
    for (auto& e : effectRegistry())
        fx.push_back({{"type", e.type}, {"name", e.name}, {"category", e.category}, {"help", e.help}, {"cost", e.costWeight}, {"time", e.timeEffect}, {"params", params(e.params)}});
    // Composite effects contributed by enabled extensions (category "Extensions").
    for (auto& [type, def] : compositeEffects())
        fx.push_back({{"type", type}, {"name", def.value("name", type)}, {"category", "Extensions"}, {"help", def.value("description", std::string())},
                      {"cost", 2}, {"time", false}, {"params", json::array()}});
    json bh = json::array();
    for (auto& b : behaviorRegistry()) bh.push_back({{"type", b.type}, {"name", b.name}, {"help", b.help}, {"params", params(b.params)}});
    json tr = json::array();
    for (auto& t : transitionRegistry()) tr.push_back({{"type", t.type}, {"name", t.name}, {"params", params(t.params)}});
    return {{"effects", fx},
            {"behaviors", bh},
            {"transitions", tr},
            {"blendModes", blendModeNames()},
            {"layerKinds", layerKinds()},
            {"easings", easingPresetNames()},
            {"ops", opNames()},
            {"textPresets", {"typeOn", "fadeUp", "pop", "bounce", "wave", "scramble", "kineticWords", "letterRotate", "maskedReveal", "blurIn"}},
            {"particlePresets", {"sparks", "dust", "snow", "rain", "smoke", "magic", "embers", "stars", "confetti"}},
            {"speedRamps", {"cinematic", "impact", "montage", "acceleration", "deceleration", "snap"}},
            {"primitives", {"cube", "sphere", "plane", "torus", "cylinder", "cone"}},
            {"permissions", allPermissions()}};
}

// ---------------------------------------------------------------- extensions
json loadInstalledExtensions() {
    clearCompositeEffects();
    json list = json::array();
    std::string root = dataPath("extensions");
    for (auto& id : listDir(root)) {
        json m = readJsonFile(pathJoin(pathJoin(root, id), "manifest.json"));
        if (!m.is_object()) continue;
        bool enabled = !fileExists(pathJoin(pathJoin(root, id), "disabled"));
        m["_enabled"] = enabled;
        m["_id"] = id;
        if (enabled) {
            json content = readJsonFile(pathJoin(pathJoin(root, id), "content.json"));
            for (auto& fx : jarr(content, "effects")) registerCompositeEffect("ext." + id + "." + fx.value("id", std::string("fx")), fx);
        }
        list.push_back(m);
    }
    return list;
}

json installExtension(const json& a) {
    PackageReadOptions ro;
    ro.developerMode = a.value("devMode", false);
    for (auto& k : jarr(a, "trustedKeys")) ro.trustedKeys.push_back(k);
    for (auto& k : jarr(a, "revokedKeys")) ro.revokedKeys.push_back(k);
    PackageReadResult r = readPackage(a.value("path", std::string()), ro);
    if (!r.ok) return err(r.error);
    if (r.pkg.kind != "extension") return err("This file is not an extension package (.mfext).");
    const json& m = r.pkg.manifest;
    std::string id = m.value("id", std::string());
    if (id.empty() || id.find('/') != std::string::npos || id.find("..") != std::string::npos) return err("Extension manifest has no valid id.");
    if (m.value("apiVersion", 1) > kExtensionApiVersion) return err("Extension requires a newer extension API (incompatible).");
    std::vector<std::string> perms = m.value("permissions", std::vector<std::string>());
    if (std::find(perms.begin(), perms.end(), "NATIVE_CODE") != perms.end() || r.pkg.files.count("native/arm64-v8a/libextension.so"))
        return err("Native-code extensions are not supported by this build. Use script, preset or composite-effect extensions.");
    if (!r.signedPkg && !ro.developerMode) return err("Unsigned extensions can only be installed in Developer Mode.");
    if (r.signedPkg && !r.trusted && !ro.developerMode) return err("Extension is signed by an untrusted publisher. Trust the publisher key in Settings > Developer first.");
    if (!a.value("confirmed", false)) {
        // First call: return manifest + permissions for the explicit approval dialog (never install silently).
        return okj({{"needsApproval", true}, {"manifest", m}, {"permissions", perms}, {"signed", r.signedPkg}, {"trusted", r.trusted}, {"signer", r.signer}});
    }
    if (g_engine) g_engine->checkpoint("before extension install");
    std::string dir = pathJoin(dataPath("extensions"), id);
    removeTree(dir);
    makeDirs(dir);
    atomicWriteFile(pathJoin(dir, "manifest.json"), m.dump(2));
    for (auto& [p, d] : r.pkg.files) {
        if (p.find("..") != std::string::npos) continue;
        std::string out = pathJoin(dir, p);
        makeDirs(pathDirname(out));
        atomicWriteFile(out, std::string(d.begin(), d.end()));
    }
    // Validate composite effect definitions reference only built-in effects.
    json content = readJsonFile(pathJoin(dir, "content.json"));
    for (auto& fx : jarr(content, "effects"))
        for (auto& step : jarr(fx, "chain"))
            if (!findEffect(step.value("type", ""))) {
                removeTree(dir);
                return err("Extension effect uses unknown building block '" + step.value("type", std::string()) + "'.");
            }
    loadInstalledExtensions();
    return okj({{"installed", id}, {"manifest", m}});
}

// ---------------------------------------------------------------- capsules / presets / templates
void collectComps(const json& doc, const std::string& compId, std::set<std::string>& seen, json& out) {
    if (seen.count(compId)) return;
    seen.insert(compId);
    const json* c = findComp(doc, compId);
    if (!c) return;
    out.push_back(*c);
    for (auto& L : jarr(*c, "layers"))
        if (L.contains("precomp")) collectComps(doc, L["precomp"].value("comp", ""), seen, out);
}

json createCapsule(const json& a) {
    auto snap = g_engine->snapshot();
    const json* comp = activeComp(*snap);
    if (!comp) return err("No composition.");
    std::vector<std::string> ids = a.value("layers", std::vector<std::string>());
    if (ids.empty()) return err("Select the layers to package as a capsule.");
    json cc = newComp("CAPROOT", a.value("name", std::string("Capsule")), comp->value("width", 1920), comp->value("height", 1080), comp->value("fps", 30.0),
                      comp->value("duration", 10.0));
    cc["bg"] = {0.0, 0.0, 0.0, 0.0};
    std::set<std::string> sel(ids.begin(), ids.end());
    json nested = json::array();
    std::set<std::string> seen;
    for (auto& L : jarr(*comp, "layers")) {
        if (!sel.count(L.value("id", ""))) continue;
        json l = L;
        if (l["parent"].is_string() && !sel.count(l["parent"].get<std::string>())) l["parent"] = nullptr;
        if (l["matte"].is_object() && !sel.count(l["matte"].value("layer", ""))) l["matte"] = nullptr;
        cc["layers"].push_back(l);
        if (l.contains("precomp")) collectComps(*snap, l["precomp"].value("comp", ""), seen, nested);
    }
    json assets = json::array();
    std::set<std::string> assetIds;
    std::function<void(const json&)> scan = [&](const json& c) {
        for (auto& L : jarr(c, "layers"))
            if (L.contains("asset")) assetIds.insert(L.value("asset", ""));
    };
    scan(cc);
    for (auto& n : nested) scan(n);
    for (auto& id : assetIds)
        if (const json* as = findAsset(*snap, id)) assets.push_back(*as);
    json controls = json::array();
    for (auto& ctl : jarr(a, "controls")) {
        json c = ctl;
        if (!c.contains("default")) {
            const json* L = findLayer(cc, c.value("layer", ""));
            const json* P = L ? resolvePath(*L, c.value("path", "")) : nullptr;
            if (P) c["default"] = (P->is_object() && (P->contains("v") || P->contains("k"))) ? evalRaw(*P, 0).toJson() : *P;
        }
        controls.push_back(c);
    }
    json cap = {{"format", "mfcapsule"}, {"version", "1.0"}, {"name", a.value("name", std::string("Capsule"))}, {"comp", cc}, {"comps", nested},
                {"assets", assets}, {"controls", controls}, {"created", nowSeconds()}};
    atomicWriteFile(pathJoin(dataPath("capsules"), safeName(cap["name"]) + ".json"), cap.dump());
    return okj({{"capsule", cap}});
}

json listJsonDir(const char* sub) {
    json out = json::array();
    std::string d = dataPath(sub);
    for (auto& n : listDir(d)) {
        if (pathExtensionLower(n) != "json") continue;
        json j = readJsonFile(pathJoin(d, n));
        if (j.is_null()) continue;
        j["_file"] = n;
        out.push_back(j);
    }
    return out;
}

json savePreset(const json& a) {
    auto snap = g_engine->snapshot();
    const json* comp = activeComp(*snap);
    const json* L = comp ? findLayer(*comp, a.value("layer", "")) : nullptr;
    if (!L) return err("Select a layer to save its effects as a preset.");
    json inc = a.value("include", json{{"effects", true}});
    json p = {{"name", a.value("name", std::string("Preset"))}, {"tags", a.value("tags", json::array())}, {"created", nowSeconds()}, {"favorite", false}};
    if (inc.value("effects", true)) p["effects"] = jarr(*L, "effects");
    if (inc.value("behaviors", false)) p["behaviors"] = jarr(*L, "behaviors");
    if (inc.value("masks", false)) p["masks"] = jarr(*L, "masks");
    if (inc.value("transform", false)) p["transform"] = jobj(*L, "transform");
    if (inc.value("blend", false)) p["blend"] = L->value("blend", std::string("normal"));
    if (a.contains("preview")) p["preview"] = a["preview"];
    std::string file = safeName(p["name"]) + ".json";
    atomicWriteFile(pathJoin(dataPath("presets"), file), p.dump(2));
    return okj({{"file", file}, {"preset", p}});
}

json newFromTemplate(const json& a) {
    json t = readJsonFile(pathJoin(dataPath("templates"), a.value("file", std::string())));
    if (!t.is_object() || !t.contains("project")) return err("Template not found.");
    json doc = t["project"];
    json values = a.value("values", json::object());
    for (auto& ph : jarr(t, "placeholders")) {
        std::string name = ph.value("name", "");
        if (!values.contains(name)) continue;
        std::string kind = ph.value("kind", "text");
        json op;
        if (kind == "text") op = {{"op", "setText"}, {"layer", ph["layer"]}, {"content", values[name]}, {"comp", ph.value("comp", std::string("C1"))}};
        else if (kind == "color" || kind == "number") op = {{"op", "setProp"}, {"layer", ph["layer"]}, {"path", ph["path"]}, {"value", values[name]}, {"mode", "static"}, {"comp", ph.value("comp", std::string("C1"))}};
        else if (kind == "media") {
            OpResult r;
            try {
                doc = applyOp(doc, {{"op", "addAsset"}, {"asset", values[name]}}, &r);
                op = {{"op", "replaceSource"}, {"layer", ph["layer"]}, {"asset", r.data["asset"]}, {"comp", ph.value("comp", std::string("C1"))}};
            } catch (std::exception& e) { return err(e.what()); }
        }
        try {
            if (!op.is_null()) doc = applyOp(doc, op);
        } catch (std::exception& e) {
            return err(std::string("Template placeholder '") + name + "': " + e.what());
        }
    }
    doc["meta"]["name"] = a.value("name", t.value("name", std::string("From Template")));
    doc.erase("template");
    std::string e;
    std::string id = g_engine->createProjectFromDoc(doc, &e);
    if (id.empty()) return err(e);
    return okj({{"id", id}});
}

// ---------------------------------------------------------------- dispatcher
json call(const std::string& m, const json& a) {
    if (!g_engine) return err("Engine not initialized.");
    Engine& E = *g_engine;
    // Projects.
    if (m == "listProjects") {
        json out = json::array();
        for (auto& s : E.store().list()) out.push_back(s.toJson());
        return okj({{"projects", out}});
    }
    if (m == "createProject") {
        std::string e;
        std::string id = E.createProject(a.value("name", std::string("Untitled")), a.value("width", 1920), a.value("height", 1080), a.value("fps", 30.0),
                                         a.value("duration", 10.0), &e);
        return id.empty() ? err(e) : okj({{"id", id}});
    }
    if (m == "createProjectFromDoc") {
        std::string e;
        std::string id = E.createProjectFromDoc(a.value("doc", json::object()), &e);
        return id.empty() ? err(e) : okj({{"id", id}});
    }
    if (m == "openProject") {
        std::string e;
        g_media->clear();
        return E.openProject(a.value("id", std::string()), e, a.value("recover", false)) ? okj() : err(e);
    }
    if (m == "closeProject") { E.closeProject(); return okj(); }
    if (m == "setRenderOptions") {
        if (a.contains("useProxies")) g_useProxies = a.value("useProxies", true);
        return okj({{"useProxies", g_useProxies.load()}});
    }
    if (m == "simulateCrash") { E.abandonProjectForTesting(); return okj(); }
    if (m == "recoveryInfo") {
        RecoveryInfo r = E.store().checkRecovery(a.value("id", std::string()));
        return okj({{"available", r.available}, {"abnormalExit", r.abnormalExit}, {"entries", r.journalEntries}, {"checkpointTime", r.checkpointTime},
                    {"recoveredTime", r.recoveredTime}, {"message", r.message}});
    }
    if (m == "discardRecovery") {
        std::string id = a.value("id", std::string());
        atomicWriteFile(pathJoin(E.store().projectDir(id), "journal.log"), "#discarded\n");
        E.store().markClosed(id);
        return okj();
    }
    if (m == "renameProject") return E.store().rename(a.value("id", std::string()), a.value("name", std::string())) ? okj() : err("Rename failed.");
    if (m == "duplicateProject") {
        std::string e;
        std::string id = E.store().duplicate(a.value("id", std::string()), a.value("name", std::string("Copy")), &e);
        return id.empty() ? err(e) : okj({{"id", id}});
    }
    if (m == "deleteProject") return E.store().remove(a.value("id", std::string())) ? okj() : err("Delete failed.");
    if (m == "versions") return okj({{"versions", E.store().versions(a.value("id", std::string()))}});
    if (m == "restoreVersion") {
        json doc;
        std::string e, id = a.value("id", std::string());
        if (!E.store().loadVersion(id, a.value("version", std::string()), doc, e)) return err(e);
        if (E.projectId() == id) E.closeProject();
        if (!E.store().save(id, doc, e, true)) return err(e);
        return okj();
    }
    if (m == "footprint") return okj({{"bytes", E.store().footprint(a.value("id", std::string()))}});
    // Document / command layer.
    if (m == "doc") return okj({{"doc", *E.snapshot()}, {"state", E.stateJson()}});
    if (m == "apply") return E.apply(a).toJson();
    if (m == "preview") return E.previewOp(a).toJson();
    if (m == "commitPreview") return okj({{"committed", E.commitPreview(a.value("label", std::string("Edit")))}});
    if (m == "cancelPreview") { E.cancelPreview(); return okj(); }
    if (m == "undo") return okj({{"done", E.undo()}});
    if (m == "redo") return okj({{"done", E.redo()}});
    if (m == "jump") return okj({{"done", E.jumpHistory(a.value("index", 0))}});
    if (m == "history") return okj(E.historyJson());
    if (m == "historyPreview") return okj({{"doc", E.doc().stateAt(a.value("index", 0))}});
    if (m == "state") return okj({{"state", E.stateJson()}});
    if (m == "save") {
        std::string e;
        return E.save(e, a.value("version", false)) ? okj() : err(e);
    }
    if (m == "autosave") return okj({{"saved", E.autosaveTick(nowSeconds())}});
    if (m == "checkpoint") { E.checkpoint(a.value("reason", std::string())); return okj(); }
    if (m == "thumbnail") return okj({{"written", E.writeThumbnail(a.value("t", 0.0))}});
    // Queries.
    if (m == "hitTest") return okj({{"layer", E.hitTest(a.value("t", 0.0), a.value("x", 0.0), a.value("y", 0.0))}});
    if (m == "layerQuad") return okj({{"quad", E.layerQuad(a.value("layer", std::string()), a.value("t", 0.0))}});
    if (m == "keyframeTimes") {
        auto snap = E.snapshot();
        const json* comp = activeComp(*snap);
        if (!comp) return err("No composition.");
        const json* L = a.contains("layer") ? findLayer(*comp, a["layer"]) : nullptr;
        return okj({{"times", keyframeTimes(*comp, L)}});
    }
    if (m == "editPoints") {
        auto snap = E.snapshot();
        const json* comp = activeComp(*snap);
        return comp ? okj({{"times", editPoints(*comp)}}) : err("No composition.");
    }
    if (m == "evalProps") {
        // Evaluate many properties at once for the inspector: {layer, t, paths:[...]}
        auto snap = E.snapshot();
        const json* comp = activeComp(*snap);
        const json* L = comp ? findLayer(*comp, a.value("layer", "")) : nullptr;
        if (!L) return err("Layer not found.");
        json out = json::object();
        double lt = layerLocalTime(*L, a.value("t", 0.0));
        for (auto& p : jarr(a, "paths")) {
            const json* P = resolvePath(*L, p.get<std::string>());
            if (P) out[p.get<std::string>()] = (P->is_object() && (P->contains("v") || P->contains("k"))) ? evalRaw(*P, lt).toJson() : *P;
        }
        return okj({{"values", out}});
    }
    if (m == "sampleProp") {
        // Sample one property across comp time for the graph editor: {layer, path, t0, t1, n}
        auto snap = E.snapshot();
        const json* comp = activeComp(*snap);
        const json* L = comp ? findLayer(*comp, a.value("layer", "")) : nullptr;
        if (!L) return err("Layer not found.");
        const json* P = resolvePath(*L, a.value("path", std::string()));
        if (!P || !P->is_object()) return err("Property not found.");
        int n = std::clamp(a.value("n", 100), 2, 1000);
        double t0 = a.value("t0", 0.0), t1 = a.value("t1", 1.0);
        json vals = json::array();
        for (int i = 0; i < n; ++i) {
            double t = t0 + (t1 - t0) * i / (n - 1);
            vals.push_back(evalRaw(*P, layerLocalTime(*L, t)).toJson());
        }
        return okj({{"values", vals}});
    }
    if (m == "copyKeyframes") {
        auto snap = E.snapshot();
        const json* comp = activeComp(*snap);
        const json* L = comp ? findLayer(*comp, a.value("layer", "")) : nullptr;
        if (!L) return err("Layer not found.");
        const json* P = resolvePath(*L, a.value("path", std::string()));
        if (!P || !P->is_object()) return err("Property not found.");
        double t0 = layerLocalTime(*L, a.value("t0", 0.0)), t1 = layerLocalTime(*L, a.value("t1", 1e9));
        json clip = keyframesClipboard(*P, t0, t1);
        if (jarr(clip, "k").empty()) return err("No keyframes in that range to copy.");
        clip["t0"] = clip["k"].front().value("t", 0.0);
        clip["t1"] = clip["k"].back().value("t", 0.0);
        return okj({{"clip", clip}});
    }
    if (m == "compToLayer") {
        auto snap = E.snapshot();
        const json* comp = activeComp(*snap);
        const json* L = comp ? findLayer(*comp, a.value("layer", "")) : nullptr;
        if (!L) return err("Layer not found.");
        Mat4 w = E.renderer().layerWorldMatrix(*comp, *L, a.value("t", 0.0)), inv;
        if (!w.inverse(inv)) return err("Layer transform is not invertible (zero scale?).");
        json pts = json::array();
        for (auto& p : jarr(a, "points")) {
            Vec3 q = inv.transformPoint({p[0].get<double>(), p[1].get<double>(), 0});
            pts.push_back({q.x, q.y});
        }
        return okj({{"points", pts}});
    }
    if (m == "registries") return okj(registries());
    if (m == "diagnostics") return okj({{"items", E.diagnostics()}});
    if (m == "capabilities") return okj({{"caps", E.capabilities()}});
    if (m == "validate") return okj({{"problems", validateProject(*E.snapshot())}});
    if (m == "clearCaches") { E.clearCaches(); g_media->clear(); return okj(); }
    if (m == "memoryPressure") { E.renderer().clearCaches(); g_media->clear(); return okj(); }
    // Fonts.
    if (m == "registerFont") {
        std::string e;
        std::string name = a.value("name", std::string());
        if (!FontManager::instance().registerFile(a.value("path", std::string()), name, &e)) return err(e);
        return okj({{"fonts", FontManager::instance().listJson()}});
    }
    if (m == "unregisterFont") return okj({{"removed", FontManager::instance().unregister(a.value("name", std::string()))}});
    if (m == "fonts") return okj({{"fonts", FontManager::instance().listJson()}});
    // Captions.
    if (m == "parseSubtitles") {
        std::string e;
        auto items = parseSubtitles(a.value("text", std::string()), a.value("ext", std::string()), &e);
        if (items.empty()) return err(e.empty() ? "No captions found in file." : e);
        return okj({{"items", captionsToJson(items)}});
    }
    if (m == "exportSubtitles") {
        auto snap = E.snapshot();
        const json* comp = activeComp(*snap);
        std::vector<CaptionItem> items;
        json style = defaultCaptionStyle();
        for (auto& L : jarr(*comp, "layers"))
            if (L.value("type", "") == "captions" && (!a.contains("layer") || L.value("id", "") == a.value("layer", ""))) {
                items = captionsFromJson(jarr(L["captions"], "items"));
                style = jobj(L["captions"], "style");
                break;
            }
        if (items.empty()) return err("There are no captions to export.");
        std::string f = a.value("format", std::string("srt"));
        std::string text = f == "vtt" ? toVtt(items) : f == "ass" ? toAss(items, style, comp->value("width", 1920), comp->value("height", 1080)) : toSrt(items);
        return okj({{"text", text}});
    }
    if (m == "readingSpeed") {
        auto snap = E.snapshot();
        const json* comp = activeComp(*snap);
        json out = json::array();
        for (auto& L : jarr(*comp, "layers"))
            if (L.value("type", "") == "captions") {
                auto items = captionsFromJson(jarr(L["captions"], "items"));
                for (auto& i : validateReadingSpeed(items, a.value("maxCps", 20.0))) out.push_back({{"index", i.index}, {"cps", i.cps}, {"message", i.message}});
            }
        return okj({{"issues", out}});
    }
    if (m == "speechRanges") {
        auto snap = E.snapshot();
        const json* comp = activeComp(*snap);
        const json* L = comp ? findLayer(*comp, a.value("layer", "")) : nullptr;
        if (!L) return err("Choose the dialogue layer.");
        auto rs = E.renderer().audio().speechRanges(*snap, *comp, *L);
        json out = json::array();
        for (auto& r : rs) out.push_back({r.first, r.second});
        return okj({{"ranges", out}});
    }
    if (m == "peaks") {
        auto snap = E.snapshot();
        const json* as = findAsset(*snap, a.value("asset", ""));
        if (!as) return err("Asset not found.");
        return okj({{"peaks", E.renderer().audio().peaks(*as, a.value("buckets", 200))}});
    }
    if (m == "modelInfo") return okj({{"info", asrModelInfo(a.value("path", std::string()))}});
    // Scripts / expressions.
    if (m == "runScript") {
        ScriptRequest req;
        req.name = a.value("name", std::string("script"));
        req.source = a.value("source", std::string());
        req.permissions = a.value("permissions", std::vector<std::string>());
        req.project = *E.snapshot();
        req.compId = E.activeCompId();
        req.selection = a.value("selection", std::vector<std::string>());
        req.playhead = a.value("t", 0.0);
        req.storage = a.value("storage", json::object());
        req.args = a.value("args", json::object());
        req.timeoutSec = a.value("timeout", 10.0);
        ScriptResult r = runScript(req);
        bool committed = false;
        if (r.ok && r.changed) committed = E.doc().commit("Run Script: " + req.name, r.project);
        return {{"ok", r.ok}, {"error", r.error}, {"line", r.line}, {"logs", r.logs}, {"alerts", r.alerts}, {"changed", committed}, {"ops", r.opsApplied},
                {"actions", r.actions}, {"storage", r.storage}, {"returnValue", r.returnValue}, {"ms", r.ms}};
    }
    if (m == "validateScript") {
        std::string e;
        int line = 0;
        bool ok = validateScriptSyntax(a.value("source", std::string()), e, line);
        return {{"ok", ok}, {"error", e}, {"line", line}};
    }
    if (m == "scriptApi") return okj({{"api", scriptApiDescription()}});
    if (m == "exampleScripts") return okj({{"scripts", exampleScripts()}});
    if (m == "validateExpression") {
        std::string e;
        bool ok = E.renderer().expressions()->validate(a.value("source", std::string()), e);
        return {{"ok", ok}, {"error", e}};
    }
    // Packages.
    if (m == "exportProject") {
        auto snap = E.snapshot();
        std::string mode = a.value("mode", std::string("collected"));
        PackageWriteOptions wo;
        wo.password = a.value("password", std::string());
        wo.signingKey = a.value("signingKey", std::string());
        std::string e;
        auto assetPath = [&](const json& as) { return E.media()->localPath(as).empty() ? as.value("cachePath", std::string()) : E.media()->localPath(as); };
        auto fontPath = [&](const std::string& f) { auto font = FontManager::instance().get(f); return font ? font->path : std::string(); };
        bool ok = exportProjectPackage(*snap, a.value("path", std::string()),
                                       mode == "linked" ? ProjectPackMode::Linked : mode == "portable" ? ProjectPackMode::Portable : ProjectPackMode::Collected,
                                       assetPath, fontPath, wo, e, E.store().thumbnailPath(E.projectId()));
        return ok ? okj() : err(e);
    }
    if (m == "importProject") {
        PackageReadOptions ro;
        ro.password = a.value("password", std::string());
        std::string e;
        json doc;
        std::vector<std::string> warnings;
        std::string mediaDir = pathJoin(dataPath("media"), formatString("import-%lld", (long long)(nowSeconds() * 1000)));
        if (!importProjectPackage(a.value("path", std::string()), mediaDir, doc, warnings, ro, e)) return err(e);
        // Register embedded fonts.
        for (auto& f : listDir(mediaDir)) {
            std::string ext = pathExtensionLower(f);
            if (ext == "ttf" || ext == "otf") FontManager::instance().registerFile(pathJoin(mediaDir, f));
        }
        std::string id = E.createProjectFromDoc(doc, &e);
        if (id.empty()) return err(e);
        return okj({{"id", id}, {"warnings", warnings}});
    }
    if (m == "packageEncrypted") return okj({{"encrypted", isPackageEncrypted(a.value("path", std::string()))}});
    if (m == "exportJsonPackage") {
        PackageWriteOptions wo;
        wo.password = a.value("password", std::string());
        wo.signingKey = a.value("signingKey", std::string());
        std::string e;
        return exportJsonPackage(a.value("path", std::string()), a.value("kind", std::string()), a.value("content", json::object()),
                                 a.value("manifest", json::object()), wo, e)
                   ? okj()
                   : err(e);
    }
    if (m == "importJsonPackage") {
        PackageReadOptions ro;
        ro.password = a.value("password", std::string());
        ro.developerMode = a.value("devMode", false);
        for (auto& k : jarr(a, "trustedKeys")) ro.trustedKeys.push_back(k);
        json content, manifest;
        std::string e;
        std::vector<std::string> w;
        if (!importJsonPackage(a.value("path", std::string()), a.value("kind", std::string()), content, manifest, ro, e, &w)) return err(e);
        return okj({{"content", content}, {"manifest", manifest}, {"warnings", w}});
    }
    if (m == "packageInfo") {
        PackageReadOptions ro;
        ro.developerMode = true;
        ro.password = a.value("password", std::string());
        PackageReadResult r = readPackage(a.value("path", std::string()), ro);
        return {{"ok", r.ok}, {"error", r.error}, {"kind", r.pkg.kind}, {"manifest", r.pkg.manifest}, {"signed", r.signedPkg}, {"signatureValid", r.signatureValid},
                {"signer", r.signer}, {"encrypted", r.encrypted}, {"warnings", r.warnings}, {"files", r.pkg.files.size()}};
    }
    if (m == "keygen") {
        std::string sk, pk;
        generateSigningKey(sk, pk);
        return okj({{"secret", sk}, {"public", pk}});
    }
    // Extensions.
    if (m == "installExtension") return installExtension(a);
    if (m == "extensions") return okj({{"extensions", loadInstalledExtensions()}});
    if (m == "uninstallExtension") {
        std::string id = a.value("id", std::string());
        if (id.empty() || id.find('/') != std::string::npos) return err("Invalid id.");
        removeTree(pathJoin(dataPath("extensions"), id));
        loadInstalledExtensions();
        return okj();
    }
    if (m == "setExtensionEnabled") {
        std::string flag = pathJoin(pathJoin(dataPath("extensions"), a.value("id", std::string())), "disabled");
        if (a.value("enabled", true)) ::unlink(flag.c_str());
        else atomicWriteFile(flag, "1");
        loadInstalledExtensions();
        return okj();
    }
    // Capsules / presets / templates.
    if (m == "createCapsule") return createCapsule(a);
    if (m == "capsules") return okj({{"capsules", listJsonDir("capsules")}});
    if (m == "deleteCapsule") { ::unlink(pathJoin(dataPath("capsules"), a.value("file", std::string())).c_str()); return okj(); }
    if (m == "saveCapsuleJson") {
        json cap = a.value("capsule", json::object());
        atomicWriteFile(pathJoin(dataPath("capsules"), safeName(cap.value("name", std::string("Capsule"))) + ".json"), cap.dump());
        return okj();
    }
    if (m == "exportCapsulePackage") {
        // {path, capsule, assetFiles:{assetId: localPath}, password?, signingKey?}
        json files = jobj(a, "assetFiles");
        PackageWriteOptions wo;
        wo.password = a.value("password", std::string());
        wo.signingKey = a.value("signingKey", std::string());
        std::string e;
        std::vector<std::string> warnings;
        auto assetPath = [&](const json& as) {
            std::string id = as.value("id", std::string());
            if (files.contains(id)) return files[id].get<std::string>();
            return as.value("path", std::string());
        };
        auto fontPath = [&](const std::string& f) { auto font = FontManager::instance().get(f); return font ? font->path : std::string(); };
        if (!exportCapsulePackage(a.value("path", std::string()), jobj(a, "capsule"), assetPath, fontPath, wo, e, &warnings)) return err(e);
        return okj({{"warnings", warnings}});
    }
    if (m == "importCapsulePackage") {
        PackageReadOptions ro;
        ro.password = a.value("password", std::string());
        ro.developerMode = true;
        json cap;
        std::vector<std::string> fonts, warnings;
        std::string e;
        std::string dir = pathJoin(dataPath("capsule-media"), formatString("c%lld", (long long)(nowSeconds() * 1000)));
        if (!importCapsulePackage(a.value("path", std::string()), dir, cap, fonts, ro, e, &warnings)) return err(e);
        for (auto& f : fonts) {
            std::string fe;
            if (!FontManager::instance().registerFile(f, std::string(), &fe)) warnings.push_back("Font not registered: " + fe);
        }
        atomicWriteFile(pathJoin(dataPath("capsules"), safeName(cap.value("name", std::string("Capsule"))) + ".json"), cap.dump());
        return okj({{"capsule", cap}, {"warnings", warnings}, {"fonts", fonts.size()}});
    }
    if (m == "savePreset") return savePreset(a);
    if (m == "presets") return okj({{"presets", listJsonDir("presets")}});
    if (m == "updatePreset") {
        std::string f = a.value("file", std::string());
        json p = readJsonFile(pathJoin(dataPath("presets"), f));
        if (!p.is_object()) return err("Preset not found.");
        json fields = jobj(a, "fields");
        for (auto& [k, v] : fields.items()) p[k] = v;
        std::string nf = a.contains("fields") && a["fields"].contains("name") ? safeName(p["name"]) + ".json" : f;
        if (nf != f) ::unlink(pathJoin(dataPath("presets"), f).c_str());
        atomicWriteFile(pathJoin(dataPath("presets"), nf), p.dump(2));
        return okj({{"file", nf}});
    }
    if (m == "duplicatePreset") {
        json p = readJsonFile(pathJoin(dataPath("presets"), a.value("file", std::string())));
        if (!p.is_object()) return err("Preset not found.");
        p["name"] = p.value("name", std::string("Preset")) + " copy";
        atomicWriteFile(pathJoin(dataPath("presets"), safeName(p["name"]) + ".json"), p.dump(2));
        return okj();
    }
    if (m == "deletePreset") { ::unlink(pathJoin(dataPath("presets"), a.value("file", std::string())).c_str()); return okj(); }
    if (m == "saveTemplate") {
        json t = {{"name", a.value("name", std::string("Template"))}, {"placeholders", a.value("placeholders", json::array())}, {"project", *E.snapshot()}, {"created", nowSeconds()}};
        atomicWriteFile(pathJoin(dataPath("templates"), safeName(t["name"]) + ".json"), t.dump());
        return okj();
    }
    if (m == "templates") {
        json list = listJsonDir("templates");
        for (auto& t : list) t.erase("project");
        return okj({{"templates", list}});
    }
    if (m == "newFromTemplate") return newFromTemplate(a);
    if (m == "checkLut") {
        LutData l;
        std::string e;
        return l.load(a.value("path", std::string()), e) ? okj({{"size", l.size}, {"title", l.title}}) : err(e);
    }
    if (m == "runScenario") {
        ScenarioResult r = runScenario(E, a.value("script", std::string()), a.value("workDir", g_cfg.cacheDir + "/scenario"));
        return {{"ok", r.ok}, {"error", r.error}, {"log", r.log}, {"outputs", r.outputs}};
    }
    if (m == "exportBegin") {
        std::lock_guard<std::mutex> lk(g_exportMutex);
        g_exportDoc = E.snapshot();
        return okj();
    }
    if (m == "exportEnd") {
        std::lock_guard<std::mutex> lk(g_exportMutex);
        g_exportDoc.reset();
        return okj();
    }
    if (m == "setLogLevel") { setLogLevel((LogLevel)a.value("level", 1)); return okj(); }
    return err("Unknown native method '" + m + "'.");
}

struct Progress {
    JNIEnv* env;
    jobject cb;
    jmethodID mid;
    bool report(float p) {
        if (!cb) return true;
        jboolean r = env->CallBooleanMethod(cb, mid, (jfloat)p);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        return r;
    }
};

}  // namespace

extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    mfa::setVm(vm);
    return JNI_VERSION_1_6;
}

JNIEXPORT jstring JNICALL Java_com_motionforge_app_engine_NativeBridge_nativeInit(JNIEnv* e, jclass, jstring cfgJson) {
    std::lock_guard<std::mutex> lk(g_initMutex);
    json c = json::parse(jstr(e, cfgJson), nullptr, false);
    if (c.is_discarded()) return jout(e, err("bad config").dump());
    if (!g_engine) {
        setLogSink([](LogLevel l, const std::string& msg) {
            __android_log_write(l == LogLevel::Error ? ANDROID_LOG_ERROR : l == LogLevel::Warn ? ANDROID_LOG_WARN : ANDROID_LOG_INFO, "MotionForge", msg.c_str());
        });
        setLogLevel(c.value("developer", false) ? LogLevel::Debug : LogLevel::Warn);
        ThreadPool::setThreadCount(c.value("threads", 4));
        g_cfg.dataDir = c.value("dataDir", std::string());
        g_cfg.cacheDir = c.value("cacheDir", std::string());
        g_cfg.fontsDir = c.value("fontsDir", std::string());
        g_cfg.modelsDir = c.value("modelsDir", std::string());
        g_cfg.autosaveInterval = c.value("autosaveInterval", 15.0);
        for (auto& d : {g_cfg.dataDir, g_cfg.cacheDir, g_cfg.fontsDir, g_cfg.modelsDir}) makeDirs(d);
        g_media = std::make_unique<mfa::AndroidMediaProvider>(e);
        g_engine = std::make_unique<Engine>(g_cfg, g_media.get());
        FontManager::instance().registerDirectory(c.value("bundledFontsDir", std::string()));
        FontManager::instance().registerDirectory(g_cfg.fontsDir);
        loadInstalledExtensions();
    }
    return jout(e, okj({{"caps", g_engine->capabilities()}}).dump());
}

JNIEXPORT jstring JNICALL Java_com_motionforge_app_engine_NativeBridge_nativeCall(JNIEnv* e, jclass, jstring method, jstring args) {
    std::string m = jstr(e, method);
    json a = json::parse(jstr(e, args), nullptr, false);
    if (a.is_discarded()) a = json::object();
    json r;
    try {
        r = call(m, a);
    } catch (std::exception& ex) {
        r = err(std::string("Internal error in ") + m + ": " + ex.what());
    }
    return jout(e, r.dump());
}

JNIEXPORT jstring JNICALL Java_com_motionforge_app_engine_NativeBridge_nativeRenderBitmap(JNIEnv* e, jclass, jobject bitmap, jdouble t, jboolean draft,
                                                                                           jboolean exportMode) {
    if (!g_engine) return jout(e, "{}");
    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(e, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS) return jout(e, err("bitmap").dump());
    auto snap = g_engine->snapshot();
    const json* comp = activeComp(*snap);
    if (!comp) return jout(e, err("no comp").dump());
    double scale = (double)info.width / std::max(1, comp->value("width", 1920));
    RenderSettings rs;
    rs.draft = draft;
    rs.exportMode = exportMode;
    rs.useProxies = g_useProxies.load();
    RenderStats st;
    Image img = g_engine->render(t, scale, rs, &st);
    void* px = nullptr;
    if (AndroidBitmap_lockPixels(e, bitmap, &px) != ANDROID_BITMAP_RESULT_SUCCESS) return jout(e, err("lock").dump());
    int h = std::min<int>(img.h, (int)info.height), w = std::min<int>(img.w, (int)info.width);
    for (int y = 0; y < h; ++y) std::memcpy((uint8_t*)px + (size_t)y * info.stride, img.row(y), (size_t)w * 4);
    AndroidBitmap_unlockPixels(e, bitmap);
    return jout(e, json{{"ms", st.ms}, {"layers", st.layersRendered}, {"passes", st.passes}, {"cacheHits", st.cacheHits}, {"cacheMisses", st.cacheMisses},
                        {"proxyFrames", st.proxyFrames}, {"warnings", st.warnings}}.dump());
}

JNIEXPORT jboolean JNICALL Java_com_motionforge_app_engine_NativeBridge_nativeRenderRgba(JNIEnv* e, jclass, jobject buffer, jint w, jint h, jdouble t) {
    if (!g_engine) return false;
    uint8_t* dst = (uint8_t*)e->GetDirectBufferAddress(buffer);
    jlong cap = e->GetDirectBufferCapacity(buffer);
    if (!dst || cap < (jlong)w * h * 4) return false;
    auto frozen = exportDoc();
    auto snap = frozen ? frozen : g_engine->snapshot();
    const json* comp = activeComp(*snap);
    if (!comp) return false;
    double scale = (double)w / std::max(1, comp->value("width", 1920));
    RenderSettings rs;
    rs.exportMode = true;
    rs.scale = scale;
    Image img = g_engine->renderer().renderFrame(*snap, comp->value("id", ""), t, rs);
    if (img.w != w || img.h != h) img = resizeImage(img, w, h);
    std::memcpy(dst, img.px.data(), (size_t)w * h * 4);
    return true;
}

JNIEXPORT jfloat JNICALL Java_com_motionforge_app_engine_NativeBridge_nativeMixAudio(JNIEnv* e, jclass, jdouble t0, jint frames, jfloatArray out,
                                                                                     jint sampleRate) {
    if (!g_engine) return 0;
    std::vector<float> buf((size_t)frames * 2);
    float peak;
    if (auto frozen = exportDoc()) {
        const json* comp = activeComp(*frozen);
        g_engine->renderer().audio().setProject(frozen.get());
        peak = comp ? g_engine->renderer().audio().mix(*frozen, *comp, t0, frames, buf.data(), sampleRate) : 0.f;
    } else {
        peak = g_engine->mixAudio(t0, frames, buf.data(), sampleRate);
    }
    e->SetFloatArrayRegion(out, 0, frames * 2, buf.data());
    return peak;
}

JNIEXPORT void JNICALL Java_com_motionforge_app_engine_NativeBridge_nativeResetAudio(JNIEnv*, jclass) {
    if (g_engine) g_engine->renderer().audio().resetState();
}

JNIEXPORT jboolean JNICALL Java_com_motionforge_app_engine_NativeBridge_nativeYuvToBitmap(JNIEnv* e, jclass, jobject y, jobject u, jobject v, jint yStride,
                                                                                          jint uvStride, jint uvPixelStride, jint w, jint h, jboolean bt709,
                                                                                          jboolean fullRange, jobject bitmap) {
    auto* yp = (const uint8_t*)e->GetDirectBufferAddress(y);
    auto* up = (const uint8_t*)e->GetDirectBufferAddress(u);
    auto* vp = (const uint8_t*)e->GetDirectBufferAddress(v);
    if (!yp || !up || !vp) return false;
    Image img;
    yuv420ToRgba(yp, yStride, up, vp, uvStride, uvPixelStride, w, h, bt709, fullRange, img);
    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(e, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS) return false;
    void* px = nullptr;
    if (AndroidBitmap_lockPixels(e, bitmap, &px) != ANDROID_BITMAP_RESULT_SUCCESS) return false;
    if ((int)info.width == w && (int)info.height == h) {
        for (int yy = 0; yy < h; ++yy) std::memcpy((uint8_t*)px + (size_t)yy * info.stride, img.row(yy), (size_t)w * 4);
    } else {
        Image r = resizeImage(img, (int)info.width, (int)info.height);
        for (int yy = 0; yy < (int)info.height; ++yy) std::memcpy((uint8_t*)px + (size_t)yy * info.stride, r.row(yy), (size_t)info.width * 4);
    }
    AndroidBitmap_unlockPixels(e, bitmap);
    return true;
}

JNIEXPORT jstring JNICALL Java_com_motionforge_app_engine_NativeBridge_nativeLongTask(JNIEnv* e, jclass, jstring method, jstring args, jobject callback) {
    if (!g_engine) return jout(e, err("Engine not initialized.").dump());
    std::string m = jstr(e, method);
    json a = json::parse(jstr(e, args), nullptr, false);
    if (a.is_discarded()) a = json::object();
    Progress pr{e, callback, nullptr};
    if (callback) {
        jclass cls = e->GetObjectClass(callback);
        pr.mid = e->GetMethodID(cls, "onProgress", "(F)Z");
    }
    Engine& E = *g_engine;
    json out;
    try {
        if (m == "transcribe") {
            AsrOptions opt;
            opt.modelPath = a.value("model", E.defaultModelPath());
            opt.language = a.value("language", std::string("auto"));
            opt.threads = a.value("threads", 4);
            opt.translate = a.value("translate", false);
            opt.useVad = a.value("vad", true);
            opt.progress = [&](float p) { return pr.report(p); };
            SegmentOptions so;
            so.maxChars = a.value("maxChars", 42);
            so.maxLines = a.value("maxLines", 2);
            so.maxCps = a.value("maxCps", 20.0);
            AsrResult r = E.transcribeLayer(a.value("layer", std::string()), opt, so);
            json res = {{"ok", r.ok}, {"error", r.error}, {"cancelled", r.cancelled}, {"language", r.language}, {"audioSeconds", r.audioSeconds},
                        {"processingSeconds", r.processingSeconds}, {"model", r.model}, {"words", (int)r.words.size()}};
            if (r.ok) {
                // Caption generation is one undoable transaction.
                OpOutcome o = E.apply({{"op", "setCaptions"}, {"items", captionsToJson(r.captions)}, {"language", r.language}, {"source", "whisper:" + r.model},
                                       {"label", "Generate Captions"}});
                res["ok"] = o.ok;
                if (!o.ok) res["error"] = o.error;
                res["captions"] = (int)r.captions.size();
                auto issues = validateReadingSpeed(r.captions, so.maxCps);
                res["readingIssues"] = (int)issues.size();
            }
            out = res;
        } else if (m == "track" || m == "stabilize") {
            auto snap = E.snapshot();
            const json* comp = activeComp(*snap);
            const json* L = comp ? findLayer(*comp, a.value("layer", "")) : nullptr;
            if (!L) throw std::runtime_error("Choose a video layer to track.");
            const json* as = findAsset(*snap, L->value("asset", ""));
            if (!as) throw std::runtime_error("The layer has no video.");
            json asset = *as, layer = *L;
            int fw = asset.value("width", 1280), fh = asset.value("height", 720);
            double scale = std::min(1.0, 640.0 / std::max(fw, 1));
            FrameFetcher ff = [&](double ct) {
                ImagePtr f = g_media->videoFrame(asset, layerSourceTime(layer, ct), (int)(fw * scale), (int)(fh * scale));
                if (f && (f->w != (int)(fw * scale))) return ImagePtr(std::make_shared<Image>(resizeImage(*f, (int)(fw * scale), (int)(fh * scale))));
                return f;
            };
            double fps = compFps(*comp);
            double t0 = a.value("t0", L->value("in", 0.0)), t1 = a.value("t1", L->value("out", 0.0) - 1.0 / fps);
            auto progress = [&](float p) { return pr.report(p); };
            if (m == "track") {
                std::string mode = a.value("mode", std::string("point"));
                TrackOptions to;
                to.patch = a.value("patch", 31);
                to.search = a.value("search", 40);
                // Point in comp space -> frame space via layer quad homography (2D layers).
                Vec2 q[4];
                E.renderer().layerQuad(*snap, comp->value("id", ""), L->value("id", ""), t0, q);
                Vec2 src[4] = {{0, 0}, {(double)fw, 0}, {(double)fw, (double)fh}, {0, (double)fh}};
                Mat3 compToFrame, frameToComp;
                Mat3::quadToQuad(q, src, compToFrame);
                compToFrame.inverse(frameToComp);
                auto toFrame = [&](double x, double y) { Vec2 p = compToFrame.apply(x, y); return p * scale; };
                auto toComp = [&](Vec2 p) { return frameToComp.apply(p.x / scale, p.y / scale); };
                json samples = json::array();
                std::string msg;
                if (mode == "twoPoint") {
                    auto tr = trackTwoPoints(ff, t0, t1, 1.0 / fps, toFrame(a.value("x", 0.0), a.value("y", 0.0)), toFrame(a.value("x2", 0.0), a.value("y2", 0.0)), to, progress);
                    for (auto& s : tr) {
                        Vec2 c = toComp(s.position);
                        samples.push_back({s.t, c.x, c.y, s.rotation, s.scale, s.confidence});
                    }
                } else {
                    auto tr = trackPoint(ff, t0, t1, 1.0 / fps, toFrame(a.value("x", 0.0), a.value("y", 0.0)), to, progress, &msg);
                    for (auto& s : tr) {
                        Vec2 c = toComp(s.p);
                        samples.push_back({s.t, c.x, c.y, s.confidence});
                    }
                }
                out = {{"ok", samples.size() > 1}, {"samples", samples}, {"message", msg}, {"error", samples.size() > 1 ? "" : (msg.empty() ? "Tracking failed." : msg)}};
            } else {
                auto st = stabilize(ff, t0, t1, 1.0 / fps, a.value("smoothing", 0.5), a.value("rotation", true), (int)(fw * scale), (int)(fh * scale), progress);
                json samples = json::array();
                for (auto& s : st) samples.push_back({s.t, s.offset.x / scale, s.offset.y / scale, s.rotation, s.scale});
                out = {{"ok", !samples.empty()}, {"samples", samples}};
            }
        } else if (m == "exportGif" || m == "exportPngSequence") {
            auto snap = E.snapshot();
            const json* comp = activeComp(*snap);
            if (!comp) throw std::runtime_error("No composition.");
            E.checkpoint("before export");
            double fps = a.value("fps", m == "exportGif" ? 15.0 : compFps(*comp));
            double t0 = a.value("t0", 0.0), t1 = a.value("t1", comp->value("duration", 1.0));
            int w = a.value("width", 480);
            double scale = (double)w / comp->value("width", 1920);
            int h = (int)std::lround(comp->value("height", 1080) * scale);
            int n = std::max(1, (int)std::floor((t1 - t0) * fps + 1e-6));
            std::string path = a.value("path", std::string());
            GifWriter gw;
            if (m == "exportGif" && !gw.open(path, w, h)) throw std::runtime_error("Cannot write the GIF file (storage full?).");
            if (m == "exportPngSequence") makeDirs(path);
            RenderSettings rs;
            rs.exportMode = true;
            rs.transparentBackground = a.value("transparent", false);
            bool cancelled = false;
            for (int i = 0; i < n; ++i) {
                Image img = E.render(t0 + i / fps, scale, rs);
                if (img.w != w || img.h != h) img = resizeImage(img, w, h);
                if (m == "exportGif") gw.addFrame(img, (int)std::lround(100.0 / fps), a.value("dither", true));
                else if (!savePng(img, pathJoin(path, formatString("frame_%05d.png", i)))) throw std::runtime_error("Failed writing PNG (storage full?).");
                if (!pr.report((float)(i + 1) / n)) { cancelled = true; break; }
            }
            if (m == "exportGif") gw.close();
            if (cancelled) {
                if (m == "exportGif") ::unlink(path.c_str());
                out = {{"ok", false}, {"cancelled", true}, {"error", "Export cancelled."}};
            } else {
                // Validate output.
                json v = {{"frames", n}};
                bool valid = true;
                if (m == "exportGif") {
                    int gw2, gh2, gn;
                    valid = inspectGif(path, gw2, gh2, gn) && gn == n && gw2 == w && gh2 == h;
                    v["validated"] = valid;
                } else {
                    valid = (int)listDir(path).size() >= n;
                }
                out = {{"ok", valid}, {"validation", v}, {"error", valid ? "" : "Output validation failed."}};
            }
        } else if (m == "exportWav") {
            auto snap = E.snapshot();
            const json* comp = activeComp(*snap);
            double t0 = a.value("t0", 0.0), t1 = a.value("t1", comp->value("duration", 1.0));
            int sr = a.value("sampleRate", 48000);
            size_t frames = (size_t)((t1 - t0) * sr);
            std::vector<float> buf(frames * 2);
            E.renderer().audio().resetState();
            for (size_t off = 0; off < frames; off += 4800) {
                int nfr = (int)std::min<size_t>(4800, frames - off);
                E.mixAudio(t0 + off / (double)sr, nfr, buf.data() + off * 2, sr);
                if (!pr.report((float)off / frames)) return jout(e, json{{"ok", false}, {"cancelled", true}}.dump());
            }
            bool ok = saveWav(a.value("path", std::string()), buf.data(), frames, 2, sr);
            out = {{"ok", ok}, {"error", ok ? "" : "Failed writing WAV."}};
        } else {
            out = err("Unknown task " + m);
        }
    } catch (std::exception& ex) {
        out = err(ex.what());
    }
    return jout(e, out.dump());
}

}  // extern "C"
