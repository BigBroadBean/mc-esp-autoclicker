// ============================================================
//  aimbot.cpp — 人类化自瞄实现
//
// 设计要点：
//   1) 目标点由游戏渲染线程（SwapBuffers 钩子）每帧计算并发布，
//      移动线程只管把鼠标“像人一样”移过去，避免 JNI 跨线程。
//   2) 250Hz 相对移动（SendInput），带启动加速、接近减速、噪声和
//      反应延迟；每帧目标会随相机变化重新计算，形成闭环修正。
//   3) 准星命中碰撞箱后切换为“视平线高度水平中心”，避免锁边缘。
// ============================================================
#include "aimbot.h"
#include "math3d.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

#include <mmsystem.h>   // timeBeginPeriod / timeEndPeriod

// ------------------------------------------------------------
// 运行状态
// ------------------------------------------------------------
static std::mutex        g_settingsMutex;
static AimSettings       g_settings;
static HWND              g_gameHwnd = nullptr;
static std::atomic<bool> g_threadRunning{false};
static std::atomic<bool> g_stop{false};
static std::atomic<bool> g_menuOpen{false};
static std::atomic<bool> g_active{false};
static std::atomic<bool> g_toggleOn{false};
static std::atomic<bool> g_hotkeyDown{false};   // 渲染线程热键路径同步写入
static std::atomic<int>  g_visualMode{0};
static HANDLE            g_threadHandle = nullptr;

static std::mutex        g_targetMutex;
static AimTarget         g_target;

static void clamp_settings(AimSettings& a) {
    if (a.triggerMode < 0) a.triggerMode = 0;
    if (a.triggerMode >= AIM_TRIGGER_COUNT) a.triggerMode = AIM_TRIGGER_HOLD_LMB;
    if (a.priority < 0) a.priority = 0;
    if (a.priority > AIM_PRIORITY_HEALTH) a.priority = AIM_PRIORITY_CROSSHAIR;
    if (a.fov < 10.f) a.fov = 10.f;
    if (a.fov > 180.f) a.fov = 180.f;
    if (a.maxDistance < 5.0) a.maxDistance = 5.0;
    if (a.maxDistance > 200.0) a.maxDistance = 200.0;
    if (a.smooth < 1) a.smooth = 1;
    if (a.smooth > 10) a.smooth = 10;
    if (a.reactionMs < 0) a.reactionMs = 0;
    if (a.reactionMs > 300) a.reactionMs = 300;
    if (a.mouseSensitivity < 0.5f) a.mouseSensitivity = 0.5f;
    if (a.mouseSensitivity > 2.0f) a.mouseSensitivity = 2.0f;
    if (a.predictionTicks < 0) a.predictionTicks = 0;
    if (a.predictionTicks > 20) a.predictionTicks = 20;
    if (a.switchCooldownMs < 0) a.switchCooldownMs = 0;
    if (a.switchCooldownMs > 2000) a.switchCooldownMs = 2000;
    if (a.secondTarget < 0) a.secondTarget = 0;
    if (a.secondTarget >= AIM_SECOND_COUNT) a.secondTarget = AIM_SECOND_LEVEL;
    if (a.secondSmooth < 1) a.secondSmooth = 1;
    if (a.secondSmooth > 10) a.secondSmooth = 10;
    if (a.stability < 0) a.stability = 0;
    if (a.stability > 30) a.stability = 30;
    if (a.visualMode < 0) a.visualMode = 0;
    if (a.visualMode > 3) a.visualMode = 3;
}

// ------------------------------------------------------------
// 设置与目标发布
// ------------------------------------------------------------
void aimbot_apply_settings(const AimSettings& s) {
    AimSettings next = s;
    clamp_settings(next);
    {
        std::lock_guard<std::mutex> lk(g_settingsMutex);
        bool modeChanged = (g_settings.triggerMode != next.triggerMode);
        g_settings = next;
        if (modeChanged) g_toggleOn.store(false, std::memory_order_release);
    }
    g_visualMode.store(next.visualMode, std::memory_order_release);
}

bool aimbot_active() {
    return g_active.load(std::memory_order_acquire);
}

