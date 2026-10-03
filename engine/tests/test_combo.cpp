// Combination and stress tests: features are exercised together, not just one at a time.
//
// Each "feature" is a small builder that edits a project through the real command layer. The suite renders
// every pair of features, then many random feature subsets (seeded, reproducible), and checks invariants:
//   * no exception / crash while building, rendering or mixing audio,
//   * output has the expected size, and rendering the same frame twice is bit-identical,
//   * project JSON survives a serialize/parse round trip with identical render output,
//   * undo all steps returns exactly the initial document, redo returns the final one,
//   * the project still passes validation.
// MF_COMBO_COUNT (env) sets the number of random subsets (default 400; CI nightly can use thousands).
#include <chrono>
#include <cstdlib>
#include <random>

#include "mf/audio.hpp"
#include "mf/document.hpp"
#include "mf/effects.hpp"
#include "mf/model.hpp"
#include "mf/renderer.hpp"
#include "mftest.hpp"

using namespace mf;

namespace {
FileMediaProvider g_cmedia;
int g_rejected = 0, g_applied = 0;

struct Builder {
    Document doc;
    std::string comp = "C1";
    std::vector<std::string> layers;
    std::mt19937 rng;
    int rejected = 0;
    explicit Builder(uint32_t seed) : doc(newProject("Combo", 320, 180, 24, 3)), rng(seed) {}
    OpResult apply(json op) {
        OpResult r;
        if (!op.contains("comp")) op["comp"] = comp;
        try {
            json next = applyOp(doc.doc(), op, &r);
            doc.commit(op.value("op", std::string("op")), next);
            ++g_applied;
        } catch (EditError&) {
            // A clean, user-facing rejection (e.g. "camera layers have no effects") is valid behavior, not a crash.
            ++rejected;
            ++g_rejected;
        }
        return r;
    }
    std::string add(const std::string& kind, json opts = json::object()) {
        OpResult r = apply({{"op", "addLayer"}, {"kind", kind}, {"options", opts}, {"at", 0.0}});
        std::string id = r.data.value("layer", std::string());
        if (!id.empty()) layers.push_back(id);
        return id;
    }
    std::string any() { return layers.empty() ? add("solid") : layers[rng() % layers.size()]; }
    void key(const std::string& id, const std::string& path, json a, json b) {
        apply({{"op", "setProp"}, {"layer", id}, {"path", path}, {"value", a}, {"t", 0.2}, {"mode", "key"}});
        apply({{"op", "setProp"}, {"layer", id}, {"path", path}, {"value", b}, {"t", 2.0}, {"mode", "key"}});
    }
};

struct Feature {
    const char* name;
    std::function<void(Builder&)> fn;
};

const std::vector<Feature>& features() {
    static const std::vector<Feature> f = {
        {"text", [](Builder& b) { b.add("text", {{"text", "Combo Title"}, {"size", 40}}); }},
        {"textAnimator", [](Builder& b) {
             std::string id = b.add("text", {{"text", "Animated words here"}, {"size", 30}});
             b.apply({{"op", "textPreset"}, {"layer", id}, {"preset", "fadeUp"}, {"t", 0.0}, {"duration", 1.0}});
         }},
        {"shapeStar", [](Builder& b) { b.add("shape", {{"shape", "star"}}); }},
        {"shapeOps", [](Builder& b) {
             std::string id = b.add("shape", {{"shape", "rect"}});
             json item = {{"type", "ellipse"}, {"op", "subtract"}};
             item["position"] = {{"v", {0, 0}}};
             item["size"] = {{"v", {120, 120}}};
             b.apply({{"op", "addShapeItem"}, {"layer", id}, {"item", item}});
             b.apply({{"op", "setProp"}, {"layer", id}, {"path", "shape.repeater.enabled"}, {"value", true}});
             b.apply({{"op", "setProp"}, {"layer", id}, {"path", "shape.trim.enabled"}, {"value", true}});
         }},
        {"solidMask", [](Builder& b) {
             std::string id = b.add("solid");
             b.apply({{"op", "addMask"}, {"layer", id}, {"shape", "ellipse"}});
         }},
        {"video", [](Builder& b) {
             json asset = {{"type", "video"}, {"name", "gen"}, {"width", 320}, {"height", 180}, {"fps", 24}, {"duration", 3.0}, {"generator", "counter"}};
             OpResult r = b.apply({{"op", "addAsset"}, {"asset", asset}});
             b.add("video", {{"asset", r.data["asset"]}});
         }},
        {"audioTone", [](Builder& b) {
             json asset = {{"type", "audio"}, {"name", "tone"}, {"toneHz", 330.0}, {"duration", 3.0}, {"hasAudio", true}, {"path", "tone:330"}};
             OpResult r = b.apply({{"op", "addAsset"}, {"asset", asset}});
             b.add("audio", {{"asset", r.data["asset"]}});
         }},
        {"threeDCameraLight", [](Builder& b) {
             std::string id = b.add("solid", {{"width", 120}, {"height", 120}});
             b.apply({{"op", "setLayer"}, {"layer", id}, {"fields", {{"threeD", true}}}});
             b.key(id, "transform.rotationY", 0.0, 60.0);
             b.add("camera");
             b.add("light", {{"light", "spot"}});
         }},
        {"model3d", [](Builder& b) { b.add("model3d", {{"primitive", "torus"}, {"size", 80.0}}); }},
        {"particles", [](Builder& b) { b.add("particles", {{"preset", "confetti"}}); }},
        {"motionBlur", [](Builder& b) {
             std::string id = b.any();
             b.key(id, "transform.position", json({40.0, 90.0, 0.0}), json({280.0, 90.0, 0.0}));
             b.apply({{"op", "setLayer"}, {"layer", id}, {"fields", {{"motionBlur", true}}}});
             b.apply({{"op", "updateComp"}, {"motionBlur", {{"enabled", true}, {"samples", 4}, {"shutter", 180}}}});
         }},
        {"precompose", [](Builder& b) {
             std::string a = b.add("solid", {{"width", 80}, {"height", 80}});
             std::string c = b.add("text", {{"text", "Nested"}});
             OpResult r = b.apply({{"op", "precompose"}, {"layers", {a, c}}, {"name", "Nested"}});
             b.layers.erase(std::remove_if(b.layers.begin(), b.layers.end(), [&](const std::string& x) { return x == a || x == c; }), b.layers.end());
             if (r.data.contains("layer")) b.layers.push_back(r.data["layer"]);
         }},
        {"adjustmentFx", [](Builder& b) {
             std::string id = b.add("adjustment");
             b.apply({{"op", "addEffect"}, {"layer", id}, {"type", "color.hueSaturation"}});
         }},
        {"trackMatte", [](Builder& b) {
             std::string fill = b.add("solid");
             std::string m = b.add("text", {{"text", "MATTE"}, {"size", 60}});
             b.apply({{"op", "setLayer"}, {"layer", fill}, {"fields", {{"matte", {{"layer", m}, {"mode", "alpha"}, {"invert", false}}}}}});
         }},
        {"expression", [](Builder& b) {
             std::string id = b.any();
             b.apply({{"op", "setExpression"}, {"layer", id}, {"path", "transform.rotation"}, {"expr", "wiggle(2, 20)"}});
         }},
        {"behavior", [](Builder& b) { b.apply({{"op", "addBehavior"}, {"layer", b.any()}, {"type", "shake"}}); }},
        {"keyframesEased", [](Builder& b) {
             std::string id = b.any();
             b.key(id, "transform.scale", json({50.0, 50.0, 100.0}), json({120.0, 120.0, 100.0}));
             b.apply({{"op", "setKeyframeInterp"}, {"layer", id}, {"path", "transform.scale"}, {"t", 0.2}, {"interp", "elastic"}, {"all", true}});
         }},
        {"transition", [](Builder& b) {
             b.apply({{"op", "setTransition"}, {"layer", b.any()}, {"edge", "in"}, {"transition", {{"type", "wipe"}, {"duration", 0.6}}}});
         }},
        {"speedReverse", [](Builder& b) {
             std::string id = b.any();
             b.apply({{"op", "setSpeed"}, {"layer", id}, {"speed", 1.5}});
             b.apply({{"op", "reverse"}, {"layer", id}, {"on", true}});
         }},
        {"captions", [](Builder& b) {
             json items = json::array();
             for (int i = 0; i < 6; ++i) items.push_back({{"start", i * 0.5}, {"end", i * 0.5 + 0.45}, {"text", "caption " + std::to_string(i)}});
             b.apply({{"op", "setCaptions"}, {"items", items}});
         }},
        {"blendMode", [](Builder& b) { b.apply({{"op", "setLayer"}, {"layer", b.any()}, {"fields", {{"blend", "screen"}}}}); }},
        {"parenting", [](Builder& b) {
             std::string n = b.add("null");
             std::string c = b.add("shape", {{"shape", "ellipse"}});
             b.apply({{"op", "parent"}, {"layer", c}, {"parent", n}});
             b.key(n, "transform.rotation", 0.0, 90.0);
         }},
        {"split", [](Builder& b) { b.apply({{"op", "split"}, {"layers", {b.any()}}, {"t", 1.3}}); }},
        {"randomEffect", [](Builder& b) {
             const auto& reg = effectRegistry();
             b.apply({{"op", "addEffect"}, {"layer", b.any()}, {"type", reg[b.rng() % reg.size()].type}});
         }},
    };
    return f;
}

uint64_t hashImage(const Image& img) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t v : img.px) h = (h ^ v) * 1099511628211ull;
    return h ^ ((uint64_t)img.w << 32) ^ (uint64_t)img.h;
}

