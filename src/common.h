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
// 返回 DLL 自身所在目录（结尾带 '\\'），失败返回 L""。
std::wstring dll_directory();
// 在 DLL 入口处（做 PEB 模块隐藏前）显式记录 DLL 目录，
// 隐藏后 GetModuleHandleExW 找不到本模块，必须用缓存路径。
void dll_set_directory(const wchar_t* dir);

// ------------------------------------------------------------
// 日志：写入 dll_directory() 目录下的 esp_log.txt（单文件 EXE 模式即 exe 目录）
// ------------------------------------------------------------
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
};

// 从 dll_directory() 读取 esp.ini；不存在时写入默认配置（单文件 EXE 模式即 exe 目录）。
void config_load(EspConfig& cfg);
// 把当前配置写回 dll_directory() 下的 esp.ini（菜单修改后立即持久化）。
void config_save(const EspConfig& cfg);

// ------------------------------------------------------------
// 反检测：伪装线程起点。
// 用 CreateThread(起点=ntdll!RtlUserThreadStart, 参数=真实函数) 创建线程，
// 使 KTHREAD.StartAddress（NtQueryInformationThread 读取）记录的是合法模块地址，
// 规避游戏“线程起点不在已加载模块内”的注入检测。真实函数需忽略其 LPVOID 参数。
// ------------------------------------------------------------
HANDLE spawn_hidden_thread(LPTHREAD_START_ROUTINE fn);
