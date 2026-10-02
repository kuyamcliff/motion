// Pixel operations: compositing with blend modes, warps, blurs, resampling.
#pragma once
#include "common.hpp"
#include "effects.hpp"

namespace mf {

// Composite src over dst with blend mode and opacity (both premultiplied), within rect.
void compositeImage(Image& dst, const Image& src, BlendMode mode, float opacity, const Rect& region);
// Same, with an optional per-pixel matte (alpha 0..1 from matte image's alpha or luma).
enum class MatteMode { None, Alpha, AlphaInverted, Luma, LumaInverted };
void applyTrackMatte(Image& img, const Image& matte, MatteMode mode, const Rect& region);

// Sample premultiplied image bilinearly (returns premultiplied RGBA 0..255 floats). Outside -> transparent.
void sampleBilinear(const Image& img, double x, double y, float out[4]);

// Warp source into destination using inverse mapping dst->src homography; `quality` 0 nearest, 1 bilinear.
// Returns affected destination rect.
Rect warpImage(Image& dst, const Image& src, const Mat3& dstToSrc, float opacity, const Rect& dstClip, int quality = 1,
               const Rect* srcRect = nullptr);

// Box-blur approximation of gaussian (3 passes) on premultiplied image within rect (rect may expand).
void gaussianBlur(Image& img, double radius, Rect& bounds, bool horizontal = true, bool vertical = true);
// Directional blur (angle radians, length px).
void directionalBlur(Image& img, double angle, double length, Rect& bounds);

// Resize image (bilinear / area average when shrinking).
Image resizeImage(const Image& src, int w, int h);
// Copy subrect.
Image cropImage(const Image& src, const Rect& r);

// Convert straight RGBA8 -> premultiplied in place, and back.
void premultiply(Image& img);
void unpremultiply(Image& img);

// Planar / semi-planar YUV 4:2:0 to premultiplied RGBA (BT.601 or BT.709, limited or full range).
void yuv420ToRgba(const uint8_t* y, int yStride, const uint8_t* u, const uint8_t* v, int uvStride, int uvPixelStride, int w, int h,
                  bool bt709, bool fullRange, Image& out);

// Image difference metrics (for golden tests).
double meanAbsDiff(const Image& a, const Image& b);
double psnr(const Image& a, const Image& b);
uint64_t imageHash(const Image& img);

// Encode/decode helpers.
bool savePng(const Image& img, const std::string& path);  // unpremultiplies
bool loadImageFile(const std::string& path, Image& out, std::string* err = nullptr);
bool loadImageMemory(const uint8_t* data, size_t size, Image& out, std::string* err = nullptr);

}  // namespace mf
