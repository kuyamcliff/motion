#include "mf/expressions.hpp"

#include <chrono>

#include "mf/model.hpp"
#include "quickjs.h"

namespace mf {

// JS helpers shared by expressions and scripts.
extern const char* kJsVectorPrelude;
const char* kJsVectorPrelude = R"JS(
function __isA(x){ return Array.isArray(x); }
function __bin(a,b,f){
  if (__isA(a) && __isA(b)) { var n=Math.max(a.length,b.length), r=new Array(n); for (var i=0;i<n;i++) r[i]=f(i<a.length?a[i]:0, i<b.length?b[i]:0); return r; }
  if (__isA(a)) return a.map(function(x){ return f(x,b); });
  if (__isA(b)) return b.map(function(x){ return f(a,x); });
  return f(a,b);
}
function __add(a,b){ if (typeof a==="string"||typeof b==="string") return a+b; return __bin(a,b,function(x,y){return x+y;}); }
function __sub(a,b){ return __bin(a,b,function(x,y){return x-y;}); }
function __mul(a,b){ return __bin(a,b,function(x,y){return x*y;}); }
function __div(a,b){ return __bin(a,b,function(x,y){return x/y;}); }
function __neg(a){ return __isA(a) ? a.map(function(x){return -x;}) : -a; }
function add(a,b){return __add(a,b);} function sub(a,b){return __sub(a,b);} function mul(a,b){return __mul(a,b);} function div(a,b){return __div(a,b);}
function dot(a,b){ var s=0; for (var i=0;i<Math.min(a.length,b.length);i++) s+=a[i]*b[i]; return s; }
function cross(a,b){ return [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]]; }
function length(a,b){ if (b!==undefined) a=__sub(a,b); if (!__isA(a)) return Math.abs(a); return Math.sqrt(dot(a,a)); }
function normalize(a){ var l=length(a); return l>0? __div(a,l) : a; }
function clamp(v,a,b){ if (__isA(v)) return v.map(function(x,i){ return clamp(x, __isA(a)?a[i]:a, __isA(b)?b[i]:b); }); return Math.min(Math.max(v,a),b); }
function degreesToRadians(d){ return d*Math.PI/180; }
function radiansToDegrees(r){ return r*180/Math.PI; }
function __lerp(a,b,u){ return __add(a, __mul(__sub(b,a), u)); }
function __map(t,tMin,tMax,v1,v2,f,n){ if (n===3){ v1=tMin; v2=tMax; tMin=0; tMax=1; } var u = tMax===tMin?1:(t-tMin)/(tMax-tMin); u=Math.min(1,Math.max(0,u)); return __lerp(v1,v2,f(u)); }
function linear(t,a,b,c,d){ return __map(t,a,b,c,d,function(u){return u;},arguments.length); }
function ease(t,a,b,c,d){ return __map(t,a,b,c,d,function(u){return u*u*(3-2*u);},arguments.length); }
function easeIn(t,a,b,c,d){ return __map(t,a,b,c,d,function(u){return u*u;},arguments.length); }
function easeOut(t,a,b,c,d){ return __map(t,a,b,c,d,function(u){return 1-(1-u)*(1-u);},arguments.length); }
)JS";

