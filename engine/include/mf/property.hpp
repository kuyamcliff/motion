// Animatable properties: static value, keyframes, interpolation, expressions.
//
// JSON shape of a property:
//   { "v": <value>,                     static value (used when no keyframes)
//     "k": [ {"t": sec, "v": <value>, "o": <interp>}, ... ],   keyframes (layer time)
//     "x": "expression source", "xe": true }                   optional expression
// Values: number | [x,y] | [x,y,z] | [r,g,b,a] | bool | string | path object
// Path: {"closed": bool, "v": [[x,y, inX,inY, outX,outY], ...]}  (tangents relative)
#pragma once
#include "common.hpp"
#include "easing.hpp"

namespace mf {

struct Value {
    std::vector<double> n;  // numeric components (paths: 6 per vertex)
    std::string s;
    enum class Kind { Number, Vector, Bool, String, Path } kind = Kind::Number;
    bool closed = false;

    double num(size_t i = 0, double def = 0) const { return i < n.size() ? n[i] : def; }
    Vec2 vec2(Vec2 def = {}) const { return n.size() >= 2 ? Vec2(n[0], n[1]) : (n.size() == 1 ? Vec2(n[0], n[0]) : def); }
    Vec3 vec3(Vec3 def = {}) const {
        if (n.size() >= 3) return {n[0], n[1], n[2]};
        if (n.size() == 2) return {n[0], n[1], def.z};
        return def;
    }
    Color color(Color def = Color(1, 1, 1, 1)) const {
        if (n.size() >= 3) return Color((float)n[0], (float)n[1], (float)n[2], n.size() > 3 ? (float)n[3] : 1.f);
        return def;
    }
    bool boolean() const { return !n.empty() && n[0] != 0; }

    static Value fromJson(const json& j);
    json toJson() const;
    static Value number(double v) { Value r; r.n = {v}; return r; }
    static Value vec(std::initializer_list<double> l) { Value r; r.kind = Kind::Vector; r.n = l; return r; }
};

Value interpolateValues(const Value& a, const Value& b, double u);

struct ExpressionEngine;
struct AudioFeatureSource;

struct EvalContext {
    const json* project = nullptr;
    const json* comp = nullptr;
    const json* layer = nullptr;
    double compTime = 0;   // composition time in seconds
    double layerTime = 0;  // time relative to layer start (keyframe space)
    double fps = 30;
    std::string propPath;  // e.g. "transform.position"
    ExpressionEngine* expr = nullptr;
    AudioFeatureSource* audio = nullptr;
    int depth = 0;         // recursion guard for expression cross references
};

struct Keyframe {
    double t = 0;
    Value v;
    Interp out;
};

// Parse keyframes from property JSON (sorted by time).
std::vector<Keyframe> parseKeyframes(const json& prop);
bool hasKeyframes(const json& prop);
bool hasExpression(const json& prop);

// Evaluate raw keyframe/static value at local time (no expression).
Value evalRaw(const json& prop, double localTime);
// Full evaluation including expression (if ctx->expr is set).
Value evalProperty(const json& prop, const EvalContext& ctx);

// Convenience accessors (property may be missing -> default).
double propNumber(const json& parent, const char* key, const EvalContext& ctx, double def);
Vec2 propVec2(const json& parent, const char* key, const EvalContext& ctx, Vec2 def);
Vec3 propVec3(const json& parent, const char* key, const EvalContext& ctx, Vec3 def);
Color propColor(const json& parent, const char* key, const EvalContext& ctx, Color def);
bool propBool(const json& parent, const char* key, const EvalContext& ctx, bool def);
std::string propString(const json& parent, const char* key, const EvalContext& ctx, const std::string& def);

// Velocity (units/sec) of numeric property component via central difference.
double propVelocity(const json& prop, double localTime, size_t component, double fps);

// Make a static property JSON from a value.
json makeProp(const json& v);

// Keyframe manipulation helpers operating on property JSON (return modified copy).
json propSetKeyframe(const json& prop, double t, const json& v, const json& interp = json(), double eps = 1e-6);
json propRemoveKeyframe(const json& prop, double t, double eps = 1e-6);
json propSetValue(const json& prop, double t, const json& v);  // keyframe if animated else static
json propReverseKeyframes(const json& prop);
json propScaleKeyframeTimes(const json& prop, double pivot, double factor);
json propDistributeKeyframes(const json& prop);
json propShiftKeyframes(const json& prop, double dt);
// Copy keyframes in [t0,t1] re-timed into [d0,d1].
json keyframesClipboard(const json& prop, double t0, double t1);
json pasteKeyframes(const json& prop, const json& clip, double d0, double d1);

}  // namespace mf
