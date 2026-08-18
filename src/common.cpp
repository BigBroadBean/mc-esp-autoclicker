// ============================================================
//  common.cpp — 宽路径、日志、配置实现
// ============================================================
#include "common.h"

static std::wstring g_dllDir;

void dll_set_directory(const wchar_t* dir) {
    g_dllDir = dir ? dir : L"";
}

std::wstring dll_directory() {
    // 优先用 DllMain 里缓存的目录（PEB 模块隐藏后 GetModuleHandleExW 会失败）
    if (!g_dllDir.empty()) return g_dllDir;
    wchar_t buf[MAX_PATH * 2] = {0};
    HMODULE h = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&dll_directory, &h);
    DWORD n = GetModuleFileNameW(h, buf, MAX_PATH * 2);
    if (n == 0) return L"";
    wchar_t* slash = wcsrchr(buf, L'\\');
    if (slash) *(slash + 1) = L'\0';
    g_dllDir = buf;
    return g_dllDir;
}

// 用宽路径打开日志文件（UTF-8 输出）。
// 共享模式必须允许其它实例/进程再次打开，否则 DLL 重载后会共享冲突导致静默丢失日志。
// 若主日志被旧实例句柄占用，自动回退到新文件名，保证总能记录诊断信息。
static HANDLE log_file() {
    static HANDLE h = INVALID_HANDLE_VALUE;
    if (h == INVALID_HANDLE_VALUE) {
        const wchar_t* names[] = {L"esp_log.txt", L"esp_log_new.txt"};
        for (int k = 0; k < 2 && h == INVALID_HANDLE_VALUE; ++k) {
            std::wstring p = dll_directory() + names[k];
            h = CreateFileW(p.c_str(), GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                SetFilePointer(h, 0, nullptr, FILE_END);
            }
        }
    }
    return h;
}

void esp_log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    HANDLE h = log_file();
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(h, buf, (DWORD)strlen(buf), &wr, nullptr);
        WriteFile(h, "\r\n", 2, &wr, nullptr);
        FlushFileBuffers(h);
    }
    OutputDebugStringA(buf);
}

void esp_log_w(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, sizeof(buf) / sizeof(wchar_t), fmt, ap);
    va_end(ap);
    // 转 UTF-8 输出，保证中文名可读
    int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (len > 0) {
        std::string utf8(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8.data(), len, nullptr, nullptr);
        esp_log("%s", utf8.c_str());
    }
}

// ------------------------------------------------------------
// 配置解析
// ------------------------------------------------------------
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static bool parse_bool(const std::string& s, bool def) {
    std::string v = trim(s);
    std::transform(v.begin(), v.end(), v.begin(), ::tolower);
    if (v == "true" || v == "1" || v == "yes" || v == "on") return true;
    if (v == "false" || v == "0" || v == "no" || v == "off") return false;
    return def;
}

static double parse_double(const std::string& s, double def) {
    try { return std::stod(trim(s)); } catch (...) { return def; }
}

static int parse_int(const std::string& s, int def) {
    try { return std::stoi(trim(s), nullptr, 0); } catch (...) { return def; }
}

// 0xRRGGBB
static uint32_t parse_color(const std::string& s, uint32_t def) {
    try {
        std::string v = trim(s);
        unsigned long long x = std::stoull(v, nullptr, 0);
        return (uint32_t)x;
    } catch (...) { return def; }
}

static void clamp_clicker_settings(ClickerSettings& cl) {
    if (cl.cpsLeft10 < 5) cl.cpsLeft10 = 5;
    if (cl.cpsRight10 < 5) cl.cpsRight10 = 5;
    if (cl.cpsMax < 20) cl.cpsMax = 20;
    if (cl.cpsMax > 500) cl.cpsMax = 500;
    if (cl.cpsLeft10 > cl.cpsMax * 10) cl.cpsLeft10 = cl.cpsMax * 10;
    if (cl.cpsRight10 > cl.cpsMax * 10) cl.cpsRight10 = cl.cpsMax * 10;
    if (cl.randomRange < 1) cl.randomRange = 1;
    if (cl.randomRange > 5) cl.randomRange = 5;
    if (cl.humanizeMode < 0) cl.humanizeMode = 0;
    if (cl.humanizeMode > 3) cl.humanizeMode = 3;
    if (cl.humanizeLevel < 1) cl.humanizeLevel = 1;
    if (cl.humanizeLevel > 5) cl.humanizeLevel = 5;
    if (cl.autoStopSeconds < 1) cl.autoStopSeconds = 1;
    if (cl.autoStopSeconds > 3600) cl.autoStopSeconds = 3600;
}

