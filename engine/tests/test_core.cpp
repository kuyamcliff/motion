// Core: easing, keyframes, properties, document/undo, edit ops, timecode-ish helpers.
#include "mf/document.hpp"
#include "mf/easing.hpp"
#include "mf/effects.hpp"
#include "mf/model.hpp"
#include "mf/property.hpp"
#include "mftest.hpp"

using namespace mf;

TEST(easing_endpoints_and_monotonic) {
    for (auto& n : easingPresetNames()) {
        Interp i = Interp::fromName(n);
        if (i.type == EaseType::Hold) {
            CHECK_NEAR(i.apply(0.5), 0.0, 1e-9);
            CHECK_NEAR(i.apply(1.0), 1.0, 1e-9);
            continue;
        }
        CHECK_NEAR(i.apply(0.0), 0.0, 1e-6);
        CHECK_NEAR(i.apply(1.0), 1.0, 1e-6);
    }
    Interp lin = Interp::linear();
    CHECK_NEAR(lin.apply(0.25), 0.25, 1e-12);
    Interp eio = Interp::easeInOut();
    CHECK_NEAR(eio.apply(0.5), 0.5, 1e-3);
    CHECK(eio.apply(0.1) < 0.1);
    CHECK(Interp::fromName("backOut").apply(0.6) > 1.0);  // overshoot
}

TEST(cubic_bezier_matches_css_ease) {
    // CSS "ease" = cubic-bezier(.25,.1,.25,1); at x=0.5 y~0.8024
    CHECK_NEAR(cubicBezierEase(0.25, 0.1, 0.25, 1.0, 0.5), 0.8024, 2e-3);
    CHECK_NEAR(cubicBezierEase(0, 0, 1, 1, 0.37), 0.37, 1e-6);
}

TEST(interp_json_roundtrip) {
    Interp b = Interp::bezier(0.1, 0.7, 0.3, 1.2);
    Interp r = Interp::fromJson(b.toJson());
    CHECK(r.type == EaseType::Bezier);
    CHECK_NEAR(r.p[1], 0.7, 1e-12);
    CHECK_NEAR(r.p[3], 1.2, 1e-12);
    CHECK(Interp::fromJson(json("bounce")).type == EaseType::Bounce);
}

TEST(keyframe_evaluation_scalar_vector_hold) {
    json p = makeProp(0.0);
    p = propSetKeyframe(p, 0.0, 0.0);
    p = propSetKeyframe(p, 1.0, 100.0);
    CHECK_NEAR(evalRaw(p, 0.5).num(), 50.0, 1e-9);
    CHECK_NEAR(evalRaw(p, -1).num(), 0.0, 1e-9);
    CHECK_NEAR(evalRaw(p, 3).num(), 100.0, 1e-9);
    json v = makeProp({0.0, 0.0});
    v = propSetKeyframe(v, 0, {0.0, 10.0});
    v = propSetKeyframe(v, 2, {200.0, 30.0});
    Value m = evalRaw(v, 1);
    CHECK_NEAR(m.n[0], 100, 1e-9);
    CHECK_NEAR(m.n[1], 20, 1e-9);
    json h = propSetKeyframe(makeProp(0.0), 0, 5.0, "hold");
    h = propSetKeyframe(h, 1, 9.0);
    CHECK_NEAR(evalRaw(h, 0.99).num(), 5.0, 1e-9);
    CHECK_NEAR(evalRaw(h, 1.0).num(), 9.0, 1e-9);
}

