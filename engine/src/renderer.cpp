#include "mf/renderer.hpp"

#include <chrono>
#include <set>

#include "mf/behaviors.hpp"
#include "mf/imageops.hpp"
#include "mf/model.hpp"
#include "mf/raster.hpp"
#include "mf/shapes.hpp"
#include "mf/text.hpp"
#include "mf/threadpool.hpp"

namespace mf {

// ====================================================================== media providers
std::shared_ptr<const Mesh> MediaProvider::model(const json& asset) {
    std::string path = localPath(asset);
    std::string key = asset.value("id", std::string()) + "|" + path;
    {
        std::lock_guard<std::mutex> lk(cacheMutex_);
        auto it = meshCache_.find(key);
        if (it != meshCache_.end()) return it->second;
    }
    auto m = std::make_shared<Mesh>();
    std::string err;
    if (path.empty() || !loadModelFile(path, *m, err)) {
        MF_LOGW("model load failed: " + err);
        m.reset();
    }
    std::lock_guard<std::mutex> lk(cacheMutex_);
    meshCache_[key] = m;
    return m;
}

std::shared_ptr<LutData> MediaProvider::lut(const json& asset) {
    std::string path = localPath(asset);
    std::lock_guard<std::mutex> lk(cacheMutex_);
    auto it = lutCache_.find(path);
    if (it != lutCache_.end()) return it->second;
    auto l = std::make_shared<LutData>();
    std::string err;
    if (path.empty() || !l->load(path, err)) {
        MF_LOGW("LUT load failed: " + err);
        l.reset();
    }
    lutCache_[path] = l;
    return l;
}

bool MediaProvider::available(const json& asset) {
    if (asset.contains("generator")) return true;
    std::string p = localPath(asset);
    return !p.empty() && fileExists(p);
}

ImagePtr generateTestPattern(const std::string& kind, int w, int h, double t, double fps) {
    auto img = std::make_shared<Image>(w, h);
    int frame = (int)std::floor(t * fps + 1e-6);
    if (kind == "bars") {
        static const Color bars[] = {Color(0.75f, 0.75f, 0.75f), Color(0.75f, 0.75f, 0), Color(0, 0.75f, 0.75f), Color(0, 0.75f, 0),
                                     Color(0.75f, 0, 0.75f), Color(0.75f, 0, 0), Color(0, 0, 0.75f)};
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const Color& c = bars[std::min(6, x * 7 / std::max(1, w))];
                uint8_t* d = img->at(x, y);
                d[0] = (uint8_t)(c.r * 255); d[1] = (uint8_t)(c.g * 255); d[2] = (uint8_t)(c.b * 255); d[3] = 255;
            }
    } else if (kind == "green") {
        // Subject (red disc) on a green screen for chroma-key tests.
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint8_t* d = img->at(x, y);
                double dx = x - w / 2.0, dy = y - h / 2.0;
                bool subj = dx * dx + dy * dy < (h * 0.25) * (h * 0.25);
                d[0] = subj ? 220 : 20; d[1] = subj ? 40 : 200; d[2] = subj ? 40 : 30; d[3] = 255;
            }
    } else {
        // "counter": moving gradient + a white square whose x encodes the frame number (for timing tests).
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint8_t* d = img->at(x, y);
                d[0] = (uint8_t)(x * 255 / std::max(1, w - 1));
                d[1] = (uint8_t)(y * 255 / std::max(1, h - 1));
                d[2] = (uint8_t)((frame * 8) & 255);
                d[3] = 255;
            }
        int sq = std::max(2, h / 10);
        int px = (frame * sq / 2) % std::max(1, w - sq);
        for (int y = h / 2 - sq / 2; y < h / 2 + sq / 2; ++y)
            for (int x = px; x < px + sq; ++x)
                if (x >= 0 && y >= 0 && x < w && y < h) { uint8_t* d = img->at(x, y); d[0] = d[1] = d[2] = d[3] = 255; }
    }
    return img;
}

ImagePtr FileMediaProvider::videoFrame(const json& asset, double st, int maxW, int maxH) {
    std::string id = asset.value("id", std::string());
    if (asset.contains("generator")) {
        int w = asset.value("width", 640), h = asset.value("height", 360);
        return generateTestPattern(asset["generator"].get<std::string>(), w, h, st, asset.value("fps", 30.0));
    }
    int fi = (int)std::floor(st * asset.value("fps", 30.0) + 1e-6);
    auto it = frames_.find({id, fi});
    if (it != frames_.end()) return it->second;
    // Image sequence: "pattern" with %d.
    if (asset.contains("sequence")) {
        std::string pat = asset["sequence"];
        std::string path = formatString(pat.c_str(), fi + asset.value("sequenceStart", 0));
        auto img = std::make_shared<Image>();
        if (loadImageFile(path, *img)) return img;
    }
    return nullptr;
}

ImagePtr FileMediaProvider::image(const json& asset, int, int) {
    std::string path = localPath(asset);
    auto it = images_.find(path);
    if (it != images_.end()) return it->second;
    auto img = std::make_shared<Image>();
    if (asset.contains("generator")) {
        auto g = generateTestPattern(asset["generator"], asset.value("width", 640), asset.value("height", 360), 0, 30);
        images_[path] = g;
        return g;
    }
    if (!loadImageFile(path, *img)) img.reset();
    images_[path] = img;
    return img;
}

std::shared_ptr<const AudioBuffer> FileMediaProvider::audio(const json& asset) {
    std::string path = localPath(asset);
    auto it = audio_.find(path);
    if (it != audio_.end()) return it->second;
    std::shared_ptr<AudioBuffer> buf;
    if (asset.contains("toneHz")) {
        buf = std::make_shared<AudioBuffer>();
        double dur = asset.value("duration", 2.0), hz = asset["toneHz"].get<double>();
        size_t n = (size_t)(dur * buf->sampleRate);
        buf->samples.resize(n * 2);
        for (size_t i = 0; i < n; ++i) {
            float v = (float)(0.5 * std::sin(2 * kPi * hz * i / buf->sampleRate));
            buf->samples[i * 2] = buf->samples[i * 2 + 1] = v;
        }
    } else if (!path.empty()) {
        buf = std::make_shared<AudioBuffer>();
        if (!loadWav(path, *buf)) buf.reset();
    }
    audio_[path] = buf;
    return buf;
}

void FileMediaProvider::putFrame(const std::string& assetId, int frameIndex, ImagePtr img) { frames_[{assetId, frameIndex}] = std::move(img); }

// ====================================================================== renderer internals
struct Renderer::Impl {
    Renderer* R;
    std::mutex cacheMutex;
    std::list<std::pair<std::string, std::shared_ptr<Image>>> lru;
    std::map<std::string, std::list<std::pair<std::string, std::shared_ptr<Image>>>::iterator> lruIndex;
    size_t lruBytes = 0;
    std::map<std::string, std::shared_ptr<Mesh>> meshCache;

    std::shared_ptr<Image> cacheGet(const std::string& k) {
        auto it = lruIndex.find(k);
        if (it == lruIndex.end()) return nullptr;
        lru.splice(lru.begin(), lru, it->second);
        return it->second->second;
    }
    void cachePut(const std::string& k, std::shared_ptr<Image> img, size_t budget) {
        if (lruIndex.count(k)) return;
        lru.push_front({k, img});
        lruIndex[k] = lru.begin();
        lruBytes += img->px.size();
        while (lruBytes > budget / 2 && !lru.empty()) {
            lruBytes -= lru.back().second->px.size();
            lruIndex.erase(lru.back().first);
            lru.pop_back();
        }
    }
};

namespace {

struct Frame {
    const json* project = nullptr;
    const json* comp = nullptr;
    double t = 0;
    double scale = 1;
    int W = 0, H = 0;
    RenderSettings rs;
    RenderStats* stats = nullptr;
    int depth = 0;
    bool hasCamera = false;
    Camera3D cam;
    std::vector<Light3D> lights;
    double dofFocus = 0, dofAperture = 0;
    bool dof = false;
};

struct LayerBuf {
    Image img;
    Rect bounds;
    double opacity = 1;
};

}  // namespace

// ---------------------------------------------------------------- class with all render logic
class RenderJob {
   public:
    RenderJob(Renderer* r, Renderer::Impl* impl, MediaProvider* media, ExpressionEngine* expr, AudioEngine* audio)
        : R(r), I(impl), media(media), expr(expr), audio(audio) {}

    Renderer* R;
    Renderer::Impl* I;
    MediaProvider* media;
    ExpressionEngine* expr;
    AudioEngine* audio;

    EvalContext ctxFor(const Frame& F, const json& layer, double t) const {
        EvalContext c;
        c.project = F.project;
        c.comp = F.comp;
        c.layer = &layer;
        c.compTime = t;
        c.layerTime = t - layer.value("start", 0.0);
        c.fps = compFps(*F.comp);
        c.expr = expr;
        c.audio = audio;
        return c;
    }

    // Local transform of a layer (without parent): T(pos) Rz Ry Rx S T(-anchor), including behaviours.
    Mat4 localMatrix(const Frame& F, const json& comp, const json& L, double t, double* opacityOut = nullptr) const {
        Frame tmp = F;
        tmp.comp = &comp;
        EvalContext ctx = ctxFor(tmp, L, t);
        const json& T = jobj(L, "transform");
        ctx.propPath = "transform";
        Vec3 anchor = propVec3(T, "anchor", ctx, {0, 0, 0});
        Vec3 pos = propVec3(T, "position", ctx, {0, 0, 0});
        Vec3 scl = propVec3(T, "scale", ctx, {100, 100, 100});
        double rz = propNumber(T, "rotation", ctx, 0);
        double rx = 0, ry = 0;
        bool threeD = L.value("threeD", false);
        if (threeD) {
            rx = propNumber(T, "rotationX", ctx, 0);
            ry = propNumber(T, "rotationY", ctx, 0);
        } else {
            pos.z = 0;
            anchor.z = 0;
        }
        TransformOffsets bo = evalBehaviors(L, ctx);
        pos = pos + bo.position;
        scl = scl * bo.scaleMul;
        rz += bo.rotation;
        if (opacityOut) *opacityOut = clampv(propNumber(T, "opacity", ctx, 100) / 100.0 * bo.opacityMul, 0.0, 1.0);
        Mat4 m = Mat4::translate(pos.x, pos.y, pos.z);
        if (threeD) {
            // Auto-orient cameras/lights toward point of interest is handled separately.
            m = m * Mat4::rotateZ(deg2rad(rz)) * Mat4::rotateY(deg2rad(ry)) * Mat4::rotateX(deg2rad(rx));
        } else {
            m = m * Mat4::rotateZ(deg2rad(rz));
        }
        m = m * Mat4::scale(scl.x / 100.0, scl.y / 100.0, threeD ? scl.z / 100.0 : 1.0) * Mat4::translate(-anchor.x, -anchor.y, -anchor.z);
        return m;
    }

