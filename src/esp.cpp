// ============================================================
//  esp.cpp — ESP + 内置连点器主循环
//   - 采集：在 SwapBuffers 钩子（游戏渲染线程）内用 GetEnv 复用 JNIEnv，
//     不新建线程、不 AttachCurrentThread，规避在线反作弊检测
//   - 连点器：独立伪装线程 PostMessage 到游戏窗口；Insert 呼出 GDI 菜单
//   - 自瞄已移除；共享内存 / 外部消息传递已移除
// ============================================================
#include "common.h"
#include "math3d.h"
#include "jvm.h"
#include "overlay.h"
#include "glrender.h"
#include "clicker.h"

#include <thread>
#include <chrono>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <cmath>
#include <cstdio>
#include <mmsystem.h>  // timeBeginPeriod / timeEndPeriod

static EspConfig  g_cfg;
static Overlay    g_overlay;
static std::mutex g_clickerCfgMutex;   // 菜单线程与连点热键回调共同写 clicker 配置
static std::atomic<bool> g_running{false};
static std::atomic<bool> g_espEnabled{true};
// ---- Insert 菜单（渲染线程写，游戏线程画）----
static std::atomic<bool> g_menuVisible{false};
static std::atomic<int>  g_menuCursor{0};
static std::atomic<bool> g_menuCaptureKey{false};
static std::atomic<int>  g_menuCaptureTarget{0};
// 强制停止标记：dll_unload 置位，用于在“等待游戏窗口”等尚未进入主循环的阶段
// 也能让线程尽快退出，避免卸载时在已释放内存上继续运行导致崩溃。
static std::atomic<bool> g_stop{false};

// ---- 共享数据（SwapBuffers 钩子在游戏渲染线程写，渲染线程读） ----
static std::mutex               g_dataMutex;
static uint64_t                 g_dataSeq = 0;   // 每发布一帧实体快照 +1（渲染线程去重）
static CamData                  g_cam;
static std::vector<EntityData>  g_entities;
// 游戏线程预投影好的屏幕坐标实体（本线程直接绘制，与游戏画面同帧同步）
static std::vector<ScreenBox>   g_boxes;
// 弹射物预测轨迹屏幕折线（游戏线程预投影，渲染线程直接画）
static std::vector<TrajectoryData> g_trajectories;
static HWND                     g_gameHwnd = nullptr;   // 游戏窗口（投影用屏幕尺寸）
// 覆盖层对象访问互斥：游戏渲染线程（SwapBuffers 钩子内直接绘制/呈现）与
// 渲染线程（定位/显示/隐藏窗口）并发访问同一个 Overlay，必须互斥，避免
// resize 重建 DIB 时游戏线程画到悬空 m_pixels。
static std::mutex               g_overlayLock;
// 覆盖层当前是否已显示并对齐（渲染线程维护，游戏线程据此决定是否绘制）。
static std::atomic<bool>        g_overlayVisible{false};

// ---- 帧间轻微平滑 ----
// 实体世界位置在游戏内以 20Hz tick 更新，虽经 partialTick 插值连续，但在 tick
// 边界速度会突变（折角），投影后表现为轻微跳变。这里对每个实体的世界位置做
// 帧率无关的指数平滑（时间常数 smoothMs），抹平 tick 边界折角，框更丝滑。
// 仅游戏渲染线程（esp_on_swap）访问，无需加锁。
struct SmoothState { double x, y, z; std::chrono::steady_clock::time_point t; bool init = false; };
static std::unordered_map<int, SmoothState> g_smooth;
static DWORD g_smoothPruneTick = 0;

static void smooth_entities(std::vector<EntityData>& ents, double tauMs) {
    if (tauMs < 0.5) tauMs = 0.5;   // 防御：避免除零/极大
    const double tau = tauMs / 1000.0;
    auto now = std::chrono::steady_clock::now();
    for (auto& e : ents) {
        auto& s = g_smooth[e.id];
        if (!s.init) { s = {e.ix, e.iy, e.iz, now, true}; continue; }
        double dt = std::chrono::duration<double>(now - s.t).count();
        if (dt < 0.0) dt = 0.0;
        double k = 1.0 - std::exp(-dt / tau);
        if (k > 1.0) k = 1.0; else if (k < 0.0) k = 0.0;   // 夹到 [0,1]
        s.x += (e.ix - s.x) * k;
        s.y += (e.iy - s.y) * k;
        s.z += (e.iz - s.z) * k;
        s.t = now;
        e.ix = s.x; e.iy = s.y; e.iz = s.z;
    }
    // 周期清理消失实体的平滑状态，避免内存无限增长
    DWORD nowT = GetTickCount();
    if (nowT - g_smoothPruneTick >= 10000) {
        g_smoothPruneTick = nowT;
        for (auto it = g_smooth.begin(); it != g_smooth.end();) {
            double age = std::chrono::duration<double>(now - it->second.t).count();
            if (age > 10.0) it = g_smooth.erase(it);
            else ++it;
        }
    }
}

