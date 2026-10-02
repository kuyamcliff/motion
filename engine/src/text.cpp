#include "mf/text.hpp"

#include <dirent.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "../../third_party/stb/stb_truetype.h"

namespace mf {

std::u32string utf8ToU32(const std::string& s) {
    std::u32string out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp;
        int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; len = 3; }
        else if ((c >> 3) == 30) { cp = c & 0x07; len = 4; }
        else { ++i; out.push_back(0xFFFD); continue; }
        if (i + len > s.size()) { out.push_back(0xFFFD); break; }
        for (int k = 1; k < len; ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        out.push_back(cp);
        i += len;
    }
    return out;
}

std::string u32ToUtf8(const std::u32string& s) {
    std::string out;
    for (char32_t c : s) {
        uint32_t cp = c;
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    }
    return out;
}

// ====================================================================== Font
Font::Font() : info_(new stbtt_fontinfo()) {}
Font::~Font() = default;

static std::string nameString(const stbtt_fontinfo* info, int nameId) {
    int len = 0;
    // Try Windows Unicode English first, then Mac Roman.
    const char* s = stbtt_GetFontNameString(info, &len, STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP, STBTT_MS_LANG_ENGLISH, nameId);
    if (s && len > 0) {
        std::string out;
        for (int i = 0; i + 1 < len; i += 2) {
            uint16_t c = ((uint8_t)s[i] << 8) | (uint8_t)s[i + 1];
            out += c < 0x80 ? (char)c : '?';
        }
        return out;
    }
    s = stbtt_GetFontNameString(info, &len, STBTT_PLATFORM_ID_MAC, STBTT_MAC_EID_ROMAN, STBTT_MAC_LANG_ENGLISH, nameId);
    if (s && len > 0) return std::string(s, len);
    return {};
}

bool Font::loadMemory(std::vector<uint8_t> data, int index) {
    data_ = std::move(data);
    if (data_.size() < 12) return false;
    numFaces = std::max(1, stbtt_GetNumberOfFonts(data_.data()));
    int off = stbtt_GetFontOffsetForIndex(data_.data(), index);
    if (off < 0) return false;
    if (!stbtt_InitFont(info_.get(), data_.data(), off)) return false;
    int unitsPerEm = 0;
    // head table unitsPerEm at offset 18.
    stbtt_uint32 head = 0;
    {
        const uint8_t* d = data_.data() + off;
        int numTables = (d[4] << 8) | d[5];
        for (int i = 0; i < numTables; ++i) {
            const uint8_t* rec = d + 12 + 16 * i;
            if (rec[0] == 'h' && rec[1] == 'e' && rec[2] == 'a' && rec[3] == 'd') head = (rec[8] << 24) | (rec[9] << 16) | (rec[10] << 8) | rec[11];
            if (rec[0] == 'f' && rec[1] == 'v' && rec[2] == 'a' && rec[3] == 'r') variable = true;
        }
        if (head && head + 20 < data_.size()) unitsPerEm = (data_[head + 18] << 8) | data_[head + 19];
    }
    if (unitsPerEm <= 0) unitsPerEm = 2048;
    unitScale_ = 1.f / unitsPerEm;
    family = nameString(info_.get(), 1);
    style = nameString(info_.get(), 2);
    if (variable) variationAxes = {"wght"};  // axes are reported; static instance rendered
    return true;
}

bool Font::loadFile(const std::string& p, int index) {
    bool ok;
    auto bytes = readFileBytes(p, &ok);
    if (!ok) return false;
    path = p;
    return loadMemory(std::move(bytes), index);
}

