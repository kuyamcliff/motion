#include "mf/scripting.hpp"

#include <cstring>

#include <chrono>
#include <set>

#include "mf/document.hpp"
#include "mf/effects.hpp"
#include "mf/expressions.hpp"
#include "mf/model.hpp"
#include "quickjs.h"

namespace mf {

extern const char* kJsVectorPrelude;

namespace {

struct ScriptState {
    const ScriptRequest* req;
    ScriptResult* res;
    json doc;
    std::set<std::string> perms;
    std::chrono::steady_clock::time_point deadline;
    std::string compId;
};

ScriptState* stateOf(JSContext* c) { return (ScriptState*)JS_GetContextOpaque(c); }

std::string jsStr(JSContext* c, JSValueConst v) {
    const char* s = JS_ToCString(c, v);
    std::string out = s ? s : "";
    JS_FreeCString(c, s);
    return out;
}

json jsToJson(JSContext* c, JSValueConst v) {
    if (JS_IsUndefined(v)) return json();
    JSValue s = JS_JSONStringify(c, v, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(s) || JS_IsUndefined(s)) { JS_FreeValue(c, s); return json(); }
    std::string str = jsStr(c, s);
    JS_FreeValue(c, s);
    json j = json::parse(str, nullptr, false);
    return j.is_discarded() ? json() : j;
}

JSValue jsonToJs(JSContext* c, const json& j) {
    std::string s = j.dump();
    return JS_ParseJSON(c, s.c_str(), s.size(), "<json>");
}

bool need(JSContext* c, const char* perm) {
    ScriptState* st = stateOf(c);
    if (st->perms.count(perm)) return true;
    JS_ThrowTypeError(c, "Permission %s is required. Add it to the script's permission manifest.", perm);
    return false;
}

bool needWrite(JSContext* c) {
    ScriptState* st = stateOf(c);
    if (st->perms.count("PROJECT_WRITE") || st->perms.count("TIMELINE_WRITE")) return true;
    JS_ThrowTypeError(c, "Permission PROJECT_WRITE (or TIMELINE_WRITE) is required to modify the project.");
    return false;
}

JSValue applyOpJs(JSContext* c, json op) {
    ScriptState* st = stateOf(c);
    if (!op.contains("comp") && !st->compId.empty()) op["comp"] = st->compId;
    try {
        OpResult r;
        st->doc = applyOp(st->doc, op, &r);
        st->res->opsApplied++;
        st->res->changed = true;
        return jsonToJs(c, r.data);
    } catch (EditError& e) {
        return JS_ThrowTypeError(c, "%s", e.what());
    } catch (std::exception& e) {
        return JS_ThrowInternalError(c, "%s", e.what());
    }
}

// ---------------------------------------------------------------- native bindings
JSValue n_log(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    std::string line;
    for (int i = 0; i < argc; ++i) {
        if (i) line += " ";
        if (JS_IsString(argv[i])) line += jsStr(c, argv[i]);
        else {
            json j = jsToJson(c, argv[i]);
            line += j.is_null() ? jsStr(c, argv[i]) : j.dump();
        }
    }
    auto& logs = stateOf(c)->res->logs;
    if (logs.size() < 2000) logs.push_back(line);
    return JS_UNDEFINED;
}

JSValue n_alert(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    if (argc > 0) stateOf(c)->res->alerts.push_back(jsStr(c, argv[0]));
    return JS_UNDEFINED;
}

JSValue n_project(JSContext* c, JSValueConst, int, JSValueConst*) {
    if (!need(c, "PROJECT_READ")) return JS_EXCEPTION;
    return jsonToJs(c, stateOf(c)->doc);
}

JSValue n_comp(JSContext* c, JSValueConst, int, JSValueConst*) {
    if (!need(c, "PROJECT_READ")) return JS_EXCEPTION;
    ScriptState* st = stateOf(c);
    const json* comp = st->compId.empty() ? activeComp(st->doc) : findComp(st->doc, st->compId);
    return comp ? jsonToJs(c, *comp) : JS_NULL;
}

JSValue n_op(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    if (!needWrite(c)) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(c, "mf.op(op) needs an operation object");
    json op = jsToJson(c, argv[0]);
    if (!op.is_object() || !op.contains("op")) return JS_ThrowTypeError(c, "Operation must be an object with an 'op' field, e.g. {op:'addLayer', kind:'text'}");
    return applyOpJs(c, op);
}

JSValue n_storageGet(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    if (!need(c, "LOCAL_STORAGE")) return JS_EXCEPTION;
    if (argc < 1) return JS_UNDEFINED;
    std::string k = jsStr(c, argv[0]);
    auto& s = stateOf(c)->res->storage;
    return s.contains(k) ? jsonToJs(c, s[k]) : JS_UNDEFINED;
}

JSValue n_storageSet(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    if (!need(c, "LOCAL_STORAGE")) return JS_EXCEPTION;
    if (argc < 2) return JS_UNDEFINED;
    auto& s = stateOf(c)->res->storage;
    if (s.dump().size() > 256 * 1024) return JS_ThrowRangeError(c, "Script storage is limited to 256 KB.");
    s[jsStr(c, argv[0])] = jsToJson(c, argv[1]);
    return JS_UNDEFINED;
}

JSValue n_action(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    if (!need(c, "RENDER")) return JS_EXCEPTION;
    if (argc > 0) stateOf(c)->res->actions.push_back(jsToJson(c, argv[0]));
    return JS_UNDEFINED;
}

// mf.prop.get(layer, path, time): evaluated value of any property (keyframes + expressions are not applied here;
// keyframes are, expressions are not, so scripts read the authored value).
JSValue n_eval(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    if (!need(c, "PROJECT_READ")) return JS_EXCEPTION;
    if (argc < 2) return JS_ThrowTypeError(c, "mf.prop.get(layerId, path, time?)");
    ScriptState* st = stateOf(c);
    const json* comp = st->compId.empty() ? activeComp(st->doc) : findComp(st->doc, st->compId);
    const json* L = comp ? findLayer(*comp, jsStr(c, argv[0])) : nullptr;
    if (!L) return JS_ThrowTypeError(c, "Layer '%s' not found", jsStr(c, argv[0]).c_str());
    std::string path = jsStr(c, argv[1]);
    const json* P = resolvePath(*L, path);
    if (!P) return JS_ThrowTypeError(c, "Property '%s' not found on layer", path.c_str());
    double t = st->req->playhead;
    if (argc > 2 && JS_IsNumber(argv[2])) JS_ToFloat64(c, &t, argv[2]);
    if (P->is_object() && (P->contains("v") || P->contains("k"))) return jsonToJs(c, evalRaw(*P, layerLocalTime(*L, t)).toJson());
    return jsonToJs(c, *P);
}

// mf.comp.setActive(id): later calls target that composition.
JSValue n_useComp(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    ScriptState* st = stateOf(c);
    std::string id = jsStr(c, argv[0]);
    if (!findComp(st->doc, id)) return JS_ThrowTypeError(c, "Composition '%s' not found", id.c_str());
    st->compId = id;
    return JS_UNDEFINED;
}

// UI-side requests that need no extra permission: show a script panel, change selection.
JSValue n_uiAction(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    if (argc > 0) {
        auto& a = stateOf(c)->res->actions;
        if (a.size() < 64) a.push_back(jsToJson(c, argv[0]));
    }
    return JS_UNDEFINED;
}

JSValue n_effects(JSContext* c, JSValueConst, int, JSValueConst*) {
    json out = json::array();
    for (auto& e : effectRegistry()) out.push_back({{"type", e.type}, {"name", e.name}, {"category", e.category}});
    return jsonToJs(c, out);
}

int interrupt(JSRuntime* rt, void* opaque) {
    auto* st = (ScriptState*)opaque;
    return std::chrono::steady_clock::now() > st->deadline ? 1 : 0;
}

const char* kScriptPrelude = R"JS(
var console = { log: function(){ __n_log.apply(null, arguments); }, warn: function(){ __n_log.apply(null, ["WARN"].concat([].slice.call(arguments))); }, error: function(){ __n_log.apply(null, ["ERROR"].concat([].slice.call(arguments))); } };
var mf = {
  app: { version: __mf_version, apiVersion: 2 },
  log: function(){ __n_log.apply(null, arguments); },
  op: function(o){ return __n_op(o); },
  get time(){ return __mf_playhead; },
  get selection(){ return __mf_selection.slice(); },
  args: __mf_args,
  project: {
    get: function(){ return __n_project(); },
    get name(){ return __n_project().meta.name; },
    rename: function(n){ return __n_op({op:"setProjectName", name:n}); }
  },
  comp: {
    active: function(){ return __n_comp(); },
    list: function(){ return __n_project().comps.map(function(c){ return {id:c.id, name:c.name, width:c.width, height:c.height, fps:c.fps, duration:c.duration}; }); },
    create: function(o){ o = o||{}; o.op = "addComp"; return __n_op(o).comp; },
    update: function(fields){ fields.op = "updateComp"; return __n_op(fields); },
    use: function(id){ __n_useComp(id); },
    setActive: function(id){ __n_op({op:"setActiveComp", comp:id}); __n_useComp(id); },
    remove: function(id){ return __n_op({op:"removeComp", target:id, comp:id}); },
    precompose: function(ids, name){ return __n_op({op:"precompose", layers:ids, name:name||"Precomp"}); }
  },
  layer: {
    add: function(kind, options, at){ var r = __n_op({op:"addLayer", kind:kind, options:options||{}, at: at===undefined?__mf_playhead:at}); return r.layer; },
    all: function(){ return __n_comp().layers; },
    get: function(id){ var ls = __n_comp().layers; for (var i=0;i<ls.length;i++) if (ls[i].id===id || ls[i].name===id) return ls[i]; return null; },
    find: function(q){ q = String(q).toLowerCase(); return __n_comp().layers.filter(function(l){ return l.name.toLowerCase().indexOf(q) >= 0; }).map(function(l){ return l.id; }); },
    ofType: function(t){ return __n_comp().layers.filter(function(l){ return l.type===t; }).map(function(l){ return l.id; }); },
    set: function(id, path, value, time){ return __n_op({op:"setProp", layer:id, path:path, value:value, t: time===undefined?__mf_playhead:time}); },
    setStatic: function(id, path, value){ return __n_op({op:"setProp", layer:id, path:path, value:value, mode:"static"}); },
    rename: function(id, name){ return __n_op({op:"setLayer", layer:id, fields:{name:name}}); },
    fields: function(id, f){ return __n_op({op:"setLayer", layer:id, fields:f}); },
    duplicate: function(id){ return __n_op({op:"duplicateLayers", layers:[id]}).layers[0]; },
    remove: function(id){ return __n_op({op:"removeLayers", layers:[].concat(id)}); },
    timing: function(id, inT, outT){ return __n_op({op:"setLayerTiming", layer:id, "in":inT, out:outT}); },
    move: function(id, dt){ return __n_op({op:"moveLayerTime", layers:[].concat(id), dt:dt}); },
    trim: function(id, edge, time){ return __n_op({op:"trimLayer", layer:id, edge:edge, t:time}); },
    split: function(id, time){ return __n_op({op:"split", layers:[].concat(id), t: time===undefined?__mf_playhead:time}); },
    reorder: function(id, index){ return __n_op({op:"reorderLayer", layer:id, index:index}); },
    speed: function(id, s){ return __n_op({op:"setSpeed", layer:id, speed:s}); },
    reverse: function(id, on){ return __n_op({op:"reverse", layer:id, on: on!==false}); },
    freeze: function(id, time, dur){ return __n_op({op:"freezeFrame", layer:id, t:time===undefined?__mf_playhead:time, duration:dur||2}).layer; },
    blend: function(id, mode){ return __n_op({op:"setLayer", layer:id, fields:{blend:mode}}); },
    matte: function(id, matteId, mode, invert){ return __n_op({op:"setLayer", layer:id, fields:{matte: matteId ? {layer:matteId, mode:mode||"alpha", invert:!!invert} : null}}); },
    threeD: function(id, on){ return __n_op({op:"setLayer", layer:id, fields:{threeD: on!==false}}); },
    parent: function(id, parentId){ return __n_op({op:"parent", layer:id, parent:parentId, t:__mf_playhead}); },
    select: function(ids){ __n_uiAction({type:"select", layers:[].concat(ids)}); }
  },
  prop: {
    get: function(id, path, time){ return __n_eval(id, path, time); },
    set: function(id, path, value, time){ return __n_op({op:"setProp", layer:id, path:path, value:value, t: time===undefined?__mf_playhead:time}); },
    expression: function(id, path, expr){ return __n_op({op:"setExpression", layer:id, path:path, expr:expr||""}); }
  },
  keyframe: {
    add: function(id, path, time, value, interp){ var o = {op:"addKeyframe", layer:id, path:path, t:time}; if (value!==undefined) o.value=value; if (interp) o.interp=interp; return __n_op(o); },
    remove: function(id, path, time){ return __n_op({op:"removeKeyframe", layer:id, path:path, t:time}); },
    interp: function(id, path, time, interp){ return __n_op({op:"setKeyframeInterp", layer:id, path:path, t:time, interp:interp}); },
    interpAll: function(id, path, interp){ return __n_op({op:"setKeyframeInterp", layer:id, path:path, t:0, interp:interp, all:true}); },
    move: function(id, path, from, to){ return __n_op({op:"moveKeyframe", layer:id, path:path, from:from, to:to}); },
    list: function(id, path){ var l = mf.layer.get(id); if (!l) return []; var p = path.split(".").reduce(function(o,k){ if (!o) return o; if (Array.isArray(o)) { for (var i=0;i<o.length;i++) if (o[i].id===k) return o[i]; return o[+k]; } return o[k]; }, l); return (p && p.k) ? p.k.map(function(k){ return {t: k.t + l.start, v: k.v, interp: k.o}; }) : []; },
    clear: function(id, path){ return __n_op({op:"clearKeyframes", layer:id, path:path, t:__mf_playhead}); },
    reverse: function(id, path){ return __n_op({op:"reverseKeyframes", layer:id, path:path}); },
    distribute: function(id, path){ return __n_op({op:"distributeKeyframes", layer:id, path:path}); },
    scale: function(id, path, factor, pivot){ return __n_op({op:"scaleKeyframes", layer:id, path:path, factor:factor, pivot:pivot}); }
  },
  effect: {
    add: function(id, type, params){ return __n_op({op:"addEffect", layer:id, type:type, params:params||{}}).effect; },
    list: function(){ return __n_effects(); },
    on: function(id){ var l = mf.layer.get(id); return l ? l.effects : []; },
    remove: function(id, fx){ return __n_op({op:"removeEffect", layer:id, effect:fx}); },
    enable: function(id, fx, on){ return __n_op({op:"setEffect", layer:id, effect:fx, fields:{enabled: on!==false}}); },
    mix: function(id, fx, pct){ return __n_op({op:"setEffect", layer:id, effect:fx, fields:{mix: pct}}); },
    move: function(id, fx, index){ return __n_op({op:"moveEffect", layer:id, effect:fx, index:index}); },
    param: function(id, fx, name, value, time){ return __n_op({op:"setProp", layer:id, path:"effects."+fx+".params."+name, value:value, t: time===undefined?__mf_playhead:time}); }
  },
  mask: {
    add: function(id, shape, rect){ var o = {op:"addMask", layer:id, shape:shape||"rect"}; if (rect) o.rect = rect; return __n_op(o).mask; },
    remove: function(id, m){ return __n_op({op:"removeMask", layer:id, mask:m}); },
    set: function(id, m, fields){ return __n_op({op:"setMask", layer:id, mask:m, fields:fields}); }
  },
  shape: {
    add: function(id, item){ return __n_op({op:"addShapeItem", layer:id, item:item}).item; },
    remove: function(id, item){ return __n_op({op:"removeShapeItem", layer:id, item:item}); },
    fill: function(id, color){ return __n_op({op:"setProp", layer:id, path:"shape.fill.color", value:color, mode:"static"}); },
    stroke: function(id, color, width){ __n_op({op:"setProp", layer:id, path:"shape.stroke.enabled", value:true}); __n_op({op:"setProp", layer:id, path:"shape.stroke.color", value:color, mode:"static"}); return __n_op({op:"setProp", layer:id, path:"shape.stroke.width", value:width||4, mode:"static"}); },
    modifier: function(id, name, on){ return __n_op({op:"setProp", layer:id, path:"shape."+name+".enabled", value: on!==false}); }
  },
  text: {
    set: function(id, content){ return __n_op({op:"setText", layer:id, content:String(content)}); },
    preset: function(id, name, duration){ return __n_op({op:"textPreset", layer:id, preset:name, duration:duration||1, t:__mf_playhead}); },
    style: function(id, s){ for (var k in s) __n_op({op:"setProp", layer:id, path:"text."+k, value:s[k], mode: (k==="font"||k==="align") ? undefined : "static"}); },
    animator: function(id, animator){ return __n_op({op:"addAnimator", layer:id, animator:animator}).animator; },
    removeAnimator: function(id, a){ return __n_op({op:"removeAnimator", layer:id, animator:a}); }
  },
  camera: {
    add: function(options){ return mf.layer.add("camera", options||{}, 0); },
    lookAt: function(id, point){ return __n_op({op:"setProp", layer:id, path:"camera.poi", value:point, t:__mf_playhead}); },
    zoom: function(id, z){ return __n_op({op:"setProp", layer:id, path:"camera.zoom", value:z, t:__mf_playhead}); }
  },
  light: { add: function(kind, options){ var o = options||{}; o.light = kind||"point"; return mf.layer.add("light", o, 0); } },
  model: {
    primitive: function(name, options){ var o = options||{}; o.primitive = name||"cube"; return mf.layer.add("model3d", o); },
    material: function(id, m){ for (var k in m) __n_op({op:"setProp", layer:id, path:"model.material."+k, value:m[k], mode:"static"}); }
  },
  particles: { add: function(preset){ return mf.layer.add("particles", {preset: preset||"sparks"}); } },
  audio: {
    volume: function(id, db, time){ return __n_op({op:"setProp", layer:id, path:"audio.volume", value:db, t: time===undefined?__mf_playhead:time}); },
    pan: function(id, p){ return __n_op({op:"setProp", layer:id, path:"audio.pan", value:p, mode:"static"}); },
    bus: function(id, b){ return __n_op({op:"setProp", layer:id, path:"audio.bus", value:b}); },
    duck: function(id, ranges, amount){ return __n_op({op:"addDuckingKeys", layer:id, ranges:ranges, amount: amount===undefined?-12:amount}); },
    mixer: function(audio){ return __n_op({op:"setCompAudio", audio:audio}); }
  },
  marker: {
    add: function(time, title, opts){ var o = opts||{}; o.op="addMarker"; o.t=time; o.title=title||""; return __n_op(o).marker; },
    list: function(){ return __n_comp().markers || []; },
    remove: function(id){ return __n_op({op:"removeMarker", marker:id}); }
  },
  caption: {
    set: function(items){ return __n_op({op:"setCaptions", items:items}); },
    add: function(items){ return __n_op({op:"setCaptions", items:items, append:true}); },
    list: function(){ var l = __n_comp().layers.filter(function(x){ return x.type==="captions"; })[0]; return l ? l.captions.items : []; },
    update: function(id, fields){ return __n_op({op:"updateCaption", caption:id, fields:fields}); },
    remove: function(id){ return __n_op({op:"removeCaption", caption:id}); },
    split: function(id, time){ return __n_op({op:"splitCaption", caption:id, t:time}); },
    merge: function(a, b){ return __n_op({op:"mergeCaptions", first:a, second:b}); },
    style: function(s){ return __n_op({op:"setCaptionStyle", style:s}); },
    replace: function(find, repl){ return __n_op({op:"captionReplace", find:find, replace:repl}).count; }
  },
  capsule: {
    insert: function(capsule, time){ return __n_op({op:"insertCapsule", capsule:capsule, t: time===undefined?__mf_playhead:time}).layer; },
    control: function(id, name, value){ return __n_op({op:"setCapsuleControl", layer:id, control:name, value:value}); },
    controls: function(id){ var l = mf.layer.get(id); return (l && l.precomp) ? l.precomp.controlDefs : []; }
  },
  preset: { apply: function(id, preset){ return __n_op({op:"applyPreset", layer:id, preset:preset}); } },
  transition: { set: function(id, edge, type, duration, params){ return __n_op({op:"setTransition", layer:id, edge:edge||"in", transition: type ? {type:type, duration:duration||0.5, params:params||{}} : null}); } },
  behavior: {
    add: function(id, type, params){ return __n_op({op:"addBehavior", layer:id, type:type, params:params||{}}).behavior; },
    bake: function(id, b){ return __n_op({op:"bakeBehavior", layer:id, behavior:b}); },
    remove: function(id, b){ return __n_op({op:"removeBehavior", layer:id, behavior:b}); }
  },
  media: {
    list: function(){ return __n_project().assets; },
    request: function(kinds){ __n_uiAction({type:"importRequest", kinds:[].concat(kinds||["video","image","audio"])}); }
  },
  storage: { get: function(k){ return __n_storageGet(k); }, set: function(k, v){ return __n_storageSet(k, v); } },
  ui: {
    alert: function(m){ __n_alert(String(m)); },
    // Declares a panel of inputs; Script Studio renders it and re-runs the script with the values in mf.args.
    panel: function(spec){ __n_uiAction({type:"panel", spec:spec}); }
  },
  render: { requestExport: function(preset){ __n_action({type:"export", preset:preset||{}}); } }
};
)JS";

thread_local const std::string* g_source = nullptr;

std::string sourceLine(int line) {
    if (!g_source || line <= 0) return {};
    size_t pos = 0;
    for (int i = 1; i < line && pos != std::string::npos; ++i) {
        pos = g_source->find('\n', pos);
        if (pos != std::string::npos) ++pos;
    }
    if (pos == std::string::npos) return {};
    size_t e = g_source->find('\n', pos);
    return g_source->substr(pos, e == std::string::npos ? std::string::npos : e - pos);
}

std::string errorWithSuggestion(JSContext* c, int& line) {
    JSValue ex = JS_GetException(c);
    std::string msg = jsStr(c, ex);
    JSValue stack = JS_GetPropertyStr(c, ex, "stack");
    line = 0;
    if (JS_IsString(stack)) {
        std::string st = jsStr(c, stack);
        auto pos = st.find("<script>:");
        if (pos != std::string::npos) line = std::atoi(st.c_str() + pos + 9);
    }
    JS_FreeValue(c, stack);
    JS_FreeValue(c, ex);
    std::string sugg;
    std::string src = sourceLine(line);
    static const char* props[] = {"opacity", "position", "scale", "rotation", "anchor"};
    if (msg.find("interrupted") != std::string::npos) msg = "Script exceeded its time limit and was stopped";
    else if (msg.find("undefined") != std::string::npos) {
        for (const char* p : props)
            if (src.find(std::string(".") + p) != std::string::npos && src.find(std::string(".transform.") + p) == std::string::npos) {
                sugg = std::string("Suggestion: use mf.layer.get(id).transform.") + p + ".v (transform properties live under .transform and store their value in .v)";
                break;
            }
        if (sugg.empty() && msg.find("cannot read property") != std::string::npos)
            sugg = "Suggestion: a value in this chain is undefined; check the layer id and property path (see API Docs).";
    }
    else if (msg.find("is not defined") != std::string::npos)
        sugg = "Suggestion: the API lives under the global 'mf' object (mf.layer, mf.keyframe, mf.effect ...)";
    else if (msg.find("not a function") != std::string::npos)
        sugg = "Suggestion: check the API reference (Script Studio > API Docs) for available functions";
    std::string out = (line > 0 ? "Line " + std::to_string(line) + ": " : std::string()) + msg;
    if (!sugg.empty()) out += "\n" + sugg;
    return out;
}

}  // namespace

