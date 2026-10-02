#include "mf/model.hpp"

#include <set>

#include "mf/behaviors.hpp"
#include "mf/document.hpp"
#include "mf/effects.hpp"

namespace mf {

// ====================================================================== factories
json defaultTransform(double ax, double ay, double px, double py) {
    return json{{"anchor", makeProp({ax, ay, 0.0})},
                {"position", makeProp({px, py, 0.0})},
                {"scale", makeProp({100.0, 100.0, 100.0})},
                {"rotation", makeProp(0.0)},
                {"rotationX", makeProp(0.0)},
                {"rotationY", makeProp(0.0)},
                {"opacity", makeProp(100.0)}};
}

json defaultCaptionStyle() {
    return json{{"name", "Clean"},
                {"font", "DejaVuSans-Bold"},
                {"size", 64},
                {"color", {1, 1, 1, 1}},
                {"strokeColor", {0, 0, 0, 1}},
                {"strokeWidth", 4},
                {"shadow", true},
                {"background", false},
                {"backgroundColor", {0, 0, 0, 0.6}},
                {"backgroundRadius", 16},
                {"position", {0.5, 0.86}},
                {"maxWidth", 0.84},
                {"highlightActiveWord", true},
                {"highlightColor", {1, 0.85, 0.1, 1}},
                {"animationIn", "fade"},
                {"animationOut", "fade"},
                {"animationDuration", 0.15},
                {"uppercase", false},
                {"speakerColors", json::array({json::array({1, 1, 1, 1}), json::array({0.6, 0.9, 1, 1}), json::array({1, 0.8, 0.6, 1})})}};
}

json newComp(const std::string& id, const std::string& name, int width, int height, double fps, double duration) {
    return json{{"id", id},
                {"name", name},
                {"width", width},
                {"height", height},
                {"fps", fps},
                {"duration", duration},
                {"bg", {0.0, 0.0, 0.0, 1.0}},
                {"layers", json::array()},
                {"markers", json::array()},
                {"guides", json::array()},
                {"workArea", {0.0, duration}},
                {"motionBlur", {{"enabled", false}, {"samples", 8}, {"shutter", 180}}},
                {"audio",
                 {{"buses",
                   {{"master", {{"gain", 0.0}, {"limiter", true}}},
                    {"music", {{"gain", 0.0}, {"duck", {{"enabled", false}, {"amount", -12.0}, {"attack", 0.15}, {"release", 0.4}}}}},
                    {"dialogue", {{"gain", 0.0}}},
                    {"effects", {{"gain", 0.0}}}}}}}};
}

json newProject(const std::string& name, int width, int height, double fps, double duration) {
    double now = nowSeconds();
    json doc = {{"format", "mforge"},
                {"formatVersion", kFormatVersion},
                {"engineVersion", kEngineVersion},
                {"meta", {{"name", name}, {"created", now}, {"modified", now}, {"author", ""}}},
                {"settings", {{"activeComp", "C1"}, {"workingColorSpace", "srgb"}, {"exportColorSpace", "srgb"}, {"mode", "linked"}}},
                {"assets", json::array()},
                {"comps", json::array({newComp("C1", "Main", width, height, fps, duration)})},
                {"captionStyles", json::array({defaultCaptionStyle()})},
                {"capsules", json::array()},
                {"nextId", 2}};
    return doc;
}

std::vector<std::string> layerKinds() {
    return {"solid", "text", "shape", "null", "adjustment", "camera", "light", "particles", "model3d", "captions",
            "image", "video", "audio", "precomp"};
}

static json particlePreset(const std::string& preset) {
    auto P = [](const json& v) { return makeProp(v); };
    json p = {{"preset", preset},
              {"rate", P(40.0)},
              {"lifetime", P(2.0)},
              {"velocity", P(200.0)},
              {"spread", P(60.0)},
              {"direction", P(-90.0)},
              {"gravity", P(0.0)},
              {"turbulence", P(0.0)},
              {"size", P(8.0)},
              {"sizeEnd", P(2.0)},
              {"opacityStart", P(100.0)},
              {"opacityEnd", P(0.0)},
              {"colorStart", P({1.0, 0.9, 0.5, 1.0})},
              {"colorEnd", P({1.0, 0.3, 0.1, 1.0})},
              {"spin", P(0.0)},
              {"emitterSize", P({0.0, 0.0})},
              {"shape", "circle"},
              {"seed", 1},
              {"trails", P(0.0)},
              {"maxParticles", 4000},
              {"glow", false},
              {"attractor", {{"enabled", false}, {"position", P({0.0, 0.0})}, {"strength", P(0.0)}}}};
    auto set = [&](const char* k, const json& v) { p[k] = P(v); };
    if (preset == "sparks") {
        set("rate", 120.0); set("lifetime", 0.9); set("velocity", 520.0); set("spread", 70.0); set("gravity", 900.0);
        set("size", 4.0); set("sizeEnd", 1.0); p["shape"] = "line"; set("trails", 0.04); p["glow"] = true;
    } else if (preset == "dust") {
        set("rate", 25.0); set("lifetime", 6.0); set("velocity", 15.0); set("spread", 360.0); set("turbulence", 40.0);
        set("size", 3.0); set("sizeEnd", 3.0); set("opacityStart", 50.0); set("opacityEnd", 0.0);
        set("colorStart", {1.0, 1.0, 1.0, 1.0}); set("colorEnd", {0.9, 0.9, 0.8, 1.0}); set("emitterSize", {1920.0, 1080.0});
    } else if (preset == "snow") {
        set("rate", 60.0); set("lifetime", 8.0); set("velocity", 80.0); set("direction", 90.0); set("spread", 20.0); set("turbulence", 60.0);
        set("size", 6.0); set("sizeEnd", 5.0); set("opacityStart", 90.0); set("opacityEnd", 60.0);
        set("colorStart", {1.0, 1.0, 1.0, 1.0}); set("colorEnd", {1.0, 1.0, 1.0, 1.0}); set("emitterSize", {2200.0, 0.0});
    } else if (preset == "rain") {
        set("rate", 300.0); set("lifetime", 1.2); set("velocity", 1400.0); set("direction", 100.0); set("spread", 2.0);
        set("size", 2.0); set("sizeEnd", 2.0); set("opacityStart", 60.0); set("opacityEnd", 60.0); p["shape"] = "line"; set("trails", 0.03);
        set("colorStart", {0.7, 0.8, 1.0, 1.0}); set("colorEnd", {0.7, 0.8, 1.0, 1.0}); set("emitterSize", {2400.0, 0.0});
    } else if (preset == "smoke") {
        set("rate", 20.0); set("lifetime", 4.0); set("velocity", 60.0); set("spread", 25.0); set("turbulence", 50.0); set("gravity", -20.0);
        set("size", 40.0); set("sizeEnd", 160.0); set("opacityStart", 35.0); set("opacityEnd", 0.0);
        set("colorStart", {0.6, 0.6, 0.6, 1.0}); set("colorEnd", {0.3, 0.3, 0.3, 1.0}); p["shape"] = "soft";
    } else if (preset == "magic") {
        set("rate", 80.0); set("lifetime", 1.8); set("velocity", 90.0); set("spread", 360.0); set("turbulence", 120.0);
        set("size", 7.0); set("sizeEnd", 0.0); set("colorStart", {0.6, 0.8, 1.0, 1.0}); set("colorEnd", {0.9, 0.4, 1.0, 1.0});
        p["shape"] = "star"; p["glow"] = true; set("spin", 180.0);
    } else if (preset == "embers") {
        set("rate", 30.0); set("lifetime", 4.0); set("velocity", 70.0); set("spread", 40.0); set("turbulence", 70.0); set("gravity", -40.0);
        set("size", 4.0); set("sizeEnd", 1.0); set("colorStart", {1.0, 0.7, 0.2, 1.0}); set("colorEnd", {1.0, 0.2, 0.0, 1.0});
        set("emitterSize", {1200.0, 0.0}); p["glow"] = true;
    } else if (preset == "stars") {
        set("rate", 15.0); set("lifetime", 5.0); set("velocity", 0.0); set("spread", 0.0); set("size", 5.0); set("sizeEnd", 5.0);
        set("opacityStart", 0.0); set("opacityEnd", 0.0); set("emitterSize", {1920.0, 1080.0}); p["shape"] = "star"; p["twinkle"] = true;
        set("colorStart", {1.0, 1.0, 1.0, 1.0}); set("colorEnd", {0.8, 0.9, 1.0, 1.0});
    } else if (preset == "confetti") {
        set("rate", 90.0); set("lifetime", 3.5); set("velocity", 700.0); set("spread", 50.0); set("gravity", 600.0); set("turbulence", 30.0);
        set("size", 14.0); set("sizeEnd", 14.0); set("opacityStart", 100.0); set("opacityEnd", 100.0); set("spin", 360.0);
        p["shape"] = "square"; p["multicolor"] = true;
    }
    return p;
}

json makeLayer(const std::string& kind, const std::string& id, const json& comp, const json& project, const json& opts) {
    int cw = comp.value("width", 1920), ch = comp.value("height", 1080);
    double dur = comp.value("duration", 10.0);
    double at = opts.value("at", 0.0);
    double len = opts.value("duration", std::max(0.1, std::min(5.0, dur - at)));
    json L = {{"id", id},
              {"name", opts.value("name", std::string())},
              {"type", kind},
              {"enabled", true},
              {"solo", false},
              {"locked", false},
              {"muted", false},
              {"label", 0},
              {"parent", nullptr},
              {"threeD", false},
              {"guide", false},
              {"start", at},
              {"in", at},
              {"out", at + len},
              {"speed", 1.0},
              {"reverse", false},
              {"freezeAt", nullptr},
              {"blend", "normal"},
              {"matte", nullptr},
              {"transform", defaultTransform(0, 0, cw / 2.0, ch / 2.0)},
              {"effects", json::array()},
              {"masks", json::array()},
              {"behaviors", json::array()},
              {"markers", json::array()},
              {"transitionIn", nullptr},
              {"transitionOut", nullptr}};
    auto defName = [&](const std::string& n) {
        if (L["name"].get<std::string>().empty()) L["name"] = n;
    };
    json audioDefaults = {{"volume", makeProp(0.0)}, {"pan", makeProp(0.0)}, {"bus", kind == "audio" ? "music" : "dialogue"},
                          {"fadeIn", 0.0}, {"fadeOut", 0.0}, {"eq", {{"low", 0.0}, {"mid", 0.0}, {"high", 0.0}}},
                          {"compressor", {{"enabled", false}, {"threshold", -18.0}, {"ratio", 3.0}}},
                          {"denoise", 0.0}, {"pitch", 0.0}};
    if (kind == "solid") {
        defName("Solid");
        int w = opts.value("width", cw), h = opts.value("height", ch);
        L["solid"] = {{"color", makeProp(opts.value("color", json({0.2, 0.4, 0.9, 1.0})))}, {"width", w}, {"height", h}};
        L["transform"] = defaultTransform(w / 2.0, h / 2.0, cw / 2.0, ch / 2.0);
    } else if (kind == "adjustment") {
        defName("Adjustment Layer");
        L["solid"] = {{"color", makeProp({1.0, 1.0, 1.0, 1.0})}, {"width", cw}, {"height", ch}};
        L["transform"] = defaultTransform(cw / 2.0, ch / 2.0, cw / 2.0, ch / 2.0);
    } else if (kind == "text") {
        std::string content = opts.value("text", std::string("Text"));
        defName(content.substr(0, 24));
        L["text"] = {{"content", makeProp(content)},
                     {"font", opts.value("font", std::string("DejaVuSans-Bold"))},
                     {"size", makeProp(opts.value("size", 120.0))},
                     {"fill", makeProp(opts.value("color", json({1.0, 1.0, 1.0, 1.0})))},
                     {"strokeColor", makeProp({0.0, 0.0, 0.0, 1.0})},
                     {"strokeWidth", makeProp(0.0)},
                     {"tracking", makeProp(0.0)},
                     {"leading", makeProp(1.2)},
                     {"baselineShift", makeProp(0.0)},
                     {"align", "center"},
                     {"boxWidth", 0},
                     {"fauxItalic", false},
                     {"shadow", {{"enabled", false}, {"color", makeProp({0.0, 0.0, 0.0, 0.7})}, {"distance", makeProp(8.0)}, {"angle", makeProp(135.0)}, {"blur", makeProp(8.0)}}},
                     {"background", {{"enabled", false}, {"color", makeProp({0.0, 0.0, 0.0, 0.6})}, {"padding", makeProp(20.0)}, {"radius", makeProp(16.0)}}},
                     {"animators", json::array()},
                     {"extrude", {{"enabled", false}, {"depth", makeProp(30.0)}, {"sideColor", makeProp({0.5, 0.5, 0.5, 1.0})}}}};
    } else if (kind == "shape") {
        defName("Shape");
        std::string prim = opts.value("shape", std::string("rect"));
        json item = {{"id", "S1"}, {"type", prim}, {"op", "add"}, {"position", makeProp({0.0, 0.0})}};
        if (prim == "rect") { item["size"] = makeProp({400.0, 300.0}); item["roundness"] = makeProp(0.0); }
        else if (prim == "ellipse") item["size"] = makeProp({300.0, 300.0});
        else if (prim == "star") { item["points"] = makeProp(5.0); item["outerRadius"] = makeProp(180.0); item["innerRadius"] = makeProp(80.0); item["rotation"] = makeProp(0.0); }
        else if (prim == "polygon") { item["points"] = makeProp(6.0); item["outerRadius"] = makeProp(180.0); item["rotation"] = makeProp(0.0); }
        else if (prim == "line" || prim == "arrow") { item["from"] = makeProp({-200.0, 0.0}); item["to"] = makeProp({200.0, 0.0}); item["headSize"] = makeProp(40.0); }
        else if (prim == "path") item["path"] = makeProp(opts.value("path", json{{"closed", true}, {"v", json::array({{-100, -100, 0, 0, 0, 0}, {100, -100, 0, 0, 0, 0}, {0, 100, 0, 0, 0, 0}})}}));
        bool lineLike = prim == "line" || prim == "arrow" || (prim == "path" && !jobj(opts, "path").value("closed", true));
        L["shape"] = {{"items", json::array({item})},
                      {"fill", {{"enabled", !lineLike}, {"type", "solid"}, {"color", makeProp(opts.value("color", json({1.0, 0.35, 0.2, 1.0})))}, {"opacity", makeProp(100.0)},
                                {"gradient", {{"start", makeProp({-200.0, 0.0})}, {"end", makeProp({200.0, 0.0})}, {"stops", json::array({{0, 1, 0.3, 0.2, 1}, {1, 0.2, 0.3, 1, 1}})}}}}},
                      {"stroke", {{"enabled", lineLike}, {"color", makeProp({1.0, 1.0, 1.0, 1.0})}, {"width", makeProp(lineLike ? 12.0 : 6.0)}, {"opacity", makeProp(100.0)},
                                  {"cap", "round"}, {"join", "round"}, {"dash", json::array()}, {"dashOffset", makeProp(0.0)}}},
                      {"trim", {{"enabled", false}, {"start", makeProp(0.0)}, {"end", makeProp(100.0)}, {"offset", makeProp(0.0)}}},
                      {"repeater", {{"enabled", false}, {"copies", makeProp(3.0)}, {"offset", makeProp(0.0)}, {"position", makeProp({250.0, 0.0})},
                                    {"scale", makeProp({100.0, 100.0})}, {"rotation", makeProp(0.0)}, {"startOpacity", makeProp(100.0)}, {"endOpacity", makeProp(100.0)}}},
                      {"zigzag", {{"enabled", false}, {"size", makeProp(10.0)}, {"ridges", makeProp(6.0)}, {"smooth", false}}},
                      {"roundCorners", {{"enabled", false}, {"radius", makeProp(20.0)}}},
                      {"twist", {{"enabled", false}, {"angle", makeProp(0.0)}}},
                      {"offsetPath", {{"enabled", false}, {"amount", makeProp(0.0)}}},
                      {"extrude", {{"enabled", false}, {"depth", makeProp(40.0)}, {"sideColor", makeProp({0.5, 0.5, 0.5, 1.0})}}}};
    } else if (kind == "null") {
        defName("Null");
    } else if (kind == "camera") {
        defName("Camera");
        double zoom = cw * 1.2;  // ~ 50mm look for 16:9
        L["threeD"] = true;
        L["transform"] = defaultTransform(0, 0, cw / 2.0, ch / 2.0);
        L["transform"]["position"] = makeProp({cw / 2.0, ch / 2.0, -zoom});
        L["camera"] = {{"zoom", makeProp(zoom)}, {"poi", makeProp({cw / 2.0, ch / 2.0, 0.0})}, {"autoOrient", "poi"},
                       {"dof", false}, {"focusDistance", makeProp(zoom)}, {"aperture", makeProp(25.0)}, {"near", 1.0}, {"far", 20000.0}};
        L["out"] = dur;
        L["in"] = 0.0;
        L["start"] = 0.0;
    } else if (kind == "light") {
        defName("Light");
        L["threeD"] = true;
        L["transform"]["position"] = makeProp({cw * 0.3, ch * 0.2, -600.0});
        L["light"] = {{"kind", opts.value("light", std::string("point"))}, {"color", makeProp({1.0, 1.0, 1.0, 1.0})}, {"intensity", makeProp(100.0)},
                      {"coneAngle", makeProp(90.0)}, {"coneFeather", makeProp(50.0)}, {"falloff", "none"}, {"radius", makeProp(1500.0)},
                      {"castShadows", false}, {"shadowDarkness", makeProp(60.0)}, {"poi", makeProp({cw / 2.0, ch / 2.0, 0.0})}};
        L["out"] = dur;
        L["in"] = 0.0;
        L["start"] = 0.0;
    } else if (kind == "particles") {
        defName("Particles");
        L["particles"] = particlePreset(opts.value("preset", std::string("sparks")));
    } else if (kind == "model3d") {
        defName(opts.value("primitive", std::string("Model")));
        L["threeD"] = true;
        L["model"] = {{"asset", opts.value("asset", std::string())},
                      {"primitive", opts.value("primitive", std::string(opts.contains("asset") ? "" : "cube"))},
                      {"size", makeProp(opts.value("size", 300.0))},
                      {"material", {{"baseColor", makeProp(opts.value("color", json({0.8, 0.8, 0.85, 1.0})))}, {"metallic", makeProp(0.0)}, {"roughness", makeProp(0.45)},
                                    {"emission", makeProp({0.0, 0.0, 0.0, 1.0})}, {"emissionStrength", makeProp(0.0)}, {"opacity", makeProp(100.0)}}},
                      {"castShadows", true},
                      {"receiveShadows", true},
                      {"wireframe", false}};
    } else if (kind == "captions") {
        defName("Captions");
        L["captions"] = {{"items", json::array()}, {"style", project.contains("captionStyles") && !project["captionStyles"].empty() ? project["captionStyles"][0] : defaultCaptionStyle()}};
        L["in"] = 0.0;
        L["start"] = 0.0;
        L["out"] = dur;
    } else if (kind == "image" || kind == "video" || kind == "audio") {
        std::string assetId = opts.value("asset", std::string());
        const json* a = findAsset(project, assetId);
        if (!a) throw EditError("Cannot add media layer: asset '" + assetId + "' is not in this project.");
        defName(a->value("name", std::string("Media")));
        L["asset"] = assetId;
        int w = a->value("width", cw), h = a->value("height", ch);
        double rot = a->value("rotation", 0.0);
        if (kind != "audio") L["transform"] = defaultTransform(w / 2.0, h / 2.0, cw / 2.0, ch / 2.0);
        if (kind != "audio" && rot != 0) L["transform"]["rotation"] = makeProp(rot);
        if (kind == "image") {
            L["out"] = at + opts.value("duration", std::min(5.0, std::max(0.1, dur - at)));
        } else {
            double adur = a->value("duration", 5.0);
            L["out"] = at + opts.value("duration", adur);
        }
        // Fit large media into the composition by default (non-destructive scale).
        if (kind != "audio" && w > 0 && h > 0 && opts.value("fit", true)) {
            double s = std::min((double)cw / w, (double)ch / h) * 100.0;
            if (std::fabs(s - 100.0) > 0.01) L["transform"]["scale"] = makeProp({s, s, 100.0});
        }
        if (kind == "audio" || (kind == "video" && a->value("hasAudio", false))) L["audio"] = audioDefaults;
        if (kind == "audio") L["transform"] = defaultTransform(0, 0, cw / 2.0, ch / 2.0);
    } else if (kind == "precomp") {
        std::string compId = opts.value("comp", std::string());
        const json* c = findComp(project, compId);
        if (!c) throw EditError("Cannot add precomp layer: composition '" + compId + "' does not exist.");
        defName(c->value("name", std::string("Precomp")));
        L["precomp"] = {{"comp", compId}, {"controls", json::object()}};
        int w = c->value("width", cw), h = c->value("height", ch);
        L["transform"] = defaultTransform(w / 2.0, h / 2.0, cw / 2.0, ch / 2.0);
        L["out"] = at + opts.value("duration", c->value("duration", 5.0));
    } else {
        throw EditError("Unknown layer type '" + kind + "'.");
    }
    if (opts.contains("overrides") && opts["overrides"].is_object()) L.merge_patch(opts["overrides"]);
    return L;
}

// ====================================================================== lookup
const json* findComp(const json& doc, const std::string& id) {
    auto it = doc.find("comps");
    if (it == doc.end()) return nullptr;
    for (auto& c : *it)
        if (c.value("id", "") == id) return &c;
    return nullptr;
}
json* findCompMut(json& doc, const std::string& id) {
    if (!doc.contains("comps")) return nullptr;
    for (auto& c : doc["comps"])
        if (c.value("id", "") == id) return &c;
    return nullptr;
}
const json* findLayer(const json& comp, const std::string& id) {
    auto it = comp.find("layers");
    if (it == comp.end()) return nullptr;
    for (auto& l : *it)
        if (l.value("id", "") == id) return &l;
    return nullptr;
}
json* findLayerMut(json& comp, const std::string& id) {
    if (!comp.contains("layers")) return nullptr;
    for (auto& l : comp["layers"])
        if (l.value("id", "") == id) return &l;
    return nullptr;
}
int layerIndex(const json& comp, const std::string& id) {
    int i = 0;
    for (auto& l : jarr(comp, "layers")) {
        if (l.value("id", "") == id) return i;
        ++i;
    }
    return -1;
}
const json* findAsset(const json& doc, const std::string& id) {
    auto it = doc.find("assets");
    if (it == doc.end()) return nullptr;
    for (auto& a : *it)
        if (a.value("id", "") == id) return &a;
    return nullptr;
}
json* findAssetMut(json& doc, const std::string& id) {
    if (!doc.contains("assets")) return nullptr;
    for (auto& a : doc["assets"])
        if (a.value("id", "") == id) return &a;
    return nullptr;
}
const json* activeComp(const json& doc) {
    std::string id = jobj(doc, "settings").value("activeComp", std::string());
    const json* c = findComp(doc, id);
    if (!c && doc.contains("comps") && !doc["comps"].empty()) return &doc["comps"][0];
    return c;
}
std::string newId(json& doc, const char* prefix) {
    int n = doc.value("nextId", 1);
    doc["nextId"] = n + 1;
    return std::string(prefix) + std::to_string(n);
}

static std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : path) {
        if (c == '.' || c == '/') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static bool isIndex(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

const json* resolvePath(const json& root, const std::string& path) {
    const json* cur = &root;
    for (auto& seg : splitPath(path)) {
        if (cur->is_object()) {
            auto it = cur->find(seg);
            if (it == cur->end()) return nullptr;
            cur = &*it;
        } else if (cur->is_array()) {
            const json* found = nullptr;
            if (isIndex(seg) && std::stoul(seg) < cur->size()) found = &(*cur)[std::stoul(seg)];
            if (!found)
                for (auto& e : *cur)
                    if (e.is_object() && e.value("id", "") == seg) { found = &e; break; }
            if (!found) return nullptr;
            cur = found;
        } else return nullptr;
    }
    return cur;
}

json* resolvePathMut(json& root, const std::string& path, bool create) {
    json* cur = &root;
    for (auto& seg : splitPath(path)) {
        if (cur->is_object() || (cur->is_null() && create)) {
            if (!cur->contains(seg)) {
                if (!create) return nullptr;
                (*cur)[seg] = json::object();
            }
            cur = &(*cur)[seg];
        } else if (cur->is_array()) {
            json* found = nullptr;
            if (isIndex(seg) && std::stoul(seg) < cur->size()) found = &(*cur)[std::stoul(seg)];
            if (!found)
                for (auto& e : *cur)
                    if (e.is_object() && e.value("id", "") == seg) { found = &e; break; }
            if (!found) return nullptr;
            cur = found;
        } else return nullptr;
    }
    return cur;
}

// ====================================================================== time
double compFps(const json& comp) { return std::max(1.0, comp.value("fps", 30.0)); }
double snapToFrame(double t, double fps) { return std::round(t * fps) / fps; }

double layerLocalTime(const json& layer, double compTime) { return compTime - layer.value("start", 0.0); }

double layerSourceTime(const json& layer, double compTime) {
    double start = layer.value("start", 0.0);
    double speed = layer.value("speed", 1.0);
    if (layer.contains("freezeAt") && layer["freezeAt"].is_number()) return layer["freezeAt"].get<double>();
    if (layer.contains("timeRemap") && layer["timeRemap"].is_object() && layer["timeRemap"].value("enabled", false)) {
        return std::max(0.0, evalRaw(layer["timeRemap"]["prop"], compTime - start).num(0));
    }
    double fwd = (compTime - start) * speed;
    if (layer.value("reverse", false)) {
        double sIn = (layer.value("in", 0.0) - start) * speed;
        double sOut = (layer.value("out", 0.0) - start) * speed;
        // Mirror within the visible source range; offset by one frame so first/last frames align.
        return std::max(0.0, sIn + sOut - fwd - 1e-6);
    }
    return std::max(0.0, fwd);
}

bool layerActiveAt(const json& layer, double compTime) {
    return compTime >= layer.value("in", 0.0) - 1e-9 && compTime < layer.value("out", 0.0) - 1e-9;
}

static void collectPropKeys(const json& j, double start, std::vector<double>& out) {
    if (j.is_object()) {
        if (j.contains("k") && j["k"].is_array())
            for (auto& k : j["k"]) out.push_back(start + k.value("t", 0.0));
        for (auto& [key, v] : j.items())
            if (key != "k" && (v.is_object() || v.is_array())) collectPropKeys(v, start, out);
    } else if (j.is_array()) {
        for (auto& v : j) collectPropKeys(v, start, out);
    }
}

std::vector<double> keyframeTimes(const json& comp, const json* layer) {
    std::vector<double> out;
    auto doLayer = [&](const json& l) { collectPropKeys(l, l.value("start", 0.0), out); };
    if (layer) doLayer(*layer);
    else
        for (auto& l : jarr(comp, "layers")) doLayer(l);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end(), [](double a, double b) { return std::fabs(a - b) < 1e-6; }), out.end());
    return out;
}

