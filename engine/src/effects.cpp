// CPU implementations of built-in effects.
#include <sstream>

#include "mf/effects.hpp"
#include "mf/imageops.hpp"
#include "mf/raster.hpp"
#include "mf/threadpool.hpp"

namespace mf {

namespace {

using Params = std::map<std::string, Value>;

double P(const Params& p, const char* k, double def) {
    auto it = p.find(k);
    return it == p.end() || it->second.n.empty() ? def : it->second.n[0];
}
bool PB(const Params& p, const char* k, bool def) {
    auto it = p.find(k);
    if (it == p.end()) return def;
    if (it->second.kind == Value::Kind::String) return it->second.s == "true";
    return it->second.n.empty() ? def : it->second.n[0] != 0;
}
Color PC(const Params& p, const char* k, Color def) {
    auto it = p.find(k);
    return it == p.end() ? def : it->second.color(def);
}
Vec2 PV(const Params& p, const char* k, Vec2 def) {
    auto it = p.find(k);
    return it == p.end() ? def : it->second.vec2(def);
}
std::string PS(const Params& p, const char* k, const std::string& def) {
    auto it = p.find(k);
    return it == p.end() || it->second.kind != Value::Kind::String ? def : it->second.s;
}

// Apply fn on unpremultiplied float colors (0..1) for pixels with alpha > 0 within rect.
template <typename F>
void colorOp(Image& img, const Rect& b0, F fn) {
    Rect b = b0.intersect({0, 0, img.w, img.h});
    parallelFor(b.height(), [&](int s, int e) {
        for (int y = b.y0 + s; y < b.y0 + e; ++y) {
            uint8_t* d = img.row(y) + b.x0 * 4;
            for (int x = b.x0; x < b.x1; ++x, d += 4) {
                if (d[3] == 0) continue;
                float a = d[3] / 255.f;
                float r = d[0] / 255.f / a, g = d[1] / 255.f / a, bl = d[2] / 255.f / a;
                fn(r, g, bl, a, x, y);
                r = clampv(r, 0.f, 1.f); g = clampv(g, 0.f, 1.f); bl = clampv(bl, 0.f, 1.f); a = clampv(a, 0.f, 1.f);
                d[0] = (uint8_t)(r * a * 255.f + 0.5f);
                d[1] = (uint8_t)(g * a * 255.f + 0.5f);
                d[2] = (uint8_t)(bl * a * 255.f + 0.5f);
                d[3] = (uint8_t)(a * 255.f + 0.5f);
            }
        }
    }, 8);
}

// Inverse-map distortion: for each destination pixel, fn computes source coordinates.
template <typename F>
void distort(Image& img, Rect& bounds, F fn) {
    Image src = img;
    Rect full{0, 0, img.w, img.h};
    parallelFor(img.h, [&](int s, int e) {
        float px[4];
        for (int y = s; y < e; ++y) {
            uint8_t* d = img.row(y);
            for (int x = 0; x < img.w; ++x, d += 4) {
                double sx = x + 0.5, sy = y + 0.5;
                fn(sx, sy);
                sampleBilinear(src, sx, sy, px);
                for (int k = 0; k < 4; ++k) d[k] = (uint8_t)std::min(255.f, px[k] + 0.5f);
            }
        }
    }, 8);
    bounds = full;
}

float luma(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

// Monotone cubic interpolation through curve points (x,y in 0..1) into a 256 LUT.
std::array<float, 256> curveLut(const Value& v) {
    std::array<float, 256> lut;
    std::vector<std::pair<double, double>> pts;
    for (size_t i = 0; i + 1 < v.n.size(); i += 2) pts.push_back({v.n[i], v.n[i + 1]});
    if (pts.size() < 2) pts = {{0, 0}, {1, 1}};
    std::sort(pts.begin(), pts.end());
    size_t n = pts.size();
    std::vector<double> m(n), dl(n - 1);
    for (size_t i = 0; i + 1 < n; ++i) dl[i] = (pts[i + 1].second - pts[i].second) / std::max(1e-9, pts[i + 1].first - pts[i].first);
    m[0] = dl[0];
    m[n - 1] = dl[n - 2];
    for (size_t i = 1; i + 1 < n; ++i) m[i] = (dl[i - 1] * dl[i] <= 0) ? 0 : (dl[i - 1] + dl[i]) / 2;
    for (int i = 0; i < 256; ++i) {
        double x = i / 255.0;
        double y;
        if (x <= pts[0].first) y = pts[0].second;
        else if (x >= pts[n - 1].first) y = pts[n - 1].second;
        else {
            size_t k = 0;
            while (k + 1 < n && pts[k + 1].first < x) ++k;
            double h = pts[k + 1].first - pts[k].first, t = (x - pts[k].first) / h;
            double t2 = t * t, t3 = t2 * t;
            y = (2 * t3 - 3 * t2 + 1) * pts[k].second + (t3 - 2 * t2 + t) * h * m[k] + (-2 * t3 + 3 * t2) * pts[k + 1].second + (t3 - t2) * h * m[k + 1];
        }
        lut[i] = (float)clampv(y, 0.0, 1.0);
    }
    return lut;
}

// Bright-pass + blur + additive composite.
void glowImpl(Image& img, Rect& bounds, double threshold, double radius, double intensity, bool useColor, Color color) {
    Image bright(img.w, img.h);
    Rect b = bounds.intersect({0, 0, img.w, img.h});
    float th = (float)threshold;
    for (int y = b.y0; y < b.y1; ++y) {
        const uint8_t* s = img.row(y);
        uint8_t* d = bright.row(y);
        for (int x = b.x0; x < b.x1; ++x) {
            const uint8_t* p = s + x * 4;
            if (p[3] == 0) continue;
            float a = p[3] / 255.f;
            float l = luma(p[0] / 255.f, p[1] / 255.f, p[2] / 255.f) / std::max(a, 1e-3f);
            float k = clampv((l - th) / std::max(1e-3f, 1 - th), 0.f, 1.f);
            if (k <= 0) continue;
            uint8_t* q = d + x * 4;
            if (useColor) {
                q[0] = (uint8_t)(color.r * k * a * 255); q[1] = (uint8_t)(color.g * k * a * 255); q[2] = (uint8_t)(color.b * k * a * 255);
                q[3] = (uint8_t)(k * a * 255);
            } else {
                for (int c = 0; c < 4; ++c) q[c] = (uint8_t)(p[c] * k);
            }
        }
    }
    Rect gb = b;
    gaussianBlur(bright, radius, gb);
    float inten = (float)intensity;
    for (int y = gb.y0; y < gb.y1; ++y) {
        uint8_t* d = img.row(y);
        const uint8_t* s = bright.row(y);
        for (int x = gb.x0; x < gb.x1; ++x)
            for (int c = 0; c < 4; ++c) {
                int v = d[x * 4 + c] + (int)(s[x * 4 + c] * inten);
                d[x * 4 + c] = (uint8_t)std::min(255, v);
            }
    }
    bounds = gb;
}

void alphaMorph(Image& img, int radius, bool dilate, std::vector<uint8_t>& out) {
    // Separable max/min filter on alpha.
    int w = img.w, h = img.h;
    std::vector<uint8_t> tmp((size_t)w * h);
    out.assign((size_t)w * h, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t v = dilate ? 0 : 255;
            for (int k = -radius; k <= radius; ++k) {
                int xx = x + k;
                uint8_t a = (xx >= 0 && xx < w) ? img.at(xx, y)[3] : 0;
                v = dilate ? std::max(v, a) : std::min(v, a);
            }
            tmp[(size_t)y * w + x] = v;
        }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t v = dilate ? 0 : 255;
            for (int k = -radius; k <= radius; ++k) {
                int yy = y + k;
                uint8_t a = (yy >= 0 && yy < h) ? tmp[(size_t)yy * w + x] : 0;
                v = dilate ? std::max(v, a) : std::min(v, a);
            }
            out[(size_t)y * w + x] = v;
        }
}

}  // namespace

// ====================================================================== LUT
bool LutData::parseCube(const std::string& text, std::string& err) {
    std::istringstream in(text);
    std::string line;
    size = 0;
    data.clear();
    double dmin[3] = {0, 0, 0}, dmax[3] = {1, 1, 1};
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string tok;
        ls >> tok;
        if (tok == "TITLE") { std::getline(ls, title); continue; }
        if (tok == "LUT_3D_SIZE") { ls >> size; continue; }
        if (tok == "LUT_1D_SIZE") { err = "1D LUTs are not supported; use a 3D .cube LUT."; return false; }
        if (tok == "DOMAIN_MIN") { ls >> dmin[0] >> dmin[1] >> dmin[2]; continue; }
        if (tok == "DOMAIN_MAX") { ls >> dmax[0] >> dmax[1] >> dmax[2]; continue; }
        if ((tok[0] >= '0' && tok[0] <= '9') || tok[0] == '-' || tok[0] == '.') {
            float r = std::stof(tok), g, b;
            ls >> g >> b;
            data.push_back(r); data.push_back(g); data.push_back(b);
        }
    }
    if (size < 2 || size > 256) { err = "Missing or invalid LUT_3D_SIZE."; return false; }
    if ((int)data.size() != size * size * size * 3) {
        err = "LUT has " + std::to_string(data.size() / 3) + " entries, expected " + std::to_string(size * size * size) + ".";
        return false;
    }
    (void)dmin; (void)dmax;
    return true;
}