TEST(keyframe_ops_reverse_scale_distribute_clipboard) {
    json p = makeProp(0.0);
    p = propSetKeyframe(p, 0, 0.0);
    p = propSetKeyframe(p, 1, 10.0);
    p = propSetKeyframe(p, 4, 40.0);
    json r = propReverseKeyframes(p);
    CHECK_NEAR(evalRaw(r, 0).num(), 40, 1e-9);
    CHECK_NEAR(evalRaw(r, 4).num(), 0, 1e-9);
    json d = propDistributeKeyframes(p);
    CHECK_NEAR(d["k"][1]["t"].get<double>(), 2.0, 1e-9);
    json s = propScaleKeyframeTimes(p, 0, 2);
    CHECK_NEAR(s["k"][2]["t"].get<double>(), 8.0, 1e-9);
    // Copy 1.2s of animation and paste over 2.4s (time stretch).
    json a = propSetKeyframe(propSetKeyframe(makeProp({0.0, 0.0}), 0, {0.0, 0.0}), 1.2, {120.0, 0.0});
    json clip = keyframesClipboard(a, 0, 1.2);
    json b = pasteKeyframes(makeProp({0.0, 0.0}), clip, 5.0, 7.4);
    CHECK_NEAR(b["k"][1]["t"].get<double>(), 7.4, 1e-9);
    CHECK_NEAR(evalRaw(b, 6.2).n[0], 60.0, 1e-6);
    json rm = propRemoveKeyframe(p, 1);
    CHECK(rm["k"].size() == 2);
}

TEST(document_undo_redo_branch) {
    Document d(newProject("T", 1920, 1080, 30, 10));
    json doc = d.doc();
    OpResult r;
    doc = applyOp(doc, {{"op", "addLayer"}, {"kind", "solid"}}, &r);
    d.commit(r.label, doc);
    std::string id = r.data["layer"];
    json moved = applyOp(d.doc(), {{"op", "setProp"}, {"layer", id}, {"path", "transform.position"}, {"value", {100.0, 200.0, 0.0}}, {"t", 0.0}});
    d.commit("Move Layer", moved);
    auto pos = [&]() { return (*resolvePath(*findLayer(*activeComp(d.doc()), id), "transform.position"))["v"]; };
    CHECK(pos()[0].get<double>() == 100.0);
    CHECK(d.undo());
    CHECK(pos()[0].get<double>() == 960.0);  // exact previous position
    CHECK(d.redo());
    CHECK(pos()[0].get<double>() == 100.0);
    CHECK(d.undo());
    // Branch: new edit discards only the redo future.
    json other = applyOp(d.doc(), {{"op", "setLayer"}, {"layer", id}, {"fields", {{"name", "Renamed"}}}});
    d.commit("Rename", other);
    CHECK(!d.canRedo());
    CHECK(d.history().size() == 2);
    CHECK(d.undo());
    CHECK(d.undo());
    CHECK(!d.canUndo());
    CHECK(activeComp(d.doc())->at("layers").empty());
    CHECK(d.jumpTo(2));
    CHECK(findLayer(*activeComp(d.doc()), id)->value("name", "") == "Renamed");
    // stateAt preview without committing.
    json s0 = d.stateAt(0);
    CHECK(activeComp(s0)->at("layers").empty());
    CHECK(d.historyIndex() == 2);
}

TEST(document_preview_commit_cancel) {
    Document d(newProject("T", 640, 360, 30, 5));
    json a = applyOp(d.doc(), {{"op", "addLayer"}, {"kind", "null"}});
    d.preview(a);
    CHECK(d.inPreview());
    d.cancelPreview();
    CHECK(activeComp(d.doc())->at("layers").empty());
    d.preview(a);
    CHECK(d.commitPreview("Add Null"));
    CHECK(d.history().size() == 1);
    CHECK(d.undoLabel() == "Add Null");
}

static json projectWithVideo(std::string& layerId, double dur = 6.0) {
    json doc = newProject("V", 1280, 720, 30, 10);
    OpResult r;
    doc = applyOp(doc, {{"op", "addAsset"}, {"asset", {{"type", "video"}, {"name", "clip.mp4"}, {"width", 1280}, {"height", 720}, {"fps", 30}, {"duration", dur}, {"hasAudio", true}, {"generator", "counter"}}}}, &r);
    std::string aid = r.data["asset"];
    doc = applyOp(doc, {{"op", "addLayer"}, {"kind", "video"}, {"options", {{"asset", aid}}}}, &r);
    layerId = r.data["layer"];
    return doc;
}

