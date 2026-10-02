#include "mf/three_d.hpp"

#include <map>
#include <sstream>

#include "mf/imageops.hpp"
#include "mf/threadpool.hpp"

namespace mf {

Mat4 lookAt(const Vec3& eye, const Vec3& target, const Vec3& down) {
    Vec3 f = (target - eye).normalized();
    if (f.length() < 1e-9) f = {0, 0, 1};
    Vec3 x = down.cross(f).normalized();
    if (x.length() < 1e-9) x = Vec3(0, 0, 1).cross(f).normalized();
    Vec3 y = f.cross(x);
    Mat4 m;
    m.at(0, 0) = x.x; m.at(0, 1) = x.y; m.at(0, 2) = x.z; m.at(0, 3) = -x.dot(eye);
    m.at(1, 0) = y.x; m.at(1, 1) = y.y; m.at(1, 2) = y.z; m.at(1, 3) = -y.dot(eye);
    m.at(2, 0) = f.x; m.at(2, 1) = f.y; m.at(2, 2) = f.z; m.at(2, 3) = -f.dot(eye);
    return m;
}

bool Camera3D::project(const Vec3& world, Vec2& screen, double& depth) const {
    Vec3 c = view.transformPoint(world);
    depth = c.z;
    if (c.z <= near_) return false;
    screen = {(width / 2.0) + (zoom * c.x / c.z) * scale, (height / 2.0) + (zoom * c.y / c.z) * scale};
    return true;
}

// ====================================================================== mesh utils
void Mesh::computeNormals() {
    normals.assign(positions.size(), Vec3());
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        Vec3 a = positions[indices[i]], b = positions[indices[i + 1]], c = positions[indices[i + 2]];
        Vec3 n = (b - a).cross(c - a);
        for (int k = 0; k < 3; ++k) normals[indices[i + k]] += n;
    }
    for (auto& n : normals) n = n.normalized();
}

void Mesh::bounds(Vec3& mn, Vec3& mx) const {
    mn = {1e30, 1e30, 1e30};
    mx = {-1e30, -1e30, -1e30};
    for (auto& p : positions) {
        mn = {std::min(mn.x, p.x), std::min(mn.y, p.y), std::min(mn.z, p.z)};
        mx = {std::max(mx.x, p.x), std::max(mx.y, p.y), std::max(mx.z, p.z)};
    }
    if (positions.empty()) mn = mx = Vec3();
}

void Mesh::append(const Mesh& o, const Mat4& xf) {
    uint32_t base = (uint32_t)positions.size();
    int mbase = (int)materials.size();
    for (auto& p : o.positions) positions.push_back(xf.transformPoint(p));
    for (size_t i = 0; i < o.positions.size(); ++i) {
        normals.push_back(i < o.normals.size() ? xf.transformDir(o.normals[i]).normalized() : Vec3(0, 0, -1));
        uvs.push_back(i < o.uvs.size() ? o.uvs[i] : Vec2());
    }
    for (auto idx : o.indices) indices.push_back(base + idx);
    for (size_t t = 0; t < o.triangleCount(); ++t) triMaterial.push_back(mbase + (t < o.triMaterial.size() ? o.triMaterial[t] : 0));
    for (auto& m : o.materials) materials.push_back(m);
    if (o.materials.empty()) materials.push_back(Material());
}