bool LutData::parse3dl(const std::string& text, std::string& err) {
    std::istringstream in(text);
    std::string line;
    std::vector<int> vals;
    int maxv = 0;
    bool first = true;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::vector<int> nums;
        int v;
        while (ls >> v) nums.push_back(v);
        if (nums.empty()) continue;
        if (first && nums.size() > 3) { first = false; continue; }  // shaper/mesh line
        first = false;
        if (nums.size() != 3) continue;
        for (int x : nums) { vals.push_back(x); maxv = std::max(maxv, x); }
    }
    int count = (int)vals.size() / 3;
    size = (int)std::round(std::cbrt((double)count));
    if (size < 2 || size * size * size != count) { err = "Unrecognized .3dl layout."; return false; }
    double scale = maxv > 4095 ? 65535.0 : maxv > 1023 ? 4095.0 : 1023.0;
    // .3dl ordering: blue fastest -> reorder to red fastest.
    data.assign((size_t)count * 3, 0);
    for (int r = 0; r < size; ++r)
        for (int g = 0; g < size; ++g)
            for (int b = 0; b < size; ++b) {
                int src = ((r * size + g) * size + b) * 3;
                int dst = ((b * size + g) * size + r) * 3;
                for (int k = 0; k < 3; ++k) data[dst + k] = (float)(vals[src + k] / scale);
            }
    return true;
}

bool LutData::load(const std::string& path, std::string& err) {
    bool ok;
    std::string text = readFileText(path, &ok);
    if (!ok) { err = "Cannot read LUT file."; return false; }
    std::string ext = pathExtensionLower(path);
    if (ext == "3dl") return parse3dl(text, err);
    return parseCube(text, err);
}

void LutData::sample(float r, float g, float b, float& orr, float& og, float& ob) const {
    float fr = clampv(r, 0.f, 1.f) * (size - 1), fg = clampv(g, 0.f, 1.f) * (size - 1), fb = clampv(b, 0.f, 1.f) * (size - 1);
    int r0 = std::min((int)fr, size - 2), g0 = std::min((int)fg, size - 2), b0 = std::min((int)fb, size - 2);
    float dr = fr - r0, dg = fg - g0, db = fb - b0;
    auto at = [&](int ri, int gi, int bi, int c) { return data[((size_t)(bi * size + gi) * size + ri) * 3 + c]; };
    float out[3];
    for (int c = 0; c < 3; ++c) {
        float c00 = at(r0, g0, b0, c) * (1 - dr) + at(r0 + 1, g0, b0, c) * dr;
        float c10 = at(r0, g0 + 1, b0, c) * (1 - dr) + at(r0 + 1, g0 + 1, b0, c) * dr;
        float c01 = at(r0, g0, b0 + 1, c) * (1 - dr) + at(r0 + 1, g0, b0 + 1, c) * dr;
        float c11 = at(r0, g0 + 1, b0 + 1, c) * (1 - dr) + at(r0 + 1, g0 + 1, b0 + 1, c) * dr;
        float c0 = c00 * (1 - dg) + c10 * dg, c1 = c01 * (1 - dg) + c11 * dg;
        out[c] = c0 * (1 - db) + c1 * db;
    }
    orr = out[0]; og = out[1]; ob = out[2];
}

