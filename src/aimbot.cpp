// ============================================================
//  aimbot.cpp — 角度域磁吸自瞄
//
// 设计要点：
//   1) 游戏渲染线程负责实体筛选、遮挡检测与角度误差发布；移动线程不碰 JNI。
//   2) FOV、目标排序和锁定都在角度域完成，不随分辨率或画面 FOV 漂移。
//   3) 读取 Minecraft 真实鼠标灵敏度，把角度误差换算为原生鼠标计数。
//   4) 250Hz 控制器按实际时间步进，有加减速、亚像素累加、死区滞回和手动让权。
// ============================================================
#include "aimbot.h"
#include "math3d.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <mmsystem.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

// ------------------------------------------------------------
// 跨线程运行状态
// ------------------------------------------------------------
std::mutex        g_settingsMutex;
AimSettings       g_settings;
HWND              g_gameHwnd = nullptr;
std::atomic<bool> g_threadRunning{false};
std::atomic<bool> g_stop{false};
std::atomic<bool> g_menuOpen{false};
std::atomic<bool> g_active{false};
std::atomic<bool> g_toggleOn{false};
std::atomic<bool> g_hotkeyDown{false};
std::atomic<bool> g_hasTarget{false};
std::atomic<bool> g_targetLocked{false};
std::atomic<int>  g_visualMode{0};
HANDLE            g_threadHandle = nullptr;

std::mutex            g_targetMutex;
AimTarget             g_target;
std::atomic<uint32_t> g_publishGen{1};

float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

float wrap_degrees(float v) {
    while (v > 180.0f) v -= 360.0f;
    while (v < -180.0f) v += 360.0f;
    return v;
}

DWORD elapsed_ms(DWORD now, DWORD then) {
    return (DWORD)(now - then);
}

void clamp_settings(AimSettings& a) {
    if (a.triggerMode < 0 || a.triggerMode >= AIM_TRIGGER_COUNT)
        a.triggerMode = AIM_TRIGGER_HOLD_LMB;
    if (a.priority < AIM_PRIORITY_CROSSHAIR || a.priority > AIM_PRIORITY_HEALTH)
        a.priority = AIM_PRIORITY_CROSSHAIR;
    a.fov = clampf(a.fov, 10.0f, 180.0f);
    a.maxDistance = clampd(a.maxDistance, 5.0, 200.0);
    a.smooth = std::max(1, std::min(10, a.smooth));
    a.reactionMs = std::max(0, std::min(300, a.reactionMs));
    a.mouseSensitivity = clampf(a.mouseSensitivity, 0.5f, 2.0f);
    a.predictionTicks = std::max(0, std::min(20, a.predictionTicks));
    a.switchCooldownMs = std::max(0, std::min(2000, a.switchCooldownMs));
    if (a.secondTarget < 0 || a.secondTarget >= AIM_SECOND_COUNT)
        a.secondTarget = AIM_SECOND_CUSTOM_HEIGHT;
    a.secondSmooth = std::max(1, std::min(10, a.secondSmooth));
    a.stability = std::max(0, std::min(30, a.stability));
    a.visualMode = std::max(0, std::min(3, a.visualMode));
    a.assist = std::max(0, std::min(10, a.assist));
    a.aimHeight = std::max(45, std::min(80, a.aimHeight));
}

uint32_t next_generation() {
    return g_publishGen.fetch_add(1, std::memory_order_relaxed) + 1;
}

void publish_target(const AimTarget& t) {
    {
        std::lock_guard<std::mutex> lk(g_targetMutex);
        g_target = t;
    }
    g_hasTarget.store(t.valid, std::memory_order_release);
    g_targetLocked.store(t.valid && t.locked, std::memory_order_release);
}

void publish_no_target(float radiusPx = 0.0f) {
    AimTarget t;
    t.frameMs = GetTickCount();
    t.gen = next_generation();
    t.fovRadiusPx = radiusPx;
    publish_target(t);
}

// ------------------------------------------------------------
// 几何
// ------------------------------------------------------------
struct Aabb {
    double minX, minY, minZ;
    double maxX, maxY, maxZ;
};

bool ray_aabb(double ox, double oy, double oz,
              double fx, double fy, double fz,
              const Aabb& b, double& tNear) {
    double tmin = 0.0;
    double tmax = 1e30;
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
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
        if (tmin > tmax) return false;
    }

    if (tmax < 0.0) return false;
    tNear = std::max(0.0, tmin);
    return true;
}

float angular_distance(const CamBasis& cb,
                       double camX, double camY, double camZ,
                       double wx, double wy, double wz) {
    const double dx = wx - camX;
    const double dy = wy - camY;
    const double dz = wz - camZ;
    const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1e-9) return 0.0f;
    const double dot = clampd((dx * cb.fx + dy * cb.fy + dz * cb.fz) / len, -1.0, 1.0);
    return (float)(std::acos(dot) * 180.0 / M_PI);
}

bool target_angle_error(const CamData& cam,
                        double wx, double wy, double wz,
                        float& yawError, float& pitchError) {
    const double dx = wx - cam.px;
    const double dy = wy - cam.py;
    const double dz = wz - cam.pz;
    const double horizontal = std::sqrt(dx * dx + dz * dz);
    if (horizontal < 1e-9 && std::fabs(dy) < 1e-9) return false;

    const float targetYaw = (float)(std::atan2(-dx, dz) * 180.0 / M_PI);
    const float targetPitch = (float)(-std::atan2(dy, std::max(1e-9, horizontal)) * 180.0 / M_PI);
    yawError = wrap_degrees(targetYaw - cam.yaw);
    pitchError = targetPitch - cam.pitch;
    return std::isfinite(yawError) && std::isfinite(pitchError);
}