void Font::glyphContours(uint32_t cp, double size, std::vector<BezierPath>& contours) const {
    std::lock_guard<std::mutex> lk(mutex_);
    contours.clear();
    int g = stbtt_FindGlyphIndex(info_.get(), (int)cp);
    if (g == 0 && cp != 0) return;
    stbtt_vertex* v = nullptr;
    int n = stbtt_GetGlyphShape(info_.get(), g, &v);
    double s = size * unitScale_;
    BezierPath cur;
    bool open = false;
    Vec2 last;
    auto finish = [&]() {
        if (open && cur.v.size() > 1) {
            // Remove duplicated closing vertex.
            if ((cur.v.back().p - cur.v.front().p).length() < 1e-9) {
                cur.v.front().in = cur.v.back().in;
                cur.v.pop_back();
            }
            cur.closed = true;
            contours.push_back(cur);
        }
        cur = BezierPath();
        open = false;
    };
    for (int i = 0; i < n; ++i) {
        Vec2 p{v[i].x * s, -v[i].y * s};
        switch (v[i].type) {
            case STBTT_vmove:
                finish();
                cur.v.push_back({p, {}, {}});
                open = true;
                break;
            case STBTT_vline:
                cur.v.push_back({p, {}, {}});
                break;
            case STBTT_vcurve: {
                Vec2 c{v[i].cx * s, -v[i].cy * s};
                // Quadratic -> cubic handles.
                Vec2 c1 = last + (c - last) * (2.0 / 3.0), c2 = p + (c - p) * (2.0 / 3.0);
                if (!cur.v.empty()) cur.v.back().out = c1 - cur.v.back().p;
                cur.v.push_back({p, c2 - p, {}});
                break;
            }
            case STBTT_vcubic: {
                Vec2 c1{v[i].cx * s, -v[i].cy * s}, c2{v[i].cx1 * s, -v[i].cy1 * s};
                if (!cur.v.empty()) cur.v.back().out = c1 - cur.v.back().p;
                cur.v.push_back({p, c2 - p, {}});
                break;
            }
        }
        last = p;
    }
    finish();
    if (v) stbtt_FreeShape(info_.get(), v);
}

BezierPath Font::glyphPath(uint32_t cp, double size, std::vector<BezierPath>& contours) const {
    glyphContours(cp, size, contours);
    return contours.empty() ? BezierPath() : contours[0];
}

double Font::advance(uint32_t cp, double size) const {
    std::lock_guard<std::mutex> lk(mutex_);
    int adv, lsb;
    stbtt_GetCodepointHMetrics(info_.get(), (int)cp, &adv, &lsb);
    return adv * size * unitScale_;
}
double Font::kern(uint32_t a, uint32_t b, double size) const {
    std::lock_guard<std::mutex> lk(mutex_);
    return stbtt_GetCodepointKernAdvance(info_.get(), (int)a, (int)b) * size * unitScale_;
}
double Font::ascent(double size) const {
    int a, d, g;
    stbtt_GetFontVMetrics(info_.get(), &a, &d, &g);
    return a * size * unitScale_;
}
double Font::descent(double size) const {
    int a, d, g;
    stbtt_GetFontVMetrics(info_.get(), &a, &d, &g);
    return -d * size * unitScale_;
}
double Font::lineGap(double size) const {
    int a, d, g;
    stbtt_GetFontVMetrics(info_.get(), &a, &d, &g);
    return g * size * unitScale_;
}
bool Font::hasGlyph(uint32_t cp) const { return stbtt_FindGlyphIndex(info_.get(), (int)cp) != 0; }

// ====================================================================== FontManager
FontManager& FontManager::instance() {
    static FontManager m;
    return m;
}

bool FontManager::registerFile(const std::string& path, const std::string& name, std::string* err) {
    auto f = std::make_shared<Font>();
    if (!f->loadFile(path)) {
        if (err) *err = "Not a readable TrueType/OpenType font: " + pathBasename(path);
        return false;
    }
    std::string n = name;
    if (n.empty()) {
        n = pathBasename(path);
        auto dot = n.find_last_of('.');
        if (dot != std::string::npos) n = n.substr(0, dot);
    }
    std::lock_guard<std::mutex> lk(mutex_);
    fonts_[n] = f;
    return true;
}

int FontManager::registerDirectory(const std::string& dir) {
    int count = 0;
    DIR* d = opendir(dir.c_str());
    if (!d) return 0;
    while (dirent* e = readdir(d)) {
        std::string fn = e->d_name;
        std::string ext = pathExtensionLower(fn);
        if (ext == "ttf" || ext == "otf" || ext == "ttc") {
            if (registerFile(pathJoin(dir, fn))) ++count;
        }
    }
    closedir(d);
    return count;
}

bool FontManager::unregister(const std::string& name) {
    std::lock_guard<std::mutex> lk(mutex_);
    return fonts_.erase(name) > 0;
}

std::shared_ptr<Font> FontManager::get(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = fonts_.find(name);
    if (it != fonts_.end()) return it->second;
    it = fonts_.find(defaultName_);
    if (it != fonts_.end()) return it->second;
    return fonts_.empty() ? nullptr : fonts_.begin()->second;
}

