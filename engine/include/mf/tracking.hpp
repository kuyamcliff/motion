// Motion tracking (template matching with normalized cross-correlation, coarse-to-fine),
// two/four point tracking, and translation/rotation stabilization.
#pragma once
#include "common.hpp"

namespace mf {

struct TrackSample {
    double t = 0;     // source/comp time
    Vec2 p;           // tracked point (frame pixels)
    double confidence = 0;
};

struct TrackOptions {
    int patch = 31;        // template size (px, odd)
    int search = 48;       // search radius (px)
    double minConfidence = 0.55;
    bool updateTemplate = true;  // adapt template slowly for appearance change
};

using FrameFetcher = std::function<ImagePtr(double t)>;
using ProgressFn = std::function<bool(float)>;  // return false to cancel

// Track a single point from t0 to t1 stepping by dt. Stops early (returns partial) when confidence drops.
std::vector<TrackSample> trackPoint(const FrameFetcher& frames, double t0, double t1, double dt, Vec2 start, const TrackOptions& opt,
                                    const ProgressFn& progress = nullptr, std::string* message = nullptr);

// Two-point track -> position (midpoint), rotation (deg) and scale (%) relative to the first frame.
struct TwoPointSample {
    double t;
    Vec2 position;
    double rotation, scale, confidence;
};
std::vector<TwoPointSample> trackTwoPoints(const FrameFetcher& frames, double t0, double t1, double dt, Vec2 a, Vec2 b, const TrackOptions& opt,
                                           const ProgressFn& progress = nullptr);

// Stabilization: per-frame correction transforms (translation, rotation) smoothing the camera path.
struct StabSample {
    double t;
    Vec2 offset;     // translation to apply
    double rotation; // degrees to apply
    double scale;    // crop compensation (%)
};
std::vector<StabSample> stabilize(const FrameFetcher& frames, double t0, double t1, double dt, double smoothingSeconds, bool rotation,
                                  int frameW, int frameH, const ProgressFn& progress = nullptr);

}  // namespace mf
