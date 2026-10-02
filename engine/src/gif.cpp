#include "mf/gif.hpp"

#include <array>

namespace mf {

namespace {

struct Box {
    std::vector<std::array<uint8_t, 3>>::iterator b, e;
};

std::vector<std::array<uint8_t, 3>> medianCut(std::vector<std::array<uint8_t, 3>> px, int colors) {
    std::vector<std::array<uint8_t, 3>> pal;
    if (px.empty()) return {{0, 0, 0}};
    std::vector<Box> boxes = {{px.begin(), px.end()}};
    while ((int)boxes.size() < colors) {
        // Split the box with the largest channel range.
        int bi = -1, bc = 0, br = -1;
        for (int i = 0; i < (int)boxes.size(); ++i) {
            if (boxes[i].e - boxes[i].b < 2) continue;
            for (int c = 0; c < 3; ++c) {
                uint8_t mn = 255, mx = 0;
                for (auto it = boxes[i].b; it != boxes[i].e; ++it) { mn = std::min(mn, (*it)[c]); mx = std::max(mx, (*it)[c]); }
                if (mx - mn > br) { br = mx - mn; bi = i; bc = c; }
            }
        }
        if (bi < 0 || br <= 0) break;
        Box bx = boxes[bi];
        auto mid = bx.b + (bx.e - bx.b) / 2;
        std::nth_element(bx.b, mid, bx.e, [bc](const auto& a, const auto& b) { return a[bc] < b[bc]; });
        boxes[bi] = {bx.b, mid};
        boxes.push_back({mid, bx.e});
    }
    for (auto& bx : boxes) {
        uint64_t s[3] = {0, 0, 0};
        size_t n = bx.e - bx.b;
        for (auto it = bx.b; it != bx.e; ++it)
            for (int c = 0; c < 3; ++c) s[c] += (*it)[c];
        pal.push_back({(uint8_t)(s[0] / std::max<size_t>(1, n)), (uint8_t)(s[1] / std::max<size_t>(1, n)), (uint8_t)(s[2] / std::max<size_t>(1, n))});
    }
    return pal;
}

class BitWriter {
   public:
    std::vector<uint8_t> bytes;
    uint32_t acc = 0;
    int bits = 0;
    void put(int code, int size) {
        acc |= (uint32_t)code << bits;
        bits += size;
        while (bits >= 8) { bytes.push_back(acc & 0xFF); acc >>= 8; bits -= 8; }
    }
    void flush() { if (bits > 0) bytes.push_back(acc & 0xFF); acc = 0; bits = 0; }
};

std::vector<uint8_t> lzw(const std::vector<uint8_t>& idx, int minCode) {
    BitWriter bw;
    int clear = 1 << minCode, eoi = clear + 1;
    int size = minCode + 1, next = eoi + 1;
    std::vector<int> dict(4096 * 256, -1);  // (prefix << 8 | byte) -> code
    bw.put(clear, size);
    int prefix = -1;
    for (uint8_t c : idx) {
        if (prefix < 0) { prefix = c; continue; }
        int key = prefix * 256 + c;
        if (dict[key] >= 0) { prefix = dict[key]; continue; }
        bw.put(prefix, size);
        if (next < 4096) {
            dict[key] = next++;
            if (next > (1 << size) && size < 12) ++size;
        } else {
            bw.put(clear, size);
            std::fill(dict.begin(), dict.end(), -1);
            size = minCode + 1;
            next = eoi + 1;
        }
        prefix = c;
    }
    if (prefix >= 0) bw.put(prefix, size);
    bw.put(eoi, size);
    bw.flush();
    return bw.bytes;
}

}  // namespace

GifWriter::~GifWriter() {
    if (f_) close();
}

bool GifWriter::open(const std::string& path, int w, int h, int loop) {
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) return false;
    w_ = w;
    h_ = h;
    std::fwrite("GIF89a", 1, 6, f_);
    uint8_t lsd[7] = {(uint8_t)(w & 255), (uint8_t)(w >> 8), (uint8_t)(h & 255), (uint8_t)(h >> 8), 0x00, 0, 0};
    std::fwrite(lsd, 1, 7, f_);
    uint8_t app[19] = {0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 0x03, 0x01, (uint8_t)(loop & 255), (uint8_t)(loop >> 8), 0};
    std::fwrite(app, 1, 19, f_);
    return true;
}

