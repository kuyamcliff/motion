// Real (software) 3D: meshes, primitives, OBJ/glTF loading, extrusion, z-buffered PBR-ish shading with shadows.
#pragma once
#include "common.hpp"
#include "raster.hpp"

namespace mf {

struct Material {
    Color baseColor{0.8f, 0.8f, 0.8f, 1};
    float metallic = 0, roughness = 0.5f;
    Color emission{0, 0, 0, 1};
    float emissionStrength = 0;
    float opacity = 1;
    std::shared_ptr<Image> baseColorTex;  // premultiplied
};

struct Mesh {
    std::vector<Vec3> positions, normals;
    std::vector<Vec2> uvs;
    std::vector<uint32_t> indices;  // triangles
    std::vector<int> triMaterial;   // per triangle material index
    std::vector<Material> materials;
    void computeNormals();
    void bounds(Vec3& mn, Vec3& mx) const;
    void append(const Mesh& o, const Mat4& xf);
    size_t triangleCount() const { return indices.size() / 3; }
};

Mesh makePrimitive(const std::string& kind, double size);  // cube, sphere, plane, torus, cylinder, cone
bool loadObj(const std::string& path, Mesh& out, std::string& err);
bool loadGltf(const std::string& path, Mesh& out, std::string& err);  // .gltf (embedded/external buffers) or .glb
bool loadModelFile(const std::string& path, Mesh& out, std::string& err);

// Triangulate polygons with holes (non-zero orientation): outer contours and holes are auto-classified.
std::vector<std::array<Vec2, 3>> triangulate(const Polys& polys);
// Extrude flattened outlines (in XY, y down) into a mesh of given depth centered on z=0..depth.
Mesh extrudePolys(const Polys& polys, double depth, const Material& front, const Material& side);

struct Light3D {
    enum class Kind { Ambient, Point, Spot, Directional } kind = Kind::Point;
    Vec3 position, direction{0, 0, 1};
    Color color{1, 1, 1, 1};
    double intensity = 1;
    double coneCos = 0.5, featherCos = 0.7;
    double radius = 0;  // falloff radius (0 = none)
    bool castShadows = false;
    double shadowDarkness = 0.6;
};

struct Camera3D {
    Mat4 view;    // world -> camera
    double zoom;  // focal length in px
    int width, height;  // viewport (render px)
    double near_ = 1, far_ = 20000;
    double scale = 1;   // render scale applied to projected coords
    Vec3 position;
    // Project world point to screen (render pixels). Returns false if behind camera.
    bool project(const Vec3& world, Vec2& screen, double& depth) const;
};

// Render a mesh into dst using z-buffer `zbuf` (size w*h, camera depth). Lights in world space.
struct MeshRenderOptions {
    bool wireframe = false;
    bool receiveShadows = true;
    float opacity = 1;
    const std::vector<float>* shadowMap = nullptr;  // optional directional shadow map
    Mat4 shadowViewProj;
    int shadowSize = 0;
};
void renderMesh(Image& dst, std::vector<float>& zbuf, const Mesh& mesh, const Mat4& model, const Camera3D& cam,
                const std::vector<Light3D>& lights, const MeshRenderOptions& opt);

// Build an orthographic shadow map from a directional light covering the given world-space bounds.
void buildShadowMap(const std::vector<std::pair<const Mesh*, Mat4>>& casters, const Light3D& light, const Vec3& center, double radius,
                    int size, std::vector<float>& depth, Mat4& viewProj);

Mat4 lookAt(const Vec3& eye, const Vec3& target, const Vec3& up);

}  // namespace mf