namespace {

const char* kExprPrelude = R"JS(
var __seed = 0, __timeless = false, __rng = 1;
function seedRandom(s, timeless){ __seed = s|0; __timeless = !!timeless; __rngInit(); }
function __rngInit(){ __rng = (Math.imul((__seed+1)|0, 2654435761) ^ Math.imul((index+1)|0, 40503) ^ (__timeless?0:Math.imul((frame+1)|0, 9973))) >>> 0; if (__rng===0) __rng=1; }
function __rand(){ __rng ^= __rng << 13; __rng >>>= 0; __rng ^= __rng >>> 17; __rng ^= __rng << 5; __rng >>>= 0; return __rng / 4294967296; }
function random(a,b){ if (a===undefined) return __rand(); if (b===undefined){ if (__isA(a)) return a.map(function(x){return __rand()*x;}); return __rand()*a; } if (__isA(a)) return a.map(function(x,i){return x+__rand()*(b[i]-x);}); return a+__rand()*(b-a); }
function gaussRandom(a,b){ var u=__rand()||1e-9, v=__rand(); var g=Math.sqrt(-2*Math.log(u))*Math.cos(2*Math.PI*v)*0.25+0.5; return random(a,b)*0 + (a===undefined?g:(b===undefined?g*a:a+g*(b-a))); }
function noise(x){ return __mf_noise(__isA(x)?x[0]+x[1]*57.1:x, __seed); }
function wiggle(freq, amp, octaves, ampMult, t){
  t = (t===undefined)?time:t; octaves = octaves||1; ampMult = (ampMult===undefined)?0.5:ampMult;
  function w(dim){ var s=0,a=1,f=freq,n=0; for (var o=0;o<octaves;o++){ s+=a*__mf_noise(t*f+dim*17.13, __seed+index*7+o*131); n+=a; a*=ampMult; f*=2; } return s/n*amp; }
  if (__isA(value)) return value.map(function(x,i){ return x+w(i); });
  return value + w(0);
}
function valueAtTime(t){ return __mf_valueAtTime(t); }
function velocityAtTime(t){ var h=0.5/fps; return __div(__sub(valueAtTime(t+h), valueAtTime(t-h)), 2*h); }
function speedAtTime(t){ return length(velocityAtTime(t)); }
function __keys(){ return __mf_keyTimes(); }
function loopOut(type, n){
  type = type||"cycle"; n = n||0; var k=__keys(); if (k.length<2) return value;
  var t1=k[k.length-1], t0=(n>0&&n<k.length)?k[k.length-1-n]:k[0], lt=time-__layerStart; if (lt<=t1) return value;
  var d=t1-t0; if (d<=0) return value; var u=lt-t1, c=Math.floor(u/d), r=u-c*d;
  if (type==="cycle") return valueAtTime(__layerStart+t0+r);
  if (type==="pingpong") return valueAtTime(__layerStart+(c%2===0? t1-r : t0+r));
  if (type==="offset") { var delta=__sub(valueAtTime(__layerStart+t1), valueAtTime(__layerStart+t0)); return __add(valueAtTime(__layerStart+t0+r), __mul(delta, c+1)); }
  if (type==="continue") { var v=velocityAtTime(__layerStart+t1-0.5/fps); return __add(valueAtTime(__layerStart+t1), __mul(v, u)); }
  return value;
}
function loopIn(type, n){
  type = type||"cycle"; n = n||0; var k=__keys(); if (k.length<2) return value;
  var t0=k[0], t1=(n>0&&n<k.length)?k[n]:k[k.length-1], lt=time-__layerStart; if (lt>=t0) return value;
  var d=t1-t0; if (d<=0) return value; var u=t0-lt, c=Math.floor(u/d), r=u-c*d;
  if (type==="cycle") return valueAtTime(__layerStart+t1-r);
  if (type==="pingpong") return valueAtTime(__layerStart+(c%2===0? t0+r : t1-r));
  if (type==="offset") { var delta=__sub(valueAtTime(__layerStart+t1), valueAtTime(__layerStart+t0)); return __sub(valueAtTime(__layerStart+t1-r), __mul(delta, c+1)); }
  if (type==="continue") { var v=velocityAtTime(__layerStart+t0+0.5/fps); return __sub(valueAtTime(__layerStart+t0), __mul(v, u)); }
  return value;
}
function loopOutDuration(type, d){ return loopOut(type, 0); }
function pingPong(n){ return loopOut("pingpong", n); }
function smooth(width, samples, t){ width=width||0.2; samples=samples||5; t=(t===undefined)?time:t; var acc=null; for (var i=0;i<samples;i++){ var v=valueAtTime(t-width/2+width*i/Math.max(1,samples-1)); acc=(acc===null)?v:__add(acc,v);} return __div(acc,samples); }
function __layerObj(ref){
  var o = { prop: function(p, t){ return __mf_prop(ref, p, t===undefined?time:t); } };
  o.transform = {};
  ["position","scale","rotation","opacity","anchorPoint","rotationX","rotationY"].forEach(function(n){
    var path = "transform." + (n==="anchorPoint"?"anchor":n);
    Object.defineProperty(o.transform, n, { get: function(){ return __mf_prop(ref, path, time); } });
    Object.defineProperty(o, n, { get: function(){ return __mf_prop(ref, path, time); } });
  });
  o.effect = function(name){ return function(param){ return __mf_prop(ref, "effects."+name+".params."+param, time); }; };
  o.text = { get sourceText(){ return __mf_prop(ref, "text.content", time); } };
  Object.defineProperty(o, "name", { get: function(){ return __mf_layerInfo(ref, "name"); } });
  Object.defineProperty(o, "inPoint", { get: function(){ return __mf_layerInfo(ref, "in"); } });
  Object.defineProperty(o, "outPoint", { get: function(){ return __mf_layerInfo(ref, "out"); } });
  Object.defineProperty(o, "startTime", { get: function(){ return __mf_layerInfo(ref, "start"); } });
  Object.defineProperty(o, "index", { get: function(){ return __mf_layerInfo(ref, "index"); } });
  return o;
}
var thisLayer = __layerObj(null);
var thisComp = {
  layer: function(r){ return __layerObj(r); },
  get width(){ return __mf_compInfo("width"); }, get height(){ return __mf_compInfo("height"); },
  get duration(){ return __mf_compInfo("duration"); }, get frameDuration(){ return 1/fps; },
  get numLayers(){ return __mf_compInfo("numLayers"); },
  marker: { get numKeys(){ return __mf_markers().length; }, key: function(i){ return __mf_markers()[i-1]; } }
};
var audio = {};
["amplitude","rms","bass","mid","treble","beat","tempo","centroid"].forEach(function(n){ Object.defineProperty(audio, n, { get: function(){ return __mf_audio(n); } }); });
function transform(){ return thisLayer.transform; }
)JS";