    Mat4 worldMatrix(const Frame& F, const json& comp, const json& L, double t, double* opacity = nullptr) const {
        Mat4 m = localMatrix(F, comp, L, t, opacity);
        json p = L.value("parent", json());
        int guard = 0;
        while (p.is_string() && guard++ < 64) {
            const json* PL = findLayer(comp, p.get<std::string>());
            if (!PL) break;
            m = localMatrix(F, comp, *PL, t) * m;
            p = PL->value("parent", json());
        }
        return m;
    }

    void setupCamera(Frame& F) {
        const json& comp = *F.comp;
        int cw = comp.value("width", 1920), ch = comp.value("height", 1080);
        F.hasCamera = false;
        F.lights.clear();
        F.dof = false;
        const json* camL = nullptr;
        bool any3D = false;
        for (auto& L : jarr(comp, "layers")) {
            if (!L.value("enabled", true) || !layerActiveAt(L, F.t)) continue;
            std::string ty = L.value("type", "");
            if (ty == "camera" && !camL) camL = &L;
            if (L.value("threeD", false) && ty != "camera" && ty != "light") any3D = true;
        }
        F.cam.width = F.W;
        F.cam.height = F.H;
        F.cam.scale = F.scale;
        if (camL) {
            EvalContext ctx = ctxFor(F, *camL, F.t);
            const json& C = (*camL)["camera"];
            Mat4 wm = worldMatrix(F, comp, *camL, F.t);
            Vec3 eye = wm.transformPoint(propVec3((*camL)["transform"], "anchor", ctx, {0, 0, 0}));
            Vec3 poi = propVec3(C, "poi", ctx, {cw / 2.0, ch / 2.0, 0});
            if (C.value("autoOrient", std::string("poi")) == "poi") {
                F.cam.view = lookAt(eye, poi, {0, 1, 0});
                // Apply camera roll (z rotation) on top.
                double rz = propNumber((*camL)["transform"], "rotation", ctx, 0);
                if (rz != 0) F.cam.view = Mat4::rotateZ(deg2rad(-rz)) * F.cam.view;
            } else {
                Mat4 inv;
                Mat4 cm = wm;
                if (cm.inverse(inv)) F.cam.view = inv;
            }
            // View must map the comp center plane so x/y are relative to the optical axis.
            F.cam.zoom = std::max(1.0, propNumber(C, "zoom", ctx, cw * 1.2));
            F.cam.position = eye;
            F.cam.near_ = C.value("near", 1.0);
            F.cam.far_ = C.value("far", 20000.0);
            F.hasCamera = true;
            if (C.value("dof", false)) {
                F.dof = true;
                F.dofFocus = propNumber(C, "focusDistance", ctx, F.cam.zoom);
                F.dofAperture = propNumber(C, "aperture", ctx, 25);
            }
        } else if (any3D) {
            double zoom = cw * 1.2;
            Vec3 eye{cw / 2.0, ch / 2.0, -zoom};
            F.cam.view = lookAt(eye, {cw / 2.0, ch / 2.0, 0}, {0, 1, 0});
            F.cam.zoom = zoom;
            F.cam.position = eye;
            F.hasCamera = true;
        }
        for (auto& L : jarr(comp, "layers")) {
            if (L.value("type", "") != "light" || !L.value("enabled", true) || !layerActiveAt(L, F.t)) continue;
            EvalContext ctx = ctxFor(F, L, F.t);
            const json& LD = L["light"];
            Light3D l;
            std::string kind = LD.value("kind", std::string("point"));
            l.kind = kind == "ambient" ? Light3D::Kind::Ambient : kind == "spot" ? Light3D::Kind::Spot : kind == "directional" ? Light3D::Kind::Directional : Light3D::Kind::Point;
            Mat4 wm = worldMatrix(F, comp, L, F.t);
            l.position = wm.transformPoint(propVec3(L["transform"], "anchor", ctx, {0, 0, 0}));
            Vec3 poi = propVec3(LD, "poi", ctx, {cw / 2.0, ch / 2.0, 0});
            l.direction = (poi - l.position).normalized();
            l.color = propColor(LD, "color", ctx, Color(1, 1, 1));
            l.intensity = propNumber(LD, "intensity", ctx, 100) / 100.0;
            double cone = deg2rad(propNumber(LD, "coneAngle", ctx, 90) / 2);
            double feather = propNumber(LD, "coneFeather", ctx, 50) / 100.0;
            l.coneCos = std::cos(cone);
            l.featherCos = std::cos(cone * (1 - feather));
            l.radius = LD.value("falloff", std::string("none")) == "none" ? 0 : propNumber(LD, "radius", ctx, 1500);
            l.castShadows = LD.value("castShadows", false);
            l.shadowDarkness = propNumber(LD, "shadowDarkness", ctx, 60) / 100.0;
            F.lights.push_back(l);
        }
    }

    // Layer-space plane -> render pixels homography. Returns false if (partly) behind camera.
    bool layerHomography(const Frame& F, const json& L, const Mat4& world, Mat3& H, double* depthOut = nullptr) const {
        bool threeD = L.value("threeD", false) && F.hasCamera;
        // Small reference square at the layer origin keeps all points in front of the camera for rotated layers.
        Vec2 src[4] = {{0, 0}, {8, 0}, {8, 8}, {0, 8}};
        Vec2 dst[4];
        double dsum = 0;
        for (int i = 0; i < 4; ++i) {
            Vec3 w = world.transformPoint({src[i].x, src[i].y, 0});
            if (threeD) {
                double d;
                if (!F.cam.project(w, dst[i], d)) return false;
                dsum += d;
            } else {
                dst[i] = {w.x * F.scale, w.y * F.scale};
            }
        }
        if (depthOut) *depthOut = dsum / 4;
        return Mat3::quadToQuad(src, dst, H);
    }

    // Content rectangle in layer space.
    bool contentRect(const Frame& F, const json& L, double t, double& x0, double& y0, double& x1, double& y1) {
        std::string ty = L.value("type", "");
        EvalContext ctx = ctxFor(F, L, t);
        if (L.contains("solid")) {
            x0 = 0; y0 = 0; x1 = L["solid"].value("width", 100); y1 = L["solid"].value("height", 100);
            return true;
        }
        if (ty == "video" || ty == "image") {
            const json* a = findAsset(*F.project, L.value("asset", ""));
            if (!a) return false;
            x0 = 0; y0 = 0; x1 = a->value("width", 100); y1 = a->value("height", 100);
            return true;
        }
        if (ty == "precomp") {
            const json* c = findComp(*F.project, L["precomp"].value("comp", ""));
            if (!c) return false;
            x0 = 0; y0 = 0; x1 = c->value("width", 100); y1 = c->value("height", 100);
            return true;
        }
        if (ty == "text") {
            auto font = FontManager::instance().get(L["text"].value("font", ""));
            if (!font) return false;
            TextStyle st = textStyle(L["text"], ctx);
            TextLayout lay = layoutText(utf8ToU32(propString(L["text"], "content", ctx, "")), *font, st);
            double hw = st.align == "left" ? 0 : st.align == "right" ? lay.width : lay.width / 2;
            x0 = -hw; x1 = x0 + std::max(lay.width, 10.0);
            if (st.align == "right") { x0 = -lay.width; x1 = 0; }
            if (st.align == "left") { x0 = 0; x1 = lay.width; }
            y0 = lay.top; y1 = lay.top + lay.height;
            return true;
        }
        if (ty == "shape") {
            ShapeGeometry g = buildShapeGeometry(L["shape"], ctx, 1.0);
            Polys all;
            for (auto& c : g.copies) {
                for (auto& it : c.fillItems)
                    for (auto ct : it.second) { for (auto& p : ct.pts) { Vec3 q = c.xf.transformPoint({p.x, p.y, 0}); p = {q.x, q.y}; } all.push_back(ct); }
                for (auto ct : c.stroke) { for (auto& p : ct.pts) { Vec3 q = c.xf.transformPoint({p.x, p.y, 0}); p = {q.x, q.y}; } all.push_back(ct); }
            }
            Rect r = polysBounds(all);
            if (r.empty()) return false;
            double sw = jobj(L["shape"], "stroke").value("enabled", false) ? propNumber(L["shape"]["stroke"], "width", ctx, 0) / 2 : 0;
            x0 = r.x0 - sw; y0 = r.y0 - sw; x1 = r.x1 + sw; y1 = r.y1 + sw;
            return true;
        }
        if (ty == "particles") {
            Vec2 esz = propVec2(L["particles"], "emitterSize", ctx, {0, 0});
            double s = std::max(60.0, propNumber(L["particles"], "velocity", ctx, 100) * 0.5);
            x0 = -esz.x / 2 - s; x1 = esz.x / 2 + s; y0 = -esz.y / 2 - s; y1 = esz.y / 2 + s;
            return true;
        }
        if (ty == "model3d") {
            double s = propNumber(L["model"], "size", ctx, 300) / 2;
            x0 = -s; y0 = -s; x1 = s; y1 = s;
            return true;
        }
        if (ty == "captions") {
            x0 = 0; y0 = 0; x1 = F.comp->value("width", 1920); y1 = F.comp->value("height", 1080);
            return true;
        }
        return false;
    }

    TextStyle textStyle(const json& T, const EvalContext& ctx) const {
        TextStyle st;
        st.size = std::max(1.0, propNumber(T, "size", ctx, 72));
        st.tracking = propNumber(T, "tracking", ctx, 0);
        st.leading = propNumber(T, "leading", ctx, 1.2);
        st.baselineShift = propNumber(T, "baselineShift", ctx, 0);
        st.align = T.value("align", std::string("center"));
        st.boxWidth = T.value("boxWidth", 0.0);
        return st;
    }

    // ------------------------------------------------------------ text drawing
    struct GlyphDraw {
        Polys polys;  // layer space
        Color color;
        double opacity;
        double blur;
        int line;
        bool clip;
    };

    std::vector<GlyphDraw> buildGlyphs(const Font& font, const TextLayout& lay, const std::vector<GlyphAnim>& anims, double size, Color fill,
                                        bool italic, double tol, uint32_t scrambleSeed,
                                        const std::function<void(size_t, GlyphDraw&)>& perGlyph = nullptr) const {
        std::vector<GlyphDraw> out;
        double extraTrack = 0;
        std::vector<BezierPath> contours;
        for (size_t i = 0; i < lay.glyphs.size(); ++i) {
            const GlyphInfo& g = lay.glyphs[i];
            const GlyphAnim& a = i < anims.size() ? anims[i] : GlyphAnim();
            extraTrack += a.tracking;
            if (g.whitespace || a.opacity <= 0.001 || a.scale == 0) continue;
            uint32_t cp = g.cp;
            if (a.scramble) {
                static const char32_t pool[] = U"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789#$%&@";
                cp = pool[hash32((uint32_t)i * 977u + scrambleSeed) % 41];
            }
            font.glyphContours(cp, size, contours);
            GlyphDraw d;
            d.color = fill;
            if (a.hasFill) {
                float k = (float)a.fillAmount;
                d.color = Color(fill.r + (a.fill.r - fill.r) * k, fill.g + (a.fill.g - fill.g) * k, fill.b + (a.fill.b - fill.b) * k, fill.a);
            }
            d.opacity = a.opacity;
            d.blur = a.blur;
            d.line = g.lineIndex;
            d.clip = a.clipAmount > 0;
            Vec2 center = g.origin + Vec2(g.advance / 2 + extraTrack, -size * 0.35);
            double rot = deg2rad(a.rotation), ca = std::cos(rot), sa = std::sin(rot);
            for (auto& c : contours) {
                Polys ps;
                flattenInto(c, tol, ps);
                for (auto& ct : ps) {
                    for (auto& p : ct.pts) {
                        Vec2 q = p + g.origin + Vec2(extraTrack, 0);
                        if (italic) q.x += -(p.y) * 0.2;
                        Vec2 rel = (q - center) * a.scale;
                        q = center + Vec2(rel.x * ca - rel.y * sa, rel.x * sa + rel.y * ca) + a.offset;
                        p = q;
                    }
                    d.polys.push_back(std::move(ct));
                }
            }
            if (perGlyph) perGlyph(i, d);
            out.push_back(std::move(d));
        }
        return out;
    }

