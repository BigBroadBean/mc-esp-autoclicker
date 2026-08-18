#pragma once
// ============================================================
//  common.h — 公共基础模块：宽路径、日志、配置
//  JNI ESP for Minecraft 1.20.1 Forge 47.4.10
// ============================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include <array>
#include <algorithm>

// ------------------------------------------------------------
// 路径工具（游戏/工程目录可能含中文，一律使用宽字符 API）
// ------------------------------------------------------------
// 数据目录（esp.ini / esp_log.txt）：
//   1) 加载器通过内存映射注入的自定义目录（mc_esp.exe -dir <path>）
//   2) 环境变量 MC_ESP_DATA_DIR
//   3) 默认 %APPDATA%\mc_esp\（通常为 C:\Users\<用户>\AppData\Roaming\mc_esp）
// 目录不存在时自动创建。
std::wstring data_directory();
// 由 DllMain 在读取加载器目录映射后调用，提前固定数据目录。
void data_set_directory(const wchar_t* dir);

void esp_log(const char* fmt, ...);
void esp_log_w(const wchar_t* fmt, ...);

// ------------------------------------------------------------
// 连点器配置（与 AutoClicker 同参数；已去除多倍点击/滚轮点击）
// ------------------------------------------------------------
struct ClickerSettings {
    bool    enabled          = false;       // 注入后初始是否启用连点
    int     toggleKey        = VK_MBUTTON;  // 启停连点热键（参考实现默认鼠标中键）
    bool    leftEnabled      = true;        // 左键连点（默认启用，等待连点总开关）
    bool    rightEnabled     = true;        // 右键连点（默认启用，等待连点总开关）
    bool    keep             = false;       // 保持模式：无需按住鼠标即可连点
    int     cpsLeft10        = 100;         // 左键 CPS * 10（100 = 10.0 CPS）
    int     cpsRight10       = 100;         // 右键 CPS * 10
    int     cpsMax           = 50;          // CPS 上限（20..500）
    bool    randomEnabled    = false;       // 随机 CPS 波动
    int     randomRange      = 2;           // 随机波动 ±CPS（1..5）
    int     humanizeMode     = 0;           // 0=均匀 1=双击连招 2=呼吸波动 3=疲劳递减
    int     humanizeLevel    = 3;           // 拟人化强度（1..5）
    bool    autoStopEnabled  = false;       // 定时自动停止
    int     autoStopSeconds  = 30;          // N 秒后自动停止
    bool    attackGate       = false;       // 仅准星目标可攻击时左键连点（JNI 内部读取）
    int     attackGateKey    = VK_F6;       // 攻击门控热键
    bool    placeGate        = false;       // 仅手持方块类物品时右键连点（JNI 内部读取）
    int     placeGateKey     = VK_F7;       // 放置门控热键
    bool    cursorGate       = false;       // 光标门控：光标可见（背包/聊天/菜单）时暂停
    bool    inGameGate       = false;       // 游戏内门控：player==null（主菜单/加载中）时暂停连点
};

// ------------------------------------------------------------
// 自瞄配置
// ------------------------------------------------------------
enum AimTriggerMode {
    AIM_TRIGGER_HOLD_LMB = 0,   // 按住鼠标左键时瞄准
    AIM_TRIGGER_HOLD_RMB,       // 按住鼠标右键时瞄准
    AIM_TRIGGER_HOLD_KEY,       // 按住自瞄键时瞄准
    AIM_TRIGGER_TOGGLE,         // 自瞄键切换 开/关
    AIM_TRIGGER_ALWAYS,         // 始终瞄准
    AIM_TRIGGER_COUNT
};

enum AimPriority {
    AIM_PRIORITY_CROSSHAIR = 0, // 优先离准星最近的目标
    AIM_PRIORITY_DISTANCE,      // 优先最近的目标
    AIM_PRIORITY_HEALTH         // 优先血量最低的目标
};

enum AimSecondTarget {
    AIM_SECOND_LEVEL = 0,       // 放平：本地玩家视平线高度的水平中心
    AIM_SECOND_FIRST_HEIGHT,    // 不放平：第一目标点高度的水平中心
    AIM_SECOND_KEEP_NEAREST,    // 不切换：一直保持最近点
    AIM_SECOND_COUNT
};