struct Current {
    const json* prop = nullptr;
    const EvalContext* ctx = nullptr;
    ExpressionEngine* eng = nullptr;
};
thread_local Current g_cur;

JSValue valueToJs(JSContext* c, const Value& v) {
    switch (v.kind) {
        case Value::Kind::Number: return JS_NewFloat64(c, v.n.empty() ? 0 : v.n[0]);
        case Value::Kind::Bool: return JS_NewBool(c, !v.n.empty() && v.n[0] != 0);
        case Value::Kind::String: return JS_NewString(c, v.s.c_str());
        default: {
            JSValue a = JS_NewArray(c);
            for (size_t i = 0; i < v.n.size(); ++i) JS_SetPropertyUint32(c, a, (uint32_t)i, JS_NewFloat64(c, v.n[i]));
            return a;
        }
    }
}

bool jsToValue(JSContext* c, JSValueConst j, const Value& like, Value& out) {
    if (JS_IsNumber(j)) {
        double d;
        JS_ToFloat64(c, &d, j);
        out = Value::number(d);
        if (like.kind == Value::Kind::Bool) out.kind = Value::Kind::Bool;
        return std::isfinite(d);
    }
    if (JS_IsBool(j)) {
        out = Value::number(JS_ToBool(c, j) ? 1 : 0);
        out.kind = Value::Kind::Bool;
        return true;
    }
    if (JS_IsString(j)) {
        const char* s = JS_ToCString(c, j);
        out = Value();
        out.kind = Value::Kind::String;
        out.s = s ? s : "";
        JS_FreeCString(c, s);
        return true;
    }
    if (JS_IsArray(j)) {
        int64_t n = 0;
        JS_GetLength(c, j, &n);
        out = Value();
        out.kind = like.kind == Value::Kind::Path ? Value::Kind::Path : Value::Kind::Vector;
        out.closed = like.closed;
        for (int64_t i = 0; i < n && i < 4096; ++i) {
            JSValue e = JS_GetPropertyUint32(c, j, (uint32_t)i);
            double d = 0;
            JS_ToFloat64(c, &d, e);
            JS_FreeValue(c, e);
            if (!std::isfinite(d)) return false;
            out.n.push_back(d);
        }
        return true;
    }
    return false;
}