// ====================================================================== primitives
Mesh makePrimitive(const std::string& kind, double size) {
    Mesh m;
    double h = size / 2;
    auto quad = [&](Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 n) {
        uint32_t i = (uint32_t)m.positions.size();
        m.positions.insert(m.positions.end(), {a, b, c, d});
        m.normals.insert(m.normals.end(), {n, n, n, n});
        m.uvs.insert(m.uvs.end(), {Vec2(0, 0), Vec2(1, 0), Vec2(1, 1), Vec2(0, 1)});
        m.indices.insert(m.indices.end(), {i, i + 1, i + 2, i, i + 2, i + 3});
    };
    if (kind == "cube") {
        quad({-h, -h, -h}, {h, -h, -h}, {h, h, -h}, {-h, h, -h}, {0, 0, -1});
        quad({h, -h, h}, {-h, -h, h}, {-h, h, h}, {h, h, h}, {0, 0, 1});
        quad({-h, -h, h}, {-h, -h, -h}, {-h, h, -h}, {-h, h, h}, {-1, 0, 0});
        quad({h, -h, -h}, {h, -h, h}, {h, h, h}, {h, h, -h}, {1, 0, 0});
        quad({-h, -h, h}, {h, -h, h}, {h, -h, -h}, {-h, -h, -h}, {0, -1, 0});
        quad({-h, h, -h}, {h, h, -h}, {h, h, h}, {-h, h, h}, {0, 1, 0});
    } else if (kind == "plane") {
        quad({-h, -h, 0}, {h, -h, 0}, {h, h, 0}, {-h, h, 0}, {0, 0, -1});
    } else if (kind == "sphere") {
        int st = 32, sl = 48;
        for (int i = 0; i <= st; ++i)
            for (int j = 0; j <= sl; ++j) {
                double th = kPi * i / st, ph = 2 * kPi * j / sl;
                Vec3 n{std::sin(th) * std::cos(ph), -std::cos(th), std::sin(th) * std::sin(ph)};
                m.positions.push_back(n * h);
                m.normals.push_back(n);
                m.uvs.push_back({(double)j / sl, (double)i / st});
            }
        for (int i = 0; i < st; ++i)
            for (int j = 0; j < sl; ++j) {
                uint32_t a = i * (sl + 1) + j, b = a + sl + 1;
                m.indices.insert(m.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
            }
    } else if (kind == "torus") {
        double R = h * 0.7, r = h * 0.3;
        int a = 48, b = 24;
        for (int i = 0; i <= a; ++i)
            for (int j = 0; j <= b; ++j) {
                double u = 2 * kPi * i / a, v = 2 * kPi * j / b;
                Vec3 c{std::cos(u) * R, 0, std::sin(u) * R};
                Vec3 n{std::cos(u) * std::cos(v), std::sin(v), std::sin(u) * std::cos(v)};
                m.positions.push_back(c + n * r);
                m.normals.push_back(n);
                m.uvs.push_back({(double)i / a, (double)j / b});
            }
        for (int i = 0; i < a; ++i)
            for (int j = 0; j < b; ++j) {
                uint32_t p = i * (b + 1) + j, q = p + b + 1;
                m.indices.insert(m.indices.end(), {p, p + 1, q, p + 1, q + 1, q});
            }
    } else if (kind == "cylinder" || kind == "cone") {
        int sl = 48;
        bool cone = kind == "cone";
        for (int j = 0; j <= sl; ++j) {
            double ph = 2 * kPi * j / sl;
            double c = std::cos(ph), s = std::sin(ph);
            double topR = cone ? 0 : h;
            Vec3 n = cone ? Vec3(c, -0.5, s).normalized() : Vec3(c, 0, s);
            m.positions.push_back({c * topR, -h, s * topR});
            m.normals.push_back(n);
            m.uvs.push_back({(double)j / sl, 0});
            m.positions.push_back({c * h, h, s * h});
            m.normals.push_back(n);
            m.uvs.push_back({(double)j / sl, 1});
        }
        for (int j = 0; j < sl; ++j) {
            uint32_t a = j * 2, b = a + 1, c = a + 2, d = a + 3;
            m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
        }
        auto cap = [&](double y, Vec3 n, bool flip) {
            uint32_t center = (uint32_t)m.positions.size();
            m.positions.push_back({0, y, 0});
            m.normals.push_back(n);
            m.uvs.push_back({0.5, 0.5});
            for (int j = 0; j <= sl; ++j) {
                double ph = 2 * kPi * j / sl;
                m.positions.push_back({std::cos(ph) * h, y, std::sin(ph) * h});
                m.normals.push_back(n);
                m.uvs.push_back({0.5 + std::cos(ph) / 2, 0.5 + std::sin(ph) / 2});
            }
            for (int j = 0; j < sl; ++j) {
                uint32_t a = center + 1 + j;
                if (flip) m.indices.insert(m.indices.end(), {center, a + 1, a});
                else m.indices.insert(m.indices.end(), {center, a, a + 1});
            }
        };
        cap(h, {0, 1, 0}, false);
        if (!cone) cap(-h, {0, -1, 0}, true);
    } else {
        return makePrimitive("cube", size);
    }
    m.materials.push_back(Material());
    m.triMaterial.assign(m.triangleCount(), 0);
    return m;
}

// ====================================================================== OBJ
static void loadMtl(const std::string& path, std::map<std::string, Material>& out) {
    bool ok;
    std::string text = readFileText(path, &ok);
    if (!ok) return;
    std::istringstream in(text);
    std::string line, cur;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string tok;
        ls >> tok;
        if (tok == "newmtl") { ls >> cur; out[cur] = Material(); }
        else if (cur.empty()) continue;
        else if (tok == "Kd") { float r, g, b; ls >> r >> g >> b; out[cur].baseColor = Color(r, g, b, 1); }
        else if (tok == "Ke") { float r, g, b; ls >> r >> g >> b; out[cur].emission = Color(r, g, b, 1); out[cur].emissionStrength = 1; }
        else if (tok == "d") { float d; ls >> d; out[cur].opacity = d; }
        else if (tok == "Ns") { float ns; ls >> ns; out[cur].roughness = clampv(1.f - std::sqrt(ns / 1000.f), 0.05f, 1.f); }
        else if (tok == "map_Kd") {
            std::string f;
            std::getline(ls, f);
            while (!f.empty() && (f[0] == ' ' || f[0] == '\t')) f.erase(0, 1);
            auto img = std::make_shared<Image>();
            if (loadImageFile(pathJoin(pathDirname(path), f), *img)) out[cur].baseColorTex = img;
        }
    }
}

bool loadObj(const std::string& path, Mesh& out, std::string& err) {
    bool ok;
    std::string text = readFileText(path, &ok);
    if (!ok) { err = "Cannot read OBJ file."; return false; }
    std::vector<Vec3> vs, vns;
    std::vector<Vec2> vts;
    std::map<std::string, Material> mtls;
    std::map<std::string, int> matIndex;
    int curMat = 0;
    out = Mesh();
    out.materials.push_back(Material());
    std::map<std::tuple<int, int, int>, uint32_t> cache;
    std::istringstream in(text);
    std::string line;
    bool anyNormals = false;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string tok;
        ls >> tok;
        if (tok == "v") { double x, y, z; ls >> x >> y >> z; vs.push_back({x, -y, z}); }  // OBJ is y-up; engine is y-down
        else if (tok == "vn") { double x, y, z; ls >> x >> y >> z; vns.push_back(Vec3(x, -y, z).normalized()); anyNormals = true; }
        else if (tok == "vt") { double u, v; ls >> u >> v; vts.push_back({u, 1 - v}); }
        else if (tok == "mtllib") { std::string f; ls >> f; loadMtl(pathJoin(pathDirname(path), f), mtls); }
        else if (tok == "usemtl") {
            std::string n; ls >> n;
            if (!matIndex.count(n)) { matIndex[n] = (int)out.materials.size(); out.materials.push_back(mtls.count(n) ? mtls[n] : Material()); }
            curMat = matIndex[n];
        } else if (tok == "f") {
            std::vector<uint32_t> poly;
            std::string vert;
            while (ls >> vert) {
                int vi = 0, ti = 0, ni = 0;
                std::sscanf(vert.c_str(), "%d/%d/%d", &vi, &ti, &ni);
                if (vert.find("//") != std::string::npos) { std::sscanf(vert.c_str(), "%d//%d", &vi, &ni); ti = 0; }
                if (vi < 0) vi = (int)vs.size() + vi + 1;
                if (ti < 0) ti = (int)vts.size() + ti + 1;
                if (ni < 0) ni = (int)vns.size() + ni + 1;
                auto key = std::make_tuple(vi, ti, ni);
                auto it = cache.find(key);
                if (it != cache.end()) { poly.push_back(it->second); continue; }
                if (vi < 1 || vi > (int)vs.size()) { err = "OBJ face references a missing vertex."; return false; }
                uint32_t idx = (uint32_t)out.positions.size();
                out.positions.push_back(vs[vi - 1]);
                out.uvs.push_back(ti >= 1 && ti <= (int)vts.size() ? vts[ti - 1] : Vec2());
                out.normals.push_back(ni >= 1 && ni <= (int)vns.size() ? vns[ni - 1] : Vec3());
                cache[key] = idx;
                poly.push_back(idx);
            }
            for (size_t k = 1; k + 1 < poly.size(); ++k) {
                // Flip winding because of the y flip.
                out.indices.insert(out.indices.end(), {poly[0], poly[k + 1], poly[k]});
                out.triMaterial.push_back(curMat);
            }
        }
    }
    if (out.indices.empty()) { err = "OBJ contains no faces."; return false; }
    if (!anyNormals) out.computeNormals();
    return true;
}