std::vector<double> editPoints(const json& comp) {
    std::vector<double> out = {0.0, comp.value("duration", 0.0)};
    for (auto& l : jarr(comp, "layers")) {
        out.push_back(l.value("in", 0.0));
        out.push_back(l.value("out", 0.0));
    }
    for (auto& m : jarr(comp, "markers")) out.push_back(m.value("t", 0.0));
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end(), [](double a, double b) { return std::fabs(a - b) < 1e-6; }), out.end());
    return out;
}

// ====================================================================== ops
namespace {

json& requireComp(json& doc, const json& op) {
    std::string id = op.value("comp", std::string());
    if (id.empty()) {
        const json* ac = activeComp(doc);
        if (!ac) throw EditError("The project has no composition.");
        id = ac->value("id", "");
    }
    json* c = findCompMut(doc, id);
    if (!c) throw EditError("Composition '" + id + "' was not found.");
    return *c;
}

json& requireLayer(json& comp, const std::string& id) {
    json* l = findLayerMut(comp, id);
    if (!l) throw EditError("Layer '" + id + "' was not found in composition '" + comp.value("name", "") + "'.");
    return *l;
}

void requireUnlocked(const json& layer) {
    if (layer.value("locked", false))
        throw EditError("Layer '" + layer.value("name", "") + "' is locked. Unlock it to edit.");
}

std::vector<std::string> idList(const json& op, const char* key) {
    std::vector<std::string> out;
    if (op.contains(key)) {
        if (op[key].is_array())
            for (auto& v : op[key]) out.push_back(v.get<std::string>());
        else if (op[key].is_string()) out.push_back(op[key].get<std::string>());
    }
    return out;
}

// Re-assign ids in a layer subtree (effects, masks, behaviors, animators, shape items).
void reidLayerChildren(json& doc, json& layer) {
    for (const char* arr : {"effects", "masks", "behaviors"}) {
        if (layer.contains(arr))
            for (auto& e : layer[arr]) e["id"] = newId(doc, arr[0] == 'e' ? "E" : arr[0] == 'm' ? "M" : "B");
    }
}

json& propRef(json& layer, const std::string& path) {
    json* p = resolvePathMut(layer, path, false);
    if (!p) throw EditError("Property '" + path + "' does not exist on layer '" + layer.value("name", "") + "'.");
    return *p;
}

void checkValueCompatible(const json& prop, const json& value, const std::string& path) {
    json cur = prop.is_object() ? (prop.contains("v") ? prop["v"] : (hasKeyframes(prop) ? prop["k"][0]["v"] : json())) : prop;
    if (cur.is_null()) return;
    auto kind = [](const json& j) { return j.is_number() ? 0 : j.is_array() ? 1 : j.is_string() ? 2 : j.is_boolean() ? 3 : 4; };
    if (kind(cur) != kind(value) && !(cur.is_number() && value.is_boolean()) && !(cur.is_boolean() && value.is_number()))
        throw EditError("Value type does not match property '" + path + "'.");
}

void shiftLayerTime(json& L, double dt) {
    L["start"] = L.value("start", 0.0) + dt;
    L["in"] = L.value("in", 0.0) + dt;
    L["out"] = L.value("out", 0.0) + dt;
}

void scaleAllKeyframes(json& j, double factor) {
    if (j.is_object()) {
        if (j.contains("k") && j["k"].is_array())
            for (auto& k : j["k"]) k["t"] = k.value("t", 0.0) * factor;
        for (auto& [key, v] : j.items())
            if (key != "k" && (v.is_object() || v.is_array())) scaleAllKeyframes(v, factor);
    } else if (j.is_array()) {
        for (auto& v : j) scaleAllKeyframes(v, factor);
    }
}

json maskPath(const std::string& shape, const json& op, const json& layer, const json& comp) {
    // Default mask shapes in layer space.
    double w = 400, h = 300, cx = 0, cy = 0;
    std::string type = layer.value("type", "");
    if (layer.contains("solid")) { w = layer["solid"].value("width", 400); h = layer["solid"].value("height", 300); cx = w / 2; cy = h / 2; }
    if (op.contains("rect") && op["rect"].is_array() && op["rect"].size() == 4) {
        double x = op["rect"][0], y = op["rect"][1], rw = op["rect"][2], rh = op["rect"][3];
        cx = x + rw / 2; cy = y + rh / 2; w = rw; h = rh;
    } else if (type == "video" || type == "image" || type == "precomp") {
        auto a = layer["transform"]["anchor"]["v"];
        if (a.is_array()) { cx = a[0]; cy = a[1]; w = cx * 1.2; h = cy * 1.2; }
    }
    (void)comp;
    json v = json::array();
    if (shape == "ellipse") {
        const double k = 0.5522847498;
        double rx = w / 2, ry = h / 2;
        v.push_back({cx, cy - ry, -rx * k, 0, rx * k, 0});
        v.push_back({cx + rx, cy, 0, -ry * k, 0, ry * k});
        v.push_back({cx, cy + ry, rx * k, 0, -rx * k, 0});
        v.push_back({cx - rx, cy, 0, ry * k, 0, -ry * k});
    } else {
        v.push_back({cx - w / 2, cy - h / 2, 0, 0, 0, 0});
        v.push_back({cx + w / 2, cy - h / 2, 0, 0, 0, 0});
        v.push_back({cx + w / 2, cy + h / 2, 0, 0, 0, 0});
        v.push_back({cx - w / 2, cy + h / 2, 0, 0, 0, 0});
    }
    return json{{"closed", true}, {"v", v}};
}

// Remove [t0,t1) from comp timeline, rippling later content left.
void rippleRemoveRange(json& doc, json& comp, double t0, double t1, const std::set<std::string>& skipLayers) {
    double gap = t1 - t0;
    if (gap <= 0) return;
    json newLayers = json::array();
    for (auto& L : comp["layers"]) {
        std::string id = L.value("id", "");
        if (skipLayers.count(id) || L.value("type", "") == "camera" || L.value("type", "") == "light" || L.value("type", "") == "captions") {
            newLayers.push_back(L);
            continue;
        }
        double in = L.value("in", 0.0), out = L.value("out", 0.0);
        if (out <= t0 + 1e-9) { newLayers.push_back(L); continue; }
        if (in >= t1 - 1e-9) { json c = L; shiftLayerTime(c, -gap); newLayers.push_back(c); continue; }
        // Overlaps the range.
        if (in < t0 && out > t1) {
            json a = L; a["out"] = t0;
            json b = L; b["id"] = newId(doc, "L"); reidLayerChildren(doc, b);
            b["in"] = t1; shiftLayerTime(b, -gap);
            newLayers.push_back(a);
            newLayers.push_back(b);
        } else if (in < t0) {
            json a = L; a["out"] = t0; newLayers.push_back(a);
        } else if (out > t1) {
            json b = L; b["in"] = t1; shiftLayerTime(b, -gap); newLayers.push_back(b);
        }
        // fully inside -> removed
    }
    comp["layers"] = newLayers;
    // Captions: drop/shift items.
    for (auto& L : comp["layers"]) {
        if (L.value("type", "") != "captions") continue;
        json items = json::array();
        for (auto& c : L["captions"]["items"]) {
            double s = c.value("start", 0.0), e = c.value("end", 0.0);
            if (e <= t0 + 1e-9) items.push_back(c);
            else if (s >= t1 - 1e-9) {
                json n = c; n["start"] = s - gap; n["end"] = e - gap;
                if (n.contains("words")) for (auto& w : n["words"]) { w["s"] = w.value("s", 0.0) - gap; w["e"] = w.value("e", 0.0) - gap; }
                items.push_back(n);
            }
        }
        L["captions"]["items"] = items;
    }
    for (auto& m : comp["markers"])
        if (m.value("t", 0.0) >= t1) m["t"] = m.value("t", 0.0) - gap;
    comp["duration"] = std::max(0.1, comp.value("duration", 0.0) - gap);
    if (comp.contains("workArea")) comp["workArea"] = {0.0, comp["duration"]};
}

json& captionLayer(json& comp, const json& op) {
    std::string id = op.value("layer", std::string());
    if (!id.empty()) return requireLayer(comp, id);
    for (auto& L : comp["layers"])
        if (L.value("type", "") == "captions") return L;
    throw EditError("This composition has no caption track. Add one first (Captions > Add Caption Track).");
}

json& findCaption(json& L, const std::string& id) {
    for (auto& c : L["captions"]["items"])
        if (c.value("id", "") == id) return c;
    throw EditError("Caption '" + id + "' not found.");
}

void sortCaptions(json& L) {
    auto& items = L["captions"]["items"];
    std::vector<json> v(items.begin(), items.end());
    std::stable_sort(v.begin(), v.end(), [](const json& a, const json& b) { return a.value("start", 0.0) < b.value("start", 0.0); });
    items = json(v);
}

std::string joinWords(const json& words) {
    std::string s;
    for (auto& w : words) {
        std::string t = w.value("w", "");
        if (!s.empty() && !t.empty() && t[0] != ',' && t[0] != '.' && t[0] != '?' && t[0] != '!') s += " ";
        s += t;
    }
    return s;
}

}  // namespace