const json* layerByRef(const json& comp, JSContext* c, JSValueConst ref) {
    if (JS_IsNull(ref) || JS_IsUndefined(ref)) return g_cur.ctx ? g_cur.ctx->layer : nullptr;
    const json& layers = jarr(comp, "layers");
    if (JS_IsNumber(ref)) {
        double d;
        JS_ToFloat64(c, &d, ref);
        int idx = (int)d - 1;  // 1-based like AE
        if (idx >= 0 && idx < (int)layers.size()) return &layers[idx];
        return nullptr;
    }
    const char* s = JS_ToCString(c, ref);
    std::string name = s ? s : "";
    JS_FreeCString(c, s);
    for (auto& L : layers)
        if (L.value("name", "") == name || L.value("id", "") == name) return &L;
    return nullptr;
}

JSValue js_noise(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    double x = 0, seed = 0;
    if (argc > 0) JS_ToFloat64(c, &x, argv[0]);
    if (argc > 1) JS_ToFloat64(c, &seed, argv[1]);
    return JS_NewFloat64(c, valueNoise1D(x, (uint32_t)(int64_t)seed));
}

JSValue js_valueAtTime(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    if (!g_cur.prop || !g_cur.ctx) return JS_UNDEFINED;
    double t = g_cur.ctx->compTime;
    if (argc > 0) JS_ToFloat64(c, &t, argv[0]);
    double start = g_cur.ctx->layer ? g_cur.ctx->layer->value("start", 0.0) : 0;
    return valueToJs(c, evalRaw(*g_cur.prop, t - start));
}

JSValue js_keyTimes(JSContext* c, JSValueConst, int, JSValueConst*) {
    JSValue a = JS_NewArray(c);
    if (!g_cur.prop) return a;
    auto ks = parseKeyframes(*g_cur.prop);
    for (size_t i = 0; i < ks.size(); ++i) JS_SetPropertyUint32(c, a, (uint32_t)i, JS_NewFloat64(c, ks[i].t));
    return a;
}

JSValue js_prop(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    if (!g_cur.ctx || !g_cur.ctx->comp || argc < 2) return JS_UNDEFINED;
    const json* L = layerByRef(*g_cur.ctx->comp, c, argv[0]);
    if (!L) return JS_ThrowReferenceError(c, "layer not found");
    const char* ps = JS_ToCString(c, argv[1]);
    std::string path = ps ? ps : "";
    JS_FreeCString(c, ps);
    const json* P = resolvePath(*L, path);
    if (!P) return JS_ThrowReferenceError(c, "property '%s' not found", path.c_str());
    double t = g_cur.ctx->compTime;
    if (argc > 2 && JS_IsNumber(argv[2])) JS_ToFloat64(c, &t, argv[2]);
    EvalContext sub = *g_cur.ctx;
    sub.layer = L;
    sub.compTime = t;
    sub.layerTime = t - L->value("start", 0.0);
    sub.depth = g_cur.ctx->depth + 1;
    sub.propPath = path;
    Current saved = g_cur;
    Value v = (P->is_object() && (P->contains("v") || P->contains("k"))) ? evalProperty(*P, sub) : Value::fromJson(*P);
    g_cur = saved;
    return valueToJs(c, v);
}

JSValue js_layerInfo(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    if (!g_cur.ctx || !g_cur.ctx->comp || argc < 2) return JS_UNDEFINED;
    const json* L = layerByRef(*g_cur.ctx->comp, c, argv[0]);
    if (!L) return JS_UNDEFINED;
    const char* ks = JS_ToCString(c, argv[1]);
    std::string k = ks ? ks : "";
    JS_FreeCString(c, ks);
    if (k == "name") return JS_NewString(c, L->value("name", "").c_str());
    if (k == "index") return JS_NewInt32(c, layerIndex(*g_cur.ctx->comp, L->value("id", "")) + 1);
    return JS_NewFloat64(c, L->value(k, 0.0));
}

JSValue js_compInfo(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    if (!g_cur.ctx || !g_cur.ctx->comp || argc < 1) return JS_UNDEFINED;
    const char* ks = JS_ToCString(c, argv[0]);
    std::string k = ks ? ks : "";
    JS_FreeCString(c, ks);
    const json& comp = *g_cur.ctx->comp;
    if (k == "numLayers") return JS_NewInt32(c, (int)jarr(comp, "layers").size());
    return JS_NewFloat64(c, comp.value(k, 0.0));
}