bool aimbot_visual_wanted() {
    return g_active.load(std::memory_order_acquire) &&
           g_visualMode.load(std::memory_order_acquire) > 0;
}

void aimbot_set_menu_open(bool open) {
    g_menuOpen.store(open, std::memory_order_release);
    if (open) {   // 打开菜单瞬间清掉目标，避免线程在未来 4ms 内再补一次鼠标
        AimTarget t;
        t.frameMs = GetTickCount();
        std::lock_guard<std::mutex> lk(g_targetMutex);
        g_target = t;
    }
}

void aimbot_set_hotkey_down(bool down) {
    g_hotkeyDown.store(down, std::memory_order_release);
}

void aimbot_toggle_trigger() {
    const bool next = !g_toggleOn.load(std::memory_order_acquire);
    aimbot_set_toggle_on(next);
}

void aimbot_set_toggle_on(bool on) {
    g_toggleOn.store(on, std::memory_order_release);
    esp_log("[aim] 切换状态 -> %s", on ? "开" : "关");
}

bool aimbot_toggle_on() {
    return g_toggleOn.load(std::memory_order_acquire);
}

void aimbot_clear_target() {
    AimTarget t;
    t.frameMs = GetTickCount();
    std::lock_guard<std::mutex> lk(g_targetMutex);
    g_target = t;
}

static void publish_target(const AimTarget& t) {
    std::lock_guard<std::mutex> lk(g_targetMutex);
    g_target = t;
}

static void publish_no_target(float radiusPx) {
    AimTarget t;
    t.frameMs = GetTickCount();
    t.fovRadiusPx = radiusPx;
    std::lock_guard<std::mutex> lk(g_targetMutex);
    g_target = t;
}

bool aimbot_get_target(AimTarget& out) {
    std::lock_guard<std::mutex> lk(g_targetMutex);
    out = g_target;
    return out.valid;
}

// ------------------------------------------------------------
// 几何：准星射线 vs 目标碰撞箱（AABB）
// ------------------------------------------------------------
struct Aabb { double minX, minY, minZ, maxX, maxY, maxZ; };

static bool ray_aabb(double ox, double oy, double oz,
                     double fx, double fy, double fz,
                     const Aabb& b, double& tNear) {
    double tmin = 0.0, tmax = 1e30;
    const double o[3] = {ox, oy, oz};
    const double d[3] = {fx, fy, fz};
    const double lo[3] = {b.minX, b.minY, b.minZ};
    const double hi[3] = {b.maxX, b.maxY, b.maxZ};
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(d[i]) < 1e-12) {
            if (o[i] < lo[i] || o[i] > hi[i]) return false;
            continue;
        }
        double t1 = (lo[i] - o[i]) / d[i];
        double t2 = (hi[i] - o[i]) / d[i];
        if (t1 > t2) std::swap(t1, t2);
        if (t1 > tmin) tmin = t1;
        if (t2 < tmax) tmax = t2;
        if (tmin > tmax) return false;
    }
    if (tmin < 0.0) tmin = 0.0;   // 相机在盒内时视为命中
    tNear = tmin;
    return tmax >= tmin;
}

// 离射线最近的 AABB 表面点：先把射线上离盒中心最近的点夹进盒子。
static void closest_point_on_aabb_to_ray(double ox, double oy, double oz,
                                         double fx, double fy, double fz,
                                         const Aabb& b,
                                         double& qx, double& qy, double& qz) {
    const double cx = (b.minX + b.maxX) * 0.5;
    const double cy = (b.minY + b.maxY) * 0.5;
    const double cz = (b.minZ + b.maxZ) * 0.5;
    double t = (cx - ox) * fx + (cy - oy) * fy + (cz - oz) * fz;
    if (t < 0.0) t = 0.0;
    qx = ox + fx * t;
    qy = oy + fy * t;
    qz = oz + fz * t;
    qx = std::max(b.minX, std::min(b.maxX, qx));
    qy = std::max(b.minY, std::min(b.maxY, qy));
    qz = std::max(b.minZ, std::min(b.maxZ, qz));
}