std::string opLabel(const json& op) {
    if (op.contains("label") && op["label"].is_string()) return op["label"];
    std::string name = op.value("op", std::string("edit"));
    static const std::map<std::string, std::string> labels = {
        {"addLayer", "Add Layer"}, {"removeLayers", "Delete Layer"}, {"duplicateLayers", "Duplicate Layer"},
        {"reorderLayer", "Reorder Layer"}, {"setLayer", "Change Layer"}, {"setLayerTiming", "Change Timing"},
        {"moveLayerTime", "Move Layer"}, {"trimLayer", "Trim Clip"}, {"split", "Split"}, {"rippleDelete", "Ripple Delete"},
        {"deleteGap", "Delete Gap"}, {"freezeFrame", "Freeze Frame"}, {"setSpeed", "Change Speed"}, {"reverse", "Reverse"},
        {"setTimeRemap", "Time Remap"}, {"speedRampPreset", "Speed Ramp"}, {"precompose", "Precompose"},
        {"setProp", "Change Property"}, {"addKeyframe", "Add Keyframe"}, {"removeKeyframe", "Delete Keyframe"},
        {"moveKeyframe", "Move Keyframe"}, {"setKeyframeInterp", "Change Interpolation"}, {"setKeyframeValue", "Change Keyframe"},
        {"clearKeyframes", "Clear Keyframes"}, {"reverseKeyframes", "Reverse Keyframes"}, {"scaleKeyframes", "Scale Keyframe Timing"},
        {"distributeKeyframes", "Distribute Keyframes"}, {"nudgeKeyframes", "Nudge Keyframes"}, {"pasteKeyframes", "Paste Keyframes"},
        {"setExpression", "Edit Expression"}, {"addEffect", "Add Effect"}, {"removeEffect", "Remove Effect"},
        {"moveEffect", "Reorder Effect"}, {"setEffect", "Change Effect"}, {"duplicateEffect", "Duplicate Effect"},
        {"resetEffect", "Reset Effect"}, {"pasteEffects", "Paste Effects"}, {"addMask", "Add Mask"}, {"removeMask", "Delete Mask"},
        {"setMask", "Change Mask"}, {"addBehavior", "Add Behavior"}, {"removeBehavior", "Remove Behavior"},
        {"setBehavior", "Change Behavior"}, {"bakeBehavior", "Bake Behavior"}, {"setTransition", "Set Transition"},
        {"addMarker", "Add Marker"}, {"removeMarker", "Delete Marker"}, {"updateMarker", "Change Marker"},
        {"setCaptions", "Generate Captions"}, {"updateCaption", "Edit Caption"}, {"splitCaption", "Split Caption"},
        {"mergeCaptions", "Merge Captions"}, {"removeCaption", "Delete Caption"}, {"setCaptionStyle", "Caption Style"},
        {"captionReplace", "Replace in Captions"}, {"editByCaption", "Edit by Transcript"}, {"addComp", "New Composition"},
        {"updateComp", "Composition Settings"}, {"removeComp", "Delete Composition"}, {"setActiveComp", "Switch Composition"},
        {"addAsset", "Import"}, {"updateAsset", "Update Media"}, {"relinkAsset", "Relink Media"}, {"removeAsset", "Remove Media"},
        {"setProjectName", "Rename Project"}, {"parent", "Set Parent"}, {"setCompAudio", "Audio Mix"}, {"applyPreset", "Apply Preset"},
        {"insertCapsule", "Insert Capsule"}, {"setCapsuleControl", "Capsule Control"}, {"addGuide", "Add Guide"},
        {"removeGuide", "Remove Guide"}, {"setText", "Edit Text"}, {"batch", "Edit"}, {"addAnimator", "Add Text Animator"},
        {"removeAnimator", "Remove Text Animator"}, {"textPreset", "Text Animation Preset"}, {"addShapeItem", "Add Shape"},
        {"removeShapeItem", "Remove Shape"}, {"replaceSource", "Replace Source"}, {"setTrackKeyframes", "Apply Tracking"},
        {"addDuckingKeys", "Auto Ducking"}, {"setMarkers", "Set Markers"}, {"setCaptionItems", "Edit Captions"}};
    auto it = labels.find(name);
    return it == labels.end() ? name : it->second;
}

