// Captions: subtitle import/export, word->caption segmentation, reading-speed validation, offline ASR (whisper.cpp).
#pragma once
#include "common.hpp"

namespace mf {

struct CaptionWord {
    double s = 0, e = 0;
    std::string w;
    float conf = 1;
};

struct CaptionItem {
    double start = 0, end = 0;
    std::string text;
    int speaker = 0;
    std::vector<CaptionWord> words;
    float conf = 1;
};

json captionsToJson(const std::vector<CaptionItem>& items);
std::vector<CaptionItem> captionsFromJson(const json& items);

// Subtitle formats.
std::vector<CaptionItem> parseSrt(const std::string& text, std::string* err = nullptr);
std::vector<CaptionItem> parseVtt(const std::string& text, std::string* err = nullptr);
std::vector<CaptionItem> parseAss(const std::string& text, std::string* err = nullptr);
std::vector<CaptionItem> parseSubtitles(const std::string& text, const std::string& ext, std::string* err = nullptr);
std::string toSrt(const std::vector<CaptionItem>& items);
std::string toVtt(const std::vector<CaptionItem>& items);
std::string toAss(const std::vector<CaptionItem>& items, const json& style, int width, int height);
std::string formatTimestamp(double t, char msSep);  // 00:00:01,500

struct SegmentOptions {
    int maxChars = 42;        // per line
    int maxLines = 2;
    double maxDuration = 6.0;
    double minDuration = 0.7;
    double pauseSplit = 0.6;  // split at gaps longer than this
    double maxCps = 20.0;     // characters per second
};
std::vector<CaptionItem> segmentWords(const std::vector<CaptionWord>& words, const SegmentOptions& opt);

struct ReadingIssue {
    size_t index = 0;
    double cps = 0;
    std::string message;
};
std::vector<ReadingIssue> validateReadingSpeed(const std::vector<CaptionItem>& items, double maxCps, double minDuration = 0.7);

// Voice activity regions (seconds) from 16 kHz mono PCM using an energy detector with hysteresis.
std::vector<std::pair<double, double>> detectVoiceRegions(const std::vector<float>& pcm16k, double thresholdDb = -42, double minSilence = 0.6,
                                                          double pad = 0.2);

// ---------------------------------------------------------------- ASR
struct AsrOptions {
    std::string modelPath;
    std::string language = "auto";  // ISO code or "auto" (multilingual models only)
    bool translate = false;
    int threads = 4;
    bool useVad = true;
    std::string initialPrompt;
    std::function<bool(float)> progress;  // 0..1, return false to cancel
};

struct AsrResult {
    bool ok = false;
    bool cancelled = false;
    std::string error;
    std::string language;
    std::vector<CaptionWord> words;
    std::vector<CaptionItem> captions;
    double audioSeconds = 0, processingSeconds = 0;
    std::string model;
};

bool asrAvailable();
json asrModelInfo(const std::string& path);  // {ok, size, type, multilingual}
AsrResult transcribe(const std::vector<float>& pcm16k, const AsrOptions& opt, const SegmentOptions& seg = SegmentOptions());

// Word error rate between reference and hypothesis (normalized, punctuation-insensitive).
double wordErrorRate(const std::string& reference, const std::string& hypothesis);

}  // namespace mf
