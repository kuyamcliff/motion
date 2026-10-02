// MOTIONFORGE engine — shared primitives (math, images, logging).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../../../third_party/json.hpp"

namespace mf {

using json = nlohmann::json;

constexpr const char* kEngineVersion = "1.0.0";
constexpr int kFormatVersion = 2;
constexpr int kExtensionApiVersion = 1;
constexpr double kPi = 3.14159265358979323846;

// Safe JSON member access returning stable references (never dangling temporaries).
// jarr/jobj return the member if it has the right type, otherwise a static empty array/object.
const json& jarr(const json& j, const char* key);
const json& jobj(const json& j, const char* key);

// ---------------------------------------------------------------- logging
enum class LogLevel { Error = 0, Warn, Info, Debug, Trace };
void setLogLevel(LogLevel l);
LogLevel logLevel();
void setLogSink(std::function<void(LogLevel, const std::string&)> sink);
void log(LogLevel l, const std::string& msg);
#define MF_LOGE(m) ::mf::log(::mf::LogLevel::Error, (m))
#define MF_LOGW(m) ::mf::log(::mf::LogLevel::Warn, (m))
#define MF_LOGI(m) ::mf::log(::mf::LogLevel::Info, (m))
#define MF_LOGD(m) ::mf::log(::mf::LogLevel::Debug, (m))

// ---------------------------------------------------------------- math
struct Vec2 {
    double x = 0, y = 0;
    Vec2() = default;
    Vec2(double x_, double y_) : x(x_), y(y_) {}
    Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(double s) const { return {x * s, y * s}; }
    Vec2 operator/(double s) const { return {x / s, y / s}; }
    Vec2& operator+=(const Vec2& o) { x += o.x; y += o.y; return *this; }
    double dot(const Vec2& o) const { return x * o.x + y * o.y; }
    double cross(const Vec2& o) const { return x * o.y - y * o.x; }
    double length() const { return std::sqrt(x * x + y * y); }
    Vec2 normalized() const { double l = length(); return l > 1e-12 ? Vec2{x / l, y / l} : Vec2{0, 0}; }
    Vec2 perp() const { return {-y, x}; }
};

struct Vec3 {
    double x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    Vec3 operator*(const Vec3& o) const { return {x * o.x, y * o.y, z * o.z}; }
    Vec3 operator/(double s) const { return {x / s, y / s, z / s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    double dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3& o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    double length() const { return std::sqrt(x * x + y * y + z * z); }
    Vec3 normalized() const { double l = length(); return l > 1e-12 ? *this / l : Vec3{0, 0, 0}; }
};

struct Color {
    float r = 0, g = 0, b = 0, a = 1;
    Color() = default;
    Color(float r_, float g_, float b_, float a_ = 1.f) : r(r_), g(g_), b(b_), a(a_) {}
};

// Row-major 4x4 matrix; points are column vectors (M * p).
struct Mat4 {
    double m[16];
    Mat4() { identity(); }
    void identity() { for (int i = 0; i < 16; ++i) m[i] = (i % 5 == 0) ? 1.0 : 0.0; }
    double& at(int r, int c) { return m[r * 4 + c]; }
    double at(int r, int c) const { return m[r * 4 + c]; }
    Mat4 operator*(const Mat4& o) const {
        Mat4 out;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) {
                double s = 0;
                for (int k = 0; k < 4; ++k) s += at(r, k) * o.at(k, c);
                out.at(r, c) = s;
            }
        return out;
    }
    Vec3 transformPoint(const Vec3& p) const {
        double x = at(0, 0) * p.x + at(0, 1) * p.y + at(0, 2) * p.z + at(0, 3);
        double y = at(1, 0) * p.x + at(1, 1) * p.y + at(1, 2) * p.z + at(1, 3);
        double z = at(2, 0) * p.x + at(2, 1) * p.y + at(2, 2) * p.z + at(2, 3);
        double w = at(3, 0) * p.x + at(3, 1) * p.y + at(3, 2) * p.z + at(3, 3);
        if (std::fabs(w) > 1e-12 && w != 1.0) return {x / w, y / w, z / w};
        return {x, y, z};
    }
    // Returns (x, y, z, w) without perspective divide.
    std::array<double, 4> transform4(const Vec3& p) const {
        std::array<double, 4> r{};
        for (int i = 0; i < 4; ++i) r[i] = at(i, 0) * p.x + at(i, 1) * p.y + at(i, 2) * p.z + at(i, 3);
        return r;
    }
    Vec3 transformDir(const Vec3& d) const {
        return {at(0, 0) * d.x + at(0, 1) * d.y + at(0, 2) * d.z, at(1, 0) * d.x + at(1, 1) * d.y + at(1, 2) * d.z,
                at(2, 0) * d.x + at(2, 1) * d.y + at(2, 2) * d.z};
    }
    static Mat4 translate(double x, double y, double z) {
        Mat4 r; r.at(0, 3) = x; r.at(1, 3) = y; r.at(2, 3) = z; return r;
    }
    static Mat4 scale(double x, double y, double z) {
        Mat4 r; r.at(0, 0) = x; r.at(1, 1) = y; r.at(2, 2) = z; return r;
    }
    static Mat4 rotateX(double rad) {
        Mat4 r; double c = std::cos(rad), s = std::sin(rad);
        r.at(1, 1) = c; r.at(1, 2) = -s; r.at(2, 1) = s; r.at(2, 2) = c; return r;
    }
    static Mat4 rotateY(double rad) {
        Mat4 r; double c = std::cos(rad), s = std::sin(rad);
        r.at(0, 0) = c; r.at(0, 2) = s; r.at(2, 0) = -s; r.at(2, 2) = c; return r;
    }
    static Mat4 rotateZ(double rad) {
        Mat4 r; double c = std::cos(rad), s = std::sin(rad);
        r.at(0, 0) = c; r.at(0, 1) = -s; r.at(1, 0) = s; r.at(1, 1) = c; return r;
    }
    bool inverse(Mat4& out) const;
    // 2D affine helpers (z ignored).
    bool isAffine2D() const {
        return at(3, 0) == 0 && at(3, 1) == 0 && at(3, 2) == 0 && at(3, 3) == 1;
    }
};

// 3x3 homography for planar warps.
struct Mat3 {
    double m[9];
    Mat3() { for (int i = 0; i < 9; ++i) m[i] = (i % 4 == 0) ? 1.0 : 0.0; }
    Vec2 apply(double x, double y) const {
        double w = m[6] * x + m[7] * y + m[8];
        if (std::fabs(w) < 1e-12) w = 1e-12;
        return {(m[0] * x + m[1] * y + m[2]) / w, (m[3] * x + m[4] * y + m[5]) / w};
    }
    bool inverse(Mat3& out) const;
    // Homography mapping unit square corners (0,0),(1,0),(1,1),(0,1) to quad.
    static bool squareToQuad(const Vec2 q[4], Mat3& out);
    static bool quadToQuad(const Vec2 src[4], const Vec2 dst[4], Mat3& out);
    Mat3 operator*(const Mat3& o) const {
        Mat3 r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                double s = 0;
                for (int k = 0; k < 3; ++k) s += m[i * 3 + k] * o.m[k * 3 + j];
                r.m[i * 3 + j] = s;
            }
        return r;
    }
};

template <typename T> inline T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline double lerp(double a, double b, double t) { return a + (b - a) * t; }
inline double deg2rad(double d) { return d * kPi / 180.0; }
inline double smoothstep(double e0, double e1, double x) {
    double t = clampv((x - e0) / (e1 - e0 + 1e-12), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

// Deterministic hashing / noise used by procedural features.
uint32_t hash32(uint32_t x);
double hashToUnit(uint32_t x);  // [0,1)
double valueNoise1D(double x, uint32_t seed);
double valueNoise2D(double x, double y, uint32_t seed);
double fbm2D(double x, double y, uint32_t seed, int octaves);

// ---------------------------------------------------------------- image
// Premultiplied RGBA8 image.
struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    Image() = default;
    Image(int w_, int h_) { resize(w_, h_); }
    void resize(int w_, int h_) { w = w_; h = h_; px.assign((size_t)std::max(0, w) * std::max(0, h) * 4, 0); }
    bool empty() const { return w <= 0 || h <= 0; }
    uint8_t* row(int y) { return px.data() + (size_t)y * w * 4; }
    const uint8_t* row(int y) const { return px.data() + (size_t)y * w * 4; }
    uint8_t* at(int x, int y) { return px.data() + ((size_t)y * w + x) * 4; }
    const uint8_t* at(int x, int y) const { return px.data() + ((size_t)y * w + x) * 4; }
    void clear() { std::fill(px.begin(), px.end(), 0); }
    void fill(const Color& c);
};

using ImagePtr = std::shared_ptr<const Image>;

// Integer rectangle [x0,x1) x [y0,y1).
struct Rect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool empty() const { return x1 <= x0 || y1 <= y0; }
    Rect intersect(const Rect& o) const {
        return {std::max(x0, o.x0), std::max(y0, o.y0), std::min(x1, o.x1), std::min(y1, o.y1)};
    }
    Rect unite(const Rect& o) const {
        if (empty()) return o;
        if (o.empty()) return *this;
        return {std::min(x0, o.x0), std::min(y0, o.y0), std::max(x1, o.x1), std::max(y1, o.y1)};
    }
    Rect expand(int d) const { return {x0 - d, y0 - d, x1 + d, y1 + d}; }
    int width() const { return x1 - x0; }
    int height() const { return y1 - y0; }
};

// ---------------------------------------------------------------- color utils
inline float srgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
inline float linearToSrgb(float c) {
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
}
void rgbToHsl(float r, float g, float b, float& h, float& s, float& l);
void hslToRgb(float h, float s, float l, float& r, float& g, float& b);
Color parseColor(const json& j, Color def = Color(1, 1, 1, 1));
json colorToJson(const Color& c);

// ---------------------------------------------------------------- misc
std::string formatString(const char* fmt, ...);
std::string readFileText(const std::string& path, bool* ok = nullptr);
std::vector<uint8_t> readFileBytes(const std::string& path, bool* ok = nullptr);
bool fileExists(const std::string& path);
bool makeDirs(const std::string& path);
std::string pathJoin(const std::string& a, const std::string& b);
std::string pathBasename(const std::string& p);
std::string pathDirname(const std::string& p);
std::string pathExtensionLower(const std::string& p);
int64_t fileSize(const std::string& path);
double nowSeconds();  // wall clock, seconds since epoch

}  // namespace mf