// ------------------------------------------------------------
// 目标选择（游戏渲染线程调用）
// ------------------------------------------------------------
namespace {
struct AimCandidate {
    int     idx = -1;
    double  score = 1e30;
    double  tie = 1e30;
    bool    locked = false;
    // 第一目标：最近点（未命中时）或射线进盒点（命中时）
    float   firstSx = 0.f, firstSy = 0.f;
    double  firstWx = 0, firstWy = 0, firstWz = 0;
    // 第二目标：命中碰撞箱后的落点（按 secondTarget 模式计算）
    double  secondWx = 0, secondWy = 0, secondWz = 0;
};

int    g_lastAimId = -1;
DWORD  g_lastAimSwitchMs = 0;
int    g_transitionEntity = -1;   // 正在做第一→第二目标过渡的实体
DWORD  g_transitionStartMs = 0;
}

void aimbot_update_target(const CamData& cam, int screenW, int screenH,
                          const std::vector<EntityData>& entities,
                          const AimSettings& cfg) {
    if (!cam.ok || screenW <= 0 || screenH <= 0) { aimbot_clear_target(); return; }

    float camFov = cam.fov;
    if (!(camFov > 10.f && camFov < 160.f)) camFov = 70.f;

    CamBasis cb;
    if (!cam_basis(cam.yaw, cam.pitch, camFov, screenW, screenH, cb)) {
        aimbot_clear_target();
        return;
    }

    const float cx = (float)screenW * 0.5f;
    const float cy = (float)screenH * 0.5f;
    const float radiusPx = cfg.fov * 0.5f / camFov * (float)screenH;
    const float radiusPxSq = radiusPx * radiusPx;

    std::vector<AimCandidate> cands;
    cands.reserve(entities.size());

    for (int i = 0; i < (int)entities.size(); ++i) {
        const EntityData& e = entities[i];
        // 弹射物/掉落物不参与自瞄（其他实体可配置，但弹射物永远排除）。
        if (e.projType != PROJ_NONE) continue;
        bool wantsPlayer = e.isPlayer && cfg.aimPlayers;
        bool wantsMob = e.isLiving && !e.isPlayer && cfg.aimMobs;
        bool wantsOther = !e.isLiving && cfg.aimOthers;
        if (!wantsPlayer && !wantsMob && !wantsOther) continue;
        if (e.isLiving && e.healthValid && e.health <= 0.001f) continue;

        // 预判：用未平滑的原始插值位置 + tick 速度外推（0=不预判）。
        double px = e.rx, py = e.ry, pz = e.rz;
        if (cfg.predictionTicks > 0 && e.hasVelocity) {
            px += e.vx * (double)cfg.predictionTicks;
            py += e.vy * (double)cfg.predictionTicks;
            pz += e.vz * (double)cfg.predictionTicks;
        }
        const double ddx = px - cam.px, ddy = py - cam.py, ddz = pz - cam.pz;
        const double dist = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
        if (dist > cfg.maxDistance) continue;

        const double half = (double)e.bbw * 0.5;
        Aabb box;
        box.minX = px - half; box.maxX = px + half;
        box.minY = py;        box.maxY = py + (double)e.bbh;
        box.minZ = pz - half; box.maxZ = pz + half;

        double tHit = 0.0;
        const bool hit = ray_aabb(cam.px, cam.py, cam.pz, cb.fx, cb.fy, cb.fz, box, tHit);

        AimCandidate c;
        c.idx = i;
        c.locked = hit;

        if (hit) {
            // 第一目标 = 准星射线进入碰撞箱的表面点。
            c.firstWx = cam.px + cb.fx * tHit;
            c.firstWy = cam.py + cb.fy * tHit;
            c.firstWz = cam.pz + cb.fz * tHit;
            c.firstWy = std::max(box.minY, std::min(box.maxY, c.firstWy));
        } else {
            // 第一目标 = 离准星射线最近的碰撞箱表面点。
            closest_point_on_aabb_to_ray(cam.px, cam.py, cam.pz,
                                         cb.fx, cb.fy, cb.fz, box,
                                         c.firstWx, c.firstWy, c.firstWz);
        }
        if (!cam_project(cb, cam.px, cam.py, cam.pz,
                         c.firstWx, c.firstWy, c.firstWz,
                         c.firstSx, c.firstSy))
            continue;

        // 第二目标：命中碰撞箱后的落点（可选择放平/不放平/保持最近点）。
        if (hit) {
            c.secondWx = px;
            c.secondWz = pz;
            switch (cfg.secondTarget) {
            case AIM_SECOND_FIRST_HEIGHT:
                c.secondWy = std::max(box.minY, std::min(box.maxY, c.firstWy));
                break;
            case AIM_SECOND_KEEP_NEAREST:
                c.secondWx = c.firstWx;
                c.secondWy = c.firstWy;
                c.secondWz = c.firstWz;
                break;
            case AIM_SECOND_LEVEL:
            default:
                c.secondWy = std::max(box.minY, std::min(box.maxY, cam.py));
                break;
            }
        } else {
            c.secondWx = c.firstWx;
            c.secondWy = c.firstWy;
            c.secondWz = c.firstWz;
        }

        const float dpx = c.firstSx - cx;
        const float dpy = c.firstSy - cy;
        const float centerDistSq = dpx * dpx + dpy * dpy;
        if (centerDistSq > radiusPxSq) continue;   // 超出自瞄 FOV
        const float centerDist = std::sqrt(centerDistSq);

        switch (cfg.priority) {
        case AIM_PRIORITY_DISTANCE:  c.score = dist; break;
        case AIM_PRIORITY_HEALTH:
            c.score = e.healthValid ? (double)e.health / (double)std::max(1.0f, e.maxHealth)
                                    : 1.0;
            break;
        case AIM_PRIORITY_CROSSHAIR:
        default:                     c.score = centerDist; break;
        }
        c.tie = centerDist;
        cands.push_back(c);
    }

    if (cands.empty()) {
        publish_no_target(radiusPx);
        return;
    }

    std::sort(cands.begin(), cands.end(), [](const AimCandidate& a, const AimCandidate& b) {
        if (a.score != b.score) return a.score < b.score;
        return a.tie < b.tie;
    });

    // 目标保持：优先继续跟当前目标，避免多目标贴近时来回横跳。
    int chosen = 0;
    int keepIdx = -1;
    for (int i = 0; i < (int)cands.size(); ++i)
        if (entities[cands[i].idx].id == g_lastAimId) { keepIdx = i; break; }
    if (keepIdx >= 0) {
        double tol = 0.0;
        switch (cfg.priority) {
        case AIM_PRIORITY_DISTANCE: tol = std::max(2.0, cands[0].score * 0.20); break;
        case AIM_PRIORITY_HEALTH:   tol = std::max(0.08, cands[0].score * 0.30); break;
        default:                    tol = std::max(20.0, cands[0].score * 0.25); break;
        }
        DWORD now = GetTickCount();
        bool inCooldown = (int)(now - g_lastAimSwitchMs) < cfg.switchCooldownMs;
        if (inCooldown || cands[keepIdx].score <= cands[0].score + tol)
            chosen = keepIdx;
    }

    // 视线检测：优先已选目标；被墙挡住时按排序依次换下一个可见目标。
    std::vector<int> order;
    order.reserve(cands.size());
    order.push_back(chosen);
    for (int i = 0; i < (int)cands.size(); ++i)
        if (i != chosen) order.push_back(i);

    const AimCandidate* picked = nullptr;
    int checked = 0;
    for (int oi : order) {
        if (checked++ >= 6) break;   // 每帧最多做 6 次射线检测，避免墙后群怪时 JNI 尖峰
        const AimCandidate& c = cands[oi];
        if (!cfg.visibleOnly) { picked = &c; break; }
        double hx = 0, hy = 0, hz = 0;
        bool hitBlock = false;
        const bool clipOk = jvm_clip_block(cam.px, cam.py, cam.pz,
                                           c.secondWx, c.secondWy, c.secondWz,
                                           hx, hy, hz, hitBlock);
        if (!clipOk || !hitBlock) { picked = &c; break; }
    }

    if (!picked) {
        publish_no_target(radiusPx);
        return;
    }

    const EntityData& ent = entities[picked->idx];
    const int eid = ent.id;
    const DWORD now = GetTickCount();
    if (eid != g_lastAimId) {
        g_lastAimId = eid;
        g_lastAimSwitchMs = now;
    }

    // 第一目标 → 第二目标过渡：
    //   - 未命中碰撞箱：直接瞄第一目标（最近点）。
    //   - 刚命中碰撞箱：从进盒点按 secondSmooth 平滑过渡到第二目标，
    //     避免锁到碰撞箱的瞬间突然跳到中心。
    double tx = picked->firstWx, ty = picked->firstWy, tz = picked->firstWz;
    if (picked->locked) {
        if (g_transitionEntity != eid) {
            g_transitionEntity = eid;
            g_transitionStartMs = now;
        }
        const float durMs = 80.0f + (float)(cfg.secondSmooth - 1) * 80.0f;
        float k = (float)(now - g_transitionStartMs) / durMs;
        if (k < 0.0f) k = 0.0f;
        if (k > 1.0f) k = 1.0f;
        k = k * k * (3.0f - 2.0f * k);   // smoothstep：起止都柔和
        tx += (picked->secondWx - picked->firstWx) * (double)k;
        ty += (picked->secondWy - picked->firstWy) * (double)k;
        tz += (picked->secondWz - picked->firstWz) * (double)k;
    } else {
        g_transitionEntity = -1;
        g_transitionStartMs = 0;
    }

    float outSx = 0.f, outSy = 0.f;
    if (!cam_project(cb, cam.px, cam.py, cam.pz, tx, ty, tz, outSx, outSy) &&
        !cam_project(cb, cam.px, cam.py, cam.pz,
                     picked->secondWx, picked->secondWy, picked->secondWz,
                     outSx, outSy)) {
        publish_no_target(radiusPx);
        return;
    }

    AimTarget t;
    t.valid = true;
    t.entityId = eid;
    t.sx = outSx;
    t.sy = outSy;
    t.locked = picked->locked;
    t.secondProgress = picked->locked ? ((cfg.secondTarget == AIM_SECOND_KEEP_NEAREST)
                                         ? 1.0f : 0.0f) : 0.0f;
    if (picked->locked && cfg.secondTarget != AIM_SECOND_KEEP_NEAREST) {
        const float durMs = 80.0f + (float)(cfg.secondSmooth - 1) * 80.0f;
        float p = (float)(now - g_transitionStartMs) / durMs;
        if (p < 0.0f) p = 0.0f;
        if (p > 1.0f) p = 1.0f;
        p = p * p * (3.0f - 2.0f * p);
        t.secondProgress = p;
    }
    t.fovRadiusPx = radiusPx;
    t.screenW = screenW;
    t.screenH = screenH;
    t.frameMs = now;
    t.name = ent.name;
    t.health = ent.health;
    t.maxHealth = ent.maxHealth;
    t.healthValid = ent.healthValid;
    publish_target(t);
}