// 查找 LWJGL 窗口
struct FindCtx { DWORD pid; HWND hwnd; };
static BOOL CALLBACK find_enum(HWND hw, LPARAM lp) {
    auto* ctx = (FindCtx*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(hw, &pid);
    if (pid != ctx->pid) return TRUE;
    wchar_t cls[64] = {};
    GetClassNameW(hw, cls, 64);
    if (wcsstr(cls, L"GLFW") || wcsstr(cls, L"LWJGL")) {
        ctx->hwnd = hw;
        return FALSE;
    }
    return TRUE;
}
static HWND find_game_window() {
    FindCtx ctx{GetCurrentProcessId(), nullptr};
    EnumWindows(find_enum, (LPARAM)&ctx);
    return ctx.hwnd;
}

// ---- 预投影（游戏渲染线程内执行） ----
// 在 SwapBuffers 钩子里用【游戏画面同一帧】的相机 + 位置完成 3D→屏幕投影，
// 产出可直接绘制的屏幕线段（ScreenBox）。渲染线程只画，不再自己投影——
// 等价于 LiquidBounce 在游戏渲染管线里用同一数据绘制，box 与画面同步。
// 关键：位置直接用游戏【同一帧】的 partialTick 插值位置，不做速度外推——
// 覆盖层重绘紧随游戏帧（事件同步），两者同帧对齐，外推反而会在实体加减速时
// 预测不准造成瞬移/拖影。
static void project_entities(const CamData& cam, const std::vector<EntityData>& ents,
                             int w, int h, std::vector<ScreenBox>& out) {
    out.clear();
    float fov = cam.fov;
    if (fov <= 1.f) fov = 70.f;
    CamBasis cb;
    if (!cam_basis(cam.yaw, cam.pitch, fov, w, h, cb)) return;

    out.reserve(ents.size());
    for (const auto& e : ents) {
        if (e.dist > g_cfg.maxDistance) continue;
        if (!g_cfg.showPlayers && e.isPlayer) continue;
        if (!g_cfg.showMobs && e.isLiving && !e.isPlayer) continue;
        if (!g_cfg.showOthers && !e.isLiving && !e.isPlayer) continue;

        ScreenBox b;
        b.color = e.isPlayer ? g_cfg.colPlayer :
                  (e.isLiving ? g_cfg.colMob : g_cfg.colOther);

        float hw = (float)e.bbw * 0.5f;
        float hh = (float)e.bbh;
        // 位置：游戏同一帧插值位置（覆盖层紧随游戏帧重绘，无需外推）
        double x = e.ix;
        double y = e.iy;
        double z = e.iz;
        double corners[8][3] = {
            {x - hw, y,      z - hw}, {x + hw, y,      z - hw},
            {x + hw, y,      z + hw}, {x - hw, y,      z + hw},
            {x - hw, y + hh, z - hw}, {x + hw, y + hh, z - hw},
            {x + hw, y + hh, z + hw}, {x - hw, y + hh, z + hw},
        };

        float sx[8], sy[8];
        double camCz[8];
        bool valid[8];
        int validCount = 0;
        bool anyOnScreen = false;
        for (int j = 0; j < 8; ++j) {
            double cX, cY, cZ;
            cam_to_camspace(cb, cam.px, cam.py, cam.pz,
                            corners[j][0], corners[j][1], corners[j][2], cX, cY, cZ);
            camCz[j] = cZ;
            valid[j] = (cZ >= 0.01);
            if (valid[j]) {
                if (camspace_to_screen(cb, cX, cY, cZ, sx[j], sy[j])) {
                    validCount++;
                    if (sx[j] >= -w && sx[j] < w * 2 && sy[j] >= -h && sy[j] < h * 2)
                        anyOnScreen = true;
                }
            }
        }
        if (validCount == 0 || !anyOnScreen) continue;

        // 2D 极值（名字/填充定位；只取有效角点，杜绝后方残留坐标）
        float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
        for (int j = 0; j < 8; ++j)
            if (valid[j] && sx[j] >= 0 && sx[j] < w && sy[j] >= 0 && sy[j] < h) {
                minX = std::min(minX, sx[j]); minY = std::min(minY, sy[j]);
                maxX = std::max(maxX, sx[j]); maxY = std::max(maxY, sy[j]);
            }
        if (minX > maxX)
            for (int j = 0; j < 8; ++j)
                if (valid[j]) {
                    minX = std::min(minX, sx[j]); minY = std::min(minY, sy[j]);
                    maxX = std::max(maxX, sx[j]); maxY = std::max(maxY, sy[j]);
                }
        b.minX = minX; b.minY = minY; b.maxX = maxX; b.maxY = maxY;
        b.hasFill = g_cfg.filledBox;
        float cx = (minX + maxX) * 0.5f;

        // 3D 盒 12 条边：逐边近平面裁剪，生成屏幕线段（渲染线程直接画）
        if (g_cfg.box3d) {
            static const int edges[12][2] = {
                {0,1},{1,2},{2,3},{3,0}, {4,5},{5,6},{6,7},{7,4},
                {0,4},{1,5},{2,6},{3,7}
            };
            auto clipEdge = [&](int ai, int bi) {
                if (b.segCount >= 12) return;
                bool va = valid[ai], vb = valid[bi];
                if (!va && !vb) return;
                if (va && vb) {
                    b.segs[b.segCount++] = {sx[ai], sy[ai], sx[bi], sy[bi]};
                    return;
                }
                double t = (0.01 - camCz[ai]) / (camCz[bi] - camCz[ai]);
                if (t < 0.0) t = 0.0;
                if (t > 1.0) t = 1.0;
                int fi = va ? ai : bi, hi = va ? bi : ai;
                double clipX = corners[fi][0] + (corners[hi][0] - corners[fi][0]) * t;
                double clipY = corners[fi][1] + (corners[hi][1] - corners[fi][1]) * t;
                double clipZ = corners[fi][2] + (corners[hi][2] - corners[fi][2]) * t;
                double cX, cY, cZ;
                cam_to_camspace(cb, cam.px, cam.py, cam.pz, clipX, clipY, clipZ, cX, cY, cZ);
                float csx, csy;
                if (camspace_to_screen(cb, cX, cY, cZ, csx, csy))
                    b.segs[b.segCount++] = {sx[fi], sy[fi], csx, csy};
            };
            for (int j = 0; j < 12; ++j) clipEdge(edges[j][0], edges[j][1]);
        }

        // 名字
        if (g_cfg.nameTags && !e.name.empty()) {
            b.name = e.name;
            b.nameX = cx;
            b.nameY = minY - 18.f;
        }
        // tracer（屏幕底部到实体中腰）
        if (g_cfg.tracer) {
            float tx, ty;
            if (cam_project(cb, cam.px, cam.py, cam.pz, x, y + hh * 0.5, z, tx, ty)) {
                b.tx = tx; b.ty = ty;
                b.hasTracer = true;
            }
        }
        out.push_back(std::move(b));
    }
}

// ---- 共享：相机空间点 + 折线近平面裁剪 ----
struct CamPt { double cX, cY, cZ; bool valid; };

// 把 n 个相机空间点连成屏幕折线（逐段近平面裁剪），写入 td。
static void build_traj_segments(const CamBasis& cb, const CamPt* pts, int n,
                                TrajectoryData& td) {
    for (int i = 0; i < n - 1 && td.segCount < 64; ++i) {
        const CamPt& a = pts[i];
        const CamPt& b = pts[i + 1];
        bool va = a.valid, vb = b.valid;
        if (!va && !vb) continue;
        float s0x, s0y, s1x, s1y;
        if (va && vb) {
            if (!camspace_to_screen(cb, a.cX, a.cY, a.cZ, s0x, s0y)) continue;
            if (!camspace_to_screen(cb, b.cX, b.cY, b.cZ, s1x, s1y)) continue;
            td.segs[td.segCount++] = {s0x, s0y, s1x, s1y};
        } else {
            // 一端在近平面内、一端在后：线性插值出 camZ=0.01 的裁剪交点
            const CamPt& in  = va ? a : b;   // 在近平面内的端点
            const CamPt& out = va ? b : a;   // 在相机后方的端点
            double t = (0.01 - out.cZ) / (in.cZ - out.cZ);
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
            double cX = out.cX + (in.cX - out.cX) * t;
            double cY = out.cY + (in.cY - out.cY) * t;
            double cZ = out.cZ + (in.cZ - out.cZ) * t;
            float ix, iy, i2x, i2y;
            if (!camspace_to_screen(cb, cX, cY, cZ, ix, iy)) continue;
            if (!camspace_to_screen(cb, in.cX, in.cY, in.cZ, i2x, i2y)) continue;
            td.segs[td.segCount++] = {i2x, i2y, ix, iy};
        }
    }
}

// 投影一个以 (cx,cy,cz) 为中心、半边长 h 的小 3D 立方体到屏幕（12 条边 + 6 个面），
// 写入 td.cube（边线）与 td.cubeFaces（50% 透明平面），用于弓预判落点/命中标记。
static void project_landing_cube(const CamBasis& cb, const CamData& cam,
                                 double cx, double cy, double cz, double h,
                                 TrajectoryData& td) {
    double corners[8][3] = {
        {cx - h, cy - h, cz - h}, {cx + h, cy - h, cz - h},
        {cx + h, cy - h, cz + h}, {cx - h, cy - h, cz + h},
        {cx - h, cy + h, cz - h}, {cx + h, cy + h, cz - h},
        {cx + h, cy + h, cz + h}, {cx - h, cy + h, cz + h},
    };
    CamPt pts[8];
    for (int j = 0; j < 8; ++j) {
        cam_to_camspace(cb, cam.px, cam.py, cam.pz,
                        corners[j][0], corners[j][1], corners[j][2],
                        pts[j].cX, pts[j].cY, pts[j].cZ);
        pts[j].valid = (pts[j].cZ >= 0.01);
    }
    static const int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0}, {4,5},{5,6},{6,7},{7,4},
        {0,4},{1,5},{2,6},{3,7}
    };
    td.cubeCount = 0;
    for (int k = 0; k < 12 && td.cubeCount < 12; ++k) {
        int ai = edges[k][0], bi = edges[k][1];
        const CamPt& a = pts[ai];
        const CamPt& b = pts[bi];
        bool va = a.valid, vb = b.valid;
        if (!va && !vb) continue;
        float sx0, sy0, sx1, sy1;
        if (va && vb) {
            if (!camspace_to_screen(cb, a.cX, a.cY, a.cZ, sx0, sy0)) continue;
            if (!camspace_to_screen(cb, b.cX, b.cY, b.cZ, sx1, sy1)) continue;
            td.cube[td.cubeCount++] = {sx0, sy0, sx1, sy1};
        } else {
            const CamPt& in = va ? a : b;
            const CamPt& out = va ? b : a;
            double t = (0.01 - out.cZ) / (in.cZ - out.cZ);
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
            double cX = out.cX + (in.cX - out.cX) * t;
            double cY = out.cY + (in.cY - out.cY) * t;
            double cZ = out.cZ + (in.cZ - out.cZ) * t;
            float ix, iy, i2x, i2y;
            if (!camspace_to_screen(cb, cX, cY, cZ, ix, iy)) continue;
            if (!camspace_to_screen(cb, in.cX, in.cY, in.cZ, i2x, i2y)) continue;
            td.cube[td.cubeCount++] = {i2x, i2y, ix, iy};
        }
    }
    // 6 个面：底面/顶面/前/后/左/右。只有 4 角点都在相机前方才填入（平面填充）。
    static const int faces[6][4] = {
        {0,1,2,3},   // 底
        {4,5,6,7},   // 顶
        {0,1,5,4},   // 前
        {2,3,7,6},   // 后
        {0,3,7,4},   // 左
        {1,2,6,5},   // 右
    };
    td.cubeFacesCount = 0;
    for (int f = 0; f < 6 && td.cubeFacesCount < 6; ++f) {
        int a0 = faces[f][0], a1 = faces[f][1], a2 = faces[f][2], a3 = faces[f][3];
        if (!pts[a0].valid || !pts[a1].valid || !pts[a2].valid || !pts[a3].valid) continue;
        double wx[4] = {pts[a0].cX, pts[a1].cX, pts[a2].cX, pts[a3].cX};
        double wy[4] = {pts[a0].cY, pts[a1].cY, pts[a2].cY, pts[a3].cY};
        double wz[4] = {pts[a0].cZ, pts[a1].cZ, pts[a2].cZ, pts[a3].cZ};
        ScreenQuad& q = td.cubeFaces[td.cubeFacesCount];
        bool ok = true;
        for (int v = 0; v < 4; ++v)
            if (!camspace_to_screen(cb, wx[v], wy[v], wz[v], q.x[v], q.y[v])) { ok = false; break; }
        if (ok) ++td.cubeFacesCount;
    }
}

// ---- 弹道模拟（世界空间点序列） ----
// 用 MC 弹射物物理按 tick 步进：**先按当前速度移动，再施加 drag 衰减 + 重力**
// （与 AbstractArrow.tick / LiquidBounce 一致）。先前把 drag/重力放在移动前，
// 导致每 tick 走得更短、掉得更快，轨迹/末端偏低。返回生成的点数（含起点）；
// landIndex 输出“落点”下标（弹体回落到 groundY 高度）。
static int simulate_ballistic(double sx, double sy, double sz,
                              double vx, double vy, double vz,
                              double drag, double grav, int maxPoints, double groundY,
                              double* ox, double* oy, double* oz, int* landIndex) {
    double x = sx, y = sy, z = sz;
    ox[0] = x; oy[0] = y; oz[0] = z;
    int n = 1;
    int land = -1;
    for (int i = 1; i < maxPoints; ++i) {
        x += vx; y += vy; z += vz;          // 先按当前速度移动
        ox[i] = x; oy[i] = y; oz[i] = z;
        ++n;
        if (land < 0 && vy < 0 && y <= groundY) land = i;
        vx *= drag; vy *= drag; vz *= drag; // 再施加 drag 衰减
        vy -= grav;                          // 与重力
    }
    if (land < 0) land = n - 1;
    if (landIndex) *landIndex = land;
    return n;
}

// 线段 [p0,p1] 与 AABB [min,max] 相交检测（slab 法）。
// 返回是否相交，并输出进入参数 t(min∈[0,1]) 及交点坐标。
static bool seg_aabb(double p0x, double p0y, double p0z,
                     double p1x, double p1y, double p1z,
                     double minx, double miny, double minz,
                     double maxx, double maxy, double maxz,
                     double& tmin, double& ix, double& iy, double& iz) {
    const double eps = 1e-9;
    double dx = p1x - p0x, dy = p1y - p0y, dz = p1z - p0z;
    double t0 = 0.0, t1 = 1.0;
    auto slab = [&](double p0, double d, double mn, double ma) -> bool {
        if (std::fabs(d) < eps) return (p0 >= mn && p0 <= ma);
        double a = (mn - p0) / d, b = (ma - p0) / d;
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
        return t0 <= t1;
    };
    if (!slab(p0x, dx, minx, maxx)) return false;
    if (!slab(p0y, dy, miny, maxy)) return false;
    if (!slab(p0z, dz, minz, maxz)) return false;
    tmin = t0;
    ix = p0x + t0 * dx; iy = p0y + t0 * dy; iz = p0z + t0 * dz;
    return true;
}