TEST(split_continuity_no_dup_no_loss) {
    std::string id;
    json doc = projectWithVideo(id);
    OpResult r;
    double fps = 30;
    double t = 45 / fps;  // frame 45
    json d2 = applyOp(doc, {{"op", "split"}, {"layers", {id}}, {"t", t}}, &r);
    const json& comp = *activeComp(d2);
    REQUIRE(comp["layers"].size() == 2);
    const json* A = findLayer(comp, id);
    const json* B = findLayer(comp, r.data["layers"][0].get<std::string>());
    REQUIRE(A && B);
    CHECK_NEAR(A->value("out", 0.0), t, 1e-12);
    CHECK_NEAR(B->value("in", 0.0), t, 1e-12);
    CHECK_NEAR(A->value("in", 0.0), 0.0, 1e-12);
    CHECK_NEAR(B->value("out", 0.0), 6.0, 1e-12);
    // Every frame maps to exactly one layer and to the same source frame as before.
    for (int f = 0; f < 180; ++f) {
        double ct = f / fps;
        int active = (layerActiveAt(*A, ct) ? 1 : 0) + (layerActiveAt(*B, ct) ? 1 : 0);
        CHECK(active == 1);
        const json* L = layerActiveAt(*A, ct) ? A : B;
        CHECK_NEAR(layerSourceTime(*L, ct), ct, 1e-9);
    }
    // Audio sync: source time continuous across the cut.
    CHECK_NEAR(layerSourceTime(*A, t - 1e-6), layerSourceTime(*B, t), 1e-5);
    // Split outside the clip is rejected with a clear message.
    bool threw = false;
    try { applyOp(doc, {{"op", "split"}, {"layers", {id}}, {"t", 8.0}}); } catch (EditError& e) { threw = std::string(e.what()).find("playhead") != std::string::npos; }
    CHECK(threw);
}

TEST(trim_speed_reverse_freeze_ripple) {
    std::string id;
    json doc = projectWithVideo(id);
    doc = applyOp(doc, {{"op", "trimLayer"}, {"layer", id}, {"edge", "in"}, {"t", 1.0}});
    doc = applyOp(doc, {{"op", "trimLayer"}, {"layer", id}, {"edge", "out"}, {"t", 99.0}});  // clamped to media length
    const json* L = findLayer(*activeComp(doc), id);
    CHECK_NEAR(L->value("in", 0.0), 1.0, 1e-9);
    CHECK_NEAR(L->value("out", 0.0), 6.0, 1e-9);
    json sp = applyOp(doc, {{"op", "setSpeed"}, {"layer", id}, {"speed", 2.0}});
    L = findLayer(*activeComp(sp), id);
    CHECK_NEAR(L->value("in", 0.0), 1.0, 1e-9);
    CHECK_NEAR(L->value("out", 0.0), 3.5, 1e-9);  // 5s of source at 2x
    CHECK_NEAR(layerSourceTime(*L, 1.0), 1.0, 1e-9);
    CHECK_NEAR(layerSourceTime(*L, 3.5), 6.0, 1e-9);
    json rv = applyOp(doc, {{"op", "reverse"}, {"layer", id}, {"on", true}});
    L = findLayer(*activeComp(rv), id);
    CHECK_NEAR(layerSourceTime(*L, 1.0), 6.0, 1e-4);
    CHECK_NEAR(layerSourceTime(*L, 5.9), 1.1, 1e-4);
    OpResult r;
    json fz = applyOp(doc, {{"op", "freezeFrame"}, {"layer", id}, {"t", 2.0}, {"duration", 1.5}}, &r);
    const json* F = findLayer(*activeComp(fz), r.data["layer"]);
    REQUIRE(F);
    CHECK_NEAR(layerSourceTime(*F, 2.7), 2.0, 1e-9);
    CHECK_NEAR(activeComp(fz)->value("duration", 0.0), 11.5, 1e-9);
    // Ripple delete: later clip moves left.
    json two = applyOp(doc, {{"op", "split"}, {"layers", {id}}, {"t", 3.0}}, &r);
    std::string second = r.data["layers"][0];
    json rd = applyOp(two, {{"op", "rippleDelete"}, {"layer", id}});
    const json* S = findLayer(*activeComp(rd), second);
    REQUIRE(S);
    CHECK_NEAR(S->value("in", 0.0), 1.0, 1e-9);
}

