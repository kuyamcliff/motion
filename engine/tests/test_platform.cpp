// Storage/recovery, packages, scripting, captions/subtitles, GIF, tracking, audio, scenarios.
#include <cstdio>

#include "mf/audio.hpp"
#include "mf/captions.hpp"
#include "mf/engine.hpp"
#include "mf/gif.hpp"
#include "mf/imageops.hpp"
#include "mf/model.hpp"
#include "mf/effects.hpp"
#include "mf/package.hpp"
#include "mf/scripting.hpp"
#include "mf/storage.hpp"
#include "mf/tracking.hpp"
#include "mftest.hpp"

using namespace mf;

static std::string freshDir(const std::string& name) {
    std::string d = mftest::tmpDir() + "/" + name;
    removeTree(d);
    makeDirs(d);
    return d;
}

TEST(storage_atomic_save_journal_recovery) {
    std::string root = freshDir("store");
    ProjectStore store(root);
    json doc = newProject("Store", 1280, 720, 30, 5);
    std::string err;
    std::string id = store.create(doc, &err);
    REQUIRE(!id.empty());
    store.markOpen(id);
    // Simulate edits journaled after the checkpoint, then a crash (no save, lock left behind).
    Document d(doc);
    uint64_t seq = 0;
    d.onCommit = [&](const std::string& label, const json& patch, uint64_t) { store.appendJournal(id, ++seq, label, patch); };
    d.commit("Add Text", applyOp(d.doc(), {{"op", "addLayer"}, {"kind", "text"}, {"options", {{"text", "Recovered"}}}}));
    d.commit("Rename", applyOp(d.doc(), {{"op", "setProjectName"}, {"name", "After Crash"}}));
    RecoveryInfo ri = store.checkRecovery(id);
    CHECK(ri.available);
    CHECK(ri.abnormalExit);
    CHECK(ri.journalEntries == 2);
    // Torn final write: append garbage line, recovery must stop before it and keep earlier entries.
    {
        FILE* f = std::fopen((store.projectDir(id) + "/journal.log").c_str(), "a");
        std::fputs("deadbeef {\"seq\":3,\"patch\":[{\"op\":\"rem", f);
        std::fclose(f);
    }
    json rec;
    REQUIRE(store.recover(id, rec, &ri));
    CHECK(rec["meta"]["name"] == "After Crash");
    CHECK(activeComp(rec)->at("layers").size() == 1);
    // Known-good file untouched by recovery.
    json loaded;
    CHECK(store.load(id, loaded, err));
    CHECK(loaded["meta"]["name"] == "Store");
    // Save checkpoint truncates journal; prev kept.
    CHECK(store.save(id, rec, err, true));
    CHECK(!store.checkRecovery(id).available);
    CHECK(fileExists(store.projectDir(id) + "/project.prev.json"));
    store.markClosed(id);
    CHECK(!store.checkRecovery(id).abnormalExit);
    // Corrupt current file -> falls back to previous stable.
    atomicWriteFile(store.projectDir(id) + "/project.json", "{broken");
    std::vector<std::string> log;
    CHECK(store.load(id, loaded, err, &log));
    CHECK(!log.empty());
    // Listing, duplicate, rename, versions, remove (to trash).
    auto list = store.list();
    CHECK(list.size() == 1);
    std::string dup = store.duplicate(id, "Copy", &err);
    CHECK(!dup.empty());
    CHECK(store.rename(dup, "Renamed"));
    CHECK(store.versions(id).size() >= 1);
    CHECK(store.remove(dup));
    CHECK(store.list().size() == 1);
}