// ====================================================================== glTF
static std::vector<uint8_t> base64Decode(const std::string& in) {
    static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<uint8_t> out;
    int val = 0, bits = -8;
    for (char c : in) {
        auto p = chars.find(c);
        if (p == std::string::npos) continue;
        val = (val << 6) + (int)p;
        bits += 6;
        if (bits >= 0) { out.push_back((uint8_t)((val >> bits) & 0xFF)); bits -= 8; }
    }
    return out;
}

bool loadGltf(const std::string& path, Mesh& out, std::string& err) {
    bool ok;
    auto bytes = readFileBytes(path, &ok);
    if (!ok) { err = "Cannot read glTF file."; return false; }
    json g;
    std::vector<uint8_t> glbBin;
    try {
        if (bytes.size() >= 12 && std::memcmp(bytes.data(), "glTF", 4) == 0) {
            size_t off = 12;
            while (off + 8 <= bytes.size()) {
                uint32_t len = bytes[off] | (bytes[off + 1] << 8) | (bytes[off + 2] << 16) | ((uint32_t)bytes[off + 3] << 24);
                uint32_t type = bytes[off + 4] | (bytes[off + 5] << 8) | (bytes[off + 6] << 16) | ((uint32_t)bytes[off + 7] << 24);
                if (off + 8 + len > bytes.size()) break;
                if (type == 0x4E4F534A) g = json::parse(bytes.begin() + off + 8, bytes.begin() + off + 8 + len);
                else if (type == 0x004E4942) glbBin.assign(bytes.begin() + off + 8, bytes.begin() + off + 8 + len);
                off += 8 + len;
            }
        } else {
            g = json::parse(bytes.begin(), bytes.end());
        }
    } catch (std::exception& e) {
        err = std::string("Invalid glTF JSON: ") + e.what();
        return false;
    }
    std::vector<std::vector<uint8_t>> buffers;
    for (auto& b : g.value("buffers", json::array())) {
        if (b.contains("uri")) {
            std::string uri = b["uri"];
            if (uri.rfind("data:", 0) == 0) buffers.push_back(base64Decode(uri.substr(uri.find(',') + 1)));
            else buffers.push_back(readFileBytes(pathJoin(pathDirname(path), uri)));
        } else buffers.push_back(glbBin);
    }
    auto viewData = [&](int viewIdx, size_t& len, size_t& stride) -> const uint8_t* {
        const json& bv = g["bufferViews"][viewIdx];
        int bi = bv.value("buffer", 0);
        size_t off = bv.value("byteOffset", 0);
        len = bv.value("byteLength", 0);
        stride = bv.value("byteStride", 0);
        if (bi >= (int)buffers.size() || off + len > buffers[bi].size()) return nullptr;
        return buffers[bi].data() + off;
    };
    auto readAccessor = [&](int ai, int comps, std::vector<double>& outv) -> bool {
        const json& a = g["accessors"][ai];
        if (!a.contains("bufferView")) return false;
        size_t len, stride;
        const uint8_t* base = viewData(a["bufferView"], len, stride);
        if (!base) return false;
        int ct = a.value("componentType", 5126);
        size_t count = a.value("count", 0);
        size_t off = a.value("byteOffset", 0);
        int csize = (ct == 5126 || ct == 5125) ? 4 : (ct == 5123 || ct == 5122) ? 2 : 1;
        if (stride == 0) stride = csize * comps;
        bool norm = a.value("normalized", false);
        outv.resize(count * comps);
        for (size_t i = 0; i < count; ++i) {
            const uint8_t* p = base + off + i * stride;
            if ((size_t)(p - base) + csize * comps > len) return false;
            for (int c = 0; c < comps; ++c) {
                const uint8_t* q = p + c * csize;
                double v;
                switch (ct) {
                    case 5126: { float f; std::memcpy(&f, q, 4); v = f; break; }
                    case 5125: { uint32_t u; std::memcpy(&u, q, 4); v = u; break; }
                    case 5123: { uint16_t u; std::memcpy(&u, q, 2); v = norm ? u / 65535.0 : u; break; }
                    case 5122: { int16_t u; std::memcpy(&u, q, 2); v = norm ? u / 32767.0 : u; break; }
                    case 5121: v = norm ? *q / 255.0 : *q; break;
                    default: v = *(const int8_t*)q; break;
                }
                outv[i * comps + c] = v;
            }
        }
        return true;
    };
    // Materials.
    std::vector<Material> mats;
    for (auto& m : g.value("materials", json::array())) {
        Material mat;
        const json& pbr = m.value("pbrMetallicRoughness", json::object());
        if (pbr.contains("baseColorFactor")) mat.baseColor = parseColor(pbr["baseColorFactor"]);
        mat.metallic = pbr.value("metallicFactor", 1.0f);
        mat.roughness = pbr.value("roughnessFactor", 1.0f);
        if (m.contains("emissiveFactor")) { mat.emission = parseColor(m["emissiveFactor"]); mat.emissionStrength = 1; }
        if (pbr.contains("baseColorTexture")) {
            int ti = pbr["baseColorTexture"].value("index", -1);
            if (ti >= 0 && g.contains("textures") && ti < (int)g["textures"].size()) {
                int src = g["textures"][ti].value("source", -1);
                if (src >= 0 && src < (int)g["images"].size()) {
                    const json& im = g["images"][src];
                    auto img = std::make_shared<Image>();
                    bool loaded = false;
                    if (im.contains("bufferView")) {
                        size_t len, stride;
                        const uint8_t* d = viewData(im["bufferView"], len, stride);
                        if (d) loaded = loadImageMemory(d, len, *img);
                    } else if (im.contains("uri")) {
                        std::string uri = im["uri"];
                        if (uri.rfind("data:", 0) == 0) {
                            auto dec = base64Decode(uri.substr(uri.find(',') + 1));
                            loaded = loadImageMemory(dec.data(), dec.size(), *img);
                        } else loaded = loadImageFile(pathJoin(pathDirname(path), uri), *img);
                    }
                    if (loaded) mat.baseColorTex = img;
                }
            }
        }
        mats.push_back(mat);
    }
    out = Mesh();
    auto nodeMatrix = [&](const json& n) {
        Mat4 m;
        if (n.contains("matrix")) {
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r) m.at(r, c) = n["matrix"][c * 4 + r].get<double>();
            return m;
        }
        Mat4 T, R, S;
        if (n.contains("translation")) T = Mat4::translate(n["translation"][0], n["translation"][1], n["translation"][2]);
        if (n.contains("scale")) S = Mat4::scale(n["scale"][0], n["scale"][1], n["scale"][2]);
        if (n.contains("rotation")) {
            double x = n["rotation"][0], y = n["rotation"][1], z = n["rotation"][2], w = n["rotation"][3];
            R.at(0, 0) = 1 - 2 * (y * y + z * z); R.at(0, 1) = 2 * (x * y - z * w); R.at(0, 2) = 2 * (x * z + y * w);
            R.at(1, 0) = 2 * (x * y + z * w); R.at(1, 1) = 1 - 2 * (x * x + z * z); R.at(1, 2) = 2 * (y * z - x * w);
            R.at(2, 0) = 2 * (x * z - y * w); R.at(2, 1) = 2 * (y * z + x * w); R.at(2, 2) = 1 - 2 * (x * x + y * y);
        }
        return T * R * S;
    };
    std::function<void(int, const Mat4&)> visit = [&](int ni, const Mat4& parent) {
        const json& n = g["nodes"][ni];
        Mat4 world = parent * nodeMatrix(n);
        if (n.contains("mesh")) {
            const json& mesh = g["meshes"][n["mesh"].get<int>()];
            for (auto& prim : mesh.value("primitives", json::array())) {
                if (prim.value("mode", 4) != 4) continue;
                const json& at = prim["attributes"];
                if (!at.contains("POSITION")) continue;
                std::vector<double> pos, nrm, uv, idx;
                if (!readAccessor(at["POSITION"], 3, pos)) continue;
                if (at.contains("NORMAL")) readAccessor(at["NORMAL"], 3, nrm);
                if (at.contains("TEXCOORD_0")) readAccessor(at["TEXCOORD_0"], 2, uv);
                Mesh pm;
                size_t vc = pos.size() / 3;
                for (size_t i = 0; i < vc; ++i) {
                    pm.positions.push_back({pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]});
                    if (nrm.size() >= (i + 1) * 3) pm.normals.push_back({nrm[i * 3], nrm[i * 3 + 1], nrm[i * 3 + 2]});
                    if (uv.size() >= (i + 1) * 2) pm.uvs.push_back({uv[i * 2], uv[i * 2 + 1]});
                }
                if (prim.contains("indices") && readAccessor(prim["indices"], 1, idx)) {
                    for (double v : idx) pm.indices.push_back((uint32_t)v);
                } else {
                    for (size_t i = 0; i < vc; ++i) pm.indices.push_back((uint32_t)i);
                }
                if (pm.normals.size() != pm.positions.size()) pm.computeNormals();
                int mi = prim.value("material", -1);
                pm.materials.push_back(mi >= 0 && mi < (int)mats.size() ? mats[mi] : Material());
                pm.triMaterial.assign(pm.triangleCount(), 0);
                // glTF is y-up, right-handed: convert to engine (y-down) with flip of y; reverse winding.
                Mat4 flip = Mat4::scale(1, -1, 1);
                Mesh tmp;
                tmp.append(pm, flip * world);
                for (size_t t = 0; t + 2 < tmp.indices.size(); t += 3) std::swap(tmp.indices[t + 1], tmp.indices[t + 2]);
                out.append(tmp, Mat4());
            }
        }
        for (auto& c : n.value("children", json::array())) visit(c.get<int>(), world);
    };
    int scene = g.value("scene", 0);
    if (g.contains("scenes") && scene < (int)g["scenes"].size()) {
        for (auto& n : g["scenes"][scene].value("nodes", json::array())) visit(n.get<int>(), Mat4());
    } else if (g.contains("nodes")) {
        for (size_t i = 0; i < g["nodes"].size(); ++i) visit((int)i, Mat4());
    }
    if (out.indices.empty()) { err = "glTF contains no triangle meshes."; return false; }
    return true;
}