    void mapPolys(Polys& ps, const Mat3& H) const {
        for (auto& c : ps)
            for (auto& p : c.pts) p = H.apply(p.x, p.y);
    }

    double hScale(const Mat3& H) const {
        // Approximate linear scale of homography at origin.
        Vec2 a = H.apply(0, 0), b = H.apply(1000, 0), c = H.apply(0, 1000);
        return std::sqrt(std::max(1e-12, std::fabs((b - a).cross(c - a)))) / 1000.0;
    }

    void drawGlyphs(Image& img, Rect& bounds, std::vector<GlyphDraw>& glyphs, const Mat3& H, double strokeWidth, Color strokeColor,
                    const TextLayout& lay, double size) const {
        Rect full{0, 0, img.w, img.h};
        double lscale = hScale(H);
        Image blurLayer;
        double blurSum = 0;
        int blurCount = 0;
        std::map<int, Coverage> lineClip;
        for (auto& g : glyphs) {
            Polys mapped = g.polys;
            mapPolys(mapped, H);
            Image* target = &img;
            if (g.blur > 0.25) {
                if (blurLayer.empty()) blurLayer.resize(img.w, img.h);
                target = &blurLayer;
                blurSum += g.blur;
                ++blurCount;
            }
            if (strokeWidth > 0) {
                Polys sp = strokePolys(g.polys, strokeWidth, LineCap::Round, LineJoin::Round);
                mapPolys(sp, H);
                Coverage sc = rasterize(sp, full);
                Paint p;
                p.color = strokeColor;
                p.opacity = (float)g.opacity;
                fillCoverage(*target, sc, p);
                bounds = bounds.unite(sc.r);
            }
            Coverage cov = rasterize(mapped, full);
            if (g.clip && g.line < (int)lay.lineY.size()) {
                auto it = lineClip.find(g.line);
                if (it == lineClip.end()) {
                    double y = lay.lineY[g.line];
                    double w = lay.width + size * 4;
                    Polys box{{{{-w, y - size}, {w, y - size}, {w, y + size * 0.3}, {-w, y + size * 0.3}}, true}};
                    mapPolys(box, H);
                    it = lineClip.emplace(g.line, rasterize(box, full)).first;
                }
                const Coverage& lc = it->second;
                for (int y = cov.r.y0; y < cov.r.y1; ++y)
                    for (int x = cov.r.x0; x < cov.r.x1; ++x) {
                        float m = (x >= lc.r.x0 && x < lc.r.x1 && y >= lc.r.y0 && y < lc.r.y1) ? lc.at(x, y) : 0.f;
                        cov.a[(size_t)(y - cov.r.y0) * cov.r.width() + (x - cov.r.x0)] *= m;
                    }
            }
            Paint p;
            p.color = g.color;
            p.opacity = (float)g.opacity;
            fillCoverage(*target, cov, p);
            bounds = bounds.unite(cov.r);
        }
        if (!blurLayer.empty() && blurCount) {
            Rect bb = bounds;
            gaussianBlur(blurLayer, blurSum / blurCount * lscale, bb);
            compositeImage(img, blurLayer, BlendMode::Normal, 1.f, bb);
            bounds = bounds.unite(bb);
        }
    }

    // ------------------------------------------------------------ content
    void drawRaster(const Frame& F, const Image& src, double nativeW, double nativeH, const Mat3& H, Image& dst, Rect& bounds) const {
        if (src.empty()) return;
        // Map image pixel coords -> layer coords (native size) -> render.
        double kx = nativeW / src.w, ky = nativeH / src.h;
        Mat3 imgToLayer;
        imgToLayer.m[0] = kx; imgToLayer.m[4] = ky;
        Mat3 imgToRender = H * imgToLayer;
        Mat3 inv;
        if (!imgToRender.inverse(inv)) return;
        Rect r = warpImage(dst, src, inv, 1.0f, {0, 0, dst.w, dst.h}, F.rs.draft ? 0 : 1);
        bounds = bounds.unite(r);
    }

    std::shared_ptr<Image> renderPrecomp(const Frame& F, const json& L, double t) {
        const json* sub = findComp(*F.project, L["precomp"].value("comp", ""));
        if (!sub || F.depth > 16) return nullptr;
        double st = layerSourceTime(L, t);
        json overrides = jobj(L["precomp"], "controls");
        json defs = jarr(L["precomp"], "controlDefs");
        std::string key = formatString("pc|%llu|%s|%.6f|%.4f|", (unsigned long long)F.rs.revision, sub->value("id", "").c_str(), st, F.scale) +
                          (defs.empty() ? std::string() : overrides.dump()) + (F.rs.exportMode ? "|x" : "");
        if (F.rs.revision != 0) {
            std::lock_guard<std::mutex> lk(I->cacheMutex);
            if (auto c = I->cacheGet(key)) {
                if (F.stats) F.stats->cacheHits++;
                return c;
            }
        }
        if (F.stats) F.stats->cacheMisses++;
        const json* compToRender = sub;
        json patched;
        if (!defs.empty()) {
            patched = *sub;
            for (auto& d : defs) {
                std::string name = d.value("name", "");
                if (!overrides.contains(name)) continue;
                json* lay = findLayerMut(patched, d.value("layer", ""));
                if (!lay) continue;
                json* prop = resolvePathMut(*lay, d.value("path", ""), false);
                if (!prop) continue;
                const json& v = overrides[name];
                std::string kind = d.value("type", "");
                if (kind == "duration") {
                    // Duration control time-stretches keyframes of the bound layer.
                    continue;
                }
                if (prop->is_object() && (prop->contains("v") || prop->contains("k"))) {
                    if (kind == "intensity" && hasKeyframes(*prop)) {
                        double base = d.value("base", 100.0);
                        double k = v.get<double>() / std::max(1e-6, base);
                        for (auto& kf : (*prop)["k"])
                            if (kf["v"].is_number()) kf["v"] = kf["v"].get<double>() * k;
                    } else {
                        prop->erase("k");
                        (*prop)["v"] = v;
                    }
                } else {
                    *prop = v;
                }
            }
            compToRender = &patched;
        }
        Frame SF;
        SF.project = F.project;
        SF.comp = compToRender;
        SF.t = st;
        SF.scale = F.scale;
        SF.W = std::max(1, (int)std::lround(compToRender->value("width", 1920) * F.scale));
        SF.H = std::max(1, (int)std::lround(compToRender->value("height", 1080) * F.scale));
        SF.rs = F.rs;
        SF.rs.transparentBackground = false;
        SF.stats = F.stats;
        SF.depth = F.depth + 1;
        auto img = std::make_shared<Image>(renderComp(SF));
        if (F.rs.revision != 0) {
            std::lock_guard<std::mutex> lk(I->cacheMutex);
            I->cachePut(key, img, R ? 256u << 20 : 0);
        }
        return img;
    }

    void drawParticles(const Frame& F, const json& L, double t, const Mat3& H, Image& dst, Rect& bounds) const {
        EvalContext ctx = ctxFor(F, L, t);
        const json& P = L["particles"];
        auto parts = simulateParticles(P, ctx, t - L.value("in", 0.0));
        if ((int)parts.size() > F.rs.particleBudget) parts.resize(F.rs.particleBudget);
        std::string shape = P.value("shape", std::string("circle"));
        double ls = hScale(H);
        Rect full{0, 0, dst.w, dst.h};
        Image tmp(dst.w, dst.h);
        Rect tb;
        for (auto& p : parts) {
            if (p.opacity <= 0.003 || p.size <= 0) continue;
            Vec2 c = H.apply(p.pos.x, p.pos.y);
            double r = std::max(0.35, p.size * ls / 2);
            Polys poly;
            if (shape == "line") {
                Vec2 a = H.apply(p.prevPos.x, p.prevPos.y);
                if ((a - c).length() < 0.5) a = c - Vec2(0, 1);
                poly = strokePolys({Contour{{a, c}, false}}, std::max(0.7, r), LineCap::Round, LineJoin::Round);
            } else if (shape == "square") {
                double rot = deg2rad(p.rotation), ca = std::cos(rot), sa = std::sin(rot);
                Contour ct;
                for (auto& q : {Vec2(-1, -0.6), Vec2(1, -0.6), Vec2(1, 0.6), Vec2(-1, 0.6)}) ct.pts.push_back(c + Vec2(q.x * r * ca - q.y * r * sa, q.x * r * sa + q.y * r * ca));
                poly.push_back(ct);
            } else if (shape == "star") {
                Polys s;
                flattenInto(BezierPath::star(c.x, c.y, 5, r * 1.4, r * 0.6, p.rotation, false), 0.5, s);
                poly = s;
            } else {
                int n = clampv((int)(r * 2), 6, 32);
                Contour ct;
                for (int k = 0; k < n; ++k) ct.pts.push_back(c + Vec2(std::cos(2 * kPi * k / n) * r, std::sin(2 * kPi * k / n) * r));
                poly.push_back(ct);
            }
            Coverage cov = rasterize(poly, full);
            Paint paint;
            if (shape == "soft") {
                paint.type = Paint::Type::Radial;
                paint.p0 = c;
                paint.p1 = c + Vec2(r, 0);
                paint.stops = {{0.f, Color(p.color.r, p.color.g, p.color.b, 1)}, {1.f, Color(p.color.r, p.color.g, p.color.b, 0)}};
            }
            paint.color = p.color;
            paint.opacity = (float)p.opacity;
            fillCoverage(tmp, cov, paint);
            tb = tb.unite(cov.r);
        }
        if (P.value("glow", false) && !tb.empty()) {
            Rect gb = tb;
            std::map<std::string, Value> gp = {{"threshold", Value::number(20)}, {"radius", Value::number(12)}, {"intensity", Value::number(120)}};
            EffectEnv env;
            env.scale = F.scale;
            applyEffect("stylize.glow", gp, tmp, gb, env);
            tb = gb;
        }
        compositeImage(dst, tmp, BlendMode::Normal, 1.f, tb);
        bounds = bounds.unite(tb);
    }

