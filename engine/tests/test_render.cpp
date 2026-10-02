// Renderer, effects, compositing, 3D, particles, expressions.
#include "mf/expressions.hpp"
#include "mf/imageops.hpp"
#include "mf/model.hpp"
#include "mf/raster.hpp"
#include "mf/renderer.hpp"
#include <set>
#include "mftest.hpp"

using namespace mf;

namespace {
FileMediaProvider g_media;

struct Proj {
    json doc;
    std::string add(const std::string& kind, json options = json::object(), json overrides = json()) {
        OpResult r;
        json op = {{"op", "addLayer"}, {"kind", kind}, {"options", options}};
        doc = applyOp(doc, op, &r);
        std::string id = r.data["layer"];
        if (overrides.is_object()) {
            json* L = findLayerMut(*findCompMut(doc, "C1"), id);
            L->merge_patch(overrides);
        }
        return id;
    }
    void op(json o) { doc = applyOp(doc, o); }
    json& layer(const std::string& id) { return *findLayerMut(*findCompMut(doc, "C1"), id); }
};

Proj makeProj(int w = 320, int h = 180) {
    Proj p;
    p.doc = newProject("R", w, h, 30, 4);
    findCompMut(p.doc, "C1")->at("bg") = {0.0, 0.0, 0.0, 0.0};
    return p;
}

Image render(Renderer& r, const json& doc, double t, double scale = 1.0, RenderSettings rs = RenderSettings()) {
    rs.scale = scale;
    return r.renderFrame(doc, "C1", t, rs);
}

int countNonBlack(const Image& img) {
    int n = 0;
    for (size_t i = 0; i < img.px.size(); i += 4)
        if (img.px[i] > 8 || img.px[i + 1] > 8 || img.px[i + 2] > 8) ++n;
    return n;
}

const uint8_t* px(const Image& img, int x, int y) { return img.at(x, y); }
}  // namespace

TEST(raster_coverage_area_and_aa) {
    Polys p = flatten(BezierPath::ellipse(50, 50, 60, 60), 0.1);
    Coverage c = rasterize(p, {0, 0, 100, 100});
    double area = 0;
    for (float v : c.a) area += v;
    CHECK_NEAR(area, kPi * 30 * 30, 30);
    // Half-pixel offset square produces 0.5 coverage on edges.
    Polys sq{{{{10.5, 10}, {20.5, 10}, {20.5, 20}, {10.5, 20}}, true}};
    Coverage c2 = rasterize(sq, {0, 0, 40, 40});
    CHECK_NEAR(c2.at(10, 15), 0.5, 0.02);
    CHECK_NEAR(c2.at(15, 15), 1.0, 0.01);
    CHECK_NEAR(c2.at(20, 15), 0.5, 0.02);
    // Hole with opposite winding (nonzero) is empty; even-odd too.
    Polys ring = flatten(BezierPath::rect(50, 50, 80, 80), 0.5);
    Polys hole = flatten(BezierPath::rect(50, 50, 40, 40), 0.5);
    std::reverse(hole[0].pts.begin(), hole[0].pts.end());
    ring.push_back(hole[0]);
    Coverage c3 = rasterize(ring, {0, 0, 100, 100});
    CHECK_NEAR(c3.at(50, 50), 0.0, 0.01);
    CHECK_NEAR(c3.at(15, 50), 1.0, 0.01);
}

TEST(stroke_dash_trim_geometry) {
    Polys line{{{{0, 0}, {100, 0}}, false}};
    Polys s = strokePolys(line, 10, LineCap::Butt, LineJoin::Miter);
    Coverage c = rasterize(s, {-20, -20, 140, 20});
    double area = 0;
    for (float v : c.a) area += v;
    CHECK_NEAR(area, 1000, 15);
    Polys d = dashPolys(line, {10, 10}, 0);
    CHECK(d.size() == 5);
    Polys t = trimPolys(line, 0.25, 0.75, 0);
    CHECK_NEAR(contourLength(t[0]), 50, 1e-6);
    Polys closed = flatten(BezierPath::rect(0, 0, 100, 100), 1);
    Polys tc = trimPolys(closed, 0.0, 0.5, 0.9);  // wraps
    double L = 0;
    for (auto& c2 : tc) L += contourLength(c2);
    CHECK_NEAR(L, 200, 1e-6);
}