bool FontManager::has(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mutex_);
    return fonts_.count(name) > 0;
}

std::vector<std::string> FontManager::names() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<std::string> out;
    for (auto& [k, v] : fonts_) out.push_back(k);
    return out;
}

json FontManager::listJson() const {
    std::lock_guard<std::mutex> lk(mutex_);
    json out = json::array();
    for (auto& [k, f] : fonts_)
        out.push_back({{"name", k}, {"family", f->family}, {"style", f->style}, {"path", f->path}, {"variable", f->variable}, {"faces", f->numFaces}});
    return out;
}

// ====================================================================== layout
TextLayout layoutText(const std::u32string& text, const Font& font, const TextStyle& st) {
    TextLayout L;
    double size = st.size;
    double track = st.tracking / 1000.0 * size;
    double lineH = size * st.leading;
    // Split into words for wrapping.
    struct Line { std::vector<GlyphInfo> g; double width = 0; };
    std::vector<Line> lines(1);
    int charIdx = 0, wordIdx = 0;
    bool inWord = false;
    double x = 0;
    size_t lastSpace = std::string::npos;  // index in current line glyphs
    for (size_t i = 0; i < text.size(); ++i) {
        uint32_t cp = text[i];
        if (cp == '\r') continue;
        if (cp == '\n') {
            lines.back().width = x;
            lines.push_back(Line());
            x = 0;
            lastSpace = std::string::npos;
            if (inWord) { ++wordIdx; inWord = false; }
            continue;
        }
        bool ws = cp == ' ' || cp == '\t';
        if (ws && inWord) { ++wordIdx; inWord = false; }
        if (!ws) inWord = true;
        GlyphInfo g;
        g.cp = cp == '\t' ? ' ' : cp;
        g.whitespace = ws;
        g.charIndex = charIdx++;
        g.wordIndex = wordIdx;
        double adv = font.advance(g.cp, size);
        if (!lines.back().g.empty()) x += font.kern(lines.back().g.back().cp, g.cp, size);
        g.origin = {x, 0};
        g.advance = adv;
        g.width = adv;
        x += adv + track;
        if (ws) lastSpace = lines.back().g.size();
        lines.back().g.push_back(g);
        if (st.boxWidth > 0 && x > st.boxWidth && !ws && lastSpace != std::string::npos && lastSpace + 1 < lines.back().g.size()) {
            // Move glyphs after last space to a new line.
            Line nl;
            auto& cur = lines.back();
            double shift = cur.g[lastSpace + 1].origin.x;
            for (size_t k = lastSpace + 1; k < cur.g.size(); ++k) {
                GlyphInfo gg = cur.g[k];
                gg.origin.x -= shift;
                nl.g.push_back(gg);
            }
            cur.g.resize(lastSpace);  // drop trailing space
            cur.width = cur.g.empty() ? 0 : cur.g.back().origin.x + cur.g.back().advance;
            x -= shift;
            lines.push_back(nl);
            lastSpace = std::string::npos;
        }
    }
    lines.back().width = x - (lines.back().g.empty() ? 0 : track);
    for (auto& ln : lines) {
        // Trim trailing whitespace from width.
        double w = 0;
        for (auto& g : ln.g)
            if (!g.whitespace) w = std::max(w, g.origin.x + g.advance);
        ln.width = w;
    }
    L.lineCount = (int)lines.size();
    double asc = font.ascent(size);
    double totalH = asc + font.descent(size) + (lines.size() - 1) * lineH;
    // Layer origin is the center of the text block (vertical) and alignment anchor (horizontal).
    double y0 = -totalH / 2 + asc;
    L.top = -totalH / 2;
    L.height = totalH;
    for (size_t li = 0; li < lines.size(); ++li) {
        auto& ln = lines[li];
        double offx = st.align == "left" ? 0 : st.align == "right" ? -ln.width : -ln.width / 2;
        double y = y0 + li * lineH - st.baselineShift;
        L.lineWidths.push_back(ln.width);
        L.lineY.push_back(y);
        L.width = std::max(L.width, ln.width);
        for (auto& g : ln.g) {
            g.origin.x += offx;
            g.origin.y = y;
            g.lineIndex = (int)li;
            L.glyphs.push_back(g);
        }
    }
    L.charCount = charIdx;
    L.wordCount = wordIdx + (inWord ? 1 : 0);
    return L;
}

