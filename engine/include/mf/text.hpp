// Fonts (TTF/OTF via stb_truetype), text layout and per-character animator evaluation.
#pragma once
#include <map>
#include <mutex>

#include "property.hpp"
#include "raster.hpp"

struct stbtt_fontinfo;

namespace mf {

std::u32string utf8ToU32(const std::string& s);
std::string u32ToUtf8(const std::u32string& s);

class Font {
   public:
    Font();
    ~Font();
    bool loadFile(const std::string& path, int index = 0);
    bool loadMemory(std::vector<uint8_t> data, int index = 0);
    // Glyph outline scaled to `size` px (em), origin at baseline, y down.
    BezierPath glyphPath(uint32_t cp, double size, std::vector<BezierPath>& contours) const;
    void glyphContours(uint32_t cp, double size, std::vector<BezierPath>& contours) const;
    double advance(uint32_t cp, double size) const;
    double kern(uint32_t a, uint32_t b, double size) const;
    double ascent(double size) const;
    double descent(double size) const;  // positive value below baseline
    double lineGap(double size) const;
    bool hasGlyph(uint32_t cp) const;
    std::string family, style, path;
    int numFaces = 1;
    bool variable = false;
    std::vector<std::string> variationAxes;

   private:
    std::vector<uint8_t> data_;
    std::unique_ptr<stbtt_fontinfo> info_;
    float unitScale_ = 1;  // 1/unitsPerEm
    mutable std::mutex mutex_;
};

class FontManager {
   public:
    static FontManager& instance();
    // Register a font under a name (defaults to PostScript-ish "Family-Style" from the file name).
    bool registerFile(const std::string& path, const std::string& name = std::string(), std::string* err = nullptr);
    int registerDirectory(const std::string& dir);
    bool unregister(const std::string& name);
    std::shared_ptr<Font> get(const std::string& name) const;  // falls back to default font
    bool has(const std::string& name) const;
    std::vector<std::string> names() const;
    json listJson() const;
    void setDefault(const std::string& name) { defaultName_ = name; }
    std::string defaultName() const { return defaultName_; }

   private:
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Font>> fonts_;
    std::string defaultName_ = "DejaVuSans";
};

struct GlyphInfo {
    uint32_t cp = 0;
    Vec2 origin;     // baseline origin in layer space
    double advance = 0;
    double width = 0;  // ink-ish width (advance) for centering transforms
    int charIndex = 0, wordIndex = 0, lineIndex = 0;
    bool whitespace = false;
};

struct TextLayout {
    std::vector<GlyphInfo> glyphs;
    int charCount = 0, wordCount = 0, lineCount = 0;
    double width = 0, height = 0;  // block size
    double top = 0;                // y of first line ascent relative to layer origin
    std::vector<double> lineWidths;
    std::vector<double> lineY;     // baseline per line
};

struct TextStyle {
    double size = 72;
    double tracking = 0;   // in 1/1000 em
    double leading = 1.2;  // line height multiplier
    double baselineShift = 0;
    std::string align = "center";  // left|center|right
    double boxWidth = 0;            // wrap width in px, 0 = no wrap
};

TextLayout layoutText(const std::u32string& text, const Font& font, const TextStyle& style);

// Per-glyph animator result (applied relative to glyph center).
struct GlyphAnim {
    Vec2 offset;
    double scale = 1.0;
    double rotation = 0;   // degrees
    double opacity = 1.0;  // multiplier
    bool hasFill = false;
    Color fill;
    double fillAmount = 0;
    double tracking = 0;   // extra px
    double blur = 0;
    bool scramble = false;
    double clipAmount = 0;  // for masked reveal: fraction of offset that is clipped
};

// Evaluate all text animators of a text layer for each glyph.
std::vector<GlyphAnim> evalTextAnimators(const json& textData, const TextLayout& layout, const EvalContext& ctx);

}  // namespace mf