TEST(render_solid_exact_and_deterministic) {
    Renderer r(&g_media);
    Proj p = makeProj();
    std::string id = p.add("solid", {{"width", 100}, {"height", 50}, {"color", {1.0, 0.0, 0.0, 1.0}}});
    Image a = render(r, p.doc, 0);
    const uint8_t* c = px(a, 160, 90);
    CHECK(c[0] == 255 && c[1] == 0 && c[2] == 0 && c[3] == 255);
    CHECK(px(a, 5, 5)[0] == 0);
    // Edge at x = 110 exactly.
    CHECK(px(a, 109, 90)[0] == 0);
    CHECK(px(a, 110, 90)[0] == 255);
    Image b = render(r, p.doc, 0);
    CHECK(imageHash(a) == imageHash(b));
    // Opacity 50 -> ~128
    p.op({{"op", "setProp"}, {"layer", id}, {"path", "transform.opacity"}, {"value", 50.0}});
    Image h = render(r, p.doc, 0);
    CHECK_NEAR(px(h, 160, 90)[0], 128, 2);
    // Half-resolution preview keeps proportions.
    Image s = render(r, p.doc, 0, 0.5);
    CHECK(s.w == 160 && s.h == 90);
}

TEST(render_keyframed_position_and_parenting) {
    Renderer r(&g_media);
    Proj p = makeProj();
    std::string id = p.add("solid", {{"width", 20}, {"height", 20}, {"color", {0.0, 1.0, 0.0, 1.0}}});
    p.op({{"op", "addKeyframe"}, {"layer", id}, {"path", "transform.position"}, {"t", 0.0}, {"value", {20.0, 90.0, 0.0}}});
    p.op({{"op", "addKeyframe"}, {"layer", id}, {"path", "transform.position"}, {"t", 1.0}, {"value", {220.0, 90.0, 0.0}}});
    Image a = render(r, p.doc, 0.5);
    CHECK(px(a, 120, 90)[1] == 255);
    CHECK(px(a, 20, 90)[1] == 0);
    std::string parent = p.add("null");
    p.op({{"op", "setProp"}, {"layer", parent}, {"path", "transform.position"}, {"value", {160.0, 90.0 + 40.0, 0.0}}});
    p.op({{"op", "setLayer"}, {"layer", id}, {"fields", {{"parent", parent}}}});
    // Child now offset by parent's position relative to its anchor (0,0): x = 160 + 120, y = 130 + 90 => outside 180 -> clipped
    Image b = render(r, p.doc, 0.0);
    CHECK(px(b, 20, 90)[1] == 0);
}

TEST(render_text_and_animator_reveal) {
    Renderer r(&g_media);
    Proj p = makeProj(640, 360);
    std::string id = p.add("text", {{"text", "MOTION"}, {"size", 80.0}});
    Image a = render(r, p.doc, 0);
    int lit = countNonBlack(a);
    CHECK(lit > 2000);
    p.op({{"op", "textPreset"}, {"layer", id}, {"preset", "fadeUp"}, {"duration", 1.0}});
    Image b = render(r, p.doc, 0.0);
    Image c = render(r, p.doc, 1.2);
    CHECK(countNonBlack(b) < lit / 10);
    CHECK(std::abs(countNonBlack(c) - lit) < lit / 20);
    // Typewriter reveals progressively.
    Proj q = makeProj(640, 360);
    std::string t2 = q.add("text", {{"text", "ABCDEFGH"}, {"size", 60.0}});
    q.op({{"op", "textPreset"}, {"layer", t2}, {"preset", "typeOn"}, {"duration", 2.0}});
    int n1 = countNonBlack(render(r, q.doc, 0.5)), n2 = countNonBlack(render(r, q.doc, 1.5));
    CHECK(n1 > 0 && n2 > n1 * 2);
}