std::vector<std::string> opNames() {
    return {"setProjectName", "addComp", "updateComp", "removeComp", "setActiveComp", "addAsset", "updateAsset", "relinkAsset",
            "removeAsset", "addLayer", "removeLayers", "duplicateLayers", "reorderLayer", "setLayer", "setLayerTiming",
            "moveLayerTime", "trimLayer", "split", "rippleDelete", "deleteGap", "freezeFrame", "setSpeed", "reverse",
            "setTimeRemap", "speedRampPreset", "precompose", "setProp", "addKeyframe", "removeKeyframe", "moveKeyframe",
            "setKeyframeInterp", "setKeyframeValue", "clearKeyframes", "reverseKeyframes", "scaleKeyframes",
            "distributeKeyframes", "nudgeKeyframes", "pasteKeyframes", "setExpression", "addEffect", "removeEffect",
            "moveEffect", "setEffect", "duplicateEffect", "resetEffect", "pasteEffects", "addMask", "removeMask", "setMask",
            "addBehavior", "removeBehavior", "setBehavior", "bakeBehavior", "setTransition", "addMarker", "removeMarker",
            "updateMarker", "setCaptions", "updateCaption", "splitCaption", "mergeCaptions", "removeCaption",
            "setCaptionStyle", "captionReplace", "editByCaption", "parent", "setCompAudio", "applyPreset", "insertCapsule",
            "setCapsuleControl", "addGuide", "removeGuide", "setText", "addAnimator", "removeAnimator", "textPreset",
            "addShapeItem", "removeShapeItem", "replaceSource", "setTrackKeyframes", "addDuckingKeys", "batch"};
}

static json textPresetAnimator(const std::string& preset, double dur) {
    auto P = [](const json& v) { return makeProp(v); };
    auto keys = [](std::initializer_list<std::pair<double, json>> ks, const char* interp = "easeOut") {
        json p = json::object();
        json arr = json::array();
        for (auto& k : ks) arr.push_back({{"t", k.first}, {"v", k.second}, {"o", interp}});
        p["k"] = arr;
        return p;
    };
    double d = std::max(0.2, dur);
    json sel = {{"start", P(0.0)}, {"end", P(100.0)}, {"offset", P(0.0)}, {"unit", "char"}, {"shape", "square"}, {"smoothness", 100.0}};
    // Ramp presets sweep a 25%-wide window across the text: units behind the window are revealed.
    auto sweep = [&](const char* interp) {
        sel["end"] = P(25.0);
        sel["offset"] = keys({{0.0, -25.0}, {d, 100.0}}, interp);
        sel["shape"] = "rampUp";
    };
    json props = json::object();
    if (preset == "typeOn") {
        sel["start"] = keys({{0.0, 0.0}, {d, 100.0}}, "linear");
        props["opacity"] = P(0.0);
    } else if (preset == "fadeUp") {
        sweep("easeOut");
        props["opacity"] = P(0.0);
        props["position"] = P({0.0, 60.0});
    } else if (preset == "pop") {
        sweep("backOut");
        props["scale"] = P(0.0);
        props["opacity"] = P(0.0);
    } else if (preset == "bounce") {
        sweep("bounce");
        props["position"] = P({0.0, -200.0});
        props["opacity"] = P(0.0);
    } else if (preset == "wave") {
        sel["shape"] = "triangle";
        sel["start"] = P(0.0);
        sel["end"] = P(30.0);
        sel["offset"] = keys({{0.0, -30.0}, {d, 100.0}}, "linear");
        props["position"] = P({0.0, -40.0});
    } else if (preset == "scramble") {
        sel["start"] = keys({{0.0, 0.0}, {d, 100.0}}, "linear");
        props["scramble"] = true;
    } else if (preset == "kineticWords") {
        sweep("expoOut");
        sel["unit"] = "word";
        props["scale"] = P(250.0);
        props["opacity"] = P(0.0);
    } else if (preset == "letterRotate") {
        sweep("backOut");
        props["rotation"] = P(-90.0);
        props["opacity"] = P(0.0);
    } else if (preset == "maskedReveal") {
        sweep("cubicOut");
        sel["unit"] = "line";
        props["position"] = P({0.0, 140.0});
        props["clip"] = true;
    } else if (preset == "blurIn") {
        sweep("easeOut");
        props["blur"] = P(30.0);
        props["opacity"] = P(0.0);
    } else {
        throw EditError("Unknown text animation preset '" + preset + "'.");
    }
    return json{{"name", preset}, {"selector", sel}, {"props", props}};
}

static void applyOne(json& doc, const json& op, OpResult& res);

json applyOp(const json& docIn, const json& op, OpResult* result) {
    OpResult local;
    OpResult& res = result ? *result : local;
    res.label = opLabel(op);
    json doc = docIn;
    applyOne(doc, op, res);
    if (doc.contains("meta")) doc["meta"]["modified"] = nowSeconds();
    return doc;
}