// Builds one combination and checks all invariants. Returns false on failure (details printed).
bool runCombo(const std::vector<int>& feats, uint32_t seed, Renderer& R) {
    std::string label;
    for (int i : feats) label += std::string(features()[i].name) + "+";
    try {
        Builder b(seed);
        json initial = b.doc.doc();
        for (int i : feats) features()[i].fn(b);
        const json& doc = b.doc.doc();
        auto problems = validateProject(doc);
        if (!problems.empty()) { std::printf("    [%s] invalid: %s\n", label.c_str(), problems[0].c_str()); return false; }
        RenderSettings rs;
        rs.scale = 0.5;
        uint64_t h0 = 0;
        for (double t : {0.0, 0.9, 2.1}) {
            Image a = R.renderFrame(doc, "C1", t, rs);
            if (a.w != 160 || a.h != 90) { std::printf("    [%s] bad size %dx%d\n", label.c_str(), a.w, a.h); return false; }
            Image a2 = R.renderFrame(doc, "C1", t, rs);
            if (hashImage(a) != hashImage(a2)) { std::printf("    [%s] non-deterministic at t=%g\n", label.c_str(), t); return false; }
            if (t == 0.9) h0 = hashImage(a);
        }
        // Serialize round trip.
        json reparsed = json::parse(doc.dump());
        if (hashImage(R.renderFrame(reparsed, "C1", 0.9, rs)) != h0) { std::printf("    [%s] round-trip render differs\n", label.c_str()); return false; }
        // Audio mix must not crash and must be finite.
        AudioEngine ae(&g_cmedia);
        std::vector<float> buf(2048 * 2);
        ae.mix(doc, *findComp(doc, "C1"), 0.5, 2048, buf.data(), 48000);
        for (float v : buf) if (!std::isfinite(v)) { std::printf("    [%s] non-finite audio\n", label.c_str()); return false; }
        // Undo everything → initial; redo everything → final.
        json fin = doc;
        b.doc.jumpTo(0);
        if (b.doc.doc() != initial) { std::printf("    [%s] undo-all != initial\n", label.c_str()); return false; }
        b.doc.jumpTo((int)b.doc.history().size());
        if (b.doc.doc() != fin) { std::printf("    [%s] redo-all != final\n", label.c_str()); return false; }
    } catch (std::exception& e) {
        std::printf("    [%s] exception: %s\n", label.c_str(), e.what());
        return false;
    }
    return true;
}
}  // namespace

