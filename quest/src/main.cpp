// MirRoar Quest client - OpenXR app: giant virtual monitors of the PC, controllers or hands as mouse,
// toolbar, system keyboard, passthrough desk window.
#include <android/log.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#define XR_USE_TIMESPEC
#include <time.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "audio_player.h"
#include "common.h"
#include "keymap.h"
#include "net_client.h"
#include "renderer.h"
#include "video_decoder.h"

using namespace mirroar;

#define XR_CHECK(x)                                                     \
    do {                                                                \
        XrResult _r = (x);                                              \
        if (XR_FAILED(_r)) {                                            \
            LOGE("%s failed: %d (%s:%d)", #x, _r, __FILE__, __LINE__); \
        }                                                               \
    } while (0)

static PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC p_eglGetNativeClientBufferANDROID;
static PFNEGLCREATEIMAGEKHRPROC p_eglCreateImageKHR;
static PFNEGLDESTROYIMAGEKHRPROC p_eglDestroyImageKHR;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC p_glEGLImageTargetTexture2DOES;

// Builds a rotation whose +Z axis points along `fwd` with +Y as close as possible to world up.
static Quat lookRotation(const Vec3& fwdIn) {
    Vec3 z = normalize(fwdIn);
    Vec3 up(0, 1, 0);
    if (fabsf(dot(z, up)) > 0.99f) up = Vec3(0, 0, 1);
    Vec3 x = normalize(cross(up, z));
    Vec3 y = cross(z, x);
    float m00 = x.x, m01 = y.x, m02 = z.x, m10 = x.y, m11 = y.y, m12 = z.y, m20 = x.z, m21 = y.z, m22 = z.z;
    float tr = m00 + m11 + m22;
    Quat q;
    if (tr > 0) {
        float s = sqrtf(tr + 1.0f) * 2;
        q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        float s = sqrtf(1.0f + m00 - m11 - m22) * 2;
        q = {0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        float s = sqrtf(1.0f + m11 - m00 - m22) * 2;
        q = {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        float s = sqrtf(1.0f + m22 - m00 - m11) * 2;
        q = {(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
    }
    return normalizeQ(q);
}

static Vec3 yawDir(float yaw) { return Vec3(sinf(yaw), 0, -cosf(yaw)); }
static float dirYaw(const Vec3& d) { return atan2f(d.x, -d.z); }

struct Swapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    int width = 0, height = 0;
    std::vector<XrSwapchainImageOpenGLESKHR> images;
};

struct HandInput {
    XrPath path = XR_NULL_PATH;
    XrSpace aimSpace = XR_NULL_HANDLE;
    Pose aim;
    bool aimValid = false;
    bool isHand = false;  // tracked hand instead of a controller
    float trigger = 0, squeeze = 0;
    XrVector2f stick{0, 0};
    bool primary = false, secondary = false, stickClick = false, menu = false, ringPinch = false;
    bool triggerHeld = false, squeezeHeld = false;
    bool prevPrimary = false, prevSecondary = false, prevStickClick = false, prevMenu = false;
    float ringPrevY = 0;
    bool ringPrevValid = false;
};

// One streamed PC monitor shown as a virtual screen.
struct Screen {
    // shared with the network thread (guarded by App::streamM unless atomic)
    std::atomic<bool> active{false};
    std::atomic<bool> ready{false};
    int videoW = 0, videoH = 0, srcW = 1, srcH = 1;
    std::atomic<float> curX{0.5f}, curY{0.5f};
    std::atomic<bool> curVisible{false};
    std::atomic<bool> cursorDirty{true};
    int64_t lastIdrRequest = 0;
    VideoDecoder decoder;

    // render thread only
    Swapchain sc;
    Pose pose;
    float widthM = 2.4f;
    float radius = 2.2f;  // for the curved layout
    GLuint tex = 0;  // external OES texture bound to the current decoded image
    AImage* image = nullptr;
    uint64_t imageGen = 0;  // decoder generation the image belongs to
    EGLImageKHR eglImage = EGL_NO_IMAGE_KHR;
    float uMax = 1, vMax = 1;
    bool hasContent = false;
    bool dirty = true;
    float lastCurX = -1, lastCurY = -1;

    // latency measurement: per-frame info from the network thread, accumulated by the render thread
    struct FrameInfo {
        int64_t pts, recvUs, captureLocalUs;
        float convertMs, encodeMs, queueMs, netMs;
    };
    std::mutex infoM;
    std::deque<FrameInfo> infos;
    std::atomic<uint32_t> dropped{0};
    struct Acc {
        uint32_t frames = 0;
        double convert = 0, encode = 0, queue = 0, net = 0, decode = 0, render = 0, total = 0;
        float maxTotal = 0;
    } acc;

    float heightM() const {
        int w = sc.width ? sc.width : 16, h = sc.height ? sc.height : 9;
        return widthM * (float)h / (float)w;
    }
};

enum HitKind { HIT_NONE, HIT_SCREEN, HIT_TOOLBAR, HIT_BAR };
struct Hit {
    HitKind kind = HIT_NONE;
    int index = -1;  // screen index (HIT_SCREEN / HIT_BAR, -1 = panel bar)
    float u = 0, v = 0, t = 1e9f;
    Vec3 point;
};

enum ToolbarButton {
    TB_KEYBOARD, TB_ESC, TB_TAB, TB_CTRL, TB_ALT, TB_SHIFT, TB_WIN,
    TB_SCREEN, TB_ENV, TB_CURVED, TB_RECENTER, TB_DESK, TB_COUNT
};
static const uint16_t kLatchVk[4] = {0xA2 /*LCtrl*/, 0xA4 /*LAlt*/, 0xA0 /*LShift*/, 0x5B /*LWin*/};
static const char* kEnvNames[ENV_COUNT] = {"Passthrough", "Studio", "Space", "Dark"};

constexpr int kToolbarW = 2400, kToolbarH = 200;
constexpr float kBarW = 0.32f, kBarH = 0.022f, kBarGap = 0.035f;

class App : public NetClient::Handler {
public:
    explicit App(android_app* a) : app_(a), net_(this) {}

    android_app* app_;
    bool resumed = false;
    bool sessionRunning = false;
    bool exitRequested = false;

    // ---------------------------------------------------------------- XR state
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    XrSpace appSpace = XR_NULL_HANDLE, viewSpace = XR_NULL_HANDLE, stageSpace = XR_NULL_HANDLE;
    std::vector<XrViewConfigurationView> viewConfigs;
    Swapchain eyeSwapchains[2];
    Swapchain panelSwapchain, toolbarSwapchain;
    int64_t colorFormat = GL_SRGB8_ALPHA8;

    bool extPassthrough = false, extCylinder = false, extRefreshRate = false, extLayerSettings = false;
    bool extHandTracking = false, extHandAim = false, extTriangleMesh = false, extTimespec = false;
    PFN_xrConvertTimeToTimespecTimeKHR xrConvertTimeToTimespecTimeKHR_ = nullptr;
    int64_t displayUs = 0;  // predicted display time of the current frame, monoUs clock
    int64_t lastStatsSend = 0;

    XrPassthroughFB passthrough = XR_NULL_HANDLE;
    XrPassthroughLayerFB reconLayer = XR_NULL_HANDLE;  // full passthrough
    XrPassthroughLayerFB deskLayer = XR_NULL_HANDLE;   // projected onto the desk mesh
    XrTriangleMeshFB deskMesh = XR_NULL_HANDLE;
    XrGeometryInstanceFB deskGeometry = XR_NULL_HANDLE;
    bool ptRunning = false, reconResumed = false, deskResumed = false;
    PFN_xrCreatePassthroughFB xrCreatePassthroughFB_ = nullptr;
    PFN_xrPassthroughStartFB xrPassthroughStartFB_ = nullptr;
    PFN_xrPassthroughPauseFB xrPassthroughPauseFB_ = nullptr;
    PFN_xrCreatePassthroughLayerFB xrCreatePassthroughLayerFB_ = nullptr;
    PFN_xrPassthroughLayerResumeFB xrPassthroughLayerResumeFB_ = nullptr;
    PFN_xrPassthroughLayerPauseFB xrPassthroughLayerPauseFB_ = nullptr;
    PFN_xrCreateTriangleMeshFB xrCreateTriangleMeshFB_ = nullptr;
    PFN_xrCreateGeometryInstanceFB xrCreateGeometryInstanceFB_ = nullptr;
    PFN_xrGeometryInstanceSetTransformFB xrGeometryInstanceSetTransformFB_ = nullptr;

    PFN_xrCreateHandTrackerEXT xrCreateHandTrackerEXT_ = nullptr;
    PFN_xrLocateHandJointsEXT xrLocateHandJointsEXT_ = nullptr;
    XrHandTrackerEXT handTrackers[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    XrHandJointLocationEXT joints[2][XR_HAND_JOINT_COUNT_EXT];
    bool handTracked[2] = {false, false};
    XrHandTrackingAimStateFB handAim[2];

    XrActionSet actionSet = XR_NULL_HANDLE;
    XrAction actAim = XR_NULL_HANDLE, actTrigger = XR_NULL_HANDLE, actSqueeze = XR_NULL_HANDLE,
             actStick = XR_NULL_HANDLE, actPrimary = XR_NULL_HANDLE, actSecondary = XR_NULL_HANDLE,
             actStickClick = XR_NULL_HANDLE, actMenu = XR_NULL_HANDLE, actHaptic = XR_NULL_HANDLE;
    HandInput hands[2];

    // ---------------------------------------------------------------- EGL / GL
    EGLDisplay eglDisplay = EGL_NO_DISPLAY;
    EGLContext eglContext = EGL_NO_CONTEXT;
    EGLConfig eglConfig = nullptr;
    EGLSurface eglSurface = EGL_NO_SURFACE;
    Renderer renderer;
    GLuint videoTex = 0;
    GLuint cursorTex = 0;
    int cursorTexW = 0, cursorTexH = 0;
    GLuint panelTex = 0, toolbarTex = 0;

    // Decoded images are given back to the decoder only a few frames later, when the GPU is surely done with them.
    struct Retired {
        Screen* screen;
        AImage* image;
        uint64_t gen;
        EGLImageKHR egl;
        uint64_t releaseAt;
    };
    std::vector<Retired> retired;
    uint64_t frameIndex = 0;

    // ---------------------------------------------------------------- layout & settings
    Screen screens[kMaxStreams];
    Pose homePose;  // where the main screen (or the status panel) goes
    float homeWidth = 2.4f;
    float screenDistance = 2.2f;
    bool curved = false;
    bool placed = false;
    bool recenterRequested = false;
    std::atomic<bool> layoutNeeded{false};
    EnvMode envMode = ENV_STUDIO;
    float floorY = -1.6f;
    uint32_t savedMonitorMask = 0;

    bool deskOn = false, deskPlacing = false;
    Pose deskPose;  // in stage space when available (stable across recenters), else app space
    float deskW = 0.75f, deskD = 0.40f;
    bool settingsDirty = false;
    int64_t lastSettingsSave = 0;

    // ---------------------------------------------------------------- interaction
    int grabTarget = -2;  // -2 none, -1 home/panel, >=0 screen
    int grabHand = -1;
    Pose grabOffset;
    int activeHand = 1;
    Hit hit;
    int hoverButton = -1;
    float smoothU = 0.5f, smoothV = 0.5f;
    int smoothStream = -1;
    float sentU = -1, sentV = -1;
    int sentStream = -1;
    int64_t clickLockUntil = 0;
    float lockU = 0, lockV = 0;
    bool leftDown = false, rightDown = false;
    float wheelAccumX = 0, wheelAccumY = 0;
    int64_t lastLocalPointerUs = 0;
    std::atomic<uint32_t> latched{0};  // bit i = kLatchVk[i] held down on the PC
    std::atomic<bool> toolbarDirty{true};

    // ---------------------------------------------------------------- streaming state (shared with net thread)
    NetClient net_;
    AudioPlayer audio;
    std::mutex streamM;
    std::vector<SMonitorInfo> monitors;
    std::atomic<bool> connected{false};
    std::vector<uint8_t> cursorRgba;
    int cursorW = 0, cursorH = 0, cursorHotX = 0, cursorHotY = 0;
    bool cursorShapeDirty = false;
    std::string serverName, via;

    bool panelHasContent = false;
    std::string panelText;
    std::string toolbarKey;

    // JNI
    JNIEnv* jni = nullptr;
    jobject activity = nullptr;
    jclass activityClass = nullptr;
    jclass stringClass = nullptr;
    jmethodID midSetKeyboard = nullptr, midIsKeyboard = nullptr, midRenderPanel = nullptr, midRenderToolbar = nullptr;

    int64_t startUs = monoUs();
    int64_t lastFrameUs = 0;

    // ============================================================== NetClient::Handler (network thread)
    void onConnected(const std::string& name, const std::string& v) override {
        std::lock_guard<std::mutex> lk(streamM);
        serverName = name;
        via = v;
        connected = true;
        toolbarDirty = true;
    }
    void onDisconnected() override {
        connected = false;
        for (auto& s : screens) {
            s.active = false;
            s.ready = false;
            s.decoder.reset();
        }
        audio.close();
        latched = 0;
        toolbarDirty = true;
    }
    void onMonitorList(const std::vector<SMonitorInfo>& m) override {
        std::lock_guard<std::mutex> lk(streamM);
        monitors = m;
        toolbarDirty = true;
    }
    void onVideoConfig(const SVideoConfig& c) override {
        Screen& s = screens[c.stream];
        {
            std::lock_guard<std::mutex> lk(streamM);
            s.videoW = (int)c.width;
            s.videoH = (int)c.height;
            s.srcW = c.srcWidth ? (int)c.srcWidth : s.videoW;
            s.srcH = c.srcHeight ? (int)c.srcHeight : s.videoH;
        }
        if (!s.active.exchange(true)) layoutNeeded = true;
        toolbarDirty = true;
        LOGI("stream %u: %ux%u (monitor %ux%u)", c.stream, c.width, c.height, c.srcWidth, c.srcHeight);
    }
    void onVideoFrame(const SVideoFrame& f, const uint8_t* data, size_t len, int64_t recvUs) override {
        const int stream = (int)f.stream;
        const bool key = (f.flags & FRAME_KEY) != 0;
        const uint64_t ts = f.captureUs;
        Screen& s = screens[stream];
        int w, h;
        {
            std::lock_guard<std::mutex> lk(streamM);
            w = s.videoW;
            h = s.videoH;
        }
        if (w <= 0 || !s.active) return;
        if (net_.clockSynced()) {
            Screen::FrameInfo fi;
            fi.pts = (int64_t)ts;
            fi.recvUs = recvUs;
            fi.captureLocalUs = net_.serverToLocalUs((int64_t)f.captureUs);
            fi.convertMs = f.convertUs / 1000.0f;
            fi.encodeMs = f.encodeUs / 1000.0f;
            fi.queueMs = f.queueUs / 1000.0f;
            int64_t sentLocal = fi.captureLocalUs + f.convertUs + f.encodeUs + f.queueUs;
            fi.netMs = std::max<int64_t>(0, recvUs - sentLocal) / 1000.0f;
            std::lock_guard<std::mutex> lk(s.infoM);
            s.infos.push_back(fi);
            while (s.infos.size() > 64) s.infos.pop_front();
        }
        if (!s.decoder.submit(data, len, ts, key, w, h)) {
            if (!key) s.dropped++;
            int64_t now = monoUs();
            if (now - s.lastIdrRequest > 500000) {
                s.lastIdrRequest = now;
                SStreamId sid{(uint32_t)stream};
                net_.send(C_REQUEST_IDR, &sid, sizeof(sid));
            }
        } else {
            s.ready = true;
        }
    }
    void onVideoStop(int stream) override {
        Screen& s = screens[stream];
        s.active = false;
        s.ready = false;
        s.decoder.reset();
        layoutNeeded = true;
        toolbarDirty = true;
    }
    void onCursorPos(const SCursorPos& p) override {
        if (p.stream >= (uint32_t)kMaxStreams) return;
        Screen& s = screens[p.stream];
        s.curX = p.x;
        s.curY = p.y;
        s.curVisible = p.visible != 0;
        s.cursorDirty = true;
    }
    void onCursorShape(const SCursorShape& s, const uint8_t* rgba) override {
        std::lock_guard<std::mutex> lk(streamM);
        cursorW = s.width;
        cursorH = s.height;
        cursorHotX = s.hotX;
        cursorHotY = s.hotY;
        cursorRgba.assign(rgba, rgba + (size_t)s.width * s.height * 4);
        cursorShapeDirty = true;
        for (auto& sc : screens) sc.cursorDirty = true;
    }
    void onAudioConfig(const SAudioConfig& c) override { audio.open((int)c.sampleRate); }
    void onAudio(const int16_t* s, size_t frames) override { audio.push(s, frames); }

    // ============================================================== keyboard / modifiers (any thread)
    void sendKeyRaw(uint16_t vk, bool down) {
        CKey k{vk, (uint8_t)(down ? 1 : 0), 0};
        net_.send(C_KEY, &k, sizeof(k));
    }
    // Releases latched modifiers (Ctrl/Alt/Shift/Win from the toolbar) after one use.
    void releaseLatches() {
        uint32_t l = latched.exchange(0);
        for (int i = 0; i < 4; ++i)
            if (l & (1u << i)) sendKeyRaw(kLatchVk[i], false);
        if (l) toolbarDirty = true;
    }
    void sendKey(uint16_t vk, bool down) {
        sendKeyRaw(vk, down);
        bool isModifier = vk == 0xA0 || vk == 0xA1 || vk == 0xA2 || vk == 0xA3 || vk == 0xA4 || vk == 0xA5 ||
                          vk == 0x5B || vk == 0x5C;
        if (!down && !isModifier) releaseLatches();
    }
    void sendText(const std::string& utf8) {
        if (utf8.empty()) return;
        net_.send(C_TEXT, utf8.data(), utf8.size());
        releaseLatches();
    }
    void toggleLatch(int i) {
        uint32_t bit = 1u << i;
        uint32_t prev = latched.fetch_xor(bit);
        sendKeyRaw(kLatchVk[i], !(prev & bit));  // pressing Win twice opens the Start menu
        toolbarDirty = true;
    }

    void setKeyboardVisible(bool show) {
        if (midSetKeyboard) jni->CallVoidMethod(activity, midSetKeyboard, (jboolean)show);
        toolbarDirty = true;
    }
    bool keyboardVisible() { return midIsKeyboard && jni->CallBooleanMethod(activity, midIsKeyboard); }

    // ============================================================== settings
    std::string settingsPath() { return std::string(app_->activity->internalDataPath) + "/settings.txt"; }

    void loadSettings() {
        std::ifstream f(settingsPath());
        std::string key;
        while (f >> key) {
            if (key == "env") { int v; f >> v; if (v >= 0 && v < ENV_COUNT) envMode = (EnvMode)v; }
            else if (key == "curved") f >> curved;
            else if (key == "width") f >> homeWidth;
            else if (key == "distance") f >> screenDistance;
            else if (key == "monitors") f >> savedMonitorMask;
            else if (key == "desk") f >> deskOn;
            else if (key == "deskSize") f >> deskW >> deskD;
            else if (key == "deskPose")
                f >> deskPose.p.x >> deskPose.p.y >> deskPose.p.z >> deskPose.q.x >> deskPose.q.y >> deskPose.q.z >> deskPose.q.w;
            else std::getline(f, key);
        }
        homeWidth = std::min(8.0f, std::max(0.6f, homeWidth));
        screenDistance = std::min(8.0f, std::max(0.6f, screenDistance));
        for (auto& s : screens) s.widthM = homeWidth;
    }

    void saveSettings() {
        std::ofstream f(settingsPath());
        f << "env " << (int)envMode << "\ncurved " << curved << "\nwidth " << homeWidth << "\ndistance "
          << screenDistance << "\nmonitors " << savedMonitorMask << "\ndesk " << deskOn << "\ndeskSize " << deskW
          << " " << deskD << "\ndeskPose " << deskPose.p.x << " " << deskPose.p.y << " " << deskPose.p.z << " "
          << deskPose.q.x << " " << deskPose.q.y << " " << deskPose.q.z << " " << deskPose.q.w << "\n";
        settingsDirty = false;
        lastSettingsSave = monoUs();
    }

    // ============================================================== init
    bool initJni() {
        app_->activity->vm->AttachCurrentThread(&jni, nullptr);
        activity = app_->activity->clazz;
        jclass c = jni->GetObjectClass(activity);
        activityClass = (jclass)jni->NewGlobalRef(c);
        stringClass = (jclass)jni->NewGlobalRef(jni->FindClass("java/lang/String"));
        midSetKeyboard = jni->GetMethodID(activityClass, "setKeyboardVisible", "(Z)V");
        midIsKeyboard = jni->GetMethodID(activityClass, "isKeyboardVisible", "()Z");
        midRenderPanel = jni->GetStaticMethodID(activityClass, "renderPanel", "(Ljava/lang/String;Ljava/lang/String;II)[I");
        midRenderToolbar = jni->GetStaticMethodID(activityClass, "renderToolbar", "([Ljava/lang/String;IIII)[I");
        if (jni->ExceptionCheck()) {
            jni->ExceptionClear();
            LOGE("JNI method lookup failed");
        }
        return true;
    }

    template <typename T>
    void proc(const char* name, T& fn) {
        xrGetInstanceProcAddr(instance, name, (PFN_xrVoidFunction*)&fn);
    }

    bool initXr() {
        PFN_xrInitializeLoaderKHR initLoader = nullptr;
        xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction*)&initLoader);
        if (initLoader) {
            XrLoaderInitInfoAndroidKHR li{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
            li.applicationVM = app_->activity->vm;
            li.applicationContext = app_->activity->clazz;
            initLoader((XrLoaderInitInfoBaseHeaderKHR*)&li);
        }

        uint32_t extCount = 0;
        xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
        std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
        xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());
        auto has = [&](const char* n) {
            for (auto& e : exts)
                if (!strcmp(e.extensionName, n)) return true;
            return false;
        };
        std::vector<const char*> enabled = {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
                                            XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME};
        auto opt = [&](const char* n, bool& flag) {
            if ((flag = has(n))) enabled.push_back(n);
        };
        opt(XR_FB_PASSTHROUGH_EXTENSION_NAME, extPassthrough);
        opt(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME, extCylinder);
        opt(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME, extRefreshRate);
        opt(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME, extLayerSettings);
        opt(XR_EXT_HAND_TRACKING_EXTENSION_NAME, extHandTracking);
        if (extHandTracking) opt(XR_FB_HAND_TRACKING_AIM_EXTENSION_NAME, extHandAim);
        if (extPassthrough) opt(XR_FB_TRIANGLE_MESH_EXTENSION_NAME, extTriangleMesh);
        opt(XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME, extTimespec);
        LOGI("ext: passthrough=%d cylinder=%d refresh=%d layerSettings=%d hands=%d handAim=%d mesh=%d", extPassthrough,
             extCylinder, extRefreshRate, extLayerSettings, extHandTracking, extHandAim, extTriangleMesh);

        XrInstanceCreateInfoAndroidKHR android{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
        android.applicationVM = app_->activity->vm;
        android.applicationActivity = app_->activity->clazz;
        XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
        ci.next = &android;
        strcpy(ci.applicationInfo.applicationName, "MirRoar");
        ci.applicationInfo.applicationVersion = 2;
        strcpy(ci.applicationInfo.engineName, "MirRoar");
        ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
        ci.enabledExtensionCount = (uint32_t)enabled.size();
        ci.enabledExtensionNames = enabled.data();
        if (XR_FAILED(xrCreateInstance(&ci, &instance))) {
            LOGE("xrCreateInstance failed");
            return false;
        }
        XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
        sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        if (XR_FAILED(xrGetSystem(instance, &sgi, &systemId))) {
            LOGE("xrGetSystem failed");
            return false;
        }

        PFN_xrGetOpenGLESGraphicsRequirementsKHR getReq = nullptr;
        proc("xrGetOpenGLESGraphicsRequirementsKHR", getReq);
        XrGraphicsRequirementsOpenGLESKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
        getReq(instance, systemId, &req);

        if (extPassthrough) {
            proc("xrCreatePassthroughFB", xrCreatePassthroughFB_);
            proc("xrPassthroughStartFB", xrPassthroughStartFB_);
            proc("xrPassthroughPauseFB", xrPassthroughPauseFB_);
            proc("xrCreatePassthroughLayerFB", xrCreatePassthroughLayerFB_);
            proc("xrPassthroughLayerResumeFB", xrPassthroughLayerResumeFB_);
            proc("xrPassthroughLayerPauseFB", xrPassthroughLayerPauseFB_);
        }
        if (extTriangleMesh) {
            proc("xrCreateTriangleMeshFB", xrCreateTriangleMeshFB_);
            proc("xrCreateGeometryInstanceFB", xrCreateGeometryInstanceFB_);
            proc("xrGeometryInstanceSetTransformFB", xrGeometryInstanceSetTransformFB_);
        }
        if (extTimespec) proc("xrConvertTimeToTimespecTimeKHR", xrConvertTimeToTimespecTimeKHR_);
        if (extHandTracking) {
            proc("xrCreateHandTrackerEXT", xrCreateHandTrackerEXT_);
            proc("xrLocateHandJointsEXT", xrLocateHandJointsEXT_);
        }
        return true;
    }

    bool initEgl() {
        eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        eglInitialize(eglDisplay, nullptr, nullptr);
        const EGLint attribs[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                  EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                  EGL_NONE};
        EGLint n = 0;
        eglChooseConfig(eglDisplay, attribs, &eglConfig, 1, &n);
        if (n == 0) {
            LOGE("no EGL config");
            return false;
        }
        const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        eglContext = eglCreateContext(eglDisplay, eglConfig, EGL_NO_CONTEXT, ctxAttribs);
        const EGLint pb[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        eglSurface = eglCreatePbufferSurface(eglDisplay, eglConfig, pb);
        if (!eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext)) {
            LOGE("eglMakeCurrent failed");
            return false;
        }
        p_eglGetNativeClientBufferANDROID =
            (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID");
        p_eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
        p_eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
        p_glEGLImageTargetTexture2DOES =
            (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
        if (!renderer.init()) return false;
        glGenTextures(1, &videoTex);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, videoTex);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        return true;
    }

    bool createSwapchain(Swapchain& sc, int w, int h) {
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        ci.format = colorFormat;
        ci.sampleCount = 1;
        ci.width = w;
        ci.height = h;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        if (XR_FAILED(xrCreateSwapchain(session, &ci, &sc.handle))) {
            LOGE("xrCreateSwapchain %dx%d failed", w, h);
            return false;
        }
        sc.width = w;
        sc.height = h;
        uint32_t n = 0;
        xrEnumerateSwapchainImages(sc.handle, 0, &n, nullptr);
        sc.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        xrEnumerateSwapchainImages(sc.handle, n, &n, (XrSwapchainImageBaseHeader*)sc.images.data());
        return true;
    }

    void destroySwapchain(Swapchain& sc) {
        if (sc.handle) xrDestroySwapchain(sc.handle);
        sc = Swapchain();
    }

    // Acquires a swapchain image and binds it as the render target.
    bool beginSwapchain(Swapchain& sc) {
        uint32_t idx;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(xrAcquireSwapchainImage(sc.handle, &ai, &idx))) return false;
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(sc.handle, &wi);
        glBindFramebuffer(GL_FRAMEBUFFER, renderer.framebufferFor(sc.images[idx].image));
        glViewport(0, 0, sc.width, sc.height);
        return true;
    }
    void endSwapchain(Swapchain& sc) {
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(sc.handle, &ri);
    }

    XrPath path(const char* s) {
        XrPath p;
        xrStringToPath(instance, s, &p);
        return p;
    }

    void initActions() {
        XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
        strcpy(asci.actionSetName, "main");
        strcpy(asci.localizedActionSetName, "Main");
        xrCreateActionSet(instance, &asci, &actionSet);
        hands[0].path = path("/user/hand/left");
        hands[1].path = path("/user/hand/right");
        XrPath both[2] = {hands[0].path, hands[1].path};
        auto mk = [&](XrAction& a, const char* name, XrActionType type) {
            XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
            strcpy(ci.actionName, name);
            strcpy(ci.localizedActionName, name);
            ci.actionType = type;
            ci.countSubactionPaths = 2;
            ci.subactionPaths = both;
            XR_CHECK(xrCreateAction(actionSet, &ci, &a));
        };
        mk(actAim, "aim", XR_ACTION_TYPE_POSE_INPUT);
        mk(actTrigger, "trigger", XR_ACTION_TYPE_FLOAT_INPUT);
        mk(actSqueeze, "squeeze", XR_ACTION_TYPE_FLOAT_INPUT);
        mk(actStick, "thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT);
        mk(actPrimary, "primary", XR_ACTION_TYPE_BOOLEAN_INPUT);
        mk(actSecondary, "secondary", XR_ACTION_TYPE_BOOLEAN_INPUT);
        mk(actStickClick, "thumbstick_click", XR_ACTION_TYPE_BOOLEAN_INPUT);
        mk(actMenu, "menu", XR_ACTION_TYPE_BOOLEAN_INPUT);
        mk(actHaptic, "haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT);

        std::vector<XrActionSuggestedBinding> touch = {
            {actAim, path("/user/hand/left/input/aim/pose")},
            {actAim, path("/user/hand/right/input/aim/pose")},
            {actTrigger, path("/user/hand/left/input/trigger/value")},
            {actTrigger, path("/user/hand/right/input/trigger/value")},
            {actSqueeze, path("/user/hand/left/input/squeeze/value")},
            {actSqueeze, path("/user/hand/right/input/squeeze/value")},
            {actStick, path("/user/hand/left/input/thumbstick")},
            {actStick, path("/user/hand/right/input/thumbstick")},
            {actStickClick, path("/user/hand/left/input/thumbstick/click")},
            {actStickClick, path("/user/hand/right/input/thumbstick/click")},
            {actPrimary, path("/user/hand/left/input/x/click")},
            {actPrimary, path("/user/hand/right/input/a/click")},
            {actSecondary, path("/user/hand/left/input/y/click")},
            {actSecondary, path("/user/hand/right/input/b/click")},
            {actMenu, path("/user/hand/left/input/menu/click")},
            {actHaptic, path("/user/hand/left/output/haptic")},
            {actHaptic, path("/user/hand/right/output/haptic")},
        };
        XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
        sb.countSuggestedBindings = (uint32_t)touch.size();
        sb.suggestedBindings = touch.data();
        XR_CHECK(xrSuggestInteractionProfileBindings(instance, &sb));

        if (!extHandAim) {
            // Fallback for hands: the generic simple controller profile (pinch = select).
            std::vector<XrActionSuggestedBinding> simple = {
                {actAim, path("/user/hand/left/input/aim/pose")},
                {actAim, path("/user/hand/right/input/aim/pose")},
                {actTrigger, path("/user/hand/left/input/select/click")},
                {actTrigger, path("/user/hand/right/input/select/click")},
            };
            sb.interactionProfile = path("/interaction_profiles/khr/simple_controller");
            sb.countSuggestedBindings = (uint32_t)simple.size();
            sb.suggestedBindings = simple.data();
            XR_CHECK(xrSuggestInteractionProfileBindings(instance, &sb));
        }

        for (int h = 0; h < 2; ++h) {
            XrActionSpaceCreateInfo sci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            sci.action = actAim;
            sci.subactionPath = hands[h].path;
            sci.poseInActionSpace.orientation.w = 1;
            XR_CHECK(xrCreateActionSpace(session, &sci, &hands[h].aimSpace));
        }
        XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        ai.countActionSets = 1;
        ai.actionSets = &actionSet;
        XR_CHECK(xrAttachSessionActionSets(session, &ai));
    }

    void initPassthrough() {
        if (!extPassthrough || !xrCreatePassthroughFB_) return;
        XrPassthroughCreateInfoFB pci{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
        if (XR_FAILED(xrCreatePassthroughFB_(session, &pci, &passthrough))) {
            passthrough = XR_NULL_HANDLE;
            return;
        }
        XrPassthroughLayerCreateInfoFB lci{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
        lci.passthrough = passthrough;
        lci.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
        if (XR_FAILED(xrCreatePassthroughLayerFB_(session, &lci, &reconLayer))) reconLayer = XR_NULL_HANDLE;

        if (!extTriangleMesh || !xrCreateTriangleMeshFB_) return;
        lci.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_PROJECTED_FB;
        if (XR_FAILED(xrCreatePassthroughLayerFB_(session, &lci, &deskLayer))) {
            deskLayer = XR_NULL_HANDLE;
            return;
        }
        // unit square on the XZ plane, scaled to the desk size by the instance transform
        static const XrVector3f verts[4] = {{-0.5f, 0, -0.5f}, {0.5f, 0, -0.5f}, {0.5f, 0, 0.5f}, {-0.5f, 0, 0.5f}};
        static const uint32_t idx[6] = {0, 1, 2, 2, 3, 0};
        XrTriangleMeshCreateInfoFB mci{XR_TYPE_TRIANGLE_MESH_CREATE_INFO_FB};
        mci.windingOrder = XR_WINDING_ORDER_UNKNOWN_FB;
        mci.vertexCount = 4;
        mci.vertexBuffer = verts;
        mci.triangleCount = 2;
        mci.indexBuffer = idx;
        if (XR_FAILED(xrCreateTriangleMeshFB_(session, &mci, &deskMesh))) return;
        XrGeometryInstanceCreateInfoFB gci{XR_TYPE_GEOMETRY_INSTANCE_CREATE_INFO_FB};
        gci.layer = deskLayer;
        gci.mesh = deskMesh;
        gci.baseSpace = deskSpace();
        gci.pose = deskPose;
        gci.scale = {deskW, 1, deskD};
        if (XR_FAILED(xrCreateGeometryInstanceFB_(session, &gci, &deskGeometry))) deskGeometry = XR_NULL_HANDLE;
        LOGI("passthrough: recon=%d desk=%d", reconLayer != XR_NULL_HANDLE, deskGeometry != XR_NULL_HANDLE);
    }

    void initHands() {
        if (!extHandTracking || !xrCreateHandTrackerEXT_) return;
        for (int h = 0; h < 2; ++h) {
            XrHandTrackerCreateInfoEXT ci{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
            ci.hand = h == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
            ci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
            if (XR_FAILED(xrCreateHandTrackerEXT_(session, &ci, &handTrackers[h]))) handTrackers[h] = XR_NULL_HANDLE;
        }
    }

    bool initSession() {
        XrGraphicsBindingOpenGLESAndroidKHR gb{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
        gb.display = eglDisplay;
        gb.config = eglConfig;
        gb.context = eglContext;
        XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
        sci.next = &gb;
        sci.systemId = systemId;
        if (XR_FAILED(xrCreateSession(instance, &sci, &session))) {
            LOGE("xrCreateSession failed");
            return false;
        }

        XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        rs.poseInReferenceSpace.orientation.w = 1;
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        xrCreateReferenceSpace(session, &rs, &appSpace);
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        xrCreateReferenceSpace(session, &rs, &viewSpace);
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
        if (XR_FAILED(xrCreateReferenceSpace(session, &rs, &stageSpace))) stageSpace = XR_NULL_HANDLE;

        uint32_t fmtCount = 0;
        xrEnumerateSwapchainFormats(session, 0, &fmtCount, nullptr);
        std::vector<int64_t> fmts(fmtCount);
        xrEnumerateSwapchainFormats(session, fmtCount, &fmtCount, fmts.data());
        colorFormat = std::find(fmts.begin(), fmts.end(), (int64_t)GL_SRGB8_ALPHA8) != fmts.end() ? GL_SRGB8_ALPHA8
                                                                                                    : fmts[0];

        uint32_t vc = 0;
        xrEnumerateViewConfigurationViews(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &vc, nullptr);
        viewConfigs.assign(vc, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
        xrEnumerateViewConfigurationViews(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, vc, &vc,
                                          viewConfigs.data());
        for (int i = 0; i < 2; ++i) {
            // The environment is simple and smooth: 80% of the recommended size is plenty.
            int w = (int)(viewConfigs[i].recommendedImageRectWidth * 0.8f);
            int h = (int)(viewConfigs[i].recommendedImageRectHeight * 0.8f);
            if (!createSwapchain(eyeSwapchains[i], w, h)) return false;
        }
        createSwapchain(toolbarSwapchain, kToolbarW, kToolbarH);

        initActions();
        initPassthrough();
        initHands();
        return true;
    }

    XrSpace deskSpace() const { return stageSpace ? stageSpace : appSpace; }

    // Starts/pauses passthrough and its two layers according to the environment and the desk window.
    void updatePassthroughState() {
        if (!passthrough) return;
        bool wantRecon = envMode == ENV_PASSTHROUGH && reconLayer;
        bool wantDesk = deskOn && envMode != ENV_PASSTHROUGH && deskGeometry;
        bool wantRun = wantRecon || wantDesk;
        if (wantRun && !ptRunning) {
            xrPassthroughStartFB_(passthrough);
            ptRunning = true;
        }
        if (reconLayer && wantRecon != reconResumed) {
            (wantRecon ? xrPassthroughLayerResumeFB_ : xrPassthroughLayerPauseFB_)(reconLayer);
            reconResumed = wantRecon;
        }
        if (deskLayer && wantDesk != deskResumed) {
            (wantDesk ? xrPassthroughLayerResumeFB_ : xrPassthroughLayerPauseFB_)(deskLayer);
            deskResumed = wantDesk;
        }
        if (!wantRun && ptRunning) {
            xrPassthroughPauseFB_(passthrough);
            ptRunning = false;
        }
    }

    bool init() {
        initJni();
        loadSettings();
        if (!initXr()) return false;
        if (!initEgl()) return false;
        if (!initSession()) return false;
        CHello hello{};
        hello.version = kProtocolVersion;
        hello.maxWidth = 3840;
        hello.maxHeight = 2160;
        hello.monitorMask = savedMonitorMask;
        strcpy(hello.deviceName, "Meta Quest");
        net_.start(hello);
        return true;
    }

    void shutdown() {
        if (settingsDirty) saveSettings();
        for (auto& s : screens) retireScreenImage(s);
        flushRetired(true);
        net_.stop();
        for (auto& s : screens) s.decoder.reset();
        audio.close();
        if (session) xrDestroySession(session);
        if (instance) xrDestroyInstance(instance);
        if (eglDisplay != EGL_NO_DISPLAY) {
            eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroySurface(eglDisplay, eglSurface);
            eglDestroyContext(eglDisplay, eglContext);
            eglTerminate(eglDisplay);
        }
        app_->activity->vm->DetachCurrentThread();
    }

    // ============================================================== events
    void pollXrEvents() {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(instance, &ev) == XR_SUCCESS) {
            switch (ev.type) {
                case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                    auto* e = (XrEventDataSessionStateChanged*)&ev;
                    // Focus comes back after the system keyboard was dismissed: keep our flag in sync.
                    if (sessionState == XR_SESSION_STATE_VISIBLE && e->state == XR_SESSION_STATE_FOCUSED)
                        setKeyboardVisible(false);
                    sessionState = e->state;
                    LOGI("session state %d", sessionState);
                    if (sessionState == XR_SESSION_STATE_READY) {
                        XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                        bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                        if (XR_SUCCEEDED(xrBeginSession(session, &bi))) {
                            sessionRunning = true;
                            if (extRefreshRate) {
                                PFN_xrRequestDisplayRefreshRateFB req = nullptr;
                                proc("xrRequestDisplayRefreshRateFB", req);
                                if (req) req(session, 90.0f);
                            }
                            updatePassthroughState();
                        }
                    } else if (sessionState == XR_SESSION_STATE_STOPPING) {
                        xrEndSession(session);
                        sessionRunning = false;
                    } else if (sessionState == XR_SESSION_STATE_EXITING || sessionState == XR_SESSION_STATE_LOSS_PENDING) {
                        exitRequested = true;
                    }
                    break;
                }
                case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
                    recenterRequested = true;
                    break;
                case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                    exitRequested = true;
                    break;
                default:
                    break;
            }
            ev = {XR_TYPE_EVENT_DATA_BUFFER};
        }
    }

    // ============================================================== layout
    bool anyReady() const {
        for (auto& s : screens)
            if (s.active && s.ready && s.hasContent) return true;
        return false;
    }

    // The screen the toolbar hangs from: the PC's primary monitor if shown, else the first visible one.
    int anchorScreen() {
        std::lock_guard<std::mutex> lk(streamM);
        int first = -1;
        for (int i = 0; i < kMaxStreams; ++i) {
            if (!screens[i].active || !screens[i].hasContent) continue;
            if (first < 0) first = i;
            if (i < (int)monitors.size() && monitors[i].primary) return i;
        }
        return first;
    }

    void recenter(const Pose& head) {
        Vec3 fwd = rotate(head.q, Vec3(0, 0, -1));
        fwd.y = 0;
        fwd = normalize(fwd);
        homePose.p = head.p + fwd * screenDistance;
        homePose.q = lookRotation(-fwd);
        placed = true;
        layoutNeeded = true;
    }

    // Arranges the active screens side by side around the user, in the PCs' monitor order,
    // keeping the anchor (primary) screen at the home position.
    void layoutScreens(const Pose& head) {
        std::vector<int> order;
        std::vector<SMonitorInfo> mons;
        {
            std::lock_guard<std::mutex> lk(streamM);
            mons = monitors;
        }
        for (int i = 0; i < kMaxStreams; ++i)
            if (screens[i].active) order.push_back(i);
        if (order.empty()) return;
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            int xa = a < (int)mons.size() ? mons[a].x : a * 10000, xb = b < (int)mons.size() ? mons[b].x : b * 10000;
            return xa < xb;
        });
        int anchor = order[0];
        for (int i : order)
            if (i < (int)mons.size() && mons[i].primary) anchor = i;

        Vec3 toHome = homePose.p - head.p;
        float baseYaw = dirYaw(Vec3(toHome.x, 0, toHome.z));
        float d = std::max(0.6f, length(Vec3(toHome.x, 0, toHome.z)));
        const float gap = 0.06f;
        // angular width of each screen, then offsets relative to the anchor
        std::vector<float> ang(order.size());
        for (size_t k = 0; k < order.size(); ++k) {
            Screen& s = screens[order[k]];
            float w = s.widthM;
            ang[k] = curved ? (w + gap) / d : 2.0f * atanf((w + gap) * 0.5f / d);
        }
        size_t ai = std::find(order.begin(), order.end(), anchor) - order.begin();
        float yaw = baseYaw;
        for (size_t k = ai; k < order.size(); ++k) {
            if (k > ai) yaw += (ang[k - 1] + ang[k]) * 0.5f;
            Screen& s = screens[order[k]];
            Vec3 f = yawDir(yaw);
            s.pose.p = Vec3(head.p.x, homePose.p.y, head.p.z) + f * d;
            s.pose.q = lookRotation(-f);
            s.radius = d;
        }
        yaw = baseYaw;
        for (size_t k = ai; k-- > 0;) {
            yaw -= (ang[k + 1] + ang[k]) * 0.5f;
            Screen& s = screens[order[k]];
            Vec3 f = yawDir(yaw);
            s.pose.p = Vec3(head.p.x, homePose.p.y, head.p.z) + f * d;
            s.pose.q = lookRotation(-f);
            s.radius = d;
        }
    }

    // Pose/size of the status panel and of the toolbar
    float panelHeightM() const { return homeWidth * 9.0f / 16.0f; }
    bool toolbarPose(Pose& pose, float& w, float& h) {
        int a = anyReady() ? anchorScreen() : -1;
        Pose base = a >= 0 ? screens[a].pose : homePose;
        float sh = a >= 0 ? screens[a].heightM() : panelHeightM();
        float sw = a >= 0 ? screens[a].widthM : homeWidth;
        w = std::min(1.7f, std::max(1.1f, sw * 0.75f));
        h = w * kToolbarH / kToolbarW;
        pose = Pose(base.q, transformPoint(base, Vec3(0, -(sh * 0.5f + kBarGap * 2 + h * 0.5f), 0)));
        return placed;
    }
    Pose barPose(int i) {
        const Pose& base = i >= 0 ? screens[i].pose : homePose;
        float sh = i >= 0 ? screens[i].heightM() : panelHeightM();
        return Pose(base.q, transformPoint(base, Vec3(0, -(sh * 0.5f + kBarGap), 0)));
    }

    // ============================================================== hit testing
    static bool hitRect(const Pose& pose, float w, float h, const Vec3& o, const Vec3& d, float& u, float& v, float& t) {
        Pose inv = inverse(pose);
        Vec3 lo = transformPoint(inv, o);
        Vec3 ld = rotate(inv.q, d);
        if (fabsf(ld.z) < 1e-6f) return false;
        t = -lo.z / ld.z;
        if (t <= 0) return false;
        Vec3 p = lo + ld * t;
        u = p.x / w + 0.5f;
        v = 0.5f - p.y / h;
        return u >= 0 && u <= 1 && v >= 0 && v <= 1;
    }
    bool hitScreen(const Screen& s, const Vec3& o, const Vec3& d, float& u, float& v, float& t) {
        float w = s.widthM, h = s.heightM();
        if (!(curved && extCylinder)) return hitRect(s.pose, w, h, o, d, u, v, t);
        Pose inv = inverse(s.pose);
        Vec3 lo = transformPoint(inv, o);
        Vec3 ld = rotate(inv.q, d);
        float r = std::max(s.radius, 1.0f);
        Vec3 oc = lo - Vec3(0, 0, r);
        float a = ld.x * ld.x + ld.z * ld.z;
        float b = 2 * (oc.x * ld.x + oc.z * ld.z);
        float cc = oc.x * oc.x + oc.z * oc.z - r * r;
        float disc = b * b - 4 * a * cc;
        if (a < 1e-6f || disc < 0) return false;
        t = (-b + sqrtf(disc)) / (2 * a);  // far intersection (we are inside the cylinder)
        if (t <= 0) return false;
        Vec3 p = oc + ld * t;
        u = atan2f(p.x, -p.z) / (w / r) + 0.5f;
        v = 0.5f - p.y / h;
        return u >= 0 && u <= 1 && v >= 0 && v <= 1;
    }

    Hit pick(const Vec3& o, const Vec3& d) {
        Hit best;
        float u, v, t;
        bool ready = anyReady();
        for (int i = 0; i < kMaxStreams; ++i) {
            Screen& s = screens[i];
            if (!ready || !s.active || !s.hasContent) continue;
            if (hitScreen(s, o, d, u, v, t) && t < best.t) best = {HIT_SCREEN, i, u, v, t, o + d * t};
            if (hitRect(barPose(i), kBarW, kBarH * 2.5f, o, d, u, v, t) && t < best.t)
                best = {HIT_BAR, i, u, v, t, o + d * t};
        }
        if (!ready && placed) {
            if (hitRect(homePose, homeWidth, panelHeightM(), o, d, u, v, t) && t < best.t)
                best = {HIT_SCREEN, -1, u, v, t, o + d * t};
            if (hitRect(barPose(-1), kBarW, kBarH * 2.5f, o, d, u, v, t) && t < best.t)
                best = {HIT_BAR, -1, u, v, t, o + d * t};
        }
        Pose tp;
        float tw, th;
        if (toolbarPose(tp, tw, th) && hitRect(tp, tw, th, o, d, u, v, t) && t < best.t)
            best = {HIT_TOOLBAR, -1, u, v, t, o + d * t};
        return best;
    }

    // ============================================================== input
    void haptic(int h, float amp, int64_t durNs) {
        XrHapticVibration vib{XR_TYPE_HAPTIC_VIBRATION};
        vib.amplitude = amp;
        vib.duration = durNs;
        vib.frequency = XR_FREQUENCY_UNSPECIFIED;
        XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
        hi.action = actHaptic;
        hi.subactionPath = hands[h].path;
        xrApplyHapticFeedback(session, &hi, (XrHapticBaseHeader*)&vib);
    }

    void sendMouseButton(int b, bool down) {
        CMouseButton m{(uint8_t)b, (uint8_t)(down ? 1 : 0)};
        net_.send(C_MOUSE_BUTTON, &m, sizeof(m));
        if (!down) releaseLatches();
    }

    void selectNextMonitorSet() {
        std::vector<SMonitorInfo> mons;
        {
            std::lock_guard<std::mutex> lk(streamM);
            mons = monitors;
        }
        if (mons.size() < 2) return;
        uint32_t current = 0;
        for (int i = 0; i < kMaxStreams; ++i)
            if (screens[i].active) current |= 1u << i;
        std::vector<uint32_t> options;
        for (size_t i = 0; i < mons.size(); ++i) options.push_back(1u << i);
        options.push_back((1u << mons.size()) - 1);
        size_t idx = std::find(options.begin(), options.end(), current) - options.begin();
        uint32_t next = options[(idx + 1) % options.size()];
        CSelectMonitors sm{next};
        net_.send(C_SELECT_MONITORS, &sm, sizeof(sm));
        savedMonitorMask = next;
        net_.setMonitorMask(next);
        settingsDirty = true;
    }

    void cycleEnvironment() {
        envMode = (EnvMode)((envMode + 1) % ENV_COUNT);
        if (envMode == ENV_PASSTHROUGH && !reconLayer) envMode = ENV_STUDIO;
        updatePassthroughState();
        settingsDirty = true;
        toolbarDirty = true;
    }

    void toggleCurved(const Pose& head) {
        if (!extCylinder) return;
        curved = !curved;
        layoutScreens(head);
        settingsDirty = true;
        toolbarDirty = true;
    }

    void toolbarAction(int b, const Pose& head) {
        switch (b) {
            case TB_KEYBOARD: setKeyboardVisible(!keyboardVisible()); break;
            case TB_ESC: sendKey(0x1B, true); sendKey(0x1B, false); break;
            case TB_TAB: sendKey(0x09, true); sendKey(0x09, false); break;
            case TB_CTRL: toggleLatch(0); break;
            case TB_ALT: toggleLatch(1); break;
            case TB_SHIFT: toggleLatch(2); break;
            case TB_WIN: toggleLatch(3); break;
            case TB_SCREEN: selectNextMonitorSet(); break;
            case TB_ENV: cycleEnvironment(); break;
            case TB_CURVED: toggleCurved(head); break;
            case TB_RECENTER: recenter(head); break;
            case TB_DESK:
                if (!deskGeometry) break;
                if (deskOn && !deskPlacing) {
                    deskOn = false;
                } else {
                    deskOn = true;
                    deskPlacing = true;
                }
                updatePassthroughState();
                settingsDirty = true;
                break;
        }
        toolbarDirty = true;
    }

    void readHands(XrTime time) {
        for (int h = 0; h < 2; ++h) {
            handTracked[h] = false;
            if (!handTrackers[h] || !xrLocateHandJointsEXT_) continue;
            XrHandJointsLocateInfoEXT li{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
            li.baseSpace = appSpace;
            li.time = time;
            XrHandJointLocationsEXT locs{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
            locs.jointCount = XR_HAND_JOINT_COUNT_EXT;
            locs.jointLocations = joints[h];
            handAim[h] = {XR_TYPE_HAND_TRACKING_AIM_STATE_FB};
            if (extHandAim) locs.next = &handAim[h];
            if (XR_SUCCEEDED(xrLocateHandJointsEXT_(handTrackers[h], &li, &locs)) && locs.isActive) handTracked[h] = true;
        }
    }

    void updateInput(XrTime time, const Pose& head, float dt) {
        readHands(time);
        if (sessionState != XR_SESSION_STATE_FOCUSED) {
            // Keyboard or system UI has focus: release anything held.
            if (leftDown) sendMouseButton(MOUSE_LEFT, false), leftDown = false;
            if (rightDown) sendMouseButton(MOUSE_RIGHT, false), rightDown = false;
            grabHand = -1;
            grabTarget = -2;
            for (auto& hd : hands) hd.aimValid = false;
            hit = Hit();
            return;
        }
        XrActiveActionSet aas{actionSet, XR_NULL_PATH};
        XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
        si.countActiveActionSets = 1;
        si.activeActionSets = &aas;
        xrSyncActions(session, &si);

        for (int h = 0; h < 2; ++h) {
            HandInput& hd = hands[h];
            XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
            gi.subactionPath = hd.path;
            XrActionStateFloat fs{XR_TYPE_ACTION_STATE_FLOAT};
            gi.action = actTrigger;
            xrGetActionStateFloat(session, &gi, &fs);
            hd.trigger = fs.isActive ? fs.currentState : 0;
            gi.action = actSqueeze;
            xrGetActionStateFloat(session, &gi, &fs);
            hd.squeeze = fs.isActive ? fs.currentState : 0;
            XrActionStateVector2f vs{XR_TYPE_ACTION_STATE_VECTOR2F};
            gi.action = actStick;
            xrGetActionStateVector2f(session, &gi, &vs);
            hd.stick = vs.isActive ? vs.currentState : XrVector2f{0, 0};
            XrActionStateBoolean bs{XR_TYPE_ACTION_STATE_BOOLEAN};
            hd.prevPrimary = hd.primary;
            hd.prevSecondary = hd.secondary;
            hd.prevStickClick = hd.stickClick;
            hd.prevMenu = hd.menu;
            gi.action = actPrimary;
            xrGetActionStateBoolean(session, &gi, &bs);
            hd.primary = bs.isActive && bs.currentState;
            gi.action = actSecondary;
            xrGetActionStateBoolean(session, &gi, &bs);
            hd.secondary = bs.isActive && bs.currentState;
            gi.action = actStickClick;
            xrGetActionStateBoolean(session, &gi, &bs);
            hd.stickClick = bs.isActive && bs.currentState;
            gi.action = actMenu;
            xrGetActionStateBoolean(session, &gi, &bs);
            hd.menu = bs.isActive && bs.currentState;
            hd.ringPinch = false;

            XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
            hd.aimValid = false;
            hd.isHand = false;
            if (XR_SUCCEEDED(xrLocateSpace(hd.aimSpace, appSpace, time, &loc)) &&
                (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
                hd.aim = Pose(loc.pose);
                hd.aimValid = true;
            }

            // Tracked hand: index pinch = click, middle pinch = right click, ring pinch + move = scroll.
            if (handTracked[h] && extHandAim && (handAim[h].status & XR_HAND_TRACKING_AIM_VALID_BIT_FB)) {
                const XrHandTrackingAimStateFB& a = handAim[h];
                hd.isHand = true;
                hd.aim = Pose(a.aimPose);
                hd.aimValid = !(a.status & XR_HAND_TRACKING_AIM_SYSTEM_GESTURE_BIT_FB);
                hd.trigger = hd.aimValid ? a.pinchStrengthIndex : 0;
                hd.primary = hd.aimValid && (a.status & XR_HAND_TRACKING_AIM_MIDDLE_PINCHING_BIT_FB);
                hd.ringPinch = hd.aimValid && (a.status & XR_HAND_TRACKING_AIM_RING_PINCHING_BIT_FB);
                hd.squeeze = 0;
                hd.stick = {0, 0};
                hd.secondary = hd.stickClick = hd.menu = false;
            }
        }

        const int64_t now = monoUs();

        // ---- controller shortcuts (hands use the toolbar)
        for (int h = 0; h < 2; ++h) {
            HandInput& hd = hands[h];
            if (hd.isHand) continue;
            if (hd.secondary && !hd.prevSecondary) setKeyboardVisible(!keyboardVisible());
            if (hd.menu && !hd.prevMenu) cycleEnvironment();
            if (hd.stickClick && !hd.prevStickClick) {
                if (h == 1)
                    toggleCurved(head);
                else
                    recenter(head);
            }
        }

        // ---- active pointer: the other trigger/pinch switches hands
        for (int h = 0; h < 2; ++h) {
            HandInput& hd = hands[h];
            if (h == activeHand) continue;
            bool trig = hd.triggerHeld ? hd.trigger > 0.4f : hd.trigger > 0.75f;
            if (trig && !hd.triggerHeld && hd.aimValid) {
                if (leftDown) sendMouseButton(MOUSE_LEFT, false), leftDown = false;
                activeHand = h;
            }
            hd.triggerHeld = trig;
        }
        HandInput& ah = hands[activeHand];

        // ---- what the active pointer is aiming at
        Hit prevHit = hit;
        hit = Hit();
        if (ah.aimValid && grabHand < 0) {
            Vec3 o = ah.aim.p;
            Vec3 d = rotate(ah.aim.q, Vec3(0, 0, -1));
            hit = pick(o, d);
        }
        int newHover = hit.kind == HIT_TOOLBAR ? std::min(TB_COUNT - 1, (int)(hit.u * TB_COUNT)) : -1;
        if (newHover != hoverButton) {
            hoverButton = newHover;
            toolbarDirty = true;
        }

        bool trig = ah.triggerHeld ? ah.trigger > 0.4f : ah.trigger > 0.75f;
        bool trigDown = trig && !ah.triggerHeld;
        bool trigUp = !trig && ah.triggerHeld;
        ah.triggerHeld = trig;

        // ---- desk window placement: follow the pointer origin, confirm with a click
        if (deskPlacing && ah.aimValid) {
            Vec3 fwd = rotate(head.q, Vec3(0, 0, -1));
            fwd.y = 0;
            Pose appDesk(lookRotation(-normalize(fwd)), ah.aim.p - Vec3(0, ah.isHand ? 0.04f : 0.02f, 0));
            if (stageSpace) {
                XrSpaceLocation sl{XR_TYPE_SPACE_LOCATION};
                if (XR_SUCCEEDED(xrLocateSpace(stageSpace, appSpace, time, &sl)) &&
                    (sl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
                    appDesk = inverse(Pose(sl.pose)) * appDesk;
            }
            deskPose = appDesk;
            if (!ah.isHand) {
                deskW = std::min(1.5f, std::max(0.3f, deskW + (fabsf(ah.stick.x) > 0.2f ? ah.stick.x * dt * 0.5f : 0)));
                deskD = std::min(1.0f, std::max(0.2f, deskD + (fabsf(ah.stick.y) > 0.2f ? ah.stick.y * dt * 0.4f : 0)));
            }
            if (trigDown && hit.kind != HIT_TOOLBAR) {
                deskPlacing = false;
                settingsDirty = true;
                toolbarDirty = true;
                haptic(activeHand, 0.4f, 15000000);
                trigDown = false;
            }
        }

        // ---- grab: grip on a controller, or trigger/pinch on a screen's handle bar
        for (int h = 0; h < 2; ++h) {
            HandInput& hd = hands[h];
            bool sq = hd.squeezeHeld ? hd.squeeze > 0.35f : hd.squeeze > 0.6f;
            if (sq && !hd.squeezeHeld && grabHand < 0 && hd.aimValid) {
                Hit hh = pick(hd.aim.p, rotate(hd.aim.q, Vec3(0, 0, -1)));
                int target = (hh.kind == HIT_SCREEN || hh.kind == HIT_BAR) ? hh.index : (anyReady() ? anchorScreen() : -1);
                startGrab(h, target);
            }
            if (!sq && hd.squeezeHeld && grabHand == h && !hd.isHand) endGrab();
            hd.squeezeHeld = sq;
        }
        if (trigDown && hit.kind == HIT_BAR && grabHand < 0) {
            startGrab(activeHand, hit.index);
            trigDown = false;
        }
        if (grabHand == activeHand && hands[grabHand].squeeze < 0.35f && trigUp) endGrab();
        if (grabHand >= 0 && hands[grabHand].aimValid) {
            HandInput& hd = hands[grabHand];
            Pose& tp = grabTarget >= 0 ? screens[grabTarget].pose : homePose;
            float& tw = grabTarget >= 0 ? screens[grabTarget].widthM : homeWidth;
            float dy = fabsf(hd.stick.y) > 0.2f ? hd.stick.y : 0;
            float dx = fabsf(hd.stick.x) > 0.2f ? hd.stick.x : 0;
            float dist = length(grabOffset.p);
            float nd = std::min(8.0f, std::max(0.4f, dist * (1.0f + dy * dt * 1.5f)));
            if (dist > 1e-3f) grabOffset.p = grabOffset.p * (nd / dist);
            tw = std::min(10.0f, std::max(0.5f, tw * (1.0f + dx * dt * 1.2f)));
            Pose target = hd.aim * grabOffset;
            tp.p = target.p;
            tp.q = lookRotation(head.p - tp.p);
            float r = length(tp.p - head.p);
            if (grabTarget >= 0) {
                screens[grabTarget].radius = r;
                if (grabTarget == anchorScreen()) {
                    homePose = tp;
                    homeWidth = tw;
                    screenDistance = r;
                }
            } else {
                screenDistance = r;
            }
            settingsDirty = true;
        }

        // ---- toolbar buttons
        if (trigDown && hit.kind == HIT_TOOLBAR && hoverButton >= 0) {
            toolbarAction(hoverButton, head);
            haptic(activeHand, 0.3f, 10000000);
            trigDown = false;
        }

        // ---- mouse on a PC screen
        bool onScreen = hit.kind == HIT_SCREEN && hit.index >= 0 && connected;
        if (onScreen) {
            Screen& s = screens[hit.index];
            if (smoothStream != hit.index) {
                smoothU = hit.u;
                smoothV = hit.v;
                smoothStream = hit.index;
            }
            float pxW = (float)std::max(s.sc.width, 1), pxH = (float)std::max(s.sc.height, 1);
            float moved = sqrtf(powf((hit.u - smoothU) * pxW, 2) + powf((hit.v - smoothV) * pxH, 2));
            float a = std::min(1.0f, std::max(ah.isHand ? 0.12f : 0.2f, moved / (ah.isHand ? 16.0f : 10.0f)));
            smoothU += (hit.u - smoothU) * a;
            smoothV += (hit.v - smoothV) * a;
            float outU = smoothU, outV = smoothV;
            if (now < clickLockUntil) {
                float du = (smoothU - lockU) * pxW, dv = (smoothV - lockV) * pxH;
                float lockR = ah.isHand ? 35.0f : 20.0f;
                if (du * du + dv * dv < lockR * lockR) {
                    outU = lockU;
                    outV = lockV;
                } else {
                    clickLockUntil = 0;
                }
            }
            float du = (outU - sentU) * pxW, dv = (outV - sentV) * pxH;
            if (sentStream != hit.index || du * du + dv * dv > 0.25f) {
                CMouseMove mm{(uint32_t)hit.index, outU, outV};
                net_.send(C_MOUSE_MOVE, &mm, sizeof(mm));
                sentU = outU;
                sentV = outV;
                sentStream = hit.index;
            }
            lastLocalPointerUs = now;
            if (trigDown) {
                sendMouseButton(MOUSE_LEFT, true);
                leftDown = true;
                clickLockUntil = now + (ah.isHand ? 400000 : 300000);
                lockU = outU;
                lockV = outV;
                haptic(activeHand, 0.25f, 8000000);
            }
            if (ah.primary && !ah.prevPrimary) {
                sendMouseButton(MOUSE_RIGHT, true);
                rightDown = true;
                clickLockUntil = now + 300000;
                lockU = outU;
                lockV = outV;
                haptic(activeHand, 0.25f, 8000000);
            }
        }
        if (trigUp && leftDown) {
            sendMouseButton(MOUSE_LEFT, false);
            leftDown = false;
        }
        if (!ah.primary && rightDown) {
            sendMouseButton(MOUSE_RIGHT, false);
            rightDown = false;
        }
        (void)prevHit;

        // ---- scrolling: controller thumbstick, or ring pinch + vertical hand movement
        if (grabHand < 0 && connected && !deskPlacing) {
            float sy = fabsf(ah.stick.y) > 0.2f ? ah.stick.y : 0;
            float sx = fabsf(ah.stick.x) > 0.3f ? ah.stick.x : 0;
            if (ah.isHand) {
                if (ah.ringPinch && ah.ringPrevValid) sy = 0, wheelAccumY += (ah.aim.p.y - ah.ringPrevY) * -3500.0f;
                ah.ringPrevValid = ah.ringPinch;
                ah.ringPrevY = ah.aim.p.y;
                if (!ah.ringPinch) wheelAccumY = 0;
            } else {
                wheelAccumY += sy * dt * 1500.0f;
                wheelAccumX += sx * dt * 1500.0f;
                if (!sy) wheelAccumY = 0;
                if (!sx) wheelAccumX = 0;
            }
            int wy = (int)(wheelAccumY / 40) * 40, wx = (int)(wheelAccumX / 40) * 40;
            if (wy || wx) {
                wheelAccumY -= wy;
                wheelAccumX -= wx;
                CMouseWheel mw{(int16_t)wx, (int16_t)wy};
                net_.send(C_MOUSE_WHEEL, &mw, sizeof(mw));
            }
        }
    }

    void startGrab(int hand, int target) {
        if (target >= 0 && !screens[target].active) return;
        grabHand = hand;
        grabTarget = target;
        Pose& tp = target >= 0 ? screens[target].pose : homePose;
        grabOffset = inverse(hands[hand].aim) * tp;
        if (leftDown) sendMouseButton(MOUSE_LEFT, false), leftDown = false;
        haptic(hand, 0.4f, 15000000);
    }
    void endGrab() {
        grabHand = -1;
        grabTarget = -2;
    }

    // ============================================================== panel, toolbar and screen content
    void updatePanel() {
        std::string status = net_.status();
        std::string body;
        bool conn = connected;
        if (conn) {
            std::lock_guard<std::mutex> lk(streamM);
            body = "Connected to " + serverName + " (" + via + ")\nStarting the video stream...";
        } else {
            body = status.empty() ? "Starting..." : status;
            body += "\n\n#Start MirRoarServer.exe on the PC: it is found automatically\n#(same Wi-Fi/LAN network, or USB cable)";
        }
        body +=
            "\n\nControllers: trigger = click, A/X = right click, thumbstick = scroll, grip = move\n"
            "Hands: index pinch = click, middle pinch = right click,\n"
            "           ring pinch + move the hand = scroll\n"
            "Handle under the screen: grab and move. Toolbar: keyboard, keys, screens";
        std::string text = std::string("MirRoar\x01") + body;
        if (text == panelText && panelHasContent) return;
        panelText = text;

        const int W = 1600, H = 900;
        if (!panelSwapchain.handle) createSwapchain(panelSwapchain, W, H);
        jstring jt = jni->NewStringUTF("MirRoar");
        jstring jb = jni->NewStringUTF(body.c_str());
        jintArray arr = (jintArray)jni->CallStaticObjectMethod(activityClass, midRenderPanel, jt, jb, W, H);
        jni->DeleteLocalRef(jt);
        jni->DeleteLocalRef(jb);
        if (!arr || jni->ExceptionCheck()) {
            jni->ExceptionClear();
            return;
        }
        uploadArgb(arr, W, H, panelTex);
        if (!beginSwapchain(panelSwapchain)) return;
        renderer.drawTexture(panelTex, -1, -1, 1, 1);
        endSwapchain(panelSwapchain);
        panelHasContent = true;
    }

    void uploadArgb(jintArray arr, int W, int H, GLuint& tex) {
        std::vector<uint8_t> rgba((size_t)W * H * 4);
        jint* px = jni->GetIntArrayElements(arr, nullptr);
        for (size_t i = 0; i < (size_t)W * H; ++i) {
            uint32_t c = (uint32_t)px[i];  // ARGB
            rgba[i * 4 + 0] = (c >> 16) & 0xff;
            rgba[i * 4 + 1] = (c >> 8) & 0xff;
            rgba[i * 4 + 2] = c & 0xff;
            rgba[i * 4 + 3] = (c >> 24) & 0xff;
        }
        jni->ReleaseIntArrayElements(arr, px, JNI_ABORT);
        jni->DeleteLocalRef(arr);
        if (!tex)
            tex = Renderer::createTexture2D(W, H, rgba.data(), true);
        else
            Renderer::updateTexture2D(tex, W, H, rgba.data());
    }

    void updateToolbar() {
        std::vector<std::string> labels(TB_COUNT);
        labels[TB_KEYBOARD] = "Keyboard";
        labels[TB_ESC] = "Esc";
        labels[TB_TAB] = "Tab";
        labels[TB_CTRL] = "Ctrl";
        labels[TB_ALT] = "Alt";
        labels[TB_SHIFT] = "Shift";
        labels[TB_WIN] = "Win";
        int activeCount = 0, single = -1;
        for (int i = 0; i < kMaxStreams; ++i)
            if (screens[i].active) ++activeCount, single = i;
        size_t monCount;
        {
            std::lock_guard<std::mutex> lk(streamM);
            monCount = monitors.size();
        }
        if (!connected) labels[TB_SCREEN] = "Screen";
        else if (activeCount > 1) labels[TB_SCREEN] = "Screens: all";
        else labels[TB_SCREEN] = "Screen " + std::to_string(single + 1) + (monCount > 1 ? " / " + std::to_string(monCount) : "");
        labels[TB_ENV] = kEnvNames[envMode];
        labels[TB_CURVED] = curved ? "Curved" : "Flat";
        labels[TB_RECENTER] = "Recenter";
        labels[TB_DESK] = deskPlacing ? "Place it..." : "Desk";

        uint32_t mask = 0;
        if (keyboardVisible()) mask |= 1u << TB_KEYBOARD;
        uint32_t l = latched;
        if (l & 1) mask |= 1u << TB_CTRL;
        if (l & 2) mask |= 1u << TB_ALT;
        if (l & 4) mask |= 1u << TB_SHIFT;
        if (l & 8) mask |= 1u << TB_WIN;
        if (deskOn) mask |= 1u << TB_DESK;

        std::string key = std::to_string(mask) + "|" + std::to_string(hoverButton);
        for (auto& s : labels) key += "|" + s;
        if (key == toolbarKey && !toolbarDirty) return;
        toolbarDirty = false;
        if (key == toolbarKey) return;
        toolbarKey = key;

        jobjectArray arr = jni->NewObjectArray(TB_COUNT, stringClass, nullptr);
        for (int i = 0; i < TB_COUNT; ++i) {
            jstring s = jni->NewStringUTF(labels[i].c_str());
            jni->SetObjectArrayElement(arr, i, s);
            jni->DeleteLocalRef(s);
        }
        jintArray px = (jintArray)jni->CallStaticObjectMethod(activityClass, midRenderToolbar, arr, (jint)mask,
                                                              (jint)hoverButton, kToolbarW, kToolbarH);
        jni->DeleteLocalRef(arr);
        if (!px || jni->ExceptionCheck()) {
            jni->ExceptionClear();
            return;
        }
        uploadArgb(px, kToolbarW, kToolbarH, toolbarTex);
        if (!beginSwapchain(toolbarSwapchain)) return;
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        renderer.drawTexture(toolbarTex, -1, -1, 1, 1);
        endSwapchain(toolbarSwapchain);
    }

    // The frame `img` is about to be shown at displayUs: record its full journey.
    void accountLatency(Screen& s, int64_t pts) {
        if (displayUs == 0 || pts == 0) return;
        Screen::FrameInfo fi;
        {
            std::lock_guard<std::mutex> lk(s.infoM);
            auto it = std::find_if(s.infos.begin(), s.infos.end(),
                                   [&](const Screen::FrameInfo& f) { return f.pts == pts; });
            if (it == s.infos.end()) return;
            fi = *it;
            s.infos.erase(s.infos.begin(), it + 1);
        }
        int64_t decoded = s.decoder.decodedAt(pts);
        if (!decoded) decoded = monoUs();
        float decodeMs = std::max<int64_t>(0, decoded - fi.recvUs) / 1000.0f;
        float renderMs = std::max<int64_t>(0, displayUs - decoded) / 1000.0f;
        float totalMs = std::max<int64_t>(0, displayUs - fi.captureLocalUs) / 1000.0f;
        Screen::Acc& a = s.acc;
        a.frames++;
        a.convert += fi.convertMs;
        a.encode += fi.encodeMs;
        a.queue += fi.queueMs;
        a.net += fi.netMs;
        a.decode += decodeMs;
        a.render += renderMs;
        a.total += totalMs;
        a.maxTotal = std::max(a.maxTotal, totalMs);
    }

    void sendStats() {
        int64_t now = monoUs();
        if (now - lastStatsSend < 500000) return;
        uint32_t windowMs = lastStatsSend ? (uint32_t)((now - lastStatsSend) / 1000) : 500;
        lastStatsSend = now;
        if (!connected) return;
        for (int i = 0; i < kMaxStreams; ++i) {
            Screen& s = screens[i];
            if (!s.active) continue;
            CStats cs{};
            cs.stream = i;
            cs.windowMs = windowMs;
            cs.dropped = s.dropped.exchange(0);
            cs.rttMs = net_.rttMs();
            Screen::Acc& a = s.acc;
            cs.frames = a.frames;
            if (a.frames) {
                double k = 1.0 / a.frames;
                cs.convertMs = (float)(a.convert * k);
                cs.encodeMs = (float)(a.encode * k);
                cs.queueMs = (float)(a.queue * k);
                cs.networkMs = (float)(a.net * k);
                cs.decodeMs = (float)(a.decode * k);
                cs.renderMs = (float)(a.render * k);
                cs.totalMs = (float)(a.total * k);
                cs.maxTotalMs = a.maxTotal;
            }
            net_.send(C_STATS, &cs, sizeof(cs));
            a = Screen::Acc();
        }
    }

    void retireScreenImage(Screen& s) {
        if (s.image || s.eglImage != EGL_NO_IMAGE_KHR)
            retired.push_back({&s, s.image, s.imageGen, s.eglImage, frameIndex + 3});
        s.image = nullptr;
        s.imageGen = 0;
        s.eglImage = EGL_NO_IMAGE_KHR;
    }
    void flushRetired(bool all) {
        for (auto it = retired.begin(); it != retired.end();) {
            if (all || it->releaseAt <= frameIndex) {
                if (it->egl != EGL_NO_IMAGE_KHR) p_eglDestroyImageKHR(eglDisplay, it->egl);
                if (it->image) it->screen->decoder.release(it->image, it->gen);
                it = retired.erase(it);
            } else {
                ++it;
            }
        }
    }

    void updateScreen(int idx) {
        Screen& s = screens[idx];
        if (!s.active) {
            if (s.sc.handle) {
                retireScreenImage(s);
                destroySwapchain(s.sc);
                s.hasContent = false;
            }
            return;
        }
        int vw, vh, sw;
        {
            std::lock_guard<std::mutex> lk(streamM);
            vw = s.videoW;
            vh = s.videoH;
            sw = s.srcW;
        }
        if (vw <= 0) return;

        DecodedFrame df;
        if (s.decoder.acquireLatest(df)) {
            // The EGLImage takes its own reference on the buffer; ours is dropped right after.
            EGLClientBuffer cb = p_eglGetNativeClientBufferANDROID(df.buffer);
            const EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
            EGLImageKHR eimg = p_eglCreateImageKHR(eglDisplay, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, cb, attrs);
            AHardwareBuffer_release(df.buffer);
            if (eimg == EGL_NO_IMAGE_KHR) {
                s.decoder.release(df.image, df.generation);
            } else {
                accountLatency(s, df.ptsUs);
                retireScreenImage(s);
                s.image = df.image;
                s.imageGen = df.generation;
                s.uMax = df.uMax;
                s.vMax = df.vMax;
                s.eglImage = eimg;
                s.dirty = true;
                if (!s.tex) {
                    glGenTextures(1, &s.tex);
                    glBindTexture(GL_TEXTURE_EXTERNAL_OES, s.tex);
                    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                }
                glBindTexture(GL_TEXTURE_EXTERNAL_OES, s.tex);
                p_glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)eimg);
            }
        }
        if (s.eglImage == EGL_NO_IMAGE_KHR) return;

        // cursor: the local pointer while aiming at this screen, otherwise the PC's cursor
        bool local = sentStream == idx && monoUs() - lastLocalPointerUs < 300000;
        float cx = local ? sentU : s.curX.load();
        float cy = local ? sentV : s.curY.load();
        bool cvis = local || s.curVisible.load();
        if (!cvis) cx = cy = -10;
        if (cx != s.lastCurX || cy != s.lastCurY || s.cursorDirty.exchange(false)) s.dirty = true;
        if (!s.dirty && s.hasContent) return;

        if (s.sc.handle && (s.sc.width != vw || s.sc.height != vh)) destroySwapchain(s.sc);
        if (!s.sc.handle) {
            createSwapchain(s.sc, vw, vh);
            s.hasContent = false;
        }
        if (!beginSwapchain(s.sc)) return;
        renderer.drawVideo(s.tex, s.uMax, s.vMax);
        if (cursorTex && cvis) {
            float scale = (float)vw / (float)std::max(sw, 1) * 1.25f;  // a bit bigger: readable on a huge screen
            float px = cx * vw - cursorHotX * scale, py = cy * vh - cursorHotY * scale;
            float x0 = px / vw * 2 - 1, x1 = (px + cursorTexW * scale) / vw * 2 - 1;
            float y0 = 1 - py / vh * 2, y1 = 1 - (py + cursorTexH * scale) / vh * 2;
            renderer.drawTexture(cursorTex, x0, y1, x1, y0);
        }
        endSwapchain(s.sc);
        s.lastCurX = cx;
        s.lastCurY = cy;
        s.dirty = false;
        if (!s.hasContent) {
            s.hasContent = true;
            layoutNeeded = true;  // its height is known now
            toolbarDirty = true;
        }
    }

    // ============================================================== frame
    void drawHand(const Mat4& vp, const Vec3& eye, int h) {
        static const int bones[][2] = {{1, 2},   {2, 3},   {3, 4},   {4, 5},   {1, 6},   {6, 7},   {7, 8},
                                       {8, 9},   {9, 10},  {1, 11},  {11, 12}, {12, 13}, {13, 14}, {14, 15},
                                       {1, 16},  {16, 17}, {17, 18}, {18, 19}, {19, 20}, {1, 21},  {21, 22},
                                       {22, 23}, {23, 24}, {24, 25}, {7, 12},  {12, 17}, {17, 22}};
        const float col[3] = {0.85f, 0.9f, 1.0f};
        for (auto& b : bones) {
            const XrHandJointLocationEXT& a = joints[h][b[0]];
            const XrHandJointLocationEXT& c = joints[h][b[1]];
            if (!(a.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
                !(c.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
                continue;
            renderer.drawRay(vp, eye, a.pose.position, c.pose.position, 0.012f, col, 0.45f, 0.45f);
        }
    }

    void frame() {
        XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState fs{XR_TYPE_FRAME_STATE};
        if (XR_FAILED(xrWaitFrame(session, &fwi, &fs))) return;
        XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
        xrBeginFrame(session, &fbi);

        int64_t now = monoUs();
        float dt = lastFrameUs ? std::min(0.1f, (now - lastFrameUs) / 1e6f) : 0.011f;
        lastFrameUs = now;

        std::vector<XrCompositionLayerBaseHeader*> layers;
        XrCompositionLayerPassthroughFB reconL{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
        XrCompositionLayerPassthroughFB deskL{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
        XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        XrCompositionLayerProjectionView projViews[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                         {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        XrCompositionLayerQuad quads[kMaxStreams + 2];
        XrCompositionLayerCylinderKHR cyls[kMaxStreams];
        XrCompositionLayerSettingsFB settings{XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
        int nq = 0, nc = 0;

        if (fs.shouldRender) {
            XrSpaceLocation hl{XR_TYPE_SPACE_LOCATION};
            Pose head;
            bool headValid = XR_SUCCEEDED(xrLocateSpace(viewSpace, appSpace, fs.predictedDisplayTime, &hl)) &&
                             (hl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
            if (headValid) head = Pose(hl.pose);
            if (headValid && (!placed || recenterRequested)) {
                recenter(head);
                recenterRequested = false;
            }
            if (headValid && layoutNeeded.exchange(false)) layoutScreens(head);
            Pose stageInApp;
            bool stageValid = false;
            if (stageSpace) {
                XrSpaceLocation sl{XR_TYPE_SPACE_LOCATION};
                if (XR_SUCCEEDED(xrLocateSpace(stageSpace, appSpace, fs.predictedDisplayTime, &sl)) &&
                    (sl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
                    floorY = sl.pose.position.y;
                    stageInApp = Pose(sl.pose);
                    stageValid = true;
                }
            }

            updateInput(fs.predictedDisplayTime, head, dt);

            ++frameIndex;
            flushRetired(false);
            displayUs = 0;
            if (xrConvertTimeToTimespecTimeKHR_) {
                timespec ts;
                if (XR_SUCCEEDED(xrConvertTimeToTimespecTimeKHR_(instance, fs.predictedDisplayTime, &ts)))
                    displayUs = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
            }
            for (int i = 0; i < kMaxStreams; ++i) updateScreen(i);
            sendStats();
            bool ready = anyReady() && connected;
            if (!ready) updatePanel();
            updateToolbar();

            if (deskGeometry && deskOn && envMode != ENV_PASSTHROUGH) {
                XrGeometryInstanceTransformFB gt{XR_TYPE_GEOMETRY_INSTANCE_TRANSFORM_FB};
                gt.baseSpace = deskSpace();
                gt.time = fs.predictedDisplayTime;
                gt.pose = deskPose;
                gt.scale = {deskW, 1, deskD};
                xrGeometryInstanceSetTransformFB_(deskGeometry, &gt);
            }

            XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
            vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            vli.displayTime = fs.predictedDisplayTime;
            vli.space = appSpace;
            XrViewState vs{XR_TYPE_VIEW_STATE};
            XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
            uint32_t vc = 0;
            xrLocateViews(session, &vli, &vs, 2, &vc, views);

            bool pt = envMode == ENV_PASSTHROUGH && reconResumed;
            if (pt) {
                reconL.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                reconL.layerHandle = reconLayer;
                layers.push_back((XrCompositionLayerBaseHeader*)&reconL);
            }

            for (int i = 0; i < 2; ++i) {
                Swapchain& sc = eyeSwapchains[i];
                beginSwapchain(sc);
                glClearColor(0, 0, 0, 0);
                glClear(GL_COLOR_BUFFER_BIT);
                Pose eyePose(views[i].pose);
                Mat4 vp = mul(projectionFromFov(views[i].fov, 0.05f, 100.0f), invertRigid(poseToMatrix(eyePose)));
                if (!pt) renderer.drawEnvironment(envMode, vp, eyePose.p, floorY, (now - startUs) / 1e6f);

                // handle bars under the screens
                const float barIdle[3] = {0.85f, 0.87f, 0.92f}, barHot[3] = {0.35f, 0.65f, 1.0f};
                auto drawBar = [&](int idx) {
                    Pose bp = barPose(idx);
                    bool hot = (hit.kind == HIT_BAR && hit.index == idx) || (grabHand >= 0 && grabTarget == idx);
                    renderer.drawQuad(vp, bp.p, rotate(bp.q, Vec3(1, 0, 0)), rotate(bp.q, Vec3(0, 1, 0)), kBarW, kBarH,
                                      hot ? barHot : barIdle, hot ? 0.9f : 0.45f);
                };
                if (anyReady()) {
                    for (int s = 0; s < kMaxStreams; ++s)
                        if (screens[s].active && screens[s].hasContent) drawBar(s);
                } else if (placed) {
                    drawBar(-1);
                }

                // desk window outline while placing it
                if (deskPlacing) {
                    Pose dp = stageValid && stageSpace ? stageInApp * deskPose : deskPose;
                    Vec3 c[4] = {transformPoint(dp, Vec3(-deskW / 2, 0, -deskD / 2)),
                                 transformPoint(dp, Vec3(deskW / 2, 0, -deskD / 2)),
                                 transformPoint(dp, Vec3(deskW / 2, 0, deskD / 2)),
                                 transformPoint(dp, Vec3(-deskW / 2, 0, deskD / 2))};
                    const float oc[3] = {0.35f, 0.65f, 1.0f};
                    for (int k = 0; k < 4; ++k) renderer.drawRay(vp, eyePose.p, c[k], c[(k + 1) % 4], 0.006f, oc, 0.9f, 0.9f);
                }

                if (sessionState == XR_SESSION_STATE_FOCUSED) {
                    for (int h = 0; h < 2; ++h) {
                        HandInput& hd = hands[h];
                        if (hd.isHand && !pt && handTracked[h]) drawHand(vp, eyePose.p, h);
                        if (!hd.aimValid) continue;
                        Vec3 o = hd.aim.p;
                        Vec3 d = rotate(hd.aim.q, Vec3(0, 0, -1));
                        bool isActive = h == activeHand;
                        Vec3 end = (isActive && hit.kind != HIT_NONE) ? hit.point : o + d * (isActive ? 1.5f : 0.4f);
                        const float colActive[3] = {0.35f, 0.65f, 1.0f};
                        const float colGrab[3] = {1.0f, 0.75f, 0.3f};
                        const float colIdle[3] = {0.8f, 0.8f, 0.85f};
                        const float* col = grabHand == h ? colGrab : (isActive ? colActive : colIdle);
                        float a0 = isActive ? 0.9f : 0.4f;
                        renderer.drawRay(vp, eyePose.p, o + d * 0.03f, end, hd.isHand ? 0.003f : 0.004f, col, a0, 0.15f);
                    }
                }
                endSwapchain(sc);

                projViews[i].pose = views[i].pose;
                projViews[i].fov = views[i].fov;
                projViews[i].subImage.swapchain = sc.handle;
                projViews[i].subImage.imageRect = {{0, 0}, {sc.width, sc.height}};
            }
            proj.space = appSpace;
            proj.viewCount = 2;
            proj.views = projViews;
            if (pt)
                proj.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                  XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
            layers.push_back((XrCompositionLayerBaseHeader*)&proj);

            if (deskResumed) {
                deskL.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                deskL.layerHandle = deskLayer;
                layers.push_back((XrCompositionLayerBaseHeader*)&deskL);
            }

            if (extLayerSettings)
                settings.layerFlags =
                    XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SUPER_SAMPLING_BIT_FB | XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SHARPENING_BIT_FB;
            auto addQuad = [&](const Swapchain& sc, const Pose& p, float w, float h, bool alpha) {
                XrCompositionLayerQuad& q = quads[nq++];
                q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
                q.next = extLayerSettings ? &settings : nullptr;
                q.layerFlags = alpha ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
                q.space = appSpace;
                q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                q.subImage.swapchain = sc.handle;
                q.subImage.imageRect = {{0, 0}, {sc.width, sc.height}};
                q.pose = p;
                q.size = {w, h};
                layers.push_back((XrCompositionLayerBaseHeader*)&q);
            };

            if (placed) {
                if (ready) {
                    for (int i = 0; i < kMaxStreams; ++i) {
                        Screen& s = screens[i];
                        if (!s.active || !s.hasContent) continue;
                        if (curved && extCylinder) {
                            float r = std::max(s.radius, 1.0f);
                            XrCompositionLayerCylinderKHR& c = cyls[nc++];
                            c = {XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
                            c.next = extLayerSettings ? &settings : nullptr;
                            c.space = appSpace;
                            c.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                            c.subImage.swapchain = s.sc.handle;
                            c.subImage.imageRect = {{0, 0}, {s.sc.width, s.sc.height}};
                            c.pose = Pose(s.pose.q, transformPoint(s.pose, Vec3(0, 0, r)));
                            c.radius = r;
                            c.centralAngle = std::min(s.widthM / r, 6.0f);
                            c.aspectRatio = (float)s.sc.width / (float)s.sc.height;
                            layers.push_back((XrCompositionLayerBaseHeader*)&c);
                        } else {
                            addQuad(s.sc, s.pose, s.widthM, s.heightM(), false);
                        }
                    }
                } else if (panelHasContent) {
                    addQuad(panelSwapchain, homePose, homeWidth, panelHeightM(), false);
                }
                Pose tp;
                float tw, th;
                if (toolbarTex && toolbarPose(tp, tw, th)) addQuad(toolbarSwapchain, tp, tw, th, true);
            }
        }

        XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
        fei.displayTime = fs.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fei.layerCount = (uint32_t)layers.size();
        fei.layers = layers.data();
        xrEndFrame(session, &fei);

        if (settingsDirty && now - lastSettingsSave > 2000000 && grabHand < 0) saveSettings();
    }

    // cursor shape upload happens on the render thread
    void syncCursorTexture() {
        std::lock_guard<std::mutex> lk(streamM);
        if (!cursorShapeDirty) return;
        if (cursorTex) glDeleteTextures(1, &cursorTex);
        cursorTex = cursorRgba.empty() ? 0 : Renderer::createTexture2D(cursorW, cursorH, cursorRgba.data(), true);
        cursorTexW = cursorW;
        cursorTexH = cursorH;
        cursorShapeDirty = false;
    }

    // physical keyboard (Bluetooth/USB) through the native input queue
    bool onKeyEvent(AInputEvent* e) {
        int code = AKeyEvent_getKeyCode(e);
        int action = AKeyEvent_getAction(e);
        if (code == AKEYCODE_BACK) return false;
        uint16_t vk = androidKeyToVk(code);
        if (!vk || !connected) return false;
        if (action == AKEY_EVENT_ACTION_DOWN) sendKey(vk, true);
        else if (action == AKEY_EVENT_ACTION_UP) sendKey(vk, false);
        return true;
    }
};

static App* g_app = nullptr;

// ------------------------------------------------------------------ JNI from MainActivity (UI thread)

static std::string jstringToUtf8(JNIEnv* env, jstring s) {
    std::string out;
    const jchar* c = env->GetStringChars(s, nullptr);
    jsize n = env->GetStringLength(s);
    for (jsize i = 0; i < n; ++i) {
        uint32_t cp = c[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n && c[i + 1] >= 0xDC00 && c[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (c[i + 1] - 0xDC00);
            ++i;
        }
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }
    env->ReleaseStringChars(s, c);
    return out;
}

extern "C" JNIEXPORT void JNICALL Java_com_mirroar_app_MainActivity_nativeOnText(JNIEnv* env, jclass, jstring text) {
    if (g_app) g_app->sendText(jstringToUtf8(env, text));
}

extern "C" JNIEXPORT void JNICALL Java_com_mirroar_app_MainActivity_nativeOnKey(JNIEnv*, jclass, jint code,
                                                                               jboolean down, jint) {
    if (!g_app) return;
    uint16_t vk = androidKeyToVk(code);
    if (vk) g_app->sendKey(vk, down);
}

// ------------------------------------------------------------------ android_native_app_glue

static void onAppCmd(android_app* app, int32_t cmd) {
    App* a = (App*)app->userData;
    switch (cmd) {
        case APP_CMD_RESUME: a->resumed = true; break;
        case APP_CMD_PAUSE:
            a->resumed = false;
            if (a->settingsDirty) a->saveSettings();
            break;
        case APP_CMD_DESTROY: a->exitRequested = true; break;
        default: break;
    }
}

static int32_t onInputEvent(android_app* app, AInputEvent* e) {
    App* a = (App*)app->userData;
    if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_KEY) return a->onKeyEvent(e) ? 1 : 0;
    return 0;
}

void android_main(android_app* app) {
    App* a = new App(app);
    g_app = a;
    app->userData = a;
    app->onAppCmd = onAppCmd;
    app->onInputEvent = onInputEvent;

    if (!a->init()) {
        LOGE("initialization failed");
        ANativeActivity_finish(app->activity);
    }

    while (!app->destroyRequested) {
        for (;;) {
            int events;
            android_poll_source* src = nullptr;
            int timeout = (!a->resumed && !a->sessionRunning && !app->destroyRequested) ? 100 : 0;
            if (ALooper_pollOnce(timeout, nullptr, &events, (void**)&src) < 0) break;
            if (src) src->process(app, src);
            if (app->destroyRequested) break;
        }
        if (a->instance) a->pollXrEvents();
        if (a->exitRequested && !app->destroyRequested) {
            ANativeActivity_finish(app->activity);
            a->exitRequested = false;
        }
        if (a->sessionRunning) {
            a->syncCursorTexture();
            a->frame();
        }
    }
    g_app = nullptr;
    a->shutdown();
    delete a;
}