std::vector<std::string> allPermissions() {
    return {"PROJECT_READ", "PROJECT_WRITE", "MEDIA_READ", "MEDIA_WRITE", "TIMELINE_WRITE", "GPU_SHADER", "AUDIO_PROCESS", "LOCAL_STORAGE", "RENDER", "NETWORK", "NATIVE_CODE"};
}

ScriptResult runScript(const ScriptRequest& req) {
    ScriptResult res;
    auto t0 = std::chrono::steady_clock::now();
    ScriptState st;
    st.req = &req;
    st.res = &res;
    st.doc = req.project;
    st.compId = req.compId;
    for (auto& p : req.permissions) st.perms.insert(p);
    if (st.perms.count("NETWORK")) res.logs.push_back("NOTE: NETWORK permission is not available to scripts; no network API exists in the sandbox.");
    res.storage = req.storage.is_object() ? req.storage : json::object();
    st.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds((int64_t)(req.timeoutSec * 1000));
    JSRuntime* rt = JS_NewRuntime();
    JS_SetMemoryLimit(rt, req.memoryLimit);
    JS_SetMaxStackSize(rt, 1u << 20);
    JS_SetInterruptHandler(rt, interrupt, &st);
    JSContext* c = JS_NewContext(rt);
    JS_SetContextOpaque(c, &st);
    JSValue g = JS_GetGlobalObject(c);
    auto fn = [&](const char* name, JSCFunction* f, int n) { JS_SetPropertyStr(c, g, name, JS_NewCFunction(c, f, name, n)); };
    fn("__n_log", n_log, 1);
    fn("__n_alert", n_alert, 1);
    fn("__n_project", n_project, 0);
    fn("__n_comp", n_comp, 0);
    fn("__n_op", n_op, 1);
    fn("__n_storageGet", n_storageGet, 1);
    fn("__n_storageSet", n_storageSet, 2);
    fn("__n_action", n_action, 1);
    fn("__n_effects", n_effects, 0);
    fn("__n_eval", n_eval, 3);
    fn("__n_useComp", n_useComp, 1);
    fn("__n_uiAction", n_uiAction, 1);
    JS_SetPropertyStr(c, g, "__mf_version", JS_NewString(c, kEngineVersion));
    JS_SetPropertyStr(c, g, "__mf_playhead", JS_NewFloat64(c, req.playhead));
    JS_SetPropertyStr(c, g, "__mf_selection", jsonToJs(c, json(req.selection)));
    JS_SetPropertyStr(c, g, "__mf_args", jsonToJs(c, req.args));
    JS_FreeValue(c, g);
    bool ok = true;
    for (const char* pre : {kJsVectorPrelude, kScriptPrelude}) {
        JSValue r = JS_Eval(c, pre, std::strlen(pre), "<prelude>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(r)) { int l; res.error = "Internal prelude error: " + errorWithSuggestion(c, l); ok = false; }
        JS_FreeValue(c, r);
    }
    if (ok) {
        std::string code;
        if (!rewriteVectorOperators(req.source, code)) code = req.source;
        // Wrap in a function so 'return' works at top level; keep line numbers aligned (same line).
        std::string wrapped = "(function(){" + code + "\n})()";
        g_source = &req.source;
        JSValue r = JS_Eval(c, wrapped.c_str(), wrapped.size(), "<script>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(r)) {
            res.error = errorWithSuggestion(c, res.line);
            ok = false;
        } else {
            res.returnValue = jsToJson(c, r);
        }
        g_source = nullptr;
        JS_FreeValue(c, r);
    }
    JS_FreeContext(c);
    JS_FreeRuntime(rt);
    res.ok = ok;
    if (ok) {
        auto problems = validateProject(st.doc);
        if (!problems.empty()) {
            res.ok = false;
            res.error = "Script produced an invalid project and was rolled back: " + problems[0];
            res.changed = false;
        } else {
            res.project = std::move(st.doc);
        }
    } else {
        res.changed = false;  // failed scripts never modify the project
    }
    res.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return res;
}

bool validateScriptSyntax(const std::string& source, std::string& error, int& line) {
    JSRuntime* rt = JS_NewRuntime();
    JSContext* c = JS_NewContext(rt);
    std::string code;
    if (!rewriteVectorOperators(source, code)) code = source;
    std::string wrapped = "(function(){" + code + "\n})";
    JSValue r = JS_Eval(c, wrapped.c_str(), wrapped.size(), "<script>", JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    bool ok = !JS_IsException(r);
    if (!ok) error = errorWithSuggestion(c, line);
    JS_FreeValue(c, r);
    JS_FreeContext(c);
    JS_FreeRuntime(rt);
    return ok;
}

// Every function of the live `mf` object with its parameter names, generated from the prelude itself so the
// reference can never drift from the API.
static json introspectApi() {
    JSRuntime* rt = JS_NewRuntime();
    JSContext* c = JS_NewContext(rt);
    JSValue g = JS_GetGlobalObject(c);
    JS_SetPropertyStr(c, g, "__mf_version", JS_NewString(c, kEngineVersion));
    JS_SetPropertyStr(c, g, "__mf_playhead", JS_NewFloat64(c, 0));
    JS_SetPropertyStr(c, g, "__mf_selection", JS_NewArray(c));
    JS_SetPropertyStr(c, g, "__mf_args", JS_NewObject(c));
    JS_FreeValue(c, g);
    json out = json::array();
    JSValue r = JS_Eval(c, kScriptPrelude, std::strlen(kScriptPrelude), "<prelude>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(c, r);
    const char* walker = R"JS(
(function(){ var out = [];
  function walk(o, p) { Object.keys(o).forEach(function(k){
    var d = Object.getOwnPropertyDescriptor(o, k);
    if (d.get) { out.push(p + k); return; }
    var v = o[k];
    if (typeof v === 'function') { var m = String(v).match(/^function\s*\(([^)]*)\)/); out.push(p + k + '(' + (m ? m[1].replace(/\s+/g, ' ') : '') + ')'); }
    else if (v && typeof v === 'object' && !Array.isArray(v)) walk(v, p + k + '.');
  }); }
  walk(mf, 'mf.'); return JSON.stringify(out); })())JS";
    JSValue w = JS_Eval(c, walker, std::strlen(walker), "<api>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsString(w)) out = json::parse(jsStr(c, w), nullptr, false);
    JS_FreeValue(c, w);
    JS_FreeContext(c);
    JS_FreeRuntime(rt);
    return out.is_array() ? out : json::array();
}

json scriptApiDescription() {
    auto f = [](const char* name, const char* sig, const char* doc, const char* perm) { return json{{"name", name}, {"signature", sig}, {"doc", doc}, {"permission", perm}}; };
    return json{{"apiVersion", 2},
                {"functions", introspectApi()},
                {"globals", json::array({f("mf.log", "(...values)", "Print to the Script Studio console.", ""),
                                         f("mf.op", "(op: object) -> object", "Apply any engine operation (see Developer Center > Operations).", "PROJECT_WRITE"),
                                         f("mf.time", "number", "Current playhead (seconds).", ""), f("mf.selection", "string[]", "Selected layer ids.", ""),
                                         f("mf.args", "object", "Values from the script's UI panel.", "")})},
                {"project", json::array({f("mf.project.get", "() -> Project", "Read the whole project document.", "PROJECT_READ"),
                                         f("mf.project.rename", "(name)", "Rename the project.", "PROJECT_WRITE")})},
                {"comp", json::array({f("mf.comp.active", "() -> Composition", "Active composition JSON.", "PROJECT_READ"),
                                      f("mf.comp.create", "({name,width,height,fps,duration}) -> id", "Create a composition.", "PROJECT_WRITE")})},
                {"layer", json::array({f("mf.layer.add", "(kind, options?, at?) -> id", "Add a layer (text, shape, solid, null, adjustment, camera, light, particles, model3d, captions).", "TIMELINE_WRITE"),
                                       f("mf.layer.get", "(idOrName) -> Layer", "Get a layer.", "PROJECT_READ"),
                                       f("mf.layer.find", "(text) -> id[]", "Find layers whose name contains text.", "PROJECT_READ"),
                                       f("mf.layer.set", "(id, path, value, time?)", "Set a property (keyframes if animated).", "TIMELINE_WRITE"),
                                       f("mf.layer.rename", "(id, name)", "Rename a layer.", "TIMELINE_WRITE"),
                                       f("mf.layer.duplicate", "(id) -> id", "Duplicate a layer.", "TIMELINE_WRITE"),
                                       f("mf.layer.remove", "(id)", "Delete a layer.", "TIMELINE_WRITE"),
                                       f("mf.layer.timing", "(id, in, out)", "Set in/out points.", "TIMELINE_WRITE"),
                                       f("mf.layer.parent", "(id, parentId|null)", "Parent a layer.", "TIMELINE_WRITE")})},
                {"keyframe", json::array({f("mf.keyframe.add", "(id, path, time, value?, interp?)", "Add a keyframe.", "TIMELINE_WRITE"),
                                          f("mf.keyframe.remove", "(id, path, time)", "Remove a keyframe.", "TIMELINE_WRITE"),
                                          f("mf.keyframe.interp", "(id, path, time, interp)", "Set interpolation (linear, hold, easeIn, bounce, spring ...).", "TIMELINE_WRITE")})},
                {"effect", json::array({f("mf.effect.add", "(id, type, params?) -> effectId", "Apply an effect.", "TIMELINE_WRITE"),
                                        f("mf.effect.list", "() -> [{type,name,category}]", "Available effects.", "")})},
                {"other", json::array({f("mf.shape.add", "(id, item)", "Add a shape item to a shape layer.", "TIMELINE_WRITE"),
                                       f("mf.text.set", "(id, content)", "Set text.", "TIMELINE_WRITE"),
                                       f("mf.text.preset", "(id, name, duration?)", "Apply a text animation preset.", "TIMELINE_WRITE"),
                                       f("mf.marker.add", "(time, title, opts?)", "Add a composition marker.", "TIMELINE_WRITE"),
                                       f("mf.caption.set", "(items)", "Replace captions from structured data.", "TIMELINE_WRITE"),
                                       f("mf.behavior.add", "(id, type, params?)", "Add a motion behavior.", "TIMELINE_WRITE"),
                                       f("mf.storage.get/set", "(key[, value])", "Script-local persistent storage.", "LOCAL_STORAGE"),
                                       f("mf.ui.alert", "(message)", "Show a message after the script finishes.", ""),
                                       f("mf.render.requestExport", "(preset)", "Queue an export job.", "RENDER")})},
                {"operations", opNames()}};
}

json exampleScripts() {
    return json::array(
        {{{"name", "Lower Third Builder (panel)"}, {"permissions", {"PROJECT_READ", "TIMELINE_WRITE"}},
          {"source", "// Declares a panel; fill it in and run again.\nmf.ui.panel({fields: [\n  {name: 'name', label: 'Name', type: 'text', default: 'Alex Rivera'},\n  {name: 'role', label: 'Role', type: 'text', default: 'Director'},\n  {name: 'color', label: 'Accent', type: 'color', default: [1, 0.42, 0.24, 1]},\n  {name: 'seconds', label: 'Duration', type: 'number', min: 1, max: 10, default: 4}\n]});\nif (!mf.args.name) return;\nvar c = mf.comp.active();\nvar bar = mf.layer.add('shape', {shape: 'rect', color: mf.args.color});\nmf.prop.set(bar, 'transform.position', [c.width * 0.3, c.height * 0.82, 0], mf.time);\nvar t1 = mf.layer.add('text', {text: mf.args.name, size: 64});\nmf.prop.set(t1, 'transform.position', [c.width * 0.3, c.height * 0.8, 0], mf.time);\nvar t2 = mf.layer.add('text', {text: mf.args.role, size: 36});\nmf.prop.set(t2, 'transform.position', [c.width * 0.3, c.height * 0.87, 0], mf.time);\n[bar, t1, t2].forEach(function(id){ mf.layer.timing(id, mf.time, mf.time + (mf.args.seconds || 4)); });\nmf.text.preset(t1, 'fadeUp', 0.6);\nmf.text.preset(t2, 'fadeUp', 0.8);\nmf.layer.select([t1]);\n"}},
         {{"name", "Stagger Selected Layers"}, {"permissions", {"PROJECT_READ", "TIMELINE_WRITE"}},
          {"source", "// Offsets each selected layer by 4 frames.\nvar sel = mf.selection;\nif (sel.length === 0) mf.ui.alert('Select layers first');\nfor (var i = 0; i < sel.length; i++) {\n  mf.op({op:'moveLayerTime', layers:[sel[i]], dt: i * 4 / mf.comp.active().fps});\n}\nmf.log('Staggered', sel.length, 'layers');\n"}},
         {{"name", "Title Generator"}, {"permissions", {"PROJECT_READ", "TIMELINE_WRITE"}},
          {"source", "var id = mf.layer.add('text', {text: mf.args.title || 'Hello', size: 140});\nmf.keyframe.add(id, 'transform.position', mf.time, [200, 540, 0]);\nmf.keyframe.add(id, 'transform.position', mf.time + 1, [960, 540, 0], 'easeOut');\nmf.effect.add(id, 'stylize.glow');\nmf.text.preset(id, 'fadeUp', 1);\nreturn id;\n"}},
         {{"name", "Markers Every Second"}, {"permissions", {"PROJECT_READ", "TIMELINE_WRITE"}},
          {"source", "var d = mf.comp.active().duration;\nfor (var t = 0; t < d; t++) mf.marker.add(t, 'Beat ' + (t + 1));\n"}},
         {{"name", "Rename by Type"}, {"permissions", {"PROJECT_READ", "TIMELINE_WRITE"}},
          {"source", "var n = {};\nmf.layer.all().forEach(function(l){ n[l.type] = (n[l.type]||0) + 1; mf.layer.rename(l.id, l.type + ' ' + n[l.type]); });\n"}},
         {{"name", "Captions From Data"}, {"permissions", {"TIMELINE_WRITE"}},
          {"source", "var lines = ['Welcome', 'to MOTIONFORGE', 'made on a phone'];\nmf.caption.set(lines.map(function(t, i){ return {start: i * 1.5, end: i * 1.5 + 1.4, text: t}; }));\n"}}});
}

}  // namespace mf