    void drawCaptions(const Frame& F, const json& L, double t, Image& dst, Rect& bounds) const {
        const json& C = L["captions"];
        const json* cur = nullptr;
        for (auto& c : jarr(C, "items"))
            if (t >= c.value("start", 0.0) && t < c.value("end", 0.0)) { cur = &c; break; }
        if (!cur) return;
        const json& S = jobj(C, "style");
        auto font = FontManager::instance().get(S.value("font", std::string("DejaVuSans-Bold")));
        if (!font) return;
        int cw = F.comp->value("width", 1920), ch = F.comp->value("height", 1080);
        std::string text = cur->value("text", "");
        if (S.value("uppercase", false))
            for (auto& c : text) c = (char)std::toupper((unsigned char)c);
        TextStyle st;
        st.size = S.value("size", 64.0);
        st.boxWidth = S.value("maxWidth", 0.84) * cw;
        st.align = "center";
        st.leading = 1.15;
        TextLayout lay = layoutText(utf8ToU32(text), *font, st);
        double s0 = cur->value("start", 0.0), s1 = cur->value("end", 0.0);
        double ad = std::max(0.01, S.value("animationDuration", 0.15));
        double uIn = clampv((t - s0) / ad, 0.0, 1.0), uOut = clampv((s1 - t) / ad, 0.0, 1.0);
        std::string ain = S.value("animationIn", std::string("fade")), aout = S.value("animationOut", std::string("fade"));
        double op = 1, sc = 1, dy = 0;
        auto anim = [&](const std::string& a, double u) {
            double e = 1 - std::pow(1 - u, 3);
            if (a == "fade") op *= u;
            else if (a == "pop") { sc *= 0.6 + 0.4 * Interp::fromName("backOut").apply(u); op *= std::min(1.0, u * 2); }
            else if (a == "slide") { dy += (1 - e) * st.size * 0.6; op *= u; }
        };
        anim(ain, uIn);
        anim(aout, uOut);
        json pj = S.value("position", json({0.5, 0.86}));
        Vec2 pos = pj.is_array() && pj.size() >= 2 ? Vec2(pj[0].get<double>(), pj[1].get<double>()) : Vec2(0.5, 0.86);
        // Layer opacity applies; caption block centered at position.
        Mat3 H;
        double cx = pos.x * cw, cy = pos.y * ch + dy;
        H.m[0] = sc * F.scale; H.m[2] = cx * F.scale;
        H.m[4] = sc * F.scale; H.m[5] = cy * F.scale;
        // Word timing -> active word index.
        int activeWord = -1;
        std::vector<double> wordStart;
        if (cur->contains("words")) {
            int wi = 0;
            for (auto& w : (*cur)["words"]) {
                wordStart.push_back(w.value("s", 0.0));
                if (t >= w.value("s", 0.0)) activeWord = wi;
                ++wi;
            }
        }
        bool typewriter = ain == "typewriter";
        int speaker = cur->value("speaker", 0);
        Color fill = parseColor(S.value("color", json({1, 1, 1, 1})));
        json sc_ = jarr(S, "speakerColors");
        if (speaker > 0 && speaker < (int)sc_.size()) fill = parseColor(sc_[speaker]);
        Color hl = parseColor(S.value("highlightColor", json({1, 0.85, 0.1, 1})));
        bool highlight = S.value("highlightActiveWord", true) && activeWord >= 0;
        std::vector<GlyphAnim> anims(lay.glyphs.size());
        for (size_t i = 0; i < lay.glyphs.size(); ++i) {
            anims[i].opacity = op;
            if (typewriter && lay.glyphs[i].wordIndex < (int)wordStart.size() && t < wordStart[lay.glyphs[i].wordIndex]) anims[i].opacity = 0;
        }
        auto glyphs = buildGlyphs(*font, lay, anims, st.size, fill, false, 0.5 / std::max(0.05, F.scale), 0,
                                  [&](size_t i, GlyphDraw& d) {
                                      if (highlight && lay.glyphs[i].wordIndex == activeWord) d.color = hl;
                                  });
        Rect full{0, 0, dst.w, dst.h};
        Image tmp(dst.w, dst.h);
        Rect tb;
        if (S.value("background", false)) {
            double pad = st.size * 0.3, rad = S.value("backgroundRadius", 16.0);
            for (size_t li = 0; li < lay.lineWidths.size(); ++li) {
                double w = lay.lineWidths[li] + pad * 2, y = lay.lineY[li];
                Polys box;
                flattenInto(BezierPath::rect(0, y - st.size * 0.33, w, st.size * 1.15, rad), 0.5, box);
                mapPolys(box, H);
                Coverage cov = rasterize(box, full);
                Paint p;
                p.color = parseColor(S.value("backgroundColor", json({0, 0, 0, 0.6})));
                p.opacity = (float)op;
                fillCoverage(tmp, cov, p);
                tb = tb.unite(cov.r);
            }
        }
        double sw = S.value("strokeWidth", 0.0);
        if (S.value("shadow", true)) {
            Image sh(dst.w, dst.h);
            Rect sb;
            std::vector<GlyphDraw> sg = glyphs;
            for (auto& g : sg) { g.color = Color(0, 0, 0, 0.75f); g.blur = 0; }
            Mat3 Hs = H;
            Hs.m[2] += st.size * 0.06 * F.scale;
            Hs.m[5] += st.size * 0.06 * F.scale;
            drawGlyphs(sh, sb, sg, Hs, sw, Color(0, 0, 0, 0.75f), lay, st.size);
            gaussianBlur(sh, st.size * 0.08 * F.scale, sb);
            compositeImage(tmp, sh, BlendMode::Normal, 1.f, sb);
            tb = tb.unite(sb);
        }
        drawGlyphs(tmp, tb, glyphs, H, sw, parseColor(S.value("strokeColor", json({0, 0, 0, 1}))), lay, st.size);
        compositeImage(dst, tmp, BlendMode::Normal, 1.f, tb);
        bounds = bounds.unite(tb);
    }

    std::shared_ptr<const Mesh> meshForLayer(const Frame& F, const json& L, const EvalContext& ctx, Mat4& normalize) {
        const json& M = L["model"];
        std::shared_ptr<const Mesh> mesh;
        std::string assetId = M.value("asset", std::string());
        if (!assetId.empty()) {
            const json* a = findAsset(*F.project, assetId);
            if (a) mesh = media->model(*a);
        }
        if (!mesh) {
            std::string prim = M.value("primitive", std::string("cube"));
            if (prim.empty()) prim = "cube";
            std::lock_guard<std::mutex> lk(I->cacheMutex);
            auto& slot = I->meshCache["prim:" + prim];
            if (!slot) slot = std::make_shared<Mesh>(makePrimitive(prim, 1.0));
            mesh = slot;
        }
        Vec3 mn, mx;
        mesh->bounds(mn, mx);
        double ext = std::max({mx.x - mn.x, mx.y - mn.y, mx.z - mn.z, 1e-9});
        double size = propNumber(M, "size", ctx, 300);
        Vec3 c = (mn + mx) * 0.5;
        normalize = Mat4::scale(size / ext, size / ext, size / ext) * Mat4::translate(-c.x, -c.y, -c.z);
        return mesh;
    }

    Material materialFor(const json& M, const EvalContext& ctx) const {
        Material m;
        const json& mat = jobj(M, "material");
        m.baseColor = propColor(mat, "baseColor", ctx, Color(0.8f, 0.8f, 0.8f));
        m.metallic = (float)propNumber(mat, "metallic", ctx, 0);
        m.roughness = (float)propNumber(mat, "roughness", ctx, 0.5);
        m.emission = propColor(mat, "emission", ctx, Color(0, 0, 0));
        m.emissionStrength = (float)propNumber(mat, "emissionStrength", ctx, 0);
        m.opacity = (float)(propNumber(mat, "opacity", ctx, 100) / 100.0);
        return m;
    }

    void drawMesh(const Frame& F, const json& L, const Mesh& mesh, const Mat4& model, Image& dst, Rect& bounds, bool castShadow, bool wire,
                  bool receive, float opacity) {
        Camera3D cam = F.cam;
        if (!F.hasCamera) {
            int cw = F.comp->value("width", 1920), ch = F.comp->value("height", 1080);
            double zoom = cw * 1.2;
            cam.view = lookAt({cw / 2.0, ch / 2.0, -zoom}, {cw / 2.0, ch / 2.0, 0}, {0, 1, 0});
            cam.zoom = zoom;
            cam.position = {cw / 2.0, ch / 2.0, -zoom};
            cam.width = F.W;
            cam.height = F.H;
            cam.scale = F.scale;
        }
        // Camera view is centered on the optical axis; shift so the comp center maps to the viewport center.
        std::vector<float> zbuf;
        MeshRenderOptions opt;
        opt.wireframe = wire;
        opt.receiveShadows = receive;
        opt.opacity = opacity;
        std::vector<float> shadow;
        for (auto& l : F.lights) {
            if (l.castShadows && l.kind == Light3D::Kind::Directional && castShadow) {
                Vec3 mn, mx;
                mesh.bounds(mn, mx);
                Vec3 c = model.transformPoint((mn + mx) * 0.5);
                double rad = 0;
                for (auto& p : {mn, mx}) rad = std::max(rad, (model.transformPoint(p) - c).length());
                buildShadowMap({{&mesh, model}}, l, c, std::max(10.0, rad * 2), 512, shadow, opt.shadowViewProj);
                opt.shadowMap = &shadow;
                opt.shadowSize = 512;
                break;
            }
        }
        Image tmp(dst.w, dst.h);
        renderMesh(tmp, zbuf, mesh, model, cam, F.lights, opt);
        compositeImage(dst, tmp, BlendMode::Normal, 1.f, {0, 0, dst.w, dst.h});
        bounds = {0, 0, dst.w, dst.h};
        (void)L;
    }

    // Masks -> coverage multiplier over the whole render. Returns false if no masks.
    bool maskCoverage(const Frame& F, const json& L, double t, const Mat3& H, std::vector<float>& m) const {
        auto it = L.find("masks");
        if (it == L.end() || !it->is_array() || it->empty()) return false;
        EvalContext ctx = ctxFor(F, L, t);
        int W = F.W, Hh = F.H;
        Rect full{0, 0, W, Hh};
        bool first = true;
        double ls = hScale(H);
        for (auto& mk : *it) {
            std::string mode = mk.value("mode", std::string("add"));
            if (mode == "none") continue;
            Value pv = evalProperty(mk["path"], ctx);
            BezierPath bp = BezierPath::fromValue(pv);
            Polys ps = flatten(bp, 0.5 / std::max(1e-3, ls));
            double expansion = propNumber(mk, "expansion", ctx, 0);
            if (expansion != 0) ps = offsetPolysApprox(ps, -expansion);
            mapPolys(ps, H);
            Coverage c = rasterize(ps, full);
            double feather = propNumber(mk, "feather", ctx, 0) * ls;
            if (feather > 0.5) blurCoverage(c, feather);
            float op = (float)(propNumber(mk, "opacity", ctx, 100) / 100.0);
            bool inv = mk.value("inverted", false);
            if (first) {
                m.assign((size_t)W * Hh, (mode == "add" || mode == "lighten" || mode == "difference") ? 0.f : 1.f);
                first = false;
            }
            for (int y = 0; y < Hh; ++y)
                for (int x = 0; x < W; ++x) {
                    float cv = (y >= c.r.y0 && y < c.r.y1 && x >= c.r.x0 && x < c.r.x1) ? c.at(x, y) : 0.f;
                    if (inv) cv = 1 - cv;
                    cv *= op;
                    float& d = m[(size_t)y * W + x];
                    if (mode == "add") d = d + cv - d * cv;
                    else if (mode == "subtract") d = d * (1 - cv);
                    else if (mode == "intersect") d = d * cv;
                    else if (mode == "lighten") d = std::max(d, cv);
                    else if (mode == "darken") d = std::min(d, cv);
                    else if (mode == "difference") d = std::fabs(d - cv);
                }
        }
        return !first;
    }

