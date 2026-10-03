// Composition renderer (CPU compositor). Deterministic: same project + time + settings -> same pixels.
#pragma once
#include <list>
#include <map>
#include <mutex>

#include "audio.hpp"
#include "effects.hpp"
#include "expressions.hpp"
#include "three_d.hpp"

namespace mf {

class MediaProvider {
   public:
    virtual ~MediaProvider() = default;
    // Video frame for asset at media time, preferably no larger than maxW x maxH (premultiplied RGBA).
    virtual ImagePtr videoFrame(const json& asset, double sourceTime, int maxW, int maxH) = 0;
    virtual ImagePtr image(const json& asset, int maxW, int maxH) = 0;
    virtual std::shared_ptr<const AudioBuffer> audio(const json& asset) = 0;
    virtual std::shared_ptr<const Mesh> model(const json& asset);
    virtual std::shared_ptr<LutData> lut(const json& asset);
    virtual bool available(const json& asset);
    // Resolve an asset to a readable local path ("" if not representable as a path).
    virtual std::string localPath(const json& asset) { return asset.value("path", std::string()); }

   protected:
    std::mutex cacheMutex_;
    std::map<std::string, std::shared_ptr<const Mesh>> meshCache_;
    std::map<std::string, std::shared_ptr<LutData>> lutCache_;
};

// Media provider that reads files directly (images via stb, models, LUTs, WAV audio) and
// synthesizes procedural "test pattern" video assets (type video with "generator").
class FileMediaProvider : public MediaProvider {
   public:
    ImagePtr videoFrame(const json& asset, double sourceTime, int maxW, int maxH) override;
    ImagePtr image(const json& asset, int maxW, int maxH) override;
    std::shared_ptr<const AudioBuffer> audio(const json& asset) override;
    // Register externally decoded frames (e.g. image sequences) for an asset id.
    void putFrame(const std::string& assetId, int frameIndex, ImagePtr img);

   private:
    std::map<std::string, ImagePtr> images_;
    std::map<std::string, std::shared_ptr<const AudioBuffer>> audio_;
    std::map<std::pair<std::string, int>, ImagePtr> frames_;
};

ImagePtr generateTestPattern(const std::string& kind, int w, int h, double t, double fps);

struct RenderSettings {
    double scale = 1.0;        // output = comp size * scale
    bool exportMode = false;   // hides guide layers, full quality
    bool draft = false;        // preview shortcuts (nearest sampling, fewer motion blur samples)
    int motionBlurSamples = 0; // 0 = comp setting
    uint64_t revision = 0;     // project revision for internal caches
    bool transparentBackground = false;
    std::string soloEffect;    // effect id rendered solo (others bypassed) on its layer
    bool bypassEffects = false;
    int particleBudget = 100000;  // adaptive quality hook
    bool useProxies = true;    // preview decodes an asset's low-res "proxy" file when present (never in exportMode)
};

struct RenderStats {
    int layersRendered = 0, passes = 0, cacheHits = 0, cacheMisses = 0;
    int proxyFrames = 0;  // video frames decoded from proxies instead of the originals
    double ms = 0;
    std::vector<std::string> warnings;
};

// GPU draw list for one frame (see Renderer::renderPlan). Items are composited bottom→top with their blend mode and
// opacity; images are premultiplied RGBA. H maps item pixel space (image pixels, or asset pixels for video) to output
// pixels, including perspective for 3D layers.
struct PlanItem {
    enum class Kind { Raster, Video };
    Kind kind = Kind::Raster;
    std::string layerId;
    std::shared_ptr<const Image> image;  // Raster
    std::string cacheKey;                // Raster: non-empty when the image is reused across frames (GPU keeps the texture)
    Mat3 H;
    float opacity = 1;
    std::string blend = "normal";
    json asset;                          // Video: engine asset (may carry "proxy")
    double sourceTime = 0;               // Video: seconds into the media
    int width = 0, height = 0;           // item pixel space size (image size, or asset size for video)
};

struct RenderPlan {
    int W = 0, H = 0;
    float bg[4] = {0, 0, 0, 1};
    bool transparent = false;
    bool fallback = false;       // a single full-frame raster (features the GPU path does not composite yet)
    std::string fallbackReason;
    std::vector<PlanItem> items;
    int cachedRasters = 0, frameRasters = 0, videoItems = 0;
};

// Reference CPU compositor for a RenderPlan: the exact semantics the GPU compositor implements (bilinear sampling of
// each item through inverse(H), then the engine's blend formula). Used by tests and as a GPU-less fallback.
Image compositePlan(const RenderPlan& plan, MediaProvider* media);

class Renderer {
   public:
    // Frame as a GPU draw list instead of pixels: video layers become decode-and-sample items, layers with static
    // content become cached layer-space rasters transformed per frame, everything else is a per-frame raster.
    RenderPlan renderPlan(const json& project, const std::string& compId, double t, const RenderSettings& rs, RenderStats* stats = nullptr);
    explicit Renderer(MediaProvider* media);
    ~Renderer();
    Image renderFrame(const json& project, const std::string& compId, double t, const RenderSettings& rs, RenderStats* stats = nullptr);
    // Topmost visible layer containing comp-space point (full-res comp coords). Empty if none.
    std::string hitTest(const json& project, const std::string& compId, double t, double x, double y);
    // Layer quad corners in comp space (full res) for overlays / transform handles.
    bool layerQuad(const json& project, const std::string& compId, const std::string& layerId, double t, Vec2 out[4]);
    Mat4 layerWorldMatrix(const json& comp, const json& layer, double t);
    AudioEngine& audio() { return audio_; }
    ExpressionEngine* expressions() { return expr_.get(); }
    void clearCaches();
    void setMemoryBudget(size_t bytes) { budget_ = bytes; }

    struct Impl;

   private:
    MediaProvider* media_;
    std::unique_ptr<ExpressionEngine> expr_;
    AudioEngine audio_;
    std::unique_ptr<Impl> impl_;
    size_t budget_ = 256u << 20;
    std::mutex renderMutex_;
};

}  // namespace mf
