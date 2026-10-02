#include "mf/raster.hpp"

namespace mf {

// ====================================================================== bezier paths
BezierPath BezierPath::fromValue(const Value& val) {
    BezierPath p;
    p.closed = val.closed;
    for (size_t i = 0; i + 5 < val.n.size(); i += 6) {
        BezierVertex v;
        v.p = {val.n[i], val.n[i + 1]};
        v.in = {val.n[i + 2], val.n[i + 3]};
        v.out = {val.n[i + 4], val.n[i + 5]};
        p.v.push_back(v);
    }
    return p;
}

Value BezierPath::toValue() const {
    Value r;
    r.kind = Value::Kind::Path;
    r.closed = closed;
    for (auto& x : v) {
        r.n.insert(r.n.end(), {x.p.x, x.p.y, x.in.x, x.in.y, x.out.x, x.out.y});
    }
    return r;
}

BezierPath BezierPath::rect(double cx, double cy, double w, double h, double roundness) {
    BezierPath p;
    double x0 = cx - w / 2, y0 = cy - h / 2, x1 = cx + w / 2, y1 = cy + h / 2;
    double r = clampv(roundness, 0.0, std::min(w, h) / 2);
    if (r <= 0.01) {
        p.v = {{{x0, y0}, {}, {}}, {{x1, y0}, {}, {}}, {{x1, y1}, {}, {}}, {{x0, y1}, {}, {}}};
        return p;
    }
    const double k = 0.5522847498 * r;
    p.v = {{{x0 + r, y0}, {-k, 0}, {}}, {{x1 - r, y0}, {}, {k, 0}}, {{x1, y0 + r}, {0, -k}, {}}, {{x1, y1 - r}, {}, {0, k}},
           {{x1 - r, y1}, {k, 0}, {}},  {{x0 + r, y1}, {}, {-k, 0}}, {{x0, y1 - r}, {0, k}, {}}, {{x0, y0 + r}, {}, {0, -k}}};
    return p;
}

BezierPath BezierPath::ellipse(double cx, double cy, double w, double h) {
    const double k = 0.5522847498;
    double rx = w / 2, ry = h / 2;
    BezierPath p;
    p.v = {{{cx, cy - ry}, {-rx * k, 0}, {rx * k, 0}},
           {{cx + rx, cy}, {0, -ry * k}, {0, ry * k}},
           {{cx, cy + ry}, {rx * k, 0}, {-rx * k, 0}},
           {{cx - rx, cy}, {0, ry * k}, {0, -ry * k}}};
    return p;
}

BezierPath BezierPath::star(double cx, double cy, int points, double outer, double inner, double rotationDeg, bool polygon) {
    BezierPath p;
    points = std::max(3, points);
    int n = polygon ? points : points * 2;
    double rot = deg2rad(rotationDeg) - kPi / 2;
    for (int i = 0; i < n; ++i) {
        double a = rot + 2 * kPi * i / n;
        double r = polygon ? outer : (i % 2 == 0 ? outer : inner);
        p.v.push_back({{cx + std::cos(a) * r, cy + std::sin(a) * r}, {}, {}});
    }
    return p;
}

static void flattenCubic(const Vec2& p0, const Vec2& p1, const Vec2& p2, const Vec2& p3, double tol, std::vector<Vec2>& out) {
    // Estimate subdivisions from control polygon deviation.
    double dd = std::max((p0 - p1 * 2 + p2).length(), (p1 - p2 * 2 + p3).length());
    int n = (int)std::ceil(std::sqrt(dd * 0.75 / std::max(1e-3, tol)));
    n = clampv(n, 1, 256);
    for (int i = 1; i <= n; ++i) {
        double t = (double)i / n, mt = 1 - t;
        Vec2 q = p0 * (mt * mt * mt) + p1 * (3 * mt * mt * t) + p2 * (3 * mt * t * t) + p3 * (t * t * t);
        out.push_back(q);
    }
}

void flattenInto(const BezierPath& p, double tol, Polys& out) {
    if (p.v.empty()) return;
    Contour c;
    c.closed = p.closed;
    c.pts.push_back(p.v[0].p);
    size_t n = p.v.size();
    size_t segs = p.closed ? n : n - 1;
    for (size_t i = 0; i < segs; ++i) {
        const BezierVertex& a = p.v[i];
        const BezierVertex& b = p.v[(i + 1) % n];
        bool straight = a.out.length() < 1e-9 && b.in.length() < 1e-9;
        if (straight) c.pts.push_back(b.p);
        else flattenCubic(a.p, a.p + a.out, b.p + b.in, b.p, tol, c.pts);
    }
    if (p.closed && c.pts.size() > 1 && (c.pts.back() - c.pts.front()).length() < 1e-9) c.pts.pop_back();
    out.push_back(std::move(c));
}

Polys flatten(const BezierPath& p, double tol) {
    Polys out;
    flattenInto(p, tol, out);
    return out;
}

void transformPolys(Polys& polys, const std::function<Vec2(const Vec2&)>& f) {
    for (auto& c : polys)
        for (auto& p : c.pts) p = f(p);
}

double contourLength(const Contour& c) {
    double L = 0;
    for (size_t i = 1; i < c.pts.size(); ++i) L += (c.pts[i] - c.pts[i - 1]).length();
    if (c.closed && c.pts.size() > 1) L += (c.pts.front() - c.pts.back()).length();
    return L;
}

double signedArea(const std::vector<Vec2>& pts) {
    double a = 0;
    for (size_t i = 0, n = pts.size(); i < n; ++i) a += pts[i].cross(pts[(i + 1) % n]);
    return a * 0.5;
}

Rect polysBounds(const Polys& polys) {
    double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
    for (auto& c : polys)
        for (auto& p : c.pts) {
            x0 = std::min(x0, p.x); y0 = std::min(y0, p.y);
            x1 = std::max(x1, p.x); y1 = std::max(y1, p.y);
        }
    if (x0 > x1) return {};
    auto fl = [](double v) { return (int)std::floor(clampv(v, -1e8, 1e8)); };
    auto ce = [](double v) { return (int)std::ceil(clampv(v, -1e8, 1e8)); };
    return {fl(x0), fl(y0), ce(x1) + 1, ce(y1) + 1};
}

// ====================================================================== rasterizer
namespace {
struct Accum {
    int w, h, stride;
    std::vector<float> a;
    Accum(int w_, int h_) : w(w_), h(h_), stride(w_ + 3), a((size_t)(w_ + 3) * h_, 0.f) {}
    void line(double x0, double y0, double x1, double y1) {
        if (std::fabs(y0 - y1) < 1e-9) return;
        double dir = 1;
        if (y0 > y1) { std::swap(x0, x1); std::swap(y0, y1); dir = -1; }
        if (y1 <= 0 || y0 >= h) return;
        double dxdy = (x1 - x0) / (y1 - y0);
        double x = x0;
        if (y0 < 0) { x -= y0 * dxdy; y0 = 0; }
        int ystart = (int)std::floor(y0);
        int yend = std::min(h, (int)std::ceil(y1));
        const double W = w + 1.0;
        for (int y = ystart; y < yend; ++y) {
            float* row = a.data() + (size_t)y * stride;
            double dy = std::min((double)(y + 1), y1) - std::max((double)y, y0);
            double xnext = x + dxdy * dy;
            double d = dy * dir;
            double xa = clampv(x, 0.0, W), xb = clampv(xnext, 0.0, W);
            double lo = std::min(xa, xb), hi = std::max(xa, xb);
            double lofl = std::floor(lo);
            int loi = (int)lofl;
            double hice = std::ceil(hi);
            int hii = (int)hice;
            if (hii <= loi + 1) {
                double xmf = 0.5 * (xa + xb) - lofl;
                row[loi] += (float)(d - d * xmf);
                row[loi + 1] += (float)(d * xmf);
            } else {
                double s = 1.0 / (hi - lo);
                double x0f = lo - lofl;
                double a0 = 0.5 * s * (1.0 - x0f) * (1.0 - x0f);
                double x1f = hi - hice + 1.0;
                double am = 0.5 * s * x1f * x1f;
                row[loi] += (float)(d * a0);
                if (hii == loi + 2) {
                    row[loi + 1] += (float)(d * (1.0 - a0 - am));
                } else {
                    double a1 = s * (1.5 - x0f);
                    row[loi + 1] += (float)(d * (a1 - a0));
                    for (int xi = loi + 2; xi < hii - 1; ++xi) row[xi] += (float)(d * s);
                    double a2 = a1 + (hii - loi - 3) * s;
                    row[hii - 1] += (float)(d * (1.0 - a2 - am));
                }
                row[hii] += (float)(d * am);
            }
            x = xnext;
        }
    }
};
}  // namespace

Coverage rasterize(const Polys& polys, const Rect& clip, FillRule rule) {
    Coverage cov;
    Rect b = polysBounds(polys).intersect(clip);
    if (b.empty()) return cov;
    cov.r = b;
    int w = b.width(), h = b.height();
    Accum acc(w, h);
    for (auto& c : polys) {
        size_t n = c.pts.size();
        if (n < 2) continue;
        for (size_t i = 0; i < n; ++i) {
            const Vec2& p = c.pts[i];
            const Vec2& q = c.pts[(i + 1) % n];
            acc.line(p.x - b.x0, p.y - b.y0, q.x - b.x0, q.y - b.y0);
        }
    }
    cov.a.resize((size_t)w * h);
    for (int y = 0; y < h; ++y) {
        const float* row = acc.a.data() + (size_t)y * acc.stride;
        float* out = cov.a.data() + (size_t)y * w;
        float s = 0;
        for (int x = 0; x < w; ++x) {
            s += row[x];
            float v = std::fabs(s);
            if (rule == FillRule::EvenOdd) {
                v = std::fmod(v, 2.f);
                if (v > 1.f) v = 2.f - v;
            }
            out[x] = v > 1.f ? 1.f : v;
        }
    }
    return cov;
}

// ====================================================================== stroking
static void addOriented(Polys& out, std::vector<Vec2> pts) {
    if (pts.size() < 3) return;
    if (signedArea(pts) < 0) std::reverse(pts.begin(), pts.end());
    out.push_back({std::move(pts), true});
}

static std::vector<Vec2> circlePoly(const Vec2& c, double r) {
    int n = clampv((int)std::ceil(r * 0.9), 8, 64);
    std::vector<Vec2> pts;
    pts.reserve(n);
    for (int i = 0; i < n; ++i) {
        double a = 2 * kPi * i / n;
        pts.push_back({c.x + std::cos(a) * r, c.y + std::sin(a) * r});
    }
    return pts;
}

Polys strokePolys(const Polys& polys, double width, LineCap cap, LineJoin join, double miterLimit) {
    Polys out;
    double hw = width / 2;
    if (hw <= 0) return out;
    for (auto& c0 : polys) {
        // Remove duplicate points.
        std::vector<Vec2> pts;
        for (auto& p : c0.pts)
            if (pts.empty() || (p - pts.back()).length() > 1e-6) pts.push_back(p);
        if (c0.closed && pts.size() > 2 && (pts.front() - pts.back()).length() < 1e-6) pts.pop_back();
        size_t n = pts.size();
        if (n == 1) {
            if (cap == LineCap::Round) addOriented(out, circlePoly(pts[0], hw));
            else if (cap == LineCap::Square) addOriented(out, {{pts[0].x - hw, pts[0].y - hw}, {pts[0].x + hw, pts[0].y - hw}, {pts[0].x + hw, pts[0].y + hw}, {pts[0].x - hw, pts[0].y + hw}});
            continue;
        }
        if (n < 2) continue;
        bool closed = c0.closed && n > 2;
        size_t segs = closed ? n : n - 1;
        for (size_t i = 0; i < segs; ++i) {
            Vec2 a = pts[i], b = pts[(i + 1) % n];
            Vec2 d = (b - a).normalized();
            Vec2 nrm = d.perp() * hw;
            Vec2 a2 = a, b2 = b;
            if (!closed && cap == LineCap::Square) {
                if (i == 0) a2 = a - d * hw;
                if (i == segs - 1) b2 = b + d * hw;
            }
            addOriented(out, {a2 + nrm, b2 + nrm, b2 - nrm, a2 - nrm});
        }
        // Joins.
        size_t firstJ = closed ? 0 : 1, lastJ = closed ? n : n - 1;
        for (size_t j = firstJ; j < lastJ; ++j) {
            Vec2 p = pts[j];
            Vec2 prev = pts[(j + n - 1) % n], next = pts[(j + 1) % n];
            Vec2 d0 = (p - prev).normalized(), d1 = (next - p).normalized();
            double cr = d0.cross(d1);
            double dt = d0.dot(d1);
            if (std::fabs(cr) < 1e-6 && dt > 0) continue;  // collinear
            if (join == LineJoin::Round && std::acos(clampv(dt, -1.0, 1.0)) > 0.35) {
                addOriented(out, circlePoly(p, hw));
                continue;
            }
            // Outer side offsets.
            double side = cr > 0 ? -1 : 1;
            Vec2 o0 = p + d0.perp() * hw * side, o1 = p + d1.perp() * hw * side;
            addOriented(out, {p, o0, o1});
            if (join == LineJoin::Miter) {
                Vec2 bis = (d0.perp() + d1.perp()).normalized() * side;
                double cosHalf = std::sqrt(std::max(1e-9, (1 + dt) / 2));
                double miterLen = hw / cosHalf;
                if (miterLen / hw <= miterLimit) addOriented(out, {o0, p + bis * miterLen, o1, p});
            }
        }
        if (!closed && cap == LineCap::Round) {
            addOriented(out, circlePoly(pts.front(), hw));
            addOriented(out, circlePoly(pts.back(), hw));
        }
    }
    return out;
}

// Extract sub-polyline of a contour between arc lengths [s0, s1] (s0 < s1, both within [0, L]).
static Contour subContour(const Contour& c, double s0, double s1) {
    Contour r;
    r.closed = false;
    size_t n = c.pts.size();
    size_t segs = c.closed ? n : n - 1;
    double acc = 0;
    for (size_t i = 0; i < segs; ++i) {
        Vec2 a = c.pts[i], b = c.pts[(i + 1) % n];
        double len = (b - a).length();
        double e = acc + len;
        if (e >= s0 && acc <= s1 && len > 0) {
            double t0 = clampv((s0 - acc) / len, 0.0, 1.0), t1 = clampv((s1 - acc) / len, 0.0, 1.0);
            Vec2 pa = a + (b - a) * t0, pb = a + (b - a) * t1;
            if (r.pts.empty() || (r.pts.back() - pa).length() > 1e-9) r.pts.push_back(pa);
            r.pts.push_back(pb);
        }
        acc = e;
        if (acc > s1) break;
    }
    return r;
}

Polys dashPolys(const Polys& polys, const std::vector<double>& dash, double offset) {
    double total = 0;
    for (double d : dash) total += std::max(0.0, d);
    if (dash.empty() || total <= 1e-6) return polys;
    Polys out;
    for (auto& c : polys) {
        double L = contourLength(c);
        double pos = -std::fmod(offset, total);
        if (pos > 0) pos -= total;
        size_t di = 0;
        while (pos < L) {
            double len = std::max(0.0, dash[di % dash.size()]);
            if (di % 2 == 0) {
                double s0 = std::max(0.0, pos), s1 = std::min(L, pos + len);
                if (s1 > s0) out.push_back(subContour(c, s0, s1));
            }
            pos += len;
            ++di;
            if (di > 100000) break;
        }
    }
    return out;
}

Polys trimPolys(const Polys& polys, double start, double end, double offset) {
    Polys out;
    start = clampv(start, 0.0, 1.0);
    end = clampv(end, 0.0, 1.0);
    if (start > end) std::swap(start, end);
    if (end - start <= 1e-9) return out;
    if (start <= 1e-9 && end >= 1 - 1e-9 && std::fabs(offset) < 1e-9) return polys;
    for (auto& c : polys) {
        double L = contourLength(c);
        if (L <= 0) continue;
        double s0 = (start + offset) * L, s1 = (end + offset) * L;
        if (c.closed) {
            double sh = std::floor(s0 / L) * L;
            s0 -= sh; s1 -= sh;
            if (s1 <= L) out.push_back(subContour(c, s0, s1));
            else {
                out.push_back(subContour(c, s0, L));
                out.push_back(subContour(c, 0, s1 - L));
            }
        } else {
            s0 = clampv(s0, 0.0, L); s1 = clampv(s1, 0.0, L);
            if (s1 > s0) out.push_back(subContour(c, s0, s1));
        }
    }
    return out;
}

Polys zigzagPolys(const Polys& polys, double size, int ridges, bool smooth) {
    Polys out;
    ridges = std::max(1, ridges);
    for (auto& c : polys) {
        Contour r;
        r.closed = c.closed;
        size_t n = c.pts.size();
        size_t segs = c.closed ? n : n - 1;
        int k = 0;
        for (size_t i = 0; i < segs; ++i) {
            Vec2 a = c.pts[i], b = c.pts[(i + 1) % n];
            Vec2 nrm = (b - a).normalized().perp();
            int steps = ridges * 2;
            for (int s = 0; s < steps; ++s) {
                double t = (double)s / steps;
                double off = (k % 2 == 0 ? 1 : -1) * size;
                if (smooth) off *= std::sin(t * kPi * 2 * ridges) >= 0 ? 1 : 1;
                r.pts.push_back(a + (b - a) * t + nrm * (s == 0 && i == 0 && !c.closed ? 0 : off));
                ++k;
            }
        }
        if (!c.closed && n > 0) r.pts.push_back(c.pts.back());
        if (smooth && r.pts.size() > 3) {
            // Chaikin smoothing pass.
            for (int it = 0; it < 2; ++it) {
                std::vector<Vec2> sm;
                size_t m = r.pts.size();
                for (size_t i = 0; i < (r.closed ? m : m - 1); ++i) {
                    Vec2 p = r.pts[i], q = r.pts[(i + 1) % m];
                    sm.push_back(p * 0.75 + q * 0.25);
                    sm.push_back(p * 0.25 + q * 0.75);
                }
                r.pts = sm;
            }
        }
        out.push_back(r);
    }
    return out;
}

Polys roundCornersPolys(const Polys& polys, double radius) {
    if (radius <= 0) return polys;
    Polys out;
    for (auto& c : polys) {
        size_t n = c.pts.size();
        if (n < 3) { out.push_back(c); continue; }
        Contour r;
        r.closed = c.closed;
        for (size_t i = 0; i < n; ++i) {
            if (!c.closed && (i == 0 || i == n - 1)) { r.pts.push_back(c.pts[i]); continue; }
            Vec2 p = c.pts[i], prev = c.pts[(i + n - 1) % n], next = c.pts[(i + 1) % n];
            double l0 = (p - prev).length(), l1 = (next - p).length();
            double rr = std::min({radius, l0 / 2, l1 / 2});
            Vec2 a = p + (prev - p).normalized() * rr, b = p + (next - p).normalized() * rr;
            // Quadratic curve a -> p -> b
            for (int s = 0; s <= 6; ++s) {
                double t = s / 6.0;
                r.pts.push_back(a * ((1 - t) * (1 - t)) + p * (2 * (1 - t) * t) + b * (t * t));
            }
        }
        out.push_back(r);
    }
    return out;
}

Polys offsetPolysApprox(const Polys& polys, double amount) {
    if (std::fabs(amount) < 1e-6) return polys;
    Polys out;
    for (auto& c : polys) {
        size_t n = c.pts.size();
        if (n < 3 || !c.closed) { out.push_back(c); continue; }
        double sgn = signedArea(c.pts) > 0 ? 1 : -1;
        Contour r;
        r.closed = true;
        for (size_t i = 0; i < n; ++i) {
            Vec2 p = c.pts[i], prev = c.pts[(i + n - 1) % n], next = c.pts[(i + 1) % n];
            Vec2 n0 = (p - prev).normalized().perp(), n1 = (next - p).normalized().perp();
            Vec2 bis = (n0 + n1).normalized();
            double cosH = std::max(0.2, bis.dot(n0));
            r.pts.push_back(p - bis * (amount * sgn / cosH));
        }
        out.push_back(r);
    }
    return out;
}

// ====================================================================== paint
Color Paint::evalAt(double px, double py) const {
    if (type == Type::Solid) return color;
    Vec2 q = inv.apply(px, py);
    if (type == Type::ImageFill) {
        if (!image || image->empty()) return Color(0, 0, 0, 0);
        int iw = image->w, ih = image->h;
        double u = q.x - std::floor(q.x / iw) * iw, v = q.y - std::floor(q.y / ih) * ih;
        const uint8_t* s = image->at(clampv((int)u, 0, iw - 1), clampv((int)v, 0, ih - 1));
        float a = s[3] / 255.f;
        if (a <= 0) return Color(0, 0, 0, 0);
        return Color(s[0] / 255.f / a, s[1] / 255.f / a, s[2] / 255.f / a, a);
    }
    double t = 0;
    Vec2 d = p1 - p0;
    if (type == Type::Linear) {
        double l2 = d.dot(d);
        t = l2 > 1e-12 ? (q - p0).dot(d) / l2 : 0;
    } else if (type == Type::Radial) {
        double r = d.length();
        t = r > 1e-9 ? (q - p0).length() / r : 0;
    } else {
        double a0 = std::atan2(d.y, d.x);
        double a = std::atan2(q.y - p0.y, q.x - p0.x) - a0;
        t = a / (2 * kPi);
        t -= std::floor(t);
    }
    t = clampv(t, 0.0, 1.0);
    if (stops.empty()) return color;
    if (t <= stops.front().first) return stops.front().second;
    if (t >= stops.back().first) return stops.back().second;
    for (size_t i = 0; i + 1 < stops.size(); ++i) {
        if (t >= stops[i].first && t <= stops[i + 1].first) {
            double span = stops[i + 1].first - stops[i].first;
            float u = span > 1e-9 ? (float)((t - stops[i].first) / span) : 0.f;
            const Color& a = stops[i].second;
            const Color& b = stops[i + 1].second;
            return Color(a.r + (b.r - a.r) * u, a.g + (b.g - a.g) * u, a.b + (b.b - a.b) * u, a.a + (b.a - a.a) * u);
        }
    }
    return stops.back().second;
}

void fillCoverage(Image& dst, const Coverage& cov, const Paint& paint) {
    if (cov.empty()) return;
    Rect r = cov.r.intersect({0, 0, dst.w, dst.h});
    bool solid = paint.type == Paint::Type::Solid;
    Color c = paint.color;
    for (int y = r.y0; y < r.y1; ++y) {
        uint8_t* row = dst.row(y);
        const float* cr = cov.a.data() + (size_t)(y - cov.r.y0) * cov.r.width() - cov.r.x0;
        for (int x = r.x0; x < r.x1; ++x) {
            float m = cr[x];
            if (m <= 0.f) continue;
            if (!solid) c = paint.evalAt(x + 0.5, y + 0.5);
            float a = clampv(c.a * paint.opacity * m, 0.f, 1.f);
            if (a <= 0.f) continue;
            uint8_t* d = row + x * 4;
            float inv = 1.f - a;
            d[0] = (uint8_t)std::min(255.f, c.r * a * 255.f + d[0] * inv + 0.5f);
            d[1] = (uint8_t)std::min(255.f, c.g * a * 255.f + d[1] * inv + 0.5f);
            d[2] = (uint8_t)std::min(255.f, c.b * a * 255.f + d[2] * inv + 0.5f);
            d[3] = (uint8_t)std::min(255.f, a * 255.f + d[3] * inv + 0.5f);
        }
    }
}

void applyCoverageAsMask(Image& dst, const Coverage& cov, bool clearOutside) {
    for (int y = 0; y < dst.h; ++y) {
        uint8_t* row = dst.row(y);
        bool rowIn = y >= cov.r.y0 && y < cov.r.y1;
        for (int x = 0; x < dst.w; ++x) {
            float m;
            if (rowIn && x >= cov.r.x0 && x < cov.r.x1) m = cov.at(x, y);
            else { if (!clearOutside) continue; m = 0.f; }
            if (m >= 1.f) continue;
            uint8_t* d = row + x * 4;
            for (int k = 0; k < 4; ++k) d[k] = (uint8_t)(d[k] * m + 0.5f);
        }
    }
}

void blurCoverage(Coverage& c, double radius) {
    if (radius < 0.5 || c.empty()) return;
    int pad = (int)std::ceil(radius * 1.5) + 1;
    Rect nr = c.r.expand(pad);
    int w = nr.width(), h = nr.height();
    std::vector<float> buf((size_t)w * h, 0.f);
    for (int y = c.r.y0; y < c.r.y1; ++y)
        for (int x = c.r.x0; x < c.r.x1; ++x) buf[(size_t)(y - nr.y0) * w + (x - nr.x0)] = c.at(x, y);
    int r = std::max(1, (int)std::round(radius / 1.7));
    std::vector<float> tmp(std::max(w, h));
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < h; ++y) {
            float* row = buf.data() + (size_t)y * w;
            float s = 0;
            for (int x = -r; x <= r; ++x) s += (x >= 0 && x < w) ? row[x] : 0.f;
            for (int x = 0; x < w; ++x) {
                tmp[x] = s / (2 * r + 1);
                int xa = x - r, xb = x + r + 1;
                s += (xb < w ? row[xb] : 0.f) - (xa >= 0 ? row[xa] : 0.f);
            }
            std::copy(tmp.begin(), tmp.begin() + w, row);
        }
        for (int x = 0; x < w; ++x) {
            float s = 0;
            for (int y = -r; y <= r; ++y) s += (y >= 0 && y < h) ? buf[(size_t)y * w + x] : 0.f;
            for (int y = 0; y < h; ++y) {
                tmp[y] = s / (2 * r + 1);
                int ya = y - r, yb = y + r + 1;
                s += (yb < h ? buf[(size_t)yb * w + x] : 0.f) - (ya >= 0 ? buf[(size_t)ya * w + x] : 0.f);
            }
            for (int y = 0; y < h; ++y) buf[(size_t)y * w + x] = tmp[y];
        }
    }
    c.r = nr;
    c.a = std::move(buf);
}

}  // namespace mf