    std::map<std::string, Value> evalParams(const Frame& F, const json& L, const json& effect, double t) const {
        std::map<std::string, Value> out;
        EvalContext ctx = ctxFor(F, L, t);
        ctx.propPath = "effects." + effect.value("id", std::string()) + ".params";
        for (json tmp_ = jobj(effect, "params"); auto& [k, v] : tmp_.items()) {
            EvalContext c = ctx;
            c.propPath += "." + k;
            if (v.is_object()) out[k] = evalProperty(v, c);
            else out[k] = Value::fromJson(v);
        }
        return out;
    }

    void runEffects(const Frame& F, const json& L, double t, Image& img, Rect& bounds) {
        if (F.rs.bypassEffects) return;
        EffectEnv env;
        env.compTime = t;
        env.layerTime = t - L.value("start", 0.0);
        env.fps = compFps(*F.comp);
        env.scale = F.scale;
        env.compW = F.comp->value("width", 1920);
        env.compH = F.comp->value("height", 1080);
        const json* project = F.project;
        MediaProvider* mp = media;
        env.lutLoader = [project, mp](const std::string& id) -> std::shared_ptr<LutData> {
            const json* a = findAsset(*project, id);
            return a ? mp->lut(*a) : nullptr;
        };
        const json* comp = F.comp;
        AudioEngine* ae = audio;
        env.spectrum = [ae, project, comp](double tt, int bands) { return ae->spectrum(*project, *comp, tt, bands); };
        env.waveform = [ae, project, comp](double tt, int n, double w) { return ae->waveform(*project, *comp, tt, n, w); };
        bool anySolo = false;
        for (auto& e : jarr(L, "effects"))
            if (e.value("solo", false) || (!F.rs.soloEffect.empty() && e.value("id", "") == F.rs.soloEffect)) anySolo = true;
        for (auto& e : jarr(L, "effects")) {
            if (!e.value("enabled", true)) continue;
            bool solo = e.value("solo", false) || (!F.rs.soloEffect.empty() && e.value("id", "") == F.rs.soloEffect);
            if (anySolo && !solo) continue;
            std::string type = e.value("type", "");
            if (type.rfind("time.", 0) == 0) continue;
            auto params = evalParams(F, L, e, t);
            double mix = e.value("mix", 100.0) / 100.0;
            Image before;
            if (mix < 0.999) before = img;
            if (const json* comp = findCompositeEffect(type)) {
                // Extension composite effect: chain of built-ins with parameter bindings.
                for (auto& step : jarr(*comp, "chain")) {
                    std::map<std::string, Value> sp;
                    for (json tmp_ = jobj(step, "params"); auto& [k, v] : tmp_.items()) {
                        if (v.is_string() && v.get<std::string>().rfind("$", 0) == 0) {
                            std::string ref = v.get<std::string>().substr(1);
                            if (params.count(ref)) sp[k] = params[ref];
                        } else sp[k] = Value::fromJson(v);
                    }
                    applyEffect(step.value("type", ""), sp, img, bounds, env);
                }
            } else {
                applyEffect(type, params, img, bounds, env);
            }
            if (F.stats) F.stats->passes++;
            if (mix < 0.999) {
                for (size_t i = 0; i < img.px.size(); ++i) img.px[i] = (uint8_t)(before.px[i] + (img.px[i] - before.px[i]) * mix);
            }
        }
    }

    struct TransitionState {
        double opacity = 1;
        Mat3 post;  // render-space affine applied after layer mapping
        bool hasPost = false;
        std::vector<std::pair<std::string, double>> ops;  // image ops with progress
        json tr;
    };

    TransitionState transitionAt(const Frame& F, const json& L, double t) const {
        TransitionState s;
        double in = L.value("in", 0.0), out = L.value("out", 0.0);
        auto handle = [&](const json& tr, double p, bool isIn) {
            if (!tr.is_object()) return;
            p = clampv(p, 0.0, 1.0);
            p = Interp::fromJson(tr.contains("easing") ? tr["easing"] : json("easeInOut")).apply(p);
            std::string type = tr.value("type", std::string("fade"));
            const json& P = jobj(tr, "params");
            double W = F.W, H = F.H;
            double q = 1 - p;  // amount of "transition" remaining
            Mat3 m;
            auto dirVec = [&](const std::string& d) {
                if (d == "left") return Vec2(-1, 0);
                if (d == "right") return Vec2(1, 0);
                if (d == "up") return Vec2(0, -1);
                return Vec2(0, 1);
            };
            if (type == "fade" || type == "dissolve" || type == "fadeColor" || type == "lightLeak") s.opacity *= (type == "dissolve" || type == "fadeColor") ? 1.0 : p;
            if (type == "push" || type == "slide") {
                Vec2 d = dirVec(P.value("direction", std::string(isIn ? "left" : "left")));
                double dist = type == "push" ? 1.0 : P.value("distance", 30.0) / 100.0;
                // Incoming clips arrive from the opposite side of the push direction.
                Vec2 off = (isIn ? d * -1 : d) * q * dist;
                m.m[2] = off.x * W; m.m[5] = off.y * H;
                if (type == "slide") s.opacity *= p;
                s.hasPost = true;
            } else if (type == "zoom") {
                double amt = P.value("amount", 50.0) / 100.0;
                double k = P.value("zoomOut", false) ? 1 - amt * q * 0.9 : 1 + amt * q;
                m.m[0] = k; m.m[4] = k; m.m[2] = W / 2 * (1 - k); m.m[5] = H / 2 * (1 - k);
                s.opacity *= p;
                s.hasPost = true;
            } else if (type == "spin") {
                double ang = deg2rad(P.value("angle", 180.0)) * q * (isIn ? -1 : 1);
                double k = 1 - (1 - P.value("scale", 0.0) / 100.0) * q;
                double c = std::cos(ang) * k, sn = std::sin(ang) * k;
                m.m[0] = c; m.m[1] = -sn; m.m[3] = sn; m.m[4] = c;
                m.m[2] = W / 2 - (c * W / 2 - sn * H / 2);
                m.m[5] = H / 2 - (sn * W / 2 + c * H / 2);
                s.opacity *= std::min(1.0, p * 1.5);
                s.hasPost = true;
            } else if (type != "fade") {
                s.ops.push_back({type, p});
                s.tr = tr;
            }
            if (s.hasPost) s.post = m * s.post;
        };
        const json& ti = L.value("transitionIn", json());
        const json& to = L.value("transitionOut", json());
        if (ti.is_object()) {
            double d = std::max(1e-6, ti.value("duration", 0.5));
            if (t < in + d) handle(ti, (t - in) / d, true);
        }
        if (to.is_object()) {
            double d = std::max(1e-6, to.value("duration", 0.5));
            if (t > out - d) handle(to, (out - t) / d, false);
        }
        return s;
    }

    void applyTransitionOps(const Frame& F, const TransitionState& ts, Image& img, Rect& bounds, double t) const {
        for (auto& [type, p] : ts.ops) {
            const json& P = jobj(ts.tr, "params");
            double q = 1 - p;
            EffectEnv env;
            env.compTime = t;
            env.scale = F.scale;
            if (type == "dissolve") {
                double grain = std::max(1.0, P.value("grain", 2.0) * F.scale);
                for (int y = 0; y < img.h; ++y)
                    for (int x = 0; x < img.w; ++x) {
                        double n = hashToUnit((uint32_t)(x / grain) * 73856093u ^ (uint32_t)(y / grain) * 19349663u);
                        if (n > p) std::memset(img.at(x, y), 0, 4);
                    }
            } else if (type == "fadeColor") {
                Color c = parseColor(P.value("color", json({0, 0, 0, 1})));
                // First half: blend toward color; at p < 0.5 the clip is fully the color fading in.
                float k = (float)clampv(q * 2, 0.0, 1.0);
                for (int y = 0; y < img.h; ++y)
                    for (int x = 0; x < img.w; ++x) {
                        uint8_t* d = img.at(x, y);
                        float a = d[3] / 255.f;
                        d[0] = (uint8_t)(d[0] + (c.r * a * 255 - d[0]) * k);
                        d[1] = (uint8_t)(d[1] + (c.g * a * 255 - d[1]) * k);
                        d[2] = (uint8_t)(d[2] + (c.b * a * 255 - d[2]) * k);
                    }
            } else if (type == "wipe" || type == "shapeReveal") {
                double W = img.w, H = img.h;
                double soft = std::max(0.001, P.value("softness", 10.0) / 100.0);
                double ang = deg2rad(P.value("angle", 0.0));
                std::string shape = P.value("shape", std::string("circle"));
                Vec2 c{0.5, 0.5};
                if (P.contains("center") && P["center"].is_array()) c = {P["center"][0].get<double>(), P["center"][1].get<double>()};
                for (int y = 0; y < img.h; ++y)
                    for (int x = 0; x < img.w; ++x) {
                        double v;
                        if (type == "wipe") {
                            double u = ((x / W - 0.5) * std::cos(ang) + (y / H - 0.5) * std::sin(ang)) + 0.5;
                            v = smoothstep(u - soft, u, p * (1 + soft));
                        } else {
                            double dx = x / W - c.x, dy = (y / H - c.y) * H / W;
                            double d = shape == "diamond" ? (std::fabs(dx) + std::fabs(dy)) : shape == "rectangle" ? std::max(std::fabs(dx), std::fabs(dy) * W / H) : std::sqrt(dx * dx + dy * dy);
                            double r = p * 0.9;
                            v = 1 - smoothstep(r - soft * 0.2, r, d);
                        }
                        if (v >= 1) continue;
                        uint8_t* d = img.at(x, y);
                        for (int k = 0; k < 4; ++k) d[k] = (uint8_t)(d[k] * v);
                    }
            } else if (type == "blur") {
                Rect b = bounds;
                gaussianBlur(img, P.value("radius", 40.0) * q * F.scale, b);
                bounds = b;
                for (auto& v : img.px) v = (uint8_t)(v * std::min(1.0, p * 2));
            } else if (type == "displacement") {
                std::map<std::string, Value> pp = {{"amount", Value::number(P.value("amount", 80.0) * q)}, {"size", Value::number(P.value("scale", 60.0))},
                                                   {"complexity", Value::number(2)}, {"evolution", Value::number(p * 180)}};
                applyEffect("distort.turbulence", pp, img, bounds, env);
                for (auto& v : img.px) v = (uint8_t)(v * p);
            } else if (type == "glitch") {
                double a = P.value("amount", 60.0) * q;
                std::map<std::string, Value> p1 = {{"amount", Value::number(a * 0.3)}};
                applyEffect("glitch.rgbSplit", p1, img, bounds, env);
                std::map<std::string, Value> p2 = {{"amount", Value::number(a)}, {"blockSize", Value::number(40)}, {"density", Value::number(40 * q)}, {"speed", Value::number(24)}};
                applyEffect("glitch.blockDisplace", p2, img, bounds, env);
            } else if (type == "lightLeak") {
                Color c = parseColor(P.value("color", json({1, 0.6, 0.2, 1})));
                double in = P.value("intensity", 100.0) / 100.0 * std::sin(q * kPi);
                for (int y = 0; y < img.h; ++y)
                    for (int x = 0; x < img.w; ++x) {
                        double g = std::max(0.0, 1 - std::sqrt(std::pow((double)x / img.w - q, 2) + std::pow((double)y / img.h - 0.3, 2)) * 1.4) * in;
                        uint8_t* d = img.at(x, y);
                        d[0] = (uint8_t)std::min(255.0, d[0] + c.r * g * d[3]);
                        d[1] = (uint8_t)std::min(255.0, d[1] + c.g * g * d[3]);
                        d[2] = (uint8_t)std::min(255.0, d[2] + c.b * g * d[3]);
                    }
            }
        }
    }

