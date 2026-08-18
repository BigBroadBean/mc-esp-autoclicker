#pragma once
// ============================================================
//  aimbot.h — 自瞄（人类化鼠标移动 + 碰撞箱锁定策略）
//
//  目标点算法：
//    - 准星射线未命中目标碰撞箱：瞄准离射线最近的碰撞箱表面点，
//      把视野“拉”进碰撞箱。
//    - 已命中碰撞箱：瞄准目标水平中心、高度钳制到本地玩家视平线高度，
//      防止锁头/锁脚导致准星滑出碰撞箱。
//  鼠标移动：
//    - 独立 250Hz 线程 SendInput 相对移动；
//    - 加速启动、接近目标减速、轻微噪声，不做瞬时锁死。
// ============================================================
#include "common.h"
#include "jvm.h"

#include <windows.h>
#include <atomic>
#include <vector>

struct AimTarget {
    bool    valid = false;
    int     entityId = -1;
    float   sx = 0.0f, sy = 0.0f;   // 目标应移动到的屏幕点（游戏客户区坐标）
    bool    locked = false;         // 准星当前是否已在目标碰撞箱内
    float   secondProgress = 0.0f;  // 第一目标→第二目标过渡进度（0..1，未锁定时为 0）
    float   fovRadiusPx = 0.0f;     // 自瞄 FOV 圈半径（用于绘制）
    int     screenW = 0, screenH = 0;
    DWORD   frameMs = 0;            // 发布时刻（GetTickCount）
    // 可视化附加信息
    std::wstring name;              // 目标名字
    float   health = 0.0f, maxHealth = 1.0f;
    bool    healthValid = false;
};

void aimbot_apply_settings(const AimSettings& s);
void aimbot_start(HWND gameHwnd);
void aimbot_stop();

// 触发条件满足且允许移动（前台、非菜单、光标隐藏）。
bool aimbot_active();
// 是否需要显示自瞄可视化（覆盖层可见性判断用）。
bool aimbot_visual_wanted();

void aimbot_set_menu_open(bool open);

// 由渲染线程热键路径同步触发状态（比后台线程轮询 GetAsyncKeyState 更可靠）。
void aimbot_set_hotkey_down(bool down);
void aimbot_toggle_trigger();
void aimbot_set_toggle_on(bool on);
bool aimbot_toggle_on();

// 游戏渲染线程每帧调用：选择目标并计算屏幕瞄准点。
void aimbot_update_target(const CamData& cam, int screenW, int screenH,
                          const std::vector<EntityData>& entities,
                          const AimSettings& cfg);

// 供移动线程 / ESP 绘制读取最新目标。
bool aimbot_get_target(AimTarget& out);
void aimbot_clear_target();