TEST(render_shapes_fill_stroke_gradient_trim_repeater_merge) {
    Renderer r(&g_media);
    Proj p = makeProj(400, 300);
    std::string id = p.add("shape", {{"shape", "ellipse"}, {"color", {0.0, 0.0, 1.0, 1.0}}});
    Image a = render(r, p.doc, 0);
    CHECK(px(a, 200, 150)[2] == 255);
    json& L = p.layer(id);
    L["shape"]["stroke"]["enabled"] = true;
    L["shape"]["stroke"]["width"] = makeProp(10.0);
    L["shape"]["fill"]["type"] = "linear";
    Image b = render(r, p.doc, 0);
    CHECK(px(b, 5, 5)[3] == 0);
    CHECK(px(b, 200, 150 + 148)[0] > 200);  // stroke (white) at bottom edge of 300px ellipse
    CHECK(px(b, 80, 150)[0] > px(b, 300, 150)[0]);  // gradient varies left->right (red->blue)
    L["shape"]["trim"]["enabled"] = true;
    L["shape"]["trim"]["end"] = makeProp(50.0);
    L["shape"]["fill"]["enabled"] = false;
    Image c = render(r, p.doc, 0);
    int left = 0, right = 0;
    for (int y = 0; y < 300; ++y) { if (px(c, 52, y)[3] > 0) ++left; if (px(c, 348, y)[3] > 0) ++right; }
    CHECK(right > 0 && left == 0);  // first half of the path (top -> right -> bottom)
    // Repeater with 3 copies of a small rect.
    Proj q = makeProj(400, 100);
    std::string sid = q.add("shape", {{"shape", "rect"}});
    json& S = q.layer(sid);
    S["shape"]["items"][0]["size"] = makeProp({20.0, 20.0});
    S["shape"]["repeater"]["enabled"] = true;
    S["shape"]["repeater"]["position"] = makeProp({50.0, 0.0});
    S["transform"]["position"] = makeProp({100.0, 50.0, 0.0});
    Image d = render(r, q.doc, 0);
    CHECK(px(d, 100, 50)[3] == 255 && px(d, 150, 50)[3] == 255 && px(d, 200, 50)[3] == 255 && px(d, 125, 50)[3] == 0);
    // Merge subtract and intersect.
    json item2 = {{"type", "rect"}, {"size", makeProp({20.0, 20.0})}, {"position", makeProp({0.0, 0.0})}, {"op", "subtract"}};
    q.op({{"op", "addShapeItem"}, {"layer", sid}, {"item", item2}});
    q.layer(sid)["shape"]["repeater"]["enabled"] = false;
    q.layer(sid)["shape"]["items"][0]["size"] = makeProp({60.0, 60.0});
    Image e = render(r, q.doc, 0);
    CHECK(px(e, 100, 50)[3] == 0);
    CHECK(px(e, 125, 50)[3] == 255);
    q.layer(sid)["shape"]["items"][1]["op"] = "intersect";
    Image f = render(r, q.doc, 0);
    CHECK(px(f, 100, 50)[3] == 255);
    CHECK(px(f, 125, 50)[3] == 0);
}