// ---- 弹射物轨迹预测（游戏渲染线程内执行） ----
// 对每个敌方的弹射物实体（排除本地玩家射出的），用其当前速度按游戏物理向前
// 步进预测，把每个预测点投影到屏幕，得到一条折线（已近平面裁剪）。
// 物理参数与 Minecraft 1.20.1 一致：
//   箭            : drag=0.99, gravity=0.05
//   雪球/末影珍珠 : drag=0.99, gravity=0.03
static void project_trajectories(const CamData& cam,
                                 const std::vector<EntityData>& ents,
                                 int w, int h, std::vector<TrajectoryData>& out) {
    out.clear();
    if (!g_cfg.showTrajectory) return;
    CamBasis cb;
    if (!cam_basis(cam.yaw, cam.pitch, cam.fov, w, h, cb)) return;

    for (const auto& e : ents) {
        if (e.projType == PROJ_NONE) continue;
        if (e.ownProjectile) continue;   // 本地玩家射出的弹射物：轨迹由弓蓄力预判覆盖
        if (e.dist > g_cfg.maxDistance) continue;
        double drag = 0.99, grav;
        switch (e.projType) {
            case PROJ_ARROW: grav = 0.05; break;
            default:         grav = 0.03; break;   // 雪球/末影珍珠
        }
        double vx = e.vx, vy = e.vy, vz = e.vz;
        // 静止弹射物（落地/停住）不画轨迹
        if (vx * vx + vy * vy + vz * vz < 1e-6) continue;

        int ticks = g_cfg.trajectoryTicks;
        if (ticks < 5) ticks = 5;
        if (ticks > 63) ticks = 63;

        double o[3][64];
        simulate_ballistic(e.ix, e.iy, e.iz, vx, vy, vz, drag, grav, ticks + 1,
                           0.0, o[0], o[1], o[2], nullptr);

        CamPt pts[64];
        for (int i = 0; i <= ticks; ++i) {
            cam_to_camspace(cb, cam.px, cam.py, cam.pz, o[0][i], o[1][i], o[2][i],
                            pts[i].cX, pts[i].cY, pts[i].cZ);
            pts[i].valid = (pts[i].cZ >= 0.01);
        }

        TrajectoryData td;
        td.color = g_cfg.colTraj;
        build_traj_segments(cb, pts, ticks + 1, td);
        if (td.segCount > 0) out.push_back(std::move(td));
    }
}

// ---- 通用弓蓄力预判（给定射手位置/朝向/power，模拟轨迹+命中+落点方块）----
// 本地玩家与其他玩家共用。方向用射手自身朝向（本地玩家第一人称下=相机朝向，
// 其他玩家用其自身 yaw/pitch）。落点沿轨迹对生物/玩家做 AABB 命中——命中则
// 落点方块为红，否则为蓝（方块落点）。
static void project_bow_shot(const CamData& cam, const std::vector<EntityData>& ents,
                             int w, int h, std::vector<TrajectoryData>& out,
                             double sx, double sy, double sz,
                             double yaw, double pitch, float power, double groundY,
                             double shooterVX, double shooterVY, double shooterVZ,
                             bool shooterOnGround,
                             uint32_t lineColor, int shooterId) {
    if (!g_cfg.showTrajectory) return;
    // 弓蓄力 power：ticks/20 -> (p^2 + 2p)/3，封顶 1（与 ArrowItem 拉弓加成一致）
    power = (power * power + power * 2.0f) / 3.0f;
    if (power > 1.0f) power = 1.0f;
    if (power < 0.05f) return;   // 刚拉弓几乎无力，不画
    double speed = 3.0 * power;

    CamBasis cb;
    if (!cam_basis(cam.yaw, cam.pitch, cam.fov, w, h, cb)) return;

    // 方向 = 射手视线（本地玩家第一人称=相机；其他玩家用其自身朝向）
    double fx = mc_forward_x(yaw, pitch);
    double fy = mc_forward_y(pitch);
    double fz = mc_forward_z(yaw, pitch);
    // 初速 = 视线方向 * 箭速 + 射手当前动量。
    // MC 1.20.1 Projectile.m_37251_ / shootFromRotation 已核验：
    //   箭速方向先经 shoot() 设置，随后 add(shooter.deltaMovement)；
    //   水平动量【始终】继承，垂直动量【仅当射手不在地面】继承。
    // 这正是跳跃/下落时旧预测偏高/偏低的原因。
    double vx = fx * speed + shooterVX;
    double vy = fy * speed + (shooterOnGround ? 0.0 : shooterVY);
    double vz = fz * speed + shooterVZ;

    double o[3][64];
    int land = -1;
    int n = simulate_ballistic(sx, sy, sz, vx, vy, vz,
                               0.99, 0.05, 64, groundY, o[0], o[1], o[2], &land);

    // ---- 命中检测：沿轨迹逐段，先方块射线（level.clip），再无障碍下检测生物/玩家 AABB ----
    // 仿 LiquidBounce：逐段按顺序找第一个命中（方块优先），命中即停，后续不再画。
    int hitSeg = -1;
    bool entityHit = false;
    double hx = 0, hy = 0, hz = 0;
    for (int i = 0; i < n - 1; ++i) {
        // 方块命中（障碍物）：优先于实体
        double bx, by, bz;
        bool bhit = false;
        if (jvm_clip_block(o[0][i], o[1][i], o[2][i],
                           o[0][i + 1], o[1][i + 1], o[2][i + 1],
                           bx, by, bz, bhit) && bhit) {
            hitSeg = i; entityHit = false;
            hx = bx; hy = by; hz = bz;
            break;
        }
        // 实体命中（生物/玩家，self 已被采集过滤；并跳过发射者自己，
        // 否则其他玩家的轨迹第一段会与其自身 AABB 相交，落点被误判在开始端）
        bool entHitHere = false;
        for (const auto& e : ents) {
            if (e.id == shooterId) continue;
            if (!e.isLiving && !e.isPlayer) continue;
            double hw = e.bbw * 0.5, hh = e.bbh;
            double minx = e.ix - hw, miny = e.iy, minz = e.iz - hw;
            double maxx = e.ix + hw, maxy = e.iy + hh, maxz = e.iz + hw;
            double tmin, ix, iy, iz;
            if (seg_aabb(o[0][i], o[1][i], o[2][i],
                         o[0][i + 1], o[1][i + 1], o[2][i + 1],
                         minx, miny, minz, maxx, maxy, maxz, tmin, ix, iy, iz)) {
                hitSeg = i; entityHit = true;
                hx = ix; hy = iy; hz = iz;
                entHitHere = true;
                break;
            }
        }
        if (entHitHere) break;
    }

    // 落点：实体命中用命中点；方块命中用命中点；否则用模拟落点
    bool anyHit = (hitSeg >= 0);
    int endIdx = anyHit ? hitSeg : land;
    double lx0 = anyHit ? hx : o[0][land];
    double ly0 = anyHit ? hy : o[1][land];
    double lz0 = anyHit ? hz : o[2][land];

    // 轨迹折线：0..endIdx 模拟点 + 落点（命中交点或模拟落点）
    double path[3][64];
    int pc = 0;
    for (int i = 0; i <= endIdx && pc < 64; ++i) {
        path[0][pc] = o[0][i]; path[1][pc] = o[1][i]; path[2][pc] = o[2][i];
        ++pc;
    }
    if (anyHit && pc < 64) {
        path[0][pc] = lx0; path[1][pc] = ly0; path[2][pc] = lz0;
        ++pc;
    }

    CamPt pts[64];
    for (int i = 0; i < pc; ++i) {
        cam_to_camspace(cb, cam.px, cam.py, cam.pz, path[0][i], path[1][i], path[2][i],
                        pts[i].cX, pts[i].cY, pts[i].cZ);
        pts[i].valid = (pts[i].cZ >= 0.01);
    }

    TrajectoryData td;
    td.color = lineColor;
    build_traj_segments(cb, pts, pc, td);
    // 落点：3D 立方体（蓝=方块落点/障碍物，红=命中实体）
    if (pc > 0) {
        float lx, ly;
        if (camspace_to_screen(cb, pts[pc - 1].cX, pts[pc - 1].cY, pts[pc - 1].cZ, lx, ly)) {
            td.hasLanding = true;
            td.lx = lx; td.ly = ly;
            td.landColor = entityHit ? g_cfg.colLandHit : g_cfg.colLand;
            // 落点 3D 立方体：半边长 0.3（比 0.5 缩小 40%），中心抬高 0.35。
            // 6 个面渲染半透明平面（50%），详见 esp_draw_overlay。
            project_landing_cube(cb, cam, lx0, ly0 + 0.35, lz0, 0.3, td);
        }
    }
    if (td.segCount > 0 || td.hasLanding) out.push_back(std::move(td));
}

// ---- 本地玩家弓蓄力预判（第一人称：相机朝向即本地玩家朝向） ----
static void project_bow_predict(const CamData& cam, const PlayerInfo& lp,
                                const std::vector<EntityData>& ents,
                                int w, int h, std::vector<TrajectoryData>& out) {
    if (!lp.ok || !lp.chargingBow) return;
    // AbstractArrow 出生点 = (shooter.x, shooter.eyeY - 0.1, shooter.z)
    project_bow_shot(cam, ents, w, h, out,
                     lp.px, lp.py + lp.eyeHeight - 0.1, lp.pz,
                     cam.yaw, cam.pitch,
                     (float)lp.useTicks / 20.0f, lp.py,
                     lp.vx, lp.vy, lp.vz, lp.onGround,
                     g_cfg.colTraj, -1);
}

