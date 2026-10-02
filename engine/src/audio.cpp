#include "mf/audio.hpp"

#include <cstdio>

#include "mf/model.hpp"
#include "mf/renderer.hpp"

namespace mf {

double dbToGain(double db) { return db <= -96 ? 0.0 : std::pow(10.0, db / 20.0); }
double gainToDb(double g) { return g <= 1e-6 ? -120.0 : 20.0 * std::log10(g); }

void AudioBuffer::sampleAt(double t, float& l, float& r) const {
    double pos = t * sampleRate;
    if (pos < 0 || channels <= 0) { l = r = 0; return; }
    size_t i = (size_t)pos;
    size_t n = frames();
    if (i + 1 >= n) { l = r = 0; return; }
    float f = (float)(pos - i);
    if (channels == 1) {
        l = r = samples[i] * (1 - f) + samples[i + 1] * f;
    } else {
        l = samples[i * channels] * (1 - f) + samples[(i + 1) * channels] * f;
        r = samples[i * channels + 1] * (1 - f) + samples[(i + 1) * channels + 1] * f;
    }
}

// ====================================================================== biquads (RBJ cookbook)
static void normalize(double b0, double b1, double b2, double a0, double a1, double a2, double c[5]) {
    c[0] = b0 / a0; c[1] = b1 / a0; c[2] = b2 / a0; c[3] = a1 / a0; c[4] = a2 / a0;
}
void biquadPeak(double fs, double f0, double g, double q, double c[5]) {
    double A = std::pow(10, g / 40), w = 2 * kPi * f0 / fs, al = std::sin(w) / (2 * q), cs = std::cos(w);
    normalize(1 + al * A, -2 * cs, 1 - al * A, 1 + al / A, -2 * cs, 1 - al / A, c);
}
void biquadLowShelf(double fs, double f0, double g, double c[5]) {
    double A = std::pow(10, g / 40), w = 2 * kPi * f0 / fs, cs = std::cos(w), al = std::sin(w) / 2 * std::sqrt(2.0), sq = 2 * std::sqrt(A) * al;
    normalize(A * ((A + 1) - (A - 1) * cs + sq), 2 * A * ((A - 1) - (A + 1) * cs), A * ((A + 1) - (A - 1) * cs - sq), (A + 1) + (A - 1) * cs + sq,
              -2 * ((A - 1) + (A + 1) * cs), (A + 1) + (A - 1) * cs - sq, c);
}
void biquadHighShelf(double fs, double f0, double g, double c[5]) {
    double A = std::pow(10, g / 40), w = 2 * kPi * f0 / fs, cs = std::cos(w), al = std::sin(w) / 2 * std::sqrt(2.0), sq = 2 * std::sqrt(A) * al;
    normalize(A * ((A + 1) + (A - 1) * cs + sq), -2 * A * ((A - 1) + (A + 1) * cs), A * ((A + 1) + (A - 1) * cs - sq), (A + 1) - (A - 1) * cs + sq,
              2 * ((A - 1) - (A + 1) * cs), (A + 1) - (A - 1) * cs - sq, c);
}
static inline float biquad(const double c[5], float* s, float x) {
    // Direct form II transposed: s[0], s[1]
    double y = c[0] * x + s[0];
    s[0] = (float)(c[1] * x - c[3] * y + s[1]);
    s[1] = (float)(c[2] * x - c[4] * y);
    return (float)y;
}

// ====================================================================== FFT
void fftReal(std::vector<float>& re, std::vector<float>& im) {
    size_t n = re.size();
    im.assign(n, 0.f);
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = -2 * kPi / len;
        float wr = (float)std::cos(ang), wi = (float)std::sin(ang);
        for (size_t i = 0; i < n; i += len) {
            float cr = 1, ci = 0;
            for (size_t j = 0; j < len / 2; ++j) {
                float ur = re[i + j], ui = im[i + j];
                float vr = re[i + j + len / 2] * cr - im[i + j + len / 2] * ci;
                float vi = re[i + j + len / 2] * ci + im[i + j + len / 2] * cr;
                re[i + j] = ur + vr; im[i + j] = ui + vi;
                re[i + j + len / 2] = ur - vr; im[i + j + len / 2] = ui - vi;
                float nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
}

// ====================================================================== mixer
static bool layerHasAudio(const json& L) {
    std::string ty = L.value("type", "");
    return (ty == "audio" || ty == "video") && L.contains("audio") && L.contains("asset");
}

float AudioEngine::mix(const json& project, const json& comp, double t0, int frames, float* out, int sr) {
    std::fill(out, out + (size_t)frames * 2, 0.f);
    const json& buses = jobj(jobj(comp, "audio"), "buses");
    auto busGain = [&](const std::string& b) { return dbToGain(buses.value(b, json::object()).value("gain", 0.0)); };
    bool anySolo = false;
    for (auto& L : jarr(comp, "layers"))
        if (layerHasAudio(L) && L.value("solo", false)) anySolo = true;
    std::vector<float> dialogue((size_t)frames * 2, 0.f), music((size_t)frames * 2, 0.f), effects((size_t)frames * 2, 0.f), master((size_t)frames * 2, 0.f);
    std::lock_guard<std::mutex> lk(m_);
    for (auto& L : jarr(comp, "layers")) {
        if (!layerHasAudio(L) || L.value("muted", false) || !L.value("enabled", true)) continue;
        if (anySolo && !L.value("solo", false)) continue;
        if (L.value("freezeAt", json()).is_number()) continue;
        double in = L.value("in", 0.0), outT = L.value("out", 0.0);
        if (t0 + (double)frames / sr <= in || t0 >= outT) continue;
        const json* asset = findAsset(project, L.value("asset", ""));
        if (!asset || !asset->value("hasAudio", asset->value("type", "") == "audio")) continue;
        auto buf = media_->audio(*asset);
        if (!buf || buf->samples.empty()) continue;
        const json& A = L["audio"];
        std::string bus = A.value("bus", std::string("dialogue"));
        std::vector<float>& dst = bus == "music" ? music : bus == "effects" ? effects : bus == "master" ? master : dialogue;
        LayerState& st = states_[L.value("id", "")];
        if (std::fabs(st.lastT - t0) > 1e-3) {
            st = LayerState();  // discontinuity: reset filter state
        }
        st.lastT = t0 + (double)frames / sr;
        // Per-layer DSP coefficients.
        double eqLow = jobj(A, "eq").value("low", 0.0), eqMid = jobj(A, "eq").value("mid", 0.0),
               eqHigh = jobj(A, "eq").value("high", 0.0);
        double cl[5], cm[5], ch[5];
        biquadLowShelf(sr, 200, eqLow, cl);
        biquadPeak(sr, 1000, eqMid, 0.9, cm);
        biquadHighShelf(sr, 5000, eqHigh, ch);
        bool eq = eqLow != 0 || eqMid != 0 || eqHigh != 0;
        const json& C = jobj(A, "compressor");
        bool comp_ = C.value("enabled", false);
        double thr = dbToGain(C.value("threshold", -18.0)), ratio = std::max(1.0, C.value("ratio", 3.0));
        double denoise = A.value("denoise", 0.0) / 100.0;
        double gateThr = dbToGain(-70 + denoise * 40);
        double pitch = A.value("pitch", 0.0);
        double pitchRatio = std::pow(2.0, pitch / 12.0);
        double fadeIn = A.value("fadeIn", 0.0), fadeOut = A.value("fadeOut", 0.0);
        double start = L.value("start", 0.0);
        double atkC = std::exp(-1.0 / (0.005 * sr)), relC = std::exp(-1.0 / (0.1 * sr));
        // Evaluate volume/pan at a coarse rate (every 64 samples) for efficiency.
        double vol = 0, pan = 0;
        const int block = 64;
        if (pitch != 0 && st.pitchBuf.empty()) st.pitchBuf.assign(4096 * 2, 0.f);
        size_t pbN = st.pitchBuf.size() / 2;
        for (int i = 0; i < frames; ++i) {
            double t = t0 + (double)i / sr;
            if (t < in || t >= outT) continue;
            if (i % block == 0) {
                double lt = t - start;
                vol = evalRaw(A["volume"], lt).num(0);
                pan = clampv(evalRaw(A["pan"], lt).num(0) / 100.0, -1.0, 1.0);
            }
            double src = layerSourceTime(L, t);
            float l, r;
            buf->sampleAt(src, l, r);
            if (eq) {
                l = biquad(cl, st.eq[0][0], l); r = biquad(cl, st.eq[0][1], r);
                l = biquad(cm, st.eq[1][0], l); r = biquad(cm, st.eq[1][1], r);
                l = biquad(ch, st.eq[2][0], l); r = biquad(ch, st.eq[2][1], r);
            }
            if (denoise > 0) {
                float a = std::max(std::fabs(l), std::fabs(r));
                st.gateEnv = a > st.gateEnv ? a : (float)(st.gateEnv * relC + a * (1 - relC));
                float g = st.gateEnv < gateThr ? (float)std::pow(st.gateEnv / gateThr, 2.0 * denoise) : 1.f;
                l *= g; r *= g;
            }
            if (pitch != 0) {
                // Two-tap delay-line pitch shifter with crossfaded read heads.
                size_t w = (size_t)(i + (size_t)(st.lastT * sr)) % pbN;
                st.pitchBuf[w * 2] = l;
                st.pitchBuf[w * 2 + 1] = r;
                double win = 2048;
                st.pitchPhase = std::fmod(st.pitchPhase + (1 - pitchRatio), win);
                if (st.pitchPhase < 0) st.pitchPhase += win;
                float ol = 0, orr = 0;
                for (int k = 0; k < 2; ++k) {
                    double d = std::fmod(st.pitchPhase + k * win / 2, win);
                    double rp = (double)w - d;
                    while (rp < 0) rp += pbN;
                    size_t ri = (size_t)rp % pbN;
                    float g = (float)std::sin(kPi * d / win);
                    ol += st.pitchBuf[ri * 2] * g;
                    orr += st.pitchBuf[ri * 2 + 1] * g;
                }
                l = ol; r = orr;
            }
            if (comp_) {
                float a = std::max(std::fabs(l), std::fabs(r));
                st.compEnv = a > st.compEnv ? (float)(st.compEnv * atkC + a * (1 - atkC)) : (float)(st.compEnv * relC + a * (1 - relC));
                if (st.compEnv > thr) {
                    double over = gainToDb(st.compEnv) - gainToDb(thr);
                    double g = dbToGain(-over * (1 - 1 / ratio));
                    l *= (float)g; r *= (float)g;
                }
            }
            double g = dbToGain(vol);
            if (fadeIn > 0 && t - in < fadeIn) g *= (t - in) / fadeIn;
            if (fadeOut > 0 && outT - t < fadeOut) g *= (outT - t) / fadeOut;
            // Constant-power pan.
            double pl = std::cos((pan + 1) * kPi / 4) * std::sqrt(2.0), pr = std::sin((pan + 1) * kPi / 4) * std::sqrt(2.0);
            dst[i * 2] += (float)(l * g * pl);
            dst[i * 2 + 1] += (float)(r * g * pr);
        }
    }
    // Ducking of music bus driven by dialogue envelope.
    const json& duck = jobj(jobj(buses, "music"), "duck");
    bool duckOn = duck.value("enabled", false);
    double duckAmt = dbToGain(duck.value("amount", -12.0));
    double atk = std::exp(-1.0 / (std::max(0.01, duck.value("attack", 0.15)) * sr)), rel = std::exp(-1.0 / (std::max(0.01, duck.value("release", 0.4)) * sr));
    double gm = busGain("master"), gd = busGain("dialogue"), gmu = busGain("music"), ge = busGain("effects");
    bool limiter = jobj(buses, "master").value("limiter", true);
    float peak = 0;
    for (int i = 0; i < frames; ++i) {
        float dl = dialogue[i * 2], dr = dialogue[i * 2 + 1];
        float duckGain = 1.f;
        if (duckOn) {
            float lvl = std::max(std::fabs(dl), std::fabs(dr)) > 0.02f ? 1.f : 0.f;
            duckEnv_ = lvl > duckEnv_ ? (float)(duckEnv_ * atk + lvl * (1 - atk)) : (float)(duckEnv_ * rel + lvl * (1 - rel));
            duckGain = (float)(1 - (1 - duckAmt) * duckEnv_);
        }
        for (int c = 0; c < 2; ++c) {
            size_t k = (size_t)i * 2 + c;
            float v = (float)((dialogue[k] * gd + music[k] * gmu * duckGain + effects[k] * ge + master[k]) * gm);
            out[k] = v;
        }
        if (limiter) {
            float a = std::max(std::fabs(out[i * 2]), std::fabs(out[i * 2 + 1]));
            float target = a > 0.98f ? 0.98f / a : 1.f;
            limEnv_ = target < limEnv_ ? target : limEnv_ + (1 - limEnv_) * 0.0005f;
            out[i * 2] *= limEnv_;
            out[i * 2 + 1] *= limEnv_;
        }
        peak = std::max({peak, std::fabs(out[i * 2]), std::fabs(out[i * 2 + 1])});
    }
    return peak;
}

// ====================================================================== analysis
std::shared_ptr<const AudioAnalysis> AudioEngine::analyze(const json& asset) {
    std::string key = asset.value("id", std::string()) + "|" + asset.value("checksum", asset.value("path", std::string()));
    {
        std::lock_guard<std::mutex> lk(m_);
        auto it = analyses_.find(key);
        if (it != analyses_.end()) return it->second;
    }
    auto buf = media_->audio(asset);
    auto A = std::make_shared<AudioAnalysis>();
    if (buf && !buf->samples.empty()) {
        int sr = buf->sampleRate;
        size_t hopN = (size_t)(sr * A->hop);
        const size_t N = 1024;
        size_t frames = buf->frames();
        std::vector<float> re(N), im(N), prevMag(N / 2, 0.f);
        for (size_t start = 0; start + N <= frames || start == 0; start += hopN) {
            double rms = 0;
            for (size_t i = 0; i < N; ++i) {
                size_t fi = std::min(frames - 1, start + i);
                float v = buf->channels == 1 ? buf->samples[fi] : 0.5f * (buf->samples[fi * buf->channels] + buf->samples[fi * buf->channels + 1]);
                re[i] = v * (float)(0.5 - 0.5 * std::cos(2 * kPi * i / (N - 1)));
                rms += v * v;
            }
            A->rms.push_back((float)std::sqrt(rms / N));
            fftReal(re, im);
            double b = 0, m = 0, h = 0, flux = 0, cs = 0, ms = 0;
            for (size_t k = 1; k < N / 2; ++k) {
                double mag = std::sqrt(re[k] * re[k] + im[k] * im[k]);
                double f = (double)k * sr / N;
                if (f < 250) b += mag; else if (f < 4000) m += mag; else h += mag;
                flux += std::max(0.0, mag - prevMag[k]);
                prevMag[k] = (float)mag;
                cs += f * mag;
                ms += mag;
            }
            A->bass.push_back((float)b);
            A->mid.push_back((float)m);
            A->treble.push_back((float)h);
            A->onset.push_back((float)flux);
            A->centroid.push_back((float)(ms > 0 ? cs / ms : 0));
            if (start + N > frames) break;
        }
        auto norm = [](std::vector<float>& v) {
            float mx = 1e-9f;
            for (float x : v) mx = std::max(mx, x);
            for (float& x : v) x /= mx;
        };
        for (float x : A->rms) A->peak = std::max(A->peak, x);
        norm(A->rms); norm(A->bass); norm(A->mid); norm(A->treble); norm(A->onset);
        // Beat picking: onset peaks above adaptive threshold, min spacing 0.25s.
        double lastBeat = -1;
        for (size_t i = 2; i + 2 < A->onset.size(); ++i) {
            double avg = 0;
            size_t w0 = i >= 40 ? i - 40 : 0;
            for (size_t k = w0; k < i; ++k) avg += A->onset[k];
            avg /= std::max<size_t>(1, i - w0);
            float o = A->onset[i];
            if (o > avg * 1.6 + 0.08 && o >= A->onset[i - 1] && o >= A->onset[i + 1]) {
                double t = i * A->hop;
                if (t - lastBeat > 0.25) { A->beats.push_back(t); lastBeat = t; }
            }
        }
        // Tempo: median inter-beat interval.
        if (A->beats.size() > 3) {
            std::vector<double> iv;
            for (size_t i = 1; i < A->beats.size(); ++i) iv.push_back(A->beats[i] - A->beats[i - 1]);
            std::nth_element(iv.begin(), iv.begin() + iv.size() / 2, iv.end());
            double med = iv[iv.size() / 2];
            double bpm = 60.0 / std::max(0.2, med);
            while (bpm < 70) bpm *= 2;
            while (bpm > 180) bpm /= 2;
            A->tempo = bpm;
        }
    }
    std::lock_guard<std::mutex> lk(m_);
    analyses_[key] = A;
    return A;
}

bool AudioEngine::features(const json* comp, double t, std::map<std::string, double>& out) {
    if (!comp || !project_) return false;
    double amp = 0, rms = 0, bass = 0, mid = 0, treble = 0, beat = 0, tempo = 0, centroid = 0;
    bool any = false;
    for (auto& L : jarr(*comp, "layers")) {
        if (!layerHasAudio(L) || L.value("muted", false) || !layerActiveAt(L, t)) continue;
        const json* a = findAsset(*project_, L.value("asset", ""));
        if (!a) continue;
        auto A = analyze(*a);
        if (A->rms.empty()) continue;
        double st = layerSourceTime(L, t);
        size_t i = std::min(A->rms.size() - 1, (size_t)std::max(0.0, st / A->hop));
        double vol = dbToGain(evalRaw(L["audio"]["volume"], t - L.value("start", 0.0)).num(0));
        rms = std::max(rms, A->rms[i] * vol);
        amp = std::max(amp, A->rms[i] * vol);
        bass = std::max(bass, A->bass[i] * vol);
        mid = std::max(mid, A->mid[i] * vol);
        treble = std::max(treble, A->treble[i] * vol);
        centroid = std::max(centroid, (double)A->centroid[i]);
        for (double b : A->beats) {
            double d = st - b;
            if (d >= 0 && d < 0.3) beat = std::max(beat, std::exp(-d * 12));
        }
        tempo = std::max(tempo, A->tempo);
        any = true;
    }
    out["amplitude"] = amp;
    out["rms"] = rms;
    out["bass"] = bass;
    out["mid"] = mid;
    out["treble"] = treble;
    out["beat"] = beat;
    out["tempo"] = tempo;
    out["centroid"] = centroid;
    return any;
}

std::vector<float> AudioEngine::spectrum(const json& project, const json& comp, double t, int bands) {
    const int N = 2048, sr = 48000;
    std::vector<float> mixbuf(N * 2);
    {
        // mix() locks internally; avoid holding m_ here.
    }
    AudioEngine tmp(media_);  // stateless mix for analysis windows
    tmp.mix(project, comp, std::max(0.0, t - N / 2.0 / sr), N, mixbuf.data(), sr);
    std::vector<float> re(N), im(N);
    for (int i = 0; i < N; ++i) re[i] = 0.5f * (mixbuf[i * 2] + mixbuf[i * 2 + 1]) * (float)(0.5 - 0.5 * std::cos(2 * kPi * i / (N - 1)));
    fftReal(re, im);
    std::vector<float> out(bands, 0.f);
    double fmin = 40, fmax = 16000;
    for (int b = 0; b < bands; ++b) {
        double f0 = fmin * std::pow(fmax / fmin, (double)b / bands), f1 = fmin * std::pow(fmax / fmin, (double)(b + 1) / bands);
        int k0 = std::max(1, (int)(f0 * N / sr)), k1 = std::max(k0 + 1, (int)(f1 * N / sr));
        double s = 0;
        for (int k = k0; k < k1 && k < N / 2; ++k) s = std::max(s, (double)std::sqrt(re[k] * re[k] + im[k] * im[k]));
        double db = gainToDb(s / (N / 4.0));
        out[b] = (float)clampv((db + 60) / 60, 0.0, 1.0);
    }
    return out;
}

std::vector<float> AudioEngine::waveform(const json& project, const json& comp, double t, int samples, double window) {
    const int sr = 48000;
    int n = std::max(16, (int)(window * sr));
    std::vector<float> mixbuf((size_t)n * 2);
    AudioEngine tmp(media_);
    tmp.mix(project, comp, std::max(0.0, t - window / 2), n, mixbuf.data(), sr);
    std::vector<float> out(samples);
    for (int i = 0; i < samples; ++i) {
        int k = (int)((double)i / samples * n);
        out[i] = 0.5f * (mixbuf[k * 2] + mixbuf[k * 2 + 1]);
    }
    return out;
}

std::vector<float> AudioEngine::peaks(const json& asset, int buckets) {
    std::vector<float> out(std::max(1, buckets), 0.f);
    auto buf = media_->audio(asset);
    if (!buf || buf->samples.empty()) return out;
    size_t frames = buf->frames();
    for (int b = 0; b < buckets; ++b) {
        size_t s = frames * b / buckets, e = std::max(s + 1, frames * (b + 1) / buckets);
        float m = 0;
        for (size_t i = s; i < e && i < frames; ++i)
            for (int c = 0; c < buf->channels; ++c) m = std::max(m, std::fabs(buf->samples[i * buf->channels + c]));
        out[b] = m;
    }
    return out;
}

std::vector<std::pair<double, double>> AudioEngine::speechRanges(const json& project, const json& comp, const json& L, double minGap) {
    std::vector<std::pair<double, double>> out;
    const json* a = findAsset(project, L.value("asset", ""));
    if (!a) return out;
    auto A = analyze(*a);
    if (A->rms.empty()) return out;
    // Speech band energy (mid) with hysteresis.
    double on = 0.12, off = 0.06;
    bool active = false;
    double s = 0;
    double in = L.value("in", 0.0), outT = L.value("out", 0.0);
    for (double t = in; t < outT; t += A->hop) {
        double st = layerSourceTime(L, t);
        size_t i = std::min(A->rms.size() - 1, (size_t)std::max(0.0, st / A->hop));
        double e = A->mid[i] * 0.7 + A->rms[i] * 0.3;
        if (!active && e > on) { active = true; s = t; }
        else if (active && e < off) {
            active = false;
            if (!out.empty() && s - out.back().second < minGap) out.back().second = t;
            else out.push_back({s, t});
        }
    }
    if (active) out.push_back({s, outT});
    // Drop blips.
    std::vector<std::pair<double, double>> filtered;
    for (auto& r : out)
        if (r.second - r.first > 0.12) filtered.push_back(r);
    (void)comp;
    return filtered;
}

// ====================================================================== WAV I/O + resample
std::vector<float> resampleMono(const AudioBuffer& buf, int target) {
    std::vector<float> out;
    if (buf.samples.empty()) return out;
    double ratio = (double)buf.sampleRate / target;
    size_t n = (size_t)(buf.frames() / ratio);
    out.resize(n);
    // Simple low-pass by averaging the source span (anti-alias) then sample.
    for (size_t i = 0; i < n; ++i) {
        double s0 = i * ratio, s1 = (i + 1) * ratio;
        size_t a = (size_t)s0, b = std::min(buf.frames(), (size_t)std::ceil(s1));
        double acc = 0;
        int cnt = 0;
        for (size_t k = a; k < b; ++k) {
            float v = 0;
            for (int c = 0; c < buf.channels; ++c) v += buf.samples[k * buf.channels + c];
            acc += v / buf.channels;
            ++cnt;
        }
        out[i] = cnt ? (float)(acc / cnt) : 0.f;
    }
    return out;
}

bool loadWav(const std::string& path, AudioBuffer& out, std::string* err) {
    bool ok;
    auto d = readFileBytes(path, &ok);
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    if (!ok || d.size() < 44) return fail("cannot read WAV");
    if (std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) return fail("not a RIFF/WAVE file");
    size_t off = 12;
    int fmt = 0, ch = 0, sr = 0, bits = 0;
    const uint8_t* data = nullptr;
    size_t dataLen = 0;
    auto u16 = [&](size_t o) { return (int)(d[o] | (d[o + 1] << 8)); };
    auto u32 = [&](size_t o) { return (uint32_t)(d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24)); };
    while (off + 8 <= d.size()) {
        uint32_t len = u32(off + 4);
        if (!std::memcmp(d.data() + off, "fmt ", 4)) {
            fmt = u16(off + 8); ch = u16(off + 10); sr = (int)u32(off + 12); bits = u16(off + 22);
            if (fmt == 0xFFFE && len >= 40) fmt = u16(off + 32);
        } else if (!std::memcmp(d.data() + off, "data", 4)) {
            data = d.data() + off + 8;
            dataLen = std::min<size_t>(len, d.size() - off - 8);
        }
        off += 8 + len + (len & 1);
    }
    if (!data || ch <= 0 || sr <= 0) return fail("missing fmt/data chunk");
    out.sampleRate = sr;
    out.channels = ch;
    size_t bps = bits / 8;
    size_t n = dataLen / bps;
    out.samples.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* p = data + i * bps;
        float v;
        if (fmt == 3 && bits == 32) std::memcpy(&v, p, 4);
        else if (bits == 16) v = (int16_t)(p[0] | (p[1] << 8)) / 32768.f;
        else if (bits == 24) v = (int32_t)(((p[0] << 8) | (p[1] << 16) | (p[2] << 24))) / 2147483648.f;
        else if (bits == 32) v = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)) / 2147483648.f;
        else if (bits == 8) v = (p[0] - 128) / 128.f;
        else return fail("unsupported WAV bit depth");
        out.samples[i] = v;
    }
    return true;
}

bool saveWav(const std::string& path, const float* s, size_t frames, int ch, int sr) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    uint32_t dataLen = (uint32_t)(frames * ch * 2);
    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); w32(36 + dataLen); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); w32(16); w16(1); w16((uint16_t)ch); w32(sr); w32(sr * ch * 2); w16((uint16_t)(ch * 2)); w16(16);
    std::fwrite("data", 1, 4, f); w32(dataLen);
    for (size_t i = 0; i < frames * ch; ++i) {
        int16_t v = (int16_t)clampv((int)std::lround(s[i] * 32767.f), -32768, 32767);
        std::fwrite(&v, 2, 1, f);
    }
    bool ok = std::ferror(f) == 0;
    std::fclose(f);
    return ok;
}

}  // namespace mf
