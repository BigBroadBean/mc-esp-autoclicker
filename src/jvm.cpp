// ============================================================
//  jvm.cpp — JVM 附加、SRG 混淆名称解析、MC 数据采集
//
//  混淆处理说明（Forge 1.20.1 运行时）：
//    类名  = 官方名（net.minecraft.client.Minecraft ...）
//    字段/方法 = SRG 名（f_91073_ / m_91087_ ...）
//  所有成员名均为从游戏本体 client-1.20.1-...-srg.jar 提取的权威值。
//  每个符号同时给出官方名回退，并记录实际命中的名称，便于移植/排错。
// ============================================================
#include "common.h"
#include "jvm.h"
#include <jvmti.h>
#include <cmath>
#include <atomic>
#include <unordered_map>

static JavaVM* g_vm = nullptr;
// JNIEnv 是线程局部的：只有 SwapBuffers 钩子所在的游戏渲染线程会用到 env（GetEnv 复用）。
// 注意：不能用 C++ thread_local / __declspec(thread)——手动映射注入时加载器
// 不会为本模块分配 TLS 槽，也不会在新建线程时复制 TLS 模板，会导致随机崩溃。
// 改用 TlsAlloc 手动管理“每线程 env”，手动映射下同样可靠。
static JNIEnv*& tls_env() {
    static DWORD slot = TlsAlloc();
    static JNIEnv* fallback = nullptr;
    if (slot == TLS_OUT_OF_INDEXES) return fallback;
    JNIEnv** p = (JNIEnv**)TlsGetValue(slot);
    if (!p) { p = new JNIEnv*(nullptr); TlsSetValue(slot, p); }
    return *p;
}
#define g_env tls_env()
static jvmtiEnv* g_jvmti = nullptr;
// 符号解析完成标记（渲染线程据此决定是否可读实时相机）
static std::atomic<bool> g_resolved{false};

// ---- 实体名字缓存（仅游戏渲染线程访问，无需加锁） ----
// 名字很少变化，按实体网络 ID 缓存，避免每帧对每个实体重复做
// getName()->getString()->GetStringChars 的字符串拷贝——这是游戏渲染线程
// SwapBuffers 钩子里的主要 JNI 开销之一，缓存后每帧名字开销归零。
static std::unordered_map<int, std::pair<std::wstring, DWORD>> g_nameCache;
static DWORD g_nameRefreshTick = 0;   // 周期性重读名字的时间点
static DWORD g_namePruneTick = 0;     // 定期清理消失实体名字的时间点
static const DWORD kNameRefreshMs = 2000;   // 每 2s 重读一次名字（跟踪改名）
static const DWORD kNameKeepMs    = 10000;  // 10s 未出现则清理缓存项

// ---- 实体元数据缓存（尺寸/类型，仅游戏渲染线程访问，无需加锁） ----
// bbw/bbh/isPlayer/isLiving 几乎不变，每帧重读是纯浪费（每实体 4 次 JNI 调用）。
// 按 id 缓存、1s 周期重读，让“每帧采集实体位置”成为可能且总开销不升——
// 这是“碰撞箱级零延迟贴合”（位置与游戏画面同步）的性能前提。
struct EntityMeta { float bbw = 0.6f, bbh = 1.8f; bool isPlayer = false, isLiving = false; int projType = PROJ_NONE; DWORD lastSeen = 0; };
static std::unordered_map<int, EntityMeta> g_metaCache;
static DWORD g_metaRefreshTick = 0;
static const DWORD kMetaRefreshMs = 1000;   // 每 1s 重读一次元数据（跟踪尺寸/类型变化）

bool jvm_ready() { return g_resolved.load(std::memory_order_acquire); }

// ------------------------------------------------------------
// 已解析的 JNI ID（全部全局引用，解析一次，长期复用）
// ------------------------------------------------------------
static jclass c_Minecraft, c_ClientLevel, c_Entity, c_LivingEntity, c_Player,
             c_EntityType, c_Vec3, c_Camera, c_GameRenderer, c_Matrix4f,
             c_Component, c_Iterable, c_Iterator, c_RemovalReason, c_ChatScreen,
             c_Projectile, c_ItemStack, c_Item,
             c_HitResult, c_EntityHitResult, c_BlockItem,
             c_ClipContext, c_ClipContext_Block, c_ClipContext_Fluid,
             c_HitResult_Type, c_BlockHitResult;

static jmethodID m_getInstance, m_getFrameTime,            // Minecraft
                 m_entitiesForRendering,                    // ClientLevel
                 m_getBbWidth, m_getBbHeight, m_getType, m_getId,
                 m_getRemovalReason, m_getName, m_getDeltaMovement,   // Entity
                 m_getOwner,                                 // Projectile
                 m_getDescriptionId,                        // EntityType
                 m_getPosition, m_getPitch, m_getYaw,       // Camera
                 m_getMainCamera, m_getFov,                 // GameRenderer
                 m_getString,                               // Component
                 m_iterator, m_hasNext, m_next,             // Iterable/Iterator
                 m_getEyeHeight, m_getYRot, m_getXRot,      // Entity（玩家）
                 m_isUsingItem, m_getTicksUsingItem, m_getMainHandItem,  // LivingEntity
                 m_isAlive, m_isAttackable,                 // LivingEntity（连点器门控）
                 m_getHealth, m_getMaxHealth,               // LivingEntity（自瞄目标排序）
                 m_getItem, m_getItemDesc,                  // ItemStack / Item
                 m_hitResultGetType, m_hitGetEntity,        // HitResult / EntityHitResult
                 m_clip, m_hitGetType, m_hitGetLocation,     // Level / HitResult
                 m_v3Ctor, m_ccCtor;                         // Vec3 / ClipContext 构造器
static jfieldID f_level, f_gameRenderer, f_player, f_screen,       // Minecraft
                f_hitResult,                                        // Minecraft（连点器门控）
                f_x, f_y, f_z, f_xo, f_yo, f_zo,                    // Entity
                f_onGround,                                         // Entity（玩家是否在地面）
                f_vx, f_vy, f_vz;                                   // Vec3

// ---- 连点器战斗状态缓存（仅游戏渲染线程访问） ----
// canAttack/canPlace 每帧在 SwapBuffers 钩子内直接读取，不再经过 UDP/共享内存。

// ------------------------------------------------------------
// 名称解析辅助（多别名 + 日志记录命中的别名）
// ------------------------------------------------------------
// Forge 的 modlauncher 把游戏类放入私有模块化类加载器：
//  - JNI 默认 FindClass 只走系统类加载器 -> 找不到
//  - 新建原生线程的线程上下文类加载器 = 系统类加载器 -> 也找不到
// 因此用 JVMTI 直接从“JVM 已加载类”里定位游戏类，最可靠。
static bool jvmti_init() {
    if (g_jvmti) return true;
    if (g_vm->GetEnv((void**)&g_jvmti, JVMTI_VERSION_1_2) != JNI_OK || !g_jvmti)
        return false;
    return true;
}

// 在已加载类中按签名 "L<binaryName>;" 查找；同时从任意首个带类加载器的类取得 loader。
static jclass jvmti_find_loaded(const char* binaryName, jobject* outLoader) {
    if (!g_jvmti) return nullptr;
    jint count = 0;
    jclass* classes = nullptr;
    if (g_jvmti->GetLoadedClasses(&count, &classes) != JVMTI_ERROR_NONE)
        return nullptr;

    std::string want = "L";
    want += binaryName;
    want += ";";

    jclass found = nullptr;
    bool gotLoader = (outLoader == nullptr);
    for (jint i = 0; i < count; ++i) {
        char* sig = nullptr;
        if (g_jvmti->GetClassSignature(classes[i], &sig, nullptr) == JVMTI_ERROR_NONE && sig) {
            bool match = (strcmp(sig, want.c_str()) == 0);
            if (!gotLoader && outLoader && !found) {
                jobject cl = nullptr;
                if (g_jvmti->GetClassLoader(classes[i], &cl) == JVMTI_ERROR_NONE && cl) {
                    *outLoader = cl;
                    gotLoader = true;
                }
            }
            g_jvmti->Deallocate((unsigned char*)sig);
            if (match) {
                found = (jclass)g_env->NewGlobalRef(classes[i]);
            }
            if (found && gotLoader) break;
        }
    }
    g_jvmti->Deallocate((unsigned char*)classes);
    return found;
}

// 缓存的转换类加载器（Forge ModuleClassLoader），用于加载尚未加载的类
static jobject g_transform_cl = nullptr;

