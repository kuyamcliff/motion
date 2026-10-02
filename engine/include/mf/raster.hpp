// Vector paths, anti-aliased rasterization, stroking and paint.
#pragma once
#include "common.hpp"
#include "property.hpp"

namespace mf {

struct BezierVertex {
    Vec2 p, in, out;  // tangents relative to p
};
struct BezierPath {
    std::vector<BezierVertex> v;
    bool closed = true;
    static BezierPath fromValue(const Value& val);
    Value toValue() const;
    static BezierPath rect(double cx, double cy, double w, double h, double roundness = 0);
    static BezierPath ellipse(double cx, double cy, double w, double h);
    static BezierPath star(double cx, double cy, int points, double outer, double inner, double rotationDeg, bool polygon);
};

struct Contour {
    std::vector<Vec2> pts;
    bool closed = true;
};
using Polys = std::vector<Contour>;

void flattenInto(const BezierPath& p, double tolerance, Polys& out);
Polys flatten(const BezierPath& p, double tolerance);
void transformPolys(Polys& polys, const std::function<Vec2(const Vec2&)>& f);
double contourLength(const Contour& c);
double signedArea(const std::vector<Vec2>& pts);

enum class FillRule { NonZero, EvenOdd };
enum class LineCap { Butt, Round, Square };
enum class LineJoin { Miter, Round, Bevel };

// Coverage values in [0,1] over a rectangle of the target image.
struct Coverage {
    Rect r;
    std::vector<float> a;  // r.width()*r.height()
    bool empty() const { return r.empty(); }
    float at(int x, int y) const { return a[(size_t)(y - r.y0) * r.width() + (x - r.x0)]; }
};

// Rasterize closed polygons (open contours are implicitly closed) clipped to `clip`.
Coverage rasterize(const Polys& polys, const Rect& clip, FillRule rule = FillRule::NonZero);
Rect polysBounds(const Polys& polys);

// Stroke outline to fillable polygons (all positively oriented; fill with NonZero).
Polys strokePolys(const Polys& polys, double width, LineCap cap, LineJoin join, double miterLimit = 4.0);
Polys dashPolys(const Polys& polys, const std::vector<double>& dash, double offset);
// Trim each contour to [start,end] fraction (0..1) with offset (fraction, wraps on closed paths).
Polys trimPolys(const Polys& polys, double start, double end, double offset);
// Zig-zag / round-corners / twist path modifiers on flattened geometry.
Polys zigzagPolys(const Polys& polys, double size, int ridgesPerSegment, bool smooth);
Polys roundCornersPolys(const Polys& polys, double radius);
Polys offsetPolysApprox(const Polys& polys, double amount);

struct Paint {
    enum class Type { Solid, Linear, Radial, Conic, ImageFill } type = Type::Solid;
    Color color{1, 1, 1, 1};
    Vec2 p0, p1;                              // gradient points in paint space
    std::vector<std::pair<float, Color>> stops;  // sorted by position
    Mat3 inv;                                 // image pixel -> paint space
    float opacity = 1.0f;
    ImagePtr image;                           // for ImageFill (premultiplied)
    Color evalAt(double px, double py) const; // unpremultiplied color at image pixel center
};

// Composite paint * coverage over dst (source-over, premultiplied).
void fillCoverage(Image& dst, const Coverage& cov, const Paint& paint);
// Multiply dst alpha by coverage (used for masks/clips). Pixels outside cov.r become transparent if `clearOutside`.
void applyCoverageAsMask(Image& dst, const Coverage& cov, bool clearOutside);

// Separable box-blur approximation of gaussian on a coverage buffer (feathering).
void blurCoverage(Coverage& c, double radius);

}  // namespace mf
