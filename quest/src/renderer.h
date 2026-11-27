// MirRoar Quest client - GL ES 3 drawing helpers (all output is linear; swapchains are sRGB).
#pragma once
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <map>
#include "common.h"

enum EnvMode { ENV_PASSTHROUGH = 0, ENV_STUDIO = 1, ENV_SPACE = 2, ENV_DARK = 3, ENV_COUNT = 4 };

class Renderer {
public:
    bool init();
    GLuint framebufferFor(GLuint colorTex);

    // Screen content pass (into the screen swapchain)
    void drawVideo(GLuint externalTex, float uMax, float vMax);
    void drawTexture(GLuint tex2D, float x0, float y0, float x1, float y1);  // NDC rect, alpha-blended

    // Eye pass (into the projection swapchain)
    void drawEnvironment(EnvMode mode, const Mat4& viewProj, const Vec3& eye, float floorY, float time);
    // Camera-facing strip from `from` to `to`; alpha fades from a0 to a1.
    void drawRay(const Mat4& viewProj, const Vec3& eye, const Vec3& from, const Vec3& to, float width,
                 const float rgb[3], float a0, float a1);
    // Flat rectangle in world space.
    void drawQuad(const Mat4& viewProj, const Vec3& center, const Vec3& right, const Vec3& up, float w, float h,
                  const float rgb[3], float alpha);

    static GLuint createTexture2D(int w, int h, const void* rgba, bool srgb);
    static void updateTexture2D(GLuint tex, int w, int h, const void* rgba);

private:
    GLuint progVideo_ = 0, progTex_ = 0, progEnv_ = 0, progRay_ = 0;
    GLint uVideoScale_ = -1, uTexRect_ = -1, uEnvInvVP_ = -1, uEnvEye_ = -1, uEnvMode_ = -1, uEnvFloor_ = -1,
          uEnvTime_ = -1, uRayVP_ = -1, uRayColor_ = -1;
    GLuint vao_ = 0, rayVbo_ = 0;
    void drawStrip(const Mat4& viewProj, const float* verts4x4, const float rgb[3]);
    std::map<GLuint, GLuint> fbos_;
};