// 通过给定类加载器 loadClass（binaryName 用 '.' 分隔）
static jclass load_class_via(jobject loader, const char* binaryName) {
    jclass c_ClassLoader = g_env->FindClass("java/lang/ClassLoader");
    if (g_env->ExceptionCheck()) g_env->ExceptionClear();
    if (!c_ClassLoader) return nullptr;
    jmethodID mLoad = g_env->GetMethodID(c_ClassLoader, "loadClass",
                                         "(Ljava/lang/String;)Ljava/lang/Class;");
    if (g_env->ExceptionCheck()) g_env->ExceptionClear();
    if (!mLoad) { g_env->DeleteLocalRef(c_ClassLoader); return nullptr; }

    std::string dotted = binaryName;
    for (auto& ch : dotted) if (ch == '/') ch = '.';
    jstring jn = g_env->NewStringUTF(dotted.c_str());
    jobject cls = g_env->CallObjectMethod(loader, mLoad, jn);
    g_env->DeleteLocalRef(jn);
    g_env->DeleteLocalRef(c_ClassLoader);
    if (g_env->ExceptionCheck()) { g_env->ExceptionClear(); return nullptr; }
    return (jclass)cls; // 本地引用，调用方负责转全局
}

// 当前线程上下文类加载器（备用手段）
static jobject thread_context_loader() {
    static jclass c_Thread = nullptr;
    if (!c_Thread) {
        c_Thread = (jclass)g_env->NewGlobalRef(g_env->FindClass("java/lang/Thread"));
        if (g_env->ExceptionCheck()) g_env->ExceptionClear();
    }
    if (!c_Thread) return nullptr;
    jmethodID mCur = g_env->GetStaticMethodID(c_Thread, "currentThread", "()Ljava/lang/Thread;");
    if (g_env->ExceptionCheck()) g_env->ExceptionClear();
    if (!mCur) return nullptr;
    jobject t = g_env->CallStaticObjectMethod(c_Thread, mCur);
    if (g_env->ExceptionCheck()) g_env->ExceptionClear();
    if (!t) return nullptr;
    jmethodID mGet = g_env->GetMethodID(c_Thread, "getContextClassLoader", "()Ljava/lang/ClassLoader;");
    if (g_env->ExceptionCheck()) g_env->ExceptionClear();
    if (!mGet) { g_env->DeleteLocalRef(t); return nullptr; }
    jobject cl = g_env->CallObjectMethod(t, mGet);
    if (g_env->ExceptionCheck()) g_env->ExceptionClear();
    g_env->DeleteLocalRef(t);
    return cl; // 可能为 null
}

static jclass resolve_class(const char* what, const char* const* names, size_t n) {
    jvmti_init();

    // 1) JVMTI 已加载类 + 记录转换类加载器
    for (size_t i = 0; i < n; ++i) {
        jobject loader = nullptr;
        jclass c = jvmti_find_loaded(names[i], &loader);
        if (loader) {
            if (!g_transform_cl) g_transform_cl = g_env->NewGlobalRef(loader);
            g_env->DeleteLocalRef(loader);
        }
        if (c) {
            esp_log("[混淆] 类 %s -> %s (JVMTI)", what, names[i]);
            return c; // 已是全局引用
        }
    }

    // 2) 用转换类加载器 loadClass（覆盖尚未加载的类，如 ClientLevel）
    if (g_transform_cl) {
        for (size_t i = 0; i < n; ++i) {
            jclass c = load_class_via(g_transform_cl, names[i]);
            if (c) {
                jclass g = (jclass)g_env->NewGlobalRef(c);
                g_env->DeleteLocalRef(c);
                esp_log("[混淆] 类 %s -> %s (转换加载器)", what, names[i]);
                return g;
            }
        }
    }

    // 3) 线程上下文类加载器
    jobject cl = thread_context_loader();
    if (cl) {
        for (size_t i = 0; i < n; ++i) {
            jclass c = load_class_via(cl, names[i]);
            if (c) {
                jclass g = (jclass)g_env->NewGlobalRef(c);
                g_env->DeleteLocalRef(c);
                g_env->DeleteLocalRef(cl);
                esp_log("[混淆] 类 %s -> %s (上下文加载器)", what, names[i]);
                return g;
            }
        }
        g_env->DeleteLocalRef(cl);
    }

    // 4) 默认 FindClass（系统类加载器）
    for (size_t i = 0; i < n; ++i) {
        jclass c = g_env->FindClass(names[i]);
        if (g_env->ExceptionCheck()) { g_env->ExceptionClear(); continue; }
        if (c) {
            jclass g = (jclass)g_env->NewGlobalRef(c);
            g_env->DeleteLocalRef(c);
            esp_log("[混淆] 类 %s -> %s", what, names[i]);
            return g;
        }
    }

    esp_log("[混淆] 未找到类 %s", what);
    return nullptr;
}

static jmethodID resolve_method(jclass cls, const char* what, const char* const* names,
                                size_t n, const char* sig, bool is_static) {
    if (!cls) { esp_log("[混淆] %s: 类为空", what); return nullptr; }
    for (size_t i = 0; i < n; ++i) {
        jmethodID m = is_static ? g_env->GetStaticMethodID(cls, names[i], sig)
                                : g_env->GetMethodID(cls, names[i], sig);
        if (g_env->ExceptionCheck()) { g_env->ExceptionClear(); continue; }
        if (m) { esp_log("[混淆] 方法 %s -> %s %s", what, names[i], sig); return m; }
    }
    esp_log("[混淆] 未找到方法 %s (%s)", what, sig);
    return nullptr;
}

static jfieldID resolve_field(jclass cls, const char* what, const char* const* names,
                              size_t n, const char* sig, bool is_static) {
    if (!cls) { esp_log("[混淆] %s: 类为空", what); return nullptr; }
    for (size_t i = 0; i < n; ++i) {
        jfieldID f = is_static ? g_env->GetStaticFieldID(cls, names[i], sig)
                               : g_env->GetFieldID(cls, names[i], sig);
        if (g_env->ExceptionCheck()) { g_env->ExceptionClear(); continue; }
        if (f) { esp_log("[混淆] 字段 %s -> %s %s", what, names[i], sig); return f; }
    }
    esp_log("[混淆] 未找到字段 %s (%s)", what, sig);
    return nullptr;
}

// 在类中按“字段类型”反射查找（应对 SRG 名不确定的字段，如 Minecraft.screen）。
// typeName 用点分官方名，如 "net.minecraft.client.gui.screens.Screen"。
// GetFieldID 只做一次反射枚举，之后每帧走 JNI 字段读取，无额外开销。
static jfieldID resolve_field_by_type(jclass cls, const char* what, const char* typeName) {
    if (!cls) { esp_log("[混淆] %s: 类为空", what); return nullptr; }
    jclass c_Class = g_env->FindClass("java/lang/Class");
    jclass c_Field = g_env->FindClass("java/lang/reflect/Field");
    if (g_env->ExceptionCheck()) g_env->ExceptionClear();
    if (!c_Class || !c_Field) {
        esp_log("[混淆] %s: 反射类加载失败", what);
        if (c_Class) g_env->DeleteLocalRef(c_Class);
        if (c_Field) g_env->DeleteLocalRef(c_Field);
        return nullptr;
    }
    jmethodID mGetDf = g_env->GetMethodID(c_Class, "getDeclaredFields", "()[Ljava/lang/reflect/Field;");
    jmethodID mGetType = g_env->GetMethodID(c_Field, "getType", "()Ljava/lang/Class;");
    jmethodID mGetName = g_env->GetMethodID(c_Class, "getName", "()Ljava/lang/String;");
    if (g_env->ExceptionCheck()) g_env->ExceptionClear();
    if (!mGetDf || !mGetType || !mGetName) {
        g_env->DeleteLocalRef(c_Class); g_env->DeleteLocalRef(c_Field);
        return nullptr;
    }
    jobjectArray arr = (jobjectArray)g_env->CallObjectMethod(cls, mGetDf);
    if (g_env->ExceptionCheck()) g_env->ExceptionClear();
    if (!arr) { g_env->DeleteLocalRef(c_Class); g_env->DeleteLocalRef(c_Field); return nullptr; }
    jfieldID result = nullptr;
    jsize n = g_env->GetArrayLength(arr);
    for (jsize i = 0; i < n && !result; ++i) {
        jobject f = g_env->GetObjectArrayElement(arr, i);
        if (g_env->ExceptionCheck()) g_env->ExceptionClear();
        if (!f) continue;
        jobject t = g_env->CallObjectMethod(f, mGetType);
        if (g_env->ExceptionCheck()) g_env->ExceptionClear();
        if (t) {
            jstring tn = (jstring)g_env->CallObjectMethod(t, mGetName);
            if (g_env->ExceptionCheck()) g_env->ExceptionClear();
            if (tn) {
                const char* ctn = g_env->GetStringUTFChars(tn, nullptr);
                if (ctn && strcmp(ctn, typeName) == 0) {
                    result = g_env->FromReflectedField(f);
                    esp_log("[混淆] 字段 %s -> (反射按类型:%s)", what, ctn);
                }
                if (ctn) g_env->ReleaseStringUTFChars(tn, ctn);
                g_env->DeleteLocalRef(tn);
            }
            g_env->DeleteLocalRef(t);
        }
        g_env->DeleteLocalRef(f);
    }
    g_env->DeleteLocalRef(arr);
    g_env->DeleteLocalRef(c_Class);
    g_env->DeleteLocalRef(c_Field);
    g_env->ExceptionClear();
    if (!result) esp_log("[混淆] 未找到字段 %s (类型反射 %s)", what, typeName);
    return result;
}