TEST(storage_migration_v1_to_v2_with_backup) {
    std::string root = freshDir("migrate");
    ProjectStore store(root);
    json v1 = newProject("Old", 640, 360, 25, 4);
    v1["formatVersion"] = 1;
    OpResult r;
    v1 = applyOp(v1, {{"op", "addLayer"}, {"kind", "solid"}}, &r);
    json& L = activeComp(v1) == nullptr ? v1 : (*findCompMut(v1, "C1"))["layers"][0];
    L["transform"]["opacity"] = makeProp(0.5);
    L["transform"].erase("rotationX");
    L["hidden"] = true;
    (*findCompMut(v1, "C1"))["captions"] = json::array({{{"start", 0.0}, {"end", 1.0}, {"text", "old caption"}}});
    std::string err;
    std::string id = store.create(v1, &err);
    std::vector<std::string> log;
    json out;
    REQUIRE(store.load(id, out, err, &log));
    CHECK(out["formatVersion"] == 2);
    const json& comp = *activeComp(out);
    bool foundCap = false;
    for (auto& l : comp["layers"]) {
        if (l["type"] == "captions") foundCap = l["captions"]["items"][0]["text"] == "old caption";
        if (l["type"] == "solid") {
            CHECK_NEAR(l["transform"]["opacity"]["v"].get<double>(), 50.0, 1e-9);
            CHECK(l["transform"].contains("rotationX"));
            CHECK(l["enabled"] == false);
        }
    }
    CHECK(foundCap);
    CHECK(fileExists(store.projectDir(id) + "/project.v1.backup.json"));
    json future = newProject("Future", 640, 360, 25, 4);
    future["formatVersion"] = 99;
    bool threw = false;
    try { migrateProject(future); } catch (std::exception& e) { threw = std::string(e.what()).find("newer") != std::string::npos; }
    CHECK(threw);
}

TEST(packages_checksum_signature_encryption) {
    std::string dir = freshDir("pkg");
    std::string sk, pk;
    generateSigningKey(sk, pk);
    json preset = {{"name", "Glow Pop"}, {"effects", json::array({{{"type", "stylize.glow"}, {"enabled", true}, {"params", effectDefaultParams("stylize.glow")}}})}};
    PackageWriteOptions wo;
    wo.signingKey = sk;
    std::string err;
    std::string p1 = dir + "/glow.mffx";
    REQUIRE(exportJsonPackage(p1, "preset", preset, json::object(), wo, err));
    PackageReadOptions ro;
    ro.trustedKeys = {pk};
    json content, manifest;
    CHECK(importJsonPackage(p1, "preset", content, manifest, ro, err));
    CHECK(content["name"] == "Glow Pop");
    CHECK(manifest["_trusted"] == true);
    // Revoked signer rejected.
    ro.revokedKeys = {pk};
    CHECK(!importJsonPackage(p1, "preset", content, manifest, ro, err));
    ro.revokedKeys.clear();
    // Tamper: flip a byte in the archive -> checksum/signature failure.
    auto bytes = readFileBytes(p1);
    std::string p2 = dir + "/tampered.mffx";
    bytes[bytes.size() / 2] ^= 0x5A;
    atomicWriteFile(p2, std::string(bytes.begin(), bytes.end()));
    CHECK(!importJsonPackage(p2, "preset", content, manifest, ro, err));
    // Encryption: wrong password fails, right password works.
    PackageWriteOptions eo;
    eo.password = "correct horse";
    std::string p3 = dir + "/secret.mfcapsule";
    REQUIRE(exportJsonPackage(p3, "capsule", {{"name", "Secret"}}, json::object(), eo, err));
    CHECK(isPackageEncrypted(p3));
    PackageReadOptions bad;
    bad.password = "wrong";
    CHECK(!importJsonPackage(p3, "capsule", content, manifest, bad, err));
    CHECK(err.find("password") != std::string::npos);
    PackageReadOptions good;
    good.password = "correct horse";
    CHECK(importJsonPackage(p3, "capsule", content, manifest, good, err));
    CHECK(content["name"] == "Secret");
    // Kind mismatch.
    CHECK(!importJsonPackage(p3, "preset", content, manifest, good, err));
}