bool loadModelFile(const std::string& path, Mesh& out, std::string& err) {
    std::string ext = pathExtensionLower(path);
    if (ext == "obj") return loadObj(path, out, err);
    if (ext == "gltf" || ext == "glb") return loadGltf(path, out, err);
    if (ext == "fbx") { err = "FBX is a proprietary format and is not supported. Convert to glTF/GLB or OBJ."; return false; }
    err = "Unsupported 3D model format '." + ext + "'. Use GLB, glTF or OBJ.";
    return false;
}

// ====================================================================== triangulation
static bool pointInPoly(const std::vector<Vec2>& poly, const Vec2& p) {
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        if (((poly[i].y > p.y) != (poly[j].y > p.y)) &&
            (p.x < (poly[j].x - poly[i].x) * (p.y - poly[i].y) / (poly[j].y - poly[i].y + 1e-300) + poly[i].x))
            in = !in;
    }
    return in;
}

static bool pointInTri(const Vec2& p, const Vec2& a, const Vec2& b, const Vec2& c) {
    double d1 = (p - a).cross(b - a), d2 = (p - b).cross(c - b), d3 = (p - c).cross(a - c);
    bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}

static void earClip(std::vector<Vec2> poly, std::vector<std::array<Vec2, 3>>& out) {
    // Expect positive orientation.
    if (signedArea(poly) < 0) std::reverse(poly.begin(), poly.end());
    std::vector<int> idx(poly.size());
    for (size_t i = 0; i < poly.size(); ++i) idx[i] = (int)i;
    int guard = 0;
    while (idx.size() > 3 && guard++ < 100000) {
        bool clipped = false;
        size_t n = idx.size();
        for (size_t i = 0; i < n; ++i) {
            const Vec2& a = poly[idx[(i + n - 1) % n]];
            const Vec2& b = poly[idx[i]];
            const Vec2& c = poly[idx[(i + 1) % n]];
            double cr = (b - a).cross(c - b);
            if (cr <= 1e-12) continue;  // reflex or degenerate
            bool ear = true;
            for (size_t k = 0; k < n && ear; ++k) {
                if (k == i || k == (i + n - 1) % n || k == (i + 1) % n) continue;
                const Vec2& p = poly[idx[k]];
                if ((p - a).length() < 1e-9 || (p - b).length() < 1e-9 || (p - c).length() < 1e-9) continue;
                if (pointInTri(p, a, b, c)) ear = false;
            }
            if (!ear) continue;
            out.push_back({a, b, c});
            idx.erase(idx.begin() + i);
            clipped = true;
            break;
        }
        if (!clipped) {
            // Degenerate input: remove a vertex with the smallest |cross| to make progress.
            size_t best = 0;
            double bv = 1e300;
            for (size_t i = 0; i < n; ++i) {
                const Vec2& a = poly[idx[(i + n - 1) % n]];
                const Vec2& b = poly[idx[i]];
                const Vec2& c = poly[idx[(i + 1) % n]];
                double v = std::fabs((b - a).cross(c - b));
                if (v < bv) { bv = v; best = i; }
            }
            idx.erase(idx.begin() + best);
        }
    }
    if (idx.size() == 3) out.push_back({poly[idx[0]], poly[idx[1]], poly[idx[2]]});
}

