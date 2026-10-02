// Audio: decoded buffers, mixer (volume/pan/fades/EQ/compressor/gate/pitch/buses/ducking/limiter),
// analysis (RMS, bands, onsets, tempo, centroid), waveform peaks, speech detection.
#pragma once
#include <map>
#include <mutex>

#include "expressions.hpp"
#include "property.hpp"

namespace mf {

struct AudioBuffer {
    int sampleRate = 48000;
    int channels = 2;
    std::vector<float> samples;  // interleaved
    double duration() const { return channels > 0 && sampleRate > 0 ? (double)samples.size() / channels / sampleRate : 0; }
    size_t frames() const { return channels > 0 ? samples.size() / channels : 0; }
    // Linear-interpolated stereo sample at time (seconds).
    void sampleAt(double t, float& l, float& r) const;
};

struct AudioAnalysis {
    double hop = 0.01;  // seconds per feature frame
    std::vector<float> rms, bass, mid, treble, onset, centroid;  // normalized 0..1 (centroid in Hz)
    std::vector<double> beats;  // beat times (s)
    double tempo = 0;           // bpm estimate
    float peak = 0;
};

class MediaProvider;

class AudioEngine : public AudioFeatureSource {
   public:
    explicit AudioEngine(MediaProvider* media) : media_(media) {}
    // Mix composition audio into interleaved stereo float `out` (frames*2). Returns peak level.
    float mix(const json& project, const json& comp, double t0, int frames, float* out, int sampleRate = 48000);
    // AudioFeatureSource
    bool features(const json* comp, double compTime, std::map<std::string, double>& out) override;
    std::vector<float> spectrum(const json& project, const json& comp, double t, int bands);
    std::vector<float> waveform(const json& project, const json& comp, double t, int samples, double window);
    std::shared_ptr<const AudioAnalysis> analyze(const json& asset);
    std::vector<float> peaks(const json& asset, int buckets);  // waveform overview (max abs per bucket)
    // Energy based voice-activity ranges in comp time for a layer (used by auto-ducking & captions).
    std::vector<std::pair<double, double>> speechRanges(const json& project, const json& comp, const json& layer, double minGap = 0.3);
    void setProject(const json* project) { project_ = project; }
    void resetState() { std::lock_guard<std::mutex> lk(m_); states_.clear(); }

   private:
    struct LayerState {
        double lastT = -1;
        float eq[3][2][4] = {};  // band, channel, biquad state
        float compEnv = 0, gateEnv = 0;
        std::vector<float> pitchBuf;
        double pitchPhase = 0;
    };
    MediaProvider* media_;
    const json* project_ = nullptr;
    std::mutex m_;
    std::map<std::string, LayerState> states_;
    std::map<std::string, std::shared_ptr<const AudioAnalysis>> analyses_;
    float duckEnv_ = 0;
    float limEnv_ = 1.f;
};

// Helpers exposed for tests.
void fftReal(std::vector<float>& re, std::vector<float>& im);  // in-place radix-2, size power of two
double dbToGain(double db);
double gainToDb(double g);
void biquadPeak(double fs, double f0, double gainDb, double q, double coef[5]);
void biquadLowShelf(double fs, double f0, double gainDb, double coef[5]);
void biquadHighShelf(double fs, double f0, double gainDb, double coef[5]);
// Resample mono/stereo float buffers (linear) e.g. to 16 kHz for ASR.
std::vector<float> resampleMono(const AudioBuffer& buf, int targetRate);
bool loadWav(const std::string& path, AudioBuffer& out, std::string* err = nullptr);
bool saveWav(const std::string& path, const float* interleaved, size_t frames, int channels, int sampleRate);

}  // namespace mf