TEST(project_package_collected_roundtrip_and_missing_media) {
    std::string dir = freshDir("projpkg");
    // Media file on disk.
    Image img(64, 32);
    img.fill(Color(1, 0, 0, 1));
    std::string mediaPath = dir + "/red.png";
    REQUIRE(savePng(img, mediaPath));
    json doc = newProject("Pack", 640, 360, 30, 3);
    OpResult r;
    doc = applyOp(doc, {{"op", "addAsset"}, {"asset", {{"type", "image"}, {"name", "red.png"}, {"path", mediaPath}, {"width", 64}, {"height", 32}}}}, &r);
    doc = applyOp(doc, {{"op", "addLayer"}, {"kind", "image"}, {"options", {{"asset", r.data["asset"]}}}});
    std::string err;
    std::string pkg = dir + "/p.mforge";
    auto resolver = [](const json& a) { return a.value("path", std::string()); };
    REQUIRE(exportProjectPackage(doc, pkg, ProjectPackMode::Collected, resolver, nullptr, PackageWriteOptions(), err));
    json back;
    std::vector<std::string> warn;
    REQUIRE(importProjectPackage(pkg, dir + "/imported", back, warn, PackageReadOptions(), err));
    std::string newPath = back["assets"][0]["path"];
    CHECK(newPath.find("/imported/") != std::string::npos);
    CHECK(fileExists(newPath));
    CHECK(back["assets"][0]["checksum"].get<std::string>().rfind("blake2b:", 0) == 0);
    // Linked package with missing media -> flagged missing, never substituted.
    json doc2 = doc;
    doc2["assets"][0]["path"] = dir + "/does-not-exist.png";
    std::string pkg2 = dir + "/linked.mforge";
    REQUIRE(exportProjectPackage(doc2, pkg2, ProjectPackMode::Linked, resolver, nullptr, PackageWriteOptions(), err));
    warn.clear();
    REQUIRE(importProjectPackage(pkg2, dir + "/imported2", back, warn, PackageReadOptions(), err));
    CHECK(back["assets"][0].value("missing", false));
    CHECK(!warn.empty());
}

TEST(scripting_api_permissions_errors_single_transaction) {
    ScriptRequest req;
    req.project = newProject("S", 1920, 1080, 30, 10);
    req.compId = "C1";
    req.permissions = {"PROJECT_READ", "TIMELINE_WRITE", "LOCAL_STORAGE"};
    req.source = "var id = mf.layer.add('text', {text:'Scripted'});\n"
                 "mf.keyframe.add(id, 'transform.position', 0, [100, 100, 0]);\n"
                 "mf.keyframe.add(id, 'transform.position', 1, [500, 100, 0], 'easeOut');\n"
                 "var p = [1,2] + [3,4];\n"
                 "mf.storage.set('count', 1);\n"
                 "mf.log('pos', p);\n"
                 "return mf.layer.find('scripted').length;";
    ScriptResult r = runScript(req);
    CHECK(r.ok);
    CHECK(r.changed);
    CHECK(r.returnValue == 1);
    CHECK(r.storage["count"] == 1);
    REQUIRE(!r.logs.empty());
    CHECK(r.logs[0].find("[4,6]") != std::string::npos);
    CHECK(activeComp(r.project)->at("layers").size() == 1);
    // Missing permission.
    ScriptRequest ro = req;
    ro.permissions = {"PROJECT_READ"};
    ScriptResult r2 = runScript(ro);
    CHECK(!r2.ok);
    CHECK(r2.error.find("PROJECT_WRITE") != std::string::npos);
    CHECK(!r2.changed);
    // Error with line number + suggestion; project untouched on failure.
    ScriptRequest bad = req;
    bad.source = "var x = 1;\nvar l = mf.layer.add('solid');\nvar o = mf.layer.get(l).opacity.value;";
    ScriptResult r3 = runScript(bad);
    CHECK(!r3.ok);
    CHECK(r3.line == 3);
    CHECK(r3.error.find("Line 3") != std::string::npos);
    CHECK(r3.error.find("Suggestion") != std::string::npos);
    // Timeout.
    ScriptRequest loop = req;
    loop.source = "while(true){}";
    loop.timeoutSec = 0.3;
    ScriptResult r4 = runScript(loop);
    CHECK(!r4.ok);
    CHECK(r4.error.find("time limit") != std::string::npos);
    // No filesystem / network access exists in the sandbox.
    ScriptRequest sb = req;
    sb.source = "return [typeof require, typeof fetch, typeof XMLHttpRequest, typeof std, typeof os].join(',');";
    ScriptResult r5 = runScript(sb);
    CHECK(r5.ok);
    CHECK(r5.returnValue == "undefined,undefined,undefined,undefined,undefined");
    int line;
    std::string err;
    CHECK(!validateScriptSyntax("var a = ;", err, line));
    CHECK(scriptApiDescription()["layer"].size() > 5);
    for (auto& ex : exampleScripts()) {
        ScriptRequest e = req;
        e.source = ex["source"];
        e.permissions = ex["permissions"].get<std::vector<std::string>>();
        e.permissions.push_back("PROJECT_READ");
        e.selection = {};
        ScriptResult er = runScript(e);
        if (!er.ok) std::printf("    example '%s': %s\n", ex["name"].get<std::string>().c_str(), er.error.c_str());
        CHECK(er.ok);
    }
}