JSValue js_markers(JSContext* c, JSValueConst, int, JSValueConst*) {
    JSValue a = JS_NewArray(c);
    if (!g_cur.ctx || !g_cur.ctx->comp) return a;
    uint32_t i = 0;
    for (auto& m : jarr(*g_cur.ctx->comp, "markers")) {
        JSValue o = JS_NewObject(c);
        JS_SetPropertyStr(c, o, "time", JS_NewFloat64(c, m.value("t", 0.0)));
        JS_SetPropertyStr(c, o, "comment", JS_NewString(c, m.value("title", "").c_str()));
        JS_SetPropertyStr(c, o, "duration", JS_NewFloat64(c, m.value("duration", 0.0)));
        JS_SetPropertyUint32(c, a, i++, o);
    }
    return a;
}

JSValue js_audio(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    if (!g_cur.ctx || !g_cur.ctx->audio || argc < 1) return JS_NewFloat64(c, 0);
    const char* ks = JS_ToCString(c, argv[0]);
    std::string k = ks ? ks : "";
    JS_FreeCString(c, ks);
    std::map<std::string, double> f;
    if (!g_cur.ctx->audio->features(g_cur.ctx->comp, g_cur.ctx->compTime, f)) return JS_NewFloat64(c, 0);
    return JS_NewFloat64(c, f.count(k) ? f[k] : 0.0);
}

struct Deadline {
    std::chrono::steady_clock::time_point until;
};
int interruptHandler(JSRuntime*, void* opaque) {
    auto* d = (Deadline*)opaque;
    return std::chrono::steady_clock::now() > d->until ? 1 : 0;
}

std::string exceptionText(JSContext* c) {
    JSValue ex = JS_GetException(c);
    const char* s = JS_ToCString(c, ex);
    std::string msg = s ? s : "error";
    JS_FreeCString(c, s);
    JSValue stack = JS_GetPropertyStr(c, ex, "stack");
    if (JS_IsString(stack)) {
        const char* st = JS_ToCString(c, stack);
        std::string sts = st ? st : "";
        JS_FreeCString(c, st);
        auto pos = sts.find("<expression>:");
        if (pos != std::string::npos) {
            size_t e = sts.find_first_not_of("0123456789", pos + 13);
            msg = "Line " + sts.substr(pos + 13, e - pos - 13) + ": " + msg;
        }
    }
    JS_FreeValue(c, stack);
    JS_FreeValue(c, ex);
    if (msg.find("is not defined") != std::string::npos || msg.find("undefined") != std::string::npos)
        msg += " (check spelling; helpers: wiggle, loopOut, valueAtTime, linear, ease, thisComp.layer(\"name\"))";
    return msg;
}

class QjsEngine : public ExpressionEngine {
   public:
    QjsEngine() {
        rt_ = JS_NewRuntime();
        JS_SetMemoryLimit(rt_, 32u << 20);
        JS_SetMaxStackSize(rt_, 512u << 10);
        JS_SetInterruptHandler(rt_, interruptHandler, &deadline_);
        ctx_ = JS_NewContext(rt_);
        JSValue g = JS_GetGlobalObject(ctx_);
        auto fn = [&](const char* name, JSCFunction* f, int n) { JS_SetPropertyStr(ctx_, g, name, JS_NewCFunction(ctx_, f, name, n)); };
        fn("__mf_noise", js_noise, 2);
        fn("__mf_valueAtTime", js_valueAtTime, 1);
        fn("__mf_keyTimes", js_keyTimes, 0);
        fn("__mf_prop", js_prop, 3);
        fn("__mf_layerInfo", js_layerInfo, 2);
        fn("__mf_compInfo", js_compInfo, 1);
        fn("__mf_markers", js_markers, 0);
        fn("__mf_audio", js_audio, 1);
        for (const char* k : {"value", "time", "frame", "fps", "index", "__layerStart"}) JS_SetPropertyStr(ctx_, g, k, JS_NewFloat64(ctx_, 0));
        JS_FreeValue(ctx_, g);
        deadline_.until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (const char* src : {kJsVectorPrelude, kExprPrelude}) {
            JSValue r = JS_Eval(ctx_, src, std::strlen(src), "<prelude>", JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(r)) MF_LOGE("expression prelude failed: " + exceptionText(ctx_));
            JS_FreeValue(ctx_, r);
        }
    }
    ~QjsEngine() override {
        for (auto& [k, v] : compiled_) JS_FreeValue(ctx_, v);
        JS_FreeContext(ctx_);
        JS_FreeRuntime(rt_);
    }

