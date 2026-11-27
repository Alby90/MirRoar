// MirRoar Quest client - logging and small 3D math helpers.
#pragma once
#include <android/log.h>
#include <math.h>
#include <openxr/openxr.h>
#include <stdint.h>
#include <time.h>

#define LOG_TAG "MirRoar"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

inline int64_t monoUs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    Vec3(const XrVector3f& v) : x(v.x), y(v.y), z(v.z) {}
    operator XrVector3f() const { return {x, y, z}; }
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
};
inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const Vec3& v) { return sqrtf(dot(v, v)); }
inline Vec3 normalize(const Vec3& v) {
    float l = length(v);
    return l > 1e-6f ? v * (1.0f / l) : Vec3(0, 0, -1);
}

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
    Quat() = default;
    Quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    Quat(const XrQuaternionf& q) : x(q.x), y(q.y), z(q.z), w(q.w) {}
    operator XrQuaternionf() const { return {x, y, z, w}; }
};
inline Quat operator*(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Quat conj(const Quat& q) { return {-q.x, -q.y, -q.z, q.w}; }
inline Vec3 rotate(const Quat& q, const Vec3& v) {
    Vec3 u(q.x, q.y, q.z);
    Vec3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}
inline Quat quatAxisAngle(const Vec3& axis, float angle) {
    float s = sinf(angle * 0.5f);
    return {axis.x * s, axis.y * s, axis.z * s, cosf(angle * 0.5f)};
}
inline Quat normalizeQ(const Quat& q) {
    float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return {q.x / l, q.y / l, q.z / l, q.w / l};
}

struct Pose {
    Quat q;
    Vec3 p;
    Pose() = default;
    Pose(const Quat& q_, const Vec3& p_) : q(q_), p(p_) {}
    Pose(const XrPosef& x) : q(x.orientation), p(x.position) {}
    operator XrPosef() const { return {q, p}; }
};
inline Pose operator*(const Pose& a, const Pose& b) { return {a.q * b.q, a.p + rotate(a.q, b.p)}; }
inline Pose inverse(const Pose& a) {
    Quat iq = conj(a.q);
    return {iq, rotate(iq, -a.p)};
}
inline Vec3 transformPoint(const Pose& a, const Vec3& v) { return a.p + rotate(a.q, v); }

// Column-major 4x4 matrix
struct Mat4 {
    float m[16];
};
inline Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
            r.m[c * 4 + rr] = s;
        }
    return r;
}
inline Mat4 poseToMatrix(const Pose& p) {
    const Quat& q = p.q;
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z, wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    Mat4 r = {{1 - 2 * (yy + zz), 2 * (xy + wz), 2 * (xz - wy), 0, 2 * (xy - wz), 1 - 2 * (xx + zz), 2 * (yz + wx), 0,
               2 * (xz + wy), 2 * (yz - wx), 1 - 2 * (xx + yy), 0, p.p.x, p.p.y, p.p.z, 1}};
    return r;
}
inline Mat4 projectionFromFov(const XrFovf& fov, float nearZ, float farZ) {
    float l = tanf(fov.angleLeft), r = tanf(fov.angleRight), d = tanf(fov.angleDown), u = tanf(fov.angleUp);
    float w = r - l, h = u - d;
    Mat4 m = {{2 / w, 0, 0, 0, 0, 2 / h, 0, 0, (r + l) / w, (u + d) / h, -(farZ + nearZ) / (farZ - nearZ), -1, 0, 0,
               -2 * farZ * nearZ / (farZ - nearZ), 0}};
    return m;
}
inline Mat4 invertRigid(const Mat4& a) {
    // inverse of a rotation+translation matrix
    Mat4 r = {{a.m[0], a.m[4], a.m[8], 0, a.m[1], a.m[5], a.m[9], 0, a.m[2], a.m[6], a.m[10], 0, 0, 0, 0, 1}};
    r.m[12] = -(r.m[0] * a.m[12] + r.m[4] * a.m[13] + r.m[8] * a.m[14]);
    r.m[13] = -(r.m[1] * a.m[12] + r.m[5] * a.m[13] + r.m[9] * a.m[14]);
    r.m[14] = -(r.m[2] * a.m[12] + r.m[6] * a.m[13] + r.m[10] * a.m[14]);
    return r;
}
