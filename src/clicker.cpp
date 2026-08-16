// ============================================================
//  clicker.cpp — 连点器核心实现
// ============================================================
#include "clicker.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>

#include <mmsystem.h>   // timeBeginPeriod / timeEndPeriod

// ------------------------------------------------------------
// 全局运行状态
// ------------------------------------------------------------
static std::mutex        g_mutex;
static ClickerSettings   g_settings;
static HWND              g_gameHwnd = nullptr;
static std::atomic<bool> g_running{false};        // 连点总开关
static std::atomic<bool> g_threadRunning{false};  // 工作线程是否已创建
static std::atomic<bool> g_stop{false};           // 工作线程退出标志
static HANDLE            g_threadHandle = nullptr;

static std::atomic<bool>      g_combatReady{false};
static std::atomic<bool>      g_canAttack{false};
static std::atomic<bool>      g_canPlace{false};
static std::atomic<long long> g_combatLastMs{0};

// 实时 CPS：环形时间戳（与 AutoClicker 实现一致）
static constexpr int kCpsWindow = 1024;
static std::atomic<long long> s_clickStamp[kCpsWindow];
static std::atomic<int>        s_clickHead{0};
static std::atomic<int>        s_clickFilled{0};
static std::atomic<long long>  g_clickCount{0};

using namespace std::chrono;

// 热键修改设置后的持久化回调（在 g_mutex 外执行）
static void (*g_settingsChangedCb)() = nullptr;

// ------------------------------------------------------------
// 对外接口
// ------------------------------------------------------------
void clicker_set_settings_changed_callback(void (*fn)()) {
    g_settingsChangedCb = fn;
}

void clicker_apply_settings(const ClickerSettings& s) {
    ClickerSettings ns = s;
    // 钳制：配置可能被手改
    if (ns.cpsLeft10 < 5) ns.cpsLeft10 = 5;
    if (ns.cpsRight10 < 5) ns.cpsRight10 = 5;
    if (ns.cpsMax < 20) ns.cpsMax = 20;
    if (ns.cpsMax > 500) ns.cpsMax = 500;
    if (ns.cpsLeft10 > ns.cpsMax * 10) ns.cpsLeft10 = ns.cpsMax * 10;
    if (ns.cpsRight10 > ns.cpsMax * 10) ns.cpsRight10 = ns.cpsMax * 10;
    if (ns.randomRange < 1) ns.randomRange = 1;
    if (ns.randomRange > 5) ns.randomRange = 5;
    if (ns.humanizeMode < 0) ns.humanizeMode = 0;
    if (ns.humanizeMode > 3) ns.humanizeMode = 3;
    if (ns.humanizeLevel < 1) ns.humanizeLevel = 1;
    if (ns.humanizeLevel > 5) ns.humanizeLevel = 5;
    if (ns.autoStopSeconds < 1) ns.autoStopSeconds = 1;
    if (ns.autoStopSeconds > 3600) ns.autoStopSeconds = 3600;
    std::lock_guard<std::mutex> lk(g_mutex);
    g_settings = ns;
}

ClickerSnapshot clicker_snapshot() {
    ClickerSnapshot s;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        s.settings = g_settings;
    }
    s.running = g_running.load(std::memory_order_acquire);
    s.combatReady = g_combatReady.load(std::memory_order_acquire);
    s.canAttack = g_canAttack.load(std::memory_order_acquire);
    s.canPlace = g_canPlace.load(std::memory_order_acquire);
    s.clickCount = g_clickCount.load(std::memory_order_relaxed);
    s.realtimeCps = [&]() {
        long long nowNs = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
        int f = s_clickFilled.load(std::memory_order_relaxed);
        if (f <= 0) return 0;
        int h = s_clickHead.load(std::memory_order_relaxed);
        int n = f < kCpsWindow ? f : kCpsWindow;
        int cnt = 0;
        for (int i = 0; i < n; ++i) {
            int idx = h - 1 - i;
            if (idx < 0) idx += kCpsWindow;
            if (nowNs - s_clickStamp[idx].load(std::memory_order_relaxed) <= 1000000000LL)
                ++cnt;
            else
                break;
        }
        return cnt;
    }();
    return s;
}

void clicker_record_click() {
    long long ns = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    int h = s_clickHead.load(std::memory_order_relaxed);
    s_clickStamp[h].store(ns, std::memory_order_relaxed);
    s_clickHead.store((h + 1) % kCpsWindow, std::memory_order_relaxed);
    int f = s_clickFilled.load(std::memory_order_relaxed);
    if (f < kCpsWindow) s_clickFilled.store(f + 1, std::memory_order_relaxed);
    g_clickCount.fetch_add(1, std::memory_order_relaxed);
}