    // Render one layer (content, masks, effects, transitions) into a full-size buffer.
    bool renderLayerCore(const Frame& F, const json& L, double t, LayerBuf& out) {
        std::string ty = L.value("type", "");
        out.img.resize(F.W, F.H);
        out.bounds = {};
        double opacity = 1;
        Mat4 world = worldMatrix(F, *F.comp, L, t, &opacity);
        TransitionState ts = transitionAt(F, L, t);
        opacity *= ts.opacity;
        out.opacity = opacity;
        if (opacity <= 0.0005) return false;
        Mat3 H;
        double depth = 0;
        bool threeD = L.value("threeD", false);
        bool meshLayer = ty == "model3d";
        bool extrudeText = threeD && ty == "text" && jobj(L["text"], "extrude").value("enabled", false);
        bool extrudeShape = threeD && ty == "shape" && jobj(L["shape"], "extrude").value("enabled", false);
        if (!meshLayer && ty != "captions") {
            if (!layerHomography(F, L, world, H, &depth)) return false;
            if (ts.hasPost) H = ts.post * H;
        }
        EvalContext ctx = ctxFor(F, L, t);
        Rect full{0, 0, F.W, F.H};
        if (ty == "solid" || ty == "adjustment") {
            double w = L["solid"].value("width", 100), h = L["solid"].value("height", 100);
            Polys q{{{{0, 0}, {w, 0}, {w, h}, {0, h}}, true}};
            mapPolys(q, H);
            Coverage c = rasterize(q, full);
            Paint p;
            p.color = propColor(L["solid"], "color", ctx, Color(1, 1, 1));
            fillCoverage(out.img, c, p);
            out.bounds = c.r;
        } else if (ty == "image" || ty == "video") {
            const json* a = findAsset(*F.project, L.value("asset", ""));
            if (!a) return false;
            double nw = a->value("width", 100), nh = a->value("height", 100);
            double ls = hScale(H);
            int mw = std::max(16, (int)std::ceil(nw * ls)), mh = std::max(16, (int)std::ceil(nh * ls));
            ImagePtr src = ty == "image" ? media->image(*a, mw, mh) : media->videoFrame(*a, layerSourceTime(L, t), mw, mh);
            if (!src || src->empty()) {
                if (F.stats) F.stats->warnings.push_back("Missing media for layer '" + L.value("name", "") + "'.");
                // Draw an explicit offline placeholder (checker) so missing media is never silent.
                Polys q{{{{0, 0}, {nw, 0}, {nw, nh}, {0, nh}}, true}};
                mapPolys(q, H);
                Coverage c = rasterize(q, full);
                Paint p;
                p.color = Color(0.5f, 0.1f, 0.4f, 1);
                fillCoverage(out.img, c, p);
                out.bounds = c.r;
            } else {
                drawRaster(F, *src, nw, nh, H, out.img, out.bounds);
            }
        } else if (ty == "precomp") {
            auto img = renderPrecomp(F, L, t);
            const json* c = findComp(*F.project, L["precomp"].value("comp", ""));
            if (!img || !c) return false;
            drawRaster(F, *img, c->value("width", 100), c->value("height", 100), H, out.img, out.bounds);
        } else if (ty == "text" && !extrudeText) {
            const json& T = L["text"];
            auto font = FontManager::instance().get(T.value("font", ""));
            if (!font) return false;
            TextStyle st = textStyle(T, ctx);
            std::u32string text = utf8ToU32(propString(T, "content", ctx, ""));
            TextLayout lay = layoutText(text, *font, st);
            auto anims = evalTextAnimators(T, lay, ctx);
            double ls = hScale(H);
            double tol = 0.35 / std::max(1e-3, ls);
            Color fill = propColor(T, "fill", ctx, Color(1, 1, 1));
            auto glyphs = buildGlyphs(*font, lay, anims, st.size, fill, T.value("fauxItalic", false), tol, (uint32_t)std::floor(t * 12));
            const json& bg = jobj(T, "background");
            if (bg.value("enabled", false)) {
                double pad = propNumber(bg, "padding", ctx, 20), rad = propNumber(bg, "radius", ctx, 16);
                double x0 = st.align == "left" ? 0 : st.align == "right" ? -lay.width : -lay.width / 2;
                Polys box;
                flattenInto(BezierPath::rect(x0 + lay.width / 2, lay.top + lay.height / 2, lay.width + pad * 2, lay.height + pad * 2, rad), tol, box);
                mapPolys(box, H);
                Coverage c = rasterize(box, full);
                Paint p;
                p.color = propColor(bg, "color", ctx, Color(0, 0, 0, 0.6f));
                fillCoverage(out.img, c, p);
                out.bounds = out.bounds.unite(c.r);
            }
            const json& sh = jobj(T, "shadow");
            if (sh.value("enabled", false)) {
                Image shImg(F.W, F.H);
                Rect sb;
                std::vector<GlyphDraw> sg = glyphs;
                Color sc = propColor(sh, "color", ctx, Color(0, 0, 0, 0.7f));
                for (auto& g : sg) { g.color = sc; g.blur = 0; }
                double ang = deg2rad(propNumber(sh, "angle", ctx, 135) - 90), dist = propNumber(sh, "distance", ctx, 8);
                Mat3 off;
                off.m[2] = std::cos(ang) * dist * F.scale;
                off.m[5] = std::sin(ang) * dist * F.scale;
                drawGlyphs(shImg, sb, sg, off * H, propNumber(T, "strokeWidth", ctx, 0), sc, lay, st.size);
                gaussianBlur(shImg, propNumber(sh, "blur", ctx, 8) * F.scale, sb);
                compositeImage(out.img, shImg, BlendMode::Normal, 1.f, sb);
                out.bounds = out.bounds.unite(sb);
            }
            drawGlyphs(out.img, out.bounds, glyphs, H, propNumber(T, "strokeWidth", ctx, 0), propColor(T, "strokeColor", ctx, Color(0, 0, 0)), lay, st.size);
        } else if (ty == "shape" && !extrudeShape) {
            const json& S = L["shape"];
            double ls = hScale(H);
            ShapeGeometry g = buildShapeGeometry(S, ctx, 0.35 / std::max(1e-3, ls));
            const json& fill = jobj(S, "fill");
            const json& stroke = jobj(S, "stroke");
            Mat3 Hinv;
            H.inverse(Hinv);
            for (auto& c : g.copies) {
                Mat3 xf;
                xf.m[0] = c.xf.at(0, 0); xf.m[1] = c.xf.at(0, 1); xf.m[2] = c.xf.at(0, 3);
                xf.m[3] = c.xf.at(1, 0); xf.m[4] = c.xf.at(1, 1); xf.m[5] = c.xf.at(1, 3);
                Mat3 HC = H * xf;
                Mat3 HCinv;
                HC.inverse(HCinv);
                if (fill.value("enabled", true) && !c.fillItems.empty()) {
                    // Exact merge ops via coverage combination.
                    Coverage acc;
                    bool firstItem = true;
                    for (auto& [op, polys] : c.fillItems) {
                        Polys mp = polys;
                        for (auto& ct : mp) ct.closed = true;
                        mapPolys(mp, HC);
                        Coverage cv = rasterize(mp, full);
                        if (firstItem) { acc = cv; firstItem = false; if (op != "add") { /* first item always adds */ } continue; }
                        Rect u = acc.r.unite(cv.r);
                        if (op == "intersect") u = acc.r.intersect(cv.r);
                        if (u.empty()) { acc = Coverage(); continue; }
                        Coverage nc;
                        nc.r = u;
                        nc.a.assign((size_t)u.width() * u.height(), 0.f);
                        for (int y = u.y0; y < u.y1; ++y)
                            for (int x = u.x0; x < u.x1; ++x) {
                                float a = (!acc.empty() && x >= acc.r.x0 && x < acc.r.x1 && y >= acc.r.y0 && y < acc.r.y1) ? acc.at(x, y) : 0.f;
                                float b = (x >= cv.r.x0 && x < cv.r.x1 && y >= cv.r.y0 && y < cv.r.y1) ? cv.at(x, y) : 0.f;
                                float r = op == "subtract" ? a * (1 - b) : op == "intersect" ? a * b : a + b - a * b;
                                nc.a[(size_t)(y - u.y0) * u.width() + (x - u.x0)] = r;
                            }
                        acc = std::move(nc);
                    }
                    if (!acc.empty()) {
                        Paint p;
                        std::string ft = fill.value("type", std::string("solid"));
                        p.color = propColor(fill, "color", ctx, Color(1, 1, 1));
                        p.opacity = (float)(propNumber(fill, "opacity", ctx, 100) / 100.0 * c.opacity);
                        if (ft != "solid" && fill.contains("gradient")) {
                            const json& G = fill["gradient"];
                            p.type = ft == "linear" ? Paint::Type::Linear : ft == "radial" ? Paint::Type::Radial : ft == "conic" ? Paint::Type::Conic : Paint::Type::Solid;
                            p.p0 = propVec2(G, "start", ctx, {-100, 0});
                            p.p1 = propVec2(G, "end", ctx, {100, 0});
                            for (auto& s : jarr(G, "stops"))
                                if (s.is_array() && s.size() >= 4)
                                    p.stops.push_back({s[0].get<float>(), Color(s[1].get<float>(), s[2].get<float>(), s[3].get<float>(), s.size() > 4 ? s[4].get<float>() : 1.f)});
                            std::sort(p.stops.begin(), p.stops.end(), [](auto& a, auto& b) { return a.first < b.first; });
                            p.inv = HCinv;
                        }
                        if (ft == "image" && fill.contains("asset")) {
                            const json* a = findAsset(*F.project, fill.value("asset", ""));
                            if (a) {
                                p.type = Paint::Type::ImageFill;
                                p.image = media->image(*a, 0, 0);
                                p.inv = HCinv;
                            }
                        }
                        fillCoverage(out.img, acc, p);
                        out.bounds = out.bounds.unite(acc.r);
                    }
                }
                if (stroke.value("enabled", false) && !c.stroke.empty()) {
                    double w = propNumber(stroke, "width", ctx, 4);
                    std::string cap = stroke.value("cap", std::string("round")), join = stroke.value("join", std::string("round"));
                    Polys sp = strokePolys(c.stroke, w, cap == "butt" ? LineCap::Butt : cap == "square" ? LineCap::Square : LineCap::Round,
                                           join == "miter" ? LineJoin::Miter : join == "bevel" ? LineJoin::Bevel : LineJoin::Round);
                    mapPolys(sp, HC);
                    Coverage cv = rasterize(sp, full);
                    Paint p;
                    p.color = propColor(stroke, "color", ctx, Color(1, 1, 1));
                    p.opacity = (float)(propNumber(stroke, "opacity", ctx, 100) / 100.0 * c.opacity);
                    fillCoverage(out.img, cv, p);
                    out.bounds = out.bounds.unite(cv.r);
                }
            }
        } else if (ty == "particles") {
            drawParticles(F, L, t, H, out.img, out.bounds);
        } else if (ty == "captions") {
            drawCaptions(F, L, t, out.img, out.bounds);
        } else if (meshLayer || extrudeText || extrudeShape) {
            Mat4 normalize;
            std::shared_ptr<const Mesh> mesh;
            Material front;
            if (meshLayer) {
                mesh = meshForLayer(F, L, ctx, normalize);
                if (!mesh) return false;
                Material m = materialFor(L["model"], ctx);
                // Override all mesh materials only if the user changed the default color or model has none.
                if (L["model"].value("asset", std::string()).empty() || mesh->materials.empty()) {
                    auto mm = std::make_shared<Mesh>(*mesh);
                    for (auto& x : mm->materials) {
                        auto tex = x.baseColorTex;
                        x = m;
                        x.baseColorTex = tex;
                    }
                    mesh = mm;
                }
            } else {
                // Extrusion of text or shape outlines.
                Polys outline;
                Color sideCol;
                double depthE;
                std::string key;
                if (extrudeText) {
                    const json& T = L["text"];
                    auto font = FontManager::instance().get(T.value("font", ""));
                    if (!font) return false;
                    TextStyle st = textStyle(T, ctx);
                    std::string content = propString(T, "content", ctx, "");
                    TextLayout lay = layoutText(utf8ToU32(content), *font, st);
                    auto glyphs = buildGlyphs(*font, lay, {}, st.size, Color(), false, 0.8, 0);
                    for (auto& g : glyphs)
                        for (auto& c : g.polys) outline.push_back(c);
                    front.baseColor = propColor(T, "fill", ctx, Color(1, 1, 1));
                    sideCol = propColor(T["extrude"], "sideColor", ctx, Color(0.5f, 0.5f, 0.5f));
                    depthE = propNumber(T["extrude"], "depth", ctx, 30);
                    key = formatString("xt|%s|%s|%.3f|%.3f|%.3f", T.value("font", "").c_str(), content.c_str(), st.size, st.tracking, depthE);
                } else {
                    const json& S = L["shape"];
                    ShapeGeometry g = buildShapeGeometry(S, ctx, 1.0);
                    for (auto& c : g.copies)
                        for (auto& it : c.fillItems)
                            for (auto ct : it.second) {
                                for (auto& p : ct.pts) { Vec3 q = c.xf.transformPoint({p.x, p.y, 0}); p = {q.x, q.y}; }
                                outline.push_back(ct);
                            }
                    front.baseColor = propColor(S["fill"], "color", ctx, Color(1, 1, 1));
                    sideCol = propColor(S["extrude"], "sideColor", ctx, Color(0.5f, 0.5f, 0.5f));
                    depthE = propNumber(S["extrude"], "depth", ctx, 40);
                    key = "xs|" + S.dump() + formatString("|%.3f", t);
                }
                Material side;
                side.baseColor = sideCol;
                std::shared_ptr<Mesh> m;
                {
                    std::lock_guard<std::mutex> lk(I->cacheMutex);
                    auto it = I->meshCache.find(key);
                    if (it != I->meshCache.end()) m = it->second;
                }
                if (!m) {
                    m = std::make_shared<Mesh>(extrudePolys(outline, depthE, front, side));
                    std::lock_guard<std::mutex> lk(I->cacheMutex);
                    if (I->meshCache.size() > 64) I->meshCache.clear();
                    I->meshCache[key] = m;
                } else {
                    m->materials = {front, side};
                }
                mesh = m;
                normalize = Mat4();
            }
            bool cast = meshLayer ? L["model"].value("castShadows", true) : true;
            bool recv = meshLayer ? L["model"].value("receiveShadows", true) : true;
            bool wire = meshLayer && L["model"].value("wireframe", false);
            drawMesh(F, L, *mesh, world * normalize, out.img, out.bounds, cast, wire, recv, 1.f);
        } else {
            return false;
        }
        // Planar 3D layers respond to lights (lambert at layer center).
        if (threeD && !meshLayer && !extrudeText && !extrudeShape && !F.lights.empty() && !out.bounds.empty()) {
            Vec3 n = world.transformDir({0, 0, -1}).normalized();
            Vec3 c = world.transformPoint({0, 0, 0});
            double r = 0, g = 0, b = 0;
            for (auto& l : F.lights) {
                if (l.kind == Light3D::Kind::Ambient) { r += l.color.r * l.intensity; g += l.color.g * l.intensity; b += l.color.b * l.intensity; continue; }
                Vec3 ld = l.kind == Light3D::Kind::Directional ? -l.direction : (l.position - c).normalized();
                double nl = std::fabs(n.dot(ld));
                double att = l.intensity;
                if (l.kind == Light3D::Kind::Spot) att *= smoothstep(l.coneCos, l.featherCos, (-ld).dot(l.direction));
                if (l.radius > 0) att *= clampv(1 - (l.position - c).length() / l.radius, 0.0, 1.0);
                r += l.color.r * att * nl; g += l.color.g * att * nl; b += l.color.b * att * nl;
            }
            Rect bb = out.bounds.intersect(full);
            for (int y = bb.y0; y < bb.y1; ++y)
                for (int x = bb.x0; x < bb.x1; ++x) {
                    uint8_t* d = out.img.at(x, y);
                    d[0] = (uint8_t)std::min<double>(d[3], d[0] * r);
                    d[1] = (uint8_t)std::min<double>(d[3], d[1] * g);
                    d[2] = (uint8_t)std::min<double>(d[3], d[2] * b);
                }
        }
        // Masks (layer space -> render).
        if (!meshLayer && ty != "captions") {
            std::vector<float> m;
            if (maskCoverage(F, L, t, H, m)) {
                for (size_t i = 0, n = (size_t)F.W * F.H; i < n; ++i) {
                    float v = m[i];
                    if (v >= 1) continue;
                    uint8_t* d = out.img.px.data() + i * 4;
                    for (int k = 0; k < 4; ++k) d[k] = (uint8_t)(d[k] * v + 0.5f);
                }
            }
        }
        if (out.bounds.empty() && ty != "adjustment") {
            // Effects such as generators can still draw on empty layers.
            out.bounds = {0, 0, 0, 0};
        }
        runEffects(F, L, t, out.img, out.bounds);
        if (!ts.ops.empty()) applyTransitionOps(F, ts, out.img, out.bounds, t);
        // Depth of field for 3D layers.
        if (F.dof && threeD && !meshLayer) {
            double blur = std::fabs(depth - F.dofFocus) * F.dofAperture * 0.0004 * F.scale;
            if (blur > 0.5) gaussianBlur(out.img, std::min(80.0, blur), out.bounds);
        }
        if (F.stats) F.stats->layersRendered++;
        return true;
    }

