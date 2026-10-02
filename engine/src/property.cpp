#include "mf/property.hpp"

#include "mf/expressions.hpp"

namespace mf {

Value Value::fromJson(const json& j) {
    Value r;
    if (j.is_number()) {
        r.kind = Kind::Number;
        r.n = {j.get<double>()};
    } else if (j.is_boolean()) {
        r.kind = Kind::Bool;
        r.n = {j.get<bool>() ? 1.0 : 0.0};
    } else if (j.is_string()) {
        r.kind = Kind::String;
        r.s = j.get<std::string>();
    } else if (j.is_array()) {
        r.kind = Kind::Vector;
        for (auto& e : j) r.n.push_back(e.is_number() ? e.get<double>() : 0.0);
    } else if (j.is_object() && j.contains("v") && j["v"].is_array()) {
        r.kind = Kind::Path;
        r.closed = j.value("closed", true);
        for (auto& vert : j["v"]) {
            for (int i = 0; i < 6; ++i) r.n.push_back(i < (int)vert.size() ? vert[i].get<double>() : 0.0);
        }
    }
    return r;
}

json Value::toJson() const {
    switch (kind) {
        case Kind::Number: return n.empty() ? json(0.0) : json(n[0]);
        case Kind::Bool: return !n.empty() && n[0] != 0;
        case Kind::String: return s;
        case Kind::Vector: return json(n);
        case Kind::Path: {
            json v = json::array();
            for (size_t i = 0; i + 5 < n.size(); i += 6) v.push_back({n[i], n[i + 1], n[i + 2], n[i + 3], n[i + 4], n[i + 5]});
            return {{"closed", closed}, {"v", v}};
        }
    }
    return nullptr;
}

Value interpolateValues(const Value& a, const Value& b, double u) {
    if (a.kind == Value::Kind::String || a.kind == Value::Kind::Bool) return u >= 1.0 ? b : a;
    if (a.n.size() != b.n.size()) return u >= 0.5 ? b : a;  // incompatible (e.g. path vertex count)
    Value r = a;
    for (size_t i = 0; i < r.n.size(); ++i) r.n[i] = a.n[i] + (b.n[i] - a.n[i]) * u;
    return r;
}

std::vector<Keyframe> parseKeyframes(const json& prop) {
    std::vector<Keyframe> out;
    if (!prop.is_object()) return out;
    auto it = prop.find("k");
    if (it == prop.end() || !it->is_array()) return out;
    out.reserve(it->size());
    for (auto& k : *it) {
        Keyframe kf;
        kf.t = k.value("t", 0.0);
        kf.v = Value::fromJson(k.contains("v") ? k["v"] : json(0.0));
        kf.out = Interp::fromJson(k.contains("o") ? k["o"] : json());
        out.push_back(std::move(kf));
    }
    std::stable_sort(out.begin(), out.end(), [](const Keyframe& a, const Keyframe& b) { return a.t < b.t; });
    return out;
}

bool hasKeyframes(const json& prop) {
    if (!prop.is_object()) return false;
    auto it = prop.find("k");
    return it != prop.end() && it->is_array() && !it->empty();
}

bool hasExpression(const json& prop) {
    if (!prop.is_object()) return false;
    auto it = prop.find("x");
    if (it == prop.end() || !it->is_string() || it->get<std::string>().empty()) return false;
    return prop.value("xe", true);
}

Value evalRaw(const json& prop, double t) {
    if (!prop.is_object()) return Value::fromJson(prop);  // bare value tolerated
    auto kit = prop.find("k");
    if (kit == prop.end() || !kit->is_array() || kit->empty()) {
        auto vit = prop.find("v");
        return vit == prop.end() ? Value() : Value::fromJson(*vit);
    }
    const json& ks = *kit;
    // Fast path: keyframes are stored sorted by commands; verify cheaply.
    size_t n = ks.size();
    auto kt = [&](size_t i) { return ks[i].value("t", 0.0); };
    bool sorted = true;
    for (size_t i = 1; i < n; ++i)
        if (kt(i) < kt(i - 1)) { sorted = false; break; }
    if (!sorted) {
        auto kfs = parseKeyframes(prop);
        if (t <= kfs.front().t) return kfs.front().v;
        if (t >= kfs.back().t) return kfs.back().v;
        for (size_t i = 0; i + 1 < kfs.size(); ++i) {
            if (t >= kfs[i].t && t < kfs[i + 1].t) {
                double span = kfs[i + 1].t - kfs[i].t;
                double u = span > 0 ? (t - kfs[i].t) / span : 1.0;
                return interpolateValues(kfs[i].v, kfs[i + 1].v, kfs[i].out.apply(u));
            }
        }
        return kfs.back().v;
    }
    if (t <= kt(0)) return Value::fromJson(ks[0]["v"]);
    if (t >= kt(n - 1)) return Value::fromJson(ks[n - 1]["v"]);
    size_t lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (kt(mid) <= t) lo = mid; else hi = mid;
    }
    double t0 = kt(lo), t1 = kt(hi);
    double u = t1 > t0 ? (t - t0) / (t1 - t0) : 1.0;
    Interp in = Interp::fromJson(ks[lo].contains("o") ? ks[lo]["o"] : json());
    return interpolateValues(Value::fromJson(ks[lo]["v"]), Value::fromJson(ks[hi]["v"]), in.apply(u));
}