TEST(subtitles_parse_export_roundtrip) {
    std::string srt = "1\n00:00:01,000 --> 00:00:02,500\nHello <i>world</i>\n\n2\n00:00:03,000 --> 00:00:04,000\nSecond\nline\n\n";
    std::string err;
    auto a = parseSrt(srt, &err);
    REQUIRE(a.size() == 2);
    CHECK(a[0].text == "Hello world");
    CHECK_NEAR(a[0].end, 2.5, 1e-9);
    CHECK(a[1].text == "Second\nline");
    auto b = parseVtt(toVtt(a), &err);
    REQUIRE(b.size() == 2);
    CHECK_NEAR(b[1].start, 3.0, 1e-9);
    auto c = parseAss(toAss(a, defaultCaptionStyle(), 1920, 1080), &err);
    REQUIRE(c.size() == 2);
    CHECK(c[1].text == "Second\nline");
    CHECK(parseSubtitles("WEBVTT\n\n00:01.000 --> 00:02.000 align:start\nX\n", "", &err).size() == 1);
    CHECK(formatTimestamp(3661.5, ',') == "01:01:01,500");
}

TEST(caption_segmentation_and_reading_speed) {
    std::vector<CaptionWord> w;
    const char* words[] = {"And", "so,", "my", "fellow", "Americans,", "ask", "not", "what", "your", "country", "can", "do", "for", "you.",
                           "Ask", "what", "you", "can", "do", "for", "your", "country."};
    double t = 0;
    for (auto* x : words) { w.push_back({t, t + 0.3, x, 0.9f}); t += 0.35; }
    SegmentOptions so;
    so.maxChars = 32;
    auto caps = segmentWords(w, so);
    CHECK(caps.size() >= 2);
    for (auto& c : caps) {
        size_t nl = c.text.find('\n');
        CHECK(nl == std::string::npos || (nl <= 40 && c.text.size() - nl <= 40));
        CHECK(c.end > c.start);
    }
    CHECK(caps[0].text.find("you.") != std::string::npos || caps.size() > 2);
    std::vector<CaptionItem> fast = {{0, 0.5, "This caption has far too many characters for half a second"}};
    auto issues = validateReadingSpeed(fast, 20);
    CHECK(issues.size() == 1);
    CHECK_NEAR(wordErrorRate("ask not what your country can do", "Ask not, what your country can do!"), 0.0, 1e-9);
    CHECK_NEAR(wordErrorRate("a b c d", "a x c"), 0.5, 1e-9);
}

