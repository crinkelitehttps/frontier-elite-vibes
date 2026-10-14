// Copied from ~/projects/shared/assets/vr-hand/vr_hand.h on 2026-10-14 (shared
// repo, unchanged); keep the two in step.
// vr_hand.h: a basic pointing hand for VR, built from five shaded boxes
// (palm, curled fingers, thumb, index finger, fingertip) along a motion
// controller's aim pose. Header-only, no dependencies beyond <cmath> and
// <vector>; renderer-agnostic (it returns triangles, you draw them).
//
// From the F-117A port (~/projects/f-117, GlRenderer::render_view, commit
// ed92cc7), where it replaces the laser pointers for touching cockpit
// buttons. See README.md beside this file.
//
// Units are centimetres; any right-handed frame works as long as `pos`,
// `fwd` and `up` share it (the F-117A port uses the aircraft frame, cm from
// the neutral eye point). Scale the output if your world isn't in cm.
//
//   vr_hand::Pose p;               // from the OpenXR aim pose
//   p.right = true; ...
//   std::vector<vr_hand::Tri> tris;
//   vr_hand::build(p, vr_hand::Style{}, &tris);
//   // optional: vr_hand::cull_and_sort(eye_pos, &tris) for painter's order
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace vr_hand {

// Touch state of the fingertip: Away (skin), Near (yellow: about to touch),
// Pressing (white).
enum class Touch { Away = 0, Near = 1, Pressing = 2 };

struct Pose {
    bool right = true;           // a left hand is the right one mirrored
    float pos[3] = {};           // the controller's aim point
    float fwd[3] = {0, 0, 1};    // unit: the aim direction (index finger)
    float up[3] = {0, 1, 0};     // unit: back of the hand (OpenXR aim +Y)
    Touch touch = Touch::Away;
};

struct Style {
    float finger = 4;            // fingertip's reach beyond pos along fwd (cm)
    float skin[3] = {0.62f, 0.56f, 0.5f};
    float near_tip[3] = {1, 0.88f, 0.25f};
    float press_tip[3] = {1, 1, 1};
    // Light direction (unit, same frame; from above, a little behind) and
    // the ambient share: lit = ambient + (1 - ambient) * max(0, n.L).
    float light[3] = {0.27f, 0.88f, -0.39f};
    float ambient = 0.4f;
};

struct Tri {
    float p[3][3];               // corners, counter-clockwise seen from outside
    float n[3];                  // outward face normal
    float rgb[3];                // shaded colour
};

// The index fingertip (pos + fwd * finger): the point to test for touches.
inline void fingertip(const Pose& p, const Style& s, float out[3]) {
    for (int i = 0; i < 3; i++) out[i] = p.pos[i] + p.fwd[i] * s.finger;
}

// Appends the hand's 60 triangles (5 boxes x 6 faces x 2) to `out`.
inline void build(const Pose& p, const Style& s, std::vector<Tri>* out) {
    const float* fw = p.fwd;
    const float* up = p.up;
    // right = up x forward; hand-local coordinates are (right, up, forward)
    // in cm from the aim point, for a right hand pointing (palm down, thumb
    // to the left); mx mirrors a left hand.
    const float rt[3] = {up[1] * fw[2] - up[2] * fw[1], up[2] * fw[0] - up[0] * fw[2], up[0] * fw[1] - up[1] * fw[0]};
    const float mx = p.right ? 1.0f : -1.0f, fl = s.finger;
    struct Box { float c[3], h[3]; bool tip; };  // centre, half extents
    const Box boxes[] = {
        {{1.8f, 0, -7.0f}, {3.6f, 1.4f, 4.5f}, false},                                   // palm
        {{3.0f, -1.4f, -1.2f}, {2.4f, 1.5f, 1.8f}, false},                               // curled fingers
        {{-2.6f, -0.6f, -4.5f}, {0.9f, 0.9f, 3.0f}, false},                              // thumb
        {{0, 0, (fl - 2.0f - 3.0f) * 0.5f}, {0.85f, 0.8f, (fl - 2.0f + 3.0f) * 0.5f}, false},  // index finger
        {{0, 0, fl - 1.0f}, {0.8f, 0.75f, 1.0f}, true},                                  // its tip
    };
    auto to_world = [&](const float l[3], float o[3]) {
        for (int i = 0; i < 3; i++) o[i] = p.pos[i] + l[0] * mx * rt[i] + l[1] * up[i] + l[2] * fw[i];
    };
    for (const Box& bx : boxes) {
        if (bx.h[2] <= 0) continue;  // finger shorter than the knuckle
        const float* base = s.skin;
        if (bx.tip && p.touch == Touch::Near) base = s.near_tip;
        if (bx.tip && p.touch == Touch::Pressing) base = s.press_tip;
        for (int k = 0; k < 3; k++)
            for (float sg : {-1.0f, 1.0f}) {
                const int a1 = (k + 1) % 3, a2 = (k + 2) % 3;
                static const float corner[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
                float q[4][3];
                for (int j = 0; j < 4; j++) {
                    float l[3];
                    l[k] = bx.c[k] + sg * bx.h[k];
                    l[a1] = bx.c[a1] + corner[j][0] * bx.h[a1];
                    l[a2] = bx.c[a2] + corner[j][1] * bx.h[a2];
                    to_world(l, q[j]);
                }
                float ln[3] = {0, 0, 0}, n[3];
                ln[k] = sg;
                for (int i = 0; i < 3; i++) n[i] = ln[0] * mx * rt[i] + ln[1] * up[i] + ln[2] * fw[i];
                const float d = n[0] * s.light[0] + n[1] * s.light[1] + n[2] * s.light[2];
                const float lit = s.ambient + (1 - s.ambient) * std::max(0.0f, d);
                // Winding: corners run a1 -> a2, counter-clockwise about +k;
                // flip for the -k face, and again for a mirrored hand.
                const bool flip = (sg < 0) != (mx < 0);
                const int order[2][3] = {{0, 1, 2}, {0, 2, 3}};
                for (const auto& o : order) {
                    Tri t;
                    for (int j = 0; j < 3; j++) {
                        const int c = flip ? o[2 - j] : o[j];
                        for (int i = 0; i < 3; i++) t.p[j][i] = q[c][i];
                    }
                    for (int i = 0; i < 3; i++) {
                        t.n[i] = n[i];
                        t.rgb[i] = base[i] * lit;
                    }
                    out->push_back(t);
                }
            }
    }
}

// For renderers without a depth buffer (as the F-117A cockpit overlay):
// drops triangles facing away from `eye` and sorts the rest far to near.
inline void cull_and_sort(const float eye[3], std::vector<Tri>* tris) {
    std::vector<std::pair<float, Tri>> keep;
    for (const Tri& t : *tris) {
        float c[3], e[3];
        for (int i = 0; i < 3; i++) {
            c[i] = (t.p[0][i] + t.p[1][i] + t.p[2][i]) / 3;
            e[i] = eye[i] - c[i];
        }
        if (t.n[0] * e[0] + t.n[1] * e[1] + t.n[2] * e[2] <= 0) continue;
        keep.push_back({e[0] * e[0] + e[1] * e[1] + e[2] * e[2], t});
    }
    std::stable_sort(keep.begin(), keep.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    tris->clear();
    for (auto& k : keep) tris->push_back(k.second);
}

}  // namespace vr_hand