TEST(combo_all_feature_pairs) {
    Renderer R(&g_cmedia);
    int n = (int)features().size(), fails = 0, runs = 0;
    for (int i = 0; i < n; ++i)
        for (int j = i; j < n; ++j) {
            ++runs;
            if (!runCombo({i, j}, (uint32_t)(i * 131 + j), R)) ++fails;
        }
    std::printf("    %d pair combinations, %d failures\n", runs, fails);
    CHECK(fails == 0);
}

TEST(combo_random_feature_subsets) {
    Renderer R(&g_cmedia);
    int count = 400;
    if (const char* e = std::getenv("MF_COMBO_COUNT")) count = std::max(1, std::atoi(e));
    std::mt19937 rng(20261002);
    int n = (int)features().size(), fails = 0;
    for (int k = 0; k < count; ++k) {
        int size = 3 + (int)(rng() % 6);  // 3..8 features together
        std::vector<int> fs;
        for (int s = 0; s < size; ++s) fs.push_back((int)(rng() % n));
        if (!runCombo(fs, rng(), R)) ++fails;
    }
    std::printf("    %d random combinations (3-8 features each), %d failures; %d ops applied, %d cleanly rejected\n", count, fails, g_applied, g_rejected);
    CHECK(g_rejected * 10 < g_applied);  // combinations must mostly be valid edits, not rejections
    CHECK(fails == 0);
}