// ====================================================================== dispatcher
void applyEffect(const std::string& type, const Params& p, Image& img, Rect& bounds, const EffectEnv& env) {
    if (img.empty()) return;
    const double S = env.scale;
    const int W = img.w, H = img.h;
    Rect full{0, 0, W, H};
    auto ptPx = [&](const char* k, Vec2 def) { Vec2 v = PV(p, k, def); return Vec2(v.x * W, v.y * H); };

    // ---------------------------------------------------------------- blur
    if (type == "blur.gaussian") {
        std::string dim = PS(p, "dimensions", "both");
        gaussianBlur(img, P(p, "radius", 10) * S, bounds, dim != "vertical", dim != "horizontal");
    } else if (type == "blur.directional") {
        directionalBlur(img, deg2rad(P(p, "angle", 0)), P(p, "length", 20) * S, bounds);
    } else if (type == "blur.radial" || type == "blur.zoom") {
        Vec2 c = ptPx("center", {0.5, 0.5});
        double amt = P(p, "amount", 10);
        bool zoom = type == "blur.zoom";
        int samples = clampv((int)(amt * (zoom ? 1.0 : 1.5)) + 4, 4, 48);
        Image src = img;
        parallelFor(H, [&](int s, int e) {
            float px[4];
            for (int y = s; y < e; ++y)
                for (int x = 0; x < W; ++x) {
                    float acc[4] = {0, 0, 0, 0};
                    double dx = x + 0.5 - c.x, dy = y + 0.5 - c.y;
                    for (int i = 0; i < samples; ++i) {
                        double t = (double)i / (samples - 1) - 0.5;
                        double sx, sy;
                        if (zoom) {
                            double k = 1 + t * amt / 100.0;
                            sx = c.x + dx * k; sy = c.y + dy * k;
                        } else {
                            double a = deg2rad(amt) * t, ca = std::cos(a), sa = std::sin(a);
                            sx = c.x + dx * ca - dy * sa; sy = c.y + dx * sa + dy * ca;
                        }
                        sampleBilinear(src, sx, sy, px);
                        for (int k = 0; k < 4; ++k) acc[k] += px[k];
                    }
                    uint8_t* d = img.at(x, y);
                    for (int k = 0; k < 4; ++k) d[k] = (uint8_t)std::min(255.f, acc[k] / samples + 0.5f);
                }
        }, 4);
        bounds = full;
    } else if (type == "blur.bokeh") {
        double r = P(p, "radius", 8) * S, boost = P(p, "boost", 30) / 100.0;
        if (r < 0.5) return;
        Rect b = bounds.expand((int)std::ceil(r) + 1).intersect(full);
        Image src = img;
        // Disc kernel sampled on concentric rings; bright highlights weighted up.
        std::vector<Vec2> taps;
        taps.push_back({0, 0});
        for (int ring = 1; ring <= 3; ++ring) {
            int n = ring * 8;
            for (int i = 0; i < n; ++i) {
                double a = 2 * kPi * i / n;
                taps.push_back({std::cos(a) * r * ring / 3.0, std::sin(a) * r * ring / 3.0});
            }
        }
        parallelFor(b.height(), [&](int s, int e) {
            float px[4];
            for (int y = b.y0 + s; y < b.y0 + e; ++y)
                for (int x = b.x0; x < b.x1; ++x) {
                    float acc[4] = {0, 0, 0, 0}, wsum = 0;
                    for (auto& t : taps) {
                        sampleBilinear(src, x + 0.5 + t.x, y + 0.5 + t.y, px);
                        float l = luma(px[0], px[1], px[2]) / 255.f;
                        float w = 1.f + (float)boost * 4.f * l * l;
                        for (int k = 0; k < 4; ++k) acc[k] += px[k] * w;
                        wsum += w;
                    }
                    uint8_t* d = img.at(x, y);
                    for (int k = 0; k < 4; ++k) d[k] = (uint8_t)std::min(255.f, acc[k] / wsum + 0.5f);
                }
        }, 4);
        bounds = b;
    }
    // ---------------------------------------------------------------- distortion
    else if (type == "distort.ripple") {
        Vec2 c = ptPx("center", {0.5, 0.5});
        double amp = P(p, "amplitude", 10) * S, wl = std::max(1.0, P(p, "wavelength", 40) * S);
        double ph = deg2rad(P(p, "phase", 0)) + env.compTime * P(p, "speed", 1) * 2 * kPi;
        distort(img, bounds, [&](double& x, double& y) {
            double dx = x - c.x, dy = y - c.y, d = std::sqrt(dx * dx + dy * dy);
            if (d < 1e-6) return;
            double off = std::sin(d / wl * 2 * kPi - ph) * amp;
            x += dx / d * off; y += dy / d * off;
        });
    } else if (type == "distort.wave") {
        double amp = P(p, "amplitude", 10) * S, wl = std::max(1.0, P(p, "wavelength", 80) * S), ang = deg2rad(P(p, "angle", 0));
        double ph = deg2rad(P(p, "phase", 0)) + env.compTime * P(p, "speed", 1) * 2 * kPi;
        double ca = std::cos(ang), sa = std::sin(ang);
        distort(img, bounds, [&](double& x, double& y) {
            double along = x * ca + y * sa;
            double off = std::sin(along / wl * 2 * kPi + ph) * amp;
            x += -sa * off; y += ca * off;
        });
    } else if (type == "distort.bulge" || type == "distort.pinch") {
        Vec2 c = ptPx("center", {0.5, 0.5});
        double r = std::max(1.0, P(p, "radius", 300) * S), amt = P(p, "amount", 50) / 100.0;
        if (type == "distort.pinch") amt = -amt;
        distort(img, bounds, [&](double& x, double& y) {
            double dx = x - c.x, dy = y - c.y, d = std::sqrt(dx * dx + dy * dy);
            if (d >= r || d < 1e-6) return;
            double u = d / r;
            double k = amt >= 0 ? std::pow(u, amt * 1.5 + 1) / u : std::pow(u, 1.0 / (1 - amt * 1.5)) / u;
            double sm = smoothstep(0.0, 1.0, u);
            k = k * (1 - sm) + sm;
            x = c.x + dx * k; y = c.y + dy * k;
        });
    } else if (type == "distort.twirl") {
        Vec2 c = ptPx("center", {0.5, 0.5});
        double r = std::max(1.0, P(p, "radius", 300) * S), ang = deg2rad(P(p, "angle", 90));
        distort(img, bounds, [&](double& x, double& y) {
            double dx = x - c.x, dy = y - c.y, d = std::sqrt(dx * dx + dy * dy);
            if (d >= r) return;
            double a = ang * (1 - d / r) * (1 - d / r);
            double ca = std::cos(a), sa = std::sin(a);
            x = c.x + dx * ca - dy * sa; y = c.y + dx * sa + dy * ca;
        });
    } else if (type == "distort.lens") {
        double k = P(p, "amount", 30) / 100.0, zoom = P(p, "zoom", 100) / 100.0;
        double cx = W / 2.0, cy = H / 2.0, norm = std::sqrt(cx * cx + cy * cy);
        distort(img, bounds, [&](double& x, double& y) {
            double dx = (x - cx) / norm, dy = (y - cy) / norm, r2 = dx * dx + dy * dy;
            double f = (1 + k * r2) / zoom;
            x = cx + dx * f * norm; y = cy + dy * f * norm;
        });
    } else if (type == "distort.turbulence") {
        double amt = P(p, "amount", 20) * S, size = std::max(1.0, P(p, "size", 80) * S);
        int oct = clampv((int)P(p, "complexity", 3), 1, 6);
        double evo = deg2rad(P(p, "evolution", 0));
        uint32_t seed = (uint32_t)P(p, "seed", 1);
        double ex = std::cos(evo) * 3, ey = std::sin(evo) * 3;
        distort(img, bounds, [&](double& x, double& y) {
            double nx = fbm2D(x / size + ex, y / size + ey, seed, oct), ny = fbm2D(x / size + 17.3 + ey, y / size + 5.1 + ex, seed + 1, oct);
            x += nx * amt; y += ny * amt;
        });
    } else if (type == "distort.cornerPin") {
        Vec2 q[4] = {ptPx("topLeft", {0, 0}), ptPx("topRight", {1, 0}), ptPx("bottomRight", {1, 1}), ptPx("bottomLeft", {0, 1})};
        Vec2 src[4] = {{0, 0}, {(double)W, 0}, {(double)W, (double)H}, {0, (double)H}};
        Mat3 m;
        if (!Mat3::quadToQuad(q, src, m)) return;
        Image s = img;
        img.clear();
        warpImage(img, s, m, 1.0f, full, 1);
        bounds = full;
    } else if (type == "distort.mirror") {
        Vec2 c = ptPx("center", {0.5, 0.5});
        double ang = deg2rad(P(p, "angle", 0));
        Vec2 n{std::cos(ang), std::sin(ang)};
        distort(img, bounds, [&](double& x, double& y) {
            double d = (x - c.x) * n.x + (y - c.y) * n.y;
            if (d > 0) { x -= 2 * d * n.x; y -= 2 * d * n.y; }
        });
    } else if (type == "distort.polar") {
        double amt = P(p, "amount", 100) / 100.0;
        bool toPolar = PS(p, "mode", "rectToPolar") == "rectToPolar";
        double cx = W / 2.0, cy = H / 2.0, R = std::min(cx, cy);
        distort(img, bounds, [&](double& x, double& y) {
            double ox = x, oy = y, sx, sy;
            if (toPolar) {
                double dx = x - cx, dy = y - cy;
                double a = std::atan2(dy, dx) / (2 * kPi) + 0.5, r = std::sqrt(dx * dx + dy * dy) / R;
                sx = a * W; sy = r * H;
            } else {
                double a = (x / W - 0.5) * 2 * kPi, r = y / H * R;
                sx = cx + std::cos(a) * r; sy = cy + std::sin(a) * r;
            }
            x = ox + (sx - ox) * amt; y = oy + (sy - oy) * amt;
        });
    }
    // ---------------------------------------------------------------- color
    else if (type == "color.exposure") {
        float m = (float)std::pow(2.0, P(p, "exposure", 0)), off = (float)P(p, "offset", 0), g = (float)(1.0 / std::max(0.01, P(p, "gamma", 1)));
        colorOp(img, bounds, [&](float& r, float& gg, float& b, float&, int, int) {
            auto f = [&](float c) { float l = srgbToLinear(c) * m + off; return linearToSrgb(std::pow(std::max(0.f, l), g)); };
            r = f(r); gg = f(gg); b = f(b);
        });
    } else if (type == "color.brightnessContrast") {
        float br = (float)P(p, "brightness", 0) / 100.f, ct = (float)P(p, "contrast", 0) / 100.f;
        float k = ct >= 0 ? 1.f / std::max(0.01f, 1 - ct) : 1 + ct;
        colorOp(img, bounds, [&](float& r, float& g, float& b, float&, int, int) {
            r = (r - 0.5f) * k + 0.5f + br; g = (g - 0.5f) * k + 0.5f + br; b = (b - 0.5f) * k + 0.5f + br;
        });
    } else if (type == "color.hueSaturation") {
        float hs = (float)P(p, "hue", 0) / 360.f, sat = (float)P(p, "saturation", 0) / 100.f, li = (float)P(p, "lightness", 0) / 100.f;
        bool colorize = PB(p, "colorize", false);
        colorOp(img, bounds, [&](float& r, float& g, float& b, float&, int, int) {
            float h, s, l;
            rgbToHsl(r, g, b, h, s, l);
            if (colorize) { h = hs; s = clampv(0.25f + sat * 0.75f, 0.f, 1.f); }
            else { h += hs; s = clampv(s * (1 + sat), 0.f, 1.f); }
            l = li >= 0 ? l + (1 - l) * li : l * (1 + li);
            hslToRgb(h, s, l, r, g, b);
        });
    } else if (type == "color.vibrance") {
        float vib = (float)P(p, "vibrance", 30) / 100.f, sat = (float)P(p, "saturation", 0) / 100.f;
        colorOp(img, bounds, [&](float& r, float& g, float& b, float&, int, int) {
            float mx = std::max({r, g, b}), mn = std::min({r, g, b});
            float curSat = mx - mn;
            float k = 1 + sat + vib * (1 - curSat);
            float l = luma(r, g, b);
            r = l + (r - l) * k; g = l + (g - l) * k; b = l + (b - l) * k;
        });
    } else if (type == "color.temperatureTint") {
        float t = (float)P(p, "temperature", 0) / 100.f, ti = (float)P(p, "tint", 0) / 100.f;
        float rm = 1 + 0.25f * t, bm = 1 - 0.25f * t, gm = 1 - 0.2f * ti;
        colorOp(img, bounds, [&](float& r, float& g, float& b, float&, int, int) {
            float l0 = luma(r, g, b);
            r *= rm; g *= gm; b *= bm;
            float l1 = luma(r, g, b);
            if (l1 > 1e-4f) { float k = l0 / l1; r *= k; g *= k; b *= k; }
        });
    } else if (type == "color.curves") {
        auto get = [&](const char* k) { auto it = p.find(k); return it == p.end() ? Value::vec({0, 0, 1, 1}) : it->second; };
        auto m = curveLut(get("master")), rl = curveLut(get("red")), gl = curveLut(get("green")), bl = curveLut(get("blue"));
        colorOp(img, bounds, [&](float& r, float& g, float& b, float&, int, int) {
            r = rl[(int)(m[(int)(r * 255 + 0.5f)] * 255 + 0.5f)];
            g = gl[(int)(m[(int)(g * 255 + 0.5f)] * 255 + 0.5f)];
            b = bl[(int)(m[(int)(b * 255 + 0.5f)] * 255 + 0.5f)];
        });
    } else if (type == "color.levels") {
        float ib = (float)P(p, "inBlack", 0), iw = (float)P(p, "inWhite", 1), gm = (float)P(p, "gamma", 1), ob = (float)P(p, "outBlack", 0), ow = (float)P(p, "outWhite", 1);
        float ig = 1.f / std::max(0.01f, gm);
        colorOp(img, bounds, [&](float& r, float& g, float& b, float&, int, int) {
            auto f = [&](float c) { float v = clampv((c - ib) / std::max(1e-4f, iw - ib), 0.f, 1.f); return ob + std::pow(v, ig) * (ow - ob); };
            r = f(r); g = f(g); b = f(b);
        });
    } else if (type == "color.channelMixer") {
        float m[9] = {(float)P(p, "rr", 100), (float)P(p, "rg", 0), (float)P(p, "rb", 0), (float)P(p, "gr", 0), (float)P(p, "gg", 100),
                      (float)P(p, "gb", 0), (float)P(p, "br", 0), (float)P(p, "bg", 0), (float)P(p, "bb", 100)};
        for (auto& v : m) v /= 100.f;
        bool mono = PB(p, "monochrome", false);
        colorOp(img, bounds, [&](float& r, float& g, float& b, float&, int, int) {
            float nr = r * m[0] + g * m[1] + b * m[2], ng = r * m[3] + g * m[4] + b * m[5], nb = r * m[6] + g * m[7] + b * m[8];
            if (mono) ng = nb = nr;
            r = nr; g = ng; b = nb;
        });
    } else if (type == "color.selective") {
        float th = (float)P(p, "targetHue", 0) / 360.f, rng = (float)P(p, "range", 30) / 360.f;
        float hs = (float)P(p, "hueShift", 0) / 360.f, ss = (float)P(p, "saturation", 0) / 100.f, ls = (float)P(p, "lightness", 0) / 100.f;
        colorOp(img, bounds, [&](float& r, float& g, float& b, float&, int, int) {
            float h, s, l;
            rgbToHsl(r, g, b, h, s, l);
            float d = std::fabs(h - th);
            d = std::min(d, 1 - d);
            float w = (float)(1 - smoothstep(rng * 0.5, rng, d)) * std::min(1.f, s * 4);
            if (w <= 0) return;
            h += hs * w;
            s = clampv(s * (1 + ss * w), 0.f, 1.f);
            l = clampv(l + ls * w * 0.5f, 0.f, 1.f);
            hslToRgb(h, s, l, r, g, b);
        });
    } else if (type == "color.shadowsHighlights") {
        float sh = (float)P(p, "shadows", 0) / 100.f, hi = (float)P(p, "highlights", 0) / 100.f;
        float wh = (float)P(p, "whites", 0) / 100.f, bk = (float)P(p, "blacks", 0) / 100.f;
        colorOp(img, bounds, [&](float& r, float& g, float& b, float&, int, int) {
            float l = luma(r, g, b);
            float ws = (1 - l) * (1 - l), whl = l * l;
            float d = sh * ws * 0.5f + hi * whl * 0.5f + wh * std::pow(l, 4.f) * 0.3f + bk * std::pow(1 - l, 4.f) * 0.3f;
            r += d; g += d; b += d;
        });
    } else if (type == "color.lut") {
        auto it = p.find("lut");
        std::string id = it == p.end() ? "" : it->second.s;
        if (id.empty() || !env.lutLoader) return;
        auto lut = env.lutLoader(id);
        if (!lut || lut->size < 2) return;
        float k = (float)P(p, "intensity", 100) / 100.f;
        colorOp(img, bounds, [&](float& r, float& g, float& b, float&, int, int) {
            float nr, ng, nb;
            lut->sample(r, g, b, nr, ng, nb);
            r += (nr - r) * k; g += (ng - g) * k; b += (nb - b) * k;
        });
    } else if (type == "color.blackWhite") {
        float k = (float)P(p, "amount", 100) / 100.f;
        colorOp(img, bounds, [&](float& r, float& g, float& b, float&, int, int) {
            float l = luma(r, g, b);
            r += (l - r) * k; g += (l - g) * k; b += (l - b) * k;
        });
    } else if (type == "color.tint") {
        Color bk = PC(p, "black", Color(0, 0, 0)), wh = PC(p, "white", Color(1, 1, 1));
        float k = (float)P(p, "amount", 100) / 100.f;
        colorOp(img, bounds, [&](float& r, float& g, float& b, float&, int, int) {
            float l = luma(r, g, b);
            float nr = bk.r + (wh.r - bk.r) * l, ng = bk.g + (wh.g - bk.g) * l, nb = bk.b + (wh.b - bk.b) * l;
            r += (nr - r) * k; g += (ng - g) * k; b += (nb - b) * k;
        });
    } else if (type == "color.invert") {
        float k = (float)P(p, "amount", 100) / 100.f;
        colorOp(img, bounds, [&](float& r, float& g, float& b, float&, int, int) {
            r += (1 - 2 * r) * k; g += (1 - 2 * g) * k; b += (1 - 2 * b) * k;
        });
    } else if (type == "color.threshold") {
        float lv = (float)P(p, "level", 50) / 100.f;
        colorOp(img, bounds, [&](float& r, float& g, float& b, float&, int, int) {
            float v = luma(r, g, b) >= lv ? 1.f : 0.f;
            r = g = b = v;
        });
    }
    // ---------------------------------------------------------------- stylize
    else if (type == "stylize.glow") {
        glowImpl(img, bounds, P(p, "threshold", 60) / 100.0, P(p, "radius", 20) * S, P(p, "intensity", 100) / 100.0, PB(p, "useColor", false),
                 PC(p, "color", Color(1, 1, 1)));
    } else if (type == "stylize.bloom") {
        double th = P(p, "threshold", 70) / 100.0, r = P(p, "radius", 40) * S, in = P(p, "intensity", 80) / 100.0;
        Rect b1 = bounds, b2 = bounds;
        Image a = img;
        glowImpl(a, b1, th, r * 0.25, in * 0.6, false, Color());
        glowImpl(a, b2, th, r, in * 0.6, false, Color());
        img = std::move(a);
        bounds = b1.unite(b2);
    } else if (type == "stylize.outline") {
        int w = std::max(0, (int)std::round(P(p, "width", 4) * S));
        if (w == 0) return;
        Color c = PC(p, "color", Color(1, 1, 1));
        float op = (float)P(p, "opacity", 100) / 100.f;
        std::string pos = PS(p, "position", "outside");
        std::vector<uint8_t> dil, ero;
        if (pos != "inside") alphaMorph(img, pos == "center" ? std::max(1, w / 2) : w, true, dil);
        if (pos != "outside") alphaMorph(img, pos == "center" ? std::max(1, w / 2) : w, false, ero);
        Image out(W, H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                size_t i = (size_t)y * W + x;
                float a = img.at(x, y)[3] / 255.f;
                float ring;
                if (pos == "outside") ring = std::max(0.f, dil[i] / 255.f - a);
                else if (pos == "inside") ring = std::max(0.f, a - ero[i] / 255.f);
                else ring = std::max(0.f, dil[i] / 255.f - ero[i] / 255.f);
                ring *= op * c.a;
                const uint8_t* s = img.at(x, y);
                uint8_t* d = out.at(x, y);
                // Outline drawn over original (inside/center) or under (outside).
                float sr = s[0] / 255.f, sg = s[1] / 255.f, sb = s[2] / 255.f, sa = s[3] / 255.f;
                float orr, og, ob, oa;
                if (pos == "outside") { orr = sr + c.r * ring * (1 - sa); og = sg + c.g * ring * (1 - sa); ob = sb + c.b * ring * (1 - sa); oa = sa + ring * (1 - sa); }
                else { orr = c.r * ring + sr * (1 - ring); og = c.g * ring + sg * (1 - ring); ob = c.b * ring + sb * (1 - ring); oa = ring + sa * (1 - ring); }
                d[0] = (uint8_t)(clampv(orr, 0.f, 1.f) * 255); d[1] = (uint8_t)(clampv(og, 0.f, 1.f) * 255);
                d[2] = (uint8_t)(clampv(ob, 0.f, 1.f) * 255); d[3] = (uint8_t)(clampv(oa, 0.f, 1.f) * 255);
            }
        img = std::move(out);
        bounds = full;
    } else if (type == "stylize.posterize") {
        float lv = (float)std::max(2.0, P(p, "levels", 6)) - 1;
        colorOp(img, bounds, [&](float& r, float& g, float& b, float&, int, int) {
            r = std::round(r * lv) / lv; g = std::round(g * lv) / lv; b = std::round(b * lv) / lv;
        });
    } else if (type == "stylize.cartoon") {
        Rect bb = bounds;
        Image smooth = img;
        gaussianBlur(smooth, P(p, "smooth", 3) * S, bb);
        float lv = (float)std::max(2.0, P(p, "levels", 6)) - 1;
        float edges = (float)P(p, "edges", 60) / 100.f;
        Image out = smooth;
        for (int y = 1; y < H - 1; ++y)
            for (int x = 1; x < W - 1; ++x) {
                auto L = [&](int xx, int yy) { const uint8_t* q = smooth.at(xx, yy); return luma(q[0], q[1], q[2]) / 255.f; };
                float gx = L(x + 1, y) - L(x - 1, y), gy = L(x, y + 1) - L(x, y - 1);
                float e = clampv(std::sqrt(gx * gx + gy * gy) * 6 * edges, 0.f, 1.f);
                uint8_t* d = out.at(x, y);
                if (d[3] == 0) continue;
                float a = d[3] / 255.f;
                for (int k = 0; k < 3; ++k) {
                    float c = d[k] / 255.f / a;
                    c = std::round(c * lv) / lv * (1 - e);
                    d[k] = (uint8_t)(clampv(c, 0.f, 1.f) * a * 255);
                }
            }
        img = std::move(out);
        bounds = bb;
    } else if (type == "stylize.sharpen") {
        Image blur = img;
        Rect bb = bounds;
        gaussianBlur(blur, P(p, "radius", 1.5) * S * 2, bb);
        float amt = (float)P(p, "amount", 50) / 100.f;
        Rect b = bounds.intersect(full);
        for (int y = b.y0; y < b.y1; ++y)
            for (int x = b.x0; x < b.x1; ++x) {
                uint8_t* d = img.at(x, y);
                const uint8_t* s = blur.at(x, y);
                for (int k = 0; k < 3; ++k) {
                    int v = (int)std::lround(d[k] + (d[k] - s[k]) * amt);
                    d[k] = (uint8_t)clampv(v, 0, (int)d[3]);
                }
            }
    } else if (type == "stylize.emboss" || type == "stylize.edgeDetect") {
        Image src = img;
        bool emb = type == "stylize.emboss";
        double ang = deg2rad(P(p, "angle", 45));
        double hgt = P(p, "height", 2) * S;
        float amt = (float)P(p, emb ? "amount" : "strength", 100) / 100.f;
        bool inv = PB(p, "invert", false);
        int ox = (int)std::round(std::cos(ang) * std::max(1.0, hgt)), oy = (int)std::round(std::sin(ang) * std::max(1.0, hgt));
        parallelFor(H, [&](int s, int e) {
            for (int y = s; y < e; ++y)
                for (int x = 0; x < W; ++x) {
                    uint8_t* d = img.at(x, y);
                    if (d[3] == 0) continue;
                    auto L = [&](int xx, int yy) {
                        xx = clampv(xx, 0, W - 1); yy = clampv(yy, 0, H - 1);
                        const uint8_t* q = src.at(xx, yy);
                        return luma(q[0], q[1], q[2]) / 255.f;
                    };
                    float v;
                    if (emb) v = clampv(0.5f + (L(x + ox, y + oy) - L(x - ox, y - oy)) * 2.f, 0.f, 1.f);
                    else {
                        float gx = -L(x - 1, y - 1) - 2 * L(x - 1, y) - L(x - 1, y + 1) + L(x + 1, y - 1) + 2 * L(x + 1, y) + L(x + 1, y + 1);
                        float gy = -L(x - 1, y - 1) - 2 * L(x, y - 1) - L(x + 1, y - 1) + L(x - 1, y + 1) + 2 * L(x, y + 1) + L(x + 1, y + 1);
                        v = clampv(std::sqrt(gx * gx + gy * gy) * amt, 0.f, 1.f);
                        if (inv) v = 1 - v;
                    }
                    float a = d[3] / 255.f;
                    for (int k = 0; k < 3; ++k) {
                        float orig = d[k] / 255.f;
                        float nv = v * a;
                        d[k] = (uint8_t)(clampv(emb ? orig + (nv - orig) * amt : nv, 0.f, 1.f) * 255);
                    }
                }
        }, 8);
    } else if (type == "stylize.halftone") {
        double ds = std::max(2.0, P(p, "dotSize", 8) * S), ang = deg2rad(P(p, "angle", 45));
        bool colorDots = PB(p, "color", false);
        double ca = std::cos(ang), sa = std::sin(ang);
        Image src = img;
        Rect bb = bounds.intersect(full);
        for (int y = bb.y0; y < bb.y1; ++y)
            for (int x = bb.x0; x < bb.x1; ++x) {
                uint8_t* d = img.at(x, y);
                if (src.at(x, y)[3] == 0) continue;
                double u = (x * ca + y * sa) / ds, v = (-x * sa + y * ca) / ds;
                double cu = std::floor(u) + 0.5, cv = std::floor(v) + 0.5;
                double cxp = (cu * ca - cv * sa) * ds, cyp = (cu * sa + cv * ca) * ds;
                float px[4];
                sampleBilinear(src, cxp, cyp, px);
                float a = px[3] / 255.f;
                float l = a > 0 ? luma(px[0], px[1], px[2]) / 255.f / a : 0;
                double rad = std::sqrt(1 - l) * 0.7071;
                double dist = std::sqrt((u - cu) * (u - cu) + (v - cv) * (v - cv));
                float cov = (float)clampv((rad - dist) * ds + 0.5, 0.0, 1.0);
                float sa0 = src.at(x, y)[3] / 255.f;
                float base = 1.f - cov;  // paper white, ink black
                if (colorDots && a > 0) {
                    for (int k = 0; k < 3; ++k) d[k] = (uint8_t)(clampv(px[k] / 255.f / a * cov + (1 - cov), 0.f, 1.f) * sa0 * 255);
                } else {
                    for (int k = 0; k < 3; ++k) d[k] = (uint8_t)(base * sa0 * 255);
                }
            }
    } else if (type == "stylize.pixelate") {
        int bs = std::max(1, (int)std::round(P(p, "blockSize", 16) * S));
        if (bs <= 1) return;
        Rect b = bounds.intersect(full);
        for (int by = (b.y0 / bs) * bs; by < b.y1; by += bs)
            for (int bx = (b.x0 / bs) * bs; bx < b.x1; bx += bs) {
                uint32_t acc[4] = {0, 0, 0, 0};
                int n = 0;
                for (int y = std::max(0, by); y < std::min(H, by + bs); ++y)
                    for (int x = std::max(0, bx); x < std::min(W, bx + bs); ++x) {
                        const uint8_t* q = img.at(x, y);
                        for (int k = 0; k < 4; ++k) acc[k] += q[k];
                        ++n;
                    }
                if (!n) continue;
                for (int y = std::max(0, by); y < std::min(H, by + bs); ++y)
                    for (int x = std::max(0, bx); x < std::min(W, bx + bs); ++x)
                        for (int k = 0; k < 4; ++k) img.at(x, y)[k] = (uint8_t)(acc[k] / n);
            }
    } else if (type == "stylize.dropShadow") {
        Color c = PC(p, "color", Color(0, 0, 0));
        float op = (float)P(p, "opacity", 60) / 100.f;
        double ang = deg2rad(P(p, "angle", 135) - 90), dist = P(p, "distance", 15) * S, soft = P(p, "softness", 10) * S;
        int dx = (int)std::round(std::cos(ang) * dist), dy = (int)std::round(std::sin(ang) * dist);
        Image sh(W, H);
        Rect b = bounds.intersect(full);
        for (int y = b.y0; y < b.y1; ++y)
            for (int x = b.x0; x < b.x1; ++x) {
                int tx = x + dx, ty = y + dy;
                if (tx < 0 || ty < 0 || tx >= W || ty >= H) continue;
                float a = img.at(x, y)[3] / 255.f * op * c.a;
                uint8_t* d = sh.at(tx, ty);
                d[0] = (uint8_t)(c.r * a * 255); d[1] = (uint8_t)(c.g * a * 255); d[2] = (uint8_t)(c.b * a * 255); d[3] = (uint8_t)(a * 255);
            }
        Rect sb = Rect{b.x0 + dx, b.y0 + dy, b.x1 + dx, b.y1 + dy}.intersect(full);
        gaussianBlur(sh, soft, sb);
        compositeImage(sh, img, BlendMode::Normal, 1.f, b);
        img = std::move(sh);
        bounds = sb.unite(b);
    }
    // ---------------------------------------------------------------- glitch
    else if (type == "glitch.rgbSplit" || type == "glitch.chromatic") {
        Image src = img;
        bool radial = type == "glitch.chromatic";
        double amt = P(p, "amount", radial ? 5 : 8) * S, ang = deg2rad(P(p, "angle", 0));
        double dx = std::cos(ang) * amt, dy = std::sin(ang) * amt;
        double cx = W / 2.0, cy = H / 2.0;
        parallelFor(H, [&](int s, int e) {
            float pr[4], pb[4], pg[4];
            for (int y = s; y < e; ++y)
                for (int x = 0; x < W; ++x) {
                    double ox = dx, oy = dy;
                    if (radial) {
                        double vx = (x - cx) / cx, vy = (y - cy) / cy;
                        ox = vx * amt; oy = vy * amt;
                    }
                    sampleBilinear(src, x + 0.5 + ox, y + 0.5 + oy, pr);
                    sampleBilinear(src, x + 0.5, y + 0.5, pg);
                    sampleBilinear(src, x + 0.5 - ox, y + 0.5 - oy, pb);
                    uint8_t* d = img.at(x, y);
                    float a = std::max({pr[3], pg[3], pb[3]});
                    d[0] = (uint8_t)std::min(a, pr[0]); d[1] = (uint8_t)std::min(a, pg[1]); d[2] = (uint8_t)std::min(a, pb[2]); d[3] = (uint8_t)a;
                }
        }, 8);
        bounds = full;
    } else if (type == "glitch.scanlines") {
        double sp = std::max(2.0, P(p, "spacing", 4) * S);
        float in = (float)P(p, "intensity", 30) / 100.f;
        double off = env.compTime * P(p, "speed", 0) * S;
        colorOp(img, bounds, [&](float& r, float& g, float& b, float&, int, int y) {
            double ph = std::fmod(y + off, sp) / sp;
            float k = ph < 0.5 ? 1.f : 1.f - in;
            r *= k; g *= k; b *= k;
        });
    } else if (type == "glitch.blockDisplace") {
        double amt = P(p, "amount", 30) * S, bs = std::max(2.0, P(p, "blockSize", 40) * S), dens = P(p, "density", 30) / 100.0;
        uint32_t seed = (uint32_t)P(p, "seed", 1) + (uint32_t)std::floor(env.compTime * P(p, "speed", 10)) * 7919u;
        Image src = img;
        img.clear();
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                uint32_t bx = (uint32_t)(x / bs), by = (uint32_t)(y / (bs * 0.5));
                uint32_t h = hash32(bx * 73856093u ^ by * 19349663u ^ seed);
                double sx = x, sy = y;
                if ((h & 0xFFFF) / 65535.0 < dens) sx += ((hash32(h) & 0xFFFF) / 65535.0 * 2 - 1) * amt;
                int ix = clampv((int)sx, 0, W - 1);
                std::memcpy(img.at(x, y), src.at(ix, y), 4);
            }
        }
        bounds = full;
    } else if (type == "glitch.noise") {
        float amt = (float)P(p, "amount", 20) / 100.f;
        bool mono = PB(p, "monochrome", true), anim = PB(p, "animated", true);
        uint32_t seed = anim ? (uint32_t)std::lround(env.compTime * env.fps) * 2654435761u : 1u;
        colorOp(img, bounds, [&](float& r, float& g, float& b, float&, int x, int y) {
            uint32_t h = hash32((uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ seed);
            float n = ((h & 0xFFFF) / 65535.f - 0.5f) * amt;
            if (mono) { r += n; g += n; b += n; }
            else {
                r += n;
                g += ((hash32(h) & 0xFFFF) / 65535.f - 0.5f) * amt;
                b += ((hash32(h + 1) & 0xFFFF) / 65535.f - 0.5f) * amt;
            }
        });
    } else if (type == "glitch.tear") {
        double amt = P(p, "amount", 40) * S;
        int slices = std::max(1, (int)P(p, "slices", 12));
        uint32_t seed = (uint32_t)P(p, "seed", 1) + (uint32_t)std::floor(env.compTime * P(p, "speed", 8)) * 104729u;
        Image src = img;
        img.clear();
        for (int y = 0; y < H; ++y) {
            int sl = (int)((double)y / H * slices);
            uint32_t h = hash32((uint32_t)sl * 2246822519u ^ seed);
            double off = (((h & 0xFFFF) / 65535.0) * 2 - 1) * amt * (((h >> 16) & 3) == 0 ? 1.0 : 0.15);
            for (int x = 0; x < W; ++x) {
                int sx = (int)std::floor(x - off);
                sx = ((sx % W) + W) % W;
                std::memcpy(img.at(x, y), src.at(sx, y), 4);
            }
        }
        bounds = full;
    } else if (type == "glitch.compression") {
        int bs = std::max(2, (int)std::round(P(p, "blockSize", 8) * S));
        float q = (float)clampv(P(p, "quality", 30), 1.0, 100.0) / 100.f;
        float levels = 2 + q * 30;
        Rect b = bounds.intersect(full);
        for (int by = (b.y0 / bs) * bs; by < b.y1; by += bs)
            for (int bx = (b.x0 / bs) * bs; bx < b.x1; bx += bs) {
                float acc[4] = {0, 0, 0, 0};
                int n = 0;
                for (int y = by; y < std::min(H, by + bs); ++y)
                    for (int x = bx; x < std::min(W, bx + bs); ++x) {
                        const uint8_t* s = img.at(x, y);
                        for (int k = 0; k < 4; ++k) acc[k] += s[k];
                        ++n;
                    }
                for (int k = 0; k < 4; ++k) acc[k] /= std::max(1, n);
                for (int y = by; y < std::min(H, by + bs); ++y)
                    for (int x = bx; x < std::min(W, bx + bs); ++x) {
                        uint8_t* d = img.at(x, y);
                        for (int k = 0; k < 3; ++k) {
                            float detail = d[k] - acc[k];
                            float qd = std::round(detail / 255.f * levels) / levels * 255.f;
                            d[k] = (uint8_t)clampv(acc[k] + qd, 0.f, (float)d[3]);
                        }
                    }
            }
    }
    // ---------------------------------------------------------------- lighting
    else if (type == "light.lensFlare") {
        Vec2 c = ptPx("center", {0.3, 0.3});
        float in = (float)P(p, "intensity", 100) / 100.f;
        Color col = PC(p, "color", Color(1, 0.85f, 0.6f));
        double cx = W / 2.0, cy = H / 2.0;
        double diag = std::sqrt((double)W * W + (double)H * H);
        struct Ghost { double t, r; float a; Color c; };
        std::vector<Ghost> ghosts = {{0.0, 0.10, 0.9f, col}, {0.5, 0.03, 0.35f, Color(0.6f, 1, 0.7f)}, {0.8, 0.06, 0.25f, Color(0.5f, 0.7f, 1)},
                                     {1.3, 0.02, 0.4f, Color(1, 0.6f, 0.4f)}, {1.6, 0.09, 0.18f, Color(0.8f, 0.6f, 1)}};
        parallelFor(H, [&](int s, int e) {
            for (int y = s; y < e; ++y)
                for (int x = 0; x < W; ++x) {
                    float ar = 0, ag = 0, ab = 0;
                    for (auto& g : ghosts) {
                        double gx = c.x + (cx - c.x) * g.t * 2, gy = c.y + (cy - c.y) * g.t * 2;
                        double d = std::sqrt((x - gx) * (x - gx) + (y - gy) * (y - gy)) / (diag * g.r);
                        float k = g.t == 0 ? (float)(std::exp(-d * d * 2) + 0.15 * std::exp(-d * 0.5)) : (float)(d < 1 ? (1 - d * d) * 0.6 + 0.4 * (d > 0.85) : 0);
                        k *= g.a * in;
                        ar += g.c.r * k; ag += g.c.g * k; ab += g.c.b * k;
                    }
                    // Horizontal streak.
                    double sd = std::fabs(y - c.y) / (H * 0.004 + 1), sx = std::fabs(x - c.x) / (W * 0.5);
                    float st = (float)(std::exp(-sd * sd) * std::max(0.0, 1 - sx) * 0.5 * in);
                    ar += col.r * st; ag += col.g * st; ab += col.b * st;
                    uint8_t* d = img.at(x, y);
                    d[0] = (uint8_t)std::min(255.f, d[0] + ar * 255); d[1] = (uint8_t)std::min(255.f, d[1] + ag * 255);
                    d[2] = (uint8_t)std::min(255.f, d[2] + ab * 255);
                    d[3] = (uint8_t)std::max<int>(d[3], std::max({d[0], d[1], d[2]}));
                }
        }, 8);
        bounds = full;
    } else if (type == "light.rays") {
        Vec2 c = ptPx("center", {0.5, 0.3});
        double len = P(p, "length", 40) / 100.0, th = P(p, "threshold", 50) / 100.0;
        float in = (float)P(p, "intensity", 100) / 100.f;
        Image bright(W, H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const uint8_t* s = img.at(x, y);
                float l = luma(s[0], s[1], s[2]) / 255.f;
                float k = (float)clampv((l - th) / std::max(1e-3, 1 - th), 0.0, 1.0);
                uint8_t* d = bright.at(x, y);
                for (int q = 0; q < 4; ++q) d[q] = (uint8_t)(s[q] * k);
            }
        int samples = 32;
        parallelFor(H, [&](int s, int e) {
            float px[4];
            for (int y = s; y < e; ++y)
                for (int x = 0; x < W; ++x) {
                    float acc[3] = {0, 0, 0};
                    double dx = x + 0.5 - c.x, dy = y + 0.5 - c.y;
                    for (int i = 0; i < samples; ++i) {
                        double k = 1 - len * i / samples;
                        sampleBilinear(bright, c.x + dx * k, c.y + dy * k, px);
                        float w = 1.f - (float)i / samples;
                        for (int q = 0; q < 3; ++q) acc[q] += px[q] * w;
                    }
                    uint8_t* d = img.at(x, y);
                    for (int q = 0; q < 3; ++q) d[q] = (uint8_t)std::min(255.f, d[q] + acc[q] / samples * in * 2);
                    d[3] = (uint8_t)std::max<int>(d[3], std::max({d[0], d[1], d[2]}));
                }
        }, 8);
        bounds = full;
    } else if (type == "light.vignette") {
        float amt = (float)P(p, "amount", 50) / 100.f;
        double rad = P(p, "radius", 75) / 100.0, soft = std::max(0.01, P(p, "softness", 50) / 100.0);
        Color c = PC(p, "color", Color(0, 0, 0));
        double cx = W / 2.0, cy = H / 2.0;
        colorOp(img, full, [&](float& r, float& g, float& b, float&, int x, int y) {
            double dx = (x + 0.5 - cx) / cx, dy = (y + 0.5 - cy) / cy;
            double d = std::sqrt(dx * dx + dy * dy) / 1.41421;
            float k = (float)smoothstep(rad - soft * 0.5, rad + soft * 0.5, d) * amt;
            r += (c.r - r) * k; g += (c.g - g) * k; b += (c.b - b) * k;
        });
    } else if (type == "light.sheen") {
        double ang = deg2rad(P(p, "angle", 30)), pos = P(p, "position", 50) / 100.0, wdt = std::max(0.01, P(p, "width", 15) / 100.0);
        float in = (float)P(p, "intensity", 60) / 100.f;
        double ca = std::cos(ang), sa = std::sin(ang);
        double extent = std::fabs(W * ca) + std::fabs(H * sa);
        colorOp(img, bounds, [&](float& r, float& g, float& b, float&, int x, int y) {
            double u = ((x - W / 2.0) * ca + (y - H / 2.0) * sa) / extent + 0.5;
            double d = std::fabs(u - pos) / wdt;
            float k = d < 1 ? (float)(1 - d * d) * in : 0.f;
            r += (1 - r) * k; g += (1 - g) * k; b += (1 - b) * k;
        });
    }
    // ---------------------------------------------------------------- keying
    else if (type == "key.chroma") {
        Color kc = PC(p, "keyColor", Color(0, 1, 0));
        float tol = (float)P(p, "tolerance", 30) / 100.f, soft = (float)P(p, "softness", 10) / 100.f, spill = (float)P(p, "spill", 50) / 100.f;
        std::string view = PS(p, "view", "composite");
        if (view == "source") return;
        // Key in a chroma plane (YCbCr) so brightness variations of the screen are tolerated.
        auto cbcr = [](float r, float g, float b, float& cb, float& cr) {
            cb = -0.168736f * r - 0.331264f * g + 0.5f * b;
            cr = 0.5f * r - 0.418688f * g - 0.081312f * b;
        };
        float kcb, kcr;
        cbcr(kc.r, kc.g, kc.b, kcb, kcr);
        int dom = (kc.g >= kc.r && kc.g >= kc.b) ? 1 : (kc.b >= kc.r ? 2 : 0);
        colorOp(img, bounds, [&](float& r, float& g, float& b, float& a, int, int) {
            float cb, cr;
            cbcr(r, g, b, cb, cr);
            float d = std::sqrt((cb - kcb) * (cb - kcb) + (cr - kcr) * (cr - kcr));
            float m = (float)smoothstep(tol * 0.5, tol * 0.5 + soft * 0.5 + 1e-3, d);
            // Spill suppression: clamp dominant key channel to average of others.
            if (spill > 0) {
                float* ch[3] = {&r, &g, &b};
                float o1 = *ch[(dom + 1) % 3], o2 = *ch[(dom + 2) % 3];
                float lim = (o1 + o2) / 2;
                if (*ch[dom] > lim) *ch[dom] -= (*ch[dom] - lim) * spill;
            }
            if (view == "alpha" || view == "matte") { float v = m * a; r = g = b = 1; a = view == "matte" ? 1 : v; if (view == "matte") r = g = b = v; return; }
            a *= m;
        });
        int choke = (int)std::round(P(p, "choke", 0) * S);
        double feather = P(p, "edgeFeather", 0) * S;
        if (choke != 0 || feather > 0.5) {
            Params mp = {{"choke", Value::number(choke / std::max(1e-6, S))}, {"feather", Value::number(feather / std::max(1e-6, S))}};
            applyEffect("key.matteChoker", mp, img, bounds, env);
        }
    } else if (type == "key.luma") {
        float th = (float)P(p, "threshold", 10) / 100.f, soft = (float)P(p, "softness", 10) / 100.f;
        bool bright = PS(p, "keyOut", "dark") == "bright";
        colorOp(img, bounds, [&](float& r, float& g, float& b, float& a, int, int) {
            float l = luma(r, g, b);
            float m = bright ? 1.f - (float)smoothstep(1 - th - soft, 1 - th, l) : (float)smoothstep(th, th + soft + 1e-3, l);
            a *= m;
        });
    } else if (type == "key.matteChoker") {
        int ch = (int)std::round(P(p, "choke", 0) * S);
        double fe = P(p, "feather", 0) * S;
        if (ch != 0) {
            std::vector<uint8_t> m;
            alphaMorph(img, std::abs(ch), ch < 0, m);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    uint8_t* d = img.at(x, y);
                    uint8_t na = m[(size_t)y * W + x];
                    if (ch > 0 && na < d[3]) {
                        float k = d[3] ? na / (float)d[3] : 0;
                        for (int q = 0; q < 4; ++q) d[q] = (uint8_t)(d[q] * k);
                    } else if (ch < 0 && na > d[3]) {
                        d[3] = std::max(d[3], na);
                    }
                }
        }
        if (fe > 0.5) {
            // Feather alpha only: blur a copy, keep color but scale by blurred alpha ratio.
            Image a = img;
            Rect bb = bounds;
            gaussianBlur(a, fe, bb);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    uint8_t* d = img.at(x, y);
                    float na = a.at(x, y)[3];
                    if (d[3] == 0) { if (na > 0) { std::memcpy(d, a.at(x, y), 4); } continue; }
                    float k = na / d[3];
                    if (k < 1) for (int q = 0; q < 4; ++q) d[q] = (uint8_t)(d[q] * k);
                }
            bounds = bb;
        }
    } else if (type == "key.channel") {
        std::string ch = PS(p, "channel", "alpha");
        bool inv = PB(p, "invert", false);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                uint8_t* d = img.at(x, y);
                if (d[3] == 0 && !inv) continue;
                float a = d[3] / 255.f;
                float r = a > 0 ? d[0] / 255.f / a : 0, g = a > 0 ? d[1] / 255.f / a : 0, b = a > 0 ? d[2] / 255.f / a : 0;
                float m = ch == "red" ? r : ch == "green" ? g : ch == "blue" ? b : ch == "luminance" ? luma(r, g, b) : a;
                if (inv) m = 1 - m;
                d[0] = (uint8_t)(r * m * 255); d[1] = (uint8_t)(g * m * 255); d[2] = (uint8_t)(b * m * 255); d[3] = (uint8_t)(m * 255);
            }
    }
    // ---------------------------------------------------------------- utility
    else if (type == "util.transform") {
        double tx = P(p, "x", 0) * S, ty = P(p, "y", 0) * S, sc = P(p, "scale", 100) / 100.0, rot = deg2rad(P(p, "rotation", 0));
        float op = (float)P(p, "opacity", 100) / 100.f;
        if (std::fabs(sc) < 1e-6) { img.clear(); return; }
        double cx = W / 2.0, cy = H / 2.0;
        // dst -> src : translate(-t) rotate(-r) scale(1/s) about center
        double c = std::cos(-rot), s = std::sin(-rot);
        Mat3 m;
        m.m[0] = c / sc; m.m[1] = -s / sc; m.m[2] = cx - (c * (cx + tx) - s * (cy + ty)) / sc;
        m.m[3] = s / sc; m.m[4] = c / sc; m.m[5] = cy - (s * (cx + tx) + c * (cy + ty)) / sc;
        Image src = img;
        img.clear();
        warpImage(img, src, m, op, full, 1);
        bounds = full;
    } else if (type == "util.crop") {
        double l = P(p, "left", 0) / 100 * W, t = P(p, "top", 0) / 100 * H, r = W - P(p, "right", 0) / 100 * W, b = H - P(p, "bottom", 0) / 100 * H;
        double fe = P(p, "feather", 0) * S;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                double d = std::min({x + 0.5 - l, r - x - 0.5, y + 0.5 - t, b - y - 0.5});
                float m = fe > 0.5 ? (float)clampv(d / fe, 0.0, 1.0) : (d > 0 ? 1.f : 0.f);
                if (m >= 1) continue;
                uint8_t* q = img.at(x, y);
                for (int k = 0; k < 4; ++k) q[k] = (uint8_t)(q[k] * m);
            }
    } else if (type == "util.fill") {
        Color c = PC(p, "color", Color(1, 0, 0));
        float op = (float)P(p, "opacity", 100) / 100.f;
        colorOp(img, bounds, [&](float& r, float& g, float& b, float&, int, int) {
            r += (c.r - r) * op; g += (c.g - g) * op; b += (c.b - b) * op;
        });
    } else if (type == "util.opacity") {
        float op = (float)P(p, "opacity", 100) / 100.f;
        for (auto& v : img.px) v = (uint8_t)(v * op + 0.5f);
    } else if (type == "util.colorSpace") {
        bool toLin = PS(p, "mode", "srgbToLinear") == "srgbToLinear";
        colorOp(img, bounds, [&](float& r, float& g, float& b, float&, int, int) {
            if (toLin) { r = srgbToLinear(r); g = srgbToLinear(g); b = srgbToLinear(b); }
            else { r = linearToSrgb(r); g = linearToSrgb(g); b = linearToSrgb(b); }
        });
    }
    // ---------------------------------------------------------------- generate
    else if (type == "generate.gradient") {
        Vec2 a = ptPx("start", {0.5, 0}), b = ptPx("end", {0.5, 1});
        Color c0 = PC(p, "startColor", Color(0, 0, 0)), c1 = PC(p, "endColor", Color(1, 1, 1));
        bool radial = PS(p, "shape", "linear") == "radial";
        float blend = (float)P(p, "blend", 0) / 100.f;
        Vec2 d = b - a;
        double l2 = std::max(1e-9, d.dot(d)), len = std::sqrt(l2);
        colorOp(img, bounds, [&](float& r, float& g, float& bl, float&, int x, int y) {
            Vec2 q{x + 0.5, y + 0.5};
            float t = (float)clampv(radial ? (q - a).length() / len : (q - a).dot(d) / l2, 0.0, 1.0);
            float nr = c0.r + (c1.r - c0.r) * t, ng = c0.g + (c1.g - c0.g) * t, nb = c0.b + (c1.b - c0.b) * t;
            r = nr + (r - nr) * blend; g = ng + (g - ng) * blend; bl = nb + (bl - nb) * blend;
        });
    } else if (type == "generate.fractalNoise") {
        double sc = std::max(1.0, P(p, "scale", 100) * S);
        int oct = clampv((int)P(p, "complexity", 4), 1, 8);
        double evo = deg2rad(P(p, "evolution", 0));
        float con = (float)P(p, "contrast", 100) / 100.f, bri = (float)P(p, "brightness", 0) / 100.f, op = (float)P(p, "opacity", 100) / 100.f;
        uint32_t seed = (uint32_t)P(p, "seed", 1);
        double ex = std::cos(evo) * 2, ey = std::sin(evo) * 2;
        colorOp(img, bounds, [&](float& r, float& g, float& b, float&, int x, int y) {
            float n = (float)fbm2D(x / sc + ex, y / sc + ey, seed, oct) * 0.5f + 0.5f;
            n = clampv((n - 0.5f) * con + 0.5f + bri, 0.f, 1.f);
            r += (n - r) * op; g += (n - g) * op; b += (n - b) * op;
        });
    } else if (type == "generate.spectrum" || type == "generate.waveform") {
        bool spec = type == "generate.spectrum";
        Vec2 c = ptPx("position", {0.5, spec ? 0.85 : 0.5});
        double width = P(p, "width", 80) / 100.0 * W;
        double height = P(p, "height", spec ? 300 : 150) * S;
        Color col = PC(p, "color", Color(1, 1, 1));
        Polys polys;
        if (spec) {
            int bands = clampv((int)P(p, "bands", 32), 4, 128);
            std::vector<float> mags = env.spectrum ? env.spectrum(env.compTime, bands) : std::vector<float>(bands, 0.f);
            std::string style = PS(p, "style", "bars");
            double bw = width / bands;
            if (style == "line") {
                Contour ct;
                ct.closed = false;
                for (int i = 0; i < bands; ++i) ct.pts.push_back({c.x - width / 2 + (i + 0.5) * bw, c.y - mags[i] * height});
                polys = strokePolys({ct}, std::max(1.0, 3 * S), LineCap::Round, LineJoin::Round);
            } else {
                for (int i = 0; i < bands; ++i) {
                    double x0 = c.x - width / 2 + i * bw + bw * 0.15, x1 = x0 + bw * 0.7;
                    double hgt = std::max(1.0 * S, (double)mags[i] * height);
                    if (style == "dots") {
                        double r = bw * 0.35;
                        Contour ct;
                        for (int k = 0; k < 16; ++k) ct.pts.push_back({(x0 + x1) / 2 + std::cos(k * kPi / 8) * r, c.y - hgt + std::sin(k * kPi / 8) * r});
                        polys.push_back(ct);
                    } else {
                        polys.push_back({{{x0, c.y - hgt}, {x1, c.y - hgt}, {x1, c.y}, {x0, c.y}}, true});
                    }
                }
            }
        } else {
            int n = 256;
            std::vector<float> wave = env.waveform ? env.waveform(env.compTime, n, P(p, "window", 60) / 1000.0) : std::vector<float>(n, 0.f);
            Contour ct;
            ct.closed = false;
            for (int i = 0; i < (int)wave.size(); ++i) ct.pts.push_back({c.x - width / 2 + width * i / (wave.size() - 1), c.y - wave[i] * height});
            polys = strokePolys({ct}, std::max(1.0, P(p, "thickness", 3) * S), LineCap::Round, LineJoin::Round);
        }
        Coverage cov = rasterize(polys, full);
        Paint paint;
        paint.color = col;
        fillCoverage(img, cov, paint);
        bounds = bounds.unite(cov.r);
    }
    // time.* effects are handled by the renderer (they need other frames).
}

}  // namespace mf