TEST(render_blend_modes_mask_matte_adjustment) {
    Renderer r(&g_media);
    Proj p = makeProj();
    p.add("solid", {{"color", {0.5, 0.5, 0.5, 1.0}}});
    std::string top = p.add("solid", {{"color", {1.0, 0.0, 0.0, 1.0}}});
    for (auto& m : blendModeNames()) {
        p.layer(top)["blend"] = m;
        Image img = render(r, p.doc, 0);
        const uint8_t* c = px(img, 100, 100);
        if (m == "multiply") { CHECK_NEAR(c[0], 128, 2); CHECK_NEAR(c[1], 0, 1); }
        if (m == "screen") { CHECK(c[0] == 255); CHECK_NEAR(c[1], 128, 2); }
        if (m == "difference") { CHECK_NEAR(c[0], 127, 2); CHECK_NEAR(c[1], 128, 2); }
        if (m == "normal") { CHECK(c[0] == 255 && c[1] == 0); }
        CHECK(c[3] == 255);
    }
    p.layer(top)["blend"] = "normal";
    OpResult mr;
    p.doc = applyOp(p.doc, {{"op", "addMask"}, {"layer", top}, {"shape", "ellipse"}, {"rect", {110, 40, 100, 100}}}, &mr);
    Image m = render(r, p.doc, 0);
    CHECK(px(m, 160, 90)[0] == 255);
    CHECK_NEAR(px(m, 20, 20)[0], 128, 2);
    p.op({{"op", "setMask"}, {"layer", top}, {"mask", mr.data["mask"]}, {"fields", {{"inverted", true}}}});
    Image mi = render(r, p.doc, 0);
    CHECK_NEAR(px(mi, 160, 90)[0], 128, 2);
    CHECK(px(mi, 20, 20)[0] == 255);
    // Track matte: text-shaped alpha matte.
    Proj q = makeProj(400, 200);
    std::string fill = q.add("solid", {{"color", {0.0, 1.0, 0.0, 1.0}}});
    std::string mt = q.add("shape", {{"shape", "rect"}});
    q.layer(mt)["shape"]["items"][0]["size"] = makeProp({100.0, 100.0});
    q.op({{"op", "setLayer"}, {"layer", fill}, {"fields", {{"matte", {{"layer", mt}, {"mode", "alpha"}, {"invert", false}}}}}});
    Image t = render(r, q.doc, 0);
    CHECK(px(t, 200, 100)[1] == 255);
    CHECK(px(t, 20, 100)[1] == 0);
    CHECK(px(t, 200, 100)[0] == 0);  // matte layer itself hidden
    // Adjustment layer inverts below.
    Proj a = makeProj();
    a.add("solid", {{"color", {1.0, 1.0, 1.0, 1.0}}});
    std::string adj = a.add("adjustment");
    a.op({{"op", "addEffect"}, {"layer", adj}, {"type", "color.invert"}});
    Image ai = render(r, a.doc, 0);
    CHECK(px(ai, 50, 50)[0] == 0);
}

TEST(render_precomp_matches_direct) {
    Renderer r(&g_media);
    Proj p = makeProj();
    std::string s1 = p.add("solid", {{"width", 60}, {"height", 60}, {"color", {1.0, 1.0, 0.0, 1.0}}});
    Image direct = render(r, p.doc, 0);
    OpResult pr;
    p.doc = applyOp(p.doc, {{"op", "precompose"}, {"layers", {s1}}}, &pr);
    RenderSettings rs;
    rs.revision = 1;
    Image nested = render(r, p.doc, 0, 1.0, rs);
    CHECK(psnr(direct, nested) > 40);
    RenderStats st;
    rs.scale = 1;
    r.renderFrame(p.doc, "C1", 0, rs, &st);
    CHECK(st.cacheHits >= 1);  // second render reuses precomp cache
}