TEST(gif_encoder_valid_file) {
    std::string dir = freshDir("gif");
    GifWriter g;
    REQUIRE(g.open(dir + "/a.gif", 64, 48));
    for (int f = 0; f < 5; ++f) {
        Image img(64, 48);
        for (int y = 0; y < 48; ++y)
            for (int x = 0; x < 64; ++x) {
                uint8_t* p = img.at(x, y);
                p[0] = (uint8_t)(x * 4); p[1] = (uint8_t)(y * 5); p[2] = (uint8_t)(f * 50); p[3] = 255;
            }
        CHECK(g.addFrame(img, 10));
    }
    CHECK(g.close());
    int w, h, n;
    CHECK(inspectGif(dir + "/a.gif", w, h, n));
    CHECK(w == 64 && h == 48 && n == 5);
}

TEST(tracking_follows_moving_square_and_stabilizes) {
    // Synthetic frames: textured square moving 3px/frame right, 1px/frame down.
    auto frame = [](double t) {
        auto img = std::make_shared<Image>(200, 120);
        int f = (int)std::lround(t * 30);
        for (int y = 0; y < 120; ++y)
            for (int x = 0; x < 200; ++x) {
                uint8_t* p = img->at(x, y);
                uint8_t v = (uint8_t)(hash32((uint32_t)(x * 7919 + y * 104729)) % 60);
                p[0] = p[1] = p[2] = v; p[3] = 255;
            }
        int sx = 40 + 3 * f, sy = 40 + f;
        for (int y = 0; y < 20; ++y)
            for (int x = 0; x < 20; ++x) {
                uint8_t* p = img->at(sx + x, sy + y);
                uint8_t v = ((x / 5 + y / 5) % 2) ? 255 : 120;
                p[0] = p[1] = p[2] = v;
            }
        return ImagePtr(img);
    };
    TrackOptions opt;
    opt.patch = 21;
    opt.search = 16;
    std::string msg;
    auto tr = trackPoint(frame, 0, 1.0, 1 / 30.0, {50, 50}, opt, nullptr, &msg);
    REQUIRE(tr.size() == 31);
    CHECK_NEAR(tr.back().p.x, 50 + 90, 1.0);
    CHECK_NEAR(tr.back().p.y, 50 + 30, 1.0);
    // Shaky camera: whole frame jitters; stabilization offsets cancel it.
    auto shaky = [&](double t) {
        int f = (int)std::lround(t * 30);
        int dx = (f % 2) ? 3 : -3;
        auto src = frame(0);
        auto img = std::make_shared<Image>(200, 120);
        for (int y = 0; y < 120; ++y)
            for (int x = 0; x < 200; ++x) std::memcpy(img->at(x, y), src->at(clampv(x - dx, 0, 199), y), 4);
        return ImagePtr(img);
    };
    auto st = stabilize(shaky, 0, 0.5, 1 / 30.0, 0.3, false, 200, 120);
    REQUIRE(st.size() >= 10);
    CHECK(std::fabs(st[5].offset.x) > 1.5);
    CHECK((st[5].offset.x > 0) != (st[6].offset.x > 0));
}