// 小工具：解析单个类
#define RES_CLS(dst, name) { static const char* n[] = {name}; dst = resolve_class(name, n, 1); }

// ------------------------------------------------------------
// 在“游戏渲染线程”内（SwapBuffers 钩子）使用：
// GetCreatedVMs + GetEnv 复用该线程已有的 JNIEnv。
// 关键：GetEnv 只对【已附加】的线程返回 env，不创建任何新 Java 线程，
// 也不会触发 JVM 的 ThreadStart 事件——规避在线反作弊对
// “新建原生线程 AttachCurrentThread”的检测（已实测确认该行为会被强杀）。
// 首次调用时顺带完成符号解析。
// ------------------------------------------------------------
bool jvm_hook_begin() {
    if (g_env) return true;                    // 本线程已就绪
    if (g_resolved.load()) return false;       // 已解析但本线程未附加，异常

    if (!g_vm) {
        HMODULE hJvm = GetModuleHandleW(L"jvm.dll");
        if (!hJvm) hJvm = GetModuleHandleW(L"server\\jvm.dll");
        if (!hJvm) { esp_log("[JVM] 未找到 jvm.dll"); return false; }

        typedef jint(JNICALL* GetCreatedVMs_t)(JavaVM**, jsize, jsize*);
        auto GetCreatedVMs = (GetCreatedVMs_t)GetProcAddress(hJvm, "JNI_GetCreatedJavaVMs");
        if (!GetCreatedVMs) { esp_log("[JVM] 找不到 JNI_GetCreatedJavaVMs"); return false; }

        jsize n = 0;
        if (GetCreatedVMs(&g_vm, 1, &n) != JNI_OK || n == 0) {
            esp_log("[JVM] JNI_GetCreatedJavaVMs 失败 n=%d", (int)n);
            g_vm = nullptr;
            return false;
        }
    }

    JNIEnv* env = nullptr;
    if (g_vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK || !env) {
        esp_log("[JVM] GetEnv 失败（当前线程未附加到 JVM？）");
        return false;
    }
    g_env = env;

    if (!g_resolved.load()) {
        esp_log("[hook] 在游戏渲染线程解析符号 ...");
        if (!jvm_resolve_all()) return false;
    }
    return true;
}

