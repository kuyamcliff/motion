// Animated GIF encoder (median-cut palette per frame, optional dithering, LZW) and PNG sequence writer.
#pragma once
#include <cstdio>

#include "common.hpp"

namespace mf {

class GifWriter {
   public:
    ~GifWriter();
    bool open(const std::string& path, int w, int h, int loop = 0);
    // delayCs: frame duration in 1/100 s. Image is premultiplied RGBA (alpha < 50% -> transparent).
    bool addFrame(const Image& img, int delayCs, bool dither = true);
    bool close();
    int frames() const { return frames_; }

   private:
    FILE* f_ = nullptr;
    int w_ = 0, h_ = 0, frames_ = 0;
};

// Decode-side validation helper: counts frames and reads logical screen size.
bool inspectGif(const std::string& path, int& w, int& h, int& frames);

}  // namespace mf