TEST(audio_mix_volume_pan_mute_fade_and_analysis) {
    FileMediaProvider media;
    AudioEngine ae(&media);
    json doc = newProject("A", 640, 360, 30, 2);
    OpResult r;
    doc = applyOp(doc, {{"op", "addAsset"}, {"asset", {{"type", "audio"}, {"name", "tone"}, {"toneHz", 440.0}, {"duration", 2.0}, {"hasAudio", true}, {"path", "tone:440"}}}}, &r);
    doc = applyOp(doc, {{"op", "addLayer"}, {"kind", "audio"}, {"options", {{"asset", r.data["asset"]}}}}, &r);
    std::string id = r.data["layer"];
    const json& comp = *activeComp(doc);
    std::vector<float> out(4800 * 2);
    float peak = ae.mix(doc, comp, 0.5, 4800, out.data());
    CHECK_NEAR(peak, 0.5, 0.02);
    json d2 = applyOp(doc, {{"op", "setProp"}, {"layer", id}, {"path", "audio.volume"}, {"value", -6.0}});
    ae.resetState();
    peak = ae.mix(d2, *activeComp(d2), 0.5, 4800, out.data());
    CHECK_NEAR(peak, 0.5 * dbToGain(-6), 0.02);
    json d3 = applyOp(doc, {{"op", "setProp"}, {"layer", id}, {"path", "audio.pan"}, {"value", -100.0}});
    ae.mix(d3, *activeComp(d3), 0.5, 4800, out.data());
    float l = 0, rr = 0;
    for (int i = 0; i < 4800; ++i) { l = std::max(l, std::fabs(out[i * 2])); rr = std::max(rr, std::fabs(out[i * 2 + 1])); }
    CHECK(l > 0.6 && rr < 0.01);
    json d4 = applyOp(doc, {{"op", "setLayer"}, {"layer", id}, {"fields", {{"muted", true}}}});
    CHECK(ae.mix(d4, *activeComp(d4), 0.5, 4800, out.data()) == 0.f);
    auto A = ae.analyze(*findAsset(doc, (*findLayer(comp, id))["asset"]));
    CHECK(!A->rms.empty());
    auto spec = ae.spectrum(doc, comp, 0.5, 32);
    int peakBand = (int)(std::max_element(spec.begin(), spec.end()) - spec.begin());
    CHECK(peakBand > 6 && peakBand < 16);  // 440 Hz in log-spaced bands from 40 Hz
    std::vector<float> re(8, 0.f), im;
    re[1] = 1;
    fftReal(re, im);
    CHECK_NEAR(std::sqrt(re[1] * re[1] + im[1] * im[1]), 1.0, 1e-5);
    // WAV roundtrip.
    std::string wav = mftest::tmpDir() + "/t.wav";
    CHECK(saveWav(wav, out.data(), 4800, 2, 48000));
    AudioBuffer b;
    CHECK(loadWav(wav, b));
    CHECK(b.frames() == 4800);
}

TEST(engine_command_layer_gesture_autosave_reopen) {
    std::string dir = freshDir("engine");
    FileMediaProvider media;
    EngineConfig cfg;
    cfg.dataDir = dir;
    cfg.cacheDir = dir + "/cache";
    cfg.autosaveInterval = 0;
    Engine E(cfg, &media);
    std::string err;
    std::string id = E.createProject("E", 1280, 720, 30, 5, &err);
    REQUIRE(E.openProject(id, err));
    OpOutcome o = E.apply({{"op", "addLayer"}, {"kind", "solid"}});
    REQUIRE(o.ok);
    std::string lid = o.data["layer"];
    // Drag gesture: many previews -> one undo step.
    for (int i = 0; i < 10; ++i) E.previewOp({{"op", "setProp"}, {"layer", lid}, {"path", "transform.position"}, {"value", {100.0 + i * 10, 200.0, 0.0}}});
    CHECK(E.commitPreview("Move Layer"));
    CHECK(E.doc().history().size() == 2);
    CHECK(E.stateJson()["undoLabel"] == "Move Layer");
    OpOutcome bad = E.apply({{"op", "removeLayers"}, {"layers", {"nope"}}});
    CHECK(bad.ok);  // removing unknown id is a no-op (nothing matched)
    OpOutcome bad2 = E.apply({{"op", "addEffect"}, {"layer", lid}, {"type", "nope"}});
    CHECK(!bad2.ok);
    CHECK(bad2.error.find("Unknown effect") != std::string::npos);
    CHECK(E.autosaveTick(nowSeconds() + 1));
    CHECK(E.saveStatus() == "saved");
    E.closeProject();
    REQUIRE(E.openProject(id, err));
    const json* L = findLayer(*activeComp(*E.snapshot()), lid);
    REQUIRE(L);
    CHECK((*L)["transform"]["position"]["v"][0].get<double>() == 190.0);
    Image img = E.render(0, 0.25);
    CHECK(img.w == 320 && img.h == 180);
    CHECK(!E.layerQuad(lid, 0).is_null());
}

