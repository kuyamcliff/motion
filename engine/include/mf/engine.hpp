// High-level engine facade: one open project, command layer (validate -> apply -> record undo -> mark dirty ->
// journal -> invalidate caches), autosave/recovery, rendering, audio, captions, scripts, packages.
// Used by the Android JNI bridge and by the host CLI / scenario runner.
#pragma once
#include <atomic>
#include <map>
#include <mutex>

#include "captions.hpp"
#include "document.hpp"
#include "renderer.hpp"
#include "storage.hpp"

namespace mf {

struct EngineConfig {
    std::string dataDir;    // app-private root (projects/, presets/, capsules/, scripts/, extensions/, settings.json)
    std::string cacheDir;   // deletable caches
    std::string fontsDir;   // user fonts
    std::string modelsDir;  // speech models
    double autosaveInterval = 15.0;
};

struct OpOutcome {
    bool ok = false;
    std::string error, label;
    json data = json::object();
    json toJson() const;
};

class Engine {
   public:
    Engine(EngineConfig cfg, MediaProvider* media);
    ~Engine();
    const EngineConfig& config() const { return cfg_; }
    ProjectStore& store() { return store_; }
    Renderer& renderer() { return renderer_; }
    MediaProvider* media() { return media_; }

    // ---------------------------------------------------------------- projects
    std::string createProject(const std::string& name, int w, int h, double fps, double duration, std::string* err = nullptr);
    std::string createProjectFromDoc(const json& doc, std::string* err = nullptr);
    bool openProject(const std::string& id, std::string& err, bool recover = false);
    void closeProject();  // saves, removes session lock
    // Test hook: forget the open project without saving or removing the session lock (as if the process died).
    void abandonProjectForTesting() { projectId_.clear(); }
    bool isOpen() const { return !projectId_.empty(); }
    const std::string& projectId() const { return projectId_; }
    Document& doc() { return doc_; }
    std::shared_ptr<const json> snapshot() const { return doc_.snapshot(); }
    std::string activeCompId() const;

    // ---------------------------------------------------------------- command layer
    OpOutcome apply(const json& op);
    // Interactive gesture: preview ops are applied on top of the gesture's base state and not recorded until commit.
    OpOutcome previewOp(const json& op);
    bool commitPreview(const std::string& label);
    void cancelPreview();
    bool undo();
    bool redo();
    bool jumpHistory(int index);
    json historyJson() const;
    json stateJson() const;  // canUndo/redo, labels, dirty, revision, saveStatus

    // ---------------------------------------------------------------- persistence
    bool save(std::string& err, bool version = false);
    // Called periodically by the host; saves if dirty and interval elapsed. Returns true if saved.
    bool autosaveTick(double now);
    void checkpoint(const std::string& reason);  // before risky operations (export, extension install)
    std::string saveStatus() const;

    // ---------------------------------------------------------------- rendering
    Image render(double t, double scale, RenderSettings rs = RenderSettings(), RenderStats* stats = nullptr, const std::string& compId = std::string());
    std::string hitTest(double t, double x, double y);
    json layerQuad(const std::string& layerId, double t);
    bool writeThumbnail(double t);
    float mixAudio(double t0, int frames, float* out, int sampleRate = 48000);

    // ---------------------------------------------------------------- captions
    AsrResult transcribeLayer(const std::string& layerId, const AsrOptions& opt, const SegmentOptions& seg = SegmentOptions());
    AsrResult transcribePcm(const std::vector<float>& pcm16k, double offset, const AsrOptions& opt, const SegmentOptions& seg = SegmentOptions());
    std::string defaultModelPath() const;

    // ---------------------------------------------------------------- misc
    json diagnostics();
    json capabilities() const;
    void clearCaches();
    std::string lastError() const { return lastError_; }

   private:
    EngineConfig cfg_;
    MediaProvider* media_;
    ProjectStore store_;
    Renderer renderer_;
    Document doc_;
    std::string projectId_;
    double lastSave_ = 0;
    uint64_t savedRevision_ = 0;
    uint64_t journalSeq_ = 0;
    std::string lastError_;
    std::string saveStatus_ = "saved";
    mutable std::mutex mutex_;
    json gestureBase_;
    bool inGesture_ = false;
};

// Runs a .mftest scenario (line-based commands) against an engine; returns log + pass/fail.
struct ScenarioResult {
    bool ok = false;
    std::vector<std::string> log;
    std::string error;
    int line = 0;
    json outputs = json::object();
};
ScenarioResult runScenario(Engine& engine, const std::string& script, const std::string& workDir);

}  // namespace mf
