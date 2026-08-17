#pragma once
// ============================================================
//  clicker.h — 连点器核心（注入 DLL 内直接 PostMessage 到游戏窗口）
//
//  移植自 AutoClicker-main 的 ClickerThreadProc，并按需求：
//    - 去掉多倍点击（multi-click）
//    - 去掉滚轮点击（scroll-to-click）
//    - 去掉外部程序消息传递（UDP / 共享内存）
//  攻击/放置门控状态由 jvm_read_combat_status() 每帧直接写入本模块，
//  与 MCCombatStatusJni 的判定逻辑一致，但不再发往进程外。
// ============================================================
#include "common.h"

#include <windows.h>
#include <cstdint>
#include <string>
#include <atomic>

// 只读快照：菜单绘制用
struct ClickerSnapshot {
    ClickerSettings settings;
    bool    running = false;
    bool    combatReady = false;   // JNI 战斗状态已成功读取
    bool    canAttack = false;
    bool    canPlace  = false;
    int     realtimeCps = 0;
    long long clickCount = 0;
};

// 用一组设置覆盖连点器运行参数（菜单/配置修改后调用）。
void clicker_apply_settings(const ClickerSettings& s);

// 获取当前设置与实时状态的只读快照。
ClickerSnapshot clicker_snapshot();

// 启动连点线程（幂等）。线程用 spawn_hidden_thread 伪装起点，不接触 JVM。
void clicker_start(HWND gameHwnd);

// 通知连点线程退出（DLL 卸载前）。
void clicker_stop();

// 设置/翻转连点总开关（菜单与热键共用）。
void clicker_set_running(bool on);
void clicker_toggle_running();

// 连点线程热键修改设置后回调（用于写回 esp.ini）。回调在锁外执行。
void clicker_set_settings_changed_callback(void (*fn)());

// 热键开关后的右下角悬浮提示回调。kind: 0=连点器 1=攻击门控 2=放置门控。
void clicker_set_hotkey_toast_callback(void (*fn)(int kind, bool on));

// 游戏渲染线程每帧调用：写入 JNI 战斗状态（无外部消息传递）。
void clicker_set_combat(bool ready, bool canAttack, bool canPlace);

// 菜单打开时暂停连点（防止点菜单时同时向游戏窗口 PostMessage）。
void clicker_set_menu_open(bool open);

// 记录一次连点点击（实时 CPS 统计）。
void clicker_record_click();

// VK 虚拟键 -> 显示名（含鼠标按键中文名）。
std::wstring clicker_key_name(int vk);