struct AimSettings {
    bool    enabled          = false;       // 自瞄总开关（默认关闭）
    int     triggerMode      = AIM_TRIGGER_HOLD_LMB;   // 触发模式
    int     triggerKey       = VK_XBUTTON1; // 按住/切换自瞄的热键
    bool    aimPlayers       = true;        // 瞄准玩家
    bool    aimMobs          = true;        // 瞄准生物
    bool    aimOthers        = false;       // 瞄准其他实体
    int     priority         = AIM_PRIORITY_CROSSHAIR;  // 目标优先级
    float   fov              = 90.0f;       // 瞄准视野范围（全角，度）：准星周围 ±fov/2
    double  maxDistance      = 60.0;        // 最大瞄准距离（格）
    int     smooth           = 6;           // 平滑度 1..10（越大越像人手缓动）
    int     reactionMs       = 100;         // 锁定新目标后的反应延迟（毫秒）
    float   mouseSensitivity = 1.0f;        // 鼠标移动倍率 0.5..2.0
    int     predictionTicks  = 0;           // 预判目标移动 tick 数 0..20
    int     switchCooldownMs = 300;         // 切换目标冷却（毫秒）
    int     secondTarget     = AIM_SECOND_LEVEL;   // 第二目标模式（命中碰撞箱后）
    int     secondSmooth     = 5;           // 第一目标 → 第二目标的过渡平滑度 1..10
    int     stability        = 8;           // 对齐第二目标后的微小移动死区（像素）0..30
    int     visualMode       = 3;           // 0=不显示 1=目标点 2=+瞄准线 3=+FOV 圈
    bool    visibleOnly      = true;        // 仅瞄准视线可达目标（墙壁后不瞄）
};

// ------------------------------------------------------------
// 配置（esp.ini，UTF-8 / ANSI 均可）
// ------------------------------------------------------------
struct EspConfig {
    bool    enabled          = false;       // ESP 默认关闭，按 Insert 在菜单中开启
    int     menuKey          = VK_INSERT;   // Insert：呼出/关闭连点器菜单
    int     espKey           = VK_HOME;     // 快捷键直接开关 ESP（默认 HOME）
    double  maxDistance      = 200.0;
    double  fov              = 70.0;        // 游戏内 FOV（垂直，默认 70）
    bool    box3d            = true;        // 3D 立体包围盒（穿墙）
    bool    box2d            = false;       // 2D 屏幕矩形
    bool    nameTags         = true;        // 名字标签
    bool    tracer           = false;       // 射线（从屏幕底部到实体）
    bool    filledBox        = false;       // 半透明填充
    int     lineWidth        = 2;
    bool    showPlayers      = true;
    bool    showMobs         = true;
    bool    showOthers       = true;
    bool    onlyWhenFocused  = true;        // 游戏失去焦点时隐藏
    bool    keepOnChat       = true;        // 打开聊天（T）时继续渲染 ESP，不隐藏
    double  smoothMs         = 20.0;        // 盒子时域平滑时间常数（毫秒，轻微平滑，消除 tick 边界跳变）
    int     renderHz         = 120;         // 渲染线程刷新率（Hz）：越高盒子越丝滑（越接近碰撞箱）
    // 弹射物轨迹（弓/雪球/末影珍珠）
    bool    showTrajectory   = true;        // 渲染敌方弹射物的预测飞行轨迹折线
    int     trajectoryTicks  = 40;          // 轨迹预测长度（游戏 tick 数，20 tick = 1 秒）

    // 自瞄
    AimSettings aim;

    // 连点器
    ClickerSettings clicker;             // 当前激活方案（运行时使用）
    static constexpr int kClickerProfiles = 4;
    ClickerSettings profiles[4];          // 4 套配置方案（对应 AutoClicker 的方案槽）
    int             activeProfile = 0;    // 0..3
    int             profileKey    = VK_F8; // 循环切换 4 套连点方案的热键（全局，不属于方案本身）

    uint32_t colPlayer  = 0xFF5555;         // 玩家：红
    uint32_t colMob     = 0xFFAA00;         // 生物：橙
    uint32_t colOther   = 0x55FFFF;         // 其他：青
    uint32_t colHud     = 0xFFFFFF;
    uint32_t colTraj    = 0x00FF88;         // 弹射物轨迹：亮绿
    uint32_t colTrajOther = 0xFF0000;       // 其他玩家弓蓄力抛物线：红（区分本地玩家轨迹）
    uint32_t colLand    = 0x00A0FF;         // 弓预判落点方块：蓝半透明
    uint32_t colLandHit = 0xFF0000;         // 弓预判命中实体方块：红（红石）
    uint32_t colAim     = 0x00FFAA;         // 自瞄目标点（未锁定碰撞箱）
    uint32_t colAimLock = 0xFFFFFF;         // 自瞄目标点（已锁定碰撞箱）
};

// 从 data_directory() 读取 esp.ini；不存在时写入默认配置。
void config_load(EspConfig& cfg);
// 把当前配置写回 data_directory() 下的 esp.ini（菜单修改后立即持久化）。
void config_save(const EspConfig& cfg);

// ------------------------------------------------------------
// 反检测：伪装线程起点。
// 用 CreateThread(起点=ntdll!RtlUserThreadStart, 参数=真实函数) 创建线程，
// 使 KTHREAD.StartAddress（NtQueryInformationThread 读取）记录的是合法模块地址，
// 规避游戏“线程起点不在已加载模块内”的注入检测。真实函数需忽略其 LPVOID 参数。
// ------------------------------------------------------------
HANDLE spawn_hidden_thread(LPTHREAD_START_ROUTINE fn);