Value evalProperty(const json& prop, const EvalContext& ctx) {
    Value v = evalRaw(prop, ctx.layerTime);
    if (ctx.expr && ctx.depth < 8 && hasExpression(prop)) {
        Value out;
        std::string err;
        if (ctx.expr->evaluate(prop["x"].get<std::string>(), v, prop, ctx, out, err)) {
            // Preserve kind/arity where the expression returned a compatible shape.
            if (v.kind == Value::Kind::Vector && out.kind == Value::Kind::Number && !out.n.empty()) {
                Value r = v;
                for (auto& c : r.n) c = out.n[0];
                return r;
            }
            return out;
        }
        // Errors are non-destructive: fall back to the keyframed value.
    }
    return v;
}

static const json* findKey(const json& parent, const char* key) {
    if (!parent.is_object()) return nullptr;
    auto it = parent.find(key);
    return it == parent.end() ? nullptr : &*it;
}

static EvalContext withPath(const EvalContext& ctx, const char* key) {
    EvalContext c = ctx;
    if (c.propPath.empty()) c.propPath = key;
    else c.propPath = c.propPath + "." + key;
    return c;
}

double propNumber(const json& parent, const char* key, const EvalContext& ctx, double def) {
    const json* p = findKey(parent, key);
    if (!p) return def;
    Value v = evalProperty(*p, withPath(ctx, key));
    return v.n.empty() ? def : v.n[0];
}
Vec2 propVec2(const json& parent, const char* key, const EvalContext& ctx, Vec2 def) {
    const json* p = findKey(parent, key);
    if (!p) return def;
    return evalProperty(*p, withPath(ctx, key)).vec2(def);
}
Vec3 propVec3(const json& parent, const char* key, const EvalContext& ctx, Vec3 def) {
    const json* p = findKey(parent, key);
    if (!p) return def;
    return evalProperty(*p, withPath(ctx, key)).vec3(def);
}
Color propColor(const json& parent, const char* key, const EvalContext& ctx, Color def) {
    const json* p = findKey(parent, key);
    if (!p) return def;
    return evalProperty(*p, withPath(ctx, key)).color(def);
}
bool propBool(const json& parent, const char* key, const EvalContext& ctx, bool def) {
    const json* p = findKey(parent, key);
    if (!p) return def;
    Value v = evalProperty(*p, withPath(ctx, key));
    return v.n.empty() ? def : v.n[0] != 0;
}
std::string propString(const json& parent, const char* key, const EvalContext& ctx, const std::string& def) {
    const json* p = findKey(parent, key);
    if (!p) return def;
    if (p->is_string()) return p->get<std::string>();
    Value v = evalProperty(*p, withPath(ctx, key));
    if (v.kind == Value::Kind::String) return v.s;
    if (!v.n.empty()) return formatString("%g", v.n[0]);
    return def;
}

double propVelocity(const json& prop, double t, size_t comp, double fps) {
    double h = 0.5 / std::max(1.0, fps);
    Value a = evalRaw(prop, t - h), b = evalRaw(prop, t + h);
    return (b.num(comp) - a.num(comp)) / (2 * h);
}

json makeProp(const json& v) { return json{{"v", v}}; }

static void sortKeys(json& ks) {
    std::vector<json> v(ks.begin(), ks.end());
    std::stable_sort(v.begin(), v.end(), [](const json& a, const json& b) { return a.value("t", 0.0) < b.value("t", 0.0); });
    ks = json(v);
}

