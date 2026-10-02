#include "mf/imageops.hpp"

#include "mf/threadpool.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_GIF
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "../../third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../third_party/stb/stb_image_write.h"

namespace mf {

static inline float lum(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

static void setLum(float& r, float& g, float& b, float l) {
    float d = l - lum(r, g, b);
    r += d; g += d; b += d;
    float L = lum(r, g, b);
    float n = std::min({r, g, b}), x = std::max({r, g, b});
    if (n < 0) { r = L + (r - L) * L / (L - n + 1e-6f); g = L + (g - L) * L / (L - n + 1e-6f); b = L + (b - L) * L / (L - n + 1e-6f); }
    if (x > 1) { r = L + (r - L) * (1 - L) / (x - L + 1e-6f); g = L + (g - L) * (1 - L) / (x - L + 1e-6f); b = L + (b - L) * (1 - L) / (x - L + 1e-6f); }
}
static float sat(float r, float g, float b) { return std::max({r, g, b}) - std::min({r, g, b}); }
static void setSat(float& r, float& g, float& b, float s) {
    float* c[3] = {&r, &g, &b};
    std::sort(c, c + 3, [](float* a, float* bb) { return *a < *bb; });
    float mn = *c[0], md = *c[1], mx = *c[2];
    if (mx > mn) { *c[1] = (md - mn) * s / (mx - mn); *c[2] = s; }
    else { *c[1] = 0; *c[2] = 0; }
    *c[0] = 0;
}

// Separable blend function on straight (unpremultiplied) colors.
static inline float blendCh(BlendMode m, float s, float d) {
    switch (m) {
        case BlendMode::Multiply: return s * d;
        case BlendMode::Screen: return s + d - s * d;
        case BlendMode::Overlay: return d <= 0.5f ? 2 * s * d : 1 - 2 * (1 - s) * (1 - d);
        case BlendMode::HardLight: return s <= 0.5f ? 2 * s * d : 1 - 2 * (1 - s) * (1 - d);
        case BlendMode::SoftLight: {
            if (s <= 0.5f) return d - (1 - 2 * s) * d * (1 - d);
            float g = d <= 0.25f ? ((16 * d - 12) * d + 4) * d : std::sqrt(d);
            return d + (2 * s - 1) * (g - d);
        }
        case BlendMode::Darken: return std::min(s, d);
        case BlendMode::Lighten: return std::max(s, d);
        case BlendMode::ColorDodge: return d <= 0 ? 0 : (s >= 1 ? 1 : std::min(1.f, d / (1 - s)));
        case BlendMode::ColorBurn: return d >= 1 ? 1 : (s <= 0 ? 0 : 1 - std::min(1.f, (1 - d) / s));
        case BlendMode::Difference: return std::fabs(s - d);
        case BlendMode::Exclusion: return s + d - 2 * s * d;
        case BlendMode::Add: return std::min(1.f, s + d);
        case BlendMode::Subtract: return std::max(0.f, d - s);
        default: return s;
    }
}

void compositeImage(Image& dst, const Image& src, BlendMode mode, float opacity, const Rect& region) {
    Rect r = region.intersect({0, 0, std::min(dst.w, src.w), std::min(dst.h, src.h)});
    if (r.empty() || opacity <= 0) return;
    parallelFor(r.height(), [&](int b, int e) {
        for (int y = r.y0 + b; y < r.y0 + e; ++y) {
            uint8_t* d = dst.row(y) + r.x0 * 4;
            const uint8_t* s = src.row(y) + r.x0 * 4;
            for (int x = r.x0; x < r.x1; ++x, d += 4, s += 4) {
                if (s[3] == 0) continue;
                float sa = s[3] / 255.f * opacity;
                float sr = s[0] / 255.f * opacity, sg = s[1] / 255.f * opacity, sb = s[2] / 255.f * opacity;
                float da = d[3] / 255.f;
                float dr = d[0] / 255.f, dg = d[1] / 255.f, db = d[2] / 255.f;
                float orr, og, ob, oa;
                if (mode == BlendMode::Normal) {
                    orr = sr + dr * (1 - sa); og = sg + dg * (1 - sa); ob = sb + db * (1 - sa); oa = sa + da * (1 - sa);
                } else if (mode == BlendMode::Add) {
                    orr = std::min(1.f, sr + dr); og = std::min(1.f, sg + dg); ob = std::min(1.f, sb + db); oa = std::min(1.f, sa + da);
                } else {
                    // Unpremultiply, blend, then W3C compositing formula.
                    float Sr = sr / sa, Sg = sg / sa, Sb = sb / sa;
                    float Dr = da > 0 ? dr / da : 0, Dg = da > 0 ? dg / da : 0, Db = da > 0 ? db / da : 0;
                    float Br, Bg, Bb;
                    if (mode == BlendMode::Hue || mode == BlendMode::Saturation || mode == BlendMode::Color || mode == BlendMode::Luminosity) {
                        Br = Sr; Bg = Sg; Bb = Sb;
                        if (mode == BlendMode::Hue) { Br = Sr; Bg = Sg; Bb = Sb; setSat(Br, Bg, Bb, sat(Dr, Dg, Db)); setLum(Br, Bg, Bb, lum(Dr, Dg, Db)); }
                        else if (mode == BlendMode::Saturation) { Br = Dr; Bg = Dg; Bb = Db; setSat(Br, Bg, Bb, sat(Sr, Sg, Sb)); setLum(Br, Bg, Bb, lum(Dr, Dg, Db)); }
                        else if (mode == BlendMode::Color) { setLum(Br, Bg, Bb, lum(Dr, Dg, Db)); }
                        else { Br = Dr; Bg = Dg; Bb = Db; setLum(Br, Bg, Bb, lum(Sr, Sg, Sb)); }
                    } else {
                        Br = blendCh(mode, Sr, Dr); Bg = blendCh(mode, Sg, Dg); Bb = blendCh(mode, Sb, Db);
                    }
                    oa = sa + da * (1 - sa);
                    orr = sa * (1 - da) * Sr + sa * da * Br + (1 - sa) * dr;
                    og = sa * (1 - da) * Sg + sa * da * Bg + (1 - sa) * dg;
                    ob = sa * (1 - da) * Sb + sa * da * Bb + (1 - sa) * db;
                }
                d[0] = (uint8_t)(clampv(orr, 0.f, 1.f) * 255.f + 0.5f);
                d[1] = (uint8_t)(clampv(og, 0.f, 1.f) * 255.f + 0.5f);
                d[2] = (uint8_t)(clampv(ob, 0.f, 1.f) * 255.f + 0.5f);
                d[3] = (uint8_t)(clampv(oa, 0.f, 1.f) * 255.f + 0.5f);
            }
        }
    });
}

void applyTrackMatte(Image& img, const Image& matte, MatteMode mode, const Rect& region) {
    if (mode == MatteMode::None) return;
    Rect r = region.intersect({0, 0, std::min(img.w, matte.w), std::min(img.h, matte.h)});
    for (int y = 0; y < img.h; ++y) {
        uint8_t* d = img.row(y);
        for (int x = 0; x < img.w; ++x, d += 4) {
            float m = 0;
            if (y >= r.y0 && y < r.y1 && x >= r.x0 && x < r.x1) {
                const uint8_t* s = matte.at(x, y);
                if (mode == MatteMode::Alpha || mode == MatteMode::AlphaInverted) m = s[3] / 255.f;
                else {
                    float a = s[3] / 255.f;
                    m = a > 0 ? lum(s[0] / 255.f / a, s[1] / 255.f / a, s[2] / 255.f / a) * a : 0.f;
                }
            }
            if (mode == MatteMode::AlphaInverted || mode == MatteMode::LumaInverted) m = 1 - m;
            if (m >= 1) continue;
            for (int k = 0; k < 4; ++k) d[k] = (uint8_t)(d[k] * m + 0.5f);
        }
    }
}

void sampleBilinear(const Image& img, double x, double y, float out[4]) {
    x -= 0.5; y -= 0.5;
    int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    float fx = (float)(x - x0), fy = (float)(y - y0);
    out[0] = out[1] = out[2] = out[3] = 0;
    const float w[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
    const int xs[4] = {x0, x0 + 1, x0, x0 + 1}, ys[4] = {y0, y0, y0 + 1, y0 + 1};
    for (int i = 0; i < 4; ++i) {
        if (xs[i] < 0 || ys[i] < 0 || xs[i] >= img.w || ys[i] >= img.h || w[i] == 0) continue;
        const uint8_t* p = img.at(xs[i], ys[i]);
        out[0] += p[0] * w[i]; out[1] += p[1] * w[i]; out[2] += p[2] * w[i]; out[3] += p[3] * w[i];
    }
}

Rect warpImage(Image& dst, const Image& src, const Mat3& m, float opacity, const Rect& clip, int quality, const Rect* srcRect) {
    if (src.empty() || opacity <= 0) return {};
    Rect sr = srcRect ? *srcRect : Rect{0, 0, src.w, src.h};
    // Destination bounds = forward-mapped source corners.
    Mat3 fwd;
    if (!m.inverse(fwd)) return {};
    Vec2 corners[4] = {fwd.apply(sr.x0, sr.y0), fwd.apply(sr.x1, sr.y0), fwd.apply(sr.x1, sr.y1), fwd.apply(sr.x0, sr.y1)};
    double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
    for (auto& c : corners) { x0 = std::min(x0, c.x); y0 = std::min(y0, c.y); x1 = std::max(x1, c.x); y1 = std::max(y1, c.y); }
    // Perspective with points behind the camera can blow up: rely on clip.
    Rect r{(int)std::floor(clampv(x0, -1e7, 1e7)) - 1, (int)std::floor(clampv(y0, -1e7, 1e7)) - 1, (int)std::ceil(clampv(x1, -1e7, 1e7)) + 1,
           (int)std::ceil(clampv(y1, -1e7, 1e7)) + 1};
    r = r.intersect(clip).intersect({0, 0, dst.w, dst.h});
    if (r.empty()) return {};
    bool affine = std::fabs(m.m[6]) < 1e-12 && std::fabs(m.m[7]) < 1e-12;
    parallelFor(r.height(), [&](int b, int e) {
        float px[4];
        for (int y = r.y0 + b; y < r.y0 + e; ++y) {
            uint8_t* d = dst.row(y) + r.x0 * 4;
            for (int x = r.x0; x < r.x1; ++x, d += 4) {
                double sx, sy;
                double cx = x + 0.5, cy = y + 0.5;
                if (affine) {
                    sx = m.m[0] * cx + m.m[1] * cy + m.m[2];
                    sy = m.m[3] * cx + m.m[4] * cy + m.m[5];
                } else {
                    double w = m.m[6] * cx + m.m[7] * cy + m.m[8];
                    if (w <= 1e-9) continue;
                    sx = (m.m[0] * cx + m.m[1] * cy + m.m[2]) / w;
                    sy = (m.m[3] * cx + m.m[4] * cy + m.m[5]) / w;
                }
                if (sx < sr.x0 - 1 || sy < sr.y0 - 1 || sx > sr.x1 + 1 || sy > sr.y1 + 1) continue;
                if (quality == 0) {
                    int ix = (int)sx, iy = (int)sy;
                    if (ix < sr.x0 || iy < sr.y0 || ix >= sr.x1 || iy >= sr.y1) continue;
                    const uint8_t* p = src.at(ix, iy);
                    for (int k = 0; k < 4; ++k) px[k] = p[k];
                } else {
                    sampleBilinear(src, sx, sy, px);
                    // Clip to source rect edges (anti-aliased by bilinear fade).
                }
                float a = px[3] * opacity;
                if (a <= 0.01f) continue;
                float inv = 1.f - a / 255.f;
                d[0] = (uint8_t)std::min(255.f, px[0] * opacity + d[0] * inv + 0.5f);
                d[1] = (uint8_t)std::min(255.f, px[1] * opacity + d[1] * inv + 0.5f);
                d[2] = (uint8_t)std::min(255.f, px[2] * opacity + d[2] * inv + 0.5f);
                d[3] = (uint8_t)std::min(255.f, a + d[3] * inv + 0.5f);
            }
        }
    }, 8);
    return r;
}

// Sliding-window box blur along rows of a channel-interleaved buffer.
static void boxBlurH(Image& img, const Rect& r, int rad) {
    if (rad <= 0) return;
    parallelFor(r.height(), [&](int b, int e) {
        std::vector<uint8_t> tmp((size_t)r.width() * 4);
        for (int y = r.y0 + b; y < r.y0 + e; ++y) {
            uint8_t* row = img.row(y);
            int s[4] = {0, 0, 0, 0};
            int win = 2 * rad + 1;
            auto px = [&](int x, int k) -> int { return (x >= r.x0 && x < r.x1) ? row[x * 4 + k] : 0; };
            for (int x = r.x0 - rad; x <= r.x0 + rad; ++x)
                for (int k = 0; k < 4; ++k) s[k] += px(x, k);
            for (int x = r.x0; x < r.x1; ++x) {
                for (int k = 0; k < 4; ++k) tmp[(x - r.x0) * 4 + k] = (uint8_t)((s[k] + win / 2) / win);
                for (int k = 0; k < 4; ++k) s[k] += px(x + rad + 1, k) - px(x - rad, k);
            }
            std::memcpy(row + r.x0 * 4, tmp.data(), tmp.size());
        }
    }, 4);
}

static void boxBlurV(Image& img, const Rect& r, int rad) {
    if (rad <= 0) return;
    parallelFor(r.width(), [&](int b, int e) {
        std::vector<uint8_t> tmp((size_t)r.height() * 4);
        for (int x = r.x0 + b; x < r.x0 + e; ++x) {
            int s[4] = {0, 0, 0, 0};
            int win = 2 * rad + 1;
            auto px = [&](int y, int k) -> int { return (y >= r.y0 && y < r.y1) ? img.at(x, y)[k] : 0; };
            for (int y = r.y0 - rad; y <= r.y0 + rad; ++y)
                for (int k = 0; k < 4; ++k) s[k] += px(y, k);
            for (int y = r.y0; y < r.y1; ++y) {
                for (int k = 0; k < 4; ++k) tmp[(y - r.y0) * 4 + k] = (uint8_t)((s[k] + win / 2) / win);
                for (int k = 0; k < 4; ++k) s[k] += px(y + rad + 1, k) - px(y - rad, k);
            }
            for (int y = r.y0; y < r.y1; ++y) std::memcpy(img.at(x, y), &tmp[(y - r.y0) * 4], 4);
        }
    }, 4);
}

void gaussianBlur(Image& img, double radius, Rect& bounds, bool horizontal, bool vertical) {
    if (radius < 0.5) return;
    // Three box passes approximate a gaussian with sigma ~= radius/2.
    double sigma = radius / 2.0;
    int rad = std::max(1, (int)std::round(std::sqrt(12.0 * sigma * sigma / 3.0 + 1.0) / 2.0));
    int grow = rad * 3 + 1;
    Rect r = bounds;
    if (horizontal) { r.x0 -= grow; r.x1 += grow; }
    if (vertical) { r.y0 -= grow; r.y1 += grow; }
    r = r.intersect({0, 0, img.w, img.h});
    bounds = r;
    for (int i = 0; i < 3; ++i) {
        if (horizontal) boxBlurH(img, r, rad);
        if (vertical) boxBlurV(img, r, rad);
    }
}

void directionalBlur(Image& img, double angle, double length, Rect& bounds) {
    if (length < 0.5) return;
    int samples = clampv((int)std::ceil(length), 2, 96);
    double dx = std::cos(angle) * length, dy = std::sin(angle) * length;
    Rect r = bounds.expand((int)std::ceil(length / 2) + 1).intersect({0, 0, img.w, img.h});
    bounds = r;
    Image src = img;
    parallelFor(r.height(), [&](int b, int e) {
        float px[4];
        for (int y = r.y0 + b; y < r.y0 + e; ++y)
            for (int x = r.x0; x < r.x1; ++x) {
                float acc[4] = {0, 0, 0, 0};
                for (int s = 0; s < samples; ++s) {
                    double t = (double)s / (samples - 1) - 0.5;
                    sampleBilinear(src, x + 0.5 + dx * t, y + 0.5 + dy * t, px);
                    for (int k = 0; k < 4; ++k) acc[k] += px[k];
                }
                uint8_t* d = img.at(x, y);
                for (int k = 0; k < 4; ++k) d[k] = (uint8_t)std::min(255.f, acc[k] / samples + 0.5f);
            }
    }, 4);
}

Image resizeImage(const Image& src, int w, int h) {
    Image out(w, h);
    if (src.empty() || w <= 0 || h <= 0) return out;
    double sx = (double)src.w / w, sy = (double)src.h / h;
    bool shrink = sx > 1.5 || sy > 1.5;
    parallelFor(h, [&](int b, int e) {
        float px[4];
        for (int y = b; y < e; ++y)
            for (int x = 0; x < w; ++x) {
                uint8_t* d = out.at(x, y);
                if (shrink) {
                    int x0 = (int)(x * sx), x1 = std::max(x0 + 1, (int)((x + 1) * sx));
                    int y0 = (int)(y * sy), y1 = std::max(y0 + 1, (int)((y + 1) * sy));
                    x1 = std::min(x1, src.w); y1 = std::min(y1, src.h);
                    uint32_t acc[4] = {0, 0, 0, 0};
                    int n = 0;
                    for (int yy = y0; yy < y1; ++yy)
                        for (int xx = x0; xx < x1; ++xx) {
                            const uint8_t* p = src.at(xx, yy);
                            for (int k = 0; k < 4; ++k) acc[k] += p[k];
                            ++n;
                        }
                    for (int k = 0; k < 4; ++k) d[k] = (uint8_t)(n ? (acc[k] + n / 2) / n : 0);
                } else {
                    sampleBilinear(src, (x + 0.5) * sx, (y + 0.5) * sy, px);
                    // Edge clamp to avoid transparent fringes when upscaling opaque media.
                    if (px[3] < 255 && x > 0 && y > 0 && x < w - 1 && y < h - 1) {}
                    for (int k = 0; k < 4; ++k) d[k] = (uint8_t)std::min(255.f, px[k] + 0.5f);
                }
            }
    }, 8);
    if (!shrink) {
        // Fix border pixels by clamping coordinates (bilinear fades edges toward transparent otherwise).
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; x += (y == 0 || y == h - 1) ? 1 : std::max(1, w - 1)) {
                int sxp = clampv((int)((x + 0.5) * sx), 0, src.w - 1), syp = clampv((int)((y + 0.5) * sy), 0, src.h - 1);
                std::memcpy(out.at(x, y), src.at(sxp, syp), 4);
            }
    }
    return out;
}

Image cropImage(const Image& src, const Rect& r0) {
    Rect r = r0.intersect({0, 0, src.w, src.h});
    Image out(std::max(0, r.width()), std::max(0, r.height()));
    for (int y = r.y0; y < r.y1; ++y) std::memcpy(out.row(y - r.y0), src.at(r.x0, y), (size_t)r.width() * 4);
    return out;
}

void premultiply(Image& img) {
    for (size_t i = 0; i < img.px.size(); i += 4) {
        uint32_t a = img.px[i + 3];
        if (a == 255) continue;
        for (int k = 0; k < 3; ++k) img.px[i + k] = (uint8_t)((img.px[i + k] * a + 127) / 255);
    }
}
void unpremultiply(Image& img) {
    for (size_t i = 0; i < img.px.size(); i += 4) {
        uint32_t a = img.px[i + 3];
        if (a == 255 || a == 0) continue;
        for (int k = 0; k < 3; ++k) img.px[i + k] = (uint8_t)std::min(255u, (img.px[i + k] * 255 + a / 2) / a);
    }
}

void yuv420ToRgba(const uint8_t* yp, int yStride, const uint8_t* up, const uint8_t* vp, int uvStride, int uvPix, int w, int h, bool bt709,
                  bool full, Image& out) {
    out.resize(w, h);
    // Coefficients (fixed point 1<<12).
    double kr = bt709 ? 0.2126 : 0.299, kb = bt709 ? 0.0722 : 0.114, kg = 1 - kr - kb;
    double ys = full ? 1.0 : 255.0 / 219.0, cs = full ? 1.0 : 255.0 / 224.0;
    int cy = (int)std::lround(ys * 4096), crv = (int)std::lround(2 * (1 - kr) * cs * 4096), cbu = (int)std::lround(2 * (1 - kb) * cs * 4096);
    int cgu = (int)std::lround(2 * (1 - kb) * kb / kg * cs * 4096), cgv = (int)std::lround(2 * (1 - kr) * kr / kg * cs * 4096);
    int yoff = full ? 0 : 16;
    parallelFor(h, [&](int b, int e) {
        for (int y = b; y < e; ++y) {
            const uint8_t* yr = yp + (size_t)y * yStride;
            const uint8_t* ur = up + (size_t)(y / 2) * uvStride;
            const uint8_t* vr = vp + (size_t)(y / 2) * uvStride;
            uint8_t* d = out.row(y);
            for (int x = 0; x < w; ++x) {
                int Y = (yr[x] - yoff) * cy;
                int U = ur[(x / 2) * uvPix] - 128, V = vr[(x / 2) * uvPix] - 128;
                int R = (Y + crv * V) >> 12, G = (Y - cgu * U - cgv * V) >> 12, B = (Y + cbu * U) >> 12;
                d[0] = (uint8_t)clampv(R, 0, 255);
                d[1] = (uint8_t)clampv(G, 0, 255);
                d[2] = (uint8_t)clampv(B, 0, 255);
                d[3] = 255;
                d += 4;
            }
        }
    }, 8);
}

double meanAbsDiff(const Image& a, const Image& b) {
    if (a.w != b.w || a.h != b.h) return 1e9;
    double s = 0;
    for (size_t i = 0; i < a.px.size(); ++i) s += std::abs((int)a.px[i] - (int)b.px[i]);
    return a.px.empty() ? 0 : s / a.px.size();
}

double psnr(const Image& a, const Image& b) {
    if (a.w != b.w || a.h != b.h) return 0;
    double mse = 0;
    for (size_t i = 0; i < a.px.size(); ++i) {
        double d = (double)a.px[i] - b.px[i];
        mse += d * d;
    }
    mse /= std::max<size_t>(1, a.px.size());
    if (mse < 1e-12) return 99.0;
    return 10 * std::log10(255.0 * 255.0 / mse);
}

uint64_t imageHash(const Image& img) {
    uint64_t h = 1469598103934665603ULL;
    for (uint8_t v : img.px) { h ^= v; h *= 1099511628211ULL; }
    h ^= (uint64_t)img.w << 32 | (uint32_t)img.h;
    return h;
}

bool savePng(const Image& img, const std::string& path) {
    Image c = img;
    unpremultiply(c);
    return stbi_write_png(path.c_str(), c.w, c.h, 4, c.px.data(), c.w * 4) != 0;
}

bool loadImageMemory(const uint8_t* data, size_t size, Image& out, std::string* err) {
    int w, h, n;
    unsigned char* p = stbi_load_from_memory(data, (int)size, &w, &h, &n, 4);
    if (!p) { if (err) *err = stbi_failure_reason() ? stbi_failure_reason() : "decode failed"; return false; }
    out.resize(w, h);
    std::memcpy(out.px.data(), p, (size_t)w * h * 4);
    stbi_image_free(p);
    premultiply(out);
    return true;
}

bool loadImageFile(const std::string& path, Image& out, std::string* err) {
    bool ok;
    auto bytes = readFileBytes(path, &ok);
    if (!ok || bytes.empty()) { if (err) *err = "cannot read " + path; return false; }
    return loadImageMemory(bytes.data(), bytes.size(), out, err);
}

}  // namespace mf
