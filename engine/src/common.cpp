#include "mf/common.hpp"

#include <sys/stat.h>
#include <sys/types.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>

namespace mf {

// ---------------------------------------------------------------- logging
namespace {
LogLevel g_level = LogLevel::Warn;
std::function<void(LogLevel, const std::string&)> g_sink;
std::mutex g_logMutex;
}  // namespace

void setLogLevel(LogLevel l) { g_level = l; }
LogLevel logLevel() { return g_level; }
void setLogSink(std::function<void(LogLevel, const std::string&)> sink) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    g_sink = std::move(sink);
}
void log(LogLevel l, const std::string& msg) {
    if ((int)l > (int)g_level) return;
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_sink) {
        g_sink(l, msg);
        return;
    }
    static const char* names[] = {"ERROR", "WARN", "INFO", "DEBUG", "TRACE"};
    std::fprintf(stderr, "[mf %s] %s\n", names[(int)l], msg.c_str());
}

// ---------------------------------------------------------------- matrices
bool Mat4::inverse(Mat4& out) const {
    const double* a = m;
    double inv[16];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    double det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (std::fabs(det) < 1e-18) return false;
    det = 1.0 / det;
    for (int i = 0; i < 16; ++i) out.m[i] = inv[i] * det;
    return true;
}

bool Mat3::inverse(Mat3& out) const {
    const double* a = m;
    double det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) + a[2] * (a[3] * a[7] - a[4] * a[6]);
    if (std::fabs(det) < 1e-18) return false;
    double id = 1.0 / det;
    out.m[0] = (a[4] * a[8] - a[5] * a[7]) * id;
    out.m[1] = (a[2] * a[7] - a[1] * a[8]) * id;
    out.m[2] = (a[1] * a[5] - a[2] * a[4]) * id;
    out.m[3] = (a[5] * a[6] - a[3] * a[8]) * id;
    out.m[4] = (a[0] * a[8] - a[2] * a[6]) * id;
    out.m[5] = (a[2] * a[3] - a[0] * a[5]) * id;
    out.m[6] = (a[3] * a[7] - a[4] * a[6]) * id;
    out.m[7] = (a[1] * a[6] - a[0] * a[7]) * id;
    out.m[8] = (a[0] * a[4] - a[1] * a[3]) * id;
    return true;
}

bool Mat3::squareToQuad(const Vec2 q[4], Mat3& out) {
    double x0 = q[0].x, y0 = q[0].y, x1 = q[1].x, y1 = q[1].y, x2 = q[2].x, y2 = q[2].y, x3 = q[3].x, y3 = q[3].y;
    double dx1 = x1 - x2, dx2 = x3 - x2, dx3 = x0 - x1 + x2 - x3;
    double dy1 = y1 - y2, dy2 = y3 - y2, dy3 = y0 - y1 + y2 - y3;
    double a, b, c, d, e, f, g, h;
    if (std::fabs(dx3) < 1e-12 && std::fabs(dy3) < 1e-12) {
        a = x1 - x0; b = x2 - x1; c = x0; d = y1 - y0; e = y2 - y1; f = y0; g = 0; h = 0;
    } else {
        double den = dx1 * dy2 - dx2 * dy1;
        if (std::fabs(den) < 1e-18) return false;
        g = (dx3 * dy2 - dx2 * dy3) / den;
        h = (dx1 * dy3 - dx3 * dy1) / den;
        a = x1 - x0 + g * x1; b = x3 - x0 + h * x3; c = x0;
        d = y1 - y0 + g * y1; e = y3 - y0 + h * y3; f = y0;
    }
    out.m[0] = a; out.m[1] = b; out.m[2] = c;
    out.m[3] = d; out.m[4] = e; out.m[5] = f;
    out.m[6] = g; out.m[7] = h; out.m[8] = 1;
    return true;
}

bool Mat3::quadToQuad(const Vec2 src[4], const Vec2 dst[4], Mat3& out) {
    Mat3 s2q, d2q, q2s;
    if (!squareToQuad(src, s2q) || !squareToQuad(dst, d2q)) return false;
    if (!s2q.inverse(q2s)) return false;
    out = d2q * q2s;
    return true;
}

// ---------------------------------------------------------------- noise
uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
double hashToUnit(uint32_t x) { return (hash32(x) & 0xFFFFFF) / double(0x1000000); }

double valueNoise1D(double x, uint32_t seed) {
    double fl = std::floor(x);
    int i = (int)fl;
    double f = x - fl;
    double a = hashToUnit((uint32_t)i * 374761393U + seed * 668265263U) * 2 - 1;
    double b = hashToUnit((uint32_t)(i + 1) * 374761393U + seed * 668265263U) * 2 - 1;
    double u = f * f * (3 - 2 * f);
    return a + (b - a) * u;
}

double valueNoise2D(double x, double y, uint32_t seed) {
    double fx = std::floor(x), fy = std::floor(y);
    int ix = (int)fx, iy = (int)fy;
    double tx = x - fx, ty = y - fy;
    auto h = [&](int a, int b) {
        return hashToUnit((uint32_t)a * 374761393U + (uint32_t)b * 668265263U + seed * 2246822519U) * 2 - 1;
    };
    double u = tx * tx * (3 - 2 * tx), v = ty * ty * (3 - 2 * ty);
    double a = h(ix, iy), b = h(ix + 1, iy), c = h(ix, iy + 1), d = h(ix + 1, iy + 1);
    return lerp(lerp(a, b, u), lerp(c, d, u), v);
}

