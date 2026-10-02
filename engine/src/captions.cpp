#include "mf/captions.hpp"

#include <chrono>
#include <sstream>

#ifdef MF_WITH_WHISPER
#include "whisper.h"
#endif

namespace mf {

json captionsToJson(const std::vector<CaptionItem>& items) {
    json out = json::array();
    for (auto& c : items) {
        json j = {{"start", c.start}, {"end", c.end}, {"text", c.text}, {"speaker", c.speaker}, {"conf", c.conf}};
        if (!c.words.empty()) {
            json w = json::array();
            for (auto& x : c.words) w.push_back({{"s", x.s}, {"e", x.e}, {"w", x.w}, {"c", x.conf}});
            j["words"] = w;
        }
        out.push_back(j);
    }
    return out;
}

std::vector<CaptionItem> captionsFromJson(const json& items) {
    std::vector<CaptionItem> out;
    if (!items.is_array()) return out;
    for (auto& j : items) {
        CaptionItem c;
        c.start = j.value("start", 0.0);
        c.end = j.value("end", 0.0);
        c.text = j.value("text", std::string());
        c.speaker = j.value("speaker", 0);
        c.conf = j.value("conf", 1.0f);
        for (auto& w : jarr(j, "words")) c.words.push_back({w.value("s", 0.0), w.value("e", 0.0), w.value("w", std::string()), w.value("c", 1.0f)});
        out.push_back(c);
    }
    return out;
}

// ====================================================================== formats
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static bool parseTime(const std::string& s, double& t) {
    // hh:mm:ss,mmm | hh:mm:ss.mmm | mm:ss.mmm | h:mm:ss.cc (ASS)
    int h = 0, m = 0;
    double sec = 0;
    std::string x = s;
    for (auto& c : x)
        if (c == ',') c = '.';
    int colons = (int)std::count(x.begin(), x.end(), ':');
    if (colons == 2) {
        if (std::sscanf(x.c_str(), "%d:%d:%lf", &h, &m, &sec) != 3) return false;
    } else if (colons == 1) {
        if (std::sscanf(x.c_str(), "%d:%lf", &m, &sec) != 2) return false;
    } else return false;
    t = h * 3600.0 + m * 60.0 + sec;
    return true;
}

static std::string stripTags(const std::string& s) {
    std::string out;
    bool tag = false, brace = false;
    for (char c : s) {
        if (c == '<') { tag = true; continue; }
        if (c == '>') { tag = false; continue; }
        if (c == '{') { brace = true; continue; }
        if (c == '}') { brace = false; continue; }
        if (!tag && !brace) out += c;
    }
    return out;
}

static std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string l;
    while (std::getline(in, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        out.push_back(l);
    }
    return out;
}

static std::vector<CaptionItem> parseCueBlocks(const std::string& text, bool vtt, std::string* err) {
    std::vector<CaptionItem> out;
    auto ls = lines(text);
    size_t i = 0;
    if (vtt) {
        if (ls.empty() || trim(ls[0]).rfind("WEBVTT", 0) != 0) { if (err) *err = "Missing WEBVTT header."; return out; }
        ++i;
    }
    while (i < ls.size()) {
        std::string l = trim(ls[i]);
        size_t arrow = l.find("-->");
        if (arrow == std::string::npos) {
            if (vtt && (l.rfind("NOTE", 0) == 0 || l.rfind("STYLE", 0) == 0 || l.rfind("REGION", 0) == 0)) {
                while (i < ls.size() && !trim(ls[i]).empty()) ++i;
            }
            ++i;
            continue;
        }
        std::string a = trim(l.substr(0, arrow)), b = trim(l.substr(arrow + 3));
        auto sp = b.find(' ');
        if (sp != std::string::npos) b = b.substr(0, sp);  // VTT cue settings
        CaptionItem c;
        if (!parseTime(a, c.start) || !parseTime(b, c.end)) { ++i; continue; }
        ++i;
        std::string body;
        while (i < ls.size() && !trim(ls[i]).empty()) {
            if (!body.empty()) body += "\n";
            body += stripTags(ls[i]);
            ++i;
        }
        c.text = trim(body);
        if (c.end > c.start && !c.text.empty()) out.push_back(c);
    }
    if (out.empty() && err && err->empty()) *err = "No subtitle cues found.";
    return out;
}

std::vector<CaptionItem> parseSrt(const std::string& text, std::string* err) { return parseCueBlocks(text, false, err); }
std::vector<CaptionItem> parseVtt(const std::string& text, std::string* err) { return parseCueBlocks(text, true, err); }

std::vector<CaptionItem> parseAss(const std::string& text, std::string* err) {
    std::vector<CaptionItem> out;
    std::vector<std::string> format;
    for (auto& raw : lines(text)) {
        std::string l = trim(raw);
        if (l.rfind("Format:", 0) == 0 && format.empty()) {
            // Only take the Events format line (comes after [Events]).
        }
        if (l.rfind("[Events]", 0) == 0) { format.clear(); continue; }
        if (l.rfind("Format:", 0) == 0) {
            format.clear();
            std::stringstream ss(l.substr(7));
            std::string f;
            while (std::getline(ss, f, ',')) format.push_back(trim(f));
            continue;
        }
        if (l.rfind("Dialogue:", 0) == 0 && !format.empty()) {
            std::string rest = l.substr(9);
            std::vector<std::string> fields;
            size_t pos = 0;
            for (size_t k = 0; k + 1 < format.size(); ++k) {
                size_t comma = rest.find(',', pos);
                if (comma == std::string::npos) break;
                fields.push_back(trim(rest.substr(pos, comma - pos)));
                pos = comma + 1;
            }
            fields.push_back(rest.substr(pos));
            CaptionItem c;
            for (size_t k = 0; k < format.size() && k < fields.size(); ++k) {
                if (format[k] == "Start") parseTime(fields[k], c.start);
                else if (format[k] == "End") parseTime(fields[k], c.end);
                else if (format[k] == "Text") {
                    std::string t = stripTags(fields[k]);
                    size_t p;
                    while ((p = t.find("\\N")) != std::string::npos) t.replace(p, 2, "\n");
                    while ((p = t.find("\\n")) != std::string::npos) t.replace(p, 2, "\n");
                    c.text = trim(t);
                }
            }
            if (c.end > c.start && !c.text.empty()) out.push_back(c);
        }
    }
    if (out.empty() && err) *err = "No Dialogue events found in ASS/SSA file.";
    return out;
}

std::vector<CaptionItem> parseSubtitles(const std::string& text, const std::string& ext, std::string* err) {
    if (ext == "vtt") return parseVtt(text, err);
    if (ext == "ass" || ext == "ssa") return parseAss(text, err);
    if (ext == "srt") return parseSrt(text, err);
    // Sniff.
    if (trim(text).rfind("WEBVTT", 0) == 0) return parseVtt(text, err);
    if (text.find("[Events]") != std::string::npos) return parseAss(text, err);
    return parseSrt(text, err);
}

std::string formatTimestamp(double t, char sep) {
    if (t < 0) t = 0;
    long ms = std::lround(t * 1000);
    int h = (int)(ms / 3600000), m = (int)(ms / 60000 % 60), s = (int)(ms / 1000 % 60), x = (int)(ms % 1000);
    return formatString("%02d:%02d:%02d%c%03d", h, m, s, sep, x);
}

std::string toSrt(const std::vector<CaptionItem>& items) {
    std::string out;
    int n = 1;
    for (auto& c : items) {
        out += std::to_string(n++) + "\n" + formatTimestamp(c.start, ',') + " --> " + formatTimestamp(c.end, ',') + "\n" + c.text + "\n\n";
    }
    return out;
}

std::string toVtt(const std::vector<CaptionItem>& items) {
    std::string out = "WEBVTT\n\n";
    for (auto& c : items) out += formatTimestamp(c.start, '.') + " --> " + formatTimestamp(c.end, '.') + "\n" + c.text + "\n\n";
    return out;
}

std::string toAss(const std::vector<CaptionItem>& items, const json& style, int w, int h) {
    auto assColor = [](const json& c) {
        Color col = parseColor(c, Color(1, 1, 1, 1));
        int a = 255 - (int)std::lround(col.a * 255), r = (int)std::lround(col.r * 255), g = (int)std::lround(col.g * 255), b = (int)std::lround(col.b * 255);
        return formatString("&H%02X%02X%02X%02X", a, b, g, r);
    };
    std::string font = style.value("font", std::string("DejaVu Sans"));
    if (font.find("DejaVuSans") == 0) font = "DejaVu Sans";
    int size = (int)style.value("size", 64.0);
    std::string out = "[Script Info]\nScriptType: v4.00+\nPlayResX: " + std::to_string(w) + "\nPlayResY: " + std::to_string(h) + "\nScaledBorderAndShadow: yes\n\n";
    out += "[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n";
    int marginV = (int)std::lround((1.0 - style.value("position", json({0.5, 0.86}))[1].get<double>()) * h);
    out += formatString("Style: Default,%s,%d,%s,%s,%s,%s,1,0,0,0,100,100,0,0,%d,%d,%d,2,40,40,%d,1\n\n", font.c_str(), size,
                        assColor(style.value("color", json({1, 1, 1, 1}))).c_str(), assColor(style.value("highlightColor", json({1, 0.85, 0.1, 1}))).c_str(),
                        assColor(style.value("strokeColor", json({0, 0, 0, 1}))).c_str(), assColor(style.value("backgroundColor", json({0, 0, 0, 0.6}))).c_str(),
                        style.value("background", false) ? 3 : 1, (int)style.value("strokeWidth", 4.0) / 2, style.value("shadow", true) ? 2 : 0, std::max(0, marginV));
    out += "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
    auto assTime = [](double t) {
        long cs = std::lround(std::max(0.0, t) * 100);
        return formatString("%d:%02d:%02d.%02d", (int)(cs / 360000), (int)(cs / 6000 % 60), (int)(cs / 100 % 60), (int)(cs % 100));
    };
    bool karaoke = style.value("highlightActiveWord", true);
    for (auto& c : items) {
        std::string text;
        if (karaoke && !c.words.empty()) {
            for (auto& wd : c.words) {
                int k = (int)std::lround((wd.e - wd.s) * 100);
                text += "{\\k" + std::to_string(std::max(1, k)) + "}" + wd.w + " ";
            }
        } else {
            text = c.text;
        }
        size_t p;
        while ((p = text.find('\n')) != std::string::npos) text.replace(p, 1, "\\N");
        out += "Dialogue: 0," + assTime(c.start) + "," + assTime(c.end) + ",Default,,0,0,0,," + trim(text) + "\n";
    }
    return out;
}

// ====================================================================== segmentation
static bool endsSentence(const std::string& w) {
    if (w.empty()) return false;
    char c = w.back();
    return c == '.' || c == '?' || c == '!';
}
static bool endsClause(const std::string& w) {
    if (w.empty()) return false;
    char c = w.back();
    return c == ',' || c == ';' || c == ':';
}

static std::string joinWordList(const std::vector<CaptionWord>& ws, size_t a, size_t b) {
    std::string s;
    for (size_t i = a; i < b; ++i) {
        std::string t = trim(ws[i].w);
        if (t.empty()) continue;
        bool attach = !s.empty() && (t[0] == ',' || t[0] == '.' || t[0] == '?' || t[0] == '!' || t[0] == '\'' || t == "n't");
        if (!s.empty() && !attach) s += " ";
        s += t;
    }
    return s;
}

// Insert a line break near the middle at a word boundary when text exceeds maxChars.
static std::string balanceLines(const std::string& text, int maxChars) {
    if ((int)text.size() <= maxChars) return text;
    size_t mid = text.size() / 2;
    size_t best = std::string::npos;
    for (size_t d = 0; d < mid; ++d) {
        if (mid + d < text.size() && text[mid + d] == ' ') { best = mid + d; break; }
        if (mid >= d && text[mid - d] == ' ') { best = mid - d; break; }
    }
    if (best == std::string::npos) return text;
    return text.substr(0, best) + "\n" + text.substr(best + 1);
}

std::vector<CaptionItem> segmentWords(const std::vector<CaptionWord>& wordsIn, const SegmentOptions& opt) {
    std::vector<CaptionWord> words;
    for (auto& w : wordsIn)
        if (!trim(w.w).empty()) words.push_back({w.s, w.e, trim(w.w), w.conf});
    std::vector<CaptionItem> out;
    size_t i = 0, n = words.size();
    int maxTotal = opt.maxChars * opt.maxLines;
    while (i < n) {
        // Grow greedily until a hard limit (characters / duration) or a long pause.
        size_t j = i;
        int chars = 0;
        bool pauseBreak = false;
        // Candidate break positions (index after word) with a quality score.
        std::vector<std::pair<size_t, double>> cands;
        while (j < n) {
            int add = (int)words[j].w.size() + (j > i ? 1 : 0);
            double dur = words[j].e - words[i].s;
            if (j > i && (chars + add > maxTotal || dur > opt.maxDuration)) break;
            chars += add;
            ++j;
            double fill = (double)chars / maxTotal;
            if (endsSentence(words[j - 1].w)) cands.push_back({j, 3.0 + fill});
            else if (endsClause(words[j - 1].w)) cands.push_back({j, 2.0 + fill});
            if (j < n && words[j].s - words[j - 1].e > opt.pauseSplit) { pauseBreak = true; break; }
            // A complete sentence that already fills a good part of the caption ends it.
            if (endsSentence(words[j - 1].w) && fill > 0.45) break;
        }
        if (j < n && !pauseBreak) {
            // Overflow: choose the best natural break that keeps the caption reasonably full.
            size_t best = 0;
            double bestScore = -1;
            for (auto& [pos, score] : cands) {
                int c = 0;
                for (size_t k = i; k < pos; ++k) c += (int)words[k].w.size() + (k > i ? 1 : 0);
                if (c < maxTotal / 4) continue;
                if (score > bestScore || (score == bestScore && pos > best)) { bestScore = score; best = pos; }
            }
            if (best > i) j = best;
        }
        CaptionItem c;
        c.words.assign(words.begin() + i, words.begin() + j);
        c.start = words[i].s;
        c.end = words[j - 1].e;
        float conf = 0;
        for (auto& w : c.words) conf += w.conf;
        c.conf = conf / std::max<size_t>(1, c.words.size());
        c.text = balanceLines(joinWordList(words, i, j), opt.maxChars);
        out.push_back(c);
        i = j;
    }
    // Enforce minimum duration without overlapping the next caption.
    for (size_t k = 0; k < out.size(); ++k) {
        double next = k + 1 < out.size() ? out[k + 1].start : 1e18;
        if (out[k].end - out[k].start < opt.minDuration) out[k].end = std::min(next, out[k].start + opt.minDuration);
    }
    return out;
}

std::vector<ReadingIssue> validateReadingSpeed(const std::vector<CaptionItem>& items, double maxCps, double minDuration) {
    std::vector<ReadingIssue> out;
    for (size_t i = 0; i < items.size(); ++i) {
        double dur = items[i].end - items[i].start;
        int chars = 0;
        for (char c : items[i].text)
            if (c != '\n') ++chars;
        double cps = dur > 0 ? chars / dur : 1e9;
        if (cps > maxCps)
            out.push_back({i, cps, formatString("Caption %zu is fast to read (%.1f characters/second, limit %.0f). Extend its duration or shorten the text.", i + 1, cps, maxCps)});
        else if (dur < minDuration)
            out.push_back({i, cps, formatString("Caption %zu is on screen for only %.2f s.", i + 1, dur)});
    }
    return out;
}

std::vector<std::pair<double, double>> detectVoiceRegions(const std::vector<float>& pcm, double thrDb, double minSilence, double pad) {
    std::vector<std::pair<double, double>> out;
    const int sr = 16000, hop = 160;  // 10 ms
    size_t frames = pcm.size() / hop;
    if (frames == 0) return out;
    std::vector<double> db(frames);
    double peak = 1e-9;
    for (size_t f = 0; f < frames; ++f) {
        double s = 0;
        for (int i = 0; i < hop; ++i) s += pcm[f * hop + i] * pcm[f * hop + i];
        double rms = std::sqrt(s / hop);
        peak = std::max(peak, rms);
        db[f] = 20 * std::log10(rms + 1e-9);
    }
    // Adaptive threshold: relative to peak (handles quiet recordings).
    double thr = std::max(thrDb, 20 * std::log10(peak) - 35);
    bool on = false;
    size_t start = 0, lastVoice = 0;
    size_t silenceFrames = (size_t)(minSilence * 100);
    for (size_t f = 0; f < frames; ++f) {
        bool v = db[f] > thr;
        if (v) {
            if (!on) { on = true; start = f; }
            lastVoice = f;
        } else if (on && f - lastVoice > silenceFrames) {
            out.push_back({start / 100.0, (lastVoice + 1) / 100.0});
            on = false;
        }
    }
    if (on) out.push_back({start / 100.0, (lastVoice + 1) / 100.0});
    double total = (double)pcm.size() / sr;
    for (auto& r : out) { r.first = std::max(0.0, r.first - pad); r.second = std::min(total, r.second + pad); }
    // Merge overlaps.
    std::vector<std::pair<double, double>> merged;
    for (auto& r : out) {
        if (!merged.empty() && r.first <= merged.back().second) merged.back().second = std::max(merged.back().second, r.second);
        else merged.push_back(r);
    }
    return merged;
}

// ====================================================================== WER
static std::vector<std::string> normWords(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (std::isalnum((unsigned char)c) || c == '\'') cur += (char)std::tolower((unsigned char)c);
        else if (!cur.empty()) { out.push_back(cur); cur.clear(); }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

double wordErrorRate(const std::string& ref, const std::string& hyp) {
    auto r = normWords(ref), h = normWords(hyp);
    if (r.empty()) return h.empty() ? 0 : 1;
    std::vector<std::vector<int>> d(r.size() + 1, std::vector<int>(h.size() + 1));
    for (size_t i = 0; i <= r.size(); ++i) d[i][0] = (int)i;
    for (size_t j = 0; j <= h.size(); ++j) d[0][j] = (int)j;
    for (size_t i = 1; i <= r.size(); ++i)
        for (size_t j = 1; j <= h.size(); ++j)
            d[i][j] = std::min({d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + (r[i - 1] == h[j - 1] ? 0 : 1)});
    return (double)d[r.size()][h.size()] / r.size();
}

// ====================================================================== ASR (whisper.cpp)
bool asrAvailable() {
#ifdef MF_WITH_WHISPER
    return true;
#else
    return false;
#endif
}

json asrModelInfo(const std::string& path) {
    json j = {{"ok", false}, {"path", path}};
    int64_t sz = fileSize(path);
    if (sz <= 0) { j["error"] = "Model file not found."; return j; }
    j["size"] = sz;
    std::string name = pathBasename(path);
    std::string type = "custom";
    for (const char* t : {"tiny", "base", "small", "medium", "large"})
        if (name.find(t) != std::string::npos) { type = t; break; }
    j["type"] = type;
    j["multilingual"] = name.find(".en") == std::string::npos;
    j["quantized"] = name.find("-q") != std::string::npos;
    // Header check: ggml magic 0x67676d6c ("ggml" little-endian "lmgg").
    auto bytes = readFileBytes(path);
    if (bytes.size() >= 4) {
        uint32_t magic = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
        j["ok"] = magic == 0x67676d6c;
        if (!j["ok"]) j["error"] = "Not a whisper.cpp ggml model file.";
    }
    return j;
}

#ifdef MF_WITH_WHISPER
namespace {
struct CbState {
    const AsrOptions* opt;
    double base;   // progress offset
    double span;   // progress span for this chunk
    bool cancelled = false;
};
}  // namespace
#endif

AsrResult transcribe(const std::vector<float>& pcm, const AsrOptions& opt, const SegmentOptions& seg) {
    AsrResult res;
    res.audioSeconds = pcm.size() / 16000.0;
    res.model = pathBasename(opt.modelPath);
#ifndef MF_WITH_WHISPER
    res.error = "This build does not include the offline speech engine.";
    return res;
#else
    auto t0 = std::chrono::steady_clock::now();
    if (pcm.size() < 1600) { res.error = "Audio is too short to transcribe (needs at least 0.1 s)."; return res; }
    json info = asrModelInfo(opt.modelPath);
    if (!info.value("ok", false)) { res.error = "Speech model unavailable: " + info.value("error", std::string("unknown error")); return res; }
    static bool quiet = [] {
        whisper_log_set([](enum ggml_log_level level, const char* text, void*) {
            if (level == GGML_LOG_LEVEL_ERROR && text) MF_LOGW(std::string("whisper: ") + text);
        }, nullptr);
        return true;
    }();
    (void)quiet;
    whisper_context_params cp = whisper_context_default_params();
    cp.use_gpu = false;
    whisper_context* ctx = whisper_init_from_file_with_params(opt.modelPath.c_str(), cp);
    if (!ctx) { res.error = "Failed to load the speech model (file may be corrupt or memory is low)."; return res; }
    bool multilingual = whisper_is_multilingual(ctx) != 0;
    std::string lang = opt.language;
    if (!multilingual) lang = "en";
    // Regions: VAD-split speech to avoid hallucinations in silence; long regions chunked at quiet points.
    std::vector<std::pair<double, double>> regions;
    if (opt.useVad) regions = detectVoiceRegions(pcm);
    double total = res.audioSeconds;
    if (regions.empty()) regions = {{0.0, total}};
    std::vector<std::pair<double, double>> chunks;
    for (auto& r : regions) {
        double s = r.first;
        while (r.second - s > 28.0) {
            // Split at the quietest 10 ms within [s+20, s+28].
            double best = s + 28.0, bestE = 1e9;
            for (double t = s + 20.0; t < s + 28.0; t += 0.01) {
                size_t a = (size_t)(t * 16000), b = std::min(pcm.size(), a + 160);
                double e = 0;
                for (size_t k = a; k < b; ++k) e += pcm[k] * pcm[k];
                if (e < bestE) { bestE = e; best = t; }
            }
            chunks.push_back({s, best});
            s = best;
        }
        chunks.push_back({s, r.second});
    }
    double speechTotal = 0;
    for (auto& c : chunks) speechTotal += c.second - c.first;
    double done = 0;
    CbState cb;
    cb.opt = &opt;
    for (auto& ch : chunks) {
        size_t a = (size_t)(ch.first * 16000), b = std::min(pcm.size(), (size_t)(ch.second * 16000));
        if (b <= a + 1600) { done += ch.second - ch.first; continue; }
        std::vector<float> buf(pcm.begin() + a, pcm.begin() + b);
        // Whisper needs >= 1 s of audio for stable decoding; pad short chunks with silence.
        if (buf.size() < 16000) buf.resize(16000, 0.f);
        whisper_full_params p = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        p.n_threads = std::max(1, opt.threads);
        p.language = lang.c_str();
        p.translate = opt.translate;
        p.print_progress = false;
        p.print_realtime = false;
        p.print_special = false;
        p.print_timestamps = false;
        p.token_timestamps = true;
        p.max_len = 1;
        p.split_on_word = true;
        p.no_context = true;
        p.suppress_blank = true;
        p.suppress_nst = true;
        if (!opt.initialPrompt.empty()) p.initial_prompt = opt.initialPrompt.c_str();
        cb.base = speechTotal > 0 ? done / speechTotal : 0;
        cb.span = speechTotal > 0 ? (ch.second - ch.first) / speechTotal : 1;
        p.progress_callback = [](whisper_context*, whisper_state*, int progress, void* user) {
            auto* s = (CbState*)user;
            if (s->opt->progress && !s->opt->progress((float)(s->base + s->span * progress / 100.0))) s->cancelled = true;
        };
        p.progress_callback_user_data = &cb;
        p.abort_callback = [](void* user) { return ((CbState*)user)->cancelled; };
        p.abort_callback_user_data = &cb;
        int rc = whisper_full(ctx, p, buf.data(), (int)buf.size());
        if (cb.cancelled) {
            res.cancelled = true;
            res.error = "Transcription cancelled.";
            whisper_free(ctx);
            return res;
        }
        if (rc != 0) {
            res.error = "Speech recognition failed (code " + std::to_string(rc) + ").";
            whisper_free(ctx);
            return res;
        }
        if (res.language.empty()) res.language = whisper_lang_str(whisper_full_lang_id(ctx));
        int ns = whisper_full_n_segments(ctx);
        for (int i = 0; i < ns; ++i) {
            std::string text = whisper_full_get_segment_text(ctx, i);
            std::string t = text;
            size_t a0 = t.find_first_not_of(' ');
            if (a0 == std::string::npos) continue;
            t = t.substr(a0);
            if (t.empty() || t[0] == '[' || t[0] == '(') continue;  // non-speech annotations like [MUSIC]
            double s0 = whisper_full_get_segment_t0(ctx, i) / 100.0 + ch.first;
            double s1 = whisper_full_get_segment_t1(ctx, i) / 100.0 + ch.first;
            if (s0 >= ch.second + 0.2) continue;  // inside the silence padding
            int nt = whisper_full_n_tokens(ctx, i);
            double psum = 0;
            int pc = 0;
            for (int k = 0; k < nt; ++k) {
                whisper_token_data td = whisper_full_get_token_data(ctx, i, k);
                if (td.id >= whisper_token_eot(ctx)) continue;
                psum += td.p;
                ++pc;
            }
            CaptionWord w;
            w.s = s0;
            w.e = std::max(s1, s0 + 0.05);
            w.w = t;
            w.conf = pc ? (float)(psum / pc) : 0.5f;
            res.words.push_back(w);
        }
        done += ch.second - ch.first;
        if (opt.progress && !opt.progress((float)std::min(1.0, done / std::max(1e-9, speechTotal)))) {
            res.cancelled = true;
            res.error = "Transcription cancelled.";
            whisper_free(ctx);
            return res;
        }
    }
    whisper_free(ctx);
    // Monotonic, non-overlapping word times.
    for (size_t i = 1; i < res.words.size(); ++i) {
        if (res.words[i].s < res.words[i - 1].s) res.words[i].s = res.words[i - 1].s;
        if (res.words[i - 1].e > res.words[i].s) res.words[i - 1].e = res.words[i].s;
    }
    res.captions = segmentWords(res.words, seg);
    res.processingSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    res.ok = true;
    return res;
#endif
}

}  // namespace mf
