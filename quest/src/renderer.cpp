#include "renderer.h"

#include <vector>

static const char* kQuadVS = R"(#version 300 es
uniform vec4 uRect; // x0 y0 x1 y1 in NDC
out vec2 vUv;
void main() {
    vec2 c = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    vUv = vec2(c.x, 1.0 - c.y); // texture row 0 = top of the image
    gl_Position = vec4(mix(uRect.xy, uRect.zw, c), 0.0, 1.0);
}
)";

static const char* kVideoFS = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
uniform samplerExternalOES uTex;
uniform vec2 uScale;
in vec2 vUv;
out vec4 oColor;
vec3 toLinear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)); }
void main() {
    vec3 c = texture(uTex, vUv * uScale).rgb;
    oColor = vec4(toLinear(c), 1.0);
}
)";

static const char* kTexFS = R"(#version 300 es
precision mediump float;
uniform sampler2D uTex;
in vec2 vUv;
out vec4 oColor;
void main() { oColor = texture(uTex, vUv); }
)";

static const char* kEnvVS = R"(#version 300 es
out vec2 vNdc;
void main() {
    vec2 c = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1)) * 2.0 - 1.0;
    vNdc = c;
    gl_Position = vec4(c, 0.999, 1.0);
}
)";

static const char* kEnvFS = R"(#version 300 es
precision highp float;
uniform mat4 uInvVP;
uniform vec3 uEye;
uniform int uMode;
uniform float uFloor;
uniform float uTime;
in vec2 vNdc;
out vec4 oColor;

float hash(vec3 p) {
    p = fract(p * 0.3183099 + 0.1);
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

vec3 studio(vec3 d) {
    vec3 horizon = vec3(0.060, 0.066, 0.085);
    vec3 zenith = vec3(0.010, 0.012, 0.020);
    vec3 col = mix(horizon, zenith, clamp(d.y * 1.6, 0.0, 1.0));
    if (d.y < -0.0005) {
        float t = (uFloor - uEye.y) / d.y;
        vec3 hit = uEye + d * t;
        vec2 g = abs(fract(hit.xz - 0.5) - 0.5) / max(fwidth(hit.xz), vec2(1e-4));
        float line = 1.0 - min(min(g.x, g.y), 1.0);
        float fade = exp(-t * 0.07);
        vec3 floorCol = vec3(0.018, 0.020, 0.026) + line * vec3(0.05, 0.08, 0.16);
        col = mix(horizon, floorCol, fade);
    }
    return col;
}

vec3 space(vec3 d) {
    vec3 col = vec3(0.002, 0.002, 0.005);
    col += vec3(0.030, 0.010, 0.050) * pow(max(dot(d, normalize(vec3(0.6, 0.25, -0.75))), 0.0), 6.0);
    col += vec3(0.005, 0.020, 0.040) * pow(max(dot(d, normalize(vec3(-0.7, -0.1, -0.4))), 0.0), 4.0);
    vec3 p = d * 220.0;
    vec3 cell = floor(p);
    float h = hash(cell);
    if (h > 0.9975) {
        float r = length(fract(p) - 0.5);
        float tw = 0.7 + 0.3 * sin(uTime * (1.0 + h * 5.0) + h * 100.0);
        col += vec3(0.9, 0.92, 1.0) * (1.0 - smoothstep(0.05, 0.35, r)) * tw;
    }
    return col;
}

void main() {
    vec4 p = uInvVP * vec4(vNdc, 1.0, 1.0);
    vec3 d = normalize(p.xyz / p.w - uEye);
    vec3 col = vec3(0.0);
    if (uMode == 1) col = studio(d);
    else if (uMode == 2) col = space(d);
    oColor = vec4(col, 1.0);
}
)";

static const char* kRayVS = R"(#version 300 es
uniform mat4 uVP;
layout(location = 0) in vec3 aPos;
layout(location = 1) in float aAlpha;
out float vAlpha;
void main() { vAlpha = aAlpha; gl_Position = uVP * vec4(aPos, 1.0); }
)";

static const char* kRayFS = R"(#version 300 es
precision mediump float;
uniform vec4 uColor;
in float vAlpha;
out vec4 oColor;
void main() { oColor = vec4(uColor.rgb, uColor.a * vAlpha); }
)";

static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOGE("shader compile error: %s", log);
    }
    return s;
}

