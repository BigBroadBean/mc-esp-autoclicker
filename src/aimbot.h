#pragma once
// ============================================================
//  aimbot.h — 角度域目标锁定 + 250Hz 鼠标辅助
//
//  游戏渲染线程完成候选排序、遮挡检测、碰撞箱落点和角度误差发布；
//  独立移动线程读取游戏真实灵敏度，把角度误差换成原生鼠标计数。
//  获取/释放 FOV、遮挡宽限、二段落点、收敛死区与手动让权共同保证
//  “能跟上但不抢手”，且手感不随分辨率、画面 FOV 或帧率漂移。
// ============================================================
#include "common.h"
#include "jvm.h"

#include <windows.h>
#include <atomic>
#include <vector>

struct AimTarget {
    bool    valid = false;
    int     entityId = -1;
    float   sx = 0.0f, sy = 0.0f;   // 最终落点（游戏客户区坐标，仅绘制使用）
    float   yawError = 0.0f;        // 相对当前相机的水平角误差（度）
    float   pitchError = 0.0f;      // 相对当前相机的垂直角误差（度）
    float   cameraYaw = 0.0f;
    float   cameraPitch = 0.0f;
    float   cameraFov = 70.0f;
    float   gameSensitivity = 0.5f; // 游戏真实灵敏度（Options.sensitivity）
    bool    sensitivityValid = false;
    bool    invertY = false;
    bool    locked = false;         // 准星已进入碰撞箱（带短时退出滞回）
    bool    occluded = false;       // 当前目标处于遮挡宽限；此时保留目标但不移动
    bool    fovGrace = false;        // 当前目标短暂越过释放 FOV；此时保留目标但不移动
    float   secondProgress = 0.0f;  // 进盒点→第二落点过渡进度（0..1）
    float   fovRadiusPx = 0.0f;     // 自瞄 FOV 圈半径（用于绘制）
    int     screenW = 0, screenH = 0;
    DWORD   frameMs = 0;            // 发布时刻（GetTickCount）
    uint32_t gen = 0;               // 渲染线程每发布一次 +1，移动线程重置待处理误差
    float   assistMul = 1.0f;       // 释放 FOV / 进盒 / 宽限状态的辅助倍率
    // 可视化附加信息
    std::wstring name;
    float   health = 0.0f, maxHealth = 1.0f;
    bool    healthValid = false;
};

void aimbot_apply_settings(const AimSettings& s);
void aimbot_start(HWND gameHwnd);
void aimbot_stop();

// 触发条件满足且允许移动（前台、非菜单、光标隐藏）。
bool aimbot_active();
// 当前触发状态下是否已有目标 / 准星是否已进入其碰撞箱（HUD 使用）。
bool aimbot_has_target();
bool aimbot_target_locked();
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