void config_load(EspConfig& cfg) {
    std::wstring path = dll_directory() + L"esp.ini";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        esp_log("[cfg] 未找到 esp.ini，生成默认配置: %ls", path.c_str());
        config_save(cfg);   // 不存在则生成默认配置
        return;
    }
    DWORD size = GetFileSize(h, nullptr);
    std::string data;
    data.resize(size);
    DWORD rd = 0;
    if (size > 0) ReadFile(h, data.data(), size, &rd, nullptr);
    CloseHandle(h);

    std::string section;
    std::string line;
    bool loadedProfile[EspConfig::kClickerProfiles] = {false, false, false, false};
    for (size_t i = 0; i <= data.size(); ++i) {
        char c = (i < data.size()) ? data[i] : '\n';
        if (c == '\n') {
            std::string s = trim(line);
            line.clear();
            if (s.empty()) continue;
            if (s[0] == ';' || s[0] == '#') continue;
            if (s.front() == '[' && s.back() == ']') { section = s.substr(1, s.size() - 2); continue; }
            size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            std::string key = trim(s.substr(0, eq));
            std::string val = s.substr(eq + 1);
            // 去掉行尾注释
            size_t sc = val.find(';');
            if (sc != std::string::npos) val = val.substr(0, sc);
            val = trim(val);
            bool isColor = (section == "colors");
            auto set = [&](bool& b) { b = parse_bool(val, b); };
            auto setd = [&](double& d) { d = parse_double(val, d); };
            auto seti = [&](int& i) { i = parse_int(val, i); };
            auto setc = [&](uint32_t& c) { c = parse_color(val, c); };
            if (section == "esp") {
                if (key == "enabled") set(cfg.enabled);
                else if (key == "menuKey" || key == "toggleKey") seti(cfg.menuKey);
                else if (key == "espKey") seti(cfg.espKey);
                else if (key == "maxDistance") setd(cfg.maxDistance);
                else if (key == "fov") setd(cfg.fov);
                else if (key == "box3d") set(cfg.box3d);
                else if (key == "box2d") set(cfg.box2d);
                else if (key == "nameTags") set(cfg.nameTags);
                else if (key == "tracer") set(cfg.tracer);
                else if (key == "filledBox") set(cfg.filledBox);
                else if (key == "lineWidth") seti(cfg.lineWidth);
                else if (key == "showPlayers") set(cfg.showPlayers);
                else if (key == "showMobs") set(cfg.showMobs);
                else if (key == "showOthers") set(cfg.showOthers);
                else if (key == "onlyWhenFocused") set(cfg.onlyWhenFocused);
                else if (key == "keepOnChat") set(cfg.keepOnChat);
                else if (key == "smoothMs") setd(cfg.smoothMs);
                else if (key == "renderHz") seti(cfg.renderHz);
                else if (key == "showTrajectory") set(cfg.showTrajectory);
                else if (key == "trajectoryTicks") seti(cfg.trajectoryTicks);
            } else if (section == "clicker" || section.rfind("clickerProfile", 0) == 0) {
                if (section == "clicker" && key == "profileKey") {
                    seti(cfg.profileKey);
                    line.clear();
                    continue;
                }
                ClickerSettings* pcl = nullptr;
                if (section == "clicker") {
                    pcl = &cfg.clicker;
                    if (key == "activeProfile") seti(cfg.activeProfile);
                } else {
                    int idx = atoi(section.c_str() + 14) - 1;
                    if (idx >= 0 && idx < EspConfig::kClickerProfiles) {
                        pcl = &cfg.profiles[idx];
                        loadedProfile[idx] = true;
                    }
                }
                if (!pcl) { line.clear(); continue; }
                ClickerSettings& cl = *pcl;
                if (key == "enabled") set(cl.enabled);
                else if (key == "toggleKey") seti(cl.toggleKey);
                else if (key == "leftEnabled") set(cl.leftEnabled);
                else if (key == "rightEnabled") set(cl.rightEnabled);
                else if (key == "keep") set(cl.keep);
                else if (key == "cpsLeft10" || key == "leftCps") seti(cl.cpsLeft10);
                else if (key == "cpsRight10" || key == "rightCps") seti(cl.cpsRight10);
                else if (key == "cpsMax") seti(cl.cpsMax);
                else if (key == "randomEnabled") set(cl.randomEnabled);
                else if (key == "randomRange") seti(cl.randomRange);
                else if (key == "humanizeMode") seti(cl.humanizeMode);
                else if (key == "humanizeLevel") seti(cl.humanizeLevel);
                else if (key == "autoStopEnabled") set(cl.autoStopEnabled);
                else if (key == "autoStopSeconds") seti(cl.autoStopSeconds);
                else if (key == "attackGate") set(cl.attackGate);
                else if (key == "attackGateKey") seti(cl.attackGateKey);
                else if (key == "placeGate") set(cl.placeGate);
                else if (key == "placeGateKey") seti(cl.placeGateKey);
                else if (key == "cursorGate") set(cl.cursorGate);
                else if (key == "inGameGate") set(cl.inGameGate);
            } else if (isColor) {
                if (key == "player") setc(cfg.colPlayer);
                else if (key == "mob") setc(cfg.colMob);
                else if (key == "other") setc(cfg.colOther);
                else if (key == "hud") setc(cfg.colHud);
                else if (key == "trajectory") setc(cfg.colTraj);
                else if (key == "trajectoryOther") setc(cfg.colTrajOther);
                else if (key == "land") setc(cfg.colLand);
                else if (key == "landHit") setc(cfg.colLandHit);
            }
        } else {
            line += c;
        }
    }

    if (cfg.activeProfile < 0) cfg.activeProfile = 0;
    if (cfg.activeProfile >= EspConfig::kClickerProfiles) cfg.activeProfile = 0;

    bool anyProfile = false;
    for (int i = 0; i < EspConfig::kClickerProfiles; ++i) anyProfile = anyProfile || loadedProfile[i];
    if (anyProfile) {
        cfg.clicker = cfg.profiles[cfg.activeProfile];
    } else {
        for (int i = 0; i < EspConfig::kClickerProfiles; ++i) cfg.profiles[i] = cfg.clicker;
    }
    clamp_clicker_settings(cfg.clicker);
    for (int i = 0; i < EspConfig::kClickerProfiles; ++i) clamp_clicker_settings(cfg.profiles[i]);
}