static void applyOne(json& doc, const json& op, OpResult& res) {
    const std::string name = op.value("op", std::string());
    auto layerOf = [&](json& comp) -> json& {
        json& L = requireLayer(comp, op.value("layer", std::string()));
        requireUnlocked(L);
        return L;
    };
    auto compTimeToLocal = [&](const json& L) { return layerLocalTime(L, op.value("t", 0.0)); };

    if (name == "batch") {
        for (auto& sub : jarr(op, "ops")) {
            OpResult r;
            applyOne(doc, sub, r);
            for (auto& [k, v] : r.data.items()) res.data[k] = v;
        }
        return;
    }
    if (name == "setProjectName") {
        std::string n = op.value("name", std::string());
        if (n.empty()) throw EditError("Project name cannot be empty.");
        doc["meta"]["name"] = n;
        return;
    }
    if (name == "addComp") {
        std::string id = newId(doc, "C");
        int w = op.value("width", 1920), h = op.value("height", 1080);
        if (w < 16 || h < 16 || w > 16384 || h > 16384) throw EditError("Composition size must be between 16 and 16384 pixels.");
        doc["comps"].push_back(newComp(id, op.value("name", std::string("Comp ") + id), w, h, op.value("fps", 30.0), op.value("duration", 10.0)));
        res.data["comp"] = id;
        return;
    }
    if (name == "updateComp") {
        json& c = requireComp(doc, op);
        for (const char* k : {"name", "width", "height", "fps", "duration", "bg", "workArea", "motionBlur"}) {
            if (op.contains(k)) c[k] = op[k];
        }
        if (c.value("duration", 1.0) <= 0) throw EditError("Duration must be positive.");
        if (op.contains("duration") && !op.contains("workArea")) c["workArea"] = {0.0, c["duration"]};
        return;
    }
    if (name == "removeComp") {
        std::string id = op.value("comp", std::string());
        if (doc["comps"].size() <= 1) throw EditError("A project needs at least one composition.");
        for (auto& c : doc["comps"])
            for (auto& L : c["layers"])
                if (L.contains("precomp") && L["precomp"].value("comp", "") == id)
                    throw EditError("Composition is used by layer '" + L.value("name", "") + "'. Remove that layer first.");
        json comps = json::array();
        for (auto& c : doc["comps"])
            if (c.value("id", "") != id) comps.push_back(c);
        doc["comps"] = comps;
        if (doc["settings"].value("activeComp", "") == id) doc["settings"]["activeComp"] = doc["comps"][0]["id"];
        return;
    }
    if (name == "setActiveComp") {
        std::string id = op.value("comp", std::string());
        if (!findComp(doc, id)) throw EditError("Composition '" + id + "' was not found.");
        doc["settings"]["activeComp"] = id;
        return;
    }
    if (name == "addAsset") {
        json a = jobj(op, "asset");
        if (!a.contains("type")) throw EditError("Asset type missing.");
        a["id"] = newId(doc, "A");
        doc["assets"].push_back(a);
        res.data["asset"] = a["id"];
        return;
    }
    if (name == "updateAsset" || name == "relinkAsset") {
        json* a = findAssetMut(doc, op.value("asset", std::string()));
        if (!a) throw EditError("Asset not found.");
        json fields = jobj(op, "fields");
        if (name == "relinkAsset") {
            // Never silently swap media: relink requires explicit path and records the previous checksum.
            if (!op.contains("path") && !op.contains("uri")) throw EditError("Relink needs a new file.");
            (*a)["previousChecksum"] = a->value("checksum", "");
            if (op.contains("path")) (*a)["path"] = op["path"];
            if (op.contains("uri")) (*a)["uri"] = op["uri"];
            if (op.contains("checksum")) (*a)["checksum"] = op["checksum"];
            a->erase("missing");
        }
        for (auto& [k, v] : fields.items())
            if (k != "id") (*a)[k] = v;
        return;
    }
    if (name == "removeAsset") {
        std::string id = op.value("asset", std::string());
        for (auto& c : doc["comps"])
            for (auto& L : c["layers"])
                if (L.value("asset", "") == id) throw EditError("Media is used by layer '" + L.value("name", "") + "'.");
        json out = json::array();
        for (auto& a : doc["assets"])
            if (a.value("id", "") != id) out.push_back(a);
        doc["assets"] = out;
        return;
    }
    if (name == "addLayer") {
        json& comp = requireComp(doc, op);
        std::string kind = op.value("kind", std::string("solid"));
        std::string id = newId(doc, "L");
        json opts = jobj(op, "options");
        if (op.contains("at")) opts["at"] = op["at"];
        json L = makeLayer(kind, id, comp, doc, opts);
        if (L.contains("shape")) {
            for (auto& it : L["shape"]["items"]) it["id"] = newId(doc, "S");
        }
        int idx = op.value("index", 0);
        auto& layers = comp["layers"];
        idx = clampv(idx, 0, (int)layers.size());
        layers.insert(layers.begin() + idx, L);
        // Extend comp if needed (media longer than comp).
        if (op.value("extendComp", false) && L.value("out", 0.0) > comp.value("duration", 0.0)) {
            comp["duration"] = L["out"];
            comp["workArea"] = {0.0, comp["duration"]};
        }
        res.data["layer"] = id;
        return;
    }
    if (name == "removeLayers") {
        json& comp = requireComp(doc, op);
        auto ids = idList(op, "layers");
        std::set<std::string> rm(ids.begin(), ids.end());
        json out = json::array();
        for (auto& L : comp["layers"]) {
            if (rm.count(L.value("id", ""))) {
                requireUnlocked(L);
                continue;
            }
            out.push_back(L);
        }
        for (auto& L : out) {
            if (L["parent"].is_string() && rm.count(L["parent"].get<std::string>())) L["parent"] = nullptr;
            if (L["matte"].is_object() && rm.count(L["matte"].value("layer", ""))) L["matte"] = nullptr;
        }
        comp["layers"] = out;
        return;
    }
    if (name == "duplicateLayers") {
        json& comp = requireComp(doc, op);
        json created = json::array();
        for (auto& id : idList(op, "layers")) {
            int idx = layerIndex(comp, id);
            if (idx < 0) throw EditError("Layer '" + id + "' was not found.");
            json c = comp["layers"][idx];
            c["id"] = newId(doc, "L");
            c["name"] = c.value("name", "") + " copy";
            reidLayerChildren(doc, c);
            comp["layers"].insert(comp["layers"].begin() + idx, c);
            created.push_back(c["id"]);
        }
        res.data["layers"] = created;
        return;
    }
    if (name == "reorderLayer") {
        json& comp = requireComp(doc, op);
        int idx = layerIndex(comp, op.value("layer", std::string()));
        if (idx < 0) throw EditError("Layer not found.");
        json L = comp["layers"][idx];
        comp["layers"].erase(comp["layers"].begin() + idx);
        int to = clampv(op.value("index", 0), 0, (int)comp["layers"].size());
        comp["layers"].insert(comp["layers"].begin() + to, L);
        return;
    }
    if (name == "setLayer") {
        json& comp = requireComp(doc, op);
        json& L = requireLayer(comp, op.value("layer", std::string()));
        json fields = jobj(op, "fields");
        bool onlyLockToggle = fields.size() == 1 && fields.contains("locked");
        if (!onlyLockToggle) requireUnlocked(L);
        static const std::set<std::string> allowed = {"name", "enabled", "solo", "locked", "muted", "blend", "threeD", "guide",
                                                      "label", "matte", "parent", "motionBlur", "quality", "notes"};
        for (auto& [k, v] : fields.items()) {
            if (!allowed.count(k)) throw EditError("Field '" + k + "' cannot be set with setLayer.");
            if (k == "parent" && v.is_string()) {
                std::string p = v.get<std::string>();
                if (p == L.value("id", "")) throw EditError("A layer cannot be its own parent.");
                // Cycle check.
                std::string cur = p;
                for (int guard = 0; guard < 256 && !cur.empty(); ++guard) {
                    const json* pl = findLayer(comp, cur);
                    if (!pl) throw EditError("Parent layer not found.");
                    if (pl->value("id", "") == L.value("id", "")) throw EditError("Parenting would create a cycle.");
                    cur = (*pl)["parent"].is_string() ? (*pl)["parent"].get<std::string>() : std::string();
                }
            }
            if (k == "matte" && v.is_object()) {
                if (v.value("layer", "") == L.value("id", "")) throw EditError("A layer cannot be its own matte.");
                if (!findLayer(comp, v.value("layer", ""))) throw EditError("Matte layer not found.");
            }
            L[k] = v;
        }
        return;
    }
    if (name == "setLayerTiming") {
        json& comp = requireComp(doc, op);
        json& L = layerOf(comp);
        for (const char* k : {"in", "out", "start"})
            if (op.contains(k)) L[k] = op[k].get<double>();
        if (L.value("out", 0.0) - L.value("in", 0.0) < 1.0 / compFps(comp) - 1e-9) throw EditError("A layer must be at least one frame long.");
        return;
    }
    if (name == "moveLayerTime") {
        json& comp = requireComp(doc, op);
        double dt = op.value("dt", 0.0);
        for (auto& id : idList(op, "layers")) {
            json& L = requireLayer(comp, id);
            requireUnlocked(L);
            shiftLayerTime(L, dt);
        }
        return;
    }
    if (name == "trimLayer") {
        json& comp = requireComp(doc, op);
        json& L = layerOf(comp);
        double t = op.value("t", 0.0);
        double minLen = 1.0 / compFps(comp);
        std::string edge = op.value("edge", std::string("in"));
        std::string type = L.value("type", "");
        bool media = type == "video" || type == "audio";
        double srcDur = 1e9;
        if (media) {
            const json* a = findAsset(doc, L.value("asset", ""));
            if (a) srcDur = a->value("duration", 1e9);
        }
        double speed = std::max(1e-6, L.value("speed", 1.0));
        bool remap = L.contains("timeRemap") && L["timeRemap"].is_object() && L["timeRemap"].value("enabled", false);
        if (edge == "in") {
            double minIn = media && !remap && !L.value("reverse", false) ? L.value("start", 0.0) : -1e9;
            t = clampv(t, minIn, L.value("out", 0.0) - minLen);
            L["in"] = t;
        } else {
            double maxOut = media && !remap ? L.value("start", 0.0) + srcDur / speed : 1e9;
            t = clampv(t, L.value("in", 0.0) + minLen, maxOut);
            L["out"] = t;
        }
        return;
    }
    if (name == "split") {
        json& comp = requireComp(doc, op);
        double t = op.value("t", 0.0);
        auto ids = idList(op, "layers");
        if (ids.empty())
            for (auto& L : comp["layers"])
                if (layerActiveAt(L, t) && !L.value("locked", false) && L.value("type", "") != "camera" && L.value("type", "") != "light" &&
                    L.value("type", "") != "captions")
                    ids.push_back(L.value("id", ""));
        json created = json::array();
        for (auto& id : ids) {
            int idx = layerIndex(comp, id);
            if (idx < 0) throw EditError("Layer not found.");
            json& L = comp["layers"][idx];
            requireUnlocked(L);
            double in = L.value("in", 0.0), out = L.value("out", 0.0);
            if (t <= in + 1e-9 || t >= out - 1e-9) throw EditError("The playhead must be inside the clip to split it.");
            json B = L;
            B["id"] = newId(doc, "L");
            reidLayerChildren(doc, B);
            B["in"] = t;
            L["out"] = t;
            if (L.contains("transitionOut")) L["transitionOut"] = nullptr;
            if (B.contains("transitionIn")) B["transitionIn"] = nullptr;
            comp["layers"].insert(comp["layers"].begin() + idx, B);
            created.push_back(B["id"]);
        }
        if (created.empty()) throw EditError("No unlocked clip under the playhead to split.");
        res.data["layers"] = created;
        return;
    }
    if (name == "rippleDelete") {
        json& comp = requireComp(doc, op);
        json& L = layerOf(comp);
        double t0 = L.value("in", 0.0), t1 = L.value("out", 0.0);
        std::string id = L.value("id", "");
        json out = json::array();
        for (auto& l : comp["layers"])
            if (l.value("id", "") != id) out.push_back(l);
        comp["layers"] = out;
        // Shift later layers only (no splitting for ripple delete of a single clip).
        double gap = t1 - t0;
        for (auto& l : comp["layers"])
            if (l.value("in", 0.0) >= t1 - 1e-9 && l.value("type", "") != "captions" && l.value("type", "") != "camera" && l.value("type", "") != "light")
                shiftLayerTime(l, -gap);
        return;
    }
    if (name == "deleteGap") {
        json& comp = requireComp(doc, op);
        double t = op.value("t", 0.0);
        double gapStart = 0, gapEnd = 1e18;
        for (auto& l : comp["layers"]) {
            std::string ty = l.value("type", "");
            if (ty == "captions" || ty == "camera" || ty == "light") continue;
            double in = l.value("in", 0.0), out = l.value("out", 0.0);
            if (in <= t && out > t) throw EditError("There is no gap at the playhead.");
            if (out <= t) gapStart = std::max(gapStart, out);
            if (in > t) gapEnd = std::min(gapEnd, in);
        }
        if (gapEnd > 1e17) throw EditError("There is no gap at the playhead (nothing after it).");
        double gap = gapEnd - gapStart;
        for (auto& l : comp["layers"])
            if (l.value("in", 0.0) >= gapEnd - 1e-9) shiftLayerTime(l, -gap);
        return;
    }
    if (name == "freezeFrame") {
        json& comp = requireComp(doc, op);
        double t = op.value("t", 0.0), dur = op.value("duration", 2.0);
        json& L = layerOf(comp);
        if (!layerActiveAt(L, t)) throw EditError("The playhead must be over the clip.");
        double src = layerSourceTime(L, t);
        int idx = layerIndex(comp, L.value("id", ""));
        json tail = comp["layers"][idx];
        json frozen = comp["layers"][idx];
        comp["layers"][idx]["out"] = t;
        frozen["id"] = newId(doc, "L");
        reidLayerChildren(doc, frozen);
        frozen["name"] = frozen.value("name", "") + " (freeze)";
        frozen["in"] = t;
        frozen["out"] = t + dur;
        frozen["freezeAt"] = src;
        if (frozen.contains("audio")) frozen["muted"] = true;
        tail["id"] = newId(doc, "L");
        reidLayerChildren(doc, tail);
        tail["in"] = t;
        shiftLayerTime(tail, dur);
        // Later clips ripple right.
        for (auto& l : comp["layers"])
            if (l.value("in", 0.0) >= t + 1e-9 && l.value("id", "") != L.value("id", "")) shiftLayerTime(l, dur);
        comp["layers"].insert(comp["layers"].begin() + idx, frozen);
        comp["layers"].insert(comp["layers"].begin() + idx, tail);
        comp["duration"] = comp.value("duration", 0.0) + dur;
        comp["workArea"] = {0.0, comp["duration"]};
        res.data["layer"] = frozen["id"];
        return;
    }
    if (name == "setSpeed") {
        json& comp = requireComp(doc, op);
        json& L = layerOf(comp);
        double s = op.value("speed", 1.0);
        if (s < 0.01 || s > 100) throw EditError("Speed must be between 1% and 10000%.");
        double old = L.value("speed", 1.0);
        double start = L.value("start", 0.0), in = L.value("in", 0.0), out = L.value("out", 0.0);
        double srcIn = (in - start) * old, srcOut = (out - start) * old;
        double nstart = in - srcIn / s;
        L["start"] = nstart;
        L["out"] = nstart + srcOut / s;
        L["speed"] = s;
        for (const char* key : {"transform", "effects", "masks", "text", "shape"})
            if (L.contains(key)) scaleAllKeyframes(L[key], old / s);
        return;
    }
    if (name == "reverse") {
        json& comp = requireComp(doc, op);
        json& L = layerOf(comp);
        L["reverse"] = op.value("on", !L.value("reverse", false));
        return;
    }
    if (name == "setTimeRemap" || name == "speedRampPreset") {
        json& comp = requireComp(doc, op);
        json& L = layerOf(comp);
        double start = L.value("start", 0.0), in = L.value("in", 0.0), out = L.value("out", 0.0);
        double speed = L.value("speed", 1.0);
        double l0 = in - start, l1 = out - start;
        double s0 = l0 * speed, s1 = l1 * speed;
        if (name == "setTimeRemap") {
            bool en = op.value("enabled", true);
            if (!en) { L["timeRemap"] = nullptr; return; }
            json prop = {{"k", json::array({{{"t", l0}, {"v", s0}, {"o", "linear"}}, {{"t", l1}, {"v", s1}, {"o", "linear"}}})}};
            L["timeRemap"] = {{"enabled", true}, {"prop", prop}};
            return;
        }
        // Speed ramp presets keep total source range identical.
        std::string preset = op.value("preset", std::string("cinematic"));
        auto K = [](double t, double v, const char* o) { return json{{"t", t}, {"v", v}, {"o", o}}; };
        json ks = json::array();
        double L_ = l1 - l0, S = s1 - s0;
        if (preset == "cinematic") {
            ks = {K(l0, s0, "linear"), K(l0 + L_ * 0.3, s0 + S * 0.45, "easeInOut"), K(l0 + L_ * 0.7, s0 + S * 0.55, "linear"), K(l1, s1, "linear")};
        } else if (preset == "impact") {
            ks = {K(l0, s0, "linear"), K(l0 + L_ * 0.45, s0 + S * 0.6, "easeOut"), K(l0 + L_ * 0.8, s0 + S * 0.68, "easeIn"), K(l1, s1, "linear")};
        } else if (preset == "montage") {
            ks = {K(l0, s0, "linear"), K(l0 + L_ * 0.25, s0 + S * 0.5, "easeInOut"), K(l0 + L_ * 0.75, s0 + S * 0.6, "easeInOut"), K(l1, s1, "linear")};
        } else if (preset == "acceleration") {
            ks = {K(l0, s0, "expoIn"), K(l1, s1, "linear")};
        } else if (preset == "deceleration") {
            ks = {K(l0, s0, "expoOut"), K(l1, s1, "linear")};
        } else if (preset == "snap") {
            ks = {K(l0, s0, "linear"), K(l0 + L_ * 0.4, s0 + S * 0.15, "expoIn"), K(l0 + L_ * 0.55, s0 + S * 0.85, "linear"), K(l1, s1, "linear")};
        } else {
            throw EditError("Unknown speed ramp preset '" + preset + "'.");
        }
        L["timeRemap"] = {{"enabled", true}, {"prop", {{"k", ks}}}, {"preset", preset}};
        return;
    }
    if (name == "precompose") {
        json& comp = requireComp(doc, op);
        auto ids = idList(op, "layers");
        if (ids.empty()) throw EditError("Select at least one layer to precompose.");
        std::string cid = newId(doc, "C");
        json nc = newComp(cid, op.value("name", std::string("Precomp ") + cid), comp.value("width", 1920), comp.value("height", 1080),
                          comp.value("fps", 30.0), comp.value("duration", 10.0));
        nc["bg"] = {0.0, 0.0, 0.0, 0.0};
        std::set<std::string> sel(ids.begin(), ids.end());
        json remaining = json::array();
        int firstIdx = -1, i = 0;
        double minIn = 1e18, maxOut = 0;
        for (auto& L : comp["layers"]) {
            if (sel.count(L.value("id", ""))) {
                requireUnlocked(L);
                if (firstIdx < 0) firstIdx = (int)remaining.size();
                nc["layers"].push_back(L);
                minIn = std::min(minIn, L.value("in", 0.0));
                maxOut = std::max(maxOut, L.value("out", 0.0));
            } else remaining.push_back(L);
            ++i;
        }
        // Dangling parents inside the precomp are cleared.
        for (auto& L : nc["layers"])
            if (L["parent"].is_string() && !sel.count(L["parent"].get<std::string>())) L["parent"] = nullptr;
        std::string lid = newId(doc, "L");
        std::string parentCompId = comp.value("id", "");
        doc["comps"].push_back(nc);
        json& comp2 = *findCompMut(doc, parentCompId);  // reacquire after push_back (reference may have moved)
        json pl = makeLayer("precomp", lid, comp2, doc, {{"comp", cid}, {"name", nc["name"]}});
        pl["in"] = minIn;
        pl["out"] = maxOut;
        pl["start"] = 0.0;
        json rem = remaining;
        rem.insert(rem.begin() + std::max(0, firstIdx), pl);
        comp2["layers"] = rem;
        res.data["comp"] = cid;
        res.data["layer"] = lid;
        return;
    }
    if (name == "setProp" || name == "addKeyframe" || name == "removeKeyframe" || name == "moveKeyframe" || name == "setKeyframeInterp" ||
        name == "setKeyframeValue" || name == "clearKeyframes" || name == "reverseKeyframes" || name == "scaleKeyframes" ||
        name == "distributeKeyframes" || name == "nudgeKeyframes" || name == "pasteKeyframes" || name == "setExpression") {
        json& comp = requireComp(doc, op);
        json& L = layerOf(comp);
        std::string path = op.value("path", std::string());
        json& P = propRef(L, path);
        double lt = compTimeToLocal(L);
        if (name == "setProp") {
            json v = op["value"];
            if (P.is_object() && (P.contains("v") || P.contains("k"))) {
                checkValueCompatible(P, v, path);
                std::string mode = op.value("mode", std::string("auto"));
                if (mode == "static") { P.erase("k"); P["v"] = v; }
                else if (mode == "key") P = propSetKeyframe(P, lt, v, op.contains("interp") ? op["interp"] : json());
                else P = propSetValue(P, lt, v);
            } else {
                P = v;  // plain (non-animatable) field
            }
        } else if (name == "addKeyframe") {
            if (!P.is_object()) throw EditError("Property '" + path + "' is not animatable.");
            json v = op.contains("value") ? op["value"] : evalRaw(P, lt).toJson();
            P = propSetKeyframe(P, lt, v, op.contains("interp") ? op["interp"] : json());
        } else if (name == "removeKeyframe") {
            P = propRemoveKeyframe(P, lt, op.value("eps", 0.5 / compFps(comp)));
        } else if (name == "moveKeyframe") {
            double from = layerLocalTime(L, op.value("from", 0.0)), to = layerLocalTime(L, op.value("to", 0.0));
            if (!hasKeyframes(P)) throw EditError("Property has no keyframes.");
            bool moved = false;
            for (auto& k : P["k"])
                if (std::fabs(k.value("t", 0.0) - from) < 0.5 / compFps(comp)) { k["t"] = to; moved = true; break; }
            if (!moved) throw EditError("No keyframe at that time.");
            P = propSetKeyframe(P, to, evalRaw(P, to).toJson());  // re-sort
        } else if (name == "setKeyframeInterp") {
            if (!hasKeyframes(P)) throw EditError("Property has no keyframes.");
            bool all = op.value("all", false);
            bool found = false;
            for (auto& k : P["k"])
                if (all || std::fabs(k.value("t", 0.0) - lt) < 0.5 / compFps(comp)) { k["o"] = op["interp"]; found = true; }
            if (!found) throw EditError("No keyframe at the playhead.");
        } else if (name == "setKeyframeValue") {
            if (!hasKeyframes(P)) throw EditError("Property has no keyframes.");
            bool found = false;
            for (auto& k : P["k"])
                if (std::fabs(k.value("t", 0.0) - lt) < 0.5 / compFps(comp)) { k["v"] = op["value"]; found = true; }
            if (!found) throw EditError("No keyframe at the playhead.");
        } else if (name == "clearKeyframes") {
            Value cur = evalRaw(P, lt);
            P.erase("k");
            P["v"] = cur.toJson();
        } else if (name == "reverseKeyframes") {
            P = propReverseKeyframes(P);
        } else if (name == "scaleKeyframes") {
            P = propScaleKeyframeTimes(P, layerLocalTime(L, op.value("pivot", L.value("start", 0.0))), op.value("factor", 1.0));
        } else if (name == "distributeKeyframes") {
            P = propDistributeKeyframes(P);
        } else if (name == "nudgeKeyframes") {
            P = propShiftKeyframes(P, op.value("dt", 1.0 / compFps(comp)));
        } else if (name == "pasteKeyframes") {
            json clip = jobj(op, "clip");
            double d0 = layerLocalTime(L, op.value("d0", op.value("t", 0.0)));
            double d1 = op.contains("d1") ? layerLocalTime(L, op["d1"].get<double>()) : d0 + (clip.value("t1", 0.0) - clip.value("t0", 0.0));
            if (!jarr(clip, "k").empty()) {
                Value sample = Value::fromJson(clip["k"][0]["v"]);
                Value cur = evalRaw(P, lt);
                if (sample.kind != cur.kind || (sample.kind == Value::Kind::Vector && sample.n.size() != cur.n.size() &&
                                                !(sample.n.size() >= 2 && cur.n.size() >= 2)))
                    throw EditError("Copied keyframes are not compatible with '" + path + "'.");
            }
            P = pasteKeyframes(P, clip, d0, d1);
        } else if (name == "setExpression") {
            std::string src = op.value("expr", std::string());
            if (src.empty()) { P.erase("x"); P.erase("xe"); }
            else { P["x"] = src; P["xe"] = op.value("enabled", true); }
        }
        return;
    }
    if (name == "setText") {
        json& comp = requireComp(doc, op);
        json& L = layerOf(comp);
        if (!L.contains("text")) throw EditError("Layer is not a text layer.");
        L["text"]["content"] = propSetValue(L["text"]["content"], compTimeToLocal(L), op.value("content", std::string()));
        if (op.value("rename", true)) L["name"] = op.value("content", std::string()).substr(0, 24);
        return;
    }
    if (name == "addAnimator" || name == "textPreset") {
        json& comp = requireComp(doc, op);
        json& L = layerOf(comp);
        if (!L.contains("text")) throw EditError("Text animators require a text layer.");
        json a = name == "textPreset" ? textPresetAnimator(op.value("preset", std::string("fadeUp")), op.value("duration", 1.0))
                                      : jobj(op, "animator");
        if (name == "textPreset" && op.contains("t")) {
            // Offset preset keyframes to start at the playhead.
            double lt = compTimeToLocal(L);
            for (const char* sk : {"start", "end", "offset"})
                if (a["selector"].contains(sk)) a["selector"][sk] = propShiftKeyframes(a["selector"][sk], lt);
        }
        a["id"] = newId(doc, "T");
        if (op.value("replace", false)) L["text"]["animators"] = json::array();
        L["text"]["animators"].push_back(a);
        res.data["animator"] = a["id"];
        return;
    }
    if (name == "removeAnimator") {
        json& comp = requireComp(doc, op);
        json& L = layerOf(comp);
        json out = json::array();
        for (auto& a : L["text"]["animators"])
            if (a.value("id", "") != op.value("animator", std::string())) out.push_back(a);
        L["text"]["animators"] = out;
        return;
    }
    if (name == "addShapeItem") {
        json& comp = requireComp(doc, op);
        json& L = layerOf(comp);
        if (!L.contains("shape")) throw EditError("Layer is not a shape layer.");
        json item = jobj(op, "item");
        if (!item.contains("type")) throw EditError("Shape item needs a type.");
        item["id"] = newId(doc, "S");
        if (!item.contains("op")) item["op"] = "add";
        L["shape"]["items"].push_back(item);
        res.data["item"] = item["id"];
        return;
    }
    if (name == "removeShapeItem") {
        json& comp = requireComp(doc, op);
        json& L = layerOf(comp);
        json out = json::array();
        for (auto& it : L["shape"]["items"])
            if (it.value("id", "") != op.value("item", std::string())) out.push_back(it);
        L["shape"]["items"] = out;
        return;
    }
    if (name == "addEffect") {
        json& comp = requireComp(doc, op);
        json& L = layerOf(comp);
        std::string type = op.value("type", std::string());
        if (!findEffect(type) && !findCompositeEffect(type)) throw EditError("Unknown effect '" + type + "'.");
        json e = {{"id", newId(doc, "E")}, {"type", type}, {"enabled", true}, {"params", effectDefaultParams(type)}};
        if (op.contains("params"))
            for (auto& [k, v] : op["params"].items()) e["params"][k] = v.is_object() ? v : makeProp(v);
        int idx = op.value("index", (int)L["effects"].size());
        idx = clampv(idx, 0, (int)L["effects"].size());
        L["effects"].insert(L["effects"].begin() + idx, e);
        res.data["effect"] = e["id"];
        return;
    }
    if (name == "removeEffect" || name == "moveEffect" || name == "setEffect" || name == "duplicateEffect" || name == "resetEffect") {
        json& comp = requireComp(doc, op);
        json& L = layerOf(comp);
        std::string eid = op.value("effect", std::string());
        int idx = -1;
        for (size_t i = 0; i < L["effects"].size(); ++i)
            if (L["effects"][i].value("id", "") == eid) idx = (int)i;
        if (idx < 0) throw EditError("Effect not found on layer.");
        auto& effects = L["effects"];
        if (name == "removeEffect") effects.erase(effects.begin() + idx);
        else if (name == "moveEffect") {
            json e = effects[idx];
            effects.erase(effects.begin() + idx);
            int to = clampv(op.value("index", 0), 0, (int)effects.size());
            effects.insert(effects.begin() + to, e);
        } else if (name == "setEffect") {
            json fields = jobj(op, "fields");
            for (auto& [k, v] : fields.items())
                if (k == "enabled" || k == "solo" || k == "name" || k == "mix") effects[idx][k] = v;
        } else if (name == "duplicateEffect") {
            json e = effects[idx];
            e["id"] = newId(doc, "E");
            effects.insert(effects.begin() + idx + 1, e);
            res.data["effect"] = e["id"];
        } else if (name == "resetEffect") {
            effects[idx]["params"] = effectDefaultParams(effects[idx].value("type", ""));
        }
        return;
    }
    if (name == "pasteEffects" || name == "applyPreset") {
        json& comp = requireComp(doc, op);
        json& L = layerOf(comp);
        json preset = name == "applyPreset" ? jobj(op, "preset") : json{{"effects", jarr(op, "effects")}};
        std::string mapping = op.value("mapping", std::string("skip"));
        if (mapping == "cancel") throw EditError("Preset application cancelled.");
        json skipped = json::array();
        for (auto e : jarr(preset, "effects")) {
            std::string type = e.value("type", "");
            if (!findEffect(type) && !findCompositeEffect(type)) {
                if (mapping == "skip" || mapping == "approximate") { skipped.push_back(type); continue; }
                throw EditError("Preset uses unknown effect '" + type + "'.");
            }
            e["id"] = newId(doc, "E");
            // Fill missing params with defaults (approximate mapping).
            json defs = effectDefaultParams(type);
            for (auto& [k, v] : defs.items())
                if (!e["params"].contains(k)) e["params"][k] = v;
            L["effects"].push_back(e);
        }
        if (preset.contains("transform") && preset["transform"].is_object()) {
            for (auto& [k, v] : preset["transform"].items())
                if (L["transform"].contains(k)) L["transform"][k] = v;
        }
        if (preset.contains("behaviors"))
            for (auto b : preset["behaviors"]) { b["id"] = newId(doc, "B"); L["behaviors"].push_back(b); }
        if (preset.contains("masks"))
            for (auto m : preset["masks"]) { m["id"] = newId(doc, "M"); L["masks"].push_back(m); }
        if (preset.contains("blend")) L["blend"] = preset["blend"];
        res.data["skipped"] = skipped;
        return;
    }
    if (name == "addMask") {
        json& comp = requireComp(doc, op);
        json& L = layerOf(comp);
        std::string shape = op.value("shape", std::string("rect"));
        json path = op.contains("path") ? op["path"] : maskPath(shape, op, L, comp);
        json m = {{"id", newId(doc, "M")}, {"name", "Mask " + std::to_string(L["masks"].size() + 1)}, {"mode", op.value("mode", std::string("add"))},
                  {"inverted", false}, {"path", makeProp(path)}, {"feather", makeProp(0.0)}, {"expansion", makeProp(0.0)}, {"opacity", makeProp(100.0)}};
        L["masks"].push_back(m);
        res.data["mask"] = m["id"];
        return;
    }
    if (name == "removeMask" || name == "setMask") {
        json& comp = requireComp(doc, op);
        json& L = layerOf(comp);
        std::string mid = op.value("mask", std::string());
        json out = json::array();
        bool found = false;
        for (auto& m : L["masks"]) {
            if (m.value("id", "") == mid) {
                found = true;
                if (name == "removeMask") continue;
                for (json tmp_ = jobj(op, "fields"); auto& [k, v] : tmp_.items())
                    if (k == "mode" || k == "inverted" || k == "name" || k == "locked") m[k] = v;
            }
            out.push_back(m);
        }
        if (!found) throw EditError("Mask not found.");
        L["masks"] = out;
        return;
    }
    if (name == "addBehavior") {
        json& comp = requireComp(doc, op);
        json& L = layerOf(comp);
        std::string type = op.value("type", std::string());
        json params = behaviorDefaultParams(type);
        if (params.empty()) throw EditError("Unknown behavior '" + type + "'.");
        if (op.contains("params"))
            for (auto& [k, v] : op["params"].items()) params[k] = v.is_object() ? v : makeProp(v);
        json b = {{"id", newId(doc, "B")}, {"type", type}, {"enabled", true}, {"params", params}, {"audioLink", op.value("audioLink", json())}};
        L["behaviors"].push_back(b);
        res.data["behavior"] = b["id"];
        return;
    }
    if (name == "removeBehavior" || name == "setBehavior" || name == "bakeBehavior") {
        json& comp = requireComp(doc, op);
        json& L = layerOf(comp);
        std::string bid = op.value("behavior", std::string());
        int idx = -1;
        for (size_t i = 0; i < L["behaviors"].size(); ++i)
            if (L["behaviors"][i].value("id", "") == bid) idx = (int)i;
        if (idx < 0) throw EditError("Behavior not found.");
        if (name == "removeBehavior") {
            L["behaviors"].erase(L["behaviors"].begin() + idx);
        } else if (name == "setBehavior") {
            for (json tmp_ = jobj(op, "fields"); auto& [k, v] : tmp_.items())
                if (k == "enabled" || k == "audioLink") L["behaviors"][idx][k] = v;
        } else {
            bakeBehaviorToKeyframes(comp, L, idx, compFps(comp));
        }
        return;
    }
    if (name == "setTransition") {
        json& comp = requireComp(doc, op);
        json& L = layerOf(comp);
        std::string edge = op.value("edge", std::string("in"));
        json tr = op.value("transition", json());
        if (tr.is_object()) {
            bool known = false;
            for (auto& t : transitionRegistry())
                if (t.type == tr.value("type", "")) known = true;
            if (!known) throw EditError("Unknown transition type.");
            double maxDur = L.value("out", 0.0) - L.value("in", 0.0);
            tr["duration"] = clampv(tr.value("duration", 0.5), 1.0 / compFps(comp), std::max(1.0 / compFps(comp), maxDur));
        }
        L[edge == "in" ? "transitionIn" : "transitionOut"] = tr;
        return;
    }
    if (name == "addMarker" || name == "removeMarker" || name == "updateMarker") {
        json& comp = requireComp(doc, op);
        json* target = &comp["markers"];
        if (op.contains("layer") && op["layer"].is_string() && !op["layer"].get<std::string>().empty()) target = &requireLayer(comp, op["layer"])["markers"];
        if (name == "addMarker") {
            json m = {{"id", newId(doc, "K")}, {"t", op.value("t", 0.0)}, {"title", op.value("title", std::string())}, {"note", op.value("note", std::string())},
                      {"color", op.value("color", json({1.0, 0.8, 0.2, 1.0}))}, {"duration", op.value("duration", 0.0)}, {"kind", op.value("kind", std::string("project"))}};
            target->push_back(m);
            res.data["marker"] = m["id"];
        } else {
            json out = json::array();
            bool found = false;
            for (auto& m : *target) {
                if (m.value("id", "") == op.value("marker", std::string())) {
                    found = true;
                    if (name == "removeMarker") continue;
                    for (json tmp_ = jobj(op, "fields"); auto& [k, v] : tmp_.items())
                        if (k != "id") m[k] = v;
                }
                out.push_back(m);
            }
            if (!found) throw EditError("Marker not found.");
            *target = out;
        }
        return;
    }
    if (name == "setMarkers") {
        json& comp = requireComp(doc, op);
        json ms = json::array();
        for (auto m : jarr(op, "markers")) { m["id"] = newId(doc, "K"); ms.push_back(m); }
        if (op.value("append", true)) for (auto& m : ms) comp["markers"].push_back(m);
        else comp["markers"] = ms;
        return;
    }
    if (name == "setCaptions" || name == "setCaptionItems") {
        json& comp = requireComp(doc, op);
        json* Lp = nullptr;
        std::string lid = op.value("layer", std::string());
        if (!lid.empty()) Lp = &requireLayer(comp, lid);
        else
            for (auto& L : comp["layers"])
                if (L.value("type", "") == "captions") { Lp = &L; break; }
        if (!Lp) {
            std::string id = newId(doc, "L");
            json L = makeLayer("captions", id, comp, doc, json::object());
            comp["layers"].insert(comp["layers"].begin(), L);
            Lp = &comp["layers"][0];
            res.data["layer"] = id;
        }
        json items = json::array();
        for (auto c : jarr(op, "items")) {
            if (!c.contains("id") || name == "setCaptions") c["id"] = newId(doc, "Q");
            items.push_back(c);
        }
        if (op.value("append", false)) for (auto& c : items) (*Lp)["captions"]["items"].push_back(c);
        else (*Lp)["captions"]["items"] = items;
        sortCaptions(*Lp);
        if (op.contains("language")) (*Lp)["captions"]["language"] = op["language"];
        if (op.contains("source")) (*Lp)["captions"]["source"] = op["source"];
        return;
    }
    if (name == "updateCaption" || name == "removeCaption") {
        json& comp = requireComp(doc, op);
        json& L = captionLayer(comp, op);
        requireUnlocked(L);
        if (name == "removeCaption") {
            json out = json::array();
            for (auto& c : L["captions"]["items"])
                if (c.value("id", "") != op.value("caption", std::string())) out.push_back(c);
            L["captions"]["items"] = out;
            return;
        }
        json& c = findCaption(L, op.value("caption", std::string()));
        for (json tmp_ = jobj(op, "fields"); auto& [k, v] : tmp_.items()) {
            if (k == "id") continue;
            c[k] = v;
            if (k == "text") c.erase("words");  // word timing no longer valid after manual text edit
        }
        if (c.value("end", 0.0) <= c.value("start", 0.0)) throw EditError("Caption end must be after its start.");
        sortCaptions(L);
        return;
    }
    if (name == "splitCaption") {
        json& comp = requireComp(doc, op);
        json& L = captionLayer(comp, op);
        json& c = findCaption(L, op.value("caption", std::string()));
        double t = op.value("t", (c.value("start", 0.0) + c.value("end", 0.0)) / 2);
        if (t <= c.value("start", 0.0) || t >= c.value("end", 0.0)) throw EditError("Split time must be inside the caption.");
        json a = c, b = c;
        b["id"] = newId(doc, "Q");
        a["end"] = t;
        b["start"] = t;
        if (c.contains("words") && !c["words"].empty()) {
            json wa = json::array(), wb = json::array();
            for (auto& w : c["words"]) (w.value("s", 0.0) < t ? wa : wb).push_back(w);
            a["words"] = wa; b["words"] = wb;
            a["text"] = joinWords(wa); b["text"] = joinWords(wb);
        } else {
            std::string text = c.value("text", "");
            size_t mid = text.size() / 2;
            size_t sp = text.find(' ', mid);
            if (sp == std::string::npos) sp = text.rfind(' ', mid);
            if (sp == std::string::npos) sp = mid;
            a["text"] = text.substr(0, sp);
            b["text"] = sp < text.size() ? text.substr(std::min(text.size(), sp + 1)) : "";
        }
        json out = json::array();
        for (auto& x : L["captions"]["items"]) {
            if (x.value("id", "") == a.value("id", "")) { out.push_back(a); out.push_back(b); }
            else out.push_back(x);
        }
        L["captions"]["items"] = out;
        res.data["caption"] = b["id"];
        return;
    }
    if (name == "mergeCaptions") {
        json& comp = requireComp(doc, op);
        json& L = captionLayer(comp, op);
        std::string a = op.value("first", std::string()), b = op.value("second", std::string());
        json& ca = findCaption(L, a);
        json cb = findCaption(L, b);
        ca["end"] = std::max(ca.value("end", 0.0), cb.value("end", 0.0));
        ca["start"] = std::min(ca.value("start", 0.0), cb.value("start", 0.0));
        ca["text"] = ca.value("text", "") + " " + cb.value("text", "");
        if (ca.contains("words") && cb.contains("words")) for (auto& w : cb["words"]) ca["words"].push_back(w);
        json out = json::array();
        for (auto& x : L["captions"]["items"])
            if (x.value("id", "") != b) out.push_back(x);
        L["captions"]["items"] = out;
        return;
    }
    if (name == "setCaptionStyle") {
        json& comp = requireComp(doc, op);
        json& L = captionLayer(comp, op);
        json style = L["captions"].value("style", defaultCaptionStyle());
        style.merge_patch(jobj(op, "style"));
        L["captions"]["style"] = style;
        if (op.value("saveAsPreset", false)) {
            style["name"] = op.value("presetName", std::string("Custom"));
            doc["captionStyles"].push_back(style);
        }
        return;
    }
    if (name == "captionReplace") {
        json& comp = requireComp(doc, op);
        json& L = captionLayer(comp, op);
        std::string find = op.value("find", std::string()), repl = op.value("replace", std::string());
        if (find.empty()) throw EditError("Search text is empty.");
        int count = 0;
        for (auto& c : L["captions"]["items"]) {
            std::string t = c.value("text", "");
            size_t pos = 0;
            bool changed = false;
            while ((pos = t.find(find, pos)) != std::string::npos) {
                t.replace(pos, find.size(), repl);
                pos += repl.size();
                ++count;
                changed = true;
            }
            if (changed) {
                c["text"] = t;
                if (c.contains("words"))
                    for (auto& w : c["words"]) {
                        std::string wt = w.value("w", "");
                        size_t p2 = 0;
                        while ((p2 = wt.find(find, p2)) != std::string::npos) { wt.replace(p2, find.size(), repl); p2 += repl.size(); }
                        w["w"] = wt;
                    }
            }
        }
        res.data["count"] = count;
        return;
    }
    if (name == "editByCaption") {
        json& comp = requireComp(doc, op);
        json& L = captionLayer(comp, op);
        auto ids = idList(op, "captions");
        std::vector<std::pair<double, double>> ranges;
        for (auto& id : ids) {
            json& c = findCaption(L, id);
            ranges.push_back({c.value("start", 0.0), c.value("end", 0.0)});
        }
        std::sort(ranges.begin(), ranges.end());
        // Apply from the latest range backwards so earlier times stay valid.
        for (auto it = ranges.rbegin(); it != ranges.rend(); ++it) rippleRemoveRange(doc, comp, it->first, it->second, {});
        return;
    }
    if (name == "parent") {
        json& comp = requireComp(doc, op);
        json& L = layerOf(comp);
        json p = op.value("parent", json());
        json sub = {{"op", "setLayer"}, {"comp", comp.value("id", "")}, {"layer", L.value("id", "")}, {"fields", {{"parent", p}}}};
        // keepTransform: convert position into parent space at the current time (2D, static values).
        if (op.value("keepTransform", true) && p.is_string()) {
            const json* PL = findLayer(comp, p.get<std::string>());
            if (PL && !hasKeyframes(L["transform"]["position"])) {
                EvalContext ctx;
                ctx.layerTime = layerLocalTime(*PL, op.value("t", 0.0));
                Vec3 ppos = propVec3((*PL)["transform"], "position", ctx, {});
                Vec3 panc = propVec3((*PL)["transform"], "anchor", ctx, {});
                Vec3 psc = propVec3((*PL)["transform"], "scale", ctx, {100, 100, 100});
                double prot = deg2rad(propNumber((*PL)["transform"], "rotation", ctx, 0));
                ctx.layerTime = layerLocalTime(L, op.value("t", 0.0));
                Vec3 cpos = propVec3(L["transform"], "position", ctx, {});
                double dx = cpos.x - ppos.x, dy = cpos.y - ppos.y;
                double c = std::cos(-prot), s = std::sin(-prot);
                double lx = (dx * c - dy * s) / std::max(1e-6, psc.x / 100) + panc.x;
                double ly = (dx * s + dy * c) / std::max(1e-6, psc.y / 100) + panc.y;
                L["transform"]["position"]["v"] = {lx, ly, cpos.z};
            }
        }
        applyOne(doc, sub, res);
        return;
    }
    if (name == "setCompAudio") {
        json& comp = requireComp(doc, op);
        comp["audio"].merge_patch(jobj(op, "audio"));
        return;
    }
    if (name == "addDuckingKeys") {
        // Writes volume keyframes on a music layer from supplied dialogue ranges (computed by audio analysis).
        json& comp = requireComp(doc, op);
        json& L = layerOf(comp);
        if (!L.contains("audio")) throw EditError("Layer has no audio.");
        double amount = op.value("amount", -12.0), attack = op.value("attack", 0.15), release = op.value("release", 0.4);
        json prop = L["audio"]["volume"];
        double base = evalRaw(prop, 0).num(0);
        prop = json{{"v", base}};
        double start = L.value("start", 0.0);
        for (auto& r : jarr(op, "ranges")) {
            double s = r[0].get<double>() - start, e = r[1].get<double>() - start;
            prop = propSetKeyframe(prop, std::max(0.0, s - attack), base, "easeInOut");
            prop = propSetKeyframe(prop, s, base + amount, "linear");
            prop = propSetKeyframe(prop, e, base + amount, "easeInOut");
            prop = propSetKeyframe(prop, e + release, base, "linear");
        }
        L["audio"]["volume"] = prop;
        return;
    }
    if (name == "insertCapsule") {
        json& comp0 = requireComp(doc, op);
        std::string targetComp = comp0.value("id", "");
        json cap = jobj(op, "capsule");
        if (!cap.contains("comp")) throw EditError("Capsule has no composition.");
        // Re-id nested comps to avoid collisions.
        std::map<std::string, std::string> compIds;
        json comps = jarr(cap, "comps");
        comps.insert(comps.begin(), cap["comp"]);
        for (auto& c : comps) compIds[c.value("id", "")] = newId(doc, "C");
        std::map<std::string, std::string> layerIds;
        for (auto& c : comps)
            for (auto& L : c["layers"]) layerIds[L.value("id", "")] = newId(doc, "L");
        for (auto& c : comps) {
            c["id"] = compIds[c.value("id", "")];
            for (auto& L : c["layers"]) {
                L["id"] = layerIds[L.value("id", "")];
                if (L["parent"].is_string()) L["parent"] = layerIds[L["parent"].get<std::string>()];
                if (L["matte"].is_object()) L["matte"]["layer"] = layerIds[L["matte"].value("layer", "")];
                if (L.contains("precomp")) L["precomp"]["comp"] = compIds[L["precomp"].value("comp", "")];
                reidLayerChildren(doc, L);
            }
            doc["comps"].push_back(c);
        }
        std::string root = compIds[cap["comp"].value("id", "")];
        json& comp = *findCompMut(doc, targetComp);
        std::string lid = newId(doc, "L");
        json L = makeLayer("precomp", lid, comp, doc, {{"comp", root}, {"at", op.value("t", 0.0)}, {"name", cap.value("name", std::string("Capsule"))}});
        json controls = json::object();
        json defs = json::array();
        for (auto ctl : jarr(cap, "controls")) {
            // Remap bound layer ids.
            std::string bl = ctl.value("layer", "");
            if (layerIds.count(bl)) ctl["layer"] = layerIds[bl];
            controls[ctl.value("name", "")] = ctl.value("default", json());
            defs.push_back(ctl);
        }
        L["precomp"]["controls"] = controls;
        L["precomp"]["controlDefs"] = defs;
        L["precomp"]["capsule"] = {{"name", cap.value("name", std::string())}, {"version", cap.value("version", std::string("1.0"))}, {"locked", true}};
        comp["layers"].insert(comp["layers"].begin(), L);
        res.data["layer"] = lid;
        res.data["comp"] = root;
        return;
    }
    if (name == "setCapsuleControl") {
        json& comp = requireComp(doc, op);
        json& L = layerOf(comp);
        if (!L.contains("precomp")) throw EditError("Layer is not a capsule.");
        std::string ctl = op.value("control", std::string());
        bool known = false;
        for (auto& d : jarr(L["precomp"], "controlDefs"))
            if (d.value("name", "") == ctl) known = true;
        if (!known) throw EditError("Capsule has no control named '" + ctl + "'.");
        L["precomp"]["controls"][ctl] = op["value"];
        return;
    }
    if (name == "addGuide" || name == "removeGuide") {
        json& comp = requireComp(doc, op);
        if (name == "addGuide") {
            json g = {{"id", newId(doc, "G")}, {"axis", op.value("axis", std::string("x"))}, {"pos", op.value("pos", 0.0)}};
            comp["guides"].push_back(g);
            res.data["guide"] = g["id"];
        } else {
            json out = json::array();
            for (auto& g : comp["guides"])
                if (g.value("id", "") != op.value("guide", std::string())) out.push_back(g);
            comp["guides"] = out;
        }
        return;
    }
    if (name == "replaceSource") {
        json& comp = requireComp(doc, op);
        json& L = layerOf(comp);
        std::string aid = op.value("asset", std::string());
        const json* a = findAsset(doc, aid);
        if (!a) throw EditError("Replacement media not found.");
        if (!L.contains("asset")) throw EditError("Layer has no media source.");
        L["asset"] = aid;
        L["name"] = a->value("name", L.value("name", ""));
        return;
    }
    if (name == "setTrackKeyframes") {
        // Apply tracker output: list of [compTime, x, y] (+optional rotation/scale) to a property.
        json& comp = requireComp(doc, op);
        json& L = layerOf(comp);
        std::string path = op.value("path", std::string("transform.position"));
        json& P = propRef(L, path);
        Value cur = evalRaw(P, 0);
        json ks = json::array();
        Vec2 off = {op.value("offsetX", 0.0), op.value("offsetY", 0.0)};
        for (auto& s : jarr(op, "samples")) {
            double t = layerLocalTime(L, s[0].get<double>());
            json v;
            if (cur.n.size() >= 3) v = {s[1].get<double>() + off.x, s[2].get<double>() + off.y, cur.n[2]};
            else if (cur.n.size() == 2) v = {s[1].get<double>() + off.x, s[2].get<double>() + off.y};
            else v = s[1];
            ks.push_back({{"t", t}, {"v", v}, {"o", "linear"}});
        }
        if (ks.empty()) throw EditError("Tracking produced no samples.");
        P.erase("v");
        P["k"] = ks;
        return;
    }
    throw EditError("Unknown operation '" + name + "'.");
}

