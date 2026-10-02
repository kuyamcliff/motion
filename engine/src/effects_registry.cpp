// Metadata for built-in effects, behaviours, transitions and blend modes.
#include <mutex>

#include "mf/effects.hpp"

namespace mf {

namespace {
ParamInfo num(const char* n, const char* label, double def, double mn, double mx, const char* help = "") {
    ParamInfo p; p.name = n; p.label = label; p.kind = "number"; p.def = def; p.min = mn; p.max = mx; p.help = help; return p;
}
ParamInfo ang(const char* n, const char* label, double def) {
    ParamInfo p; p.name = n; p.label = label; p.kind = "angle"; p.def = def; p.min = -360; p.max = 360; return p;
}
ParamInfo col(const char* n, const char* label, Color c) {
    ParamInfo p; p.name = n; p.label = label; p.kind = "color"; p.def = colorToJson(c); p.min = 0; p.max = 1; return p;
}
ParamInfo pt(const char* n, const char* label, double x, double y) {
    ParamInfo p; p.name = n; p.label = label; p.kind = "point"; p.def = json::array({x, y}); p.min = 0; p.max = 1;
    p.help = "Normalized position (0..1) relative to the composition."; return p;
}
ParamInfo bln(const char* n, const char* label, bool def) {
    ParamInfo p; p.name = n; p.label = label; p.kind = "bool"; p.def = def; p.min = 0; p.max = 1; return p;
}
ParamInfo enm(const char* n, const char* label, std::vector<std::string> opts, int def = 0) {
    ParamInfo p; p.name = n; p.label = label; p.kind = "enum"; p.options = opts; p.def = opts[def]; return p;
}
ParamInfo curve(const char* n, const char* label) {
    ParamInfo p; p.name = n; p.label = label; p.kind = "curve"; p.def = json::array({json::array({0, 0}), json::array({1, 1})}); return p;
}
ParamInfo asset(const char* n, const char* label) {
    ParamInfo p; p.name = n; p.label = label; p.kind = "asset"; p.def = ""; return p;
}

EffectInfo fx(const char* type, const char* name, const char* cat, std::vector<ParamInfo> params, const char* help = "",
              int cost = 1) {
    EffectInfo e; e.type = type; e.name = name; e.category = cat; e.params = std::move(params); e.help = help;
    e.costWeight = cost; e.gpuCost = cost >= 3; return e;
}

std::vector<EffectInfo> buildRegistry() {
    std::vector<EffectInfo> r;
    // Blur
    r.push_back(fx("blur.gaussian", "Gaussian Blur", "Blur",
                   {num("radius", "Radius", 10, 0, 250, "Blur radius in composition pixels. Larger radii cost more CPU/GPU time."),
                    enm("dimensions", "Dimensions", {"both", "horizontal", "vertical"})},
                   "Smooth blur. Affects final export.", 2));
    r.push_back(fx("blur.directional", "Directional Blur", "Blur", {ang("angle", "Direction", 0), num("length", "Length", 20, 0, 500)}, "", 3));
    r.push_back(fx("blur.radial", "Radial (Spin) Blur", "Blur", {pt("center", "Center", 0.5, 0.5), num("amount", "Amount", 10, 0, 90)}, "", 3));
    r.push_back(fx("blur.zoom", "Zoom Blur", "Blur", {pt("center", "Center", 0.5, 0.5), num("amount", "Amount", 20, 0, 100)}, "", 3));
    r.push_back(fx("blur.bokeh", "Bokeh Blur", "Blur", {num("radius", "Radius", 8, 0, 60), num("boost", "Highlight Boost", 30, 0, 100)}, "", 4));
    // Distortion
    r.push_back(fx("distort.ripple", "Ripple", "Distortion",
                   {pt("center", "Center", 0.5, 0.5), num("amplitude", "Amplitude", 10, 0, 200), num("wavelength", "Wavelength", 40, 2, 800),
                    num("speed", "Speed", 1, -20, 20), ang("phase", "Phase", 0)}));
    r.push_back(fx("distort.wave", "Wave Warp", "Distortion",
                   {num("amplitude", "Amplitude", 10, 0, 300), num("wavelength", "Wavelength", 80, 2, 2000), ang("angle", "Direction", 0),
                    num("speed", "Speed", 1, -20, 20), ang("phase", "Phase", 0)}));
    r.push_back(fx("distort.bulge", "Bulge", "Distortion", {pt("center", "Center", 0.5, 0.5), num("radius", "Radius", 300, 1, 4000), num("amount", "Amount", 50, -100, 100)}));
    r.push_back(fx("distort.pinch", "Pinch", "Distortion", {pt("center", "Center", 0.5, 0.5), num("radius", "Radius", 300, 1, 4000), num("amount", "Amount", 50, 0, 100)}));
    r.push_back(fx("distort.twirl", "Twirl / Swirl", "Distortion", {pt("center", "Center", 0.5, 0.5), num("radius", "Radius", 300, 1, 4000), ang("angle", "Angle", 90)}));
    r.push_back(fx("distort.lens", "Lens Distortion", "Distortion", {num("amount", "Amount", 30, -100, 100), num("zoom", "Zoom", 100, 10, 400)}));
    r.push_back(fx("distort.turbulence", "Turbulent Displace", "Distortion",
                   {num("amount", "Amount", 20, 0, 300), num("size", "Size", 80, 2, 1000), num("complexity", "Complexity", 3, 1, 6),
                    ang("evolution", "Evolution", 0), num("seed", "Random Seed", 1, 0, 10000)}, "", 2));
    r.push_back(fx("distort.cornerPin", "Corner Pin", "Distortion",
                   {pt("topLeft", "Top Left", 0, 0), pt("topRight", "Top Right", 1, 0), pt("bottomRight", "Bottom Right", 1, 1),
                    pt("bottomLeft", "Bottom Left", 0, 1)}));
    r.push_back(fx("distort.mirror", "Mirror", "Distortion", {pt("center", "Reflection Center", 0.5, 0.5), ang("angle", "Reflection Angle", 0)}));
    r.push_back(fx("distort.polar", "Polar Coordinates", "Distortion", {num("amount", "Interpolation", 100, 0, 100), enm("mode", "Type", {"rectToPolar", "polarToRect"})}));
    // Color
    r.push_back(fx("color.exposure", "Exposure", "Color", {num("exposure", "Exposure (stops)", 0, -5, 5), num("offset", "Offset", 0, -0.5, 0.5), num("gamma", "Gamma", 1, 0.1, 5)}));
    r.push_back(fx("color.brightnessContrast", "Brightness & Contrast", "Color", {num("brightness", "Brightness", 0, -100, 100), num("contrast", "Contrast", 0, -100, 100)}));
    r.push_back(fx("color.hueSaturation", "Hue / Saturation", "Color",
                   {ang("hue", "Hue", 0), num("saturation", "Saturation", 0, -100, 100), num("lightness", "Lightness", 0, -100, 100), bln("colorize", "Colorize", false)}));
    r.push_back(fx("color.vibrance", "Vibrance", "Color", {num("vibrance", "Vibrance", 30, -100, 100), num("saturation", "Saturation", 0, -100, 100)}));
    r.push_back(fx("color.temperatureTint", "Temperature & Tint", "Color", {num("temperature", "Temperature", 0, -100, 100), num("tint", "Tint", 0, -100, 100)}));
    r.push_back(fx("color.curves", "Curves", "Color", {curve("master", "RGB"), curve("red", "Red"), curve("green", "Green"), curve("blue", "Blue")}));
    r.push_back(fx("color.levels", "Levels", "Color",
                   {num("inBlack", "Input Black", 0, 0, 1), num("inWhite", "Input White", 1, 0, 1), num("gamma", "Gamma", 1, 0.1, 10),
                    num("outBlack", "Output Black", 0, 0, 1), num("outWhite", "Output White", 1, 0, 1)}));
    r.push_back(fx("color.channelMixer", "Channel Mixer", "Color",
                   {num("rr", "Red-Red", 100, -200, 200), num("rg", "Red-Green", 0, -200, 200), num("rb", "Red-Blue", 0, -200, 200),
                    num("gr", "Green-Red", 0, -200, 200), num("gg", "Green-Green", 100, -200, 200), num("gb", "Green-Blue", 0, -200, 200),
                    num("br", "Blue-Red", 0, -200, 200), num("bg", "Blue-Green", 0, -200, 200), num("bb", "Blue-Blue", 100, -200, 200),
                    bln("monochrome", "Monochrome", false)}));
    r.push_back(fx("color.selective", "Selective Color (HSL)", "Color",
                   {ang("targetHue", "Target Hue", 0), num("range", "Range", 30, 1, 180), ang("hueShift", "Hue Shift", 0),
                    num("saturation", "Saturation", 0, -100, 100), num("lightness", "Lightness", 0, -100, 100)}));
    r.push_back(fx("color.shadowsHighlights", "Shadows / Highlights", "Color",
                   {num("shadows", "Shadows", 0, -100, 100), num("highlights", "Highlights", 0, -100, 100), num("whites", "Whites", 0, -100, 100),
                    num("blacks", "Blacks", 0, -100, 100)}));
    r.push_back(fx("color.lut", "LUT (.cube/.3dl)", "Color", {asset("lut", "LUT File"), num("intensity", "Intensity", 100, 0, 100)}));
    r.push_back(fx("color.blackWhite", "Black & White", "Color", {num("amount", "Amount", 100, 0, 100)}));
    r.push_back(fx("color.tint", "Tint", "Color", {col("black", "Map Black To", Color(0, 0, 0)), col("white", "Map White To", Color(1, 1, 1)), num("amount", "Amount", 100, 0, 100)}));
    r.push_back(fx("color.invert", "Invert", "Color", {num("amount", "Amount", 100, 0, 100)}));
    r.push_back(fx("color.threshold", "Threshold", "Color", {num("level", "Level", 50, 0, 100)}));
    // Stylize
    r.push_back(fx("stylize.glow", "Glow", "Stylize",
                   {num("threshold", "Threshold", 60, 0, 100), num("radius", "Radius", 20, 0, 250), num("intensity", "Intensity", 100, 0, 500),
                    bln("useColor", "Use Color", false), col("color", "Color", Color(1, 1, 1))}, "Adds light around bright areas.", 3));
    r.push_back(fx("stylize.bloom", "Bloom", "Stylize", {num("threshold", "Threshold", 70, 0, 100), num("radius", "Radius", 40, 0, 250), num("intensity", "Intensity", 80, 0, 500)}, "", 4));
    r.push_back(fx("stylize.outline", "Outline (Stroke)", "Stylize",
                   {num("width", "Width", 4, 0, 60), col("color", "Color", Color(1, 1, 1)), num("opacity", "Opacity", 100, 0, 100),
                    enm("position", "Position", {"outside", "center", "inside"})}, "", 2));
    r.push_back(fx("stylize.posterize", "Posterize", "Stylize", {num("levels", "Levels", 6, 2, 64)}));
    r.push_back(fx("stylize.cartoon", "Cartoon", "Stylize", {num("levels", "Shading Levels", 6, 2, 32), num("edges", "Edge Strength", 60, 0, 100), num("smooth", "Smoothing", 3, 0, 20)}, "", 3));
    r.push_back(fx("stylize.sharpen", "Sharpen", "Stylize", {num("amount", "Amount", 50, 0, 500), num("radius", "Radius", 1.5, 0.5, 20)}, "", 2));
    r.push_back(fx("stylize.emboss", "Emboss", "Stylize", {ang("angle", "Direction", 45), num("height", "Relief", 2, 0, 20), num("amount", "Blend", 100, 0, 100)}));
    r.push_back(fx("stylize.edgeDetect", "Edge Detect", "Stylize", {num("strength", "Strength", 100, 0, 400), bln("invert", "Invert", false)}));
    r.push_back(fx("stylize.halftone", "Halftone", "Stylize", {num("dotSize", "Dot Size", 8, 2, 100), ang("angle", "Angle", 45), bln("color", "Color Dots", false)}));
    r.push_back(fx("stylize.pixelate", "Pixelate / Mosaic", "Stylize", {num("blockSize", "Block Size", 16, 1, 400)}));
    r.push_back(fx("stylize.dropShadow", "Drop Shadow", "Stylize",
                   {col("color", "Color", Color(0, 0, 0)), num("opacity", "Opacity", 60, 0, 100), ang("angle", "Direction", 135),
                    num("distance", "Distance", 15, 0, 1000), num("softness", "Softness", 10, 0, 250)}, "", 2));
    // Glitch
    r.push_back(fx("glitch.rgbSplit", "RGB Split", "Glitch", {num("amount", "Amount", 8, 0, 200), ang("angle", "Angle", 0)}));
    r.push_back(fx("glitch.scanlines", "Scanlines", "Glitch", {num("spacing", "Spacing", 4, 2, 100), num("intensity", "Intensity", 30, 0, 100), num("speed", "Scroll Speed", 0, -500, 500)}));
    r.push_back(fx("glitch.blockDisplace", "Block Displacement", "Glitch",
                   {num("amount", "Amount", 30, 0, 500), num("blockSize", "Block Size", 40, 4, 400), num("density", "Density", 30, 0, 100),
                    num("speed", "Speed (changes/s)", 10, 0, 60), num("seed", "Seed", 1, 0, 10000)}));
    r.push_back(fx("glitch.noise", "Noise / Grain", "Glitch", {num("amount", "Amount", 20, 0, 100), bln("monochrome", "Monochrome", true), bln("animated", "Animated", true)}));
    r.push_back(fx("glitch.chromatic", "Chromatic Aberration", "Glitch", {num("amount", "Amount", 5, 0, 100)}));
    r.push_back(fx("glitch.tear", "Frame Tearing", "Glitch", {num("amount", "Amount", 40, 0, 500), num("slices", "Slices", 12, 1, 100), num("speed", "Speed", 8, 0, 60), num("seed", "Seed", 1, 0, 10000)}));
    r.push_back(fx("glitch.compression", "Compression Artifacts", "Glitch", {num("blockSize", "Block Size", 8, 2, 64), num("quality", "Quality", 30, 1, 100)}));
    // Lighting
    r.push_back(fx("light.lensFlare", "Lens Flare", "Lighting", {pt("center", "Flare Center", 0.3, 0.3), num("intensity", "Brightness", 100, 0, 300), col("color", "Color", Color(1, 0.85f, 0.6f))}, "", 2));
    r.push_back(fx("light.rays", "Light Rays (Volumetric)", "Lighting",
                   {pt("center", "Source", 0.5, 0.3), num("length", "Ray Length", 40, 0, 100), num("intensity", "Intensity", 100, 0, 400), num("threshold", "Threshold", 50, 0, 100)}, "", 4));
    r.push_back(fx("light.vignette", "Vignette", "Lighting",
                   {num("amount", "Amount", 50, 0, 100), num("radius", "Radius", 75, 0, 200), num("softness", "Softness", 50, 1, 100), col("color", "Color", Color(0, 0, 0))}));
    r.push_back(fx("light.sheen", "Specular Sheen", "Lighting", {ang("angle", "Angle", 30), num("position", "Position", 50, -50, 150), num("width", "Width", 15, 1, 100), num("intensity", "Intensity", 60, 0, 200)}));
    // Keying
    r.push_back(fx("key.chroma", "Chroma Key", "Keying",
                   {col("keyColor", "Key Color", Color(0, 1, 0)), num("tolerance", "Tolerance", 30, 0, 100), num("softness", "Softness", 10, 0, 100),
                    num("spill", "Spill Suppression", 50, 0, 100), num("edgeFeather", "Edge Feather", 0, 0, 30), num("choke", "Edge Choke", 0, -20, 20),
                    enm("view", "View", {"composite", "alpha", "matte", "source"})}, "", 2));
    r.push_back(fx("key.luma", "Luma Key", "Keying", {num("threshold", "Threshold", 10, 0, 100), num("softness", "Softness", 10, 0, 100), enm("keyOut", "Key Out", {"dark", "bright"})}));
    r.push_back(fx("key.matteChoker", "Matte Choker", "Keying", {num("choke", "Choke", 0, -30, 30), num("feather", "Feather", 0, 0, 50)}));
    r.push_back(fx("key.channel", "Set Matte From Channel", "Keying", {enm("channel", "Channel", {"alpha", "red", "green", "blue", "luminance"}), bln("invert", "Invert", false)}));
    // Time
    {
        auto e = fx("time.echo", "Echo / Trails", "Time",
                    {num("count", "Number of Echoes", 4, 1, 30), num("interval", "Echo Time (s)", -0.05, -2, 2), num("decay", "Decay", 50, 0, 100),
                     enm("mode", "Operator", {"composite", "add", "max"})}, "", 4);
        e.timeEffect = true;
        r.push_back(e);
        auto p = fx("time.posterize", "Posterize Time", "Time", {num("fps", "Frame Rate", 8, 1, 60)});
        p.timeEffect = true;
        r.push_back(p);
    }
    // Utility / generate
    r.push_back(fx("util.transform", "Transform", "Utility",
                   {num("x", "Offset X", 0, -5000, 5000), num("y", "Offset Y", 0, -5000, 5000),
                    num("scale", "Scale", 100, 0, 1000), ang("rotation", "Rotation", 0), num("opacity", "Opacity", 100, 0, 100)}));
    r.push_back(fx("util.crop", "Crop", "Utility",
                   {num("left", "Left %", 0, 0, 100), num("top", "Top %", 0, 0, 100), num("right", "Right %", 0, 0, 100), num("bottom", "Bottom %", 0, 0, 100),
                    num("feather", "Edge Feather", 0, 0, 100)}));
    r.push_back(fx("util.fill", "Fill", "Utility", {col("color", "Color", Color(1, 0, 0)), num("opacity", "Opacity", 100, 0, 100)}));
    r.push_back(fx("util.opacity", "Opacity", "Utility", {num("opacity", "Opacity", 100, 0, 100)}));
    r.push_back(fx("util.colorSpace", "Color Space Convert", "Utility", {enm("mode", "Conversion", {"srgbToLinear", "linearToSrgb"})}));
    r.push_back(fx("generate.gradient", "Gradient Ramp", "Generate",
                   {pt("start", "Start", 0.5, 0), col("startColor", "Start Color", Color(0, 0, 0)), pt("end", "End", 0.5, 1),
                    col("endColor", "End Color", Color(1, 1, 1)), enm("shape", "Shape", {"linear", "radial"}), num("blend", "Blend With Original", 0, 0, 100)}));
    r.push_back(fx("generate.fractalNoise", "Fractal Noise", "Generate",
                   {num("scale", "Scale", 100, 5, 2000), num("complexity", "Complexity", 4, 1, 8), ang("evolution", "Evolution", 0),
                    num("contrast", "Contrast", 100, 0, 400), num("brightness", "Brightness", 0, -100, 100), num("opacity", "Opacity", 100, 0, 100),
                    num("seed", "Seed", 1, 0, 10000)}, "", 2));
    r.push_back(fx("generate.spectrum", "Audio Spectrum", "Generate",
                   {num("bands", "Bands", 32, 4, 128), num("height", "Max Height", 300, 10, 2000), pt("position", "Baseline Center", 0.5, 0.85),
                    num("width", "Width", 80, 5, 100), col("color", "Color", Color(1, 1, 1)), enm("style", "Style", {"bars", "line", "dots"})}));
    r.push_back(fx("generate.waveform", "Audio Waveform", "Generate",
                   {num("height", "Height", 150, 5, 2000), pt("position", "Center", 0.5, 0.5), num("width", "Width", 80, 5, 100),
                    col("color", "Color", Color(1, 1, 1)), num("thickness", "Thickness", 3, 1, 40), num("window", "Time Window (ms)", 60, 5, 1000)}));
    return r;
}

std::mutex g_compositeMutex;
std::map<std::string, json> g_composites;
}  // namespace

const std::vector<EffectInfo>& effectRegistry() {
    static const std::vector<EffectInfo> reg = buildRegistry();
    return reg;
}

const EffectInfo* findEffect(const std::string& type) {
    for (auto& e : effectRegistry())
        if (e.type == type) return &e;
    return nullptr;
}

json effectDefaultParams(const std::string& type) {
    json params = json::object();
    if (const EffectInfo* e = findEffect(type)) {
        for (auto& p : e->params) params[p.name] = json{{"v", p.def}};
    } else if (const json* c = findCompositeEffect(type)) {
        for (auto& p : jarr(*c, "params")) params[p.value("name", "")] = json{{"v", p.value("default", json(0))}};
    }
    return params;
}

std::vector<std::string> effectCategories() {
    std::vector<std::string> out;
    for (auto& e : effectRegistry())
        if (std::find(out.begin(), out.end(), e.category) == out.end()) out.push_back(e.category);
    return out;
}

void registerCompositeEffect(const std::string& type, const json& def) {
    std::lock_guard<std::mutex> lk(g_compositeMutex);
    g_composites[type] = def;
}
void clearCompositeEffects() {
    std::lock_guard<std::mutex> lk(g_compositeMutex);
    g_composites.clear();
}
const json* findCompositeEffect(const std::string& type) {
    std::lock_guard<std::mutex> lk(g_compositeMutex);
    auto it = g_composites.find(type);
    return it == g_composites.end() ? nullptr : &it->second;
}

std::map<std::string, json> compositeEffects() {
    std::lock_guard<std::mutex> lk(g_compositeMutex);
    return g_composites;
}

// ---------------------------------------------------------------- behaviours
const std::vector<BehaviorInfo>& behaviorRegistry() {
    static const std::vector<BehaviorInfo> reg = [] {
        std::vector<BehaviorInfo> r;
        auto b = [&](const char* t, const char* n, std::vector<ParamInfo> p, const char* help) {
            BehaviorInfo i; i.type = t; i.name = n; i.params = std::move(p); i.help = help; r.push_back(i);
        };
        auto common = [](std::vector<ParamInfo> p) {
            p.push_back(num("seed", "Seed", 1, 0, 100000));
            return p;
        };
        b("shake", "Shake", common({num("amplitude", "Amplitude", 20, 0, 1000), num("frequency", "Frequency", 8, 0.1, 60), num("decay", "Decay", 0, 0, 100),
                                    enm("axis", "Axis", {"both", "x", "y"}), num("randomness", "Randomness", 100, 0, 100), num("smoothing", "Smoothing", 50, 0, 100)}),
          "Random positional shake.");
        b("handheld", "Handheld Camera", common({num("amplitude", "Amplitude", 6, 0, 200), num("frequency", "Frequency", 1.5, 0.1, 10), num("rotation", "Rotation", 1.5, 0, 30)}),
          "Slow organic drift with slight rotation.");
        b("bounce", "Bounce", {num("height", "Height", 100, 0, 2000), num("frequency", "Bounces/sec", 2, 0.1, 20), num("decay", "Decay", 50, 0, 100)}, "Vertical bounce settling over time.");
        b("drift", "Drift", {num("vx", "Velocity X", 20, -2000, 2000), num("vy", "Velocity Y", 0, -2000, 2000)}, "Constant velocity motion.");
        b("float", "Float", common({num("amplitude", "Amplitude", 15, 0, 500), num("frequency", "Frequency", 0.5, 0.01, 10)}), "Gentle hovering.");
        b("orbit", "Orbit", {num("radius", "Radius", 100, 0, 5000), num("speed", "Revolutions/sec", 0.5, -10, 10), ang("phase", "Phase", 0)}, "Circular orbit around the base position.");
        b("wiggle", "Wiggle", common({num("amplitude", "Amplitude", 30, 0, 2000), num("frequency", "Frequency", 3, 0.1, 60),
                                      enm("target", "Property", {"position", "scale", "rotation", "opacity"})}), "Smooth random variation of a property.");
        b("jitter", "Jitter", common({num("amplitude", "Amplitude", 5, 0, 500), num("fps", "Steps/sec", 12, 1, 60)}), "Stepped random offsets.");
        b("swing", "Swing", {num("angle", "Angle", 15, 0, 180), num("frequency", "Frequency", 1, 0.05, 20), num("decay", "Decay", 20, 0, 100)}, "Pendulum rotation.");
        b("overshoot", "Overshoot Pop-in", {num("amount", "Overshoot", 25, 0, 200), num("duration", "Duration", 0.6, 0.05, 5)}, "Scale pops in past 100% and settles.");
        b("elastic", "Elastic Scale", {num("amount", "Amount", 30, 0, 200), num("frequency", "Frequency", 3, 0.5, 20), num("duration", "Duration", 1.2, 0.1, 10)}, "Springy scale entrance.");
        b("pulse", "Pulse", {num("amount", "Amount", 10, 0, 200), num("frequency", "Frequency", 2, 0.05, 30)}, "Rhythmic scale pulsing.");
        b("breathing", "Breathing", {num("amount", "Amount", 3, 0, 50), num("frequency", "Frequency", 0.25, 0.01, 5)}, "Slow subtle scale.");
        b("recoil", "Camera Recoil", {num("distance", "Distance", 40, 0, 1000), num("at", "Hit Time (s)", 0, 0, 3600), num("duration", "Duration", 0.35, 0.05, 5)}, "Sharp kick back then recovery.");
        b("impact", "Impact", common({num("amount", "Amount", 30, 0, 500), num("at", "Hit Time (s)", 0, 0, 3600), num("duration", "Duration", 0.4, 0.05, 5)}), "Scale punch plus decaying shake.");
        b("whip", "Whip", {num("distance", "Distance", 600, -5000, 5000), ang("angle", "Direction", 0), num("at", "Start (s)", 0, 0, 3600), num("duration", "Duration", 0.3, 0.05, 5)}, "Fast eased slide-in.");
        b("followThrough", "Follow-through", {num("amount", "Amount", 50, 0, 100), num("lag", "Lag (s)", 0.12, 0.01, 2)}, "Lags behind its own keyframed motion with a damped settle.");
        return r;
    }();
    return reg;
}

json behaviorDefaultParams(const std::string& type) {
    json p = json::object();
    for (auto& b : behaviorRegistry())
        if (b.type == type)
            for (auto& pi : b.params) p[pi.name] = json{{"v", pi.def}};
    return p;
}

// ---------------------------------------------------------------- transitions
const std::vector<TransitionInfo>& transitionRegistry() {
    static const std::vector<TransitionInfo> reg = [] {
        std::vector<TransitionInfo> r;
        auto t = [&](const char* type, const char* name, std::vector<ParamInfo> p) {
            TransitionInfo i; i.type = type; i.name = name; i.params = std::move(p); r.push_back(i);
        };
        t("fade", "Fade", {});
        t("dissolve", "Dither Dissolve", {num("grain", "Grain Size", 2, 1, 50)});
        t("fadeColor", "Dip to Color", {col("color", "Color", Color(0, 0, 0))});
        t("wipe", "Linear Wipe", {ang("angle", "Angle", 0), num("softness", "Softness", 10, 0, 100)});
        t("push", "Push", {enm("direction", "Direction", {"left", "right", "up", "down"})});
        t("slide", "Slide", {enm("direction", "Direction", {"left", "right", "up", "down"}), num("distance", "Distance %", 30, 0, 100)});
        t("zoom", "Zoom", {num("amount", "Zoom Amount %", 50, 0, 500), bln("zoomOut", "Zoom Out", false)});
        t("spin", "Spin", {ang("angle", "Rotation", 180), num("scale", "Scale %", 0, 0, 100)});
        t("blur", "Blur", {num("radius", "Max Blur", 40, 0, 250)});
        t("shapeReveal", "Shape Reveal", {enm("shape", "Shape", {"circle", "diamond", "rectangle"}), num("softness", "Softness", 5, 0, 100), pt("center", "Center", 0.5, 0.5)});
        t("displacement", "Displacement", {num("amount", "Amount", 80, 0, 500), num("scale", "Scale", 60, 2, 500)});
        t("glitch", "Glitch", {num("amount", "Amount", 60, 0, 300)});
        t("lightLeak", "Light Leak", {col("color", "Color", Color(1, 0.6f, 0.2f)), num("intensity", "Intensity", 100, 0, 300)});
        return r;
    }();
    return reg;
}

// ---------------------------------------------------------------- blend modes
BlendMode blendModeFromName(const std::string& s) {
    static const std::pair<const char*, BlendMode> table[] = {
        {"normal", BlendMode::Normal}, {"multiply", BlendMode::Multiply}, {"screen", BlendMode::Screen}, {"overlay", BlendMode::Overlay},
        {"softLight", BlendMode::SoftLight}, {"hardLight", BlendMode::HardLight}, {"darken", BlendMode::Darken}, {"lighten", BlendMode::Lighten},
        {"colorDodge", BlendMode::ColorDodge}, {"colorBurn", BlendMode::ColorBurn}, {"difference", BlendMode::Difference},
        {"exclusion", BlendMode::Exclusion}, {"add", BlendMode::Add}, {"subtract", BlendMode::Subtract}, {"hue", BlendMode::Hue},
        {"saturation", BlendMode::Saturation}, {"color", BlendMode::Color}, {"luminosity", BlendMode::Luminosity}};
    for (auto& p : table)
        if (s == p.first) return p.second;
    return BlendMode::Normal;
}

std::vector<std::string> blendModeNames() {
    return {"normal", "multiply", "screen", "overlay", "softLight", "hardLight", "darken", "lighten", "colorDodge", "colorBurn",
            "difference", "exclusion", "add", "subtract", "hue", "saturation", "color", "luminosity"};
}

}  // namespace mf