    JSValue compile(const std::string& src, std::string& err) {
        auto it = compiled_.find(src);
        if (it != compiled_.end()) return JS_DupValue(ctx_, it->second);
        std::string code;
        if (!rewriteVectorOperators(src, code)) code = src;
        deadline_.until = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        JSValue f = JS_Eval(ctx_, code.c_str(), code.size(), "<expression>", JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(f)) {
            err = exceptionText(ctx_);
            return JS_EXCEPTION;
        }
        if (compiled_.size() > 512) {
            for (auto& [k, v] : compiled_) JS_FreeValue(ctx_, v);
            compiled_.clear();
        }
        compiled_[src] = JS_DupValue(ctx_, f);
        return f;
    }

    bool validate(const std::string& src, std::string& err) override {
        JSValue f = compile(src, err);
        if (JS_IsException(f)) return false;
        JS_FreeValue(ctx_, f);
        return true;
    }

    bool evaluate(const std::string& src, const Value& value, const json& prop, const EvalContext& ctx, Value& out, std::string& err) override {
        std::string key = (ctx.layer ? ctx.layer->value("id", std::string()) : std::string()) + ":" + ctx.propPath;
        Current saved = g_cur;
        g_cur.prop = &prop;
        g_cur.ctx = &ctx;
        g_cur.eng = this;
        bool ok = false;
        JSValue f = compile(src, err);
        if (!JS_IsException(f)) {
            JSValue g = JS_GetGlobalObject(ctx_);
            JS_SetPropertyStr(ctx_, g, "value", valueToJs(ctx_, value));
            JS_SetPropertyStr(ctx_, g, "time", JS_NewFloat64(ctx_, ctx.compTime));
            JS_SetPropertyStr(ctx_, g, "frame", JS_NewFloat64(ctx_, std::floor(ctx.compTime * ctx.fps + 1e-6)));
            JS_SetPropertyStr(ctx_, g, "fps", JS_NewFloat64(ctx_, ctx.fps));
            double idx = ctx.layer && ctx.comp ? layerIndex(*ctx.comp, ctx.layer->value("id", "")) + 1 : 1;
            JS_SetPropertyStr(ctx_, g, "index", JS_NewFloat64(ctx_, idx));
            JS_SetPropertyStr(ctx_, g, "__layerStart", JS_NewFloat64(ctx_, ctx.layer ? ctx.layer->value("start", 0.0) : 0.0));
            JS_SetPropertyStr(ctx_, g, "__seed", JS_NewInt32(ctx_, 0));
            JS_SetPropertyStr(ctx_, g, "__timeless", JS_NewBool(ctx_, false));
            JSValue init = JS_GetPropertyStr(ctx_, g, "__rngInit");
            JS_FreeValue(ctx_, JS_Call(ctx_, init, g, 0, nullptr));
            JS_FreeValue(ctx_, init);
            JS_FreeValue(ctx_, g);
            deadline_.until = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            JSValue r = JS_EvalFunction(ctx_, f);  // consumes f
            if (JS_IsException(r)) {
                err = exceptionText(ctx_);
                if (err.find("interrupted") != std::string::npos) err = "Expression took too long (over 100 ms) and was stopped.";
            } else if (!jsToValue(ctx_, r, value, out)) {
                err = "Expression result is not a number, array, boolean or string.";
            } else {
                ok = true;
            }
            JS_FreeValue(ctx_, r);
        }
        g_cur = saved;
        recordError(key, ok ? std::string() : err);
        return ok;
    }

   private:
    JSRuntime* rt_ = nullptr;
    JSContext* ctx_ = nullptr;
    Deadline deadline_;
    std::map<std::string, JSValue> compiled_;
};

}  // namespace

std::unique_ptr<ExpressionEngine> createExpressionEngine() { return std::make_unique<QjsEngine>(); }

}  // namespace mf