TEST(render_3d_camera_perspective_and_mesh) {
    Renderer r(&g_media);
    Proj p = makeProj(640, 360);
    std::string s = p.add("solid", {{"width", 200}, {"height", 200}, {"color", {1.0, 1.0, 1.0, 1.0}}});
    p.layer(s)["threeD"] = true;
    Image flat = render(r, p.doc, 0);
    int area0 = countNonBlack(flat);
    CHECK_NEAR(area0, 40000, 900);  // default camera: z=0 plane maps 1:1
    p.op({{"op", "setProp"}, {"layer", s}, {"path", "transform.rotationY"}, {"value", 60.0}});
    int area1 = countNonBlack(render(r, p.doc, 0));
    CHECK_NEAR(area1, 20000, 2500);  // foreshortened ~cos(60)
    p.op({{"op", "setProp"}, {"layer", s}, {"path", "transform.rotationY"}, {"value", 0.0}});
    p.op({{"op", "setProp"}, {"layer", s}, {"path", "transform.position"}, {"value", {320.0, 180.0, 768.0}}});
    int area2 = countNonBlack(render(r, p.doc, 0));
    CHECK(area2 < area0 / 2);  // farther away -> smaller
    // Camera layer dolly.
    std::string cam = p.add("camera");
    p.op({{"op", "setProp"}, {"layer", s}, {"path", "transform.position"}, {"value", {320.0, 180.0, 0.0}}});
    int area3 = countNonBlack(render(r, p.doc, 0));
    CHECK_NEAR(area3, 40000, 1500);
    double zoom = p.layer(cam)["camera"]["zoom"]["v"].get<double>();
    p.op({{"op", "setProp"}, {"layer", cam}, {"path", "transform.position"}, {"value", {320.0, 180.0, -zoom / 2}}});
    int area4 = countNonBlack(render(r, p.doc, 0));
    CHECK(area4 > area3 * 3);
    // Mesh: lit cube shows distinct face shading.
    Proj m = makeProj(400, 300);
    std::string cube = m.add("model3d", {{"primitive", "cube"}, {"size", 150.0}});
    m.op({{"op", "setProp"}, {"layer", cube}, {"path", "transform.rotationY"}, {"value", 35.0}});
    m.op({{"op", "setProp"}, {"layer", cube}, {"path", "transform.rotationX"}, {"value", -25.0}});
    std::string light = m.add("light", {{"light", "directional"}});
    (void)light;
    Image mi = render(r, m.doc, 0);
    CHECK(countNonBlack(mi) > 15000);
    std::set<int> shades;
    for (int y = 60; y < 240; y += 6)
        for (int x = 100; x < 300; x += 6)
            if (px(mi, x, y)[3] == 255) shades.insert(px(mi, x, y)[0] / 16);
    CHECK(shades.size() >= 3);
    // Extruded 3D text renders.
    Proj e = makeProj(640, 360);
    std::string tx = e.add("text", {{"text", "3D"}, {"size", 150.0}});
    e.layer(tx)["threeD"] = true;
    e.layer(tx)["text"]["extrude"]["enabled"] = true;
    e.op({{"op", "setProp"}, {"layer", tx}, {"path", "transform.rotationY"}, {"value", 30.0}});
    CHECK(countNonBlack(render(r, e.doc, 0)) > 3000);
}

TEST(render_all_effects_run_and_are_deterministic) {
    Renderer r(&g_media);
    for (auto& info : effectRegistry()) {
        Proj p = makeProj(160, 90);
        p.add("solid", {{"color", {0.2, 0.6, 0.9, 1.0}}});
        std::string t = p.add("text", {{"text", "FX"}, {"size", 50.0}});
        p.op({{"op", "addEffect"}, {"layer", t}, {"type", info.type}});
        Image a = render(r, p.doc, 0.4);
        Image b = render(r, p.doc, 0.4);
        if (imageHash(a) != imageHash(b)) std::printf("    nondeterministic: %s\n", info.type.c_str());
        CHECK(imageHash(a) == imageHash(b));
        CHECK(a.w == 160 && a.h == 90);
    }
}

TEST(render_effect_semantics) {
    Renderer r(&g_media);
    Proj p = makeProj(200, 100);
    std::string s = p.add("solid", {{"color", {0.8, 0.2, 0.2, 1.0}}});
    OpResult e;
    p.doc = applyOp(p.doc, {{"op", "addEffect"}, {"layer", s}, {"type", "color.blackWhite"}}, &e);
    const uint8_t* c = px(render(r, p.doc, 0), 100, 50);
    CHECK(std::abs(c[0] - c[1]) <= 1 && std::abs(c[1] - c[2]) <= 1);
    p.op({{"op", "removeEffect"}, {"layer", s}, {"effect", e.data["effect"]}});
    // Chroma key removes green screen.
    Proj k = makeProj(320, 180);
    OpResult ar;
    k.doc = applyOp(k.doc, {{"op", "addAsset"}, {"asset", {{"type", "video"}, {"name", "gs"}, {"width", 320}, {"height", 180}, {"fps", 30}, {"duration", 2}, {"generator", "green"}}}}, &ar);
    std::string v = k.add("video", {{"asset", ar.data["asset"]}});
    k.op({{"op", "addEffect"}, {"layer", v}, {"type", "key.chroma"}});
    Image ki = render(r, k.doc, 0);
    CHECK(px(ki, 10, 10)[1] < 40);      // green gone (black bg)
    CHECK(px(ki, 160, 90)[0] > 180);    // subject kept
    // LUT identity leaves colors unchanged.
    std::string lutPath = mftest::tmpDir() + "/identity.cube";
    {
        FILE* f = std::fopen(lutPath.c_str(), "w");
        std::fprintf(f, "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n");
        std::fclose(f);
    }
    LutData lut;
    std::string err;
    CHECK(lut.load(lutPath, err));
    float o1, o2, o3;
    lut.sample(0.3f, 0.6f, 0.9f, o1, o2, o3);
    CHECK_NEAR(o1, 0.3, 1e-5);
    CHECK_NEAR(o3, 0.9, 1e-5);
}