// ------------------------------------------------------------
// 解析全部符号
// ------------------------------------------------------------
bool jvm_resolve_all() {
    if (!g_env) return false;

    // ---- 类（官方名） ----
    RES_CLS(c_Minecraft,     "net/minecraft/client/Minecraft");
    RES_CLS(c_ClientLevel,   "net/minecraft/client/multiplayer/ClientLevel");
    RES_CLS(c_Entity,        "net/minecraft/world/entity/Entity");
    RES_CLS(c_LivingEntity,  "net/minecraft/world/entity/LivingEntity");
    RES_CLS(c_Player,        "net/minecraft/world/entity/player/Player");
    RES_CLS(c_EntityType,    "net/minecraft/world/entity/EntityType");
    RES_CLS(c_Vec3,          "net/minecraft/world/phys/Vec3");
    RES_CLS(c_Camera,        "net/minecraft/client/Camera");
    RES_CLS(c_GameRenderer,  "net/minecraft/client/renderer/GameRenderer");
    RES_CLS(c_Matrix4f,      "org/joml/Matrix4f");
    RES_CLS(c_Component,     "net/minecraft/network/chat/Component");
    RES_CLS(c_Iterable,      "java/lang/Iterable");
    RES_CLS(c_Iterator,      "java/util/Iterator");
    RES_CLS(c_RemovalReason, "net/minecraft/world/entity/Entity$RemovalReason");
    RES_CLS(c_ChatScreen,    "net/minecraft/client/gui/screens/ChatScreen");
    RES_CLS(c_Projectile,    "net/minecraft/world/entity/projectile/Projectile");
    RES_CLS(c_ItemStack,     "net/minecraft/world/item/ItemStack");
    RES_CLS(c_Item,          "net/minecraft/world/item/Item");
    RES_CLS(c_HitResult,     "net/minecraft/world/phys/HitResult");
    RES_CLS(c_EntityHitResult,"net/minecraft/world/phys/EntityHitResult");
    RES_CLS(c_BlockItem,     "net/minecraft/world/item/BlockItem");
    RES_CLS(c_ClipContext,   "net/minecraft/world/level/ClipContext");
    RES_CLS(c_ClipContext_Block, "net/minecraft/world/level/ClipContext$Block");
    RES_CLS(c_ClipContext_Fluid, "net/minecraft/world/level/ClipContext$Fluid");
    RES_CLS(c_HitResult_Type,"net/minecraft/world/phys/HitResult$Type");
    RES_CLS(c_BlockHitResult,"net/minecraft/world/phys/BlockHitResult");

    // ---- Minecraft ----
    {
        static const char* n[] = {"m_91087_", "getInstance"};
        m_getInstance = resolve_method(c_Minecraft, "getInstance", n, 2,
                                       "()Lnet/minecraft/client/Minecraft;", true);
    }
    {
        static const char* n[] = {"m_91296_", "getFrameTime"};
        m_getFrameTime = resolve_method(c_Minecraft, "getFrameTime", n, 2, "()F", false);
    }
    {
        static const char* n[] = {"f_91073_", "level"};
        f_level = resolve_field(c_Minecraft, "level", n, 2,
                                "Lnet/minecraft/client/multiplayer/ClientLevel;", false);
    }
    {
        static const char* n[] = {"f_91063_", "gameRenderer"};
        f_gameRenderer = resolve_field(c_Minecraft, "gameRenderer", n, 2,
                                       "Lnet/minecraft/client/renderer/GameRenderer;", false);
    }
    {
        static const char* n[] = {"f_91074_", "player"};
        f_player = resolve_field(c_Minecraft, "player", n, 2,
                                 "Lnet/minecraft/client/player/LocalPlayer;", false);
    }
    // 当前打开界面（Esc/背包/聊天…）：有界面时不渲染 ESP。
    // 先按 SRG 名尝试，失败再按类型反射查找，保证命中。
    // （mappings-extracted/1.20.1/fields.csv：enn/z = f_91080_ = screen）
    {
        static const char* n[] = {"f_91080_", "screen", "z"};
        f_screen = resolve_field(c_Minecraft, "screen", n, 3,
                                 "Lnet/minecraft/client/gui/screens/Screen;", false);
        if (!f_screen)
            f_screen = resolve_field_by_type(c_Minecraft, "screen",
                                             "net.minecraft.client.gui.screens.Screen");
    }

    // 准星命中结果（连点器攻击门控；逻辑与 MCCombatStatusJni forge1201 映射一致）
    {
        static const char* n[] = {"f_91077_", "hitResult"};
        f_hitResult = resolve_field(c_Minecraft, "hitResult", n, 2,
                                    "Lnet/minecraft/world/phys/HitResult;", false);
    }
    {
        static const char* n[] = {"m_6662_", "getType"};
        m_hitResultGetType = resolve_method(c_HitResult, "HitResult.getType", n, 2,
                                            "()Lnet/minecraft/world/phys/HitResult$Type;", false);
    }
    {
        static const char* n[] = {"m_82443_", "getEntity"};
        m_hitGetEntity = resolve_method(c_EntityHitResult, "EntityHitResult.getEntity", n, 2,
                                        "()Lnet/minecraft/world/entity/Entity;", false);
    }
    {
        static const char* n[] = {"m_6084_", "isAlive"};
        m_isAlive = resolve_method(c_LivingEntity, "LivingEntity.isAlive", n, 2, "()Z", false);
    }
    {
        static const char* n[] = {"m_6097_", "isAttackable"};
        m_isAttackable = resolve_method(c_LivingEntity, "LivingEntity.isAttackable", n, 2, "()Z", false);
    }
    {
        static const char* n[] = {"m_21223_", "getHealth"};
        m_getHealth = resolve_method(c_LivingEntity, "LivingEntity.getHealth", n, 2, "()F", false);
    }
    {
        static const char* n[] = {"m_21233_", "getMaxHealth"};
        m_getMaxHealth = resolve_method(c_LivingEntity, "LivingEntity.getMaxHealth", n, 2, "()F", false);
    }

    // ---- ClientLevel ----
    {
        static const char* n[] = {"m_104735_", "entitiesForRendering"};
        m_entitiesForRendering = resolve_method(c_ClientLevel, "entitiesForRendering", n, 2,
                                                "()Ljava/lang/Iterable;", false);
    }

    // ---- Entity ----
    {
        static const char* n[] = {"f_19854_", "x"};
        f_x = resolve_field(c_Entity, "x", n, 2, "D", false);
    }
    {
        static const char* n[] = {"f_19855_", "y"};
        f_y = resolve_field(c_Entity, "y", n, 2, "D", false);
    }
    {
        static const char* n[] = {"f_19856_", "z"};
        f_z = resolve_field(c_Entity, "z", n, 2, "D", false);
    }
    {
        static const char* n[] = {"f_19790_", "xOld", "xo"};
        f_xo = resolve_field(c_Entity, "xOld", n, 3, "D", false);
    }
    {
        static const char* n[] = {"f_19791_", "yOld", "yo"};
        f_yo = resolve_field(c_Entity, "yOld", n, 3, "D", false);
    }
    {
        static const char* n[] = {"f_19792_", "zOld", "zo"};
        f_zo = resolve_field(c_Entity, "zOld", n, 3, "D", false);
    }
    {
        static const char* n[] = {"f_19861_", "onGround"};
        f_onGround = resolve_field(c_Entity, "onGround", n, 2, "Z", false);
    }
    {
        static const char* n[] = {"m_20205_", "getBbWidth"};
        m_getBbWidth = resolve_method(c_Entity, "getBbWidth", n, 2, "()F", false);
    }
    {
        static const char* n[] = {"m_19879_", "getId"};
        m_getId = resolve_method(c_Entity, "getId", n, 2, "()I", false);
    }
    {
        static const char* n[] = {"m_20206_", "getBbHeight"};
        m_getBbHeight = resolve_method(c_Entity, "getBbHeight", n, 2, "()F", false);
    }
    {
        static const char* n[] = {"m_6095_", "getType"};
        m_getType = resolve_method(c_Entity, "getType", n, 2,
                                   "()Lnet/minecraft/world/entity/EntityType;", false);
    }
    {
        static const char* n[] = {"m_146911_", "getRemovalReason"};
        m_getRemovalReason = resolve_method(c_Entity, "getRemovalReason", n, 2,
                                            "()Lnet/minecraft/world/entity/Entity$RemovalReason;", false);
    }
    {
        static const char* n[] = {"m_20184_", "getDeltaMovement"};
        m_getDeltaMovement = resolve_method(c_Entity, "getDeltaMovement", n, 2,
                                            "()Lnet/minecraft/world/phys/Vec3;", false);
    }
    {
        static const char* n[] = {"m_20192_", "getEyeHeight"};
        m_getEyeHeight = resolve_method(c_Entity, "getEyeHeight", n, 2, "()F", false);
    }
    {
        static const char* n[] = {"m_146908_", "getYRot"};
        m_getYRot = resolve_method(c_Entity, "getYRot", n, 2, "()F", false);
    }
    {
        static const char* n[] = {"m_146909_", "getXRot"};
        m_getXRot = resolve_method(c_Entity, "getXRot", n, 2, "()F", false);
    }

    // ---- LivingEntity（玩家蓄力状态） ----
    {
        static const char* n[] = {"m_6117_", "isUsingItem"};
        m_isUsingItem = resolve_method(c_LivingEntity, "isUsingItem", n, 2, "()Z", false);
    }
    {
        static const char* n[] = {"m_21252_", "getTicksUsingItem"};
        m_getTicksUsingItem = resolve_method(c_LivingEntity, "getTicksUsingItem", n, 2, "()I", false);
    }
    {
        static const char* n[] = {"m_21205_", "getMainHandItem"};
        m_getMainHandItem = resolve_method(c_LivingEntity, "getMainHandItem", n, 2,
                                           "()Lnet/minecraft/world/item/ItemStack;", false);
    }

    // ---- Projectile（弹射物归属，排除自己射出的） ----
    {
        static const char* n[] = {"m_19749_", "getOwner"};
        m_getOwner = resolve_method(c_Projectile, "getOwner", n, 2,
                                    "()Lnet/minecraft/world/entity/Entity;", false);
    }

    // ---- ItemStack / Item（判断手持是否为弓） ----
    {
        static const char* n[] = {"m_41720_", "getItem"};
        m_getItem = resolve_method(c_ItemStack, "getItem", n, 2,
                                   "()Lnet/minecraft/world/item/Item;", false);
    }
    {
        static const char* n[] = {"m_41467_", "getDescriptionId"};
        m_getItemDesc = resolve_method(c_Item, "getDescriptionId", n, 2, "()Ljava/lang/String;", false);
    }

    // ---- 方块射线（弓预判轨迹的障碍物命中检测，仿 LiquidBounce world.clip） ----
    {
        static const char* n[] = {"m_45547_", "clip"};
        m_clip = resolve_method(c_ClientLevel, "clip", n, 2,
                                "(Lnet/minecraft/world/level/ClipContext;)Lnet/minecraft/world/phys/BlockHitResult;", false);
    }
    {
        static const char* n[] = {"m_6662_", "getType"};
        m_hitGetType = resolve_method(c_BlockHitResult, "getType", n, 2,
                                      "()Lnet/minecraft/world/phys/HitResult$Type;", false);
    }
    {
        static const char* n[] = {"m_82450_", "getLocation"};
        m_hitGetLocation = resolve_method(c_BlockHitResult, "getLocation", n, 2,
                                          "()Lnet/minecraft/world/phys/Vec3;", false);
    }
    {
        static const char* n[] = {"<init>"};
        m_v3Ctor = resolve_method(c_Vec3, "Vec3 ctor", n, 1, "(DDD)V", false);
    }
    {
        static const char* n[] = {"<init>"};
        m_ccCtor = resolve_method(c_ClipContext, "ClipContext ctor", n, 1,
                                  "(Lnet/minecraft/world/phys/Vec3;Lnet/minecraft/world/phys/Vec3;Lnet/minecraft/world/level/ClipContext$Block;Lnet/minecraft/world/level/ClipContext$Fluid;Lnet/minecraft/world/entity/Entity;)V", false);
    }
    {
        static const char* n[] = {"m_7755_", "getName", "getDisplayName"};
        m_getName = resolve_method(c_Entity, "getName", n, 3,
                                   "()Lnet/minecraft/network/chat/Component;", false);
    }

    // ---- EntityType ----
    {
        static const char* n[] = {"m_20675_", "getDescriptionId"};
        m_getDescriptionId = resolve_method(c_EntityType, "getDescriptionId", n, 2,
                                            "()Ljava/lang/String;", false);
    }

    // ---- Vec3 ----
    {
        static const char* n[] = {"f_82479_", "x"};
        f_vx = resolve_field(c_Vec3, "x", n, 2, "D", false);
    }
    {
        static const char* n[] = {"f_82480_", "y"};
        f_vy = resolve_field(c_Vec3, "y", n, 2, "D", false);
    }
    {
        static const char* n[] = {"f_82481_", "z"};
        f_vz = resolve_field(c_Vec3, "z", n, 2, "D", false);
    }

    // ---- Camera ----
    // 运行时实测：m_90589_(f_90557_) 返回俯仰角(±90)，m_90590_(f_90558_) 返回累计偏航角。
    // 注意：这与字节码里 getYRot/getXRot 的类名语义相反，必须以运行时真实值为准。
    {
        static const char* n[] = {"m_90583_", "getPosition"};
        m_getPosition = resolve_method(c_Camera, "getPosition", n, 2,
                                       "()Lnet/minecraft/world/phys/Vec3;", false);
    }
    {
        static const char* n[] = {"m_90589_", "getYRot"};
        m_getPitch = resolve_method(c_Camera, "pitch", n, 2, "()F", false);
    }
    {
        static const char* n[] = {"m_90590_", "getXRot"};
        m_getYaw = resolve_method(c_Camera, "yaw", n, 2, "()F", false);
    }

    // ---- GameRenderer ----
    {
        static const char* n[] = {"m_109153_", "getMainCamera"};
        m_getMainCamera = resolve_method(c_GameRenderer, "getMainCamera", n, 2,
                                         "()Lnet/minecraft/client/Camera;", false);
    }
    // 实际 FOV（随设置/疾跑/药水等变化），用于精确投影
    // 1.20.1 SRG: m_109141_ (LCamera;FZ)D 返回 double
    {
        static const char* n[] = {"m_109141_", "getFov"};
        m_getFov = resolve_method(c_GameRenderer, "getFov", n, 2,
                                  "(Lnet/minecraft/client/Camera;FZ)D", false);
    }

    // ---- Component / Iterable / Iterator ----
    {
        static const char* n[] = {"getString"};
        m_getString = resolve_method(c_Component, "getString", n, 1, "()Ljava/lang/String;", false);
    }
    {
        static const char* n[] = {"iterator"};
        m_iterator = resolve_method(c_Iterable, "iterator", n, 1, "()Ljava/util/Iterator;", false);
    }
    {
        static const char* n[] = {"hasNext"};
        m_hasNext = resolve_method(c_Iterator, "hasNext", n, 1, "()Z", false);
    }
    {
        static const char* n[] = {"next"};
        m_next = resolve_method(c_Iterator, "next", n, 1, "()Ljava/lang/Object;", false);
    }

    bool ok = c_Minecraft && c_Entity && c_ClientLevel && c_Camera && c_GameRenderer &&
              m_getInstance && m_getFrameTime && f_level && f_gameRenderer &&
              m_entitiesForRendering && f_x && f_y && f_z && m_getBbWidth && m_getBbHeight &&
              m_getType && m_getPosition && m_getPitch && m_getYaw && m_getMainCamera;
    if (ok) g_resolved.store(true, std::memory_order_release);
    esp_log(ok ? "[混淆] 全部关键符号解析成功" : "[混淆] 存在缺失符号，ESP 不可用");
    return ok;
}