// ====================================================================== animators
static double selectorShape(const std::string& shape, double u) {
    // u = position of unit within selection range (0..1); returns amount 0..1.
    if (u < 0 || u > 1) return 0;
    if (shape == "rampUp") return u;
    if (shape == "rampDown") return 1 - u;
    if (shape == "triangle") return 1 - std::fabs(u * 2 - 1);
    if (shape == "round") return std::sqrt(std::max(0.0, 1 - (u * 2 - 1) * (u * 2 - 1)));
    if (shape == "smooth") { double t = 1 - std::fabs(u * 2 - 1); return t * t * (3 - 2 * t); }
    return 1;  // square
}

std::vector<GlyphAnim> evalTextAnimators(const json& td, const TextLayout& layout, const EvalContext& ctx) {
    std::vector<GlyphAnim> out(layout.glyphs.size());
    auto it = td.find("animators");
    if (it == td.end() || !it->is_array() || it->empty()) return out;
    for (auto& a : *it) {
        if (!a.value("enabled", true)) continue;
        const json& sel = a.value("selector", json::object());
        const json& pr = a.value("props", json::object());
        double s0 = propNumber(sel, "start", ctx, 0) / 100.0;
        double s1 = propNumber(sel, "end", ctx, 100) / 100.0;
        double off = propNumber(sel, "offset", ctx, 0) / 100.0;
        std::string unit = sel.value("unit", std::string("char"));
        std::string shape = sel.value("shape", std::string("square"));
        int count = unit == "word" ? std::max(1, layout.wordCount) : unit == "line" ? std::max(1, layout.lineCount) : std::max(1, layout.charCount);
        Vec2 pos = propVec2(pr, "position", ctx, {0, 0});
        bool hasPos = pr.contains("position");
        double scl = propNumber(pr, "scale", ctx, 100) / 100.0;
        bool hasScale = pr.contains("scale");
        double rot = propNumber(pr, "rotation", ctx, 0);
        double opa = propNumber(pr, "opacity", ctx, 100) / 100.0;
        bool hasOpa = pr.contains("opacity");
        bool hasFill = pr.contains("fill");
        Color fill = propColor(pr, "fill", ctx, Color(1, 1, 1));
        double trk = propNumber(pr, "tracking", ctx, 0);
        double blur = propNumber(pr, "blur", ctx, 0);
        bool scramble = pr.value("scramble", false);
        bool clip = pr.value("clip", false);
        double a0 = std::min(s0, s1) + off, a1 = std::max(s0, s1) + off;
        for (size_t i = 0; i < layout.glyphs.size(); ++i) {
            const GlyphInfo& g = layout.glyphs[i];
            int idx = unit == "word" ? g.wordIndex : unit == "line" ? g.lineIndex : g.charIndex;
            // Unit occupies [idx/count, (idx+1)/count]; amount = coverage by selection range shaped.
            double c0 = (double)idx / count, c1 = (double)(idx + 1) / count;
            double amount;
            if (shape == "square") {
                double ov = std::max(0.0, std::min(c1, a1) - std::max(c0, a0));
                amount = ov / (c1 - c0);
            } else {
                double center = (c0 + c1) / 2;
                double span = a1 - a0;
                if (span <= 1e-9) amount = 0;
                else {
                    // rampUp: units left of the window are unaffected (0), right of it fully affected (1).
                    double u = (center - a0) / span;
                    if (shape == "rampUp") amount = u < 0 ? 0 : (u > 1 ? 1 : u);
                    else if (shape == "rampDown") amount = u < 0 ? 1 : (u > 1 ? 0 : 1 - u);
                    else amount = selectorShape(shape, u);
                }
            }
            if (amount <= 1e-6) continue;
            GlyphAnim& ga = out[i];
            if (hasPos) ga.offset += pos * amount;
            if (hasScale) ga.scale *= 1 + (scl - 1) * amount;
            ga.rotation += rot * amount;
            if (hasOpa) ga.opacity *= 1 + (opa - 1) * amount;
            if (hasFill) { ga.hasFill = true; ga.fill = fill; ga.fillAmount = std::max(ga.fillAmount, amount); }
            ga.tracking += trk * amount;
            ga.blur += blur * amount;
            if (scramble && amount > 0.5) ga.scramble = true;
            if (clip) ga.clipAmount = 1;
        }
    }
    return out;
}

}  // namespace mf