// ---- 其他玩家弓蓄力预判（渲染每个正在拉弓的玩家的抛物线） ----
static void project_other_bow_predicts(const CamData& cam,
                                       const std::vector<EntityData>& ents,
                                       int w, int h, std::vector<TrajectoryData>& out) {
    if (!g_cfg.showTrajectory) return;
    for (const auto& e : ents) {
        if (!e.isPlayer || !e.chargingBow) continue;
        if (e.dist > g_cfg.maxDistance) continue;
        // 其他玩家眼睛高度用默认 1.62；出生点按 AbstractArrow 为 eyeY - 0.1；
        // groundY 用其脚底高度；线用红色（colTrajOther）区分本地玩家。
        project_bow_shot(cam, ents, w, h, out,
                         e.ix, e.iy + 1.62 - 0.1, e.iz,
                         e.yaw, e.pitch,
                         (float)e.useTicks / 20.0f, e.iy,
                         e.vx, e.vy, e.vz, e.onGround,
                         g_cfg.colTrajOther, e.id);
    }
}

// ============================================================
// Insert 菜单（鼠标可交互 + 滑块 + 离屏缓存绘制）
//   Insert       呼出 / 关闭
//   鼠标左键     点选行 / 拖拽滑块 / 点击面板外关闭
//   ↑ / ↓        选择项目
//   ← / →        减小 / 增大，或循环选项（按住可连续调整）
//   Enter        切换布尔项；在热键项上按 Enter 后按任意键绑定
//   Esc          关闭菜单 / 取消热键绑定
//
//   卡顿优化：菜单只在“状态变化 / 鼠标悬停变化 / 250ms 动态刷新”时
//   离屏重绘一次，平时每帧仅 memcpy 缓存面板 + UpdateLayeredWindow，
//   不再每帧重画 20+ 行文字与填充。
// ============================================================
enum MenuRow {
    M_CLICKER = 0,
    M_ESP,
    M_PROFILE,
    M_LEFT,
    M_LEFT_CPS,
    M_LEFT_PRESET,
    M_RIGHT,
    M_RIGHT_CPS,
    M_RIGHT_PRESET,
    M_KEEP,
    M_HOTKEY,
    M_ATTACK_GATE,
    M_PLACE_GATE,
    M_CURSOR_GATE,
    M_RANDOM,
    M_RANDOM_RANGE,
    M_HUMAN_MODE,
    M_HUMAN_LEVEL,
    M_CPS_MAX,
    M_AUTOSTOP,
    M_AUTOSTOP_SEC,
    M_ATTACK_KEY,
    M_PLACE_KEY,
    M_COUNT
};

static const wchar_t* kHumanNames[4] = { L"均匀", L"双击连招", L"呼吸波动", L"疲劳递减" };

// ---- 菜单缓存与交互状态 ----
struct MenuCache {
    std::vector<uint32_t> px;
    int w = 0, h = 0;
    bool valid = false;
};
static MenuCache        g_menuCache;         // 只由游戏渲染线程访问
static std::atomic<bool> g_menuDirty{true};
static std::atomic<int>  g_menuHover{-1};
static std::atomic<int>  g_menuDrag{-1};
static DWORD             g_menuLastRenderMs = 0;   // 游戏线程专用

static constexpr int kMenuRowH = 17;
static constexpr int kMenuHeaderH = 26;
static constexpr int kMenuFooterH = 26;

static int menu_panel_height() {
    return kMenuHeaderH + M_COUNT * kMenuRowH + kMenuFooterH;
}

static void menu_panel_rect(int w, int h, int& px, int& py, int& pw, int& ph) {
    pw = (std::min)(400, w - 16);
    ph = menu_panel_height();
    px = w - pw - 10;
    py = h - ph - 8;
    if (py < 8) py = 8;
}

static int menu_hit_row(int py, int y) {
    int rel = y - (py + kMenuHeaderH);
    if (rel < 0) return -1;
    int row = rel / kMenuRowH;
    if (row >= M_COUNT) return -1;
    return row;
}

static bool menu_is_slider(int row) {
    return row == M_LEFT_CPS || row == M_RIGHT_CPS || row == M_RANDOM_RANGE ||
           row == M_HUMAN_LEVEL || row == M_CPS_MAX || row == M_AUTOSTOP_SEC;
}

static void menu_slider_rect(int px, int pw, int y, int& x0, int& x1) {
    x0 = px + 194;
    x1 = px + pw - 14;
    if (x1 < x0 + 20) x1 = x0 + 20;
}

static float menu_slider_norm(int row, const ClickerSettings& cl) {
    switch (row) {
    case M_LEFT_CPS: {
        int max10 = cl.cpsMax * 10;
        return (float)(cl.cpsLeft10 - 5) / (float)(max10 - 5);
    }
    case M_RIGHT_CPS: {
        int max10 = cl.cpsMax * 10;
        return (float)(cl.cpsRight10 - 5) / (float)(max10 - 5);
    }
    case M_RANDOM_RANGE: return (float)(cl.randomRange - 1) / 4.0f;
    case M_HUMAN_LEVEL:  return (float)(cl.humanizeLevel - 1) / 4.0f;
    case M_CPS_MAX:      return (float)(cl.cpsMax - 20) / 480.0f;
    case M_AUTOSTOP_SEC:
        return (float)((std::log((double)cl.autoStopSeconds) - std::log(1.0)) /
                       (std::log(3600.0) - std::log(1.0)));
    default: return 0.0f;
    }
}

static void menu_slider_apply(int row, float t, ClickerSettings& cl) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    switch (row) {
    case M_LEFT_CPS: {
        int max10 = cl.cpsMax * 10;
        int v = 5 + (int)(t * (float)(max10 - 5) + 0.5f);
        if (v > max10) v = max10;
        cl.cpsLeft10 = v;
        break;
    }
    case M_RIGHT_CPS: {
        int max10 = cl.cpsMax * 10;
        int v = 5 + (int)(t * (float)(max10 - 5) + 0.5f);
        if (v > max10) v = max10;
        cl.cpsRight10 = v;
        break;
    }
    case M_RANDOM_RANGE:
        cl.randomRange = 1 + (int)(t * 4.0f + 0.5f);
        break;
    case M_HUMAN_LEVEL:
        cl.humanizeLevel = 1 + (int)(t * 4.0f + 0.5f);
        break;
    case M_CPS_MAX: {
        int v = 20 + (int)(t * 480.0f + 0.5f);
        if (v > 500) v = 500;
        cl.cpsMax = v;
        int max10 = v * 10;
        if (cl.cpsLeft10 > max10) cl.cpsLeft10 = max10;
        if (cl.cpsRight10 > max10) cl.cpsRight10 = max10;
        break;
    }
    case M_AUTOSTOP_SEC:
        cl.autoStopSeconds = (int)(std::exp(std::log(1.0) +
                                  t * (std::log(3600.0) - std::log(1.0))) + 0.5);
        if (cl.autoStopSeconds < 1) cl.autoStopSeconds = 1;
        if (cl.autoStopSeconds > 3600) cl.autoStopSeconds = 3600;
        break;
    default: break;
    }
}

static void commit_clicker_config() {
    g_cfg.profiles[g_cfg.activeProfile] = g_cfg.clicker;
    clicker_apply_settings(g_cfg.clicker);
    config_save(g_cfg);
    g_menuDirty.store(true, std::memory_order_release);
}

// 连点线程内热键修改攻击/放置门控后，把最新设置写回 g_cfg + esp.ini。
static void on_clicker_hotkey_changed() {
    std::lock_guard<std::mutex> lock(g_clickerCfgMutex);
    ClickerSnapshot cs = clicker_snapshot();
    g_cfg.clicker = cs.settings;
    g_cfg.profiles[g_cfg.activeProfile] = cs.settings;
    config_save(g_cfg);
    g_menuDirty.store(true, std::memory_order_release);
}

static void menu_move(int dir) {
    int cur = g_menuCursor.load(std::memory_order_relaxed);
    cur += dir;
    if (cur < 0) cur = M_COUNT - 1;
    if (cur >= M_COUNT) cur = 0;
    g_menuCursor.store(cur, std::memory_order_release);
    g_menuDirty.store(true, std::memory_order_release);
}