void jvm_cleanup() {
    if (!g_env) return;
    auto rel = [](jclass& c) { if (c) { g_env->DeleteGlobalRef(c); c = nullptr; } };
    rel(c_Minecraft); rel(c_ClientLevel); rel(c_Entity); rel(c_LivingEntity);
    rel(c_Player); rel(c_EntityType); rel(c_Vec3); rel(c_Camera); rel(c_GameRenderer);
    rel(c_Matrix4f); rel(c_Component); rel(c_Iterable); rel(c_Iterator); rel(c_RemovalReason);
    rel(c_ChatScreen); rel(c_Projectile); rel(c_ItemStack); rel(c_Item);
    rel(c_HitResult); rel(c_EntityHitResult); rel(c_BlockItem);
    rel(c_ClipContext); rel(c_ClipContext_Block); rel(c_ClipContext_Fluid);
    rel(c_HitResult_Type); rel(c_BlockHitResult);
}

// ------------------------------------------------------------
// 读取相机（FOV 固定从 esp.ini 配置读取，不再使用投影矩阵推导）
// ------------------------------------------------------------
CamData jvm_read_camera() {
    CamData cd{};
    cd.fov = 70.f;
    if (!g_env) return cd;
    g_env->PushLocalFrame(32);

    jobject mc = g_env->CallStaticObjectMethod(c_Minecraft, m_getInstance);
    if (mc && g_env->ExceptionCheck()) { g_env->ExceptionClear(); }
    if (!mc) { g_env->PopLocalFrame(nullptr); return cd; }

    jobject gr = g_env->GetObjectField(mc, f_gameRenderer);
    if (gr && g_env->ExceptionCheck()) { g_env->ExceptionClear(); }
    if (!gr) { g_env->PopLocalFrame(nullptr); return cd; }

    float partialTick = g_env->CallFloatMethod(mc, m_getFrameTime);
    if (g_env->ExceptionCheck()) { g_env->ExceptionClear(); partialTick = 1.f; }

    jobject cam = g_env->CallObjectMethod(gr, m_getMainCamera);
    if (g_env->ExceptionCheck()) { g_env->ExceptionClear(); }
    jobject pos = cam ? g_env->CallObjectMethod(cam, m_getPosition) : nullptr;
    if (g_env->ExceptionCheck()) { g_env->ExceptionClear(); }

    if (cam && pos) {
        cd.px = g_env->GetDoubleField(pos, f_vx);
        cd.py = g_env->GetDoubleField(pos, f_vy);
        cd.pz = g_env->GetDoubleField(pos, f_vz);
        // 运行时实测：m_90589_=俯仰(±90), m_90590_=累计偏航
        cd.pitch = g_env->CallFloatMethod(cam, m_getPitch);
        cd.yaw = g_env->CallFloatMethod(cam, m_getYaw);
        // 偏航限制到 [0,360) 避免溢出不影响投影正确性
        if (cd.yaw < 0.f) cd.yaw += 360.f;
        else if (cd.yaw >= 360.f) cd.yaw = fmodf(cd.yaw, 360.f);
        if (g_env->ExceptionCheck()) g_env->ExceptionClear();

        // 实际 FOV：changingFov=true 时含疾跑/药水等缩放，与游戏画面一致
        if (m_getFov) {
            cd.fov = (float)g_env->CallDoubleMethod(gr, m_getFov, cam, partialTick, JNI_TRUE);
            if (g_env->ExceptionCheck()) g_env->ExceptionClear();
            if (!(cd.fov > 10.f && cd.fov < 160.f)) cd.fov = 70.f;   // 防御值
        }
        cd.ok = true;
    }

    // 当前打开界面检测（Esc/背包/聊天…）：mc.screen != null → guiOpen=true（默认隐藏 ESP）。
    // 若打开的是聊天界面（按 T 打开 ChatScreen），记录 screenIsChat，由 esp.cpp 按
    // keepOnChat 配置决定是否仍隐藏——聊天界面下通常希望继续渲染 ESP。
    if (f_screen) {
        jobject scr = g_env->GetObjectField(mc, f_screen);
        if (g_env->ExceptionCheck()) g_env->ExceptionClear();
        cd.guiOpen = (scr != nullptr);
        cd.screenIsChat = (scr && c_ChatScreen) ? g_env->IsInstanceOf(scr, c_ChatScreen) : false;
        if (g_env->ExceptionCheck()) g_env->ExceptionClear();
        if (scr) g_env->DeleteLocalRef(scr);
    }

    cd.partialTick = partialTick;
    g_env->PopLocalFrame(nullptr);
    return cd;
}

// ------------------------------------------------------------
// 读取本地玩家信息（弓蓄力预判轨迹用）
// ------------------------------------------------------------
PlayerInfo jvm_read_player() {
    PlayerInfo p;
    if (!g_env) return p;
    g_env->PushLocalFrame(32);

    jobject mc = g_env->CallStaticObjectMethod(c_Minecraft, m_getInstance);
    if (mc && g_env->ExceptionCheck()) { g_env->ExceptionClear(); }
    if (!mc) { g_env->PopLocalFrame(nullptr); return p; }

    jobject self = f_player ? g_env->GetObjectField(mc, f_player) : nullptr;
    if (g_env->ExceptionCheck()) g_env->ExceptionClear();
    if (!self) { g_env->PopLocalFrame(nullptr); return p; }

    p.px = g_env->GetDoubleField(self, f_x);
    p.py = g_env->GetDoubleField(self, f_y);
    p.pz = g_env->GetDoubleField(self, f_z);
    p.yaw   = m_getYRot ? g_env->CallFloatMethod(self, m_getYRot) : 0.f;
    p.pitch = m_getXRot ? g_env->CallFloatMethod(self, m_getXRot) : 0.f;
    p.eyeHeight = m_getEyeHeight ? g_env->CallFloatMethod(self, m_getEyeHeight) : 1.62f;
    if (g_env->ExceptionCheck()) g_env->ExceptionClear();

    // 玩家当前速度（动量继承：水平恒继承，垂直仅非地面时继承）
    p.onGround = f_onGround ? g_env->GetBooleanField(self, f_onGround) : false;
    if (g_env->ExceptionCheck()) g_env->ExceptionClear();
    if (m_getDeltaMovement) {
        jobject mv = g_env->CallObjectMethod(self, m_getDeltaMovement);
        if (g_env->ExceptionCheck()) g_env->ExceptionClear();
        if (mv) {
            p.vx = g_env->GetDoubleField(mv, f_vx);
            p.vy = g_env->GetDoubleField(mv, f_vy);
            p.vz = g_env->GetDoubleField(mv, f_vz);
            if (g_env->ExceptionCheck()) g_env->ExceptionClear();
            g_env->DeleteLocalRef(mv);
        }
    }

    // 判定正在拉弓：使用物品中 + 主手物品是弓(bow)
    if (m_isUsingItem && m_getMainHandItem && m_getItem && m_getItemDesc) {
        if (g_env->CallBooleanMethod(self, m_isUsingItem)) {
            if (g_env->ExceptionCheck()) g_env->ExceptionClear();
            p.useTicks = m_getTicksUsingItem ? g_env->CallIntMethod(self, m_getTicksUsingItem) : 0;
            if (g_env->ExceptionCheck()) g_env->ExceptionClear();
            jobject stack = g_env->CallObjectMethod(self, m_getMainHandItem);
            if (g_env->ExceptionCheck()) g_env->ExceptionClear();
            if (stack) {
                jobject item = g_env->CallObjectMethod(stack, m_getItem);
                if (g_env->ExceptionCheck()) g_env->ExceptionClear();
                if (item) {
                    jstring s = (jstring)g_env->CallObjectMethod(item, m_getItemDesc);
                    if (g_env->ExceptionCheck()) g_env->ExceptionClear();
                    if (s) {
                        const char* utf = g_env->GetStringUTFChars(s, nullptr);
                        if (utf) {
                            p.chargingBow = (strcmp(utf, "item.minecraft.bow") == 0);
                            g_env->ReleaseStringUTFChars(s, utf);
                        }
                        g_env->DeleteLocalRef(s);
                    }
                    g_env->DeleteLocalRef(item);
                }
                g_env->DeleteLocalRef(stack);
            }
        }
    }

    p.ok = true;
    g_env->PopLocalFrame(nullptr);
    return p;
}