TEST(combo_every_effect_on_every_layer_kind) {
    Renderer R(&g_cmedia);
    int fails = 0, runs = 0;
    const char* kinds[] = {"text", "shape", "solid", "particles", "model3d", "adjustment"};
    for (auto& fx : effectRegistry())
        for (const char* k : kinds) {
            ++runs;
            try {
                Builder b(runs);
                json opts = std::string(k) == "text" ? json{{"text", "FX"}} : json::object();
                std::string id = b.add(k, opts);
                b.apply({{"op", "addEffect"}, {"layer", id}, {"type", fx.type}});
                // Animate the first numeric parameter so keyframed effect params are covered too.
                for (auto& p : fx.params)
                    if (p.kind == "number" || p.kind == "angle" || p.kind == "percent") {
                        const json& L = *findLayer(*findComp(b.doc.doc(), "C1"), id);
                        std::string eid = L["effects"].back()["id"];
                        b.key(id, "effects." + eid + ".params." + p.name, p.min, std::min(p.max, p.min + (p.max - p.min) * 0.5));
                        break;
                    }
                RenderSettings rs;
                rs.scale = 0.5;
                Image img = R.renderFrame(b.doc.doc(), "C1", 1.0, rs);
                if (img.w != 160) { ++fails; std::printf("    %s on %s: bad output\n", fx.type.c_str(), k); }
            } catch (std::exception& e) {
                ++fails;
                std::printf("    %s on %s: %s\n", fx.type.c_str(), k, e.what());
            }
        }
    std::printf("    %d effect x layer-kind combinations, %d failures\n", runs, fails);
    CHECK(fails == 0);
}

// ---------------------------------------------------------------- stress
namespace {
double ms(std::chrono::steady_clock::time_point a) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count(); }
}