TEST(speed_ramp_preserves_source_range) {
    std::string id;
    json doc = projectWithVideo(id);
    for (std::string p : {"cinematic", "impact", "montage", "acceleration", "deceleration", "snap"}) {
        json d = applyOp(doc, {{"op", "speedRampPreset"}, {"layer", id}, {"preset", p}});
        const json* L = findLayer(*activeComp(d), id);
        CHECK_NEAR(layerSourceTime(*L, 0.0), 0.0, 1e-6);
        CHECK_NEAR(layerSourceTime(*L, 6.0), 6.0, 1e-6);
    }
}

TEST(precompose_and_validate) {
    json doc = newProject("P", 1920, 1080, 30, 10);
    OpResult r1, r2, r3;
    doc = applyOp(doc, {{"op", "addLayer"}, {"kind", "solid"}}, &r1);
    doc = applyOp(doc, {{"op", "addLayer"}, {"kind", "text"}, {"options", {{"text", "Hi"}}}}, &r2);
    doc = applyOp(doc, {{"op", "precompose"}, {"layers", {r1.data["layer"], r2.data["layer"]}}, {"name", "Group"}}, &r3);
    CHECK(doc["comps"].size() == 2);
    const json& main = *activeComp(doc);
    CHECK(main["layers"].size() == 1);
    CHECK(main["layers"][0]["type"] == "precomp");
    CHECK(validateProject(doc).empty());
    bool threw = false;
    try { applyOp(doc, {{"op", "removeComp"}, {"comp", r3.data["comp"]}}); } catch (EditError&) { threw = true; }
    CHECK(threw);
}

TEST(parenting_cycle_rejected) {
    json doc = newProject("P", 1920, 1080, 30, 10);
    OpResult a, b;
    doc = applyOp(doc, {{"op", "addLayer"}, {"kind", "null"}}, &a);
    doc = applyOp(doc, {{"op", "addLayer"}, {"kind", "null"}}, &b);
    doc = applyOp(doc, {{"op", "setLayer"}, {"layer", b.data["layer"]}, {"fields", {{"parent", a.data["layer"]}}}});
    bool threw = false;
    try { applyOp(doc, {{"op", "setLayer"}, {"layer", a.data["layer"]}, {"fields", {{"parent", b.data["layer"]}}}}); } catch (EditError&) { threw = true; }
    CHECK(threw);
}

TEST(locked_layer_rejects_edits) {
    json doc = newProject("P", 1920, 1080, 30, 10);
    OpResult a;
    doc = applyOp(doc, {{"op", "addLayer"}, {"kind", "solid"}}, &a);
    std::string id = a.data["layer"];
    doc = applyOp(doc, {{"op", "setLayer"}, {"layer", id}, {"fields", {{"locked", true}}}});
    bool threw = false;
    try { applyOp(doc, {{"op", "removeLayers"}, {"layers", {id}}}); } catch (EditError& e) { threw = std::string(e.what()).find("locked") != std::string::npos; }
    CHECK(threw);
    doc = applyOp(doc, {{"op", "setLayer"}, {"layer", id}, {"fields", {{"locked", false}}}});
    doc = applyOp(doc, {{"op", "removeLayers"}, {"layers", {id}}});
    CHECK(activeComp(doc)->at("layers").empty());
}