// ------------------------------------------------------------
// 读取连点器门控状态（移植 MCCombatStatusJni 的 UpdateStatus 判定逻辑）：
//   canAttack = hitResult.getType()==ENTITY
//            && entity instanceof LivingEntity
//            && entity.isAlive()
//            && entity != 本地玩家
//            && entity.isAttackable()   // 1.20.1 forge1201 映射
//   canPlace  = player.getMainHandItem().getItem() instanceof BlockItem
// 全部在 SwapBuffers 钩子内直接读取；不创建 socket / 共享内存。
// ------------------------------------------------------------
CombatStatus jvm_read_combat_status() {
    CombatStatus st;
    if (!g_env) return st;
    if (!c_Minecraft || !m_getInstance || !f_player || !f_hitResult ||
        !c_LivingEntity || !c_HitResult_Type) return st;

    g_env->PushLocalFrame(64);

    jobject mc = g_env->CallStaticObjectMethod(c_Minecraft, m_getInstance);
    if (g_env->ExceptionCheck()) g_env->ExceptionClear();
    if (!mc) { g_env->PopLocalFrame(nullptr); return st; }

    jobject player = f_player ? g_env->GetObjectField(mc, f_player) : nullptr;
    if (g_env->ExceptionCheck()) g_env->ExceptionClear();
    st.inGame = (player != nullptr);

    // ---- canPlace：只依赖玩家手持物品，与准星无关 ----
    if (player && m_getMainHandItem && m_getItem && c_BlockItem) {
        jobject stack = g_env->CallObjectMethod(player, m_getMainHandItem);
        if (g_env->ExceptionCheck()) g_env->ExceptionClear();
        if (stack) {
            jobject item = g_env->CallObjectMethod(stack, m_getItem);
            if (g_env->ExceptionCheck()) g_env->ExceptionClear();
            if (item) {
                st.canPlace = g_env->IsInstanceOf(item, c_BlockItem) == JNI_TRUE;
                g_env->ExceptionClear();
                g_env->DeleteLocalRef(item);
            }
            g_env->DeleteLocalRef(stack);
        }
    }

    // ---- canAttack：准星命中实体 + 目标存活/可攻击/非自身 ----
    jobject mop = f_hitResult ? g_env->GetObjectField(mc, f_hitResult) : nullptr;
    if (g_env->ExceptionCheck()) g_env->ExceptionClear();
    if (mop && m_hitResultGetType) {
        // HitResult$Type.ENTITY（Forge 运行时枚举名；部分命名空间用混淆名 c）
        static jobject g_typeEntity = nullptr;
        if (!g_typeEntity) {
            jfieldID f = g_env->GetStaticFieldID(c_HitResult_Type, "ENTITY",
                    "Lnet/minecraft/world/phys/HitResult$Type;");
            if (g_env->ExceptionCheck()) { g_env->ExceptionClear(); f = nullptr; }
            if (!f) {
                f = g_env->GetStaticFieldID(c_HitResult_Type, "c",
                        "Lnet/minecraft/world/phys/HitResult$Type;");
                if (g_env->ExceptionCheck()) { g_env->ExceptionClear(); f = nullptr; }
            }
            if (f) {
                jobject v = g_env->GetStaticObjectField(c_HitResult_Type, f);
                if (g_env->ExceptionCheck()) g_env->ExceptionClear();
                if (v) g_typeEntity = g_env->NewGlobalRef(v);
                if (v) g_env->DeleteLocalRef(v);
            }
        }

        jobject typeObj = g_env->CallObjectMethod(mop, m_hitResultGetType);
        if (g_env->ExceptionCheck()) g_env->ExceptionClear();
        if (typeObj) {
            if (g_typeEntity && g_env->IsSameObject(typeObj, g_typeEntity)) {
                st.hitType = 2;
                jobject entity = m_hitGetEntity ? g_env->CallObjectMethod(mop, m_hitGetEntity) : nullptr;
                if (g_env->ExceptionCheck()) g_env->ExceptionClear();
                if (entity) {
                    st.targetLiving = g_env->IsInstanceOf(entity, c_LivingEntity) == JNI_TRUE;
                    g_env->ExceptionClear();
                    if (st.targetLiving) {
                        st.targetAlive = m_isAlive ? g_env->CallBooleanMethod(entity, m_isAlive) == JNI_TRUE : true;
                        if (g_env->ExceptionCheck()) g_env->ExceptionClear();
                        st.targetAttackable = m_isAttackable ? g_env->CallBooleanMethod(entity, m_isAttackable) == JNI_TRUE : true;
                        if (g_env->ExceptionCheck()) g_env->ExceptionClear();
                    }
                    bool isSelf = player && g_env->IsSameObject(entity, player);
                    if (g_env->ExceptionCheck()) g_env->ExceptionClear();
                    st.canAttack = st.targetLiving && st.targetAlive &&
                                   st.targetAttackable && !isSelf;
                    g_env->DeleteLocalRef(entity);
                }
            } else {
                st.hitType = 1;   // 非实体：方块或 MISS 均按不可攻击处理
            }
            g_env->DeleteLocalRef(typeObj);
        }
        g_env->DeleteLocalRef(mop);
    }

    if (player) g_env->DeleteLocalRef(player);
    g_env->PopLocalFrame(nullptr);
    st.ok = true;
    return st;
}

// ------------------------------------------------------------
// 方块射线检测：对线段 [from,to] 用 level.clip(ClipContext) 做碰撞检测
// （仿 LiquidBounce 的 world.clip）。命中则 outHit=true 并输出命中点；
// 否则 outHit=false。符号未解析/出错时返回 false（调用方跳过方块检测）。
// ------------------------------------------------------------
bool jvm_clip_block(double x0, double y0, double z0,
                    double x1, double y1, double z1,
                    double& hx, double& hy, double& hz, bool& outHit) {
    outHit = false;
    if (!g_env || !m_clip || !m_v3Ctor || !m_ccCtor || !m_hitGetType || !m_hitGetLocation) return false;

    // 惰性解析枚举常量（COLLIDER / NONE / MISS）：Forge 运行时枚举字段名即常量名
    static jobject g_collider = nullptr, g_none = nullptr, g_miss = nullptr;
    if (!g_collider && c_ClipContext_Block) {
        if (jfieldID f = g_env->GetStaticFieldID(c_ClipContext_Block, "COLLIDER",
                "Lnet/minecraft/world/level/ClipContext$Block;")) {
            if (!g_env->ExceptionCheck()) {
                jobject c = g_env->GetStaticObjectField(c_ClipContext_Block, f);
                if (!g_env->ExceptionCheck() && c) g_collider = g_env->NewGlobalRef(c);
            } else g_env->ExceptionClear();
        } else g_env->ExceptionClear();
        if (jfieldID f = g_env->GetStaticFieldID(c_ClipContext_Fluid, "NONE",
                "Lnet/minecraft/world/level/ClipContext$Fluid;")) {
            if (!g_env->ExceptionCheck()) {
                jobject c = g_env->GetStaticObjectField(c_ClipContext_Fluid, f);
                if (!g_env->ExceptionCheck() && c) g_none = g_env->NewGlobalRef(c);
            } else g_env->ExceptionClear();
        } else g_env->ExceptionClear();
        if (jfieldID f = g_env->GetStaticFieldID(c_HitResult_Type, "MISS",
                "Lnet/minecraft/world/phys/HitResult$Type;")) {
            if (!g_env->ExceptionCheck()) {
                jobject c = g_env->GetStaticObjectField(c_HitResult_Type, f);
                if (!g_env->ExceptionCheck() && c) g_miss = g_env->NewGlobalRef(c);
            } else g_env->ExceptionClear();
        } else g_env->ExceptionClear();
    }
    if (!g_collider || !g_none || !g_miss) return false;

    g_env->PushLocalFrame(64);
    jobject mc = g_env->CallStaticObjectMethod(c_Minecraft, m_getInstance);
    if (mc && g_env->ExceptionCheck()) g_env->ExceptionClear();
    if (!mc) { g_env->PopLocalFrame(nullptr); return false; }
    jobject self = f_player ? g_env->GetObjectField(mc, f_player) : nullptr;
    if (g_env->ExceptionCheck()) g_env->ExceptionClear();
    jobject level = g_env->GetObjectField(mc, f_level);
    if (g_env->ExceptionCheck()) g_env->ExceptionClear();
    if (!level || !self) { g_env->PopLocalFrame(nullptr); return false; }

    jobject v0 = g_env->NewObject(c_Vec3, m_v3Ctor, x0, y0, z0);
    jobject v1 = g_env->NewObject(c_Vec3, m_v3Ctor, x1, y1, z1);
    if (g_env->ExceptionCheck()) { g_env->ExceptionClear(); g_env->PopLocalFrame(nullptr); return false; }
    jobject ctx = g_env->NewObject(c_ClipContext, m_ccCtor, v0, v1, g_collider, g_none, self);
    if (g_env->ExceptionCheck()) { g_env->ExceptionClear(); g_env->PopLocalFrame(nullptr); return false; }
    jobject res = g_env->CallObjectMethod(level, m_clip, ctx);
    if (g_env->ExceptionCheck()) { g_env->ExceptionClear(); g_env->PopLocalFrame(nullptr); return false; }

    jobject type = g_env->CallObjectMethod(res, m_hitGetType);
    if (g_env->ExceptionCheck()) { g_env->ExceptionClear(); g_env->PopLocalFrame(nullptr); return false; }
    bool isMiss = type && g_env->IsSameObject(type, g_miss);
    if (!isMiss) {
        jobject loc = g_env->CallObjectMethod(res, m_hitGetLocation);
        if (g_env->ExceptionCheck()) { g_env->ExceptionClear(); g_env->PopLocalFrame(nullptr); return false; }
        if (loc) {
            hx = g_env->GetDoubleField(loc, f_vx);
            hy = g_env->GetDoubleField(loc, f_vy);
            hz = g_env->GetDoubleField(loc, f_vz);
            if (g_env->ExceptionCheck()) { g_env->ExceptionClear(); g_env->PopLocalFrame(nullptr); return false; }
            outHit = true;
        }
    }
    g_env->PopLocalFrame(nullptr);
    return true;
}