// ------------------------------------------------------------
// 人类化鼠标移动线程
// ------------------------------------------------------------
static bool key_down(int vk) {
    return vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static bool game_focused(HWND gameHwnd) {
    if (!gameHwnd) return false;
    HWND fg = GetForegroundWindow();
    if (fg == gameHwnd) return true;
    if (fg) {
        DWORD fgPid = 0, gamePid = 0;
        GetWindowThreadProcessId(fg, &fgPid);
        GetWindowThreadProcessId(gameHwnd, &gamePid);
        if (fgPid == gamePid && gamePid != 0) return true;   // 同进程其它窗口兜底
    } else if (IsWindowVisible(gameHwnd) && !IsIconic(gameHwnd)) {
        return true;   // 独占全屏等场景 GetForegroundWindow 可能为 NULL
    }
    return false;
}

static uint32_t g_rng = 0x9E3779B9u ^ 0x1234567u;
static float noise01() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return (float)(g_rng & 0xFFFFFF) / 16777216.0f;
}

struct AimSession {
    int    entityId = -1;
    DWORD  startMs = 0;
    DWORD  pauseUntil = 0;
    float  lastDx = 0.f, lastDy = 0.f;
    bool   started = false;
    // 稳定度：第一次对齐第二目标后记录锚点，目标只移动一点点时不再动鼠标。
    bool   stableSet = false;
    float  stableSx = 0.f, stableSy = 0.f;
};