float fov_radius_pixels(float fullAimFov, float cameraFov, int screenW, int screenH) {
    if (screenW <= 0 || screenH <= 0) return 0.0f;
    const float cam = clampf(cameraFov, 10.0f, 160.0f);
    const float half = clampf(fullAimFov * 0.5f, 1.0f, 89.0f);
    const double focalPx = (double)screenH * 0.5 /
                           std::tan((double)cam * 0.5 * M_PI / 180.0);
    double radius = focalPx * std::tan((double)half * M_PI / 180.0);
    const double maxRadius = std::hypot((double)screenW, (double)screenH) * 2.0;
    if (!std::isfinite(radius)) radius = maxRadius;
    return (float)std::min(radius, maxRadius);
}

// ------------------------------------------------------------
// 速度时域平滑：位置直接使用游戏的 partialTick 插值值，不再额外制造延迟。
// ------------------------------------------------------------
struct EntMotion {
    double vx = 0.0;
    double vz = 0.0;
    DWORD lastMs = 0;
    bool inited = false;
};

std::unordered_map<int, EntMotion> g_entMotion;
DWORD g_motionPruneMs = 0;

void prune_motion(DWORD now) {
    if (elapsed_ms(now, g_motionPruneMs) < 4000) return;
    g_motionPruneMs = now;
    for (auto it = g_entMotion.begin(); it != g_entMotion.end();) {
        if (elapsed_ms(now, it->second.lastMs) > 1600) it = g_entMotion.erase(it);
        else ++it;
    }
}

void smooth_velocity(int id, DWORD now, bool valid, double vx, double vz,
                     double& outVx, double& outVz) {
    EntMotion& s = g_entMotion[id];
    if (!s.inited || elapsed_ms(now, s.lastMs) > 250) {
        s.vx = valid ? vx : 0.0;
        s.vz = valid ? vz : 0.0;
        s.inited = true;
    } else if (valid) {
        double dt = std::max(0.001, std::min(0.12, elapsed_ms(now, s.lastMs) * 0.001));
        const double alpha = 1.0 - std::exp(-dt / 0.075);
        s.vx += (vx - s.vx) * alpha;
        s.vz += (vz - s.vz) * alpha;
    }
    s.lastMs = now;
    outVx = s.vx;
    outVz = s.vz;
}

// ------------------------------------------------------------
// 目标锁定状态（仅游戏渲染线程访问）
// ------------------------------------------------------------
struct TargetLockState {
    int entityId = -1;
    DWORD switchMs = 0;
    DWORD lastUpdateMs = 0;
    DWORD lastPresentMs = 0;
    DWORD lastInFovMs = 0;
    DWORD lastVisibleMs = 0;
    DWORD lastHitMs = 0;
    DWORD transitionStartMs = 0;
    bool hasEntry = false;
    double entryFx = 0.5;
    double entryFy = 0.62;
    double entryFz = 0.5;
};

TargetLockState g_lock;

void reset_lock_state() {
    g_lock = TargetLockState();
}

void acquire_entity(int entityId, DWORD now) {
    g_lock = TargetLockState();
    g_lock.entityId = entityId;
    g_lock.switchMs = now;
    g_lock.lastUpdateMs = now;
    g_lock.lastPresentMs = now;
    g_lock.lastInFovMs = now;
    // 可见性确认后由调用方写入；不能预设“刚获取就可见”，否则首帧被墙挡住
    // 的目标会凭空得到遮挡宽限。
}

enum VisibilityState {
    VIS_UNKNOWN = 0,
    VIS_CLEAR,
    VIS_BLOCKED
};

struct AimCandidate {
    int idx = -1;
    int entityId = -1;
    double score = 1e30;
    double tie = 1e30;
    double distance = 0.0;
    float angle = 180.0f;
    bool current = false;
    bool inAcquireFov = false;
    bool inReleaseFov = false;
    bool fovGrace = false;
    bool hitNow = false;
    double hitT = 0.0;
    VisibilityState visibility = VIS_UNKNOWN;

    Aabb actualBox{};
    Aabb targetBox{};
    double bodyWx = 0.0, bodyWy = 0.0, bodyWz = 0.0;
    double visWx = 0.0, visWy = 0.0, visWz = 0.0;
};

bool materially_better(const AimCandidate& challenger,
                       const AimCandidate& current,
                       int priority, bool sticky) {
    const double improvement = current.score - challenger.score;
    if (improvement <= 0.0) return false;

    switch (priority) {
    case AIM_PRIORITY_DISTANCE:
        return improvement >= std::max(sticky ? 1.75 : 0.75,
                                       current.score * (sticky ? 0.18 : 0.08));
    case AIM_PRIORITY_HEALTH:
        return improvement >= (sticky ? 0.15 : 0.06);
    case AIM_PRIORITY_CROSSHAIR:
    default:
        return improvement >= std::max(sticky ? 1.8 : 0.65,
                                       current.score * (sticky ? 0.28 : 0.12));
    }
}

void check_visibility(const CamData& cam, AimCandidate& c) {
    double hx = 0.0, hy = 0.0, hz = 0.0;
    bool hitBlock = false;
    const bool clipOk = jvm_clip_block(cam.px, cam.py, cam.pz,
                                       c.visWx, c.visWy, c.visWz,
                                       hx, hy, hz, hitBlock);
    // 符号缺失或 JNI 临时失败时 fail-open，避免功能无声失效。
    c.visibility = (!clipOk || !hitBlock) ? VIS_CLEAR : VIS_BLOCKED;
}

