// Forge Capsules v2: typed controls (text, color, size, position, intensity, speed) and self-contained packages.
#include "mf/document.hpp"
#include "mf/imageops.hpp"
#include "mf/model.hpp"
#include "mf/package.hpp"
#include "mf/renderer.hpp"
#include "mf/storage.hpp"
#include "mf/text.hpp"
#include "mftest.hpp"

using namespace mf;

namespace {
FileMediaProvider g_capMedia;

uint64_t hashImg(const Image& img) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t v : img.px) h = (h ^ v) * 1099511628211ull;
    return h;
}

// A capsule whose comp has an animated shape + a title, exposing all control types.
json makeCapsule(const std::string& imagePath) {
    json doc = newProject("Cap src", 320, 180, 24, 3);
    OpResult r;
    doc = applyOp(doc, {{"op", "addLayer"}, {"comp", "C1"}, {"kind", "shape"}, {"options", {{"shape", "rect"}, {"color", {1.0, 0.2, 0.2, 1.0}}}}}, &r);
    std::string shape = r.data["layer"];
    doc = applyOp(doc, {{"op", "setProp"}, {"comp", "C1"}, {"layer", shape}, {"path", "shape.items.0.size"}, {"value", {60, 40}}, {"mode", "static"}});
    doc = applyOp(doc, {{"op", "setProp"}, {"comp", "C1"}, {"layer", shape}, {"path", "transform.position"}, {"value", {60, 90, 0}}, {"t", 0.0}, {"mode", "key"}});
    doc = applyOp(doc, {{"op", "setProp"}, {"comp", "C1"}, {"layer", shape}, {"path", "transform.position"}, {"value", {260, 90, 0}}, {"t", 2.0}, {"mode", "key"}});
    doc = applyOp(doc, {{"op", "addEffect"}, {"comp", "C1"}, {"layer", shape}, {"type", "stylize.glow"}});
    doc = applyOp(doc, {{"op", "addLayer"}, {"comp", "C1"}, {"kind", "text"}, {"options", {{"text", "CAPSULE"}, {"size", 30}}}}, &r);
    std::string title = r.data["layer"];
    json asset = {{"type", "image"}, {"name", "logo.png"}, {"path", imagePath}, {"width", 16}, {"height", 16}};
    doc = applyOp(doc, {{"op", "addAsset"}, {"asset", asset}}, &r);
    std::string aid = r.data["asset"];
    doc = applyOp(doc, {{"op", "addLayer"}, {"comp", "C1"}, {"kind", "image"}, {"options", {{"asset", aid}, {"fit", false}}}}, &r);
    json fx = findLayer(*findComp(doc, "C1"), shape)->at("effects")[0];
    std::string fxParam = "intensity";
    // Threshold 0 so the (not very bright) red shape actually glows and intensity is visible.
    doc = applyOp(doc, {{"op", "setProp"}, {"comp", "C1"}, {"layer", shape}, {"path", "effects." + fx.value("id", std::string()) + ".params.threshold"}, {"value", 0.0}, {"mode", "static"}});
    json controls = json::array({
        {{"name", "Title"}, {"type", "text"}, {"layer", title}, {"path", "text.content"}, {"default", "CAPSULE"}},
        {{"name", "Color"}, {"type", "color"}, {"layer", shape}, {"path", "shape.fill.color"}, {"default", {1.0, 0.2, 0.2, 1.0}}},
        {{"name", "Size"}, {"type", "size"}, {"layer", shape}, {"path", "transform.scale"}, {"base", 100}, {"default", 100}},
        {{"name", "Position"}, {"type", "position"}, {"layer", shape}, {"path", "transform.position"}, {"default", {0, 0}}},
        {{"name", "Glow"}, {"type", "intensity"}, {"layer", shape}, {"path", "effects." + fx.value("id", std::string()) + ".params." + fxParam}, {"base", 100}, {"default", 100}},
        {{"name", "Speed"}, {"type", "speed"}, {"layer", shape}, {"path", ""}, {"base", 100}, {"default", 100}},
    });
    const json& comp = *findComp(doc, "C1");
    json assets = json::array({*findAsset(doc, aid)});
    return {{"format", "mfcapsule"}, {"version", "2.0"}, {"name", "Test Capsule"}, {"comp", comp}, {"comps", json::array()}, {"assets", assets}, {"controls", controls}};
}

json insert(const json& cap, std::string& lid) {
    json doc = newProject("Host", 320, 180, 24, 3);
    findCompMut(doc, "C1")->at("bg") = {0.0, 0.0, 0.0, 1.0};
    for (auto& a : cap["assets"]) doc["assets"].push_back(a);
    OpResult r;
    doc = applyOp(doc, {{"op", "insertCapsule"}, {"comp", "C1"}, {"capsule", cap}, {"t", 0.0}}, &r);
    lid = r.data["layer"];
    return doc;
}
}  // namespace