    const json* findEffectOfType(const json& L, const char* type) const {
        for (auto& e : jarr(L, "effects"))
            if (e.value("enabled", true) && e.value("type", "") == type) return &e;
        return nullptr;
    }

    // Time-aware wrapper: posterize time, echo, motion blur.
    bool renderLayer(const Frame& F, const json& L, double t, LayerBuf& out) {
        double fps = compFps(*F.comp);
        if (const json* pe = findEffectOfType(L, "time.posterize")) {
            auto p = evalParams(F, L, *pe, t);
            double pf = std::max(0.1, p["fps"].num(0, 8));
            t = std::floor(t * pf + 1e-6) / pf;
        }
        const json& mb = jobj(*F.comp, "motionBlur");
        int samples = 1;
        if (mb.value("enabled", false) && L.value("motionBlur", false)) {
            samples = F.rs.motionBlurSamples > 0 ? F.rs.motionBlurSamples : mb.value("samples", 8);
            if (F.rs.draft) samples = std::min(samples, 3);
            samples = clampv(samples, 1, 64);
        }
        auto core = [&](double tt, LayerBuf& lb) -> bool {
            if (const json* ee = findEffectOfType(L, "time.echo")) {
                auto p = evalParams(F, L, *ee, tt);
                int count = clampv((int)p["count"].num(0, 4), 1, 30);
                double interval = p["interval"].num(0, -0.05), decay = p["decay"].num(0, 50) / 100.0;
                std::string mode = p.count("mode") ? p["mode"].s : "composite";
                if (!renderLayerCore(F, L, tt, lb)) return false;
                Image acc(F.W, F.H);
                Rect ab;
                double w = 1;
                std::vector<std::pair<LayerBuf, double>> echoes;
                for (int i = count; i >= 1; --i) {
                    LayerBuf e;
                    double et = tt + interval * i;
                    if (et < L.value("in", 0.0) || et >= L.value("out", 0.0)) continue;
                    if (!renderLayerCore(F, L, et, e)) continue;
                    double ew = std::pow(1 - decay, i);
                    compositeImage(acc, e.img, mode == "add" ? BlendMode::Add : mode == "max" ? BlendMode::Lighten : BlendMode::Normal, (float)(ew * e.opacity),
                                   e.bounds);
                    ab = ab.unite(e.bounds);
                }
                compositeImage(acc, lb.img, mode == "add" ? BlendMode::Add : BlendMode::Normal, (float)lb.opacity, lb.bounds);
                lb.img = std::move(acc);
                lb.bounds = ab.unite(lb.bounds);
                lb.opacity = 1;
                (void)w;
                return true;
            }
            return renderLayerCore(F, L, tt, lb);
        };
        if (samples <= 1) return core(t, out);
        double shutter = mb.value("shutter", 180.0) / 360.0 / fps;
        std::vector<float> acc((size_t)F.W * F.H * 4, 0.f);
        Rect bounds;
        int got = 0;
        double opSum = 0;
        for (int i = 0; i < samples; ++i) {
            double st = t + shutter * ((double)i / (samples - 1) - 0.5);
            LayerBuf lb;
            if (!core(st, lb)) continue;
            ++got;
            opSum += lb.opacity;
            Rect b = lb.bounds.intersect({0, 0, F.W, F.H});
            bounds = bounds.unite(b);
            for (int y = b.y0; y < b.y1; ++y)
                for (int x = b.x0; x < b.x1; ++x) {
                    const uint8_t* s = lb.img.at(x, y);
                    float* d = &acc[((size_t)y * F.W + x) * 4];
                    float o = (float)lb.opacity;
                    for (int k = 0; k < 4; ++k) d[k] += s[k] * o;
                }
        }
        if (!got) return false;
        out.img.resize(F.W, F.H);
        for (int y = bounds.y0; y < bounds.y1; ++y)
            for (int x = bounds.x0; x < bounds.x1; ++x) {
                float* s = &acc[((size_t)y * F.W + x) * 4];
                uint8_t* d = out.img.at(x, y);
                for (int k = 0; k < 4; ++k) d[k] = (uint8_t)std::min(255.f, s[k] / samples + 0.5f);
            }
        out.bounds = bounds;
        out.opacity = 1;
        return true;
    }