TEST(render_transitions_and_motion_blur) {
    Renderer r(&g_media);
    for (auto& tr : transitionRegistry()) {
        Proj p = makeProj(160, 90);
        std::string s = p.add("solid", {{"color", {1.0, 1.0, 1.0, 1.0}}});
        p.op({{"op", "setTransition"}, {"layer", s}, {"edge", "in"}, {"transition", {{"type", tr.type}, {"duration", 1.0}}}});
        int mid = countNonBlack(render(r, p.doc, 0.5));
        int end = countNonBlack(render(r, p.doc, 1.5));
        CHECK(end == 160 * 90);
        CHECK(mid <= end);
    }
    Proj m = makeProj(320, 100);
    std::string s = m.add("solid", {{"width", 20}, {"height", 20}, {"color", {1.0, 1.0, 1.0, 1.0}}});
    m.op({{"op", "addKeyframe"}, {"layer", s}, {"path", "transform.position"}, {"t", 0.0}, {"value", {0.0, 50.0, 0.0}}});
    m.op({{"op", "addKeyframe"}, {"layer", s}, {"path", "transform.position"}, {"t", 1.0}, {"value", {300.0, 50.0, 0.0}}});
    m.layer(s)["motionBlur"] = true;
    findCompMut(m.doc, "C1")->at("motionBlur")["enabled"] = true;
    Image b = render(r, m.doc, 0.5);
    int partial = 0;
    for (int x = 0; x < 320; ++x) { int v = px(b, x, 50)[0]; if (v > 10 && v < 245) ++partial; }
    CHECK(partial > 4);  // smeared edges
}

TEST(render_particles_presets) {
    Renderer r(&g_media);
    for (std::string pr : {"sparks", "dust", "snow", "rain", "smoke", "magic", "embers", "stars", "confetti"}) {
        Proj p = makeProj(320, 180);
        p.add("particles", {{"preset", pr}});
        int n = countNonBlack(render(r, p.doc, 1.5));
        if (n == 0) std::printf("    empty particles: %s\n", pr.c_str());
        CHECK(n > 0);
    }
}

TEST(render_captions_layer_highlight) {
    Renderer r(&g_media);
    Proj p = makeProj(640, 360);
    json w1 = {{"s", 0.0}, {"e", 0.5}, {"w", "Hello"}};
    json w2 = {{"s", 0.5}, {"e", 1.0}, {"w", "there"}};
    p.op({{"op", "setCaptions"}, {"items", json::array({{{"start", 0.0}, {"end", 1.0}, {"text", "Hello there"}, {"words", {w1, w2}}}})}});
    Image a = render(r, p.doc, 0.25), b = render(r, p.doc, 0.75), c = render(r, p.doc, 2.0);
    CHECK(countNonBlack(a) > 500);
    CHECK(countNonBlack(c) == 0);
    CHECK(imageHash(a) != imageHash(b));  // active word highlight moves
}

