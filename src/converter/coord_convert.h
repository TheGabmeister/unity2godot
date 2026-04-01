#pragma once
#include <array>
#include <cmath>
#include <string>
#include <sstream>
#include <iomanip>

namespace u2g {

struct Vec3  { float x = 0, y = 0, z = 0; };
struct Quat  { float x = 0, y = 0, z = 0, w = 1; };

struct Transform3D {
    // row-major: basis[row][col]
    float basis[3][3] = {{1,0,0},{0,1,0},{0,0,1}};
    Vec3  origin;
};

// Convert Unity local transform (LH) to Godot Transform3D (RH).
// Unity: Y-up, Z-forward (left-handed)
// Godot: Y-up, -Z-forward (right-handed)
inline Transform3D unityToGodot(Vec3 pos, Quat rot, Vec3 scale) {
    // Step 1: Handedness flip
    pos.z = -pos.z;
    rot.x = -rot.x;
    rot.y = -rot.y;
    // rot.z and rot.w unchanged

    // Step 2: Quaternion → rotation matrix
    float x = rot.x, y = rot.y, z = rot.z, w = rot.w;
    float xx = x*x, yy = y*y, zz = z*z;
    float xy = x*y, xz = x*z, yz = y*z;
    float wx = w*x, wy = w*y, wz = w*z;

    float R[3][3] = {
        { 1-2*(yy+zz), 2*(xy-wz),   2*(xz+wy)  },
        { 2*(xy+wz),   1-2*(xx+zz), 2*(yz-wx)   },
        { 2*(xz-wy),   2*(yz+wx),   1-2*(xx+yy) }
    };

    // Step 3: Multiply each column by scale
    Transform3D t;
    for (int r = 0; r < 3; r++) {
        t.basis[r][0] = R[r][0] * scale.x;
        t.basis[r][1] = R[r][1] * scale.y;
        t.basis[r][2] = R[r][2] * scale.z;
    }
    t.origin = pos;
    return t;
}

inline bool isIdentity(const Transform3D& t) {
    const float eps = 1e-5f;
    if (std::abs(t.origin.x) > eps || std::abs(t.origin.y) > eps || std::abs(t.origin.z) > eps)
        return false;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            if (std::abs(t.basis[r][c] - (r == c ? 1.0f : 0.0f)) > eps)
                return false;
    return true;
}

inline std::string fmtFloat(float v) {
    if (v == 0.0f) return "0";
    if (v == 1.0f) return "1";
    if (v == -1.0f) return "-1";
    std::ostringstream ss;
    ss << v;
    return ss.str();
}

inline std::string serializeTransform(const Transform3D& t) {
    std::ostringstream ss;
    ss << "Transform3D(";
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            ss << fmtFloat(t.basis[r][c]) << ", ";
    ss << fmtFloat(t.origin.x) << ", " << fmtFloat(t.origin.y) << ", " << fmtFloat(t.origin.z) << ")";
    return ss.str();
}

} // namespace u2g
