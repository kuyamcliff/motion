// GPU compositor for the preview: draws a RenderPlan (see mf::Renderer::renderPlan) with OpenGL ES 2.
//
// All functions run on the preview GL thread with its EGL context current (GpuPreview.kt owns the thread, the EGL
// surface and the hardware video decoders that feed external OES textures).
//
// Semantics match mf::compositePlan (the CPU reference): every item is sampled through inverse(H) with bilinear
// filtering and blended in premultiplied RGBA with the engine's blend formulas. Normal and Add blend with fixed-
// function blending straight into the accumulator; the other 16 modes draw the item into a layer buffer and run a
// blend pass that reads both (ping-pong accumulators).
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <android/bitmap.h>
#include <android/log.h>
#include <jni.h>

#include <chrono>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "android_media.h"
#include "mf/engine.hpp"
#include "mf/model.hpp"
#include "mf/renderer.hpp"

using mf::json;

namespace mfa {
mf::Engine* engineForGpu();
AndroidMediaProvider* mediaForGpu();
}  // namespace mfa

namespace {
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "mf-gpu", __VA_ARGS__)

const char* kItemVS = R"(
attribute vec2 aPos;
uniform mat3 uH;      // item px -> output px (row-major uploaded as transpose)
uniform vec2 uOut;    // output size (px)
uniform vec2 uSize;   // item size (px)
varying vec2 vUV;
void main() {
    vec3 p = uH * vec3(aPos, 1.0);
    vUV = aPos / uSize;
    // Homogeneous output position -> clip space; the GPU interpolates vUV perspective-correctly.
    gl_Position = vec4(2.0 * p.x / uOut.x - p.z, 2.0 * p.y / uOut.y - p.z, 0.0, p.z);
})";

const char* kRasterFS = R"(
precision mediump float;
varying vec2 vUV;
uniform sampler2D uTex;
uniform float uOpacity;
void main() { gl_FragColor = texture2D(uTex, vUV) * uOpacity; })";

const char* kVideoFS = R"(
#extension GL_OES_EGL_image_external : require
precision mediump float;
varying vec2 vUV;
uniform samplerExternalOES uTex;
uniform mat4 uTexM;   // SurfaceTexture transform (frame origin bottom-left)
uniform float uOpacity;
void main() {
    vec2 st = (uTexM * vec4(vUV.x, 1.0 - vUV.y, 0.0, 1.0)).xy;
    gl_FragColor = vec4(texture2D(uTex, st).rgb, 1.0) * uOpacity;
})";

const char* kQuadVS = R"(
attribute vec2 aPos;  // 0..1
uniform vec4 uRect;   // x0, y0, x1, y1 in NDC
varying vec2 vUV;
void main() { vUV = aPos; gl_Position = vec4(mix(uRect.x, uRect.z, aPos.x), mix(uRect.y, uRect.w, aPos.y), 0.0, 1.0); })";