    double layerDepth(const Frame& F, const json& L) {
        if (!F.hasCamera) return 0;
        Mat4 w = worldMatrix(F, *F.comp, L, F.t);
        double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        Vec3 c = w.transformPoint({0, 0, 0});
        if (contentRect(F, L, F.t, x0, y0, x1, y1)) c = w.transformPoint({(x0 + x1) / 2, (y0 + y1) / 2, 0});
        return F.cam.view.transformPoint(c).z;
    }

    Image renderComp(Frame& F) {
        const json& comp = *F.comp;
        Image acc(F.W, F.H);
        if (F.depth > 0 || !F.rs.transparentBackground) acc.fill(parseColor(comp.value("bg", json({0, 0, 0, 1}))));
        setupCamera(F);
        const json& layers = jarr(comp, "layers");
        // Layers used as track mattes are hidden from normal compositing.
        std::set<std::string> matteSources;
        bool anySolo = false;
        for (auto& L : layers) {
            if (L.value("matte", json()).is_object() && L.value("enabled", true)) matteSources.insert(L["matte"].value("layer", ""));
            if (L.value("solo", false) && L.value("enabled", true)) anySolo = true;
        }
        std::vector<const json*> order;
        for (size_t i = layers.size(); i-- > 0;) {
            const json& L = layers[i];
            std::string ty = L.value("type", "");
            if (!L.value("enabled", true) || !layerActiveAt(L, F.t)) continue;
            if (ty == "null" || ty == "camera" || ty == "light" || ty == "audio") continue;
            if (L.value("guide", false) && F.rs.exportMode) continue;
            if (matteSources.count(L.value("id", ""))) continue;
            if (anySolo && !L.value("solo", false)) continue;
            order.push_back(&L);
        }
        // Depth-sort runs of consecutive 3D layers (farther first).
        if (F.hasCamera) {
            size_t i = 0;
            while (i < order.size()) {
                if (!(*order[i]).value("threeD", false)) { ++i; continue; }
                size_t j = i;
                while (j < order.size() && (*order[j]).value("threeD", false)) ++j;
                std::vector<std::pair<double, const json*>> run;
                for (size_t k = i; k < j; ++k) run.push_back({layerDepth(F, *order[k]), order[k]});
                std::stable_sort(run.begin(), run.end(), [](auto& a, auto& b) { return a.first > b.first; });
                for (size_t k = i; k < j; ++k) order[k] = run[k - i].second;
                i = j;
            }
        }
        for (const json* Lp : order) {
            const json& L = *Lp;
            std::string ty = L.value("type", "");
            if (ty == "adjustment") {
                LayerBuf region;
                // Region = adjustment layer quad (with masks) times opacity.
                Image copy = acc;
                Rect b{0, 0, F.W, F.H};
                if (!renderLayerCoreRegion(F, L, F.t, region)) continue;
                runEffects(F, L, F.t, copy, b);
                for (size_t i = 0, n = (size_t)F.W * F.H; i < n; ++i) {
                    float m = region.img.px[i * 4 + 3] / 255.f * (float)region.opacity;
                    if (m <= 0) continue;
                    for (int k = 0; k < 4; ++k) acc.px[i * 4 + k] = (uint8_t)(acc.px[i * 4 + k] + (copy.px[i * 4 + k] - acc.px[i * 4 + k]) * m);
                }
                continue;
            }
            LayerBuf lb;
            if (!renderLayer(F, L, F.t, lb)) continue;
            const json& mt = L.value("matte", json());
            if (mt.is_object()) {
                const json* ML = findLayer(comp, mt.value("layer", ""));
                if (ML) {
                    LayerBuf mb;
                    std::string mode = mt.value("mode", std::string("alpha"));
                    bool inv = mt.value("invert", false);
                    MatteMode mm = mode == "luma" ? (inv ? MatteMode::LumaInverted : MatteMode::Luma) : (inv ? MatteMode::AlphaInverted : MatteMode::Alpha);
                    if (layerActiveAt(*ML, F.t) && renderLayer(F, *ML, F.t, mb)) {
                        if (mb.opacity < 1) for (auto& v : mb.img.px) v = (uint8_t)(v * mb.opacity);
                        applyTrackMatte(lb.img, mb.img, mm, {0, 0, F.W, F.H});
                    } else if (!inv) {
                        continue;  // matte absent -> nothing visible
                    }
                }
            }
            BlendMode bm = blendModeFromName(L.value("blend", std::string("normal")));
            compositeImage(acc, lb.img, bm, (float)lb.opacity, lb.bounds.intersect({0, 0, F.W, F.H}));
        }
        return acc;
    }

    // Adjustment layer region: solid quad coverage with masks; color irrelevant.
    bool renderLayerCoreRegion(const Frame& F, const json& L, double t, LayerBuf& out) {
        out.img.resize(F.W, F.H);
        double opacity = 1;
        Mat4 world = worldMatrix(F, *F.comp, L, t, &opacity);
        TransitionState ts = transitionAt(F, L, t);
        out.opacity = opacity * ts.opacity;
        Mat3 H;
        if (!layerHomography(F, L, world, H)) return false;
        double w = L["solid"].value("width", 100), h = L["solid"].value("height", 100);
        Polys q{{{{0, 0}, {w, 0}, {w, h}, {0, h}}, true}};
        mapPolys(q, H);
        Coverage c = rasterize(q, {0, 0, F.W, F.H});
        Paint p;
        fillCoverage(out.img, c, p);
        std::vector<float> m;
        if (maskCoverage(F, L, t, H, m))
            for (size_t i = 0, n = (size_t)F.W * F.H; i < n; ++i) out.img.px[i * 4 + 3] = (uint8_t)(out.img.px[i * 4 + 3] * m[i]);
        return true;
    }
};

// ====================================================================== Renderer
Renderer::Renderer(MediaProvider* media) : media_(media), expr_(createExpressionEngine()), audio_(media), impl_(new Impl) { impl_->R = this; }
Renderer::~Renderer() = default;

void Renderer::clearCaches() {
    std::lock_guard<std::mutex> lk(impl_->cacheMutex);
    impl_->lru.clear();
    impl_->lruIndex.clear();
    impl_->lruBytes = 0;
    impl_->meshCache.clear();
}

Image Renderer::renderFrame(const json& project, const std::string& compId, double t, const RenderSettings& rs, RenderStats* stats) {
    std::lock_guard<std::mutex> lk(renderMutex_);
    auto t0 = std::chrono::steady_clock::now();
    const json* comp = findComp(project, compId);
    if (!comp) comp = activeComp(project);
    if (!comp) return Image();
    audio_.setProject(&project);
    RenderJob job(this, impl_.get(), media_, expr_.get(), &audio_);
    Frame F;
    F.project = &project;
    F.comp = comp;
    F.t = t;
    F.scale = rs.scale;
    F.W = std::max(1, (int)std::lround(comp->value("width", 1920) * rs.scale));
    F.H = std::max(1, (int)std::lround(comp->value("height", 1080) * rs.scale));
    F.rs = rs;
    F.stats = stats;
    Image out = job.renderComp(F);
    if (stats) stats->ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return out;
}

Mat4 Renderer::layerWorldMatrix(const json& comp, const json& layer, double t) {
    RenderJob job(this, impl_.get(), media_, expr_.get(), &audio_);
    Frame F;
    F.comp = &comp;
    F.t = t;
    return job.worldMatrix(F, comp, layer, t);
}

bool Renderer::layerQuad(const json& project, const std::string& compId, const std::string& layerId, double t, Vec2 out[4]) {
    std::lock_guard<std::mutex> lk(renderMutex_);
    const json* comp = findComp(project, compId);
    if (!comp) return false;
    const json* L = findLayer(*comp, layerId);
    if (!L) return false;
    RenderJob job(this, impl_.get(), media_, expr_.get(), &audio_);
    Frame F;
    F.project = &project;
    F.comp = comp;
    F.t = t;
    F.scale = 1;
    F.W = comp->value("width", 1920);
    F.H = comp->value("height", 1080);
    job.setupCamera(F);
    double x0, y0, x1, y1;
    if (!job.contentRect(F, *L, t, x0, y0, x1, y1)) {
        x0 = -20; y0 = -20; x1 = 20; y1 = 20;  // nulls etc.
    }
    Mat4 w = job.worldMatrix(F, *comp, *L, t);
    Mat3 H;
    if (L->value("type", "") == "captions") {
        out[0] = {x0, y0}; out[1] = {x1, y0}; out[2] = {x1, y1}; out[3] = {x0, y1};
        return true;
    }
    if (!job.layerHomography(F, *L, w, H)) return false;
    Vec2 c[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    for (int i = 0; i < 4; ++i) out[i] = H.apply(c[i].x, c[i].y);
    return true;
}

std::string Renderer::hitTest(const json& project, const std::string& compId, double t, double x, double y) {
    const json* comp = findComp(project, compId);
    if (!comp) return {};
    for (auto& L : jarr(*comp, "layers")) {
        std::string ty = L.value("type", "");
        if (!L.value("enabled", true) || !layerActiveAt(L, t) || ty == "audio" || ty == "camera" || ty == "light" || ty == "captions") continue;
        Vec2 q[4];
        if (!layerQuad(project, compId, L.value("id", ""), t, q)) continue;
        std::vector<Vec2> poly(q, q + 4);
        bool in = false;
        for (size_t i = 0, j = 3; i < 4; j = i++) {
            if (((poly[i].y > y) != (poly[j].y > y)) && (x < (poly[j].x - poly[i].x) * (y - poly[i].y) / (poly[j].y - poly[i].y + 1e-300) + poly[i].x)) in = !in;
        }
        if (in) return L.value("id", "");
    }
    return {};
}

}  // namespace mf
