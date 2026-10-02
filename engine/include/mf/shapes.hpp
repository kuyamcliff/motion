// Shape layer geometry (primitives + modifiers) and the particle system.
#pragma once
#include "property.hpp"
#include "raster.hpp"

namespace mf {

struct ShapeGeometry {
    // One entry per repeater copy; each holds flattened contours in layer space.
    struct Copy {
        // Fill items in stacking order with their merge op ("add" | "subtract" | "intersect");
        // combined exactly at raster time via coverage operations.
        std::vector<std::pair<std::string, Polys>> fillItems;
        Polys stroke;  // stroke centerlines (after trim/dash)
        Mat4 xf;       // copy transform (layer space)
        double opacity = 1;
    };
    std::vector<Copy> copies;
};

// Build shape geometry at time ctx (layer space). `tolerance` controls bezier flattening (layer px).
ShapeGeometry buildShapeGeometry(const json& shape, const EvalContext& ctx, double tolerance);
// Flattened outline of a single shape item (layer space).
Polys shapeItemPolys(const json& item, const EvalContext& ctx, double tolerance);

// ---------------------------------------------------------------- particles
struct Particle {
    Vec2 pos, prevPos;  // layer space
    double size = 1, opacity = 1, rotation = 0;
    Color color;
    double age01 = 0;
};
// Deterministic analytic particle simulation in layer space (emitter at layer origin).
std::vector<Particle> simulateParticles(const json& particles, const EvalContext& ctx, double layerAge);

}  // namespace mf