bool GifWriter::addFrame(const Image& img, int delayCs, bool dither) {
    if (!f_ || img.w != w_ || img.h != h_) return false;
    // Sample pixels for palette (subsample large frames).
    std::vector<std::array<uint8_t, 3>> samples;
    size_t n = (size_t)w_ * h_;
    size_t step = std::max<size_t>(1, n / 60000);
    bool anyTransparent = false;
    for (size_t i = 0; i < n; i += step) {
        const uint8_t* p = img.px.data() + i * 4;
        if (p[3] < 128) { anyTransparent = true; continue; }
        float a = p[3] / 255.f;
        samples.push_back({(uint8_t)std::min(255.f, p[0] / a), (uint8_t)std::min(255.f, p[1] / a), (uint8_t)std::min(255.f, p[2] / a)});
    }
    for (size_t i = 0; i < n && !anyTransparent; ++i) if (img.px[i * 4 + 3] < 128) anyTransparent = true;
    auto pal = medianCut(samples, anyTransparent ? 255 : 256);
    while (pal.size() < 256) pal.push_back({0, 0, 0});
    int transIndex = anyTransparent ? 255 : -1;
    // Map with optional Floyd-Steinberg error diffusion.
    std::vector<uint8_t> idx(n);
    std::vector<float> err((size_t)(w_ + 2) * 2 * 3, 0.f);
    auto nearest = [&](float r, float g, float b) {
        int best = 0;
        float bd = 1e30f;
        int lim = anyTransparent ? 255 : 256;
        for (int k = 0; k < lim; ++k) {
            float dr = r - pal[k][0], dg = g - pal[k][1], db = b - pal[k][2];
            float d = dr * dr * 0.3f + dg * dg * 0.59f + db * db * 0.11f;
            if (d < bd) { bd = d; best = k; }
        }
        return best;
    };
    std::vector<int> cache(1 << 15, -1);
    for (int y = 0; y < h_; ++y) {
        float* cur = err.data() + (y % 2) * (w_ + 2) * 3;
        float* nxt = err.data() + ((y + 1) % 2) * (w_ + 2) * 3;
        std::fill(nxt, nxt + (w_ + 2) * 3, 0.f);
        for (int x = 0; x < w_; ++x) {
            const uint8_t* p = img.at(x, y);
            if (p[3] < 128 && anyTransparent) { idx[(size_t)y * w_ + x] = (uint8_t)transIndex; continue; }
            float a = std::max(1, (int)p[3]) / 255.f;
            float r = p[0] / a + (dither ? cur[(x + 1) * 3] : 0), g = p[1] / a + (dither ? cur[(x + 1) * 3 + 1] : 0), b = p[2] / a + (dither ? cur[(x + 1) * 3 + 2] : 0);
            r = clampv(r, 0.f, 255.f); g = clampv(g, 0.f, 255.f); b = clampv(b, 0.f, 255.f);
            int key = ((int)r >> 3) << 10 | ((int)g >> 3) << 5 | ((int)b >> 3);
            int k = cache[key];
            if (k < 0) k = cache[key] = nearest(r, g, b);
            idx[(size_t)y * w_ + x] = (uint8_t)k;
            if (dither) {
                float er = r - pal[k][0], eg = g - pal[k][1], eb = b - pal[k][2];
                float* c = cur + (x + 2) * 3;
                c[0] += er * 7 / 16; c[1] += eg * 7 / 16; c[2] += eb * 7 / 16;
                float* d = nxt + x * 3;
                d[0] += er * 3 / 16; d[1] += eg * 3 / 16; d[2] += eb * 3 / 16;
                d[3] += er * 5 / 16; d[4] += eg * 5 / 16; d[5] += eb * 5 / 16;
                d[6] += er / 16; d[7] += eg / 16; d[8] += eb / 16;
            }
        }
    }
    uint8_t gce[8] = {0x21, 0xF9, 0x04, (uint8_t)(anyTransparent ? 0x09 : 0x04), (uint8_t)(delayCs & 255), (uint8_t)(delayCs >> 8),
                      (uint8_t)(transIndex < 0 ? 0 : transIndex), 0};
    std::fwrite(gce, 1, 8, f_);
    uint8_t desc[10] = {0x2C, 0, 0, 0, 0, (uint8_t)(w_ & 255), (uint8_t)(w_ >> 8), (uint8_t)(h_ & 255), (uint8_t)(h_ >> 8), 0x87};
    std::fwrite(desc, 1, 10, f_);
    for (auto& c : pal) std::fwrite(c.data(), 1, 3, f_);
    uint8_t minCode = 8;
    std::fputc(minCode, f_);
    auto data = lzw(idx, minCode);
    for (size_t i = 0; i < data.size(); i += 255) {
        int len = (int)std::min<size_t>(255, data.size() - i);
        std::fputc(len, f_);
        std::fwrite(data.data() + i, 1, len, f_);
    }
    std::fputc(0, f_);
    ++frames_;
    return std::ferror(f_) == 0;
}

bool GifWriter::close() {
    if (!f_) return false;
    std::fputc(0x3B, f_);
    bool ok = std::ferror(f_) == 0;
    std::fclose(f_);
    f_ = nullptr;
    return ok;
}

bool inspectGif(const std::string& path, int& w, int& h, int& frames) {
    auto d = readFileBytes(path);
    if (d.size() < 13 || std::memcmp(d.data(), "GIF8", 4) != 0) return false;
    w = d[6] | (d[7] << 8);
    h = d[8] | (d[9] << 8);
    frames = 0;
    size_t p = 13;
    if (d[10] & 0x80) p += 3 * (1 << ((d[10] & 7) + 1));
    while (p < d.size()) {
        uint8_t b = d[p++];
        if (b == 0x3B) return true;
        if (b == 0x21) {
            ++p;  // label
            while (p < d.size() && d[p]) p += d[p] + 1;
            ++p;
        } else if (b == 0x2C) {
            if (p + 9 > d.size()) return false;
            uint8_t flags = d[p + 8];
            p += 9;
            if (flags & 0x80) p += 3 * (1 << ((flags & 7) + 1));
            ++p;  // min code size
            while (p < d.size() && d[p]) p += d[p] + 1;
            ++p;
            ++frames;
        } else return false;
    }
    return false;
}

}  // namespace mf
