// entry/src/main/cpp/napi_init.cpp
#include "napi/native_api.h"
#include <ace/xcomponent/native_interface_xcomponent.h>
#include <native_window/external_window.h>
#include <vulkan/vulkan.h>       // ★ 用 Vulkan 替代 EGL/GLES
#include <hilog/log.h>
#include <dlfcn.h>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

#undef  LOG_TAG
#define LOG_TAG "BlenderHost"
#define LOGI(f,...) OH_LOG_INFO (LOG_APP, f, ##__VA_ARGS__)
#define LOGE(f,...) OH_LOG_ERROR(LOG_APP, f, ##__VA_ARGS__)

// ─────────────── Blender SO API ───────────────
typedef int  (*FnInit)(int, const char**);
typedef void (*FnStep)(void);
typedef int  (*FnIsRun)(void);
typedef void (*FnExit)(void);
typedef void (*FnSetNativeWindow)(void*, uint32_t, uint32_t);
typedef void (*FnOnPointerEvent)(int kind, int button, float x, float y);
typedef void (*FnOnSurfaceResize)(uint32_t, uint32_t);
typedef void (*FnOnKeyEvent)(int action, int keyCode, int metaState);

struct BlenderAPI {
    void*             handle          = nullptr;
    FnInit            Init            = nullptr;
    FnStep            Step            = nullptr;
    FnIsRun           IsRun           = nullptr;
    FnExit            Exit            = nullptr;
    FnSetNativeWindow SetNativeWindow = nullptr;
    FnOnPointerEvent  OnPointerEvent  = nullptr;
    FnOnSurfaceResize OnSurfaceResize = nullptr;
    FnOnKeyEvent      OnKeyEvent      = nullptr;
} g_blender;

static bool LoadBlenderByOrder() {
    LOGI("========== dlopen (Vulkan build) ==========");

    std::vector<std::string> libs = {
        "libc++_shared.so",
        "libz.so",
        "liblzma.so",
        "libdeflate.so.0",
        "libzstd.so.1",
        "libpng.so",
        "libjpeg.so.62",
        "libwebp.so.7",
        "libtiff.so.6",
        "libImath.so",
        "libIex.so",
        "libIlmThread.so",
        "libOpenEXR.so",
        "libOpenImageIO_Util.so",
        "libOpenImageIO.so",
        "libvulkan.so",
        "libblender.so"
    };

    void* blender_handle = nullptr;
    for (const auto& name : libs) {
        void* h = dlopen(name.c_str(), RTLD_LAZY | RTLD_GLOBAL);
        if (h) {
            LOGI("✅ dlopen OK: %{public}s", name.c_str());
            if (name == "libblender.so") blender_handle = h;
        } else {
            const char* err = dlerror();
            LOGE("❌ dlopen FAIL: %{public}s -> %{public}s",
                 name.c_str(), err ? err : "unknown");
            if (name == "libblender.so" || name == "libvulkan.so") return false;
        }
    }
    if (!blender_handle) return false;
    g_blender.handle = blender_handle;

    #define SYM(x, T, n) do { \
        g_blender.x = (T)dlsym(g_blender.handle, n); \
        if (!g_blender.x) { LOGE("❌ dlsym %{public}s FAIL", n); return false; } \
        else { LOGI("✅ dlsym OK: %{public}s", n); } } while(0)

    SYM(Init,            FnInit,            "Blender_Init");
    SYM(Step,            FnStep,            "Blender_Step");
    SYM(IsRun,           FnIsRun,           "Blender_IsRunning");
    SYM(Exit,            FnExit,            "Blender_Exit");
    SYM(SetNativeWindow, FnSetNativeWindow, "Blender_SetNativeWindow");
    SYM(OnPointerEvent,  FnOnPointerEvent,  "Blender_OnPointerEvent");
    SYM(OnSurfaceResize, FnOnSurfaceResize, "Blender_OnSurfaceResize");
    SYM(OnKeyEvent,      FnOnKeyEvent,      "Blender_OnKeyEvent");
    #undef SYM
    return true;
}

// ─────────────── 渲染上下文 ───────────────────────────────
struct RenderCtx {
    OHNativeWindow*   win = nullptr;
    uint32_t          w   = 0;
    uint32_t          h   = 0;
    std::atomic<bool> running{false};
    std::atomic<bool> inited {false};
} g_rc;