void update_entry_point(const CamData& cam, const CamBasis& cb,
                        const AimCandidate& c, DWORD now) {
    if (c.hitNow) {
        // 锁定状态只看准星是否穿过真实碰撞箱；保存的是碰撞箱内相对坐标，
        // 后续映射到预测后的 targetBox，避免预判开启时二段落点向后跳。
        const bool newEntry = !g_lock.hasEntry || elapsed_ms(now, g_lock.lastHitMs) > 130;
        if (newEntry) {
            const double hitX = cam.px + cb.fx * c.hitT;
            const double hitY = cam.py + cb.fy * c.hitT;
            const double hitZ = cam.pz + cb.fz * c.hitT;
            const double bw = std::max(1e-6, c.actualBox.maxX - c.actualBox.minX);
            const double bh = std::max(1e-6, c.actualBox.maxY - c.actualBox.minY);
            const double bd = std::max(1e-6, c.actualBox.maxZ - c.actualBox.minZ);
            g_lock.entryFx = clampd((hitX - c.actualBox.minX) / bw, 0.0, 1.0);
            g_lock.entryFy = clampd((hitY - c.actualBox.minY) / bh, 0.0, 1.0);
            g_lock.entryFz = clampd((hitZ - c.actualBox.minZ) / bd, 0.0, 1.0);
            g_lock.transitionStartMs = now;
            g_lock.hasEntry = true;
        }
        g_lock.lastHitMs = now;
    } else if (g_lock.hasEntry && elapsed_ms(now, g_lock.lastHitMs) > 110) {
        g_lock.hasEntry = false;
        g_lock.transitionStartMs = 0;
    }
}

} // namespace

// ------------------------------------------------------------
// 公共状态接口
// ------------------------------------------------------------
void aimbot_apply_settings(const AimSettings& s) {
    AimSettings next = s;
    clamp_settings(next);
    {
        std::lock_guard<std::mutex> lk(g_settingsMutex);
        const bool modeChanged = g_settings.triggerMode != next.triggerMode;
        const bool disabled = g_settings.enabled && !next.enabled;
        g_settings = next;
        if (modeChanged || disabled) g_toggleOn.store(false, std::memory_order_release);
    }
    g_visualMode.store(next.visualMode, std::memory_order_release);
}

bool aimbot_active() {
    return g_active.load(std::memory_order_acquire);
}

bool aimbot_has_target() {
    return g_hasTarget.load(std::memory_order_acquire);
}

bool aimbot_target_locked() {
    return g_targetLocked.load(std::memory_order_acquire);
}

bool aimbot_visual_wanted() {
    return g_active.load(std::memory_order_acquire) &&
           g_visualMode.load(std::memory_order_acquire) > 0;
}

void aimbot_set_menu_open(bool open) {
    const bool changed = g_menuOpen.exchange(open, std::memory_order_acq_rel) != open;
    // render_loop 会高频同步该状态；只在真正打开的一刻清目标，避免菜单期间
    // 每个宿主 tick 都推进 generation 并争抢 target mutex。
    if (open && changed) publish_no_target();
}

void aimbot_set_hotkey_down(bool down) {
    g_hotkeyDown.store(down, std::memory_order_release);
}

void aimbot_toggle_trigger() {
    aimbot_set_toggle_on(!g_toggleOn.load(std::memory_order_acquire));
}

void aimbot_set_toggle_on(bool on) {
    const bool old = g_toggleOn.exchange(on, std::memory_order_acq_rel);
    if (old != on) esp_log("[aim] 切换状态 -> %s", on ? "开" : "关");
}

bool aimbot_toggle_on() {
    return g_toggleOn.load(std::memory_order_acquire);
}

void aimbot_clear_target() {
    publish_no_target();
}

bool aimbot_get_target(AimTarget& out) {
    std::lock_guard<std::mutex> lk(g_targetMutex);
    out = g_target;
    return out.valid;
}

