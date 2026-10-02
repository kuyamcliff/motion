// Mobile Motion API (script API v2): one script drives every namespace; checks results in the project.
#include "mf/model.hpp"
#include "mf/scripting.hpp"
#include "mftest.hpp"

using namespace mf;

TEST(script_api_v2_covers_every_namespace) {
    ScriptRequest req;
    req.name = "api-v2";
    req.permissions = {"PROJECT_READ", "PROJECT_WRITE", "TIMELINE_WRITE", "LOCAL_STORAGE", "RENDER"};
    req.project = newProject("API", 640, 360, 30, 5);
    req.compId = "C1";
    req.source = R"JS(
var t = mf.layer.add('text', {text: 'Hello'}, 0);
mf.text.style(t, {size: 80, tracking: 10});
mf.text.animator(t, {selector: {start: {v: 0}, end: {v: 100}, offset: {v: 0}, unit: 'char', shape: 'square'}, props: {opacity: {v: 0}}});
mf.prop.set(t, 'transform.position', [100, 100, 0], 0);
mf.keyframe.add(t, 'transform.position', 0, [100, 100, 0]);
mf.keyframe.add(t, 'transform.position', 2, [500, 100, 0], 'easeOut');
mf.keyframe.interpAll(t, 'transform.position', 'bounce');
var kf = mf.keyframe.list(t, 'transform.position');
if (kf.length !== 2) throw new Error('keyframes ' + kf.length);
var mid = mf.prop.get(t, 'transform.position', 1);
if (!(mid[0] > 100 && mid[0] < 500)) throw new Error('eval ' + mid);
mf.prop.expression(t, 'transform.rotation', 'time * 10');
var s = mf.layer.add('shape', {shape: 'rect'}, 0);
mf.shape.fill(s, [0, 1, 0, 1]);
mf.shape.stroke(s, [1, 1, 1, 1], 6);
mf.shape.modifier(s, 'trim', true);
var fx = mf.effect.add(s, 'stylize.glow');
mf.effect.param(s, fx, 'intensity', 50, 0);
mf.effect.mix(s, fx, 70);
var m = mf.mask.add(s, 'ellipse');
mf.mask.set(s, m, {inverted: true});
mf.layer.blend(s, 'screen');
mf.layer.matte(s, t, 'alpha', false);
mf.layer.split(s, 1.5);
var cam = mf.camera.add();
mf.camera.zoom(cam, 900);
var light = mf.light.add('spot');
var cube = mf.model.primitive('torus', {size: 120});
mf.model.material(cube, {metallic: 0.8});
mf.particles.add('snow');
mf.marker.add(1, 'Beat');
if (mf.marker.list().length !== 1) throw new Error('markers');
mf.caption.set([{start: 0, end: 1, text: 'hello world'}]);
mf.caption.add([{start: 1, end: 2, text: 'second'}]);
if (mf.caption.replace('world', 'there') !== 1) throw new Error('replace');
mf.caption.style({uppercase: true});
mf.transition.set(cube, 'in', 'fade', 0.4);
var b = mf.behavior.add(cube, 'float');
mf.behavior.bake(cube, b);
var nc = mf.comp.create({name: 'Second', width: 320, height: 180, fps: 24, duration: 2});
mf.comp.use(nc);
mf.layer.add('solid', {}, 0);
if (mf.comp.active().layers.length !== 1) throw new Error('comp.use');
mf.comp.use('C1');
mf.layer.select([t]);
mf.ui.panel({fields: [{name: 'title', type: 'text', default: 'Hi'}]});
mf.storage.set('runs', (mf.storage.get('runs') || 0) + 1);
return mf.comp.list().length;
)JS";
    ScriptResult r = runScript(req);
    if (!r.ok) std::printf("    %s\n", r.error.c_str());
    REQUIRE(r.ok);
    CHECK(r.returnValue == 2);
    CHECK(r.changed);
    const json* c1 = findComp(r.project, "C1");
    REQUIRE(c1 != nullptr);
    int texts = 0, shapes = 0, cams = 0, lights = 0, models = 0, parts = 0, caps = 0;
    for (auto& L : (*c1)["layers"]) {
        std::string ty = L.value("type", "");
        texts += ty == "text"; shapes += ty == "shape"; cams += ty == "camera"; lights += ty == "light";
        models += ty == "model3d"; parts += ty == "particles"; caps += ty == "captions";
    }
    CHECK(texts == 1); CHECK(shapes == 2); CHECK(cams == 1); CHECK(lights == 1); CHECK(models == 1); CHECK(parts == 1); CHECK(caps == 1);
    bool sawPanel = false, sawSelect = false;
    for (auto& a : r.actions) { sawPanel |= a.value("type", "") == "panel"; sawSelect |= a.value("type", "") == "select"; }
    CHECK(sawPanel && sawSelect);
    CHECK(r.storage.value("runs", 0) == 1);
    // The generated reference lists the new namespaces.
    json api = scriptApiDescription();
    CHECK(api["apiVersion"] == 2);
    std::string all = api["functions"].dump();
    for (const char* fn : {"mf.prop.get(", "mf.mask.add(", "mf.camera.add(", "mf.capsule.insert(", "mf.caption.merge(", "mf.ui.panel(", "mf.audio.duck("})
        CHECK(all.find(fn) != std::string::npos);
    CHECK(api["functions"].size() > 100);
}

TEST(script_api_v2_permission_and_errors) {
    ScriptRequest req;
    req.permissions = {"PROJECT_READ"};
    req.project = newProject("API", 320, 180, 30, 2);
    req.compId = "C1";
    req.source = "mf.camera.add();";
    ScriptResult r = runScript(req);
    CHECK(!r.ok);
    CHECK(r.error.find("PROJECT_WRITE") != std::string::npos);
    req.permissions = {"PROJECT_READ", "TIMELINE_WRITE"};
    req.source = "mf.prop.get('nope', 'transform.position');";
    r = runScript(req);
    CHECK(!r.ok);
    CHECK(r.error.find("not found") != std::string::npos);
}