// ──────── datafiles 就绪信号 ──────────────────────────────
static std::atomic<bool> g_datafiles_ready{false};
static napi_value NotifyDatafilesReady(napi_env /*env*/, napi_callback_info /*info*/) {
    g_datafiles_ready = true;
    LOGI("notifyDatafilesReady: C++ side received signal.");
    return nullptr;
}

static napi_value IsBlenderReady(napi_env env, napi_callback_info /*info*/) {
    napi_value result;
    napi_get_boolean(env, g_rc.inited.load(), &result);
    return result;
}

// ─────────────── 验证 Vulkan loader 可用 + 能看到 OHOS 扩展 ───────────────
static bool ProbeVulkan() {
    auto vkGetInstanceProcAddr_fn =
        (PFN_vkGetInstanceProcAddr)dlsym(RTLD_DEFAULT, "vkGetInstanceProcAddr");
    if (!vkGetInstanceProcAddr_fn) {
        LOGE("vkGetInstanceProcAddr not found (libvulkan.so missing?)");
        return false;
    }

    auto vkEnumerateInstanceExtensionProperties_fn =
        (PFN_vkEnumerateInstanceExtensionProperties)
        vkGetInstanceProcAddr_fn(nullptr, "vkEnumerateInstanceExtensionProperties");
    if (!vkEnumerateInstanceExtensionProperties_fn) {
        LOGE("vkEnumerateInstanceExtensionProperties FAIL");
        return false;
    }

    uint32_t cnt = 0;
    vkEnumerateInstanceExtensionProperties_fn(nullptr, &cnt, nullptr);
    std::vector<VkExtensionProperties> props(cnt);
    vkEnumerateInstanceExtensionProperties_fn(nullptr, &cnt, props.data());

    bool hasSurface = false, hasOhosSurface = false;
    for (auto& p : props) {
        LOGI("  VK ext: %{public}s", p.extensionName);
        if (std::string(p.extensionName) == "VK_KHR_surface")  hasSurface = true;
        if (std::string(p.extensionName) == "VK_OHOS_surface") hasOhosSurface = true;
    }
    if (!hasSurface || !hasOhosSurface) {
        LOGE("Vulkan ext missing: KHR_surface=%d OHOS_surface=%d",
             hasSurface, hasOhosSurface);
        return false;
    }
    LOGI("Vulkan probe OK: KHR_surface + OHOS_surface present.");
    return true;
}

// ─────────────── 渲染线程 ───────────────
static std::thread g_renderThread;

