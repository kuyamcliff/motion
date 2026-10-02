#include "mf/scripting.hpp"

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
  app: { version: __mf_version, apiVersion: 1 },
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
    create: function(o){ o = o||{}; o.op = "addComp"; return __n_op(o).comp; },
    update: function(fields){ fields.op = "updateComp"; return __n_op(fields); }
  },
  layer: {
    add: function(kind, options, at){ var r = __n_op({op:"addLayer", kind:kind, options:options||{}, at: at===undefined?__mf_playhead:at}); return r.layer; },
    all: function(){ return __n_comp().layers; },
    get: function(id){ var ls = __n_comp().layers; for (var i=0;i<ls.length;i++) if (ls[i].id===id || ls[i].name===id) return ls[i]; return null; },
    find: function(q){ q = String(q).toLowerCase(); return __n_comp().layers.filter(function(l){ return l.name.toLowerCase().indexOf(q) >= 0; }).map(function(l){ return l.id; }); },
    set: function(id, path, value, time){ return __n_op({op:"setProp", layer:id, path:path, value:value, t: time===undefined?__mf_playhead:time}); },
    setStatic: function(id, path, value){ return __n_op({op:"setProp", layer:id, path:path, value:value, mode:"static"}); },
    rename: function(id, name){ return __n_op({op:"setLayer", layer:id, fields:{name:name}}); },
    fields: function(id, f){ return __n_op({op:"setLayer", layer:id, fields:f}); },
    duplicate: function(id){ return __n_op({op:"duplicateLayers", layers:[id]}).layers[0]; },
    remove: function(id){ return __n_op({op:"removeLayers", layers:[id]}); },
    timing: function(id, inT, outT){ return __n_op({op:"setLayerTiming", layer:id, "in":inT, out:outT}); },
    parent: function(id, parentId){ return __n_op({op:"parent", layer:id, parent:parentId, t:__mf_playhead}); }
  },
  keyframe: {
    add: function(id, path, time, value, interp){ var o = {op:"addKeyframe", layer:id, path:path, t:time}; if (value!==undefined) o.value=value; if (interp) o.interp=interp; return __n_op(o); },
    remove: function(id, path, time){ return __n_op({op:"removeKeyframe", layer:id, path:path, t:time}); },
    interp: function(id, path, time, interp){ return __n_op({op:"setKeyframeInterp", layer:id, path:path, t:time, interp:interp}); }
  },
  effect: {
    add: function(id, type, params){ return __n_op({op:"addEffect", layer:id, type:type, params:params||{}}).effect; },
    list: function(){ return __n_effects(); }
  },
  shape: { add: function(id, item){ return __n_op({op:"addShapeItem", layer:id, item:item}).item; } },
  text: {
    set: function(id, content){ return __n_op({op:"setText", layer:id, content:String(content)}); },
    preset: function(id, name, duration){ return __n_op({op:"textPreset", layer:id, preset:name, duration:duration||1, t:__mf_playhead}); }
  },
  marker: { add: function(time, title, opts){ var o = opts||{}; o.op="addMarker"; o.t=time; o.title=title||""; return __n_op(o).marker; } },
  caption: { set: function(items){ return __n_op({op:"setCaptions", items:items}); } },
  behavior: { add: function(id, type, params){ return __n_op({op:"addBehavior", layer:id, type:type, params:params||{}}).behavior; } },
  storage: { get: function(k){ return __n_storageGet(k); }, set: function(k, v){ return __n_storageSet(k, v); } },
  ui: { alert: function(m){ __n_alert(String(m)); } },
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

json scriptApiDescription() {
    auto f = [](const char* name, const char* sig, const char* doc, const char* perm) { return json{{"name", name}, {"signature", sig}, {"doc", doc}, {"permission", perm}}; };
    return json{{"apiVersion", 1},
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
        {{{"name", "Stagger Selected Layers"}, {"permissions", {"PROJECT_READ", "TIMELINE_WRITE"}},
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