void config_save(const EspConfig& cfg) {
    std::wstring path = dll_directory() + L"esp.ini";
    std::string out;
    out.reserve(4096);
    auto line = [&](const char* s) { out += s; out += "\r\n"; };
    auto b2s = [](bool b) { return b ? "true" : "false"; };

    line("[esp]");
    line((std::string("enabled = ") + b2s(cfg.enabled)).c_str());
    line((std::string("menuKey = ") + std::to_string(cfg.menuKey) + "          ; VK_INSERT = 45").c_str());
    line((std::string("espKey = ") + std::to_string(cfg.espKey) + "            ; VK_HOME = 36，直接开关 ESP").c_str());
    line((std::string("maxDistance = ") + std::to_string(cfg.maxDistance)).c_str());
    line((std::string("fov = ") + std::to_string(cfg.fov) + "                     ; 与游戏内 FOV 一致（默认 70）").c_str());
    line((std::string("box3d = ") + b2s(cfg.box3d)).c_str());
    line((std::string("box2d = ") + b2s(cfg.box2d)).c_str());
    line((std::string("nameTags = ") + b2s(cfg.nameTags)).c_str());
    line((std::string("tracer = ") + b2s(cfg.tracer)).c_str());
    line((std::string("filledBox = ") + b2s(cfg.filledBox)).c_str());
    line((std::string("lineWidth = ") + std::to_string(cfg.lineWidth)).c_str());
    line((std::string("showPlayers = ") + b2s(cfg.showPlayers)).c_str());
    line((std::string("showMobs = ") + b2s(cfg.showMobs)).c_str());
    line((std::string("showOthers = ") + b2s(cfg.showOthers)).c_str());
    line((std::string("onlyWhenFocused = ") + b2s(cfg.onlyWhenFocused)).c_str());
    line((std::string("keepOnChat = ") + b2s(cfg.keepOnChat) + "               ; 打开聊天（T）时继续渲染 ESP").c_str());
    line((std::string("smoothMs = ") + std::to_string(cfg.smoothMs) + "                 ; 盒子时域平滑时间常数（毫秒）").c_str());
    line((std::string("renderHz = ") + std::to_string(cfg.renderHz) + "                  ; 渲染线程刷新率（Hz）").c_str());
    line((std::string("showTrajectory = ") + b2s(cfg.showTrajectory) + "           ; 渲染弹射物预测轨迹").c_str());
    line((std::string("trajectoryTicks = ") + std::to_string(cfg.trajectoryTicks) + "            ; 轨迹预测长度（游戏 tick）").c_str());

    auto saveClicker = [&](const ClickerSettings& cl, const std::string& sectionName, bool withActive) {
        line(sectionName.c_str());
        if (withActive) {
            line((std::string("activeProfile = ") + std::to_string(cfg.activeProfile)).c_str());
            line((std::string("profileKey = ") + std::to_string(cfg.profileKey) + "             ; VK_F8 = 119，循环切换方案").c_str());
        }
        line((std::string("enabled = ") + b2s(cl.enabled) + "              ; 注入后初始是否启用连点").c_str());
        line((std::string("toggleKey = ") + std::to_string(cl.toggleKey) + "               ; VK_MBUTTON = 4").c_str());
        line((std::string("leftEnabled = ") + b2s(cl.leftEnabled)).c_str());
        line((std::string("rightEnabled = ") + b2s(cl.rightEnabled)).c_str());
        line((std::string("keep = ") + b2s(cl.keep) + "                 ; 保持模式：无需按住鼠标").c_str());
        line((std::string("cpsLeft10 = ") + std::to_string(cl.cpsLeft10) + "             ; 左键 CPS*10（100=10.0CPS）").c_str());
        line((std::string("cpsRight10 = ") + std::to_string(cl.cpsRight10) + "             ; 右键 CPS*10").c_str());
        line((std::string("cpsMax = ") + std::to_string(cl.cpsMax) + "               ; CPS 上限（20..500）").c_str());
        line((std::string("randomEnabled = ") + b2s(cl.randomEnabled) + "         ; 随机 CPS 波动").c_str());
        line((std::string("randomRange = ") + std::to_string(cl.randomRange) + "               ; 随机波动 ±CPS（1..5）").c_str());
        line((std::string("humanizeMode = ") + std::to_string(cl.humanizeMode) + "             ; 0=均匀 1=双击 2=呼吸 3=疲劳").c_str());
        line((std::string("humanizeLevel = ") + std::to_string(cl.humanizeLevel) + "             ; 拟人化强度（1..5）").c_str());
        line((std::string("autoStopEnabled = ") + b2s(cl.autoStopEnabled) + "        ; 定时自动停止").c_str());
        line((std::string("autoStopSeconds = ") + std::to_string(cl.autoStopSeconds) + "            ; N 秒后自动停止").c_str());
        line((std::string("attackGate = ") + b2s(cl.attackGate) + "           ; 仅准星目标可攻击时左键连点").c_str());
        line((std::string("attackGateKey = ") + std::to_string(cl.attackGateKey) + "              ; VK_F6 = 117").c_str());
        line((std::string("placeGate = ") + b2s(cl.placeGate) + "            ; 仅手持方块时右键连点").c_str());
        line((std::string("placeGateKey = ") + std::to_string(cl.placeGateKey) + "              ; VK_F7 = 118").c_str());
        line((std::string("cursorGate = ") + b2s(cl.cursorGate) + "           ; 光标可见时暂停连点").c_str());
        line((std::string("inGameGate = ") + b2s(cl.inGameGate) + "         ; 仅已进入游戏（player!=null）时连点").c_str());
    };

    saveClicker(cfg.clicker, "[clicker]", true);
    for (int i = 0; i < EspConfig::kClickerProfiles; ++i)
        saveClicker(cfg.profiles[i], "[clickerProfile" + std::to_string(i + 1) + "]", false);

    line("[colors]");
    char cb[64];
    auto color_line = [&](const char* key, uint32_t v, const char* note = nullptr) {
        snprintf(cb, sizeof(cb), "%s = 0x%06X", key, v & 0xFFFFFF);
        std::string s = cb;
        if (note) { s += " ; "; s += note; }
        out += s; out += "\r\n";
    };
    color_line("player", cfg.colPlayer, "玩家：红");
    color_line("mob", cfg.colMob, "生物：橙");
    color_line("other", cfg.colOther, "其他：青");
    color_line("hud", cfg.colHud);
    color_line("trajectory", cfg.colTraj, "弹射物轨迹：亮绿");
    color_line("trajectoryOther", cfg.colTrajOther, "其他玩家弓蓄力抛物线：红");
    color_line("land", cfg.colLand, "弓预判落点方块：蓝");
    color_line("landHit", cfg.colLandHit, "弓预判命中实体方块：红");

    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(h, out.data(), (DWORD)out.size(), &wr, nullptr);
        CloseHandle(h);
    } else {
        esp_log("[cfg] 保存 esp.ini 失败 err=%lu path=%ls", GetLastError(), path.c_str());
    }
}