static void RenderThreadMain() {
    LOGI(">>> RenderThread enter (Vulkan manual management)");

    if (!ProbeVulkan()) {
        LOGE("Vulkan not usable on this device.");
        return;
    }
    
    // ★ 等待 ArkTS 通知 datafiles 已就绪（最多等 30s）
    int waited_ms = 0;
    while (!g_datafiles_ready.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        waited_ms += 50;
        if (waited_ms % 2000 == 0) {
            LOGI("RenderThread: waiting for datafiles... (%{public}d ms)", waited_ms);
        }
        if (waited_ms >= 30000) {
            LOGE("RenderThread: datafiles wait timeout! Proceeding anyway.");
            break;
        }
    }
    LOGI("RenderThread: datafiles ready, proceeding to Init.");

    g_blender.SetNativeWindow(g_rc.win, g_rc.w, g_rc.h);

    const char* argv[] = { "blender", "--factory-startup",
                           "--gpu-backend", "vulkan", nullptr };
    int ret = g_blender.Init(4, argv);
    LOGI("Blender_Init returned %{public}d", ret);
    g_rc.inited = (ret == 0);

    while (g_rc.running.load()) {
        if (g_rc.inited && g_blender.IsRun()) {
            /* 手动调用 Step，Vulkan 渲染到你的 OHNativeWindow */
            g_blender.Step();
            /* 手动 present（如果需要） */
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }

    if (g_rc.inited && g_blender.Exit) g_blender.Exit();
    LOGI("<<< RenderThread exit");
}

// ─────────────── XComponent 回调 ───────────────
static void OnSurfaceCreatedCB(OH_NativeXComponent* comp, void* window) {
    LOGI("OnSurfaceCreatedCB");
    uint64_t w = 0, h = 0;
    OH_NativeXComponent_GetXComponentSize(comp, window, &w, &h);
    g_rc.win = (OHNativeWindow*)window;
    g_rc.w   = (uint32_t)w;
    g_rc.h   = (uint32_t)h;
    LOGI("Surface size: %{public}llux%{public}llu",
         (unsigned long long)w, (unsigned long long)h);

    if (!g_blender.handle && !LoadBlenderByOrder()) return;

    g_rc.running = true;
    g_renderThread = std::thread(RenderThreadMain);
}

static void OnSurfaceChangedCB(OH_NativeXComponent* comp, void* window) {
    uint64_t w = 0, h = 0;
    OH_NativeXComponent_GetXComponentSize(comp, window, &w, &h);
    LOGI("OnSurfaceChangedCB %{public}llux%{public}llu",
         (unsigned long long)w, (unsigned long long)h);
    /* 只更新本地缓存；真正的 resize dispatch 到渲染线程 */
    g_rc.w = (uint32_t)w;
    g_rc.h = (uint32_t)h;
    if (g_blender.OnSurfaceResize) {
        g_blender.OnSurfaceResize((uint32_t)w, (uint32_t)h);
    }
}

static void OnSurfaceDestroyedCB(OH_NativeXComponent*, void*) {
    LOGI("OnSurfaceDestroyedCB");
    g_rc.running = false;
    if (g_renderThread.joinable()) g_renderThread.join();
    g_rc.win = nullptr;
}// ─────────────── XComponent 输入回调 ───────────────

/* kind 编码与 libblender.so 中 Blender_OnPointerEvent 约定一致 */
enum : int {
    PTR_MOUSE_MOVE   = 0,
    PTR_MOUSE_DOWN   = 1,
    PTR_MOUSE_UP     = 2,
    PTR_TOUCH_DOWN   = 10,
    PTR_TOUCH_MOVE   = 11,
    PTR_TOUCH_UP     = 12,
    PTR_TOUCH_CANCEL = 13,
};

static void DispatchTouchEventCB(OH_NativeXComponent* comp, void* window) {
    if (!g_blender.OnPointerEvent) return;

    OH_NativeXComponent_TouchEvent ev{};
    if (OH_NativeXComponent_GetTouchEvent(comp, window, &ev) != 0) return;
    
    /* ★ 过滤鼠标合成的触摸事件
     * OHOS 给鼠标合成触摸分配的 id 通常是 1001+，
     * 真实手指 id 从 0 开始，多指也不会超过 10。
     * 用阈值过滤比依赖 GetTouchPointToolType 更可靠。 */
    if (ev.id >= 100) {
        return;
    }
    
    int kind = -1;
    switch (ev.type) {
        case OH_NATIVEXCOMPONENT_DOWN:   kind = PTR_TOUCH_DOWN;   break;
        case OH_NATIVEXCOMPONENT_UP:     kind = PTR_TOUCH_UP;     break;
        case OH_NATIVEXCOMPONENT_MOVE:   kind = PTR_TOUCH_MOVE;   break;
        case OH_NATIVEXCOMPONENT_CANCEL: kind = PTR_TOUCH_CANCEL; break;
        default: return;
    }
    LOGI("[DispatchTouchEventCB] action=%{public}d button=%{public}d xy=(%{public}.1f,%{public}.1f)",
         kind, ev.id, ev.x, ev.y);
    /* ev.id 是本次事件所属触控点的 id，ev.x/ev.y 是该点坐标（组件局部） */
    g_blender.OnPointerEvent(kind, ev.id, ev.x, ev.y);
}

static void OnMouseEventCB(OH_NativeXComponent* comp, void* window) {
    if (!g_blender.OnPointerEvent) return;
    
    
    OH_NativeXComponent_MouseEvent mouseEvent;
    OH_NativeXComponent_GetMouseEvent(comp, window, &mouseEvent);

    OH_NativeXComponent_MouseEvent ev{};
    if (OH_NativeXComponent_GetMouseEvent(comp, window, &ev) != 0) return;

    int btn = 0;
    switch (ev.button) {
        case OH_NATIVEXCOMPONENT_LEFT_BUTTON:   btn = 0; break;
        case OH_NATIVEXCOMPONENT_MIDDLE_BUTTON: btn = 1; break;
        case OH_NATIVEXCOMPONENT_RIGHT_BUTTON:  btn = 2; break;
        default: btn = 0; break;   /* move 事件通常是 NONE_BUTTON */
    }

    int kind = -1;
    switch (ev.action) {
        case OH_NATIVEXCOMPONENT_MOUSE_MOVE:    kind = PTR_MOUSE_MOVE; break;
        case OH_NATIVEXCOMPONENT_MOUSE_PRESS:   kind = PTR_MOUSE_DOWN; break;
        case OH_NATIVEXCOMPONENT_MOUSE_RELEASE: kind = PTR_MOUSE_UP;   break;
        default: return;
    }
    
    LOGI("[OnMouseEventCB] action=%{public}d button=%{public}d xy=(%{public}.1f,%{public}.1f)",
         kind, btn, ev.x, ev.y);
    g_blender.OnPointerEvent(kind, btn, ev.x, ev.y);
}

static void OnHoverEventCB(OH_NativeXComponent*, bool /*isHover*/) {
    /* 目前不关心 hover 进入/离开。后面需要 cursor enter/leave 再接。 */
}

static napi_value SendKeyEvent(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    int32_t action = 0, keyCode = 0, metaState = 0;
    napi_get_value_int32(env, argv[0], &action);
    napi_get_value_int32(env, argv[1], &keyCode);
    napi_get_value_int32(env, argv[2], &metaState);

    if (g_blender.OnKeyEvent) {
        g_blender.OnKeyEvent(action, keyCode, metaState);
    }
    return nullptr;
}

static OH_NativeXComponent_MouseEvent_Callback g_mouseCbk = {
    .DispatchMouseEvent = OnMouseEventCB,
    .DispatchHoverEvent = OnHoverEventCB,
};

static OH_NativeXComponent_Callback g_cbk = {
    OnSurfaceCreatedCB, OnSurfaceChangedCB,
    OnSurfaceDestroyedCB, DispatchTouchEventCB
};

// ─────────────── NAPI 导出 ───────────────
static napi_value SetDatafilesPath(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    size_t len = 0;
    napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len);
    static std::string s; s.resize(len);
    napi_get_value_string_utf8(env, argv[0], s.data(), len + 1, &len);
    // s 是 .../files/blender/datafiles
    setenv("BLENDER_SYSTEM_DATAFILES", s.c_str(), 1);
    // scripts 和 datafiles 是平级目录，推导出来
    std::string blender_base = s.substr(0, s.rfind('/'));  // .../files/blender
    std::string scripts_path = blender_base + "/scripts";
    setenv("BLENDER_SYSTEM_SCRIPTS", scripts_path.c_str(), 1);
    LOGI("BLENDER_SYSTEM_DATAFILES = %{public}s", s.c_str());
    LOGI("BLENDER_SYSTEM_SCRIPTS   = %{public}s", scripts_path.c_str());
    return nullptr;
}