json propSetKeyframe(const json& prop, double t, const json& v, const json& interp, double eps) {
    json p = prop.is_object() ? prop : makeProp(prop);
    if (!p.contains("k") || !p["k"].is_array()) p["k"] = json::array();
    for (auto& k : p["k"]) {
        if (std::fabs(k.value("t", 0.0) - t) < eps) {
            k["v"] = v;
            if (!interp.is_null()) k["o"] = interp;
            return p;
        }
    }
    json k = {{"t", t}, {"v", v}};
    if (!interp.is_null()) k["o"] = interp;
    p["k"].push_back(k);
    sortKeys(p["k"]);
    return p;
}

json propRemoveKeyframe(const json& prop, double t, double eps) {
    json p = prop;
    if (!p.is_object() || !p.contains("k")) return p;
    json out = json::array();
    json last;
    for (auto& k : p["k"]) {
        if (std::fabs(k.value("t", 0.0) - t) < eps) { last = k["v"]; continue; }
        out.push_back(k);
    }
    if (out.empty()) {
        p.erase("k");
        if (!last.is_null()) p["v"] = last;  // keep the value the user saw
    } else {
        p["k"] = out;
    }
    return p;
}

json propSetValue(const json& prop, double t, const json& v) {
    if (hasKeyframes(prop)) return propSetKeyframe(prop, t, v);
    json p = prop.is_object() ? prop : json::object();
    p["v"] = v;
    return p;
}

json propReverseKeyframes(const json& prop) {
    if (!hasKeyframes(prop)) return prop;
    json p = prop;
    auto& ks = p["k"];
    double t0 = ks.front().value("t", 0.0), t1 = ks.back().value("t", 0.0);
    std::vector<json> v(ks.begin(), ks.end());
    std::vector<json> out;
    for (size_t i = v.size(); i-- > 0;) {
        json k = v[i];
        k["t"] = t0 + (t1 - v[i].value("t", 0.0));
        // Interp of segment (i-1 -> i) becomes out-interp of reversed key i.
        if (i > 0 && v[i - 1].contains("o")) k["o"] = v[i - 1]["o"]; else k.erase("o");
        out.push_back(k);
    }
    ks = json(out);
    return p;
}

json propScaleKeyframeTimes(const json& prop, double pivot, double factor) {
    if (!hasKeyframes(prop)) return prop;
    json p = prop;
    for (auto& k : p["k"]) k["t"] = pivot + (k.value("t", 0.0) - pivot) * factor;
    sortKeys(p["k"]);
    return p;
}

json propDistributeKeyframes(const json& prop) {
    if (!hasKeyframes(prop) || prop["k"].size() < 3) return prop;
    json p = prop;
    auto& ks = p["k"];
    double t0 = ks.front().value("t", 0.0), t1 = ks.back().value("t", 0.0);
    size_t n = ks.size();
    for (size_t i = 0; i < n; ++i) ks[i]["t"] = t0 + (t1 - t0) * double(i) / double(n - 1);
    return p;
}

json propShiftKeyframes(const json& prop, double dt) {
    if (!hasKeyframes(prop)) return prop;
    json p = prop;
    for (auto& k : p["k"]) k["t"] = k.value("t", 0.0) + dt;
    return p;
}

json keyframesClipboard(const json& prop, double t0, double t1) {
    json clip = {{"t0", t0}, {"t1", t1}, {"k", json::array()}};
    if (!hasKeyframes(prop)) return clip;
    for (auto& k : prop["k"]) {
        double t = k.value("t", 0.0);
        if (t >= t0 - 1e-9 && t <= t1 + 1e-9) clip["k"].push_back(k);
    }
    return clip;
}

json pasteKeyframes(const json& prop, const json& clip, double d0, double d1) {
    json p = prop.is_object() ? prop : makeProp(prop);
    double t0 = clip.value("t0", 0.0), t1 = clip.value("t1", 0.0);
    double span = t1 - t0;
    for (auto& k : jarr(clip, "k")) {
        double t = k.value("t", 0.0);
        double u = span > 1e-12 ? (t - t0) / span : 0.0;
        double nt = d0 + u * (d1 - d0);
        p = propSetKeyframe(p, nt, k["v"], k.contains("o") ? k["o"] : json());
    }
    return p;
}

}  // namespace mf