// Engine blend formulas (mf::compositeImage) on premultiplied inputs.
const char* kBlendFS = R"(
precision highp float;
varying vec2 vUV;
uniform sampler2D uDst;
uniform sampler2D uSrc;
uniform int uMode;
float lum(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
vec3 clipColor(vec3 c) {
    float l = lum(c); float n = min(min(c.r, c.g), c.b); float x = max(max(c.r, c.g), c.b);
    if (n < 0.0) c = l + (c - l) * l / (l - n + 1e-6);
    if (x > 1.0) c = l + (c - l) * (1.0 - l) / (x - l + 1e-6);
    return c;
}
vec3 setLum(vec3 c, float l) { return clipColor(c + (l - lum(c))); }
float sat(vec3 c) { return max(max(c.r, c.g), c.b) - min(min(c.r, c.g), c.b); }
vec3 setSat(vec3 c, float s) {
    float mx = max(max(c.r, c.g), c.b); float mn = min(min(c.r, c.g), c.b);
    return mx > mn ? (c - mn) * s / (mx - mn) : vec3(0.0);
}
float ch(float s, float d) {
    if (uMode == 1) return s * d;
    if (uMode == 2) return s + d - s * d;
    if (uMode == 3) return d <= 0.5 ? 2.0 * s * d : 1.0 - 2.0 * (1.0 - s) * (1.0 - d);
    if (uMode == 4) { if (s <= 0.5) return d - (1.0 - 2.0 * s) * d * (1.0 - d);
                      float g = d <= 0.25 ? ((16.0 * d - 12.0) * d + 4.0) * d : sqrt(d); return d + (2.0 * s - 1.0) * (g - d); }
    if (uMode == 5) return s <= 0.5 ? 2.0 * s * d : 1.0 - 2.0 * (1.0 - s) * (1.0 - d);
    if (uMode == 6) return min(s, d);
    if (uMode == 7) return max(s, d);
    if (uMode == 8) return d <= 0.0 ? 0.0 : (s >= 1.0 ? 1.0 : min(1.0, d / (1.0 - s)));
    if (uMode == 9) return d >= 1.0 ? 1.0 : (s <= 0.0 ? 0.0 : 1.0 - min(1.0, (1.0 - d) / s));
    if (uMode == 10) return abs(s - d);
    if (uMode == 11) return s + d - 2.0 * s * d;
    if (uMode == 12) return min(1.0, s + d);
    if (uMode == 13) return max(0.0, d - s);
    return s;
}
void main() {
    vec4 d = texture2D(uDst, vUV);
    vec4 s = texture2D(uSrc, vUV);
    if (s.a < 0.5 / 255.0) { gl_FragColor = d; return; }
    vec3 S = s.rgb / s.a;
    vec3 D = d.a > 0.0 ? d.rgb / d.a : vec3(0.0);
    vec3 B;
    if (uMode == 14) B = setLum(setSat(S, sat(D)), lum(D));
    else if (uMode == 15) B = setLum(setSat(D, sat(S)), lum(D));
    else if (uMode == 16) B = setLum(S, lum(D));
    else if (uMode == 17) B = setLum(D, lum(S));
    else B = vec3(ch(S.r, D.r), ch(S.g, D.g), ch(S.b, D.b));
    float oa = s.a + d.a * (1.0 - s.a);
    vec3 o = s.a * (1.0 - d.a) * S + s.a * d.a * B + (1.0 - s.a) * d.rgb;
    gl_FragColor = clamp(vec4(o, oa), 0.0, 1.0);
})";