double fbm2D(double x, double y, uint32_t seed, int octaves) {
    double s = 0, amp = 0.5, freq = 1, norm = 0;
    for (int i = 0; i < octaves; ++i) {
        s += amp * valueNoise2D(x * freq, y * freq, seed + i * 31);
        norm += amp;
        amp *= 0.5;
        freq *= 2;
    }
    return s / norm;
}

// ---------------------------------------------------------------- image
void Image::fill(const Color& c) {
    uint8_t r = (uint8_t)std::lround(clampv(c.r * c.a, 0.f, 1.f) * 255);
    uint8_t g = (uint8_t)std::lround(clampv(c.g * c.a, 0.f, 1.f) * 255);
    uint8_t b = (uint8_t)std::lround(clampv(c.b * c.a, 0.f, 1.f) * 255);
    uint8_t a = (uint8_t)std::lround(clampv(c.a, 0.f, 1.f) * 255);
    for (size_t i = 0; i < px.size(); i += 4) {
        px[i] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = a;
    }
}

// ---------------------------------------------------------------- color
void rgbToHsl(float r, float g, float b, float& h, float& s, float& l) {
    float mx = std::max({r, g, b}), mn = std::min({r, g, b});
    l = (mx + mn) * 0.5f;
    if (mx - mn < 1e-6f) { h = 0; s = 0; return; }
    float d = mx - mn;
    s = l > 0.5f ? d / (2 - mx - mn) : d / (mx + mn);
    if (mx == r) h = (g - b) / d + (g < b ? 6 : 0);
    else if (mx == g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
    h /= 6;
}
static float hue2rgb(float p, float q, float t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.f / 6) return p + (q - p) * 6 * t;
    if (t < 0.5f) return q;
    if (t < 2.f / 3) return p + (q - p) * (2.f / 3 - t) * 6;
    return p;
}
void hslToRgb(float h, float s, float l, float& r, float& g, float& b) {
    if (s <= 1e-6f) { r = g = b = l; return; }
    h = h - std::floor(h);
    float q = l < 0.5f ? l * (1 + s) : l + s - l * s;
    float p = 2 * l - q;
    r = hue2rgb(p, q, h + 1.f / 3);
    g = hue2rgb(p, q, h);
    b = hue2rgb(p, q, h - 1.f / 3);
}

Color parseColor(const json& j, Color def) {
    if (j.is_array() && j.size() >= 3) {
        return Color(j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j.size() > 3 ? j[3].get<float>() : 1.f);
    }
    if (j.is_string()) {
        std::string s = j.get<std::string>();
        if (!s.empty() && s[0] == '#') s = s.substr(1);
        if (s.size() == 6 || s.size() == 8) {
            unsigned v = (unsigned)std::stoul(s, nullptr, 16);
            if (s.size() == 6) return Color(((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f, 1);
            return Color(((v >> 24) & 255) / 255.f, ((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f);
        }
    }
    return def;
}
json colorToJson(const Color& c) { return json::array({c.r, c.g, c.b, c.a}); }

// ---------------------------------------------------------------- misc
std::string formatString(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < (int)sizeof(buf)) return std::string(buf, n > 0 ? n : 0);
    std::string out(n + 1, '\0');
    va_start(ap, fmt);
    std::vsnprintf(out.data(), out.size(), fmt, ap);
    va_end(ap);
    out.resize(n);
    return out;
}

std::string readFileText(const std::string& path, bool* ok) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (ok) *ok = false; return {}; }
    std::stringstream ss;
    ss << f.rdbuf();
    if (ok) *ok = true;
    return ss.str();
}

std::vector<uint8_t> readFileBytes(const std::string& path, bool* ok) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (ok) *ok = false; return {}; }
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> out((size_t)std::max<std::streamoff>(0, n));
    if (n > 0) f.read((char*)out.data(), n);
    if (ok) *ok = (bool)f || f.eof();
    return out;
}

bool fileExists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

bool makeDirs(const std::string& path) {
    if (path.empty()) return false;
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        if (path[i] == '/' || i + 1 == path.size()) {
            if (cur == "/") continue;
            ::mkdir(cur.c_str(), 0755);
        }
    }
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string pathJoin(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}
std::string pathBasename(const std::string& p) {
    auto pos = p.find_last_of('/');
    return pos == std::string::npos ? p : p.substr(pos + 1);
}
std::string pathDirname(const std::string& p) {
    auto pos = p.find_last_of('/');
    return pos == std::string::npos ? std::string(".") : p.substr(0, pos);
}
std::string pathExtensionLower(const std::string& p) {
    std::string b = pathBasename(p);
    auto pos = b.find_last_of('.');
    if (pos == std::string::npos) return {};
    std::string e = b.substr(pos + 1);
    for (auto& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}
int64_t fileSize(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return -1;
    return (int64_t)st.st_size;
}
double nowSeconds() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(system_clock::now().time_since_epoch()).count();
}

}  // namespace mf