TEST(scenario_prd_example_end_to_end) {
    std::string dir = freshDir("scenario");
    FileMediaProvider media;
    EngineConfig cfg;
    cfg.dataDir = dir + "/data";
    Engine E(cfg, &media);
    std::string script = R"(
CREATE project 1280x720 30fps duration 3
IMPORT video generator:counter duration 3
ADD text "Hi"
SET text = "Hello"
ADD keyframe position t=0
SET position = [200,500]
ADD keyframe position t=1s
SET position = [1100,500]
APPLY glow
TIME 0.5
EXPECT position == [650,500]
EXPECT text == "Hello"
SPLIT 1.5
EXPECT layers == 3
UNDO
EXPECT layers == 2
REDO
EXPECT layers == 3
SAVE
REOPEN
EXPORT gif out.gif fps 10 scale 0.25
VALIDATE
EXPORT wav out.wav
RENDER frame.png 0.5
)";
    ScenarioResult r = runScenario(E, script, dir);
    if (!r.ok) std::printf("    %s\n", r.error.c_str());
    CHECK(r.ok);
    CHECK(fileExists(dir + "/out.gif"));
    CHECK(fileExists(dir + "/frame.png"));
}

TEST(whisper_offline_captions_jfk) {
    if (!asrAvailable()) { std::printf("    (skipped: built without whisper)\n"); return; }
    std::string model = mftest::sourceRoot() + "/.cache/models/ggml-tiny.en-q5_1.bin";
    if (!fileExists(model)) { std::printf("    (skipped: model not downloaded)\n"); return; }
    AudioBuffer buf;
    REQUIRE(loadWav(mftest::sourceRoot() + "/third_party/whisper.cpp/samples/jfk.wav", buf));
    AsrOptions opt;
    opt.modelPath = model;
    opt.language = "en";
    int progressCalls = 0;
    opt.progress = [&](float) { ++progressCalls; return true; };
    AsrResult r = transcribe(resampleMono(buf, 16000), opt);
    REQUIRE(r.ok);
    std::string text;
    for (auto& c : r.captions) text += c.text + " ";
    for (auto& c : text) if (c == '\n') c = ' ';
    double wer = wordErrorRate("And so my fellow Americans ask not what your country can do for you ask what you can do for your country", text);
    std::printf("    transcript: %s\n    WER=%.3f words=%zu captions=%zu %.1fs audio in %.1fs\n", text.c_str(), wer, r.words.size(), r.captions.size(), r.audioSeconds, r.processingSeconds);
    CHECK(wer < 0.15);
    CHECK(r.words.size() >= 18);
    CHECK(progressCalls > 0);
    // Word timing tolerances: "country" (last word) ends near 10.4 s; first word starts before 1 s.
    CHECK(r.words.front().s < 1.0);
    CHECK_NEAR(r.words.back().e, 10.5, 1.0);
    for (size_t i = 1; i < r.words.size(); ++i) CHECK(r.words[i].s >= r.words[i - 1].s);
    // Cancellation works.
    AsrOptions c = opt;
    c.progress = [](float) { return false; };
    AsrResult rc = transcribe(resampleMono(buf, 16000), c);
    CHECK(!rc.ok && rc.cancelled);
    // Bad model file reports an actionable error.
    AsrOptions bad = opt;
    bad.modelPath = mftest::sourceRoot() + "/README.md";
    AsrResult rb = transcribe(resampleMono(buf, 16000), bad);
    CHECK(!rb.ok && rb.error.find("model") != std::string::npos);
}