std::vector<std::array<Vec2, 3>> triangulate(const Polys& polysIn) {
    std::vector<std::array<Vec2, 3>> out;
    std::vector<std::vector<Vec2>> contours;
    for (auto& c : polysIn) {
        std::vector<Vec2> pts;
        for (auto& p : c.pts)
            if (pts.empty() || (p - pts.back()).length() > 1e-6) pts.push_back(p);
        if (pts.size() > 2 && (pts.front() - pts.back()).length() < 1e-6) pts.pop_back();
        if (pts.size() >= 3 && std::fabs(signedArea(pts)) > 1e-6) contours.push_back(pts);
    }
    size_t n = contours.size();
    // Nesting depth decides outer (even) vs hole (odd).
    std::vector<int> depth(n, 0);
    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < n; ++j)
            if (i != j && pointInPoly(contours[j], contours[i][0])) depth[i]++;
    std::vector<std::vector<Vec2>> outers;
    std::vector<std::vector<std::vector<Vec2>>> holes;
    std::vector<size_t> outerOf(n, SIZE_MAX);
    for (size_t i = 0; i < n; ++i) {
        if (depth[i] % 2 == 0) {
            auto c = contours[i];
            if (signedArea(c) < 0) std::reverse(c.begin(), c.end());
            outerOf[i] = outers.size();
            outers.push_back(c);
            holes.push_back({});
        }
    }
    for (size_t i = 0; i < n; ++i) {
        if (depth[i] % 2 == 1) {
            // Find the containing outer with depth == depth[i]-1 and smallest area.
            size_t best = SIZE_MAX;
            double bestA = 1e300;
            for (size_t j = 0; j < n; ++j) {
                if (depth[j] == depth[i] - 1 && pointInPoly(contours[j], contours[i][0])) {
                    double a = std::fabs(signedArea(contours[j]));
                    if (a < bestA) { bestA = a; best = j; }
                }
            }
            if (best == SIZE_MAX || outerOf[best] == SIZE_MAX) continue;
            auto h = contours[i];
            if (signedArea(h) > 0) std::reverse(h.begin(), h.end());
            holes[outerOf[best]].push_back(h);
        }
    }
    for (size_t o = 0; o < outers.size(); ++o) {
        std::vector<Vec2> poly = outers[o];
        auto& hs = holes[o];
        std::sort(hs.begin(), hs.end(), [](const std::vector<Vec2>& a, const std::vector<Vec2>& b) {
            double ma = -1e300, mb = -1e300;
            for (auto& p : a) ma = std::max(ma, p.x);
            for (auto& p : b) mb = std::max(mb, p.x);
            return ma > mb;
        });
        for (auto& h : hs) {
            size_t mi = 0;
            for (size_t k = 1; k < h.size(); ++k)
                if (h[k].x > h[mi].x) mi = k;
            Vec2 M = h[mi];
            // Ray cast to +x.
            double bestX = 1e300;
            size_t pi = SIZE_MAX;
            Vec2 I;
            for (size_t k = 0; k < poly.size(); ++k) {
                Vec2 a = poly[k], b = poly[(k + 1) % poly.size()];
                if ((a.y > M.y) == (b.y > M.y)) continue;
                double x = a.x + (M.y - a.y) * (b.x - a.x) / (b.y - a.y);
                if (x >= M.x && x < bestX) {
                    bestX = x;
                    I = {x, M.y};
                    pi = a.x > b.x ? k : (k + 1) % poly.size();
                }
            }
            if (pi == SIZE_MAX) continue;
            Vec2 P = poly[pi];
            // Check reflex vertices inside triangle M,I,P; pick the one with smallest angle.
            double bestAng = 1e300;
            for (size_t k = 0; k < poly.size(); ++k) {
                const Vec2& a = poly[(k + poly.size() - 1) % poly.size()];
                const Vec2& b = poly[k];
                const Vec2& c = poly[(k + 1) % poly.size()];
                if ((b - a).cross(c - b) > 0) continue;  // convex
                if (k == pi) continue;
                if (pointInTri(b, M, I, P)) {
                    double ang = std::atan2(std::fabs(b.y - M.y), b.x - M.x);
                    if (ang < bestAng) { bestAng = ang; pi = k; }
                }
            }
            std::vector<Vec2> merged;
            for (size_t k = 0; k <= pi; ++k) merged.push_back(poly[k]);
            for (size_t k = 0; k <= h.size(); ++k) merged.push_back(h[(mi + k) % h.size()]);
            merged.push_back(poly[pi]);
            for (size_t k = pi + 1; k < poly.size(); ++k) merged.push_back(poly[k]);
            poly = merged;
        }
        earClip(poly, out);
    }
    return out;
}

