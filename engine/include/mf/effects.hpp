// Effect registry + CPU implementations. Effects operate on premultiplied RGBA images
// in composition space. Every parameter is an animatable property.
#pragma once
#include <map>

#include "common.hpp"
#include "property.hpp"

namespace mf {

struct ParamInfo {
    std::string name, label, kind;  // number|angle|percent|color|point|bool|enum|layer|asset|curve
    json def;
    double min = 0, max = 100;
    std::vector<std::string> options;  // for enum
    std::string help;
};

struct EffectInfo {
    std::string type, name, category, help;
    std::vector<ParamInfo> params;
    bool timeEffect = false;  // requires re-rendering at other times (echo, posterize time)
    bool gpuCost = false;     // expensive; used by performance diagnostics
    int costWeight = 1;
};

const std::vector<EffectInfo>& effectRegistry();
const EffectInfo* findEffect(const std::string& type);
json effectDefaultParams(const std::string& type);
std::vector<std::string> effectCategories();

// Extension-provided composite effects (chains of built-ins with bound params).
void registerCompositeEffect(const std::string& type, const json& definition);
void clearCompositeEffects();
const json* findCompositeEffect(const std::string& type);
// Snapshot of all registered composite (extension) effects: type -> definition.
std::map<std::string, json> compositeEffects();

// Behaviors (procedural motion modifiers) metadata.
struct BehaviorInfo {
    std::string type, name, help;
    std::vector<ParamInfo> params;
};
const std::vector<BehaviorInfo>& behaviorRegistry();
json behaviorDefaultParams(const std::string& type);

// Transitions metadata.
struct TransitionInfo {
    std::string type, name;
    std::vector<ParamInfo> params;
};
const std::vector<TransitionInfo>& transitionRegistry();

// Blend modes.
enum class BlendMode {
    Normal, Multiply, Screen, Overlay, SoftLight, HardLight, Darken, Lighten, ColorDodge, ColorBurn,
    Difference, Exclusion, Add, Subtract, Hue, Saturation, Color, Luminosity
};
BlendMode blendModeFromName(const std::string& s);
std::vector<std::string> blendModeNames();

// ---------------------------------------------------------------- processing
struct LutData;
struct EffectEnv {
    double compTime = 0, layerTime = 0, fps = 30;
    double scale = 1.0;  // render scale (preview < 1) so pixel-size params stay resolution independent
    int compW = 0, compH = 0;  // full-res comp size
    uint32_t seed = 0;
    std::function<std::shared_ptr<LutData>(const std::string& assetId)> lutLoader;
    // Audio analysis for audio-reactive generators: normalized magnitudes (0..1).
    std::function<std::vector<float>(double compTime, int bands)> spectrum;
    std::function<std::vector<float>(double compTime, int samples, double windowSec)> waveform;
};

// Apply a single effect in place. `bounds` is the region containing content (updated when effects grow it).
void applyEffect(const std::string& type, const std::map<std::string, Value>& params, Image& img, Rect& bounds,
                 const EffectEnv& env);

// 3D LUT (.cube / .3dl)
struct LutData {
    int size = 0;
    std::vector<float> data;  // size^3 * 3, r fastest
    std::string title;
    bool load(const std::string& path, std::string& err);
    bool parseCube(const std::string& text, std::string& err);
    bool parse3dl(const std::string& text, std::string& err);
    void sample(float r, float g, float b, float& or_, float& og, float& ob) const;
};

}  // namespace mf