TEST(stress_500_layers_with_keyframes) {
    Renderer R(&g_cmedia);
    json doc = newProject("Big", 1280, 720, 30, 10);
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 500; ++i) {
        OpResult r;
        const char* kind = i % 3 == 0 ? "text" : i % 3 == 1 ? "shape" : "solid";
        json opts = i % 3 == 0 ? json{{"text", "L" + std::to_string(i)}, {"size", 24}} : json{{"width", 40}, {"height", 40}};
        doc = applyOp(doc, {{"op", "addLayer"}, {"comp", "C1"}, {"kind", kind}, {"options", opts}, {"at", (i % 50) * 0.1}}, &r);
        std::string id = r.data["layer"];
        for (int k = 0; k < 4; ++k)
            doc = applyOp(doc, {{"op", "setProp"}, {"comp", "C1"}, {"layer", id}, {"path", "transform.position"}, {"value", {(i * 37) % 1280, (i * 53 + k * 90) % 720, 0}}, {"t", k * 1.0}, {"mode", "key"}});
    }
    double buildMs = ms(t0);
    t0 = std::chrono::steady_clock::now();
    RenderSettings rs;
    rs.scale = 0.5;
    Image img = R.renderFrame(doc, "C1", 2.5, rs);
    double renderMs = ms(t0);
    std::printf("    500 layers / 2000 keyframes: build %.0f ms, half-res frame %.0f ms, doc %zu KB\n", buildMs, renderMs, doc.dump().size() / 1024);
    CHECK(img.w == 640);
    CHECK(renderMs < 20000);
    CHECK(validateProject(doc).empty());
}

TEST(stress_4k_frame_and_heavy_effects) {
    Renderer R(&g_cmedia);
    json doc = newProject("4K", 3840, 2160, 30, 2);
    OpResult r;
    doc = applyOp(doc, {{"op", "addAsset"}, {"asset", {{"type", "video"}, {"name", "uhd"}, {"width", 3840}, {"height", 2160}, {"fps", 30}, {"duration", 2.0}, {"generator", "counter"}}}}, &r);
    doc = applyOp(doc, {{"op", "addLayer"}, {"comp", "C1"}, {"kind", "video"}, {"options", {{"asset", r.data["asset"]}}}}, &r);
    std::string id = r.data["layer"];
    for (const char* fx : {"blur.gaussian", "stylize.glow", "color.curves", "glitch.rgbSplit", "light.vignette"})
        doc = applyOp(doc, {{"op", "addEffect"}, {"comp", "C1"}, {"layer", id}, {"type", fx}});
    auto t0 = std::chrono::steady_clock::now();
    RenderSettings rs;
    rs.exportMode = true;
    Image img = R.renderFrame(doc, "C1", 1.0, rs);
    double t = ms(t0);
    std::printf("    4K frame with 5 effects: %.0f ms\n", t);
    CHECK(img.w == 3840 && img.h == 2160);
    CHECK(t < 60000);
}

TEST(stress_nested_comps_particles_3d_captions) {
    Renderer R(&g_cmedia);
    json doc = newProject("Deep", 640, 360, 30, 6);
    OpResult r;
    // 8 levels of nesting.
    doc = applyOp(doc, {{"op", "addLayer"}, {"comp", "C1"}, {"kind", "shape"}, {"options", {{"shape", "star"}}}}, &r);
    std::string inner = r.data["layer"];
    for (int d = 0; d < 8; ++d) {
        doc = applyOp(doc, {{"op", "precompose"}, {"comp", "C1"}, {"layers", {inner}}, {"name", "Level " + std::to_string(d)}}, &r);
        inner = r.data["layer"];
        doc = applyOp(doc, {{"op", "setProp"}, {"comp", "C1"}, {"layer", inner}, {"path", "transform.rotation"}, {"value", d * 5.0}, {"mode", "static"}});
    }
    for (int i = 0; i < 3; ++i) doc = applyOp(doc, {{"op", "addLayer"}, {"comp", "C1"}, {"kind", "particles"}, {"options", {{"preset", i == 0 ? "snow" : i == 1 ? "sparks" : "confetti"}}}}, &r);
    for (int i = 0; i < 20; ++i) doc = applyOp(doc, {{"op", "addLayer"}, {"comp", "C1"}, {"kind", "model3d"}, {"options", {{"primitive", i % 2 ? "cube" : "sphere"}, {"size", 40.0}}}}, &r);
    doc = applyOp(doc, {{"op", "addLayer"}, {"comp", "C1"}, {"kind", "light"}, {"options", {{"light", "point"}}}}, &r);
    json items = json::array();
    for (int i = 0; i < 2000; ++i) items.push_back({{"start", i * 0.003}, {"end", i * 0.003 + 0.0029}, {"text", "word " + std::to_string(i)}});
    doc = applyOp(doc, {{"op", "setCaptions"}, {"comp", "C1"}, {"items", items}});
    auto t0 = std::chrono::steady_clock::now();
    RenderSettings rs;
    rs.scale = 0.5;
    Image a = R.renderFrame(doc, "C1", 3.0, rs);
    double t = ms(t0);
    std::printf("    8-deep nesting + 3 particle systems + 20 3D models + 2000 captions: %.0f ms\n", t);
    CHECK(a.w == 320);
    CHECK(validateProject(doc).empty());
}