Mesh extrudePolys(const Polys& polys, double depth, const Material& front, const Material& side) {
    Mesh m;
    m.materials = {front, side};
    auto tris = triangulate(polys);
    auto addV = [&](Vec3 p, Vec3 n) {
        m.positions.push_back(p);
        m.normals.push_back(n);
        m.uvs.push_back({0, 0});
        return (uint32_t)m.positions.size() - 1;
    };
    for (auto& t : tris) {
        // Front cap at z=0 faces the camera (-z).
        uint32_t a = addV({t[0].x, t[0].y, 0}, {0, 0, -1}), b = addV({t[1].x, t[1].y, 0}, {0, 0, -1}), c = addV({t[2].x, t[2].y, 0}, {0, 0, -1});
        m.indices.insert(m.indices.end(), {a, b, c});
        m.triMaterial.push_back(0);
        if (depth > 0) {
            uint32_t d = addV({t[0].x, t[0].y, depth}, {0, 0, 1}), e = addV({t[1].x, t[1].y, depth}, {0, 0, 1}), f = addV({t[2].x, t[2].y, depth}, {0, 0, 1});
            m.indices.insert(m.indices.end(), {d, f, e});
            m.triMaterial.push_back(0);
        }
    }
    if (depth > 0) {
        std::vector<std::vector<Vec2>> all;
        for (auto& c : polys) all.push_back(c.pts);
        auto insideAll = [&](const Vec2& p) {
            int cnt = 0;
            for (auto& c : all)
                if (c.size() >= 3 && pointInPoly(c, p)) ++cnt;
            return cnt % 2 == 1;
        };
        for (auto& c : polys) {
            size_t n = c.pts.size();
            if (n < 2) continue;
            for (size_t i = 0; i < n; ++i) {
                Vec2 a = c.pts[i], b = c.pts[(i + 1) % n];
                Vec2 e = b - a;
                if (e.length() < 1e-9) continue;
                Vec2 nn = e.perp().normalized();
                Vec2 mid = (a + b) * 0.5;
                if (insideAll(mid + nn * 0.01)) nn = nn * -1;
                Vec3 N{nn.x, nn.y, 0};
                uint32_t p0 = addV({a.x, a.y, 0}, N), p1 = addV({b.x, b.y, 0}, N), p2 = addV({b.x, b.y, depth}, N), p3 = addV({a.x, a.y, depth}, N);
                m.indices.insert(m.indices.end(), {p0, p1, p2, p0, p2, p3});
                m.triMaterial.push_back(1);
                m.triMaterial.push_back(1);
            }
        }
    }
    return m;
}

