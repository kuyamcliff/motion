#include "mf/easing.hpp"

namespace mf {

double cubicBezierEase(double x1, double y1, double x2, double y2, double x) {
    x1 = clampv(x1, 0.0, 1.0);
    x2 = clampv(x2, 0.0, 1.0);
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    auto bx = [&](double t) { double mt = 1 - t; return 3 * mt * mt * t * x1 + 3 * mt * t * t * x2 + t * t * t; };
    auto by = [&](double t) { double mt = 1 - t; return 3 * mt * mt * t * y1 + 3 * mt * t * t * y2 + t * t * t; };
    auto dbx = [&](double t) {
        double mt = 1 - t;
        return 3 * mt * mt * x1 + 6 * mt * t * (x2 - x1) + 3 * t * t * (1 - x2);
    };
    // Newton iterations, fall back to bisection.
    double t = x;
    for (int i = 0; i < 8; ++i) {
        double err = bx(t) - x;
        if (std::fabs(err) < 1e-9) return by(t);
        double d = dbx(t);
        if (std::fabs(d) < 1e-9) break;
        t -= err / d;
        if (t < 0 || t > 1) break;
    }
    double lo = 0, hi = 1;
    t = x;
    for (int i = 0; i < 60; ++i) {
        double v = bx(t);
        if (std::fabs(v - x) < 1e-10) break;
        if (v < x) lo = t; else hi = t;
        t = 0.5 * (lo + hi);
    }
    return by(t);
}

static double bounceOut(double u) {
    const double n1 = 7.5625, d1 = 2.75;
    if (u < 1 / d1) return n1 * u * u;
    if (u < 2 / d1) { u -= 1.5 / d1; return n1 * u * u + 0.75; }
    if (u < 2.5 / d1) { u -= 2.25 / d1; return n1 * u * u + 0.9375; }
    u -= 2.625 / d1;
    return n1 * u * u + 0.984375;
}

static double familyIn(EaseType t, double u, double amount, double freq) {
    switch (t) {
        case EaseType::Sine: return 1 - std::cos(u * kPi / 2);
        case EaseType::Quad: return u * u;
        case EaseType::Cubic: return u * u * u;
        case EaseType::Expo: return u <= 0 ? 0 : std::pow(2, 10 * u - 10);
        case EaseType::Circ: return 1 - std::sqrt(std::max(0.0, 1 - u * u));
        case EaseType::Back: {
            double c1 = 1.70158 * amount, c3 = c1 + 1;
            return c3 * u * u * u - c1 * u * u;
        }
        case EaseType::Bounce: return 1 - bounceOut(1 - u);
        case EaseType::Elastic: {
            if (u <= 0) return 0;
            if (u >= 1) return 1;
            double c4 = (2 * kPi) / 3;
            return -std::pow(2, 10 * u - 10) * std::sin((u * 10 - 10.75) * c4 * (freq / 3.0)) * amount;
        }
        default: return u;
    }
}

double easeFamily(EaseType t, EaseMode m, double u, double amount, double frequency) {
    u = clampv(u, 0.0, 1.0);
    if (t == EaseType::Spring || t == EaseType::Damped) {
        // Physically-inspired: critically under-damped oscillator settling at 1.
        double zeta = t == EaseType::Spring ? 0.25 / std::max(0.05, amount) : 0.6 / std::max(0.05, amount);
        zeta = clampv(zeta, 0.05, 0.95);
        double w = frequency * 2 * kPi / 2.0;
        double wd = w * std::sqrt(1 - zeta * zeta);
        double env = std::exp(-zeta * w * u * 2.0);
        double v = 1 - env * (std::cos(wd * u * 2.0) + (zeta * w / wd) * std::sin(wd * u * 2.0));
        if (u >= 1) return 1;
        // Blend into exact 1 at u=1 to guarantee continuity with next segment.
        double blend = smoothstep(0.85, 1.0, u);
        return v * (1 - blend) + blend;
    }
    switch (m) {
        case EaseMode::In: return familyIn(t, u, amount, frequency);
        case EaseMode::Out: return 1 - familyIn(t, 1 - u, amount, frequency);
        case EaseMode::InOut:
        default:
            if (u < 0.5) return familyIn(t, u * 2, amount, frequency) * 0.5;
            return 1 - familyIn(t, (1 - u) * 2, amount, frequency) * 0.5;
    }
}

double Interp::apply(double u) const {
    u = clampv(u, 0.0, 1.0);
    switch (type) {
        case EaseType::Hold: return u >= 1.0 ? 1.0 : 0.0;
        case EaseType::Linear: return u;
        case EaseType::Bezier: return cubicBezierEase(p[0], p[1], p[2], p[3], u);
        default: return easeFamily(type, mode, u, amount, frequency);
    }
}

static const char* typeName(EaseType t) {
    switch (t) {
        case EaseType::Hold: return "hold";
        case EaseType::Linear: return "linear";
        case EaseType::Bezier: return "bezier";
        case EaseType::Sine: return "sine";
        case EaseType::Quad: return "quad";
        case EaseType::Cubic: return "cubic";
        case EaseType::Expo: return "expo";
        case EaseType::Circ: return "circular";
        case EaseType::Back: return "back";
        case EaseType::Bounce: return "bounce";
        case EaseType::Elastic: return "elastic";
        case EaseType::Spring: return "spring";
        case EaseType::Damped: return "damped";
    }
    return "linear";
}

static EaseType typeFromName(const std::string& s, bool& ok) {
    ok = true;
    if (s == "hold") return EaseType::Hold;
    if (s == "linear") return EaseType::Linear;
    if (s == "bezier") return EaseType::Bezier;
    if (s == "sine") return EaseType::Sine;
    if (s == "quad") return EaseType::Quad;
    if (s == "cubic") return EaseType::Cubic;
    if (s == "expo") return EaseType::Expo;
    if (s == "circular" || s == "circ") return EaseType::Circ;
    if (s == "back" || s == "overshoot") return EaseType::Back;
    if (s == "bounce") return EaseType::Bounce;
    if (s == "elastic") return EaseType::Elastic;
    if (s == "spring") return EaseType::Spring;
    if (s == "damped") return EaseType::Damped;
    ok = false;
    return EaseType::Linear;
}

static const char* modeName(EaseMode m) {
    switch (m) {
        case EaseMode::In: return "in";
        case EaseMode::Out: return "out";
        default: return "inOut";
    }
}

json Interp::toJson() const {
    if (type == EaseType::Linear) return "linear";
    if (type == EaseType::Hold) return "hold";
    json j;
    j["type"] = typeName(type);
    if (type == EaseType::Bezier) j["p"] = {p[0], p[1], p[2], p[3]};
    else {
        j["mode"] = modeName(mode);
        if (amount != 1.0) j["amount"] = amount;
        if (frequency != 3.0) j["frequency"] = frequency;
    }
    return j;
}

Interp Interp::fromName(const std::string& name) {
    if (name == "easeIn") return easeIn();
    if (name == "easeOut") return easeOut();
    if (name == "easeInOut" || name == "ease") return easeInOut();
    Interp r;
    bool ok;
    std::string base = name;
    EaseMode mode = EaseMode::Out;
    auto strip = [&](const std::string& suf, EaseMode m) {
        if (base.size() > suf.size() && base.compare(base.size() - suf.size(), suf.size(), suf) == 0) {
            base = base.substr(0, base.size() - suf.size());
            mode = m;
            return true;
        }
        return false;
    };
    if (!strip("InOut", EaseMode::InOut)) {
        if (!strip("In", EaseMode::In)) strip("Out", EaseMode::Out);
    }
    r.type = typeFromName(base, ok);
    if (!ok) return linear();
    r.mode = (base == "overshoot" && mode == EaseMode::Out) ? EaseMode::Out : mode;
    if (r.type == EaseType::Bounce || r.type == EaseType::Elastic || r.type == EaseType::Back) {
        if (name == base) r.mode = EaseMode::Out;  // bare "bounce" means settle at end
    }
    return r;
}

std::string Interp::name() const {
    if (type == EaseType::Bezier) {
        auto near = [&](double a, double b, double c, double d) {
            return std::fabs(p[0] - a) < 1e-3 && std::fabs(p[1] - b) < 1e-3 && std::fabs(p[2] - c) < 1e-3 && std::fabs(p[3] - d) < 1e-3;
        };
        if (near(0.42, 0, 1, 1)) return "easeIn";
        if (near(0, 0, 0.58, 1)) return "easeOut";
        if (near(0.42, 0, 0.58, 1)) return "easeInOut";
        return "bezier";
    }
    std::string n = typeName(type);
    if (type == EaseType::Linear || type == EaseType::Hold || type == EaseType::Spring || type == EaseType::Damped) return n;
    return n + (mode == EaseMode::In ? "In" : mode == EaseMode::Out ? "Out" : "InOut");
}

Interp Interp::fromJson(const json& j) {
    if (j.is_null()) return linear();
    if (j.is_string()) return fromName(j.get<std::string>());
    Interp r;
    bool ok;
    r.type = typeFromName(j.value("type", std::string("linear")), ok);
    if (j.contains("p") && j["p"].is_array() && j["p"].size() == 4)
        for (int i = 0; i < 4; ++i) r.p[i] = j["p"][i].get<double>();
    std::string m = j.value("mode", std::string("inOut"));
    r.mode = m == "in" ? EaseMode::In : m == "out" ? EaseMode::Out : EaseMode::InOut;
    r.amount = j.value("amount", 1.0);
    r.frequency = j.value("frequency", 3.0);
    return r;
}

std::vector<std::string> easingPresetNames() {
    return {"linear", "hold", "easeIn", "easeOut", "easeInOut", "sineInOut", "cubicInOut", "expoInOut", "circularInOut",
            "backOut", "overshoot", "bounce", "elastic", "spring", "damped"};
}

}  // namespace mf