static void aim_session_reset(AimSession& s) {
    s = AimSession();
}

static void process_aim_mouse(const AimSettings& st, const AimTarget& t, AimSession& s) {
    const DWORD now = GetTickCount();
    if (!t.valid || (int)(now - t.frameMs) > 150) {   // 目标丢失/过期为立刻停
        aim_session_reset(s);
        return;
    }

    const float dx = t.sx - (float)t.screenW * 0.5f;
    const float dy = t.sy - (float)t.screenH * 0.5f;
    const float dist = std::sqrt(dx * dx + dy * dy);

    if (s.started && s.entityId != t.entityId) {
        const float jump = std::sqrt((dx - s.lastDx) * (dx - s.lastDx) +
                                     (dy - s.lastDy) * (dy - s.lastDy));
        s.entityId = t.entityId;
        s.stableSet = false;   // 换目标后重新建立稳定锚点
        if (jump > 90.0f) {   // 新目标画面位置突变：重新开始反应/加速过程
            s.startMs = now;
        }
    }
    if (!s.started) {
        s.started = true;
        s.entityId = t.entityId;
        s.startMs = now;
    }
    s.lastDx = dx;
    s.lastDy = dy;

    // 稳定度：仅当已经完成第一→第二目标过渡（secondProgress≈1）时生效。
    if (t.locked && t.secondProgress >= 1.0f) {
        if (!s.stableSet) {
            if (dist <= 1.5f) {   // 已真正对齐第二目标：记录稳定锚点
                s.stableSet = true;
                s.stableSx = t.sx;
                s.stableSy = t.sy;
                return;
            }
        } else {
            const float mdx = t.sx - s.stableSx;
            const float mdy = t.sy - s.stableSy;
            const float moved = std::sqrt(mdx * mdx + mdy * mdy);
            if (moved <= (float)st.stability) {
                return;   // 目标只变了一点点：保持不动
            }
            // 变化超过死区：解除稳定，正常追一次，追上后重新落锚。
            s.stableSet = false;
        }
    } else {
        s.stableSet = false;
    }

    if (s.pauseUntil > now) return;

    // 反应延迟：新目标锁定后先停顿一小段时间，更像人发现目标。
    const int reaction = st.reactionMs + (int)(noise01() * 30.0f) - 15;
    const int elapsed = (int)(now - s.startMs);
    if (elapsed < reaction) return;

    if (dist <= 1.2f) return;   // 小死区：避免在目标点上像素级抖动

    // 偶发 20-50ms 微停顿（约 0.2% 概率/帧），打散机械节奏。
    if ((g_rng % 1000u) < 2u) {
        s.pauseUntil = now + 20u + (g_rng % 30u);
        return;
    }

    const float smoothNorm = (float)(st.smooth - 1) / 9.0f;
    float step = 0.20f - 0.165f * smoothNorm;   // 平滑1≈20%/tick，平滑10≈3.5%/tick
    step *= st.mouseSensitivity;

    // 启动加速：前 150ms 从 45% 逐渐到 100%
    if (elapsed < 150) {
        step *= 0.45f + 0.55f * ((float)elapsed / 150.0f);
    }
    // 接近减速：最后 42px 缓入目标点
    const float decelPx = 42.0f;
    if (dist < decelPx) {
        step *= 0.18f + 0.82f * (dist / decelPx);
    }
    // 轻微噪声：移动幅度 ±8%，再加一点垂直抖动
    step *= 1.0f + (noise01() - 0.5f) * 0.16f;
    float mx = dx * step + (noise01() - 0.5f) * 0.7f;
    float my = dy * step + (noise01() - 0.5f) * 0.7f;

    const float moveLen = std::sqrt(mx * mx + my * my);
    if (moveLen > dist) {   // 不越过目标点，保留自然过冲余量
        mx *= dist / moveLen;
        my *= dist / moveLen;
    }

    LONG ix = (LONG)std::llround((double)mx);
    LONG iy = (LONG)std::llround((double)my);
    if (ix == 0 && std::fabs((double)mx) >= 0.5) ix = (mx > 0.f) ? 1 : -1;
    if (iy == 0 && std::fabs((double)my) >= 0.5) iy = (my > 0.f) ? 1 : -1;
    if (ix == 0 && iy == 0) {
        if (dist > 3.0f) { ix = (dx > 0.f) ? 1 : -1; iy = (dy > 0.f) ? 1 : -1; }
        else return;
    }

    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dx = ix;
    in.mi.dy = iy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(in));
}

