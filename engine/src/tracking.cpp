#include "mf/tracking.hpp"

#include "mf/threadpool.hpp"

namespace mf {

namespace {

struct Gray {
    int w = 0, h = 0;
    std::vector<float> v;
    float at(int x, int y) const { return v[(size_t)clampv(y, 0, h - 1) * w + clampv(x, 0, w - 1)]; }
};

Gray toGray(const Image& img) {
    Gray g;
    g.w = img.w;
    g.h = img.h;
    g.v.resize((size_t)img.w * img.h);
    for (size_t i = 0; i < g.v.size(); ++i) {
        const uint8_t* p = img.px.data() + i * 4;
        g.v[i] = (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) / 255.f;
    }
    return g;
}

Gray half(const Gray& g) {
    Gray o;
    o.w = std::max(1, g.w / 2);
    o.h = std::max(1, g.h / 2);
    o.v.resize((size_t)o.w * o.h);
    for (int y = 0; y < o.h; ++y)
        for (int x = 0; x < o.w; ++x)
            o.v[(size_t)y * o.w + x] = 0.25f * (g.at(2 * x, 2 * y) + g.at(2 * x + 1, 2 * y) + g.at(2 * x, 2 * y + 1) + g.at(2 * x + 1, 2 * y + 1));
    return o;
}

struct Patch {
    int r;
    std::vector<float> v;
    float mean = 0, norm = 0;
};

Patch extract(const Gray& g, Vec2 c, int r) {
    Patch p;
    p.r = r;
    int n = 2 * r + 1;
    p.v.resize((size_t)n * n);
    double s = 0;
    int cx = (int)std::lround(c.x), cy = (int)std::lround(c.y);
    for (int y = -r; y <= r; ++y)
        for (int x = -r; x <= r; ++x) {
            float v = g.at(cx + x, cy + y);
            p.v[(size_t)(y + r) * n + (x + r)] = v;
            s += v;
        }
    p.mean = (float)(s / p.v.size());
    double nn = 0;
    for (auto& v : p.v) { v -= p.mean; nn += v * v; }
    p.norm = (float)std::sqrt(nn);
    return p;
}

double ncc(const Gray& g, const Patch& t, int cx, int cy) {
    int r = t.r, n = 2 * r + 1;
    double s = 0, ss = 0;
    for (int y = -r; y <= r; ++y)
        for (int x = -r; x <= r; ++x) s += g.at(cx + x, cy + y);
    double mean = s / ((double)n * n);
    double cross = 0;
    for (int y = -r; y <= r; ++y)
        for (int x = -r; x <= r; ++x) {
            double v = g.at(cx + x, cy + y) - mean;
            cross += v * t.v[(size_t)(y + r) * n + (x + r)];
            ss += v * v;
        }
    double den = std::sqrt(ss) * t.norm;
    return den > 1e-9 ? cross / den : 0;
}

// Search best match around `guess` within radius; returns sub-pixel location and score.
Vec2 searchMatch(const Gray& g, const Patch& t, Vec2 guess, int radius, double& score) {
    int gx = (int)std::lround(guess.x), gy = (int)std::lround(guess.y);
    int side = 2 * radius + 1;
    std::vector<double> scores((size_t)side * side, -2);
    parallelFor(side, [&](int b, int e) {
        for (int yy = b; yy < e; ++yy)
            for (int xx = 0; xx < side; ++xx) scores[(size_t)yy * side + xx] = ncc(g, t, gx + xx - radius, gy + yy - radius);
    }, 2);
    int bi = 0;
    for (int i = 1; i < side * side; ++i)
        if (scores[i] > scores[bi]) bi = i;
    int bx = bi % side, by = bi / side;
    score = scores[bi];
    // Parabolic sub-pixel refinement.
    double dx = 0, dy = 0;
    if (bx > 0 && bx < side - 1) {
        double l = scores[(size_t)by * side + bx - 1], c = scores[bi], r = scores[(size_t)by * side + bx + 1];
        double d = l - 2 * c + r;
        if (std::fabs(d) > 1e-9) dx = 0.5 * (l - r) / d;
    }
    if (by > 0 && by < side - 1) {
        double u = scores[(size_t)(by - 1) * side + bx], c = scores[bi], dn = scores[(size_t)(by + 1) * side + bx];
        double d = u - 2 * c + dn;
        if (std::fabs(d) > 1e-9) dy = 0.5 * (u - dn) / d;
    }
    return {gx + bx - radius + clampv(dx, -0.5, 0.5), gy + by - radius + clampv(dy, -0.5, 0.5)};
}

}  // namespace

std::vector<TrackSample> trackPoint(const FrameFetcher& frames, double t0, double t1, double dt, Vec2 start, const TrackOptions& opt,
                                    const ProgressFn& progress, std::string* message) {
    std::vector<TrackSample> out;
    ImagePtr f0 = frames(t0);
    if (!f0 || f0->empty()) { if (message) *message = "Could not read the first frame."; return out; }
    Gray g0 = toGray(*f0);
    int r = std::max(3, opt.patch / 2);
    Patch tmpl = extract(g0, start, r), tmplC = extract(half(g0), start * 0.5, std::max(3, r / 2));
    if (tmpl.norm < 1e-3) { if (message) *message = "The selected point has no texture to track. Choose a high-contrast feature."; return out; }
    out.push_back({t0, start, 1.0});
    Vec2 p = start, vel;
    int steps = (int)std::ceil(std::fabs(t1 - t0) / std::fabs(dt));
    double dir = t1 >= t0 ? 1 : -1;
    for (int i = 1; i <= steps; ++i) {
        double t = t0 + dir * i * std::fabs(dt);
        ImagePtr f = frames(t);
        if (!f || f->empty()) { if (message) *message = "Frame unavailable; tracking stopped."; break; }
        Gray g = toGray(*f), gh = half(g);
        double sc;
        // Coarse search at half resolution with motion prediction, then refine.
        Vec2 coarse = searchMatch(gh, tmplC, (p + vel) * 0.5, std::max(4, opt.search / 2), sc);
        Vec2 fine = searchMatch(g, tmpl, coarse * 2.0, 4, sc);
        if (sc < opt.minConfidence) {
            if (message) *message = formatString("Tracking lost at %.2fs (confidence %.2f). Adjust the point and continue from there.", t, sc);
            break;
        }
        vel = fine - p;
        p = fine;
        out.push_back({t, p, sc});
        if (opt.updateTemplate && sc > 0.9) {
            Patch nt = extract(g, p, r);
            for (size_t k = 0; k < tmpl.v.size(); ++k) tmpl.v[k] = tmpl.v[k] * 0.85f + nt.v[k] * 0.15f;
            double nn = 0;
            for (float v : tmpl.v) nn += v * v;
            tmpl.norm = (float)std::sqrt(nn);
        }
        if (progress && !progress((float)i / steps)) { if (message) *message = "Tracking cancelled."; break; }
    }
    return out;
}

std::vector<TwoPointSample> trackTwoPoints(const FrameFetcher& frames, double t0, double t1, double dt, Vec2 a, Vec2 b, const TrackOptions& opt,
                                           const ProgressFn& progress) {
    std::vector<TwoPointSample> out;
    auto ta = trackPoint(frames, t0, t1, dt, a, opt, progress ? [&](float f) { return progress(f * 0.5f); } : ProgressFn());
    auto tb = trackPoint(frames, t0, t1, dt, b, opt, progress ? [&](float f) { return progress(0.5f + f * 0.5f); } : ProgressFn());
    size_t n = std::min(ta.size(), tb.size());
    if (n == 0) return out;
    Vec2 d0 = tb[0].p - ta[0].p;
    double a0 = std::atan2(d0.y, d0.x), l0 = std::max(1e-6, d0.length());
    for (size_t i = 0; i < n; ++i) {
        Vec2 d = tb[i].p - ta[i].p;
        out.push_back({ta[i].t, (ta[i].p + tb[i].p) * 0.5, (std::atan2(d.y, d.x) - a0) * 180 / kPi, d.length() / l0 * 100.0,
                       std::min(ta[i].confidence, tb[i].confidence)});
    }
    return out;
}

std::vector<StabSample> stabilize(const FrameFetcher& frames, double t0, double t1, double dt, double smoothSec, bool rotation, int fw, int fh,
                                  const ProgressFn& progress) {
    // Track a grid of features, estimate per-frame global motion (median translation + rotation), smooth the path.
    std::vector<StabSample> out;
    std::vector<Vec2> pts;
    for (int gy = 1; gy <= 3; ++gy)
        for (int gx = 1; gx <= 3; ++gx) pts.push_back({fw * gx / 4.0, fh * gy / 4.0});
    TrackOptions opt;
    opt.minConfidence = 0.4;
    opt.updateTemplate = true;
    std::vector<std::vector<TrackSample>> tracks;
    for (size_t i = 0; i < pts.size(); ++i) {
        tracks.push_back(trackPoint(frames, t0, t1, dt, pts[i], opt, progress ? [&](float f) { return progress((i + f) / pts.size()); } : ProgressFn()));
    }
    int steps = (int)std::ceil((t1 - t0) / dt) + 1;
    std::vector<Vec2> path(steps);
    std::vector<double> rot(steps, 0);
    for (int s = 0; s < steps; ++s) {
        std::vector<double> dxs, dys, das;
        for (size_t i = 0; i < tracks.size(); ++i) {
            if ((int)tracks[i].size() <= s) continue;
            dxs.push_back(tracks[i][s].p.x - pts[i].x);
            dys.push_back(tracks[i][s].p.y - pts[i].y);
            Vec2 c{fw / 2.0, fh / 2.0};
            Vec2 v0 = pts[i] - c, v1 = tracks[i][s].p - c;
            if (v0.length() > 10) das.push_back(std::atan2(v1.y, v1.x) - std::atan2(v0.y, v0.x));
        }
        auto median = [](std::vector<double>& v) {
            if (v.empty()) return 0.0;
            std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
            return v[v.size() / 2];
        };
        if (dxs.empty() && s > 0) { path[s] = path[s - 1]; rot[s] = rot[s - 1]; continue; }
        path[s] = {median(dxs), median(dys)};
        rot[s] = rotation ? median(das) * 180 / kPi : 0;
    }
    int rad = std::max(1, (int)std::round(smoothSec / dt));
    double maxOff = 0;
    for (int s = 0; s < steps; ++s) {
        Vec2 acc;
        double ra = 0, wsum = 0;
        for (int k = -rad; k <= rad; ++k) {
            int j = clampv(s + k, 0, steps - 1);
            double w = std::exp(-0.5 * (k * k) / (rad * rad / 4.0 + 1e-9));
            acc += path[j] * w;
            ra += rot[j] * w;
            wsum += w;
        }
        Vec2 smooth = acc / wsum;
        StabSample ss;
        ss.t = t0 + s * dt;
        ss.offset = smooth - path[s];
        ss.rotation = ra / wsum - rot[s];
        maxOff = std::max({maxOff, std::fabs(ss.offset.x) / fw, std::fabs(ss.offset.y) / fh});
        out.push_back(ss);
    }
    double scale = 100.0 * (1 + 2 * maxOff);
    for (auto& s : out) s.scale = scale;
    return out;
}

}  // namespace mf