// ====================================================================== validation
std::vector<std::string> validateProject(const json& doc) {
    std::vector<std::string> out;
    if (!doc.is_object()) { out.push_back("Project is not an object."); return out; }
    if (doc.value("format", "") != "mforge") out.push_back("Missing format marker.");
    if (!doc.contains("comps") || !doc["comps"].is_array() || doc["comps"].empty()) out.push_back("Project has no compositions.");
    std::set<std::string> ids;
    for (auto& c : jarr(doc, "comps")) {
        std::string cid = c.value("id", "");
        if (cid.empty()) out.push_back("Composition without id.");
        if (!ids.insert(cid).second) out.push_back("Duplicate id " + cid);
        for (auto& L : jarr(c, "layers")) {
            std::string lid = L.value("id", "");
            if (lid.empty()) out.push_back("Layer without id in " + cid);
            if (!ids.insert(lid).second) out.push_back("Duplicate layer id " + lid);
            if (L.value("out", 0.0) < L.value("in", 0.0)) out.push_back("Layer " + lid + " has out < in.");
            if (L.contains("asset") && !findAsset(doc, L.value("asset", ""))) out.push_back("Layer " + lid + " references missing asset.");
            if (L.contains("precomp") && !findComp(doc, L["precomp"].value("comp", ""))) out.push_back("Layer " + lid + " references missing comp.");
        }
    }
    // Precomp cycles.
    std::function<bool(const std::string&, std::set<std::string>&)> cyc = [&](const std::string& cid, std::set<std::string>& stack) {
        if (stack.count(cid)) return true;
        stack.insert(cid);
        const json* c = findComp(doc, cid);
        if (c)
            for (auto& L : jarr(*c, "layers"))
                if (L.contains("precomp") && cyc(L["precomp"].value("comp", ""), stack)) return true;
        stack.erase(cid);
        return false;
    };
    for (auto& c : jarr(doc, "comps")) {
        std::set<std::string> st;
        if (cyc(c.value("id", ""), st)) { out.push_back("Composition nesting cycle at " + c.value("id", "")); break; }
    }
    return out;
}