TEST(keyframe_ops_through_dispatcher) {
    json doc = newProject("K", 1920, 1080, 30, 10);
    OpResult a;
    doc = applyOp(doc, {{"op", "addLayer"}, {"kind", "text"}, {"options", {{"text", "Hello"}}}}, &a);
    std::string id = a.data["layer"];
    doc = applyOp(doc, {{"op", "addKeyframe"}, {"layer", id}, {"path", "transform.position"}, {"t", 0.0}, {"value", {200.0, 500.0, 0.0}}});
    doc = applyOp(doc, {{"op", "addKeyframe"}, {"layer", id}, {"path", "transform.position"}, {"t", 1.0}, {"value", {1500.0, 500.0, 0.0}}});
    const json& P = *resolvePath(*findLayer(*activeComp(doc), id), "transform.position");
    CHECK_NEAR(evalRaw(P, 0.5).n[0], 850, 1e-9);
    doc = applyOp(doc, {{"op", "setKeyframeInterp"}, {"layer", id}, {"path", "transform.position"}, {"t", 0.0}, {"interp", "easeInOut"}});
    const json& P2 = *resolvePath(*findLayer(*activeComp(doc), id), "transform.position");
    CHECK_NEAR(evalRaw(P2, 0.5).n[0], 850, 2);
    CHECK(evalRaw(P2, 0.2).n[0] < 200 + 1300 * 0.2);
    doc = applyOp(doc, {{"op", "moveKeyframe"}, {"layer", id}, {"path", "transform.position"}, {"from", 1.0}, {"to", 2.0}});
    const json& P3 = *resolvePath(*findLayer(*activeComp(doc), id), "transform.position");
    CHECK_NEAR(P3["k"][1]["t"].get<double>(), 2.0, 1e-9);
    auto kt = keyframeTimes(*activeComp(doc), nullptr);
    CHECK(kt.size() == 2);
    // Type mismatch rejected.
    bool threw = false;
    try { applyOp(doc, {{"op", "setProp"}, {"layer", id}, {"path", "transform.opacity"}, {"value", "abc"}}); } catch (EditError&) { threw = true; }
    CHECK(threw);
}

TEST(captions_ops_split_merge_replace_editbytranscript) {
    std::string vid;
    json doc = projectWithVideo(vid);
    json w1 = {{"s", 0.5}, {"e", 0.9}, {"w", "hello"}};
    json w2 = {{"s", 1.0}, {"e", 1.4}, {"w", "world"}};
    json i1 = {{"start", 0.5}, {"end", 1.5}, {"text", "hello world"}, {"words", json::array({w1, w2})}};
    json i2 = {{"start", 2.0}, {"end", 3.0}, {"text", "second line"}};
    json i3 = {{"start", 4.0}, {"end", 5.0}, {"text", "third"}};
    json items = json::array({i1, i2, i3});
    OpResult r;
    doc = applyOp(doc, {{"op", "setCaptions"}, {"items", items}}, &r);
    const json* comp = activeComp(doc);
    const json* cl = nullptr;
    for (auto& L : (*comp)["layers"]) if (L["type"] == "captions") cl = &L;
    REQUIRE(cl);
    std::string c1 = (*cl)["captions"]["items"][0]["id"], c2 = (*cl)["captions"]["items"][1]["id"];
    json s = applyOp(doc, {{"op", "splitCaption"}, {"caption", c1}, {"t", 0.95}});
    for (auto& L : (*activeComp(s))["layers"]) if (L["type"] == "captions") {
        CHECK(L["captions"]["items"].size() == 4);
        CHECK(L["captions"]["items"][0]["text"] == "hello");
        CHECK(L["captions"]["items"][1]["text"] == "world");
    }
    json m = applyOp(doc, {{"op", "mergeCaptions"}, {"first", c1}, {"second", c2}});
    for (auto& L : (*activeComp(m))["layers"]) if (L["type"] == "captions") CHECK(L["captions"]["items"].size() == 2);
    OpResult rr;
    json rep = applyOp(doc, {{"op", "captionReplace"}, {"find", "line"}, {"replace", "row"}}, &rr);
    CHECK(rr.data["count"] == 1);
    // Edit by transcript: remove [2,3) ripples later content by 1s.
    json eb = applyOp(doc, {{"op", "editByCaption"}, {"captions", {c2}}});
    const json& ec = *activeComp(eb);
    CHECK_NEAR(ec.value("duration", 0.0), 9.0, 1e-9);
    int vids = 0;
    for (auto& L : ec["layers"]) if (L["type"] == "video") ++vids;
    CHECK(vids == 2);
    for (auto& L : ec["layers"]) if (L["type"] == "captions") {
        CHECK(L["captions"]["items"].size() == 2);
        CHECK_NEAR(L["captions"]["items"][1]["start"].get<double>(), 3.0, 1e-9);
    }
}

