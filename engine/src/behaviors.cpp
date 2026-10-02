#include "mf/behaviors.hpp"

#include <map>

#include "mf/expressions.hpp"

namespace mf {

static double smoothNoise(double t, double freq, uint32_t seed, int octaves = 2) {
    double s = 0, amp = 1, f = freq, norm = 0;
    for (int i = 0; i < octaves; ++i) {
        s += amp * valueNoise1D(t * f, seed + i * 977);
        norm += amp;
        amp *= 0.5;
        f *= 2;
    }
    return s / norm;
}

static void applyOne(const json& b, const json& layer, const EvalContext& ctx, TransformOffsets& out) {
    const json& P = b["params"];
    auto num = [&](const char* k, double d) { return propNumber(P, k, ctx, d); };
    std::string en = P.contains("axis") ? propString(P, "axis", ctx, "both") : "both";
    std::string type = b.value("type", "");
    double t = ctx.compTime;
    double lt = t - layer.value("in", 0.0);  // time since layer appears
    uint32_t seed = (uint32_t)num("seed", 1) * 7919u + 13u;
    double gain = 1.0;
    if (b.contains("audioLink") && b["audioLink"].is_object() && ctx.audio && ctx.comp) {
        std::map<std::string, double> f;
        if (ctx.audio->features(ctx.comp, t, f)) {
            std::string feat = b["audioLink"].value("feature", "bass");
            gain = f.count(feat) ? f[feat] * b["audioLink"].value("amount", 1.0) : 0.0;
        }
    }
    if (type == "shake") {
        double amp = num("amplitude", 20) * gain, freq = num("frequency", 8), decay = num("decay", 0) / 100.0;
        double smoothing = num("smoothing", 50) / 100.0;
        double rnd = num("randomness", 100) / 100.0;
        double env = decay > 0 ? std::exp(-lt * decay * 4) : 1.0;
        int oct = smoothing > 0.5 ? 1 : 3;
        double x = smoothNoise(t, freq, seed, oct) * rnd + std::sin(t * freq * 2 * kPi) * (1 - rnd);
        double y = smoothNoise(t, freq, seed + 101, oct) * rnd + std::cos(t * freq * 2 * kPi * 1.3) * (1 - rnd);
        if (en == "y") x = 0;
        if (en == "x") y = 0;
        out.position += Vec3(x * amp * env, y * amp * env, 0);
    } else if (type == "handheld") {
        double amp = num("amplitude", 6) * gain, freq = num("frequency", 1.5);
        out.position += Vec3(smoothNoise(t, freq, seed, 3) * amp, smoothNoise(t, freq * 0.8, seed + 5, 3) * amp, 0);
        out.rotation += smoothNoise(t, freq * 0.6, seed + 11, 2) * num("rotation", 1.5) * gain;
    } else if (type == "bounce") {
        double h = num("height", 100) * gain, f = num("frequency", 2), decay = num("decay", 50) / 100.0;
        double phase = lt * f;
        double env = std::exp(-lt * decay * 3);
        double y = -std::fabs(std::sin(phase * kPi)) * h * env;
        out.position += Vec3(0, y, 0);
    } else if (type == "drift") {
        out.position += Vec3(num("vx", 20) * lt, num("vy", 0) * lt, 0);
    } else if (type == "float") {
        double a = num("amplitude", 15) * gain, f = num("frequency", 0.5);
        out.position += Vec3(std::sin(t * f * 2 * kPi * 0.7 + seed) * a * 0.4, std::sin(t * f * 2 * kPi) * a, 0);
    } else if (type == "orbit") {
        double r = num("radius", 100) * gain, s = num("speed", 0.5), ph = deg2rad(num("phase", 0));
        double a = lt * s * 2 * kPi + ph;
        out.position += Vec3(std::cos(a) * r, std::sin(a) * r, 0);
    } else if (type == "wiggle") {
        double amp = num("amplitude", 30) * gain, f = num("frequency", 3);
        std::string target = propString(P, "target", ctx, "position");
        double nx = smoothNoise(t, f, seed, 2), ny = smoothNoise(t, f, seed + 37, 2);
        if (target == "position") out.position += Vec3(nx * amp, ny * amp, 0);
        else if (target == "scale") { double m = 1 + nx * amp / 100.0; out.scaleMul = out.scaleMul * Vec3(m, m, 1); }
        else if (target == "rotation") out.rotation += nx * amp;
        else if (target == "opacity") out.opacityMul *= clampv(1 + nx * amp / 100.0, 0.0, 1.0);
    } else if (type == "jitter") {
        double amp = num("amplitude", 5) * gain, fps = std::max(1.0, num("fps", 12));
        uint32_t step = (uint32_t)std::floor(t * fps);
        out.position += Vec3((hashToUnit(step * 31 + seed) * 2 - 1) * amp, (hashToUnit(step * 57 + seed + 3) * 2 - 1) * amp, 0);
    } else if (type == "swing") {
        double ang = num("angle", 15) * gain, f = num("frequency", 1), decay = num("decay", 20) / 100.0;
        out.rotation += std::sin(lt * f * 2 * kPi) * ang * std::exp(-lt * decay * 2);
    } else if (type == "overshoot") {
        double amount = num("amount", 25) / 100.0, dur = std::max(0.05, num("duration", 0.6));
        double u = clampv(lt / dur, 0.0, 1.0);
        double c1 = 1.70158 * (amount / 0.1 * 0.6 + 0.4), c3 = c1 + 1;
        double v = u >= 1 ? 1.0 : 1 + c3 * std::pow(u - 1, 3) + c1 * std::pow(u - 1, 2);
        out.scaleMul = out.scaleMul * Vec3(v, v, 1);
    } else if (type == "elastic") {
        double amount = num("amount", 30) / 100.0, f = num("frequency", 3), dur = std::max(0.1, num("duration", 1.2));
        double u = clampv(lt / dur, 0.0, 1.0);
        double v = 1 + amount * std::exp(-u * 5) * std::sin(u * f * 2 * kPi) * (1 - u);
        out.scaleMul = out.scaleMul * Vec3(v, v, 1);
    } else if (type == "pulse") {
        double a = num("amount", 10) / 100.0 * gain, f = num("frequency", 2);
        double v = 1 + a * (0.5 + 0.5 * std::sin(t * f * 2 * kPi));
        out.scaleMul = out.scaleMul * Vec3(v, v, 1);
    } else if (type == "breathing") {
        double a = num("amount", 3) / 100.0, f = num("frequency", 0.25);
        double v = 1 + a * std::sin(t * f * 2 * kPi);
        out.scaleMul = out.scaleMul * Vec3(v, v, 1);
    } else if (type == "recoil") {
        double at = num("at", 0), dur = std::max(0.05, num("duration", 0.35)), d = num("distance", 40) * gain;
        double u = (t - at) / dur;
        if (u >= 0 && u <= 1) out.position += Vec3(0, 0, 0) + Vec3(0, -d * std::exp(-u * 4) * std::sin(u * kPi), 0);
    } else if (type == "impact") {
        double at = num("at", 0), dur = std::max(0.05, num("duration", 0.4)), a = num("amount", 30) / 100.0 * gain;
        double u = (t - at) / dur;
        if (u >= 0 && u <= 1) {
            double env = std::exp(-u * 5);
            double s = 1 + a * env;
            out.scaleMul = out.scaleMul * Vec3(s, s, 1);
            out.position += Vec3(smoothNoise(t, 25, seed) * a * 60 * env, smoothNoise(t, 25, seed + 9) * a * 60 * env, 0);
        }
    } else if (type == "whip") {
        double at = num("at", 0), dur = std::max(0.05, num("duration", 0.3)), d = num("distance", 600), ang = deg2rad(num("angle", 0));
        double u = clampv((t - at) / dur, 0.0, 1.0);
        double e = 1 - std::pow(1 - u, 4);
        double rem = (1 - e) * d;
        out.position += Vec3(-std::cos(ang) * rem, -std::sin(ang) * rem, 0);
    } else if (type == "followThrough") {
        // Lagged copy of keyframed position blended back with damped oscillation.
        double amount = num("amount", 50) / 100.0, lag = num("lag", 0.12);
        const json& pos = layer["transform"]["position"];
        if (hasKeyframes(pos)) {
            Value now = evalRaw(pos, ctx.layerTime), past = evalRaw(pos, ctx.layerTime - lag);
            out.position += Vec3((past.num(0) - now.num(0)) * amount, (past.num(1) - now.num(1)) * amount, 0);
        }
    }
}

TransformOffsets evalBehaviors(const json& layer, const EvalContext& ctx) {
    TransformOffsets out;
    auto it = layer.find("behaviors");
    if (it == layer.end() || !it->is_array()) return out;
    for (auto& b : *it) {
        if (!b.value("enabled", true) || !b.contains("params")) continue;
        applyOne(b, layer, ctx, out);
    }
    return out;
}

void bakeBehaviorToKeyframes(const json& comp, json& layer, int index, double fps) {
    json b = layer["behaviors"][index];
    json single = layer;
    single["behaviors"] = json::array({b});
    double in = layer.value("in", 0.0), out = layer.value("out", 0.0), start = layer.value("start", 0.0);
    json& T = layer["transform"];
    json pos = T["position"], scl = T["scale"], rot = T["rotation"], opa = T["opacity"];
    json npos = json{{"k", json::array()}}, nscl = json{{"k", json::array()}}, nrot = json{{"k", json::array()}}, nopa = json{{"k", json::array()}};
    bool usesPos = false, usesScale = false, usesRot = false, usesOpa = false;
    std::vector<std::tuple<double, TransformOffsets>> samples;
    for (double t = in; t < out + 1e-9; t += 1.0 / fps) {
        EvalContext ctx;
        ctx.comp = &comp;
        ctx.layer = &single;
        ctx.compTime = t;
        ctx.layerTime = t - start;
        ctx.fps = fps;
        TransformOffsets o = evalBehaviors(single, ctx);
        samples.push_back({t, o});
        if (o.position.length() > 1e-6) usesPos = true;
        if (std::fabs(o.scaleMul.x - 1) > 1e-6 || std::fabs(o.scaleMul.y - 1) > 1e-6) usesScale = true;
        if (std::fabs(o.rotation) > 1e-6) usesRot = true;
        if (std::fabs(o.opacityMul - 1) > 1e-6) usesOpa = true;
    }
    for (auto& [t, o] : samples) {
        double lt = t - start;
        if (usesPos) {
            Value p = evalRaw(pos, lt);
            npos["k"].push_back({{"t", lt}, {"v", {p.num(0) + o.position.x, p.num(1) + o.position.y, p.num(2) + o.position.z}}, {"o", "linear"}});
        }
        if (usesScale) {
            Value s = evalRaw(scl, lt);
            nscl["k"].push_back({{"t", lt}, {"v", {s.num(0) * o.scaleMul.x, s.num(1) * o.scaleMul.y, s.num(2, 100) * o.scaleMul.z}}, {"o", "linear"}});
        }
        if (usesRot) nrot["k"].push_back({{"t", lt}, {"v", evalRaw(rot, lt).num(0) + o.rotation}, {"o", "linear"}});
        if (usesOpa) nopa["k"].push_back({{"t", lt}, {"v", evalRaw(opa, lt).num(0) * o.opacityMul}, {"o", "linear"}});
    }
    if (usesPos) T["position"] = npos;
    if (usesScale) T["scale"] = nscl;
    if (usesRot) T["rotation"] = nrot;
    if (usesOpa) T["opacity"] = nopa;
    layer["behaviors"].erase(layer["behaviors"].begin() + index);
}

}  // namespace mf