// ------------------------------------------------------------
// 目标选择（游戏渲染线程）
// ------------------------------------------------------------
void aimbot_update_target(const CamData& cam, int screenW, int screenH,
                          const std::vector<EntityData>& entities,
                          const AimSettings& cfgInput) {
    if (!cam.ok || screenW <= 0 || screenH <= 0) {
        aimbot_clear_target();
        return;
    }

    AimSettings cfg = cfgInput;
    clamp_settings(cfg);

    float cameraFov = cam.fov;
    if (!(cameraFov > 10.0f && cameraFov < 160.0f)) cameraFov = 70.0f;

    CamBasis cb;
    if (!cam_basis(cam.yaw, cam.pitch, cameraFov, screenW, screenH, cb)) {
        aimbot_clear_target();
        return;
    }

    const DWORD now = GetTickCount();
    // 触发结束 / GUI 打开时 update_target 会停调；再次触发不能沿用旧目标和旧进盒点。
    if (g_lock.entityId >= 0 && g_lock.lastUpdateMs != 0 &&
        elapsed_ms(now, g_lock.lastUpdateMs) > 350)
        reset_lock_state();
    g_lock.lastUpdateMs = now;
    prune_motion(now);

    const float radiusPx = fov_radius_pixels(cfg.fov, cameraFov, screenW, screenH);
    const float acquireHalfDeg = clampf(cfg.fov * 0.5f, 5.0f, 90.0f);
    const float releaseHalfDeg = (acquireHalfDeg >= 89.5f) ? 90.0f :
        std::min(89.5f, std::max(acquireHalfDeg + 2.0f, acquireHalfDeg * 1.18f));
    const double heightFrac = (double)cfg.aimHeight / 100.0;
    constexpr DWORD kFovGraceMs = 150;
    constexpr DWORD kOcclusionGraceMs = 180;

    std::vector<AimCandidate> cands;
    cands.reserve(entities.size());
    bool currentFovExpired = false;

    for (int i = 0; i < (int)entities.size(); ++i) {
        const EntityData& e = entities[i];
        if (e.projType != PROJ_NONE) continue;

        const bool wanted = (e.isPlayer && cfg.aimPlayers) ||
                            (e.isLiving && !e.isPlayer && cfg.aimMobs) ||
                            (!e.isLiving && cfg.aimOthers);
        if (!wanted) continue;
        if (e.isLiving && e.healthValid && e.health <= 0.001f) continue;
        if (!(e.bbw > 0.01f && e.bbh > 0.01f)) continue;

        const double px = e.rx;
        const double py = e.ry;
        const double pz = e.rz;
        const double dx = px - cam.px;
        const double dy = py - cam.py;
        const double dz = pz - cam.pz;
        const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!(distance > 0.01 && distance <= cfg.maxDistance)) continue;

        double svx = 0.0, svz = 0.0;
        const bool needPrediction = cfg.predictionTicks > 0;
        if (needPrediction)
            smooth_velocity(e.id, now, e.hasVelocity, e.vx, e.vz, svx, svz);

        int prediction = cfg.predictionTicks;
        if (distance < 4.0) prediction = 0;
        else if (distance < 8.0)
            prediction = (int)std::lround(prediction * (distance - 4.0) / 4.0);

        // 服务端速度按 blocks/tick；限制最大提前量，防止瞬移/击退尖峰把落点甩出屏幕。
        double leadX = svx * prediction;
        double leadZ = svz * prediction;
        const double leadLen = std::hypot(leadX, leadZ);
        const double maxLead = std::min(6.0, std::max(0.0, distance * 0.35));
        if (leadLen > maxLead && leadLen > 1e-9) {
            leadX *= maxLead / leadLen;
            leadZ *= maxLead / leadLen;
        }
        const double halfW = (double)e.bbw * 0.5;

        AimCandidate c;
        c.idx = i;
        c.entityId = e.id;
        c.current = g_lock.entityId >= 0 && e.id == g_lock.entityId;
        c.distance = distance;
        c.actualBox = {px - halfW, py, pz - halfW,
                       px + halfW, py + (double)e.bbh, pz + halfW};
        c.targetBox = {c.actualBox.minX + leadX, c.actualBox.minY, c.actualBox.minZ + leadZ,
                       c.actualBox.maxX + leadX, c.actualBox.maxY, c.actualBox.maxZ + leadZ};

        c.bodyWx = px + leadX;
        c.bodyWy = clampd(py + (double)e.bbh * heightFrac,
                          c.targetBox.minY, c.targetBox.maxY);
        c.bodyWz = pz + leadZ;
        c.visWx = px;
        c.visWy = clampd(py + (double)e.bbh * heightFrac,
                         c.actualBox.minY, c.actualBox.maxY);
        c.visWz = pz;

        // 获取/排序使用真实碰撞箱中心高度，不使用预测点；否则高 predictionTicks
        // 会把原本在 FOV 内的目标提前移出候选。预测只作用于最终落点。
        c.angle = angular_distance(cb, cam.px, cam.py, cam.pz,
                                   c.visWx, c.visWy, c.visWz);
        c.inAcquireFov = c.angle <= acquireHalfDeg;
        c.inReleaseFov = c.angle <= releaseHalfDeg;

        if (c.current) {
            g_lock.lastPresentMs = now;
            if (c.inReleaseFov) g_lock.lastInFovMs = now;
            c.fovGrace = !c.inReleaseFov &&
                         elapsed_ms(now, g_lock.lastInFovMs) <= kFovGraceMs;
            if (!c.inReleaseFov && !c.fovGrace) currentFovExpired = true;
        }
        if (!c.inAcquireFov && !(c.current && (c.inReleaseFov || c.fovGrace)))
            continue;

        c.hitNow = ray_aabb(cam.px, cam.py, cam.pz,
                            cb.fx, cb.fy, cb.fz, c.actualBox, c.hitT);

        switch (cfg.priority) {
        case AIM_PRIORITY_DISTANCE:
            c.score = distance;
            break;
        case AIM_PRIORITY_HEALTH:
            c.score = e.healthValid
                    ? (double)e.health / std::max(1.0, (double)e.maxHealth)
                    : 1.0;
            break;
        case AIM_PRIORITY_CROSSHAIR:
        default:
            c.score = c.angle;
            break;
        }
        c.tie = c.angle;
        cands.push_back(c);
    }

    if (cands.empty()) {
        if (currentFovExpired ||
            (g_lock.entityId >= 0 && elapsed_ms(now, g_lock.lastPresentMs) > 180))
            reset_lock_state();
        publish_no_target(radiusPx);
        return;
    }

    std::stable_sort(cands.begin(), cands.end(), [](const AimCandidate& a, const AimCandidate& b) {
        if (std::fabs(a.score - b.score) > 1e-9) return a.score < b.score;
        return a.tie < b.tie;
    });

    int currentIdx = -1;
    for (int i = 0; i < (int)cands.size(); ++i) {
        if (cands[i].current) {
            currentIdx = i;
            break;
        }
    }

    // 当前目标优先检测，再按评分检测候选；最多 8 条方块射线/帧。
    // 关闭视线检测时无需建立临时顺序数组，也不做 JNI clip。
    if (!cfg.visibleOnly) {
        for (auto& c : cands) c.visibility = VIS_CLEAR;
    } else {
        std::vector<int> visibilityOrder;
        visibilityOrder.reserve(cands.size());
        if (currentIdx >= 0) visibilityOrder.push_back(currentIdx);
        for (int i = 0; i < (int)cands.size(); ++i)
            if (i != currentIdx) visibilityOrder.push_back(i);

        int checked = 0;
        for (int idx : visibilityOrder) {
            if (checked >= 8) break;
            check_visibility(cam, cands[idx]);
            ++checked;
        }
    }

    if (currentIdx >= 0 && cands[currentIdx].visibility == VIS_CLEAR)
        g_lock.lastVisibleMs = now;

    int bestIdx = -1;
    for (int i = 0; i < (int)cands.size(); ++i) {
        if (!cands[i].inAcquireFov || cands[i].visibility != VIS_CLEAR) continue;
        bestIdx = i;
        break;
    }

    const bool cooldown = g_lock.entityId >= 0 &&
                          elapsed_ms(now, g_lock.switchMs) < (DWORD)cfg.switchCooldownMs;
    int chosenIdx = -1;
    bool occludedGrace = false;
    bool currentOcclusionExpired = false;

    if (currentIdx >= 0) {
        AimCandidate& current = cands[currentIdx];
        const bool blocked = current.visibility == VIS_BLOCKED;
        // 遮挡宽限是锁定连续性的安全缓冲，不应依赖“黏锁”切换偏好；
        // sticky 只影响目标之间何时切换，不影响墙边一帧闪烁。
        const bool blockedGrace = blocked && g_lock.lastVisibleMs != 0 &&
                                  elapsed_ms(now, g_lock.lastVisibleMs) <= kOcclusionGraceMs;
        currentOcclusionExpired = blocked && !blockedGrace;
        const bool currentUsable = (current.inReleaseFov || current.fovGrace) &&
                                   (current.visibility == VIS_CLEAR || blockedGrace);

        if (currentUsable) {
            chosenIdx = currentIdx;
            occludedGrace = blockedGrace;

            if (!blockedGrace && bestIdx >= 0 && bestIdx != currentIdx && !cooldown) {
                const AimCandidate& best = cands[bestIdx];
                if (current.fovGrace ||
                    materially_better(best, current, cfg.priority,
                                      cfg.sticky && current.inAcquireFov)) {
                    chosenIdx = bestIdx;
                    occludedGrace = false;
                }
            }
        }
    }

    if (chosenIdx < 0 && bestIdx >= 0) chosenIdx = bestIdx;
    if (chosenIdx < 0) {
        if (currentFovExpired || currentOcclusionExpired ||
            (g_lock.entityId >= 0 && elapsed_ms(now, g_lock.lastPresentMs) > 180))
            reset_lock_state();
        publish_no_target(radiusPx);
        return;
    }

    if ((currentFovExpired || currentOcclusionExpired) && g_lock.entityId >= 0 &&
        cands[chosenIdx].entityId != g_lock.entityId)
        reset_lock_state();

    AimCandidate& picked = cands[chosenIdx];
    const EntityData& ent = entities[picked.idx];
    const bool switched = picked.entityId != g_lock.entityId;
    if (switched) acquire_entity(picked.entityId, now);

    g_lock.lastPresentMs = now;
    if (picked.inReleaseFov) g_lock.lastInFovMs = now;
    if (picked.visibility == VIS_CLEAR) g_lock.lastVisibleMs = now;

    update_entry_point(cam, cb, picked, now);
    const bool locked = g_lock.hasEntry;

    double targetX = picked.bodyWx;
    double targetY = picked.bodyWy;
    double targetZ = picked.bodyWz;
    float secondProgress = 0.0f;

    if (locked) {
        const double bw = picked.targetBox.maxX - picked.targetBox.minX;
        const double bh = picked.targetBox.maxY - picked.targetBox.minY;
        const double bd = picked.targetBox.maxZ - picked.targetBox.minZ;
        const double entryX = picked.targetBox.minX + bw * g_lock.entryFx;
        const double entryY = picked.targetBox.minY + bh * g_lock.entryFy;
        const double entryZ = picked.targetBox.minZ + bd * g_lock.entryFz;

        double secondX = (picked.targetBox.minX + picked.targetBox.maxX) * 0.5;
        double secondY = picked.bodyWy;
        double secondZ = (picked.targetBox.minZ + picked.targetBox.maxZ) * 0.5;

        switch (cfg.secondTarget) {
        case AIM_SECOND_FIRST_HEIGHT:
            secondY = entryY;
            break;
        case AIM_SECOND_KEEP_NEAREST:
            secondX = entryX;
            secondY = entryY;
            secondZ = entryZ;
            break;
        case AIM_SECOND_LEVEL:
            secondY = clampd(cam.py, picked.targetBox.minY, picked.targetBox.maxY);
            break;
        case AIM_SECOND_CUSTOM_HEIGHT:
        default:
            secondY = picked.bodyWy;
            break;
        }

        if (cfg.secondTarget == AIM_SECOND_KEEP_NEAREST) {
            targetX = entryX;
            targetY = entryY;
            targetZ = entryZ;
            secondProgress = 1.0f;
        } else {
            const float durationMs = 55.0f + (cfg.secondSmooth - 1) * 45.0f;
            float k = durationMs > 0.0f
                    ? (float)elapsed_ms(now, g_lock.transitionStartMs) / durationMs
                    : 1.0f;
            k = clampf(k, 0.0f, 1.0f);
            const float eased = k * k * (3.0f - 2.0f * k);
            targetX = entryX + (secondX - entryX) * eased;
            targetY = entryY + (secondY - entryY) * eased;
            targetZ = entryZ + (secondZ - entryZ) * eased;
            secondProgress = eased;
        }
    }

    float outSx = 0.0f, outSy = 0.0f;
    float yawError = 0.0f, pitchError = 0.0f;
    if (!cam_project(cb, cam.px, cam.py, cam.pz,
                     targetX, targetY, targetZ, outSx, outSy) ||
        !target_angle_error(cam, targetX, targetY, targetZ, yawError, pitchError)) {
        publish_no_target(radiusPx);
        return;
    }

    const bool fovGrace = picked.fovGrace && !picked.inReleaseFov;
    // 只有二段落点已经基本稳定后才降低进盒辅助；过早降权会让二段过渡拖沓。
    float assistMul = (locked && secondProgress >= 0.85f) ? 0.62f : 1.0f;
    if (!picked.inAcquireFov && picked.inReleaseFov) {
        const float span = std::max(0.1f, releaseHalfDeg - acquireHalfDeg);
        const float edge = clampf((picked.angle - acquireHalfDeg) / span, 0.0f, 1.0f);
        assistMul *= 1.0f - edge * 0.75f;
    }
    if (occludedGrace || fovGrace) assistMul = 0.0f;

    AimTarget t;
    t.valid = true;
    t.entityId = picked.entityId;
    t.sx = outSx;
    t.sy = outSy;
    t.yawError = yawError;
    t.pitchError = pitchError;
    t.cameraYaw = cam.yaw;
    t.cameraPitch = cam.pitch;
    t.cameraFov = cameraFov;
    t.gameSensitivity = cam.mouseSensitivity;
    t.sensitivityValid = cam.sensitivityValid;
    t.invertY = cam.invertY;
    t.locked = locked;
    t.occluded = occludedGrace;
    t.fovGrace = fovGrace;
    t.secondProgress = secondProgress;
    t.fovRadiusPx = radiusPx;
    t.screenW = screenW;
    t.screenH = screenH;
    t.frameMs = now;
    t.gen = next_generation();
    t.assistMul = assistMul;
    t.name = ent.name;
    t.health = ent.health;
    t.maxHealth = ent.maxHealth;
    t.healthValid = ent.healthValid;
    publish_target(t);
}