static void menu_adjust(int row, int dir, bool fast) {
    ClickerSettings& cl = g_cfg.clicker;
    switch (row) {
    case M_CLICKER:
        clicker_toggle_running();
        cl.enabled = clicker_snapshot().running;
        break;
    case M_ESP: {
        bool next = !g_espEnabled.load(std::memory_order_acquire);
        g_espEnabled.store(next, std::memory_order_release);
        g_cfg.enabled = next;
        break;
    }
    case M_PROFILE: {
        g_cfg.profiles[g_cfg.activeProfile] = g_cfg.clicker;
        int next = g_cfg.activeProfile + dir;
        if (next < 0) next = EspConfig::kClickerProfiles - 1;
        if (next >= EspConfig::kClickerProfiles) next = 0;
        g_cfg.activeProfile = next;
        g_cfg.clicker = g_cfg.profiles[next];
        break;
    }
    case M_LEFT:  cl.leftEnabled = !cl.leftEnabled; break;
    case M_RIGHT: cl.rightEnabled = !cl.rightEnabled; break;
    case M_KEEP:  cl.keep = !cl.keep; break;
    case M_ATTACK_GATE: cl.attackGate = !cl.attackGate; break;
    case M_PLACE_GATE:  cl.placeGate = !cl.placeGate; break;
    case M_CURSOR_GATE: cl.cursorGate = !cl.cursorGate; break;
    case M_RANDOM: cl.randomEnabled = !cl.randomEnabled; break;
    case M_AUTOSTOP: cl.autoStopEnabled = !cl.autoStopEnabled; break;

    case M_LEFT_CPS: {
        int step = fast ? 10 : 1;
        int max10 = cl.cpsMax * 10;
        int v = cl.cpsLeft10 + dir * step;
        if (v < 5) v = 5;
        if (v > max10) v = max10;
        cl.cpsLeft10 = v;
        break;
    }
    case M_RIGHT_CPS: {
        int step = fast ? 10 : 1;
        int max10 = cl.cpsMax * 10;
        int v = cl.cpsRight10 + dir * step;
        if (v < 5) v = 5;
        if (v > max10) v = max10;
        cl.cpsRight10 = v;
        break;
    }
    case M_LEFT_PRESET: {
        static const int presets[6] = { 60, 100, 150, 200, 300, 400 };
        int cur = cl.cpsLeft10, idx = 0, best = 100000;
        for (int i = 0; i < 6; ++i) {
            int d = cur - presets[i]; if (d < 0) d = -d;
            if (d < best) { best = d; idx = i; }
        }
        idx += dir;
        if (idx < 0) idx = 0;
        if (idx > 5) idx = 5;
        int v = presets[idx];
        int max10 = cl.cpsMax * 10;
        if (v > max10) v = max10;
        cl.cpsLeft10 = v;
        break;
    }
    case M_RIGHT_PRESET: {
        static const int presets[6] = { 60, 100, 150, 200, 300, 400 };
        int cur = cl.cpsRight10, idx = 0, best = 100000;
        for (int i = 0; i < 6; ++i) {
            int d = cur - presets[i]; if (d < 0) d = -d;
            if (d < best) { best = d; idx = i; }
        }
        idx += dir;
        if (idx < 0) idx = 0;
        if (idx > 5) idx = 5;
        int v = presets[idx];
        int max10 = cl.cpsMax * 10;
        if (v > max10) v = max10;
        cl.cpsRight10 = v;
        break;
    }
    case M_RANDOM_RANGE:
        cl.randomRange += dir;
        if (cl.randomRange < 1) cl.randomRange = 1;
        if (cl.randomRange > 5) cl.randomRange = 5;
        break;
    case M_HUMAN_MODE:
        cl.humanizeMode += dir;
        if (cl.humanizeMode < 0) cl.humanizeMode = 3;
        if (cl.humanizeMode > 3) cl.humanizeMode = 0;
        break;
    case M_HUMAN_LEVEL:
        cl.humanizeLevel += dir;
        if (cl.humanizeLevel < 1) cl.humanizeLevel = 1;
        if (cl.humanizeLevel > 5) cl.humanizeLevel = 5;
        break;
    case M_CPS_MAX: {
        int step = fast ? 10 : 1;
        int v = cl.cpsMax + dir * step;
        if (v < 20) v = 20;
        if (v > 500) v = 500;
        cl.cpsMax = v;
        int max10 = v * 10;
        if (cl.cpsLeft10 > max10) cl.cpsLeft10 = max10;
        if (cl.cpsRight10 > max10) cl.cpsRight10 = max10;
        break;
    }
    case M_AUTOSTOP_SEC: {
        int step = fast ? 10 : 1;
        int v = cl.autoStopSeconds + dir * step;
        if (v < 1) v = 1;
        if (v > 3600) v = 3600;
        cl.autoStopSeconds = v;
        break;
    }
    default: return;
    }
    commit_clicker_config();
}

static bool menu_key_is_nav(int vk) {
    return vk == VK_INSERT || vk == VK_ESCAPE || vk == VK_UP || vk == VK_DOWN ||
           vk == VK_LEFT || vk == VK_RIGHT || vk == VK_RETURN ||
           vk == VK_LBUTTON || vk == VK_RBUTTON ||
           vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_SHIFT ||
           vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_CONTROL ||
           vk == VK_LMENU || vk == VK_RMENU || vk == VK_MENU;
}

static void menu_capture_commit(int vk) {
    int target = g_menuCaptureTarget.load(std::memory_order_relaxed);
    switch (target) {
    case M_HOTKEY:     g_cfg.clicker.toggleKey = vk; break;
    case M_ATTACK_KEY: g_cfg.clicker.attackGateKey = vk; break;
    case M_PLACE_KEY:  g_cfg.clicker.placeGateKey = vk; break;
    default: break;
    }
    g_menuCaptureKey.store(false, std::memory_order_release);
    commit_clicker_config();
    esp_log("[menu] 热键已绑定 target=%d vk=%d", target, vk);
}

