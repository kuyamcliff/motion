// Interpolation / easing curves used by keyframes, graph editor and behaviours.
#pragma once
#include "common.hpp"

namespace mf {

enum class EaseType {
    Hold, Linear, Bezier, Sine, Quad, Cubic, Expo, Circ, Back, Bounce, Elastic, Spring, Damped
};
enum class EaseMode { In, Out, InOut };

struct Interp {
    EaseType type = EaseType::Linear;
    EaseMode mode = EaseMode::InOut;
    // Cubic Bezier control points (x1,y1,x2,y2) in normalized segment space.
    double p[4] = {0.333, 0.0, 0.667, 1.0};
    double amount = 1.0;     // overshoot / amplitude for back/elastic/spring
    double frequency = 3.0;  // oscillations for elastic/spring/damped

    static Interp linear() { return Interp{}; }
    static Interp hold() { Interp i; i.type = EaseType::Hold; return i; }
    static Interp bezier(double x1, double y1, double x2, double y2) {
        Interp i; i.type = EaseType::Bezier; i.p[0] = x1; i.p[1] = y1; i.p[2] = x2; i.p[3] = y2; return i;
    }
    static Interp easeIn() { return bezier(0.42, 0, 1, 1); }
    static Interp easeOut() { return bezier(0, 0, 0.58, 1); }
    static Interp easeInOut() { return bezier(0.42, 0, 0.58, 1); }

    // Map normalized progress u in [0,1] to eased progress (can overshoot).
    double apply(double u) const;
    json toJson() const;
    static Interp fromJson(const json& j);
    // Names used by UI / scripts ("linear","hold","easeIn","bounce" ...).
    static Interp fromName(const std::string& name);
    std::string name() const;
};

// Solve cubic bezier y(x) for CSS-style timing functions; x1/x2 clamped to [0,1].
double cubicBezierEase(double x1, double y1, double x2, double y2, double x);

// Raw easing families (In mode); other modes derived.
double easeFamily(EaseType t, EaseMode m, double u, double amount, double frequency);

// Named preset list for graph editor ("sine","cubic","bounce","elastic","spring","overshoot","back","expo","circular").
std::vector<std::string> easingPresetNames();

}  // namespace mf