TEST(stress_undo_history_and_long_timeline) {
    Document d(newProject("Long", 640, 360, 30, 3600));  // one hour
    OpResult r;
    json doc = applyOp(d.doc(), {{"op", "addLayer"}, {"comp", "C1"}, {"kind", "text"}, {"options", {{"text", "Long"}}}}, &r);
    d.commit("add", doc);
    std::string id = r.data["layer"];
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i) {
        json n = applyOp(d.doc(), {{"op", "setProp"}, {"comp", "C1"}, {"layer", id}, {"path", "transform.opacity"}, {"value", (double)(i % 100)}, {"t", i * 3.0}, {"mode", "key"}});
        d.commit("key", n);
    }
    double t = ms(t0);
    int steps = 0;
    while (d.undo()) ++steps;
    std::printf("    1000 keyframe edits on a 1 h timeline: %.0f ms, undid %d steps\n", t, steps);
    CHECK(steps > 0);
    CHECK(!d.canUndo());
}

// GPU plan parity: for every pair of features, compositing the GPU draw list (reference compositor) must match the
// CPU renderer. Differences come only from resampling cached layer-space rasters, so thresholds are tight on the
// average and allow a thin band of edge pixels.
TEST(gpu_plan_matches_cpu_render_all_feature_pairs) {
    Renderer R(&g_cmedia);
    int n = (int)features().size(), fails = 0, runs = 0, cached = 0, video = 0, perFrame = 0, fallbacks = 0;
    for (int i = 0; i < n; ++i)
        for (int j = i; j < n; ++j) {
            Builder b((uint32_t)(i * 131 + j));
            features()[i].fn(b);
            features()[j].fn(b);
            const json& doc = b.doc.doc();
            RenderSettings rs;
            rs.scale = 0.5;
            for (double t : {0.9, 2.1}) {
                ++runs;
                Image ref = R.renderFrame(doc, "C1", t, rs);
                RenderPlan plan = R.renderPlan(doc, "C1", t, rs);
                Image got = compositePlan(plan, &g_cmedia);
                cached += plan.cachedRasters; video += plan.videoItems; perFrame += plan.frameRasters; fallbacks += plan.fallback;
                if (got.w != ref.w || got.h != ref.h) { ++fails; std::printf("    [%s+%s] size\n", features()[i].name, features()[j].name); continue; }
                double sum = 0;
                int bad = 0;
                for (size_t k = 0; k < ref.px.size(); ++k) {
                    int d = std::abs((int)ref.px[k] - (int)got.px[k]);
                    sum += d;
                    if (d > 64) ++bad;
                }
                double mean = sum / ref.px.size();
                double badFrac = (double)bad / ref.px.size();
                if (mean > 2.5 || badFrac > 0.01) {
                    ++fails;
                    std::printf("    [%s+%s t=%.1f] mean %.2f, %.2f%% far off (cached %d, video %d, per-frame %d, fallback %d)\n", features()[i].name,
                                features()[j].name, t, mean, badFrac * 100, plan.cachedRasters, plan.videoItems, plan.frameRasters, (int)plan.fallback);
                }
            }
        }
    std::printf("    %d frames: %d cached layer rasters, %d GPU video items, %d per-frame rasters, %d CPU fallbacks; %d mismatches\n", runs, cached,
                video, perFrame, fallbacks, fails);
    CHECK(fails == 0);
    CHECK(cached > 0 && video > 0);
}