static bool key_down_any(int vk) {
    return vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static void menu_activate_row(int row) {
    switch (row) {
    case M_HOTKEY:
    case M_ATTACK_KEY:
    case M_PLACE_KEY:
        g_menuCaptureTarget.store(row, std::memory_order_release);
        g_menuCaptureKey.store(true, std::memory_order_release);
        esp_log("[menu] 等待按下新热键…");
        break;
    case M_PROFILE:
    case M_LEFT_PRESET:
    case M_RIGHT_PRESET:
    case M_HUMAN_MODE:
        menu_adjust(row, 1, false);   // 鼠标点击这些行 = 循环到下一项
        break;
    default:
        menu_adjust(row, 0, false);   // 布尔项点击切换
        break;
    }
}

// 鼠标拖拽/点击时实时改值；不在拖动中写盘，松开左键才 commit。
static void menu_slider_set_from_x(int row, int x) {
    // 使用当前覆盖层客户区宽高计算滑块轨道
    RECT cr{};
    if (g_overlay.hwnd()) GetClientRect(g_overlay.hwnd(), &cr);
    int W = cr.right > 0 ? cr.right : 800;
    int H = cr.bottom > 0 ? cr.bottom : 600;
    int px = 0, pya = 0, pwv = 0, phv = 0;
    menu_panel_rect(W, H, px, pya, pwv, phv);
    int ry = pya + kMenuHeaderH + row * kMenuRowH;
    int x0 = 0, x1 = 0;
    menu_slider_rect(px, pwv, ry, x0, x1);
    float t = (float)(x - x0) / (float)(x1 - x0);
    menu_slider_apply(row, t, g_cfg.clicker);
    clicker_apply_settings(g_cfg.clicker);
    g_menuDirty.store(true, std::memory_order_release);
}

static void menu_poll_mouse() {
    RECT cr{};
    if (!g_overlay.hwnd() || !GetClientRect(g_overlay.hwnd(), &cr)) return;
    int W = cr.right, H = cr.bottom;
    if (W <= 0 || H <= 0) return;
    int px = 0, py = 0, pw = 0, ph = 0;
    menu_panel_rect(W, H, px, py, pw, ph);

    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(g_overlay.hwnd(), &pt);

    bool down = key_down_any(VK_LBUTTON);
    static bool prevDown = false;

    if (down && !prevDown) {
        prevDown = true;
        // 面板外左键：关闭菜单
        if (pt.x < px || pt.x >= px + pw || pt.y < py || pt.y >= py + ph) {
            g_menuVisible.store(false, std::memory_order_release);
            g_menuDirty.store(true, std::memory_order_release);
            return;
        }
        int row = menu_hit_row(py, pt.y);
        if (row >= 0) {
            g_menuCursor.store(row, std::memory_order_release);
            g_menuHover.store(row, std::memory_order_release);
            g_menuDirty.store(true, std::memory_order_release);
            if (menu_is_slider(row)) {
                g_menuDrag.store(row, std::memory_order_release);
                menu_slider_set_from_x(row, pt.x);
            } else {
                menu_activate_row(row);
            }
        }
    } else if (!down && prevDown) {
        prevDown = false;
        if (g_menuDrag.load(std::memory_order_relaxed) >= 0) {
            g_menuDrag.store(-1, std::memory_order_release);
            commit_clicker_config();   // 拖动结束才写配置
        }
    } else if (down) {
        int drag = g_menuDrag.load(std::memory_order_relaxed);
        if (drag >= 0) menu_slider_set_from_x(drag, pt.x);
    } else {
        int row = (pt.x >= px && pt.x < px + pw && pt.y >= py && pt.y < py + ph)
                      ? menu_hit_row(py, pt.y) : -1;
        int old = g_menuHover.load(std::memory_order_relaxed);
        if (row != old) {
            g_menuHover.store(row, std::memory_order_release);
            g_menuDirty.store(true, std::memory_order_release);
        }
    }
}

// 渲染线程每 ~16ms 调用。只处理菜单输入，不接触 JVM / 不创建窗口。
static void menu_handle_input(HWND gameHwnd) {
    if (GetForegroundWindow() != gameHwnd || IsIconic(gameHwnd)) return;
    std::lock_guard<std::mutex> cfgLock(g_clickerCfgMutex);

    static bool prevInsert = false, prevUp = false, prevDown = false,
                prevLeft = false, prevRight = false, prevEnter = false,
                prevEsc = false;
    static DWORD nextRepeat = 0;
    static int  repeatDir = 0;

    // ---- 热键捕获模式 ----
    if (g_menuCaptureKey.load(std::memory_order_acquire)) {
        prevInsert = key_down_any(g_cfg.menuKey);
        prevUp = key_down_any(VK_UP);
        prevDown = key_down_any(VK_DOWN);
        prevLeft = key_down_any(VK_LEFT);
        prevRight = key_down_any(VK_RIGHT);
        prevEnter = key_down_any(VK_RETURN);
        prevEsc = key_down_any(VK_ESCAPE);
        if (key_down_any(VK_ESCAPE)) {
            g_menuCaptureKey.store(false, std::memory_order_release);
            g_menuDirty.store(true, std::memory_order_release);
            esp_log("[menu] 取消热键绑定");
            return;
        }
        for (int vk = 1; vk <= 254; ++vk) {
            if (vk == VK_ESCAPE || menu_key_is_nav(vk)) continue;
            if (key_down_any(vk)) {
                menu_capture_commit(vk);
                return;
            }
        }
        return;
    }

    bool insert = key_down_any(g_cfg.menuKey);
    if (insert && !prevInsert) {
        bool next = !g_menuVisible.load(std::memory_order_acquire);
        g_menuVisible.store(next, std::memory_order_release);
        g_menuDirty.store(true, std::memory_order_release);
        if (next) {
            g_menuCursor.store(0, std::memory_order_release);
            g_menuHover.store(-1, std::memory_order_release);
            g_menuDrag.store(-1, std::memory_order_release);
            esp_log("[menu] Insert 打开菜单");
        } else {
            esp_log("[menu] Insert 关闭菜单");
        }
    }
    prevInsert = insert;
    if (!g_menuVisible.load(std::memory_order_acquire)) return;

    bool esc = key_down_any(VK_ESCAPE);
    if (esc && !prevEsc) {
        g_menuVisible.store(false, std::memory_order_release);
        g_menuDirty.store(true, std::memory_order_release);
        prevEsc = true;
        return;
    }
    prevEsc = esc;

    bool up = key_down_any(VK_UP);
    bool down = key_down_any(VK_DOWN);
    bool left = key_down_any(VK_LEFT);
    bool right = key_down_any(VK_RIGHT);
    bool enter = key_down_any(VK_RETURN);
    bool fast = key_down_any(VK_SHIFT) || key_down_any(VK_LSHIFT) || key_down_any(VK_RSHIFT);

    auto navAction = [&](bool cur, bool prev, int dir) {
        if (cur && !prev) {
            nextRepeat = GetTickCount() + 260;
            repeatDir = dir;
            menu_move(dir);
            return;
        }
        if (cur && repeatDir == dir && (int)(GetTickCount() - nextRepeat) >= 0) {
            nextRepeat = GetTickCount() + 45;
            menu_move(dir);
        }
    };
    navAction(up, prevUp, -1);
    navAction(down, prevDown, 1);
    prevUp = up; prevDown = down;

    int row = g_menuCursor.load(std::memory_order_relaxed);
    auto adjAction = [&](bool cur, bool prev, int dir) {
        if (cur && !prev) {
            nextRepeat = GetTickCount() + 260;
            repeatDir = dir;
            menu_adjust(row, dir, fast);
            return;
        }
        if (cur && repeatDir == dir && (int)(GetTickCount() - nextRepeat) >= 0) {
            nextRepeat = GetTickCount() + 55;
            menu_adjust(row, dir, fast);
        }
    };
    adjAction(left, prevLeft, -1);
    adjAction(right, prevRight, 1);
    prevLeft = left; prevRight = right;

    if (enter && !prevEnter) {
        row = g_menuCursor.load(std::memory_order_relaxed);
        menu_activate_row(row);
    }
    prevEnter = enter;

    // ---- 鼠标：覆盖层在菜单打开期间已切换为可点击 ----
    menu_poll_mouse();
}

// ============================================================
// 菜单离屏绘制（只在 dirty / 250ms 动态刷新时执行一次）
// ============================================================
static void draw_menu_panel(Overlay& ov, int panelW, int panelH) {
    ClickerSnapshot cs = clicker_snapshot();
    const ClickerSettings& cl = cs.settings;

    int activeProfile = 0;
    {
        std::lock_guard<std::mutex> lock(g_clickerCfgMutex);
        activeProfile = g_cfg.activeProfile;
    }

    const uint32_t colBg = 0x10131C;
    const uint32_t colBorder = 0x35618F;
    const uint32_t colSel = 0x1E3A5F;
    const uint32_t colHover = 0x24486F;
    const uint32_t colText = 0xE7EFFB;
    const uint32_t colDim = 0x9AA7B8;
    const uint32_t colAcc = 0x66A3FF;
    const uint32_t colOn = 0x52D88C;
    const uint32_t colOff = 0x8A93A3;
    const uint32_t colTrack = 0x2A3850;
    const uint32_t colThumb = 0xDDE7F5;

    ov.fillRectOpaque(0, 0, (float)panelW, (float)panelH, colBg);
    ov.drawLine(0, 0, (float)panelW, 0, colBorder, 1);
    ov.drawLine(0, (float)panelH - 1, (float)panelW, (float)panelH - 1, colBorder, 1);
    ov.drawLine(0, 0, 0, (float)panelH, colBorder, 1);
    ov.drawLine((float)panelW - 1, 0, (float)panelW - 1, (float)panelH, colBorder, 1);

    wchar_t buf[80];
    swprintf(buf, 80, L"连点器菜单  [Insert 关闭]  CPS %d", cs.realtimeCps);
    ov.drawText(10, 5, buf, colAcc, 14);
    ov.drawLine(8, (float)kMenuHeaderH - 2, (float)panelW - 8, (float)kMenuHeaderH - 2, colBorder, 1);

    int cursor = g_menuCursor.load(std::memory_order_relaxed);
    int hover = g_menuHover.load(std::memory_order_relaxed);
    int drag = g_menuDrag.load(std::memory_order_relaxed);
    std::wstring on = L"开", off = L"关";

    for (int i = 0; i < M_COUNT; ++i) {
        const wchar_t* label = L"";
        std::wstring value;
        uint32_t color = colText;
        bool slider = menu_is_slider(i);
        switch (i) {
        case M_CLICKER:      label = L"连点器"; value = cs.running ? on : off; color = cs.running ? colOn : colOff; break;
        case M_ESP:          label = L"ESP"; value = g_espEnabled.load() ? on : off; color = g_espEnabled.load() ? colOn : colOff; break;
        case M_PROFILE:      label = L"配置方案"; swprintf(buf, 80, L"方案 %d", activeProfile + 1); value = buf; color = colAcc; break;
        case M_LEFT:         label = L"左键连点"; value = cl.leftEnabled ? on : off; color = cl.leftEnabled ? colOn : colOff; break;
        case M_LEFT_CPS:     label = L"左键 CPS"; swprintf(buf, 80, L"%.1f", cl.cpsLeft10 / 10.0); value = buf; color = colAcc; break;
        case M_LEFT_PRESET:  label = L"左键预设"; value = L"6/10/15/20/30/40"; color = colAcc; break;
        case M_RIGHT:        label = L"右键连点"; value = cl.rightEnabled ? on : off; color = cl.rightEnabled ? colOn : colOff; break;
        case M_RIGHT_CPS:    label = L"右键 CPS"; swprintf(buf, 80, L"%.1f", cl.cpsRight10 / 10.0); value = buf; color = colAcc; break;
        case M_RIGHT_PRESET: label = L"右键预设"; value = L"6/10/15/20/30/40"; color = colAcc; break;
        case M_KEEP:         label = L"保持模式"; value = cl.keep ? on : off; color = cl.keep ? colOn : colOff; break;
        case M_HOTKEY:       label = L"连点热键"; value = clicker_key_name(cl.toggleKey); color = colAcc; break;
        case M_ATTACK_GATE:  label = L"仅可攻击时左键"; value = cl.attackGate ? on : off; color = cl.attackGate ? colOn : colOff; break;
        case M_PLACE_GATE:   label = L"仅持方块时右键"; value = cl.placeGate ? on : off; color = cl.placeGate ? colOn : colOff; break;
        case M_CURSOR_GATE:  label = L"光标门控"; value = cl.cursorGate ? on : off; color = cl.cursorGate ? colOn : colOff; break;
        case M_RANDOM:       label = L"随机 CPS"; value = cl.randomEnabled ? on : off; color = cl.randomEnabled ? colOn : colOff; break;
        case M_RANDOM_RANGE: label = L"随机范围"; swprintf(buf, 80, L"±%d CPS", cl.randomRange); value = buf; color = colAcc; break;
        case M_HUMAN_MODE:   label = L"拟人化节奏"; value = kHumanNames[cl.humanizeMode & 3]; color = colAcc; break;
        case M_HUMAN_LEVEL:  label = L"拟人化强度"; swprintf(buf, 80, L"%d / 5", cl.humanizeLevel); value = buf; color = colAcc; break;
        case M_CPS_MAX:      label = L"CPS 上限"; swprintf(buf, 80, L"%d", cl.cpsMax); value = buf; color = colAcc; break;
        case M_AUTOSTOP:     label = L"定时停止"; value = cl.autoStopEnabled ? on : off; color = cl.autoStopEnabled ? colOn : colOff; break;
        case M_AUTOSTOP_SEC: label = L"停止秒数"; swprintf(buf, 80, L"%d 秒", cl.autoStopSeconds); value = buf; color = colAcc; break;
        case M_ATTACK_KEY:   label = L"攻击门控热键"; value = clicker_key_name(cl.attackGateKey); color = colAcc; break;
        case M_PLACE_KEY:    label = L"放置门控热键"; value = clicker_key_name(cl.placeGateKey); color = colAcc; break;
        }

        float ry = (float)(kMenuHeaderH + i * kMenuRowH);
        if (i == cursor) {
            ov.fillRectOpaque(3, ry, (float)panelW - 3, ry + kMenuRowH + 1, colSel);
        } else if (i == hover) {
            ov.fillRectAlpha(3, ry, (float)panelW - 3, ry + kMenuRowH + 1, colHover, 0.65f);
        }

        if (g_menuCaptureKey.load() && g_menuCaptureTarget.load() == i)
            value = L"按任意键…";

        ov.drawText(10, ry, label, i == cursor ? colText : colDim, 13);

        if (slider) {
            float vw = ov.measureText(value, 13);
            ov.drawText(126, ry, value, i == cursor ? colAcc : color, 13);
            int x0 = 0, x1 = 0;
            menu_slider_rect(0, panelW, (int)ry, x0, x1);
            float t = menu_slider_norm(i, cl);
            float thumbX = x0 + t * (float)(x1 - x0);
            ov.fillRectOpaque((float)x0, ry + 7, (float)x1, ry + 10, colTrack);
            ov.fillRectOpaque((float)x0, ry + 7, thumbX, ry + 10, colAcc);
            ov.fillRectOpaque(thumbX - 3, ry + 3, thumbX + 3, ry + 14, colThumb);
        } else {
            float vw = ov.measureText(value, 13);
            ov.drawText((float)panelW - 12 - vw, ry, value, i == cursor ? colAcc : color, 13);
        }
    }

    int fy = kMenuHeaderH + M_COUNT * kMenuRowH + 4;
    swprintf(buf, 80, L"状态  攻击:%s  放置:%s  %s",
             cs.combatReady ? (cs.canAttack ? L"可" : L"否") : L"未就绪",
             cs.combatReady ? (cs.canPlace ? L"可" : L"否") : L"未就绪",
             cs.running ? L"连点中" : L"已停止");
    ov.drawText(10, (float)fy, buf, colDim, 12);
}

static bool menu_need_render(int w, int h) {
    int px = 0, py = 0, pw = 0, ph = 0;
    menu_panel_rect(w, h, px, py, pw, ph);
    if (!g_menuCache.valid || g_menuCache.w != pw || g_menuCache.h != ph)
        return true;
    if (g_menuDirty.load(std::memory_order_acquire))
        return true;
    return (int)(GetTickCount() - g_menuLastRenderMs) >= 250;
}

static void render_menu_cache(int w, int h) {
    int px = 0, py = 0, pw = 0, ph = 0;
    menu_panel_rect(w, h, px, py, pw, ph);
    if (pw <= 0 || ph <= 0) return;

    if ((int)g_menuCache.px.size() != pw * ph) {
        g_menuCache.px.assign((size_t)pw * ph, 0);
        g_menuCache.valid = false;
    }
    g_menuCache.w = pw;
    g_menuCache.h = ph;

    if (g_overlay.begin_offscreen(pw, ph, g_menuCache.px.data())) {
        draw_menu_panel(g_overlay, pw, ph);
        g_overlay.end_offscreen();
        g_menuCache.valid = true;
    }
    g_menuDirty.store(false, std::memory_order_release);
    g_menuLastRenderMs = GetTickCount();
}

static void blit_menu_cache(int w, int h) {
    if (!g_menuCache.valid) return;
    int px = 0, py = 0, pw = 0, ph = 0;
    menu_panel_rect(w, h, px, py, pw, ph);
    if (pw != g_menuCache.w || ph != g_menuCache.h) return;
    uint32_t* dst = g_overlay.lockNoClear(w, h);
    if (!dst) return;
    for (int y = 0; y < ph; ++y) {
        memcpy(dst + (size_t)(py + y) * w + px,
               g_menuCache.px.data() + (size_t)y * pw,
               (size_t)pw * sizeof(uint32_t));
    }
}

// ---- 方案 B：在调用线程（游戏渲染线程）内直接绘制覆盖层 ----
// ESP 仍逐帧重绘；菜单使用离屏缓存，静态时每帧只 memcpy 面板区域。
static void esp_draw_overlay() {
    // 先取预投影数据（g_dataMutex 不嵌套在 g_overlayLock 内，避免锁序反转）
    std::vector<ScreenBox> boxes;
    std::vector<TrajectoryData> trajs;
    {
        std::lock_guard<std::mutex> lock(g_dataMutex);
        boxes = g_boxes;
        trajs = g_trajectories;
    }

    std::lock_guard<std::mutex> lock(g_overlayLock);
    if (!g_running) return;

    bool espOn = g_espEnabled.load(std::memory_order_acquire);
    bool menuOpen = g_menuVisible.load(std::memory_order_acquire);
    if (!espOn && !menuOpen) return;

    int w = 0, h = 0;
    if (!g_overlay.lockNoClear(w, h) || w <= 0 || h <= 0) return;

    bool renderMenu = menuOpen && menu_need_render(w, h);

    // ESP 开启或菜单缓存需要重绘时才整屏清空；静态菜单直接复用上一帧缓冲。
    if (espOn || renderMenu) {
        g_overlay.lock(w, h);
    }

    int lw = std::max(1, g_cfg.lineWidth - 1);
    if (espOn) {
        for (const auto& b : boxes) {
            for (int i = 0; i < b.segCount; ++i)
                g_overlay.drawLine(b.segs[i].x0, b.segs[i].y0,
                                   b.segs[i].x1, b.segs[i].y1, b.color, lw);
            if (b.hasFill) g_overlay.fillRect(b.minX, b.minY, b.maxX, b.maxY, b.color);
            if (!b.name.empty()) {
                float tw = g_overlay.measureText(b.name, 14);
                g_overlay.drawText(b.nameX - tw * 0.5f, b.nameY, b.name, b.color, 14);
            }
            if (b.hasTracer)
                g_overlay.drawLine((float)(w / 2), (float)h, b.tx, b.ty, b.color, 1);
        }

        for (const auto& t : trajs) {
            for (int i = 0; i < t.segCount; ++i)
                g_overlay.drawLine(t.segs[i].x0, t.segs[i].y0,
                                   t.segs[i].x1, t.segs[i].y1, t.color, lw);
            if (t.hasLanding) {
                for (int i = 0; i < t.cubeFacesCount; ++i) {
                    const auto& q = t.cubeFaces[i];
                    g_overlay.fillPoly(q.x, q.y, 4, t.landColor, 0.5f);
                }
                for (int i = 0; i < t.cubeCount; ++i)
                    g_overlay.drawLine(t.cube[i].x0, t.cube[i].y0,
                                       t.cube[i].x1, t.cube[i].y1, t.landColor, 1);
            }
        }
    }

    if (espOn && g_cfg.nameTags) {
        ClickerSnapshot cs = clicker_snapshot();
        wchar_t status[96];
        swprintf(status, 96, L"ESP ON   CLICKER %s   ATK:%s   PLACE:%s",
                 cs.running ? L"ON" : L"OFF",
                 cs.combatReady ? (cs.canAttack ? L"可" : L"否") : L"-",
                 cs.combatReady ? (cs.canPlace ? L"可" : L"否") : L"-");
        float sw = g_overlay.measureText(status, 14);
        g_overlay.drawText((float)w - sw - 10, (float)h - 22, status, g_cfg.colHud, 14);
    }

    if (renderMenu) render_menu_cache(w, h);
    if (menuOpen) blit_menu_cache(w, h);

    g_overlay.present();
}

// ---- SwapBuffers 钩子回调（游戏渲染线程） ----
// glrender 钩住 gdi32!SwapBuffers，游戏每帧在其渲染线程调用本函数。
// 用 GetEnv 复用该线程已有 JNIEnv（不新建线程、不 AttachCurrentThread），
// 采集相机 + 实体 + 连点器门控状态，并在本线程直接绘制覆盖层。
void esp_on_swap() {
    if (!g_running) return;
    bool enabled = g_espEnabled.load(std::memory_order_acquire);
    bool menuOpen = g_menuVisible.load(std::memory_order_acquire);

    // 空闲优化：ESP 关、菜单关、攻击/放置门控都关时，完全不碰 JVM，
    // SwapBuffers 钩子只做一次原子判断就返回。
    ClickerSnapshot idleSnap = clicker_snapshot();
    bool needCombat = menuOpen || idleSnap.settings.attackGate || idleSnap.settings.placeGate;
    if (!enabled && !needCombat) {
        // 空闲预热：每 1s 尝试一次符号解析。这样第一次按 Insert 打开菜单时
        // JNI 符号早已就绪，不会把解析开销压在开菜单那一帧上。
        if (!jvm_ready()) {
            static DWORD nextWarmup = 0;
            DWORD nowT = GetTickCount();
            if ((int)(nowT - nextWarmup) >= 0) {
                nextWarmup = nowT + 1000;
                jvm_hook_begin();
            }
        }
        return;
    }

    // 首次在此线程解析符号，之后每帧直接复用 JNIEnv
    if (!jvm_hook_begin()) return;
    // 符号未全部解析成功时跳过本帧，避免用空方法 ID 调用 JNI 崩溃
    if (!jvm_ready()) return;

    CamData cam = jvm_read_camera();
    if (!cam.ok) return;

    // 聊天界面（按 T 打开）：keepOnChat 配置决定是否仍渲染。
    if (cam.guiOpen && cam.screenIsChat && g_cfg.keepOnChat)
        cam.guiOpen = false;

    // ---- 连点器门控状态（5ms 节流，与参考 DLL 采样周期一致）----
    // 仅在菜单打开或门控开启时读取，未开启门控时 zero JNI 开销。
    {
        ClickerSnapshot cs = clicker_snapshot();
        bool needCombat = menuOpen || cs.settings.attackGate || cs.settings.placeGate;
        if (needCombat && !cam.guiOpen) {
            static DWORD lastCombatTick = 0;
            DWORD nowT = GetTickCount();
            if ((int)(nowT - lastCombatTick) >= 5) {
                lastCombatTick = nowT;
                CombatStatus st = jvm_read_combat_status();
                clicker_set_combat(st.ok, st.ok && st.canAttack, st.ok && st.canPlace);
            }
        }
    }

    std::vector<EntityData> entities;
    std::vector<ScreenBox> boxes;
    std::vector<TrajectoryData> trajs;
    // 仅 ESP 启用时采集实体；连点器门控由 jvm_read_combat_status 单独完成。
    if (enabled && !cam.guiOpen) {
        jvm_collect_entities(cam.px, cam.py, cam.pz, cam.partialTick, entities);
        // 帧间轻微平滑：抹平 20Hz tick 边界折角，框更丝滑（时间常数 smoothMs）
        smooth_entities(entities, g_cfg.smoothMs);
        // 在游戏线程内完成 3D→屏幕投影（同一帧相机/位置），box 与画面同步
        int w = 0, h = 0;
        if (g_gameHwnd) {
            RECT rc;
            GetClientRect(g_gameHwnd, &rc);
            w = rc.right; h = rc.bottom;
        }
        if (w > 0 && h > 0) {
            project_entities(cam, entities, w, h, boxes);
            // 弹射物轨迹预测（同一帧相机/位置/速度），生成屏幕折线
            project_trajectories(cam, entities, w, h, trajs);
            // 弓蓄力预判：本地玩家拉弓预测本次发射轨迹（追加到 trajs）
            PlayerInfo lp = jvm_read_player();
            project_bow_predict(cam, lp, entities, w, h, trajs);
            // 其他玩家弓蓄力预判：渲染每个正在拉弓的玩家的抛物线
            project_other_bow_predicts(cam, entities, w, h, trajs);
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_dataMutex);
        g_cam = cam;
        g_entities.swap(entities);
        g_boxes.swap(boxes);
        g_trajectories.swap(trajs);
        ++g_dataSeq;
    }
    // ESP 或菜单任一需要显示时，在游戏渲染线程同步绘制（零延迟贴合）。
    if ((enabled || menuOpen) &&
        g_overlayVisible.load(std::memory_order_acquire) && !cam.guiOpen)
        esp_draw_overlay();
}

// ---- 渲染线程（覆盖层宿主 + 菜单键盘处理） ----
// 绘制已迁移到游戏渲染线程（esp_on_swap → esp_draw_overlay）。
// 本线程负责：创建覆盖层窗口、泵消息、Insert 菜单输入、可见性管理。
static void render_loop(HWND gameHwnd) {
    if (!g_overlay.create(gameHwnd)) return;
    esp_log("[render] 覆盖层宿主启动（绘制在游戏线程，方案 B 零延迟同步）");

    // 高精度定时（1ms 级 sleep），消除 15.6ms 系统时钟跳变导致的帧率抖动
    timeBeginPeriod(1);
    // 渲染线程实际按 esp.ini 的 renderHz 运行（默认 120Hz = ~8ms），
    // 菜单输入/鼠标/可见性响应不再被固定 16ms 限制。
    int hostHz = g_cfg.renderHz;
    if (hostHz < 30) hostHz = 30;
    if (hostHz > 250) hostHz = 250;
    const int hostPeriodMs = 1000 / hostHz;

    // Win10 1803+ 高精度可等待定时器：Sleep(8) 仍可能被调度器按 15.6ms
    // 节拍合并，导致菜单实际只有 ~60Hz；改用 CREATE_WAITABLE_TIMER_HIGH_RESOLUTION。
    typedef HANDLE(WINAPI* CreateWaitableTimerExW_t)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
    HANDLE hHostTimer = nullptr;
    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    auto pCreateWaitableTimerExW = hK32 ? (CreateWaitableTimerExW_t)GetProcAddress(hK32, "CreateWaitableTimerExW") : nullptr;
    if (pCreateWaitableTimerExW)
        hHostTimer = pCreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    if (!hHostTimer)
        hHostTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

    // 界面打开防抖计数：连续 N 帧确认 guiOpen 才隐藏覆盖层，
    // 避免 mc.screen 瞬时非空（mod 弹窗等）导致整层 60Hz 闪烁。
    int guiOpenStreak = 0;
    // 覆盖层当前可见性（本线程内的局部缓存，只在状态翻转时操作窗口）
    bool overlayVisible = false;
    // 菜单打开时覆盖层需要截获鼠标；关闭时恢复鼠标穿透
    bool overlayClickable = false;

    while (g_running) {
        // 心跳：确认宿主线程持续存活（每 ~1s 一次），用于定位死亡点
        {
            static DWORD hbTick = 0; static DWORD hbFrame = 0;
            DWORD nowT = GetTickCount();
            hbFrame++;
            if (nowT - hbTick >= 1000) {
                esp_log("[render] 心跳 存活 tick=%u, frames=%lu", nowT, hbFrame);
                hbTick = nowT;
                hbFrame = 0;
            }
        }

        // 泵出覆盖层窗口的消息，避免游戏主线程对覆盖层 SendMessage 永久阻塞。
        MSG msg;
        while (PeekMessageW(&msg, g_overlay.hwnd(), 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // Insert 呼出/关闭菜单 + 菜单内键盘/鼠标操作
        menu_handle_input(gameHwnd);

        bool menuOpen = g_menuVisible.load(std::memory_order_acquire);
        clicker_set_menu_open(menuOpen);

        // 菜单打开时覆盖层从鼠标穿透切换为可点击（仍不抢游戏焦点）
        if (menuOpen != overlayClickable) {
            std::lock_guard<std::mutex> lock(g_overlayLock);
            g_overlay.set_clickable(menuOpen);
            overlayClickable = menuOpen;
            esp_log("[menu] 鼠标%s", menuOpen ? "捕获（菜单打开，连点已暂停）" : "穿透（菜单关闭）");
        }

        // ============ 统一可见性状态管理 ============
        // ESP 或菜单任一开启就显示覆盖层；状态翻转才操作窗口。
        bool wantVisible = g_espEnabled.load(std::memory_order_acquire) || menuOpen;

        // 界面检测：相机/界面状态由 SwapBuffers 钩子在游戏渲染线程每帧更新。
        if (wantVisible) {
            bool guiOpen = false;
            {
                std::lock_guard<std::mutex> lock(g_dataMutex);
                guiOpen = g_cam.ok && g_cam.guiOpen;
            }
            if (guiOpen) {
                if (++guiOpenStreak >= 3) {
                    wantVisible = false;
                } else {
                    // 防抖未确认：保持当前可见性，跳过本帧
                    std::this_thread::sleep_for(std::chrono::milliseconds(4));
                    continue;
                }
            } else {
                guiOpenStreak = 0;
            }
        }

        // 游戏最小化
        if (wantVisible && IsIconic(gameHwnd)) wantVisible = false;

        // 游戏失焦（esp.ini onlyWhenFocused）。前台判定优先严格比较游戏窗口；
        // 若前台句柄瞬时指向同进程其它窗口，用“前台属于本进程”兜底。
        if (wantVisible && g_cfg.onlyWhenFocused) {
            bool focused = (GetForegroundWindow() == gameHwnd);
            if (!focused) {
                DWORD fgPid = 0;
                HWND fg = GetForegroundWindow();
                if (fg) GetWindowThreadProcessId(fg, &fgPid);
                focused = (fgPid == GetCurrentProcessId());
            }
            if (!focused) wantVisible = false;
        }

        // 状态翻转时才操作窗口；已可见时每帧仅重排对齐。
        {
            std::lock_guard<std::mutex> lock(g_overlayLock);
            if (wantVisible) {
                if (!g_overlay.position(gameHwnd)) {
                    if (overlayVisible) { ShowWindow(g_overlay.hwnd(), SW_HIDE); overlayVisible = false; }
                    g_overlayVisible.store(false, std::memory_order_release);
                } else {
                    if (!overlayVisible) {
                        overlayVisible = true;   // position() 内部已 ShowWindow
                        esp_log("[render] 覆盖层显示");
                    }
                    g_overlayVisible.store(true, std::memory_order_release);
                }
            } else if (overlayVisible) {
                ShowWindow(g_overlay.hwnd(), SW_HIDE);
                overlayVisible = false;
                g_overlayVisible.store(false, std::memory_order_release);
                esp_log("[render] 覆盖层隐藏");
            }
        }

        // 相对周期唤醒；负 100ns 单位表示相对时间
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)hostPeriodMs * 10000LL;
        SetWaitableTimer(hHostTimer, &due, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(hHostTimer, INFINITE);
    }
    if (hHostTimer) CloseHandle(hHostTimer);
    g_overlayVisible.store(false, std::memory_order_release);
    timeEndPeriod(1);
}

// ---- 入口 ----
extern "C" __declspec(dllexport) DWORD WINAPI esp_thread_main(LPVOID) {
    esp_log("[ESP] 线程启动");

    // 立即初始化，不延迟：注入后马上加载配置、找窗口、装钩子、渲染。
    // 手动映射 + GetEnv（不新建线程/不 AttachCurrentThread）已规避注入检测，
    // 无需再靠延迟启动避扫描窗口。
    config_load(g_cfg);
    g_espEnabled = g_cfg.enabled;
    clicker_apply_settings(g_cfg.clicker);
    clicker_set_settings_changed_callback(on_clicker_hotkey_changed);
    clicker_set_running(g_cfg.clicker.enabled);

    HWND gameHwnd = nullptr;
    for (int i = 0; i < 300; ++i) {
        if (g_stop) { esp_log("[ESP] 等待窗口期间被停止"); return 1; }
        gameHwnd = find_game_window();
        if (gameHwnd) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!gameHwnd) { esp_log("[ESP] 等待游戏窗口超时"); return 1; }
    esp_log("[ESP] 找到游戏窗口: %p", gameHwnd);
    g_gameHwnd = gameHwnd;   // 供 SwapBuffers 钩子内投影取屏幕尺寸

    g_running = true;

    // 连点线程：不接触 JVM、不 AttachCurrentThread；仅向本进程游戏窗口 PostMessage。
    clicker_start(gameHwnd);

    // 安装 SwapBuffers 钩子：在游戏渲染线程内复用已有 JNIEnv 采集实体数据
    // 与连点器门控状态（canAttack / canPlace），不再创建外部消息通道。
    gl_install_hook();

    // 渲染线程（覆盖层宿主 + Insert 菜单输入）：创建/定位/显隐覆盖层窗口 + 泵消息，
    // 不接触 JVM；绘制由游戏线程在 SwapBuffers 钩子内完成。
    render_loop(gameHwnd);

    // 停止：先停连点线程，再还原 SwapBuffers 钩子（阻止游戏渲染线程继续进入本模块）
    g_running = false;
    clicker_stop();
    gl_remove_hook();
    esp_log("[ESP] 线程退出（保留覆盖层窗口，避免卸载期 DestroyWindow 竞态崩溃）");
    return 0;
}

extern "C" __declspec(dllexport) void esp_stop() {
    g_running = false;
    g_stop = true;
}