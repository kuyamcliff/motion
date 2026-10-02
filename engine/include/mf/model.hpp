// Project schema, factories and the edit-operation dispatcher.
// Every user-visible edit is expressed as an "op" JSON object and applied through applyOp(),
// which returns the next immutable document. Document::commit() turns it into an undoable transaction.
#pragma once
#include "common.hpp"
#include "property.hpp"

namespace mf {

// ---------------------------------------------------------------- factories
json newProject(const std::string& name, int width, int height, double fps, double duration);
json newComp(const std::string& id, const std::string& name, int width, int height, double fps, double duration);
json defaultTransform(double ax, double ay, double px, double py);
// kind: solid,text,shape,null,adjustment,camera,light,particles,model3d,captions,image,video,audio,precomp
json makeLayer(const std::string& kind, const std::string& id, const json& comp, const json& project, const json& opts);
json defaultCaptionStyle();
std::vector<std::string> layerKinds();

// ---------------------------------------------------------------- lookup
const json* findComp(const json& doc, const std::string& id);
json* findCompMut(json& doc, const std::string& id);
const json* findLayer(const json& comp, const std::string& id);
json* findLayerMut(json& comp, const std::string& id);
int layerIndex(const json& comp, const std::string& id);
const json* findAsset(const json& doc, const std::string& id);
json* findAssetMut(json& doc, const std::string& id);
const json* activeComp(const json& doc);
std::string newId(json& doc, const char* prefix);

// Resolve "transform.position" / "effects.E3.params.radius" / "text.size" inside a layer.
// Array segments match elements by "id" (or numeric index).
const json* resolvePath(const json& root, const std::string& path);
json* resolvePathMut(json& root, const std::string& path, bool create = false);

// Layer time helpers.
double layerLocalTime(const json& layer, double compTime);  // keyframe space
double layerSourceTime(const json& layer, double compTime);  // media time (speed/reverse/remap/freeze)
bool layerActiveAt(const json& layer, double compTime);
double compFps(const json& comp);
double snapToFrame(double t, double fps);

// Collect all keyframe times (comp time) for a layer (or all layers when layer==nullptr).
std::vector<double> keyframeTimes(const json& comp, const json* layer);
// Edit points: layer in/out points and markers.
std::vector<double> editPoints(const json& comp);

// ---------------------------------------------------------------- operations
struct OpResult {
    std::string label;
    json data = json::object();  // created ids etc.
};

// Apply one op; throws EditError with an actionable message on invalid input.
json applyOp(const json& doc, const json& op, OpResult* result = nullptr);
// Human readable label for an op (used for history).
std::string opLabel(const json& op);
// All op names (for scripting docs / command palette).
std::vector<std::string> opNames();

// Validate overall document structure; returns list of problems (empty == valid).
std::vector<std::string> validateProject(const json& doc);

// Missing-media / font diagnostics for the project inspector.
struct Diagnostic {
    std::string severity;  // error|warning|info
    std::string code;      // MISSING_ASSET, MISSING_FONT, HIGH_COST, EXPRESSION_ERROR...
    std::string message;
    std::string target;    // id
    std::string recommendation;
};
std::vector<Diagnostic> diagnoseProject(const json& doc, const std::vector<std::string>& availableFonts,
                                        std::function<bool(const json& asset)> assetAvailable);

}  // namespace mf