static DWORD WINAPI aim_thread_entry(LPVOID) {
    timeBeginPeriod(1);

    const auto period = std::chrono::milliseconds(4);   // 250Hz
    auto next = std::chrono::steady_clock::now();
    AimSession session;

    while (!g_stop.load(std::memory_order_acquire)) {
        next += period;
        auto nowTick = std::chrono::steady_clock::now();
        if (next <= nowTick) next = nowTick + period;

        AimSettings st;
        {
            std::lock_guard<std::mutex> lk(g_settingsMutex);
            st = g_settings;
        }
        const bool menuOpen = g_menuOpen.load(std::memory_order_acquire);
        bool trigger = false;
        switch (st.triggerMode) {
        case AIM_TRIGGER_HOLD_LMB: trigger = key_down(VK_LBUTTON); break;
        case AIM_TRIGGER_HOLD_RMB: trigger = key_down(VK_RBUTTON); break;
        case AIM_TRIGGER_HOLD_KEY: trigger = g_hotkeyDown.load(std::memory_order_acquire); break;
        case AIM_TRIGGER_TOGGLE:   trigger = g_toggleOn.load(std::memory_order_acquire); break;
        case AIM_TRIGGER_ALWAYS:   trigger = true; break;
        default: trigger = false; break;
        }

        const bool allowed = st.enabled && trigger && !menuOpen &&
                             game_focused(g_gameHwnd);
        {
            static bool lastAllowed = false;
            if (allowed != lastAllowed) {
                lastAllowed = allowed;
                esp_log("[aim] 自瞄激活状态 -> %s (mode=%d keyDown=%d)",
                        allowed ? "开" : "关", st.triggerMode,
                        g_hotkeyDown.load(std::memory_order_acquire) ? 1 : 0);
            }
        }
        g_active.store(allowed, std::memory_order_release);

        if (allowed) {
            AimTarget t;
            aimbot_get_target(t);
            process_aim_mouse(st, t, session);
        } else {
            aim_session_reset(session);
        }

        std::this_thread::sleep_until(next);
    }

    g_active.store(false, std::memory_order_release);
    timeEndPeriod(1);
    return 0;
}

// ------------------------------------------------------------
// 生命周期
// ------------------------------------------------------------
void aimbot_start(HWND gameHwnd) {
    if (g_threadRunning.exchange(true)) return;
    g_gameHwnd = gameHwnd;
    g_stop.store(false, std::memory_order_release);
    g_active.store(false, std::memory_order_release);
    g_toggleOn.store(false, std::memory_order_release);
    aimbot_clear_target();
    g_threadHandle = spawn_hidden_thread(aim_thread_entry);
    if (!g_threadHandle) {
        g_threadRunning.store(false, std::memory_order_release);
        esp_log("[aim] 自瞄线程创建失败");
        return;
    }
    esp_log("[aim] 自瞄线程已启动（250Hz，人类化移动）");
}

void aimbot_stop() {
    g_stop.store(true, std::memory_order_release);
    if (g_threadHandle) {
        WaitForSingleObject(g_threadHandle, 3000);
        CloseHandle(g_threadHandle);
        g_threadHandle = nullptr;
    }
    g_threadRunning.store(false, std::memory_order_release);
    g_active.store(false, std::memory_order_release);
    aimbot_clear_target();
    esp_log("[aim] 自瞄线程已停止");
}