// Final image into the window (or nothing for offscreen readback); optional transparency checkerboard.
const char* kPresentFS = R"(
precision mediump float;
varying vec2 vUV;
uniform sampler2D uTex;
uniform float uChecker;
void main() {
    vec4 c = texture2D(uTex, vec2(vUV.x, 1.0 - vUV.y));
    vec2 q = floor(gl_FragCoord.xy / 16.0);
    float k = mod(q.x + q.y, 2.0) < 1.0 ? 0.23 : 0.16;
    vec3 bg = uChecker > 0.5 ? vec3(k) : vec3(0.0);
    gl_FragColor = vec4(c.rgb + (1.0 - c.a) * bg, 1.0);
})";

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        LOGW("shader compile failed: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint link(const char* vs, const char* fs) {
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "aPos");
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        LOGW("program link failed: %s", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

struct Target {
    GLuint tex = 0, fbo = 0;
    int w = 0, h = 0;
    void ensure(int W, int H) {
        if (tex && w == W && h == H) return;
        release();
        w = W;
        h = H;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    }
    void release() {
        if (fbo) glDeleteFramebuffers(1, &fbo);
        if (tex) glDeleteTextures(1, &tex);
        fbo = tex = 0;
        w = h = 0;
    }
};

struct CachedTex {
    GLuint tex = 0;
    int w = 0, h = 0;
    uint64_t lastUsed = 0;
};

struct Gpu {
    bool ready = false;
    GLuint progRaster = 0, progVideo = 0, progBlend = 0, progPresent = 0;
    GLuint quadVbo = 0, itemVbo = 0;
    Target acc[2], layer;
    int cur = 0;
    std::unordered_map<std::string, CachedTex> cache;  // stable layer rasters by plan cache key
    std::vector<CachedTex> pool;                       // per-frame rasters
    uint64_t frame = 0;
    mf::RenderPlan plan;
    double lastPlanMs = 0, lastGpuMs = 0;
};
Gpu g;

GLuint newTexture() {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

void upload(CachedTex& t, const mf::Image& img) {
    if (!t.tex) t.tex = newTexture();
    glBindTexture(GL_TEXTURE_2D, t.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (t.w == img.w && t.h == img.h) glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, img.w, img.h, GL_RGBA, GL_UNSIGNED_BYTE, img.px.data());
    else glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.w, img.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.px.data());
    t.w = img.w;
    t.h = img.h;
}

int blendIndex(const std::string& name) { return (int)mf::blendModeFromName(name); }  // matches uMode in kBlendFS

void setHomography(GLuint prog, const mf::Mat3& H) {
    // GLSL mat3 is column-major and ES 2 cannot transpose on upload.
    float m[9] = {(float)H.m[0], (float)H.m[3], (float)H.m[6], (float)H.m[1], (float)H.m[4], (float)H.m[7], (float)H.m[2], (float)H.m[5], (float)H.m[8]};
    glUniformMatrix3fv(glGetUniformLocation(prog, "uH"), 1, GL_FALSE, m);
}

void drawItemQuad(GLuint prog, const mf::Mat3& H, int w, int h, int outW, int outH) {
    float v[8] = {0, 0, (float)w, 0, 0, (float)h, (float)w, (float)h};
    glBindBuffer(GL_ARRAY_BUFFER, g.itemVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    setHomography(prog, H);
    glUniform2f(glGetUniformLocation(prog, "uOut"), (float)outW, (float)outH);
    glUniform2f(glGetUniformLocation(prog, "uSize"), (float)w, (float)h);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void drawUnitQuad(GLuint prog, float x0, float y0, float x1, float y1) {
    glBindBuffer(GL_ARRAY_BUFFER, g.quadVbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glUniform4f(glGetUniformLocation(prog, "uRect"), x0, y0, x1, y1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

bool init() {
    if (g.ready) return true;
    g.progRaster = link(kItemVS, kRasterFS);
    g.progVideo = link(kItemVS, kVideoFS);
    g.progBlend = link(kQuadVS, kBlendFS);
    g.progPresent = link(kQuadVS, kPresentFS);
    if (!g.progRaster || !g.progBlend || !g.progPresent) return false;
    float q[8] = {0, 0, 1, 0, 0, 1, 1, 1};
    glGenBuffers(1, &g.quadVbo);
    glBindBuffer(GL_ARRAY_BUFFER, g.quadVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STATIC_DRAW);
    glGenBuffers(1, &g.itemVbo);
    g.ready = true;
    return true;
}

void release() {
    for (auto& t : g.acc) t.release();
    g.layer.release();
    for (auto& [k, t] : g.cache) glDeleteTextures(1, &t.tex);
    g.cache.clear();
    for (auto& t : g.pool)
        if (t.tex) glDeleteTextures(1, &t.tex);
    g.pool.clear();
    for (GLuint p : {g.progRaster, g.progVideo, g.progBlend, g.progPresent})
        if (p) glDeleteProgram(p);
    if (g.quadVbo) glDeleteBuffers(1, &g.quadVbo);
    if (g.itemVbo) glDeleteBuffers(1, &g.itemVbo);
    g = Gpu();
}

// Composites g.plan into the accumulator; videoTex/videoMtx are per plan item (0 = decode on the CPU instead).
void composite(const std::vector<int>& videoTex, const std::vector<float>& videoMtx) {
    const mf::RenderPlan& P = g.plan;
    int W = std::max(1, P.W), H = std::max(1, P.H);
    g.acc[0].ensure(W, H);
    g.acc[1].ensure(W, H);
    g.layer.ensure(W, H);
    g.cur = 0;
    ++g.frame;
    glBindFramebuffer(GL_FRAMEBUFFER, g.acc[0].fbo);
    glViewport(0, 0, W, H);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    if (P.transparent) glClearColor(0, 0, 0, 0);
    else glClearColor(P.bg[0] * P.bg[3], P.bg[1] * P.bg[3], P.bg[2] * P.bg[3], P.bg[3]);
    glClear(GL_COLOR_BUFFER_BIT);
    size_t poolIdx = 0;
    for (size_t i = 0; i < P.items.size(); ++i) {
        const mf::PlanItem& it = P.items[i];
        GLuint prog = g.progRaster;
        GLuint tex = 0;
        mf::Mat3 Hm = it.H;
        int w = it.width, h = it.height;
        bool video = it.kind == mf::PlanItem::Kind::Video && i < videoTex.size() && videoTex[i] != 0 && g.progVideo;
        if (video) {
            prog = g.progVideo;
            tex = (GLuint)videoTex[i];
        } else {
            std::shared_ptr<const mf::Image> img = it.image;
            if (it.kind == mf::PlanItem::Kind::Video) {
                // No hardware decoder for this asset (e.g. generated test media): decode on the CPU.
                auto* media = mfa::mediaForGpu();
                img = media ? media->videoFrame(it.asset, it.sourceTime, it.width, it.height) : nullptr;
                if (!img || img->empty()) continue;
                mf::Mat3 s;
                s.m[0] = (double)it.width / img->w;
                s.m[4] = (double)it.height / img->h;
                Hm = it.H * s;
                w = img->w;
                h = img->h;
            }
            if (!img || img->empty()) continue;
            CachedTex* ct;
            if (!it.cacheKey.empty()) {
                auto f = g.cache.find(it.cacheKey);
                if (f == g.cache.end()) {
                    ct = &g.cache[it.cacheKey];
                    upload(*ct, *img);
                } else {
                    ct = &f->second;
                }
            } else {
                if (poolIdx >= g.pool.size()) g.pool.emplace_back();
                ct = &g.pool[poolIdx++];
                upload(*ct, *img);
            }
            ct->lastUsed = g.frame;
            tex = ct->tex;
        }
        int mode = blendIndex(it.blend);
        bool direct = mode == (int)mf::BlendMode::Normal || mode == (int)mf::BlendMode::Add;
        glUseProgram(prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(video ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D, tex);
        glUniform1i(glGetUniformLocation(prog, "uTex"), 0);
        glUniform1f(glGetUniformLocation(prog, "uOpacity"), it.opacity);
        if (video) glUniformMatrix4fv(glGetUniformLocation(prog, "uTexM"), 1, GL_FALSE, &videoMtx[i * 16]);
        if (direct) {
            glBindFramebuffer(GL_FRAMEBUFFER, g.acc[g.cur].fbo);
            glViewport(0, 0, W, H);
            glEnable(GL_BLEND);
            if (mode == (int)mf::BlendMode::Add) glBlendFunc(GL_ONE, GL_ONE);
            else glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            drawItemQuad(prog, Hm, w, h, W, H);
            glDisable(GL_BLEND);
            continue;
        }
        // Layer buffer, then blend pass acc[cur] + layer -> acc[1-cur].
        glBindFramebuffer(GL_FRAMEBUFFER, g.layer.fbo);
        glViewport(0, 0, W, H);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        drawItemQuad(prog, Hm, w, h, W, H);
        int nxt = 1 - g.cur;
        glBindFramebuffer(GL_FRAMEBUFFER, g.acc[nxt].fbo);
        glViewport(0, 0, W, H);
        glUseProgram(g.progBlend);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g.acc[g.cur].tex);
        glUniform1i(glGetUniformLocation(g.progBlend, "uDst"), 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, g.layer.tex);
        glUniform1i(glGetUniformLocation(g.progBlend, "uSrc"), 1);
        glUniform1i(glGetUniformLocation(g.progBlend, "uMode"), mode);
        drawUnitQuad(g.progBlend, -1, -1, 1, 1);
        glActiveTexture(GL_TEXTURE0);
        g.cur = nxt;
    }
    // Drop layer textures that have not been used for a while (layer edited, deleted or zoom changed).
    for (auto it = g.cache.begin(); it != g.cache.end();) {
        if (g.frame - it->second.lastUsed > 240) {
            glDeleteTextures(1, &it->second.tex);
            it = g.cache.erase(it);
        } else {
            ++it;
        }
    }
}

std::string itemsJson() {
    json items = json::array();
    for (size_t i = 0; i < g.plan.items.size(); ++i) {
        const auto& it = g.plan.items[i];
        json j = {{"i", (int)i}, {"kind", it.kind == mf::PlanItem::Kind::Video ? "video" : "raster"}, {"layer", it.layerId}};
        if (it.kind == mf::PlanItem::Kind::Video) {
            j["asset"] = it.asset;
            j["t"] = it.sourceTime;
        }
        items.push_back(j);
    }
    return json{{"W", g.plan.W}, {"H", g.plan.H}, {"fallback", g.plan.fallback}, {"fallbackReason", g.plan.fallbackReason}, {"items", items},
                {"cached", g.plan.cachedRasters}, {"perFrame", g.plan.frameRasters}, {"video", g.plan.videoItems}, {"planMs", g.lastPlanMs}}
        .dump();
}
}  // namespace

extern "C" {

JNIEXPORT jboolean JNICALL Java_com_motionforge_app_engine_NativeBridge_nativeGpuInit(JNIEnv*, jclass) { return init(); }

JNIEXPORT void JNICALL Java_com_motionforge_app_engine_NativeBridge_nativeGpuRelease(JNIEnv*, jclass) { release(); }

// Builds the plan for time t at the given output width (comp aspect kept). Returns the item list as JSON so the
// caller can prepare video textures. exportMode disables proxies and guide layers.
JNIEXPORT jstring JNICALL Java_com_motionforge_app_engine_NativeBridge_nativeGpuPlan(JNIEnv* e, jclass, jdouble t, jint outW, jboolean exportMode,
                                                                                      jboolean useProxies) {
    mf::Engine* E = mfa::engineForGpu();
    g.plan = mf::RenderPlan();
    if (!E || !E->isOpen()) return e->NewStringUTF("{\"items\":[]}");
    auto snap = E->snapshot();
    const json* comp = mf::activeComp(*snap);
    if (!comp) return e->NewStringUTF("{\"items\":[]}");
    mf::RenderSettings rs;
    rs.scale = (double)std::max(16, (int)outW) / std::max(1, comp->value("width", 1920));
    rs.exportMode = exportMode;
    rs.useProxies = useProxies;
    rs.revision = 0;
    auto t0 = std::chrono::steady_clock::now();
    g.plan = E->renderer().renderPlan(*snap, comp->value("id", ""), t, rs);
    g.lastPlanMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return e->NewStringUTF(itemsJson().c_str());
}

// Composites the current plan and, when viewW > 0, presents it into the bound window surface at dst (view px,
// y down). Returns timing JSON.
JNIEXPORT jstring JNICALL Java_com_motionforge_app_engine_NativeBridge_nativeGpuComposite(JNIEnv* e, jclass, jintArray videoTex, jfloatArray videoMtx,
                                                                                           jint viewW, jint viewH, jfloat dx, jfloat dy, jfloat dw, jfloat dh,
                                                                                           jboolean checker, jfloatArray clear) {
    if (!g.ready) return e->NewStringUTF("{\"ok\":false}");
    auto t0 = std::chrono::steady_clock::now();
    std::vector<int> tex(g.plan.items.size(), 0);
    std::vector<float> mtx(g.plan.items.size() * 16, 0.f);
    if (videoTex) {
        jsize n = std::min<jsize>(e->GetArrayLength(videoTex), (jsize)tex.size());
        e->GetIntArrayRegion(videoTex, 0, n, tex.data());
    }
    if (videoMtx) {
        jsize n = std::min<jsize>(e->GetArrayLength(videoMtx), (jsize)mtx.size());
        e->GetFloatArrayRegion(videoMtx, 0, n, mtx.data());
    }
    composite(tex, mtx);
    if (viewW > 0 && viewH > 0) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, viewW, viewH);
        float c[4] = {0, 0, 0, 1};
        if (clear) e->GetFloatArrayRegion(clear, 0, 4, c);
        glClearColor(c[0], c[1], c[2], c[3]);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(g.progPresent);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g.acc[g.cur].tex);
        glUniform1i(glGetUniformLocation(g.progPresent, "uTex"), 0);
        glUniform1f(glGetUniformLocation(g.progPresent, "uChecker"), checker ? 1.f : 0.f);
        // View px (y down) -> NDC (y up).
        float x0 = 2 * dx / viewW - 1, x1 = 2 * (dx + dw) / viewW - 1;
        float y0 = 1 - 2 * (dy + dh) / viewH, y1 = 1 - 2 * dy / viewH;
        drawUnitQuad(g.progPresent, x0, y0, x1, y1);
    }
    g.lastGpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    json r = {{"ok", true}, {"gpuMs", g.lastGpuMs}, {"planMs", g.lastPlanMs}, {"cachedTextures", (int)g.cache.size()},
              {"items", (int)g.plan.items.size()}, {"fallback", g.plan.fallback}};
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) r["glError"] = (int)err;
    return e->NewStringUTF(r.dump().c_str());
}

// Reads the composited frame (plan size) into an ARGB_8888 bitmap of the same size (tests, thumbnails).
JNIEXPORT jboolean JNICALL Java_com_motionforge_app_engine_NativeBridge_nativeGpuReadback(JNIEnv* e, jclass, jobject bitmap) {
    if (!g.ready || !g.acc[g.cur].fbo) return false;
    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(e, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS) return false;
    int W = g.acc[g.cur].w, H = g.acc[g.cur].h;
    if ((int)info.width != W || (int)info.height != H) return false;
    std::vector<uint8_t> px((size_t)W * H * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, g.acc[g.cur].fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    void* dst = nullptr;
    if (AndroidBitmap_lockPixels(e, bitmap, &dst) != ANDROID_BITMAP_RESULT_SUCCESS) return false;
    for (int y = 0; y < H; ++y) std::memcpy((uint8_t*)dst + (size_t)y * info.stride, px.data() + (size_t)y * W * 4, (size_t)W * 4);
    AndroidBitmap_unlockPixels(e, bitmap);
    return true;
}
}
