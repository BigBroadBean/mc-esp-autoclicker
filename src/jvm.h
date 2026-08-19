#pragma once
// ============================================================
//  jvm.h — JVM 附加 + SRG 混淆名称解析 + MC 数据访问
// ============================================================
#include <jni.h>
#include <cstdint>
#include <string>
#include <vector>
#include <array>

// 在“游戏渲染线程”（SwapBuffers 钩子）内复用已有 JNIEnv：
// GetCreatedVMs + GetEnv，不新建线程、不 AttachCurrentThread。
// 首次调用时完成符号解析。成功返回 true。
bool jvm_hook_begin();

// 是否已完成符号解析。渲染线程仅在就绪后读取实时相机，
// 否则 jvm_read_camera 会用空方法 ID 崩溃。
bool jvm_ready();

// 解析全部需要的类/字段/方法（处理 Forge SRG 混淆）。成功返回 true。
bool jvm_resolve_all();

// 释放全局引用（卸载/退出时）。
void jvm_cleanup();

// ---------- 每帧读取 ----------
struct CamData {
    double  px, py, pz;   // 相机世界坐标
    float   yaw, pitch;   // 角度
    float   fov;          // 透视 fov（垂直，来自游戏投影矩阵）
    float   mouseSensitivity; // Options.sensitivity 原始值（通常 0..1）
    bool    sensitivityValid;
    bool    invertY;      // Options.invertYMouse
    float   partialTick;  // 帧间插值系数
    bool    guiOpen;      // 有界面打开（Esc/背包/聊天等，mc.screen != null）→ 默认不渲染 ESP
    bool    screenIsChat; // 当前打开的是否为聊天界面（ChatScreen，按 T 打开）
    bool    ok;
};

// 连点器门控状态（在游戏渲染线程内直接读取，不再向外部程序发送消息）
struct CombatStatus {
    bool    ok = false;          // JNI 读取成功
    bool    inGame = false;      // 已进入游戏（player != null）
    bool    canAttack = false;   // 准星目标可攻击（逻辑与 MCCombatStatusJni 一致）
    bool    canPlace = false;    // 手持物品为方块类放置物（BlockItem）
    int     hitType = 0;         // 0=未命中 1=方块 2=实体
    bool    targetLiving = false;
    bool    targetAlive = false;
    bool    targetAttackable = false;
};

// 弹射物类型（用于轨迹渲染的物理参数选择）
enum ProjectileType {
    PROJ_NONE = 0,
    PROJ_ARROW = 1,     // 弓：箭 / 光灵箭
    PROJ_SNOWBALL = 2,  // 雪球
    PROJ_PEARL = 3,     // 末影珍珠
};

struct EntityData {
    int     id = 0;           // 实体网络 ID（用于跨快照匹配/外推）
    double  ix, iy, iz;       // 平滑后的位置（ESP 渲染使用）
    double  rx, ry, rz;       // 平滑前的原始插值位置（保留，供后续扩展使用）
    float   bbw, bbh;         // 包围盒宽高
    double  dist;             // 到相机距离
    std::wstring name;        // 名字（玩家名/实体类型名）
    bool    isPlayer;
    bool    isLiving;
    // 其他玩家弓蓄力预判（仅玩家实体、且正在拉弓时有效）
    bool    chargingBow = false;   // 正在使用弓蓄力
    int     useTicks = 0;          // 已蓄力 tick 数
    float   yaw = 0, pitch = 0;    // 该玩家朝向（度）
    bool    onGround = false;      // 蓄力玩家是否在地面（决定箭是否继承垂直动量）
    // 弹射物轨迹（仅 projType != PROJ_NONE 时有效）
    int     projType = PROJ_NONE;   // 弹射物类型
    double  vx = 0, vy = 0, vz = 0; // 当前速度（blocks/tick，来自 getDeltaMovement）
    bool    hasVelocity = false;    // 速度字段是否有效（自瞄预判使用）
    bool    ownProjectile = false;  // 是否本地玩家发射的弹射物（不渲染轨迹，由弓预判覆盖）
    // 自瞄目标选择（仅 needAimData 时读取，避免每帧对全部实体做额外 JNI）
    float   health = 0.0f;          // 当前生命值
    float   maxHealth = 1.0f;       // 最大生命值
    bool    healthValid = false;    // 是否成功读到生命值
};

