// Procedural motion behaviours (shake, bounce, orbit ...) applied on top of keyframed transforms.
#pragma once
#include "property.hpp"

namespace mf {

struct TransformOffsets {
    Vec3 position;        // added to position (px)
    Vec3 scaleMul{1, 1, 1};  // multiplies scale
    double rotation = 0;  // added degrees
    double opacityMul = 1;
};

// Evaluate all enabled behaviours of a layer at a composition time.
TransformOffsets evalBehaviors(const json& layer, const EvalContext& ctx);

// Bake behaviour `index` of `layer` into transform keyframes (one per frame across the layer) and remove it.
void bakeBehaviorToKeyframes(const json& comp, json& layer, int index, double fps);

}  // namespace mf