namespace {

// ------------------------------------------------------------
// 250Hz 角度域鼠标控制器
// ------------------------------------------------------------
bool key_down(int vk) {
    return vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool game_focused(HWND gameHwnd) {
    if (!gameHwnd || IsIconic(gameHwnd)) return false;
    HWND fg = GetForegroundWindow();
    if (fg == gameHwnd) return true;
    if (fg) {
        DWORD fgPid = 0, gamePid = 0;
        GetWindowThreadProcessId(fg, &fgPid);
        GetWindowThreadProcessId(gameHwnd, &gamePid);
        return fgPid == gamePid && gamePid != 0;
    }
    return false;
}

bool cursor_hidden() {
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    // 查询失败时 fail-closed：宁可暂停一帧，也不在未知光标状态下移动系统鼠标。
    if (!GetCursorInfo(&ci)) return false;
    return (ci.flags & CURSOR_SHOWING) == 0;
}

float degrees_per_mouse_count(float sensitivity) {
    const float s = clampf(sensitivity, 0.0f, 1.0f);
    const float base = s * 0.6f + 0.2f;
    // MouseHandler.turnPlayer: delta *= (s*0.6+0.2)^3 * 8，Entity.turn 再乘 0.15。
    return base * base * base * 8.0f * 0.15f;
}

float angular_error_to_pixels(float yawError, float pitchError,
                              float cameraFov, int screenH) {
    if (screenH <= 0) return 1e9f;
    const float fov = clampf(cameraFov, 10.0f, 160.0f);
    // 垂直 FOV 投影下，水平归一化中的 aspect 会与 halfW 抵消，
    // 水平和垂直都使用同一个像素焦距 h/(2*tan(fov/2))。
    const double focal = (double)screenH * 0.5 /
                         std::tan((double)fov * 0.5 * M_PI / 180.0);
    const double yaw = clampd(yawError, -80.0, 80.0) * M_PI / 180.0;
    const double pitch = clampd(pitchError, -80.0, 80.0) * M_PI / 180.0;
    return (float)std::hypot(std::tan(yaw) * focal, std::tan(pitch) * focal);
}

struct AimSession {
    int entityId = -1;
    DWORD startMs = 0;
    DWORD handsOffUntil = 0;
    uint32_t lastGen = 0;

    float velocityYaw = 0.0f;       // 度/秒
    float velocityPitch = 0.0f;
    float countFracX = 0.0f;
    float countFracY = 0.0f;
    float spentCountsX = 0.0f;      // 当前发布帧已经发送的计数
    float spentCountsY = 0.0f;
    float sentSinceFrameX = 0.0f;   // 用于从相机变化里剥离自瞄自身输入
    float sentSinceFrameY = 0.0f;

    float previousCameraYaw = 0.0f;
    float previousCameraPitch = 0.0f;
    float previousDegPerCount = 0.15f;
    DWORD previousFrameMs = 0;
    bool hasCameraSample = false;
    bool started = false;
    bool stable = false;
};

void session_stop_motion(AimSession& s) {
    s.velocityYaw = 0.0f;
    s.velocityPitch = 0.0f;
    s.countFracX = 0.0f;
    s.countFracY = 0.0f;
    s.stable = false;
}

void session_soft_idle(AimSession& s) {
    session_stop_motion(s);
    s.spentCountsX = 0.0f;
    s.spentCountsY = 0.0f;
    s.sentSinceFrameX = 0.0f;
    s.sentSinceFrameY = 0.0f;
}

void session_reset(AimSession& s) {
    s = AimSession();
}

void detect_manual_override(const AimTarget& t, AimSession& s,
                            float degPerCount, DWORD now) {
    if (!s.hasCameraSample) {
        s.previousCameraYaw = t.cameraYaw;
        s.previousCameraPitch = t.cameraPitch;
        s.previousDegPerCount = degPerCount;
        s.previousFrameMs = t.frameMs;
        s.hasCameraSample = true;
        return;
    }

    const DWORD frameGap = elapsed_ms(t.frameMs, s.previousFrameMs);
    const float observedYaw = wrap_degrees(t.cameraYaw - s.previousCameraYaw);
    const float observedPitch = t.cameraPitch - s.previousCameraPitch;
    // sentSinceFrameY 保存的是已经按反转选项归一化后的“逻辑 pitch 计数”，
    // 因此无论游戏是否反转 Y 轴，预期相机 pitch 变化都与它同号。
    const float expectedYaw = s.sentSinceFrameX * s.previousDegPerCount;
    const float expectedPitch = s.sentSinceFrameY * s.previousDegPerCount;
    const float manualYaw = observedYaw - expectedYaw;
    const float manualPitch = observedPitch - expectedPitch;
    const float manualLen = std::hypot(manualYaw, manualPitch);
    const float expectedLen = std::hypot(expectedYaw, expectedPitch);
    const float desiredLen = std::hypot(t.yawError, t.pitchError);

    // 卡帧会把多帧鼠标/游戏旋转挤进一次采样，此时差分归因不可靠。
    if (frameGap <= 80 && manualLen > 0.001f && desiredLen > 0.25f) {
        const float dot = (manualYaw * t.yawError + manualPitch * t.pitchError) /
                          (manualLen * desiredLen);
        const float threshold = std::max(2.0f, expectedLen * 1.35f + 0.65f);
        // 只在输入明显背离辅助方向时让权；同方向的大幅甩枪不应被误判为反抗。
        if (manualLen > threshold && (dot < -0.20f || manualLen > 12.0f)) {
            s.handsOffUntil = now + 150;
            s.velocityYaw = 0.0f;
            s.velocityPitch = 0.0f;
            s.stable = false;
        }
    }

    s.previousCameraYaw = t.cameraYaw;
    s.previousCameraPitch = t.cameraPitch;
    s.previousDegPerCount = degPerCount;
    s.previousFrameMs = t.frameMs;
}

void process_aim_mouse(const AimSettings& st, const AimTarget& t,
                       AimSession& s, float dt) {
    const DWORD now = GetTickCount();
    if (!t.valid || elapsed_ms(now, t.frameMs) > 220) {
        if (!t.valid || elapsed_ms(now, t.frameMs) > 500) session_reset(s);
        else session_soft_idle(s);
        return;
    }

    const float actualSensitivity = t.sensitivityValid ? t.gameSensitivity : 0.5f;
    // SendInput 注入的是相对移动计数；在游戏 Raw Input 关闭时 Windows 仍可能
    // 应用指针曲线，因此速度微调保留为机器间手感校准，而非游戏灵敏度替代项。
    const float degPerCount = std::max(0.001f, degrees_per_mouse_count(actualSensitivity));

    if (!s.started || s.entityId != t.entityId) {
        session_reset(s);
        s.started = true;
        s.entityId = t.entityId;
        s.startMs = now;
    }

    if (t.gen != s.lastGen) {
        detect_manual_override(t, s, degPerCount, now);
        s.lastGen = t.gen;
        // 新发布的角度误差已经包含上一帧 SendInput 的结果；只清本帧预算。
        // sentSinceFrame 要在检测后清，供下一次相机采样剥离本帧自瞄输入。
        s.spentCountsX = 0.0f;
        s.spentCountsY = 0.0f;
        s.sentSinceFrameX = 0.0f;
        s.sentSinceFrameY = 0.0f;
    }

    if (t.occluded || t.fovGrace || t.assistMul <= 0.0f) {
        session_stop_motion(s);
        return;
    }

    if ((int)(s.handsOffUntil - now) > 0) {
        session_stop_motion(s);
        return;
    }
    if (st.assist <= 0) {
        session_stop_motion(s);
        return;
    }
    if (elapsed_ms(now, s.startMs) < (DWORD)st.reactionMs) {
        session_stop_motion(s);
        return;
    }

    const float remainingYaw = t.yawError - s.spentCountsX * degPerCount;
    const float remainingPitch = t.pitchError - s.spentCountsY * degPerCount;
    const float remainingPx = angular_error_to_pixels(remainingYaw, remainingPitch,
                                                       t.cameraFov, t.screenH);

    // stability 表示“停手后的释放半径”，而不是首次停手距离：
    // 先收敛到约 1px/半个最小计数，再允许目标在配置半径内轻微移动。
    const float oneCountPx = angular_error_to_pixels(degPerCount, 0.0f,
                                                       t.cameraFov, t.screenH);
    const float enterPx = std::max(1.0f, oneCountPx * 0.55f);
    const float leavePx = std::max(enterPx + 0.75f, (float)st.stability);
    if (s.stable) {
        if (remainingPx <= leavePx) {
            s.velocityYaw = 0.0f;
            s.velocityPitch = 0.0f;
            return;
        }
        s.stable = false;
    } else if (remainingPx <= enterPx) {
        s.stable = true;
        s.velocityYaw = 0.0f;
        s.velocityPitch = 0.0f;
        return;
    }

    const float smoothT = (float)(st.smooth - 1) / 9.0f;
    const float assistT = (float)st.assist / 10.0f;
    const float response = std::pow(std::max(0.0f, assistT), 0.72f) *
                           t.assistMul * st.mouseSensitivity;
    const float settleSeconds = 0.025f + smoothT * 0.125f;

    float desiredYawRate = remainingYaw / settleSeconds * response;
    float desiredPitchRate = remainingPitch / settleSeconds * response;

    const float maxBaseRate = 960.0f + (260.0f - 960.0f) * smoothT;
    const float maxYawRate = maxBaseRate * (0.55f + 0.09f * st.assist) *
                             st.mouseSensitivity;
    const float maxPitchRate = maxYawRate * 0.82f;
    desiredYawRate = clampf(desiredYawRate, -maxYawRate, maxYawRate);
    desiredPitchRate = clampf(desiredPitchRate, -maxPitchRate, maxPitchRate);

    const float accelerationSeconds = 0.010f + smoothT * 0.030f;
    const float velocityAlpha = 1.0f - std::exp(-dt / accelerationSeconds);
    s.velocityYaw += (desiredYawRate - s.velocityYaw) * velocityAlpha;
    s.velocityPitch += (desiredPitchRate - s.velocityPitch) * velocityAlpha;

    float stepYaw = s.velocityYaw * dt;
    float stepPitch = s.velocityPitch * dt;
    if (std::fabs(stepYaw) > std::fabs(remainingYaw)) stepYaw = remainingYaw;
    if (std::fabs(stepPitch) > std::fabs(remainingPitch)) stepPitch = remainingPitch;

    s.countFracX += stepYaw / degPerCount;
    s.countFracY += stepPitch / degPerCount;
    LONG moveX = (LONG)std::trunc(s.countFracX);
    LONG moveY = (LONG)std::trunc(s.countFracY);
    s.countFracX -= (float)moveX;
    s.countFracY -= (float)moveY;
    if (moveX == 0 && moveY == 0) return;

    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = moveX;
    // 默认正 dy 让视角向下；游戏启用“反转 Y 轴”时同步反转发送方向。
    input.mi.dy = t.invertY ? -moveY : moveY;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    if (SendInput(1, &input, sizeof(input)) != 1) {
        s.countFracX += (float)moveX;
        s.countFracY += (float)moveY;
        return;
    }

    s.spentCountsX += (float)moveX;
    s.spentCountsY += (float)moveY;
    s.sentSinceFrameX += (float)moveX;
    s.sentSinceFrameY += (float)moveY;
}

DWORD WINAPI aim_thread_entry(LPVOID) {
    timeBeginPeriod(1);

    constexpr auto period = std::chrono::milliseconds(4);
    auto next = std::chrono::steady_clock::now();
    auto previous = next;
    AimSession session;

    while (!g_stop.load(std::memory_order_acquire)) {
        next += period;
        const auto nowSteady = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(nowSteady - previous).count();
        previous = nowSteady;
        dt = clampf(dt, 0.001f, 0.020f);
        if (next <= nowSteady) next = nowSteady + period;

        AimSettings st;
        {
            std::lock_guard<std::mutex> lk(g_settingsMutex);
            st = g_settings;
        }

        bool trigger = false;
        switch (st.triggerMode) {
        case AIM_TRIGGER_HOLD_LMB: trigger = key_down(VK_LBUTTON); break;
        case AIM_TRIGGER_HOLD_RMB: trigger = key_down(VK_RBUTTON); break;
        case AIM_TRIGGER_HOLD_KEY: trigger = g_hotkeyDown.load(std::memory_order_acquire); break;
        case AIM_TRIGGER_TOGGLE:   trigger = g_toggleOn.load(std::memory_order_acquire); break;
        case AIM_TRIGGER_ALWAYS:   trigger = true; break;
        default: break;
        }

        const bool allowed = st.enabled && trigger &&
                             !g_menuOpen.load(std::memory_order_acquire) &&
                             game_focused(g_gameHwnd) && cursor_hidden();
        g_active.store(allowed, std::memory_order_release);

        if (allowed) {
            AimTarget target;
            aimbot_get_target(target);
            process_aim_mouse(st, target, session, dt);
        } else {
            // 禁止状态下始终清控制器，避免旧亚计数、稳定标记或 reaction 会话
            // 泄漏到下一次触发；目标锁本身由 update_target 的 350ms 空窗规则管理。
            session_reset(session);
        }

        std::this_thread::sleep_until(next);
    }

    g_active.store(false, std::memory_order_release);
    timeEndPeriod(1);
    return 0;
}

} // namespace

void aimbot_start(HWND gameHwnd) {
    if (g_threadRunning.exchange(true)) return;
    g_gameHwnd = gameHwnd;
    g_stop.store(false, std::memory_order_release);
    g_active.store(false, std::memory_order_release);
    g_toggleOn.store(false, std::memory_order_release);
    g_hotkeyDown.store(false, std::memory_order_release);
    reset_lock_state();
    aimbot_clear_target();

    g_threadHandle = spawn_hidden_thread(aim_thread_entry);
    if (!g_threadHandle) {
        g_threadRunning.store(false, std::memory_order_release);
        esp_log("[aim] 自瞄线程创建失败");
        return;
    }
    esp_log("[aim] 自瞄线程已启动（250Hz，角度域控制）");
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
    g_entMotion.clear();
    reset_lock_state();
    esp_log("[aim] 自瞄线程已停止");
}