// 游戏线程（SwapBuffers 钩子）内预投影好的屏幕坐标实体——渲染线程只画、不投影。
// 投影在游戏画面同一帧、用同一相机/位置完成，box 与画面精确同步（等价于
// LiquidBounce 在游戏渲染管线内用同一数据绘制，零延迟贴合）。
struct ScreenSeg { float x0, y0, x1, y1; };   // 一条 3D 边的屏幕线段（已近平面裁剪）
struct ScreenQuad { float x[4], y[4]; };      // 一个 3D 面的屏幕四边形（已近平面裁剪）
struct ScreenBox {
    uint32_t color = 0;
    std::array<ScreenSeg, 12> segs;   // 3D 盒 12 条边
    int  segCount = 0;                // 有效边数（box3d 关闭时为 0）
    std::wstring name;                // 名字（可为空）
    float nameX = 0, nameY = 0;       // 名字锚点（中心 X / 顶部 Y，渲染线程按宽度居中）
    float tx = 0, ty = 0;             // tracer 目标点
    bool  hasTracer = false;
    float minX = 0, minY = 0, maxX = 0, maxY = 0;   // 2D 极值（填充/名字定位）
    bool  hasFill = false;            // filledBox
    bool  has2d   = false;            // box2d（屏幕矩形外框）
};

// 弹射物轨迹屏幕折线（游戏线程预投影，渲染线程直接画）
struct TrajectoryData {
    uint32_t color = 0;
    std::array<ScreenSeg, 64> segs;   // 轨迹折线段（已近平面裁剪）
    int  segCount = 0;
    bool hasLanding = false;          // 是否标记落点
    float lx = 0, ly = 0;             // 落点屏幕坐标
    uint32_t landColor = 0;           // 落点方块颜色（蓝=方块落点/障碍物，红=命中实体）
    std::array<ScreenSeg, 12> cube;   // 落点 3D 立方体 12 条边（屏幕坐标，近平面裁剪）
    int  cubeCount = 0;
    std::array<ScreenQuad, 6> cubeFaces;  // 落点 3D 立方体 6 个面（屏幕四边形，50% 透明平面）
    int  cubeFacesCount = 0;
};

// 本地玩家信息（弓蓄力预判轨迹用）
struct PlayerInfo {
    bool ok = false;
    double px, py, pz;      // 玩家位置（脚底）
    float  eyeHeight;       // 眼睛高度
    float  yaw, pitch;      // 玩家朝向（度）
    bool   chargingBow = false;   // 正在拉弓蓄力
    int    useTicks = 0;          // 已蓄力 tick 数
    double vx = 0, vy = 0, vz = 0; // 玩家当前速度（blocks/tick，动量继承）
    bool   onGround = false;       // 玩家是否在地面（决定垂直动量是否继承）
};

// 返回当前相机数据；失败 ok=false。
CamData jvm_read_camera();

// 返回本地玩家信息（弓蓄力预判轨迹用）；失败 ok=false。
PlayerInfo jvm_read_player();

// 读取连点器门控状态（准星目标可攻击 / 手持方块）。失败 ok=false。
// 判定逻辑移植自 MCCombatStatusJni.cpp 的 UpdateStatus()，但只在本进程内使用。
CombatStatus jvm_read_combat_status();

// 方块射线检测：对线段 [from,to] 用 level.clip(ClipContext) 做碰撞检测（仿 LiquidBounce）。
// 命中则 outHit=true 并输出命中点；否则 outHit=false。符号未解析/出错返回 false。
bool jvm_clip_block(double x0, double y0, double z0,
                    double x1, double y1, double z1,
                    double& hx, double& hy, double& hz, bool& outHit);

// 采集 level 中的所有实体（按 config 过滤）。返回收集到的数量。
// needAimData=true 时额外读取生物生命值（跳过死亡目标 / 血量排序）；
// needAimVelocity=true 时再读取生物速度（自瞄预判）。
// 需先调用 jvm_attach / jvm_resolve_all。
int jvm_collect_entities(double camX, double camY, double camZ, float partialTick,
                         bool needAimData, bool needAimVelocity,
                         std::vector<EntityData>& out);
