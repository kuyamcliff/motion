#include "mf/shapes.hpp"

namespace mf {

Polys shapeItemPolys(const json& it, const EvalContext& ctx, double tol) {
    std::string type = it.value("type", "rect");
    Vec2 pos = propVec2(it, "position", ctx, {0, 0});
    Polys out;
    if (type == "rect") {
        Vec2 sz = propVec2(it, "size", ctx, {100, 100});
        flattenInto(BezierPath::rect(pos.x, pos.y, sz.x, sz.y, propNumber(it, "roundness", ctx, 0)), tol, out);
    } else if (type == "ellipse") {
        Vec2 sz = propVec2(it, "size", ctx, {100, 100});
        flattenInto(BezierPath::ellipse(pos.x, pos.y, sz.x, sz.y), tol, out);
    } else if (type == "star" || type == "polygon") {
        int pts = (int)std::round(propNumber(it, "points", ctx, 5));
        double outer = propNumber(it, "outerRadius", ctx, 100), inner = propNumber(it, "innerRadius", ctx, 50);
        flattenInto(BezierPath::star(pos.x, pos.y, pts, outer, inner, propNumber(it, "rotation", ctx, 0), type == "polygon"), tol, out);
    } else if (type == "path") {
        const json* p = it.contains("path") ? &it["path"] : nullptr;
        if (p) {
            Value v = evalProperty(*p, ctx);
            BezierPath bp = BezierPath::fromValue(v);
            for (auto& vx : bp.v) vx.p = vx.p + pos;
            flattenInto(bp, tol, out);
        }
    } else if (type == "line" || type == "arrow") {
        Vec2 a = propVec2(it, "from", ctx, {-100, 0}) + pos, b = propVec2(it, "to", ctx, {100, 0}) + pos;
        out.push_back({{a, b}, false});
        if (type == "arrow") {
            double hs = propNumber(it, "headSize", ctx, 30);
            Vec2 d = (b - a).normalized(), n = d.perp();
            out.push_back({{b - d * hs + n * hs * 0.6, b, b - d * hs - n * hs * 0.6}, false});
        }
    }
    return out;
}

ShapeGeometry buildShapeGeometry(const json& shape, const EvalContext& ctx, double tol) {
    ShapeGeometry g;
    std::vector<std::pair<std::string, Polys>> items;
    Polys strokeLines;
    for (auto& it : jarr(shape, "items")) {
        if (!it.value("enabled", true)) continue;
        Polys p = shapeItemPolys(it, ctx, tol);
        items.push_back({it.value("op", std::string("add")), p});
        for (auto& c : p) strokeLines.push_back(c);
    }
    auto modifyAll = [&](const std::function<Polys(const Polys&)>& f) {
        for (auto& it : items) it.second = f(it.second);
        strokeLines = f(strokeLines);
    };
    const json& rc = jobj(shape, "roundCorners");
    if (rc.value("enabled", false)) {
        double r = propNumber(rc, "radius", ctx, 10);
        modifyAll([&](const Polys& p) { return roundCornersPolys(p, r); });
    }
    const json& off = jobj(shape, "offsetPath");
    if (off.value("enabled", false)) {
        double a = propNumber(off, "amount", ctx, 0);
        modifyAll([&](const Polys& p) { return offsetPolysApprox(p, a); });
    }
    const json& zz = jobj(shape, "zigzag");
    if (zz.value("enabled", false)) {
        double sz = propNumber(zz, "size", ctx, 10);
        int ridges = (int)std::round(propNumber(zz, "ridges", ctx, 5));
        bool smooth = zz.value("smooth", false);
        modifyAll([&](const Polys& p) { return zigzagPolys(p, sz, ridges, smooth); });
    }
    const json& tw = jobj(shape, "twist");
    if (tw.value("enabled", false)) {
        double ang = deg2rad(propNumber(tw, "angle", ctx, 0));
        Rect b = polysBounds(strokeLines);
        Vec2 c{(b.x0 + b.x1) / 2.0, (b.y0 + b.y1) / 2.0};
        double R = std::max(1.0, std::max(b.width(), b.height()) / 2.0);
        auto f = [&](const Vec2& p) {
            Vec2 d = p - c;
            double a = ang * std::min(1.0, d.length() / R);
            double ca = std::cos(a), sa = std::sin(a);
            return Vec2(c.x + d.x * ca - d.y * sa, c.y + d.x * sa + d.y * ca);
        };
        modifyAll([&](const Polys& p) { Polys q = p; transformPolys(q, f); return q; });
    }
    const json& tr = jobj(shape, "trim");
    if (tr.value("enabled", false)) {
        double s = propNumber(tr, "start", ctx, 0) / 100.0, e = propNumber(tr, "end", ctx, 100) / 100.0, o = propNumber(tr, "offset", ctx, 0) / 360.0;
        modifyAll([&](const Polys& p) {
            Polys q = trimPolys(p, s, e, o);
            return q;
        });
    }
    const json& st = jobj(shape, "stroke");
    if (st.contains("dash") && st["dash"].is_array() && !st["dash"].empty()) {
        std::vector<double> dash;
        for (auto& d : st["dash"]) dash.push_back(d.get<double>());
        strokeLines = dashPolys(strokeLines, dash, propNumber(st, "dashOffset", ctx, 0));
    }
    const json& rp = jobj(shape, "repeater");
    bool rep = rp.value("enabled", false);
    int copies = 1;
    Vec2 rpPos, rpScale{100, 100};
    double rpRot = 0, rpOff = 0, so = 100, eo = 100;
    if (rep) {
        copies = clampv((int)std::round(propNumber(rp, "copies", ctx, 3)), 0, 500);
        rpOff = propNumber(rp, "offset", ctx, 0);
        rpPos = propVec2(rp, "position", ctx, {100, 0});
        rpScale = propVec2(rp, "scale", ctx, {100, 100});
        rpRot = propNumber(rp, "rotation", ctx, 0);
        so = propNumber(rp, "startOpacity", ctx, 100);
        eo = propNumber(rp, "endOpacity", ctx, 100);
    }
    for (int i = 0; i < copies; ++i) {
        double k = i + rpOff;
        ShapeGeometry::Copy c;
        c.fillItems = items;
        c.stroke = strokeLines;
        Mat4 xf;
        if (rep) {
            xf = Mat4::translate(rpPos.x * k, rpPos.y * k, 0) * Mat4::rotateZ(deg2rad(rpRot * k)) *
                 Mat4::scale(std::pow(rpScale.x / 100.0, k), std::pow(rpScale.y / 100.0, k), 1);
        }
        c.xf = xf;
        c.opacity = !rep ? 1.0 : copies > 1 ? (so + (eo - so) * (double)i / (copies - 1)) / 100.0 : so / 100.0;
        g.copies.push_back(std::move(c));
    }
    return g;
}

// ====================================================================== particles
std::vector<Particle> simulateParticles(const json& P, const EvalContext& ctx, double age) {
    std::vector<Particle> out;
    double rate = std::max(0.0, propNumber(P, "rate", ctx, 40));
    double life = std::max(0.01, propNumber(P, "lifetime", ctx, 2));
    double vel = propNumber(P, "velocity", ctx, 200), spread = deg2rad(propNumber(P, "spread", ctx, 60));
    double dir = deg2rad(propNumber(P, "direction", ctx, -90));
    double grav = propNumber(P, "gravity", ctx, 0), turb = propNumber(P, "turbulence", ctx, 0);
    double size0 = propNumber(P, "size", ctx, 8), size1 = propNumber(P, "sizeEnd", ctx, size0);
    double op0 = propNumber(P, "opacityStart", ctx, 100) / 100.0, op1 = propNumber(P, "opacityEnd", ctx, 0) / 100.0;
    Color c0 = propColor(P, "colorStart", ctx, Color(1, 1, 1)), c1 = propColor(P, "colorEnd", ctx, c0);
    double spin = propNumber(P, "spin", ctx, 0);
    Vec2 esz = propVec2(P, "emitterSize", ctx, {0, 0});
    double trails = propNumber(P, "trails", ctx, 0);
    uint32_t seed = (uint32_t)P.value("seed", 1);
    int maxP = P.value("maxParticles", 4000);
    bool multicolor = P.value("multicolor", false);
    bool twinkle = P.value("twinkle", false);
    const json& at = jobj(P, "attractor");
    bool attract = at.value("enabled", false);
    Vec2 apos = attract ? propVec2(at, "position", ctx, {0, 0}) : Vec2();
    double astr = attract ? propNumber(at, "strength", ctx, 0) : 0;
    if (rate <= 0 || age < 0) return out;
    // Static emitters like "stars" spawn a fixed population at t=0.
    bool stat = vel == 0 && P.value("preset", std::string()) == "stars";
    long first = (long)std::floor(std::max(0.0, age - life) * rate);
    long last = (long)std::floor(age * rate);
    if (stat) { first = 0; last = (long)(rate * life); }
    if (last - first > maxP) first = last - maxP;
    static const Color palette[] = {Color(1, 0.3f, 0.3f), Color(1, 0.8f, 0.2f), Color(0.3f, 0.8f, 1), Color(0.5f, 1, 0.4f), Color(1, 0.4f, 0.9f)};
    auto posAt = [&](long i, double a, const Vec2& origin, double ang, double v) {
        Vec2 p = origin + Vec2(std::cos(ang), std::sin(ang)) * (v * a) + Vec2(0, 0.5 * grav * a * a);
        if (turb > 0) {
            p.x += valueNoise1D(a * 1.3 + i * 0.37, seed + 11) * turb * std::min(1.0, a);
            p.y += valueNoise1D(a * 1.1 + i * 0.53, seed + 23) * turb * std::min(1.0, a);
        }
        if (attract && astr != 0) {
            Vec2 d = apos - p;
            double k = clampv(astr / 100.0 * a, -1.0, 1.0);
            p = p + d * (1 - std::exp(-std::fabs(k) * 2)) * (k >= 0 ? 1 : -1);
        }
        return p;
    };
    for (long i = first; i <= last; ++i) {
        double birth = stat ? 0 : i / rate;
        double a = stat ? std::fmod(age + hashToUnit((uint32_t)i * 97 + seed) * life, life) : age - birth;
        if (a < 0 || a > life) continue;
        uint32_t h = (uint32_t)i * 2654435761u + seed * 97u;
        double r1 = hashToUnit(h), r2 = hashToUnit(h + 1), r3 = hashToUnit(h + 2), r4 = hashToUnit(h + 3), r5 = hashToUnit(h + 4);
        Vec2 origin{(r3 - 0.5) * esz.x, (r4 - 0.5) * esz.y};
        double ang = dir + (r1 - 0.5) * spread;
        double v = vel * (0.6 + 0.8 * r2);
        Particle pt;
        pt.pos = posAt(i, a, origin, ang, v);
        pt.prevPos = trails > 0 ? posAt(i, std::max(0.0, a - trails), origin, ang, v) : pt.pos;
        double u = a / life;
        pt.age01 = u;
        pt.size = std::max(0.0, size0 + (size1 - size0) * u) * (0.7 + 0.6 * r5);
        pt.opacity = clampv(op0 + (op1 - op0) * u, 0.0, 1.0);
        if (stat) pt.opacity = twinkle ? 0.4 + 0.6 * (0.5 + 0.5 * std::sin(age * (2 + r5 * 4) + r1 * 6.28)) : 1.0;
        else if (u < 0.08) pt.opacity *= u / 0.08;  // soft spawn
        pt.color = multicolor ? palette[h % 5] : Color(c0.r + (c1.r - c0.r) * (float)u, c0.g + (c1.g - c0.g) * (float)u, c0.b + (c1.b - c0.b) * (float)u, 1);
        pt.rotation = spin * a + r1 * 360;
        out.push_back(pt);
    }
    return out;
}

}  // namespace mf