TEST(expressions_vector_math_wiggle_loop_refs_errors) {
    auto eng = createExpressionEngine();
    std::string out;
    CHECK(rewriteVectorOperators("value + [10, 0]", out));
    CHECK(out.find("__add") != std::string::npos);
    json doc = newProject("X", 1920, 1080, 30, 10);
    OpResult a, b;
    doc = applyOp(doc, {{"op", "addLayer"}, {"kind", "null"}, {"options", {{"name", "Ctrl"}}}}, &a);
    doc = applyOp(doc, {{"op", "addLayer"}, {"kind", "solid"}}, &b);
    doc = applyOp(doc, {{"op", "setProp"}, {"layer", a.data["layer"]}, {"path", "transform.position"}, {"value", {111.0, 222.0, 0.0}}});
    const json& comp = *activeComp(doc);
    const json& L = *findLayer(comp, b.data["layer"]);
    EvalContext ctx;
    ctx.project = &doc;
    ctx.comp = &comp;
    ctx.layer = &L;
    ctx.fps = 30;
    ctx.compTime = 1.0;
    ctx.layerTime = 1.0;
    ctx.expr = eng.get();
    ctx.propPath = "transform.position";
    json prop = {{"v", {100.0, 50.0, 0.0}}, {"x", "value + [Math.sin(time * 4) * 20, 0]"}};
    Value v = evalProperty(prop, ctx);
    CHECK_NEAR(v.n[0], 100 + std::sin(4.0) * 20, 1e-9);
    CHECK_NEAR(v.n[1], 50, 1e-9);
    prop["x"] = "thisComp.layer(\"Ctrl\").transform.position";
    v = evalProperty(prop, ctx);
    CHECK_NEAR(v.n[0], 111, 1e-9);
    prop["x"] = "wiggle(5, 20)";
    Value w1 = evalProperty(prop, ctx), w2 = evalProperty(prop, ctx);
    CHECK(w1.n == w2.n);  // deterministic
    CHECK(std::fabs(w1.n[0] - 100) <= 20.0001);
    ctx.compTime = 1.37;
    Value w3 = evalProperty(prop, ctx);
    CHECK(w3.n != w1.n);
    // loopOut cycle.
    json kp = {{"k", json::array({{{"t", 0.0}, {"v", 0.0}}, {{"t", 1.0}, {"v", 10.0}}})}, {"x", "loopOut()"}};
    ctx.compTime = 2.5;
    ctx.layerTime = 2.5;
    CHECK_NEAR(evalProperty(kp, ctx).num(), 5.0, 1e-6);
    kp["x"] = "loopOut('pingpong')";
    ctx.compTime = 1.25; ctx.layerTime = 1.25;
    CHECK_NEAR(evalProperty(kp, ctx).num(), 7.5, 1e-6);
    kp["x"] = "linear(time, 0, 2, 0, 100)";
    CHECK_NEAR(evalProperty(kp, ctx).num(), 62.5, 1e-9);
    kp["x"] = "clamp(value * 3, 0, 20)";
    CHECK_NEAR(evalProperty(kp, ctx).num(), 20.0, 1e-9);
    // Errors are non-destructive and visible.
    kp["x"] = "thisLayr.opacity";
    double raw = evalRaw(kp, 1.25).num();
    CHECK_NEAR(evalProperty(kp, ctx).num(), raw, 1e-9);
    auto errs = eng->errors();
    bool found = false;
    for (auto& [k, e] : errs) if (e.find("thisLayr") != std::string::npos) found = true;
    CHECK(found);
    // Infinite loop is interrupted.
    kp["x"] = "while(true){}";
    CHECK_NEAR(evalProperty(kp, ctx).num(), raw, 1e-9);
    std::string err;
    CHECK(!eng->validate("value +", err));
    CHECK(!err.empty());
}

TEST(render_hit_test_and_quad) {
    Renderer r(&g_media);
    Proj p = makeProj(400, 300);
    std::string s = p.add("solid", {{"width", 100}, {"height", 100}});
    std::string t = p.add("text", {{"text", "TOP"}, {"size", 40.0}});
    CHECK(r.hitTest(p.doc, "C1", 0, 200, 150) == t);
    CHECK(r.hitTest(p.doc, "C1", 0, 160, 110) == s);
    CHECK(r.hitTest(p.doc, "C1", 0, 10, 10).empty());
    Vec2 q[4];
    CHECK(r.layerQuad(p.doc, "C1", s, 0, q));
    CHECK_NEAR(q[0].x, 150, 1e-6);
    CHECK_NEAR(q[2].y, 200, 1e-6);
}