static napi_value SetPythonHome(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    size_t len = 0;
    napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len);
    static std::string home; home.resize(len);
    napi_get_value_string_utf8(env, argv[0], home.data(), len + 1, &len);

    setenv("PYTHONHOME", home.c_str(), 1);
    std::string pypath = home + ":" + home + "/lib-dynload";
    setenv("PYTHONPATH", pypath.c_str(), 1);
    setenv("PYTHONDONTWRITEBYTECODE", "1", 1);
    setenv("PYTHONNOUSERSITE", "1", 1);
    LOGI("PYTHONHOME=%{public}s", home.c_str());
    return nullptr;
}

EXTERN_C_START
static napi_value NapiInit(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        { "setDatafilesPath",    nullptr, SetDatafilesPath,    nullptr, nullptr, nullptr, napi_default, nullptr },
        { "notifyDatafilesReady",nullptr, NotifyDatafilesReady,nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setPythonHome", nullptr, SetPythonHome, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "sendKeyEvent", nullptr, SendKeyEvent, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "isBlenderReady", nullptr, IsBlenderReady, nullptr, nullptr, nullptr, napi_default, nullptr },
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    napi_value xcompObj;
    napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &xcompObj);
    OH_NativeXComponent* nx = nullptr;
    napi_unwrap(env, xcompObj, (void**)&nx);
    if (nx) {
        OH_NativeXComponent_RegisterCallback(nx, &g_cbk);
        OH_NativeXComponent_RegisterMouseEventCallback(nx, &g_mouseCbk);  // ★ 新增
        LOGI("XComponent callback registered.");
    } else {
        LOGE("napi_unwrap XComponent FAIL");
    }
    return exports;
}
EXTERN_C_END

static napi_module demoModule = {
    .nm_version = 1, .nm_flags = 0, .nm_filename = nullptr,
    .nm_register_func = NapiInit, .nm_modname = "entry",
    .nm_priv = nullptr, .reserved = {0},
};
extern "C" __attribute__((constructor)) void RegisterEntryModule(void) {
    napi_module_register(&demoModule);
}