std::vector<Diagnostic> diagnoseProject(const json& doc, const std::vector<std::string>& fonts, std::function<bool(const json&)> assetAvailable) {
    std::vector<Diagnostic> out;
    for (auto& a : jarr(doc, "assets")) {
        if (assetAvailable && !assetAvailable(a))
            out.push_back({"error", "MISSING_ASSET", "Media '" + a.value("name", "") + "' cannot be found at its recorded location.", a.value("id", ""),
                           "Relink the file, relink a folder, or search by checksum. Edits are preserved."});
        if (a.value("vfr", false))
            out.push_back({"warning", "VARIABLE_FRAME_RATE", "'" + a.value("name", "") + "' has a variable frame rate.", a.value("id", ""),
                           "Playback timing follows presentation timestamps; consider a constant-frame-rate proxy for precise edits."});
        if (a.value("hdr", false))
            out.push_back({"info", "HDR_SOURCE", "'" + a.value("name", "") + "' is HDR. It is tone-mapped to the SDR working space.", a.value("id", ""),
                           "Choose an HDR export when the device supports it to preserve highlights."});
    }
    std::set<std::string> fontSet(fonts.begin(), fonts.end());
    for (auto& c : jarr(doc, "comps")) {
        for (auto& L : jarr(c, "layers")) {
            if (L.contains("text")) {
                std::string f = L["text"].value("font", "");
                if (!fontSet.empty() && !fontSet.count(f))
                    out.push_back({"warning", "MISSING_FONT", "Font '" + f + "' used by '" + L.value("name", "") + "' is not installed.", L.value("id", ""),
                                   "Import the font or substitute DejaVu Sans. The original font name is kept in the project."});
            }
            int cost = 0, blurs = 0;
            for (auto& e : jarr(L, "effects")) {
                if (!e.value("enabled", true)) continue;
                const EffectInfo* info = findEffect(e.value("type", ""));
                if (info) cost += info->costWeight;
                if (e.value("type", "").rfind("blur.", 0) == 0) ++blurs;
            }
            if (cost >= 12 || blurs >= 4)
                out.push_back({"warning", "HIGH_GPU_COST", "Layer '" + L.value("name", "") + "' has a heavy effect stack (" + std::to_string(blurs) + " blur passes, cost " + std::to_string(cost) + ").",
                               L.value("id", ""), "Merge compatible blur passes or lower preview quality to Medium."});
        }
    }
    return out;
}

}  // namespace mf