// ====================================================================== rendering
namespace {
struct CV {  // clip-space vertex
    Vec3 cam;   // camera space
    Vec3 world, normal;
    Vec2 uv;
};

CV lerpCV(const CV& a, const CV& b, double t) {
    CV r;
    r.cam = a.cam + (b.cam - a.cam) * t;
    r.world = a.world + (b.world - a.world) * t;
    r.normal = a.normal + (b.normal - a.normal) * t;
    r.uv = a.uv + (b.uv - a.uv) * t;
    return r;
}

Color shade(const Material& mat, const Vec3& P, Vec3 N, const Vec3& camPos, const Vec2& uv, const std::vector<Light3D>& lights, float shadow) {
    Color base = mat.baseColor;
    if (mat.baseColorTex && !mat.baseColorTex->empty()) {
        const Image& t = *mat.baseColorTex;
        double u = uv.x - std::floor(uv.x), v = uv.y - std::floor(uv.y);
        float px[4];
        sampleBilinear(t, u * t.w, v * t.h, px);
        float a = px[3] / 255.f;
        if (a > 0) base = Color(base.r * px[0] / 255.f / a, base.g * px[1] / 255.f / a, base.b * px[2] / 255.f / a, base.a * a);
    }
    Vec3 V = (camPos - P).normalized();
    if (N.dot(V) < 0) N = -N;  // two-sided
    float r = 0, g = 0, b = 0;
    float rough = std::max(0.04f, mat.roughness);
    float alpha = rough * rough;
    float metal = mat.metallic;
    Vec3 F0 = Vec3(0.04, 0.04, 0.04) * (1 - metal) + Vec3(base.r, base.g, base.b) * metal;
    bool anyDirect = false;
    for (auto& L : lights) {
        if (L.kind == Light3D::Kind::Ambient) {
            r += base.r * L.color.r * (float)L.intensity;
            g += base.g * L.color.g * (float)L.intensity;
            b += base.b * L.color.b * (float)L.intensity;
            continue;
        }
        anyDirect = true;
        Vec3 Ld;
        double atten = L.intensity;
        if (L.kind == Light3D::Kind::Directional) Ld = -L.direction.normalized();
        else {
            Vec3 d = L.position - P;
            double dist = d.length();
            Ld = d / std::max(1e-9, dist);
            if (L.radius > 0) atten *= clampv(1.0 - dist / L.radius, 0.0, 1.0);
            if (L.kind == Light3D::Kind::Spot) {
                double c = (-Ld).dot(L.direction.normalized());
                atten *= smoothstep(L.coneCos, L.featherCos, c);
            }
        }
        double NL = N.dot(Ld);
        if (NL <= 0 || atten <= 0) continue;
        float sh = (L.castShadows && L.kind == Light3D::Kind::Directional) ? (1.f - (1.f - shadow) * (float)L.shadowDarkness) : 1.f;
        Vec3 H = (Ld + V).normalized();
        double NH = std::max(0.0, N.dot(H)), NV = std::max(1e-4, N.dot(V)), VH = std::max(0.0, V.dot(H));
        double a2 = alpha * alpha;
        double dd = NH * NH * (a2 - 1) + 1;
        double D = a2 / (kPi * dd * dd);
        double k = (rough + 1) * (rough + 1) / 8;
        double G = (NL / (NL * (1 - k) + k)) * (NV / (NV * (1 - k) + k));
        double fw = std::pow(1 - VH, 5);
        Vec3 F = F0 + (Vec3(1, 1, 1) - F0) * fw;
        Vec3 spec = F * (D * G / (4 * NL * NV + 1e-4));
        Vec3 kd = (Vec3(1, 1, 1) - F) * (1 - metal);
        double rad = atten * NL * sh;
        r += (float)((kd.x * base.r + spec.x * kPi * 0.25) * L.color.r * rad);
        g += (float)((kd.y * base.g + spec.y * kPi * 0.25) * L.color.g * rad);
        b += (float)((kd.z * base.b + spec.z * kPi * 0.25) * L.color.b * rad);
    }
    (void)anyDirect;
    r += mat.emission.r * mat.emissionStrength;
    g += mat.emission.g * mat.emissionStrength;
    b += mat.emission.b * mat.emissionStrength;
    return Color(r, g, b, base.a * mat.opacity);
}
}  // namespace