TEST(capsule_v2_every_control_type_changes_output) {
    std::string img = mftest::tmpDir() + "/logo.png";
    Image logo(16, 16);
    for (size_t i = 0; i < logo.px.size(); i += 4) { logo.px[i] = 0; logo.px[i + 1] = 255; logo.px[i + 2] = 0; logo.px[i + 3] = 255; }
    REQUIRE(savePng(logo, img));
    json cap = makeCapsule(img);
    std::string lid;
    json doc = insert(cap, lid);
    Renderer R(&g_capMedia);
    RenderSettings rs;
    uint64_t base = hashImg(R.renderFrame(doc, "C1", 1.0, rs));
    auto with = [&](const std::string& control, json value) {
        json d = applyOp(doc, {{"op", "setCapsuleControl"}, {"comp", "C1"}, {"layer", lid}, {"control", control}, {"value", value}});
        return hashImg(R.renderFrame(d, "C1", 1.0, rs));
    };
    CHECK(with("Title", "CHANGED") != base);
    CHECK(with("Color", {0.1, 0.3, 1.0, 1.0}) != base);
    CHECK(with("Size", 200.0) != base);
    CHECK(with("Position", {0.0, 40.0}) != base);
    CHECK(with("Glow", 0.0) != base);
    CHECK(with("Speed", 200.0) != base);
    // Default values reproduce the author's render exactly.
    CHECK(with("Size", 100.0) == base);
    CHECK(with("Position", {0.0, 0.0}) == base);
    // Speed 200% at t=0.5 equals speed 100% at t=1.0 (time scaling of the internal animation).
    json fast = applyOp(doc, {{"op", "setCapsuleControl"}, {"comp", "C1"}, {"layer", lid}, {"control", "Speed"}, {"value", 200.0}});
    CHECK(hashImg(R.renderFrame(fast, "C1", 0.5, rs)) == base);
    // Unknown controls are rejected cleanly.
    bool threw = false;
    try { applyOp(doc, {{"op", "setCapsuleControl"}, {"comp", "C1"}, {"layer", lid}, {"control", "Nope"}, {"value", 1}}); } catch (EditError&) { threw = true; }
    CHECK(threw);
}

TEST(capsule_v2_package_embeds_fonts_and_media) {
    std::string dir = mftest::tmpDir() + "/capsule_pkg";
    makeDirs(dir);
    std::string img = dir + "/logo.png";
    Image logo(16, 16);
    for (size_t i = 0; i < logo.px.size(); i += 4) { logo.px[i] = 0; logo.px[i + 1] = 0; logo.px[i + 2] = 255; logo.px[i + 3] = 255; }
    REQUIRE(savePng(logo, img));
    json cap = makeCapsule(img);
    std::string pkgPath = dir + "/test.mfcapsule";
    std::string err;
    std::vector<std::string> warnings;
    auto assetPath = [](const json& a) { return a.value("path", std::string()); };
    auto fontPath = [](const std::string& f) { auto font = FontManager::instance().get(f); return font ? font->path : std::string(); };
    REQUIRE(exportCapsulePackage(pkgPath, cap, assetPath, fontPath, PackageWriteOptions(), err, &warnings));
    CHECK(warnings.empty());
    // Delete the original media: the imported capsule must render from the packaged copy.
    ::unlink(img.c_str());
    json imported;
    std::vector<std::string> fonts;
    REQUIRE(importCapsulePackage(pkgPath, dir + "/extracted", imported, fonts, PackageReadOptions(), err, &warnings));
    CHECK(fonts.size() == 1);
    CHECK(fileExists(fonts.empty() ? std::string() : fonts[0]));
    REQUIRE(imported["assets"].size() == 1);
    std::string newPath = imported["assets"][0].value("path", std::string());
    CHECK(fileExists(newPath));
    CHECK(newPath != img);
    CHECK(imported["controls"].size() == 6);
    std::string lid;
    json doc = insert(imported, lid);
    Renderer R(&g_capMedia);
    Image out = R.renderFrame(doc, "C1", 1.0, RenderSettings());
    int blue = 0;
    for (size_t i = 0; i < out.px.size(); i += 4) if (out.px[i + 2] > 200 && out.px[i] < 60 && out.px[i + 1] < 60) ++blue;
    CHECK(blue > 50);  // the packaged logo image renders
    // A non-capsule package is rejected by the capsule importer.
    std::string other = dir + "/preset.mffx";
    REQUIRE(exportJsonPackage(other, "preset", json{{"name", "p"}}, json::object(), PackageWriteOptions(), err));
    CHECK(!importCapsulePackage(other, dir + "/x", imported, fonts, PackageReadOptions(), err, nullptr));
}