static GLuint link(const char* vs, const char* fs) {
    GLuint p = glCreateProgram();
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        LOGE("program link error: %s", log);
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

bool Renderer::init() {
    progVideo_ = link(kQuadVS, kVideoFS);
    progTex_ = link(kQuadVS, kTexFS);
    progEnv_ = link(kEnvVS, kEnvFS);
    progRay_ = link(kRayVS, kRayFS);
    uVideoScale_ = glGetUniformLocation(progVideo_, "uScale");
    uTexRect_ = glGetUniformLocation(progTex_, "uRect");
    uEnvInvVP_ = glGetUniformLocation(progEnv_, "uInvVP");
    uEnvEye_ = glGetUniformLocation(progEnv_, "uEye");
    uEnvMode_ = glGetUniformLocation(progEnv_, "uMode");
    uEnvFloor_ = glGetUniformLocation(progEnv_, "uFloor");
    uEnvTime_ = glGetUniformLocation(progEnv_, "uTime");
    uRayVP_ = glGetUniformLocation(progRay_, "uVP");
    uRayColor_ = glGetUniformLocation(progRay_, "uColor");
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &rayVbo_);
    return progVideo_ && progTex_ && progEnv_ && progRay_;
}

GLuint Renderer::framebufferFor(GLuint colorTex) {
    auto it = fbos_.find(colorTex);
    if (it != fbos_.end()) return it->second;
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) LOGE("incomplete framebuffer");
    fbos_[colorTex] = fbo;
    return fbo;
}

void Renderer::drawVideo(GLuint externalTex, float uMax, float vMax) {
    glDisable(GL_BLEND);
    glUseProgram(progVideo_);
    glBindVertexArray(vao_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, externalTex);
    glUniform4f(glGetUniformLocation(progVideo_, "uRect"), -1, -1, 1, 1);
    glUniform2f(uVideoScale_, uMax, vMax);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Renderer::drawTexture(GLuint tex, float x0, float y0, float x1, float y1) {
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(progTex_);
    glBindVertexArray(vao_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform4f(uTexRect_, x0, y0, x1, y1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);
}

void Renderer::drawEnvironment(EnvMode mode, const Mat4& viewProj, const Vec3& eye, float floorY, float time) {
    // invert view-projection (generic 4x4 inverse)
    const float* m = viewProj.m;
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (fabsf(det) < 1e-12f) return;
    for (float& v : inv) v /= det;

    glDisable(GL_BLEND);
    glUseProgram(progEnv_);
    glBindVertexArray(vao_);
    glUniformMatrix4fv(uEnvInvVP_, 1, GL_FALSE, inv);
    glUniform3f(uEnvEye_, eye.x, eye.y, eye.z);
    glUniform1i(uEnvMode_, (int)mode);
    glUniform1f(uEnvFloor_, floorY);
    glUniform1f(uEnvTime_, time);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Renderer::drawStrip(const Mat4& viewProj, const float* v, const float rgb[3]) {
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(progRay_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, rayVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 16, v, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 16, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 16, (void*)12);
    glUniformMatrix4fv(uRayVP_, 1, GL_FALSE, viewProj.m);
    glUniform4f(uRayColor_, rgb[0], rgb[1], rgb[2], 1.0f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisable(GL_BLEND);
}

void Renderer::drawRay(const Mat4& viewProj, const Vec3& eye, const Vec3& from, const Vec3& to, float width,
                       const float rgb[3], float a0, float a1) {
    Vec3 dir = normalize(to - from);
    Vec3 side = normalize(cross(dir, eye - from)) * (width * 0.5f);
    Vec3 p[4] = {from - side, from + side, to - side, to + side};
    float a[4] = {a0, a0, a1, a1};
    float v[16];
    for (int i = 0; i < 4; ++i) {
        v[i * 4] = p[i].x;
        v[i * 4 + 1] = p[i].y;
        v[i * 4 + 2] = p[i].z;
        v[i * 4 + 3] = a[i];
    }
    drawStrip(viewProj, v, rgb);
}

void Renderer::drawQuad(const Mat4& viewProj, const Vec3& c, const Vec3& right, const Vec3& up, float w, float h,
                        const float rgb[3], float alpha) {
    Vec3 r = right * (w * 0.5f), u = up * (h * 0.5f);
    Vec3 p[4] = {c - r - u, c + r - u, c - r + u, c + r + u};
    float v[16];
    for (int i = 0; i < 4; ++i) {
        v[i * 4] = p[i].x;
        v[i * 4 + 1] = p[i].y;
        v[i * 4 + 2] = p[i].z;
        v[i * 4 + 3] = alpha;
    }
    drawStrip(viewProj, v, rgb);
}

GLuint Renderer::createTexture2D(int w, int h, const void* rgba, bool srgb) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

void Renderer::updateTexture2D(GLuint tex, int w, int h, const void* rgba) {
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}