void renderMesh(Image& dst, std::vector<float>& zbuf, const Mesh& mesh, const Mat4& model, const Camera3D& cam,
                const std::vector<Light3D>& lights0, const MeshRenderOptions& opt) {
    int W = dst.w, H = dst.h;
    if ((int)zbuf.size() != W * H) zbuf.assign((size_t)W * H, 1e30f);
    std::vector<Light3D> lights = lights0;
    if (lights.empty()) {
        // Default lighting so unlit scenes still read as 3D.
        Light3D amb; amb.kind = Light3D::Kind::Ambient; amb.intensity = 0.35; lights.push_back(amb);
        Light3D key; key.kind = Light3D::Kind::Directional; key.direction = Vec3(-0.4, 0.6, 1).normalized(); key.intensity = 0.85; lights.push_back(key);
    }
    Mat4 normalM;
    {
        Mat4 inv;
        if (model.inverse(inv)) {
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) normalM.at(r, c) = inv.at(c, r);
        }
    }
    struct Tri { CV v[3]; int mat; };
    std::vector<Tri> tris;
    tris.reserve(mesh.triangleCount());
    double nearZ = std::max(0.5, cam.near_);
    for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        CV vs[3];
        for (int k = 0; k < 3; ++k) {
            uint32_t i = mesh.indices[t + k];
            vs[k].world = model.transformPoint(mesh.positions[i]);
            vs[k].normal = i < mesh.normals.size() ? normalM.transformDir(mesh.normals[i]).normalized() : Vec3(0, 0, -1);
            vs[k].uv = i < mesh.uvs.size() ? mesh.uvs[i] : Vec2();
            vs[k].cam = cam.view.transformPoint(vs[k].world);
        }
        int mat = t / 3 < mesh.triMaterial.size() ? mesh.triMaterial[t / 3] : 0;
        // Near-plane clipping (Sutherland-Hodgman against z = nearZ).
        std::vector<CV> poly(vs, vs + 3), outp;
        for (size_t i = 0; i < poly.size(); ++i) {
            const CV& a = poly[i];
            const CV& b = poly[(i + 1) % poly.size()];
            bool ain = a.cam.z > nearZ, bin = b.cam.z > nearZ;
            if (ain) outp.push_back(a);
            if (ain != bin) outp.push_back(lerpCV(a, b, (nearZ - a.cam.z) / (b.cam.z - a.cam.z)));
        }
        for (size_t i = 1; i + 1 < outp.size(); ++i) tris.push_back({{outp[0], outp[i], outp[i + 1]}, mat});
    }
    auto proj = [&](const Vec3& c) {
        return Vec2(W / 2.0 + cam.zoom * c.x / c.z * cam.scale, H / 2.0 + cam.zoom * c.y / c.z * cam.scale);
    };
    int bandH = 16;
    int bands = (H + bandH - 1) / bandH;
    const Material defMat;
    parallelFor(bands, [&](int b0, int b1) {
        for (int band = b0; band < b1; ++band) {
            int yMin = band * bandH, yMax = std::min(H, yMin + bandH);
            for (auto& tri : tris) {
                Vec2 s[3];
                for (int k = 0; k < 3; ++k) s[k] = proj(tri.v[k].cam);
                double area = (s[1] - s[0]).cross(s[2] - s[0]);
                if (std::fabs(area) < 1e-12) continue;
                int minY = std::max(yMin, (int)std::floor(std::min({s[0].y, s[1].y, s[2].y})));
                int maxY = std::min(yMax - 1, (int)std::ceil(std::max({s[0].y, s[1].y, s[2].y})));
                if (minY > maxY) continue;
                int minX = std::max(0, (int)std::floor(std::min({s[0].x, s[1].x, s[2].x})));
                int maxX = std::min(W - 1, (int)std::ceil(std::max({s[0].x, s[1].x, s[2].x})));
                if (minX > maxX) continue;
                const Material& mat = tri.mat < (int)mesh.materials.size() ? mesh.materials[tri.mat] : defMat;
                double iz[3] = {1 / tri.v[0].cam.z, 1 / tri.v[1].cam.z, 1 / tri.v[2].cam.z};
                for (int y = minY; y <= maxY; ++y) {
                    for (int x = minX; x <= maxX; ++x) {
                        Vec2 p{x + 0.5, y + 0.5};
                        double w0 = (s[1] - p).cross(s[2] - p) / area;
                        double w1 = (s[2] - p).cross(s[0] - p) / area;
                        double w2 = 1 - w0 - w1;
                        if (w0 < -1e-9 || w1 < -1e-9 || w2 < -1e-9) continue;
                        if (opt.wireframe && std::min({w0, w1, w2}) > 0.03) continue;
                        double izp = w0 * iz[0] + w1 * iz[1] + w2 * iz[2];
                        double z = 1 / izp;
                        float& zb = zbuf[(size_t)y * W + x];
                        if (z >= zb) continue;
                        double pw0 = w0 * iz[0] * z, pw1 = w1 * iz[1] * z, pw2 = w2 * iz[2] * z;
                        Vec3 P = tri.v[0].world * pw0 + tri.v[1].world * pw1 + tri.v[2].world * pw2;
                        Vec3 N = (tri.v[0].normal * pw0 + tri.v[1].normal * pw1 + tri.v[2].normal * pw2).normalized();
                        Vec2 uv = tri.v[0].uv * pw0 + tri.v[1].uv * pw1 + tri.v[2].uv * pw2;
                        float shadow = 1.f;
                        if (opt.shadowMap && opt.receiveShadows && opt.shadowSize > 0) {
                            Vec3 sp = opt.shadowViewProj.transformPoint(P);
                            int sx = (int)((sp.x * 0.5 + 0.5) * opt.shadowSize), sy = (int)((sp.y * 0.5 + 0.5) * opt.shadowSize);
                            if (sx >= 0 && sy >= 0 && sx < opt.shadowSize && sy < opt.shadowSize) {
                                float sd = (*opt.shadowMap)[(size_t)sy * opt.shadowSize + sx];
                                if (sp.z > sd + 2.0) shadow = 0.f;
                            }
                        }
                        Color c = shade(mat, P, N, cam.position, uv, lights, shadow);
                        float a = clampv(c.a * opt.opacity, 0.f, 1.f);
                        if (a <= 0.004f) continue;
                        if (a >= 0.999f) zb = (float)z;
                        uint8_t* d = dst.at(x, y);
                        float inv = 1 - a;
                        d[0] = (uint8_t)std::min(255.f, clampv(c.r, 0.f, 1.f) * a * 255.f + d[0] * inv);
                        d[1] = (uint8_t)std::min(255.f, clampv(c.g, 0.f, 1.f) * a * 255.f + d[1] * inv);
                        d[2] = (uint8_t)std::min(255.f, clampv(c.b, 0.f, 1.f) * a * 255.f + d[2] * inv);
                        d[3] = (uint8_t)std::min(255.f, a * 255.f + d[3] * inv);
                    }
                }
            }
        }
    }, 1);
}

void buildShadowMap(const std::vector<std::pair<const Mesh*, Mat4>>& casters, const Light3D& light, const Vec3& center, double radius,
                    int size, std::vector<float>& depth, Mat4& viewProj) {
    Vec3 dir = light.direction.normalized();
    Vec3 eye = center - dir * radius * 2;
    Vec3 up = std::fabs(dir.y) > 0.9 ? Vec3(0, 0, 1) : Vec3(0, 1, 0);
    Mat4 view = lookAt(eye, center, up);
    Mat4 ortho = Mat4::scale(1.0 / radius, 1.0 / radius, 1.0);
    viewProj = ortho * view;
    depth.assign((size_t)size * size, 1e30f);
    for (auto& [mesh, model] : casters) {
        for (size_t t = 0; t + 2 < mesh->indices.size(); t += 3) {
            Vec3 p[3];
            Vec2 s[3];
            for (int k = 0; k < 3; ++k) {
                p[k] = viewProj.transformPoint(model.transformPoint(mesh->positions[mesh->indices[t + k]]));
                s[k] = {(p[k].x * 0.5 + 0.5) * size, (p[k].y * 0.5 + 0.5) * size};
            }
            double area = (s[1] - s[0]).cross(s[2] - s[0]);
            if (std::fabs(area) < 1e-12) continue;
            int minX = std::max(0, (int)std::floor(std::min({s[0].x, s[1].x, s[2].x})));
            int maxX = std::min(size - 1, (int)std::ceil(std::max({s[0].x, s[1].x, s[2].x})));
            int minY = std::max(0, (int)std::floor(std::min({s[0].y, s[1].y, s[2].y})));
            int maxY = std::min(size - 1, (int)std::ceil(std::max({s[0].y, s[1].y, s[2].y})));
            for (int y = minY; y <= maxY; ++y)
                for (int x = minX; x <= maxX; ++x) {
                    Vec2 q{x + 0.5, y + 0.5};
                    double w0 = (s[1] - q).cross(s[2] - q) / area, w1 = (s[2] - q).cross(s[0] - q) / area, w2 = 1 - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                    float z = (float)(w0 * p[0].z + w1 * p[1].z + w2 * p[2].z);
                    float& d = depth[(size_t)y * size + x];
                    if (z < d) d = z;
                }
        }
    }
}

}  // namespace mf