void clicker_set_running(bool on) {
    g_running.store(on, std::memory_order_release);
}

void clicker_toggle_running() {
    clicker_set_running(!g_running.load(std::memory_order_acquire));
}

void clicker_set_combat(bool ready, bool canAttack, bool canPlace) {
    g_combatReady.store(ready, std::memory_order_release);
    g_canAttack.store(canAttack, std::memory_order_release);
    g_canPlace.store(canPlace, std::memory_order_release);
    if (ready) g_combatLastMs.store((long long)GetTickCount64(), std::memory_order_release);
}

std::wstring clicker_key_name(int vk) {
    switch (vk) {
    case 0:             return L"无";
    case VK_LBUTTON:    return L"鼠标左键";
    case VK_RBUTTON:    return L"鼠标右键";
    case VK_MBUTTON:    return L"鼠标中键";
    case VK_XBUTTON1:   return L"鼠标侧键1";
    case VK_XBUTTON2:   return L"鼠标侧键2";
    default: break;
    }
    wchar_t buffer[256] = {0};
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    switch (vk) {
    case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
    case VK_RCONTROL: case VK_RMENU:
    case VK_LWIN: case VK_RWIN: case VK_APPS:
    case VK_INSERT: case VK_HOME: case VK_PRIOR:
    case VK_DELETE: case VK_END: case VK_NEXT:
    case VK_NUMLOCK: case VK_SCROLL:
    case VK_OEM_NEC_EQUAL:
        scan |= 0xE000;
        break;
    default: break;
    }
    if (GetKeyNameTextW((LONG)scan << 16, buffer, 255) > 0) return buffer;
    wchar_t fb[32];
    swprintf(fb, 32, L"VK_%d", vk);
    return fb;
}