TEST(effects_masks_behaviors_transitions_ops) {
    json doc = newProject("E", 1920, 1080, 30, 10);
    OpResult a, e, m, b;
    doc = applyOp(doc, {{"op", "addLayer"}, {"kind", "solid"}}, &a);
    std::string id = a.data["layer"];
    doc = applyOp(doc, {{"op", "addEffect"}, {"layer", id}, {"type", "blur.gaussian"}}, &e);
    doc = applyOp(doc, {{"op", "addEffect"}, {"layer", id}, {"type", "stylize.glow"}});
    doc = applyOp(doc, {{"op", "moveEffect"}, {"layer", id}, {"effect", e.data["effect"]}, {"index", 1}});
    const json* L = findLayer(*activeComp(doc), id);
    CHECK((*L)["effects"][1]["type"] == "blur.gaussian");
    doc = applyOp(doc, {{"op", "addMask"}, {"layer", id}, {"shape", "ellipse"}}, &m);
    doc = applyOp(doc, {{"op", "setMask"}, {"layer", id}, {"mask", m.data["mask"]}, {"fields", {{"mode", "subtract"}, {"inverted", true}}}});
    doc = applyOp(doc, {{"op", "addBehavior"}, {"layer", id}, {"type", "shake"}}, &b);
    doc = applyOp(doc, {{"op", "bakeBehavior"}, {"layer", id}, {"behavior", b.data["behavior"]}});
    L = findLayer(*activeComp(doc), id);
    CHECK((*L)["behaviors"].empty());
    CHECK(hasKeyframes((*L)["transform"]["position"]));
    doc = applyOp(doc, {{"op", "setTransition"}, {"layer", id}, {"edge", "in"}, {"transition", {{"type", "wipe"}, {"duration", 0.5}}}});
    bool threw = false;
    try { applyOp(doc, {{"op", "addEffect"}, {"layer", id}, {"type", "nope"}}); } catch (EditError&) { threw = true; }
    CHECK(threw);
    for (auto& info : effectRegistry()) {
        json d = applyOp(doc, {{"op", "addEffect"}, {"layer", id}, {"type", info.type}});
        CHECK(!findLayer(*activeComp(d), id)->at("effects").empty());
    }
}

TEST(text_presets_all_valid) {
    json doc = newProject("T", 1920, 1080, 30, 10);
    OpResult a;
    doc = applyOp(doc, {{"op", "addLayer"}, {"kind", "text"}}, &a);
    for (std::string p : {"typeOn", "fadeUp", "pop", "bounce", "wave", "scramble", "kineticWords", "letterRotate", "maskedReveal", "blurIn"}) {
        json d = applyOp(doc, {{"op", "textPreset"}, {"layer", a.data["layer"]}, {"preset", p}, {"duration", 1.0}});
        CHECK(findLayer(*activeComp(d), a.data["layer"])->at("text").at("animators").size() == 1);
    }
}

TEST(diagnostics_missing_asset_and_font) {
    std::string id;
    json doc = projectWithVideo(id);
    OpResult t;
    doc = applyOp(doc, {{"op", "addLayer"}, {"kind", "text"}, {"options", {{"font", "NoSuchFont"}}}}, &t);
    auto diags = diagnoseProject(doc, {"DejaVuSans"}, [](const json&) { return false; });
    bool missingAsset = false, missingFont = false;
    for (auto& d : diags) {
        if (d.code == "MISSING_ASSET") missingAsset = true;
        if (d.code == "MISSING_FONT") missingFont = true;
    }
    CHECK(missingAsset);
    CHECK(missingFont);
}