// ------------------------------------------------------------
// 采集实体
// ------------------------------------------------------------
int jvm_collect_entities(double camX, double camY, double camZ, float partialTick,
                         bool needAimData, bool needAimVelocity,
                         std::vector<EntityData>& out) {
    if (!g_env) return 0;
    if (!f_x || !f_y || !f_z || !f_xo || !f_yo || !f_zo) return 0;
    g_env->PushLocalFrame(512);
    int count = 0;

    // 名字周期性刷新：每 2s 重读一次（跟踪改名），其余帧直接用缓存，
    // 名字 JNI 开销归零（游戏渲染线程 SwapBuffers 钩子里省掉 150+ 次字符串拷贝）。
    DWORD nowT = GetTickCount();
    bool refreshNames = (nowT - g_nameRefreshTick >= kNameRefreshMs);
    if (refreshNames) g_nameRefreshTick = nowT;
    // 元数据周期性刷新：每 1s 重读一次（跟踪尺寸/类型变化），其余帧直接用缓存
    bool refreshMeta = (nowT - g_metaRefreshTick >= kMetaRefreshMs);
    if (refreshMeta) g_metaRefreshTick = nowT;

    jobject mc = g_env->CallStaticObjectMethod(c_Minecraft, m_getInstance);
    if (mc && g_env->ExceptionCheck()) { g_env->ExceptionClear(); }
    if (!mc) { g_env->PopLocalFrame(nullptr); return 0; }

    jobject level = g_env->GetObjectField(mc, f_level);
    if (g_env->ExceptionCheck()) g_env->ExceptionClear();
    if (!level) { g_env->PopLocalFrame(nullptr); return 0; }

    // 本地玩家（自己）不显示
    jobject self = f_player ? g_env->GetObjectField(mc, f_player) : nullptr;
    if (g_env->ExceptionCheck()) g_env->ExceptionClear();

    jobject it = g_env->CallObjectMethod(level, m_entitiesForRendering);
    if (g_env->ExceptionCheck()) g_env->ExceptionClear();
    jobject iter = it ? g_env->CallObjectMethod(it, m_iterator) : nullptr;
    if (g_env->ExceptionCheck()) g_env->ExceptionClear();
    if (!iter) { g_env->PopLocalFrame(nullptr); return 0; }

    while (g_env->CallBooleanMethod(iter, m_hasNext)) {
        if (g_env->ExceptionCheck()) { g_env->ExceptionClear(); break; }
        jobject ent = g_env->CallObjectMethod(iter, m_next);
        if (g_env->ExceptionCheck()) { g_env->ExceptionClear(); continue; }
        if (!ent) continue;

        // 跳过本地玩家自己
        if (self && g_env->IsSameObject(ent, self)) { g_env->DeleteLocalRef(ent); continue; }

        // 移除中的实体跳过
        if (m_getRemovalReason) {
            jobject reason = g_env->CallObjectMethod(ent, m_getRemovalReason);
            if (g_env->ExceptionCheck()) g_env->ExceptionClear();
            if (reason) { g_env->DeleteLocalRef(reason); g_env->DeleteLocalRef(ent); continue; }
        }

        EntityData d;
        // 实体唯一网络 ID：用作渲染线程盒子平滑状态的关键字
        d.id = m_getId ? g_env->CallIntMethod(ent, m_getId) : 0;
        if (g_env->ExceptionCheck()) g_env->ExceptionClear();
        double x = g_env->GetDoubleField(ent, f_x);
        double y = g_env->GetDoubleField(ent, f_y);
        double z = g_env->GetDoubleField(ent, f_z);
        double xo = g_env->GetDoubleField(ent, f_xo);
        double yo = g_env->GetDoubleField(ent, f_yo);
        double zo = g_env->GetDoubleField(ent, f_zo);
        float t = (partialTick > 0.f && partialTick <= 1.f) ? partialTick : 1.f;
        // 位置插值，减少渲染抖动
        d.ix = xo + (x - xo) * t;
        d.iy = yo + (y - yo) * t;
        d.iz = zo + (z - zo) * t;
        // 原始插值位置：渲染侧会做帧间平滑，假人瞬移检测必须用未平滑值
        d.rx = d.ix;
        d.ry = d.iy;
        d.rz = d.iz;

        d.bbw = 0.6f; d.bbh = 1.8f;
        d.isPlayer = false; d.isLiving = false; d.projType = PROJ_NONE;
        // 元数据（尺寸/类型）：优先从缓存（每 1s 刷新一次），避免每帧
        // 对每个实体做多次 JNI 调用——这是“每帧采集位置”不增开销的前提。
        auto mit = g_metaCache.find(d.id);
        if (!refreshMeta && mit != g_metaCache.end()) {
            mit->second.lastSeen = nowT;    // 刷新“最后出现”时间
            d.bbw = mit->second.bbw; d.bbh = mit->second.bbh;
            d.isPlayer = mit->second.isPlayer; d.isLiving = mit->second.isLiving;
            d.projType = mit->second.projType;
        } else {
            EntityMeta mt;
            mt.bbw = m_getBbWidth ? g_env->CallFloatMethod(ent, m_getBbWidth) : 0.6f;
            mt.bbh = m_getBbHeight ? g_env->CallFloatMethod(ent, m_getBbHeight) : 1.8f;
            if (g_env->ExceptionCheck()) g_env->ExceptionClear();
            mt.isPlayer = g_env->IsInstanceOf(ent, c_Player);
            mt.isLiving = g_env->IsInstanceOf(ent, c_LivingEntity);
            // 弹射物类型检测：getType().getDescriptionId() 匹配（每 1s 一次，缓存）
            mt.projType = PROJ_NONE;
            if (m_getType && m_getDescriptionId) {
                jobject type = g_env->CallObjectMethod(ent, m_getType);
                if (g_env->ExceptionCheck()) g_env->ExceptionClear();
                if (type) {
                    jstring s = (jstring)g_env->CallObjectMethod(type, m_getDescriptionId);
                    if (g_env->ExceptionCheck()) g_env->ExceptionClear();
                    if (s) {
                        const char* utf = g_env->GetStringUTFChars(s, nullptr);
                        if (utf) {
                            const char* Arrow = "entity.minecraft.arrow";
                            const char* SArow = "entity.minecraft.spectral_arrow";
                            const char* Snow  = "entity.minecraft.snowball";
                            const char* Pearl = "entity.minecraft.ender_pearl";
                            if (strcmp(utf, Arrow) == 0 || strcmp(utf, SArow) == 0)
                                mt.projType = PROJ_ARROW;
                            else if (strcmp(utf, Snow) == 0)
                                mt.projType = PROJ_SNOWBALL;
                            else if (strcmp(utf, Pearl) == 0)
                                mt.projType = PROJ_PEARL;
                            g_env->ReleaseStringUTFChars(s, utf);
                        }
                        g_env->DeleteLocalRef(s);
                    }
                    g_env->DeleteLocalRef(type);
                }
            }
            mt.lastSeen = nowT;
            if (d.id != 0) g_metaCache[d.id] = mt;
            d.bbw = mt.bbw; d.bbh = mt.bbh;
            d.isPlayer = mt.isPlayer; d.isLiving = mt.isLiving;
            d.projType = mt.projType;
        }

        // 弹射物：每帧读取当前速度（getDeltaMovement），用于轨迹物理预测；
        // 并读取归属（getOwner），标记是否本地玩家射出（该类轨迹由弓预判覆盖，不重复画）。
        // 弹射物数量很少（通常 <10），每帧这些 JNI 调用可忽略。
        if (d.projType != PROJ_NONE) {
            if (m_getDeltaMovement) {
                jobject mv = g_env->CallObjectMethod(ent, m_getDeltaMovement);
                if (g_env->ExceptionCheck()) g_env->ExceptionClear();
                if (mv) {
                    d.vx = g_env->GetDoubleField(mv, f_vx);
                    d.vy = g_env->GetDoubleField(mv, f_vy);
                    d.vz = g_env->GetDoubleField(mv, f_vz);
                    d.hasVelocity = true;
                    if (g_env->ExceptionCheck()) g_env->ExceptionClear();
                    g_env->DeleteLocalRef(mv);
                }
            }
            if (m_getOwner && self) {
                jobject owner = g_env->CallObjectMethod(ent, m_getOwner);
                if (g_env->ExceptionCheck()) g_env->ExceptionClear();
                if (owner) {
                    d.ownProjectile = g_env->IsSameObject(owner, self);
                    g_env->DeleteLocalRef(owner);
                }
            }
        }

        // 自瞄数据：仅 needAimData 且为生物时读取生命值；
        // needAimVelocity 时才读速度（预判用）。
        if (needAimData && d.isLiving) {
            if (needAimVelocity && m_getDeltaMovement) {
                jobject mv = g_env->CallObjectMethod(ent, m_getDeltaMovement);
                if (g_env->ExceptionCheck()) g_env->ExceptionClear();
                if (mv) {
                    d.vx = g_env->GetDoubleField(mv, f_vx);
                    d.vy = g_env->GetDoubleField(mv, f_vy);
                    d.vz = g_env->GetDoubleField(mv, f_vz);
                    d.hasVelocity = true;
                    if (g_env->ExceptionCheck()) g_env->ExceptionClear();
                    g_env->DeleteLocalRef(mv);
                }
            }
            if (m_getHealth && m_getMaxHealth) {
                d.health = g_env->CallFloatMethod(ent, m_getHealth);
                if (g_env->ExceptionCheck()) g_env->ExceptionClear();
                d.maxHealth = g_env->CallFloatMethod(ent, m_getMaxHealth);
                if (g_env->ExceptionCheck()) g_env->ExceptionClear();
                d.healthValid = (d.maxHealth > 0.0f);
            } else if (m_isAlive) {
                const bool alive = g_env->CallBooleanMethod(ent, m_isAlive) == JNI_TRUE;
                if (g_env->ExceptionCheck()) g_env->ExceptionClear();
                d.health = alive ? 1.0f : 0.0f;
                d.maxHealth = 1.0f;
                d.healthValid = true;
            }
        }

        // 其他玩家弓蓄力预判：仅玩家实体，实时读是否正在使用弓（isUsingItem）、
        // 蓄力 tick、主手是否弓、朝向。玩家数量少（通常 <20），每帧这些 JNI 可忽略。
        // 只对使用物品的玩家深入读取主手物品，减少开销。
        d.chargingBow = false; d.useTicks = 0;
        if (d.isPlayer && m_isUsingItem && m_getYRot && m_getXRot) {
            bool usingItem = false;
            if (m_isUsingItem) {
                usingItem = g_env->CallBooleanMethod(ent, m_isUsingItem);
                if (g_env->ExceptionCheck()) g_env->ExceptionClear();
            }
            int ut = 0;
            if (usingItem && m_getTicksUsingItem) {
                ut = g_env->CallIntMethod(ent, m_getTicksUsingItem);
                if (g_env->ExceptionCheck()) g_env->ExceptionClear();
            }
            if (usingItem && ut > 0 && m_getMainHandItem && m_getItem && m_getItemDesc) {
                jobject stack = g_env->CallObjectMethod(ent, m_getMainHandItem);
                if (g_env->ExceptionCheck()) g_env->ExceptionClear();
                if (stack) {
                    jobject item = g_env->CallObjectMethod(stack, m_getItem);
                    if (g_env->ExceptionCheck()) g_env->ExceptionClear();
                    if (item) {
                        jstring s = (jstring)g_env->CallObjectMethod(item, m_getItemDesc);
                        if (g_env->ExceptionCheck()) g_env->ExceptionClear();
                        if (s) {
                            const char* utf = g_env->GetStringUTFChars(s, nullptr);
                            if (utf) {
                                if (strcmp(utf, "item.minecraft.bow") == 0) {
                                    d.chargingBow = true;
                                    d.useTicks = ut;
                                }
                                g_env->ReleaseStringUTFChars(s, utf);
                            }
                            g_env->DeleteLocalRef(s);
                        }
                        g_env->DeleteLocalRef(item);
                    }
                    g_env->DeleteLocalRef(stack);
                }
            }
            // 朝向/动量/是否在地面：仅蓄力时读，避免对每个玩家每帧多做 JNI。
            // 动量用于弓轨迹预判：MC 1.20.1 箭出生时继承射手水平动量，
            // 且仅当射手不在地面时继承垂直动量（跳跃/下落轨迹修正）。
            if (d.chargingBow) {
                d.yaw = g_env->CallFloatMethod(ent, m_getYRot);
                if (g_env->ExceptionCheck()) g_env->ExceptionClear();
                d.pitch = g_env->CallFloatMethod(ent, m_getXRot);
                if (g_env->ExceptionCheck()) g_env->ExceptionClear();
                if (m_getDeltaMovement) {
                    jobject mv = g_env->CallObjectMethod(ent, m_getDeltaMovement);
                    if (g_env->ExceptionCheck()) g_env->ExceptionClear();
                    if (mv) {
                        d.vx = g_env->GetDoubleField(mv, f_vx);
                        d.vy = g_env->GetDoubleField(mv, f_vy);
                        d.vz = g_env->GetDoubleField(mv, f_vz);
                        if (g_env->ExceptionCheck()) g_env->ExceptionClear();
                        g_env->DeleteLocalRef(mv);
                    }
                }
                if (f_onGround) {
                    d.onGround = g_env->GetBooleanField(ent, f_onGround) == JNI_TRUE;
                    if (g_env->ExceptionCheck()) g_env->ExceptionClear();
                }
            }
        }

        double dx = d.ix - camX, dy = d.iy - camY, dz = d.iz - camZ;
        d.dist = std::sqrt(dx * dx + dy * dy + dz * dz);

        // 名字：优先从缓存（每 2s 刷新一次），避免每帧对每个实体重复做
        // getName()->getString()->GetStringChars 的字符串拷贝——这是游戏渲染线程
        // SwapBuffers 钩子里的主要 JNI 开销，缓存后每帧名字开销归零。
        std::wstring name;
        auto cit = g_nameCache.find(d.id);
        if (!refreshNames && cit != g_nameCache.end()) {
            cit->second.second = nowT;      // 刷新“最后出现”时间
            name = cit->second.first;
        } else {
            // 命中缓存失败（首帧或每 2s 刷新）：走 JNI 读名字
            if (m_getName) {
                jobject comp = g_env->CallObjectMethod(ent, m_getName);
                if (g_env->ExceptionCheck()) g_env->ExceptionClear();
                if (comp && m_getString) {
                    jstring s = (jstring)g_env->CallObjectMethod(comp, m_getString);
                    if (g_env->ExceptionCheck()) g_env->ExceptionClear();
                    if (s) {
                        const jchar* cw = g_env->GetStringChars(s, nullptr);
                        if (cw) {
                            jsize len = g_env->GetStringLength(s);
                            name.assign((wchar_t*)cw, (size_t)len);
                            g_env->ReleaseStringChars(s, cw);
                        }
                        g_env->DeleteLocalRef(s);
                    }
                    g_env->DeleteLocalRef(comp);
                }
            }
            if (name.empty() && m_getType && m_getDescriptionId) {
                jobject type = g_env->CallObjectMethod(ent, m_getType);
                if (g_env->ExceptionCheck()) g_env->ExceptionClear();
                if (type) {
                    jstring s = (jstring)g_env->CallObjectMethod(type, m_getDescriptionId);
                    if (g_env->ExceptionCheck()) g_env->ExceptionClear();
                    if (s) {
                        const char* utf = g_env->GetStringUTFChars(s, nullptr);
                        if (utf) {
                            // "entity.minecraft.zombie" -> "zombie"
                            std::string n = utf;
                            const char* prefix = "entity.minecraft.";
                            size_t p = n.find(prefix);
                            n = (p == std::string::npos) ? n : n.substr(p + strlen(prefix));
                            // UTF-8 -> wide（实体类型名均为 ASCII，直接扩宽即可）
                            name.assign(n.begin(), n.end());
                            g_env->ReleaseStringUTFChars(s, utf);
                        }
                        g_env->DeleteLocalRef(s);
                    }
                    g_env->DeleteLocalRef(type);
                }
            }
            if (name.empty()) name = L"?";
            if (d.id != 0) g_nameCache[d.id] = {name, nowT};
        }
        d.name = std::move(name);

        out.push_back(std::move(d));
        ++count;
        // 释放本实体本地引用：不释放会随实体数累积，超大服务器（数百实体）时
        // 超过 PushLocalFrame(512) → JNI 本地引用表溢出 → 抛异常/崩溃。
        g_env->DeleteLocalRef(ent);
    }

    // 定期清理长时间未出现的实体名字缓存项（实体消失/传送后不再占用内存）
    if (nowT - g_namePruneTick >= 5000) {
        g_namePruneTick = nowT;
        for (auto it = g_nameCache.begin(); it != g_nameCache.end();) {
            if (nowT - it->second.second >= kNameKeepMs) it = g_nameCache.erase(it);
            else ++it;
        }
        // 元数据缓存一并清理（同样按“最后出现”时间）
        for (auto it = g_metaCache.begin(); it != g_metaCache.end();) {
            if (nowT - it->second.lastSeen >= kNameKeepMs) it = g_metaCache.erase(it);
            else ++it;
        }
    }

    g_env->PopLocalFrame(nullptr);
    return count;
}