// ------------------------------------------------------------
// 内部工具
// ------------------------------------------------------------
static bool key_down(int vk) {
    return vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static bool cursor_showing() {
    CURSORINFO ci = {};
    ci.cbSize = sizeof(ci);
    return GetCursorInfo(&ci) != FALSE && (ci.flags & CURSOR_SHOWING) != 0;
}

// 亚毫秒精确等待（移植自 AutoClicker）
static void PreciseSleepUntil(steady_clock::time_point target, bool precise) {
    for (;;) {
        auto remain = target - steady_clock::now();
        if (remain <= steady_clock::duration::zero()) return;
        long long us = duration_cast<microseconds>(remain).count();
        if (us > 8000) {
            Sleep((DWORD)((us - 2000) / 1000));
        } else if (us > 1500) {
            Sleep(1);
        } else if (precise) {
            while (steady_clock::now() < target) YieldProcessor();
            return;
        } else {
            Sleep(1);
        }
    }
}

static int cps_to_ms(int cps10) {
    float cps = cps10 / 10.0f;
    int ms = (int)(500.0f / cps);
    return ms < 1 ? 1 : ms;
}

// ------------------------------------------------------------
// 连点线程主体（不接触 JVM；线程起点已由 spawn_hidden_thread 伪装）
// ------------------------------------------------------------
static DWORD WINAPI clicker_thread_main(LPVOID) {
    using PfnPostMessageA = BOOL(WINAPI*)(HWND, UINT, WPARAM, LPARAM);
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    PfnPostMessageA MyPostMessageA = u32 ? (PfnPostMessageA)GetProcAddress(u32, "PostMessageA") : nullptr;

    // xorshift64* PRNG（随机 CPS 抖动）
    unsigned long long rng = 0x9E3779B97F4A7C15ull ^ (unsigned long long)GetTickCount64();
    auto rnd = [&]() -> unsigned {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return (unsigned)(rng >> 32);
    };

    // 拟人化节奏 + 随机波动 -> 实际延迟（所有设置从当前 cfg 快照读取，避免锁外竞态）
    auto humanDelay = [&](const ClickerSettings& cfg, int baseCps10, double humanFactor) -> int {
        double cps = (double)baseCps10 / humanFactor;
        int jitter = 0;
        if (cfg.randomEnabled)
            jitter = (int)(rnd() % (unsigned)(cfg.randomRange * 20 + 1)) - cfg.randomRange * 10;
        int cps10 = (int)cps + jitter;
        if (cps10 < 5) cps10 = 5;
        if (cps10 > cfg.cpsMax * 10) cps10 = cfg.cpsMax * 10;
        return cps_to_ms(cps10);
    };

    enum ClickState { CS_IDLE, CS_WAIT_UP, CS_WAIT_DOWN };
    ClickState leftSt = CS_IDLE, rightSt = CS_IDLE;
    auto nextLeftTime = steady_clock::now();
    auto nextRightTime = steady_clock::now();
    auto nextScan = steady_clock::now();
    POINT lastPt = {};

    bool prevToggle = false, prevAttack = false, prevPlace = false;
    long long clickIdxL = 0, clickIdxR = 0;
    bool prevHeldL = false, prevHeldR = false;
    auto heldStartL = steady_clock::now();
    auto heldStartR = steady_clock::now();

    static steady_clock::time_point stopStart{};
    static bool stopArmed = false;

    timeBeginPeriod(1);

    auto releaseLeft = [&]() {
        if (leftSt == CS_WAIT_UP && g_gameHwnd && IsWindow(g_gameHwnd)) {
            GetCursorPos(&lastPt);
            ScreenToClient(g_gameHwnd, &lastPt);
            if (MyPostMessageA)
                MyPostMessageA(g_gameHwnd, WM_LBUTTONUP, 0, MAKELPARAM(lastPt.x, lastPt.y));
        }
        leftSt = CS_IDLE;
    };
    auto releaseRight = [&]() {
        if (rightSt == CS_WAIT_UP && g_gameHwnd && IsWindow(g_gameHwnd)) {
            GetCursorPos(&lastPt);
            ScreenToClient(g_gameHwnd, &lastPt);
            if (MyPostMessageA)
                MyPostMessageA(g_gameHwnd, WM_RBUTTONUP, 0, MAKELPARAM(lastPt.x, lastPt.y));
        }
        rightSt = CS_IDLE;
    };

    for (;;) {
        if (g_stop.load(std::memory_order_acquire)) break;
        auto now = steady_clock::now();

        if (now >= nextScan) {
            nextScan = now + milliseconds(4);

            ClickerSettings cfg;
            {
                std::lock_guard<std::mutex> lk(g_mutex);
                cfg = g_settings;
            }

            // ---- 连点总开关热键（沿自 AutoClicker：按下沿触发，松开后可再次触发）----
            bool curToggle = key_down(cfg.toggleKey);
            if (curToggle && !prevToggle) clicker_toggle_running();
            prevToggle = curToggle;

            // ---- 攻击/放置门控热键（修改后写回配置）----
            bool curAttack = key_down(cfg.attackGateKey);
            if (curAttack && !prevAttack && cfg.attackGateKey != 0) {
                cfg.attackGate = !cfg.attackGate;
                clicker_apply_settings(cfg);
                if (g_settingsChangedCb) g_settingsChangedCb();
            }
            prevAttack = curAttack;

            bool curPlace = key_down(cfg.placeGateKey);
            if (curPlace && !prevPlace && cfg.placeGateKey != 0) {
                cfg.placeGate = !cfg.placeGate;
                clicker_apply_settings(cfg);
                if (g_settingsChangedCb) g_settingsChangedCb();
            }
            prevPlace = curPlace;

            // ---- 定时自动停止 ----
            bool running = g_running.load(std::memory_order_acquire);
            if (running && cfg.autoStopEnabled && cfg.autoStopSeconds > 0) {
                if (!stopArmed) {
                    stopStart = now;
                    stopArmed = true;
                } else if (now - stopStart >= seconds(cfg.autoStopSeconds)) {
                    g_running.store(false, std::memory_order_release);
                    stopArmed = false;
                }
            } else if (!running) {
                stopArmed = false;
            }
        }

        ClickerSettings cfg;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            cfg = g_settings;
        }
        bool running = g_running.load(std::memory_order_acquire);

        // ---- 门控（失败关闭，宁可少点不可误点）----
        long long lastCombat = g_combatLastMs.load(std::memory_order_acquire);
        bool combatFresh = g_combatReady.load(std::memory_order_acquire) &&
                           (GetTickCount64() - lastCombat) < 500;
        bool canAtkGate = !cfg.attackGate ||
                          (combatFresh && g_canAttack.load(std::memory_order_acquire));
        bool canPlaceGate = !cfg.placeGate ||
                            (combatFresh && g_canPlace.load(std::memory_order_acquire));
        bool cursorGate = !cfg.cursorGate || !cursor_showing();

        bool fgOk = g_gameHwnd && IsWindow(g_gameHwnd) && !IsIconic(g_gameHwnd);
        if (fgOk) {
            HWND fg = GetForegroundWindow();
            if (fg != g_gameHwnd) {
                DWORD fgPid = 0;
                if (fg) GetWindowThreadProcessId(fg, &fgPid);
                fgOk = (fgPid == GetCurrentProcessId());
            }
        }

        if (!canAtkGate) releaseLeft();
        if (!canPlaceGate) releaseRight();
        if (!cursorGate || !fgOk || !running) {
            releaseLeft();
            releaseRight();
        }

        bool leftActive = running && cfg.leftEnabled && fgOk && canAtkGate && cursorGate;
        bool rightActive = running && cfg.rightEnabled && fgOk && canPlaceGate && cursorGate;

        bool leftHeld = leftActive && (key_down(VK_LBUTTON) || cfg.keep);
        if (leftHeld) {
            if (now >= nextLeftTime) {
                GetCursorPos(&lastPt);
                ScreenToClient(g_gameHwnd, &lastPt);
                LPARAM lp = MAKELPARAM(lastPt.x, lastPt.y);
                if (leftSt != CS_WAIT_UP) {
                    if (MyPostMessageA) MyPostMessageA(g_gameHwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp);
                    leftSt = CS_WAIT_UP;
                    clicker_record_click();
                } else {
                    if (MyPostMessageA) MyPostMessageA(g_gameHwnd, WM_LBUTTONUP, 0, lp);
                    leftSt = CS_IDLE;
                }
                if (!prevHeldL) { heldStartL = now; clickIdxL = 0; }
                double t = duration<double>(now - heldStartL).count();
                double amp = cfg.humanizeLevel / 5.0;
                double f = 1.0;
                switch (cfg.humanizeMode) {
                case 1:  f = (clickIdxL % 2 == 0) ? 1.0 - 0.28 * amp : 1.0 + 0.38 * amp; break;
                case 2:  f = 1.0 + 0.24 * amp * std::sin(t * 6.28318530718 / 3.5); break;
                case 3:  f = 1.0 + 0.30 * amp * (1.0 - std::exp(-t / 8.0)); break;
                default: f = 1.0; break;
                }
                if (f < 0.5) f = 0.5;
                if (f > 1.5) f = 1.5;
                ++clickIdxL;
                nextLeftTime = now + milliseconds(humanDelay(cfg, cfg.cpsLeft10, f));
            }
            prevHeldL = true;
        } else {
            releaseLeft();
            prevHeldL = false;
        }

        bool rightHeld = rightActive && (key_down(VK_RBUTTON) || cfg.keep);
        if (rightHeld) {
            if (now >= nextRightTime) {
                GetCursorPos(&lastPt);
                ScreenToClient(g_gameHwnd, &lastPt);
                LPARAM lp = MAKELPARAM(lastPt.x, lastPt.y);
                if (rightSt != CS_WAIT_UP) {
                    if (MyPostMessageA) MyPostMessageA(g_gameHwnd, WM_RBUTTONDOWN, MK_RBUTTON, lp);
                    rightSt = CS_WAIT_UP;
                    clicker_record_click();
                } else {
                    if (MyPostMessageA) MyPostMessageA(g_gameHwnd, WM_RBUTTONUP, 0, lp);
                    rightSt = CS_IDLE;
                }
                if (!prevHeldR) { heldStartR = now; clickIdxR = 0; }
                double t = duration<double>(now - heldStartR).count();
                double amp = cfg.humanizeLevel / 5.0;
                double f = 1.0;
                switch (cfg.humanizeMode) {
                case 1:  f = (clickIdxR % 2 == 0) ? 1.0 - 0.28 * amp : 1.0 + 0.38 * amp; break;
                case 2:  f = 1.0 + 0.24 * amp * std::sin(t * 6.28318530718 / 3.5); break;
                case 3:  f = 1.0 + 0.30 * amp * (1.0 - std::exp(-t / 8.0)); break;
                default: f = 1.0; break;
                }
                if (f < 0.5) f = 0.5;
                if (f > 1.5) f = 1.5;
                ++clickIdxR;
                nextRightTime = now + milliseconds(humanDelay(cfg, cfg.cpsRight10, f));
            }
            prevHeldR = true;
        } else {
            releaseRight();
            prevHeldR = false;
        }

        auto next = nextScan;
        if (leftHeld && nextLeftTime < next) next = nextLeftTime;
        if (rightHeld && nextRightTime < next) next = nextRightTime;
        if (next > now) PreciseSleepUntil(next, leftHeld || rightHeld);
    }

    releaseLeft();
    releaseRight();
    timeEndPeriod(1);
    g_threadRunning.store(false, std::memory_order_release);
    return 0;
}

void clicker_start(HWND gameHwnd) {
    if (g_threadRunning.exchange(true, std::memory_order_acq_rel)) return;
    g_stop.store(false, std::memory_order_release);
    g_gameHwnd = gameHwnd;
    g_threadHandle = spawn_hidden_thread(clicker_thread_main);
    if (!g_threadHandle) {
        g_threadRunning.store(false, std::memory_order_release);
    }
}

void clicker_stop() {
    g_stop.store(true, std::memory_order_release);
    g_running.store(false, std::memory_order_release);
    if (g_threadHandle) {
        WaitForSingleObject(g_threadHandle, 1500);
        CloseHandle(g_threadHandle);
        g_threadHandle = nullptr;
    }
    g_threadRunning.store(false, std::memory_order_release);
}
