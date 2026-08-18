# MC ESP + 内置连点器（单文件 EXE）

Minecraft 1.20.1 Forge 47.4.10 注入式 **ESP + 连点器**，全部打包进一个
`mc_esp.exe`：运行时把内嵌 DLL 解包到 `%TEMP%`，注入后尝试清理（游戏占用时延迟到重启清理），
不再需要单独分发 `mc_esp.dll` 或 `injector.exe`。

- 注入方式：`CreateRemoteThread + LoadLibraryW`
- ESP 绘制：GDI 透明覆盖层（截图排除，游戏内直绘）
- 连点器移植自 `AutoClicker-main`
- 攻击 / 放置门控逻辑移植自 `MCCombatStatusJni`，但**不发送 UDP / 共享内存**，
  由 DLL 在游戏渲染线程内直接读取并消费

> 已移除：自瞄（Aimbot）、多倍点击（multi-click）、滚轮点击（scroll-to-click）、
> 外部渲染共享内存（esp_shm）与外部消息传递。

---

## 使用

1. 启动 Minecraft（1.20.1 Forge 47.4.10，64 位 Java），进入世界。
2. 双击或命令行运行 `mc_esp.exe`：
   ```
   mc_esp.exe                自动查找 Minecraft 并注入
   mc_esp.exe -pid <PID>     注入指定 PID
   mc_esp.exe -find          仅查找进程，不注入
   mc_esp.exe -dir <数据目录> 自定义 esp.ini / esp_log.txt 保存目录
   ```
3. 注入成功后按 **`Insert`** 呼出 / 关闭菜单。

`esp.ini` 与 `esp_log.txt` 默认位于 **`%APPDATA%\mc_esp\`**
（通常是 `C:\Users\<用户名>\AppData\Roaming\mc_esp`），首次运行自动创建并生成
`esp.ini`；也可以用 `-dir <数据目录>` 或环境变量 `MC_ESP_DATA_DIR` 指定其他位置。

### Insert 菜单操作

| 输入 | 功能 |
| --- | --- |
| `Insert` | 呼出 / 关闭菜单 |
| 鼠标左键 | 点击标签页 / 行 / 布尔项；拖拽滑块；点面板外关闭菜单 |
| 鼠标滚轮 | 当前页内上 / 下选择项目 |
| 鼠标悬停 | 0.35 秒后显示当前功能说明 |
| `Tab` | 循环切换功能页 |
| `↑` / `↓` | 当前页内选择项目 |
| `←` / `→` | 调整数值 / 循环选项 / 切换布尔项 |
| `Shift + ←/→` | 数值快速调整 |
| `Enter` | 切换布尔项；在热键项目上按 Enter 后按任意键绑定 |
| `Esc` | 关闭菜单 / 取消热键绑定 |

菜单打开期间覆盖层临时截获鼠标，同时自动暂停连点，关闭后恢复鼠标穿透。
鼠标消息由 Overlay 的真实 `WndProc` 接收（`WM_LBUTTONDOWN/UP`、`WM_MOUSEMOVE`、
`WM_MOUSEWHEEL`、`WM_MOUSELEAVE`），不靠 `GetAsyncKeyState` 模拟。

### 功能分页

- **连点**：总开关、左/右键开关、左/右键 CPS 与预设、保持模式、连点热键
- **门控**：攻击门控、放置门控、光标门控、游戏内门控，及各自热键
- **高级**：随机 CPS、随机范围、拟人化节奏与强度、CPS 上限、定时停止
- **ESP**：ESP 开关 / 热键、玩家 / 生物 / 其他过滤、3D 盒、2D 矩形、填充、
  名字、射线、线宽、最大距离、弹射物轨迹、轨迹 tick
- **系统**：配置方案、方案循环热键

带滑块的菜单项：左/右键 CPS、随机范围、拟人化强度、CPS 上限、停止秒数、
ESP 线宽、ESP 最大距离、轨迹 tick。

### 默认热键

| 功能 | 默认按键 |
| --- | --- |
| 菜单 | `Insert` |
| ESP 开关 | `Home` |
| 连点器启停 | 鼠标中键（`VK_MBUTTON`） |
| 攻击门控 | `F6` |
| 放置门控 | `F7` |
| 循环切换 4 套连点方案 | `F8` |

通过快捷键开关 ESP / 连点器 / 门控 / 切换方案时，右下角会显示 Toast
（约 2.2 秒）。

游戏内右下角有常驻状态栏（HUD）：只要 **ESP 或连点器任一开启** 就会显示，
不再依赖 ESP 开关；打开菜单时改由菜单底部状态行显示。内容包含
`ESP / 连点(实时 CPS) / 攻击 / 放置 / 游戏内外 / 方案`，状态变化时自动刷新。

---

## 功能

### ESP

- 玩家 / 生物 / 其他实体过滤
- 3D 包围盒（穿墙）、2D 屏幕矩形、半透明填充、名字标签、Tracer
- 线宽、最大距离菜单内实时可调
- 弹射物轨迹（箭 / 雪球 / 末影珍珠）与轨迹预测长度
- 本地与其他玩家弓蓄力预判 + 落点方块标记
- 帧间平滑、聊天界面继续显示、失焦自动隐藏
- `box2d` 此前只存在于 `esp.ini` 但未实现，现已生效

### 连点器

- 4 套配置方案，菜单切换或按 `F8` 循环切换，各自独立保存
- 左右键独立连点，CPS 独立可调（0.1 CPS 精度）
- 6 / 10 / 15 / 20 / 30 / 40 CPS 快捷预设
- 保持模式：无需按住鼠标即可连点
- 随机 CPS 波动（±1..±5）
- 拟人化节奏：均匀 / 双击连招 / 呼吸波动 / 疲劳递减，强度 1..5
- CPS 上限（20..500）、定时自动停止
- 光标门控：光标可见（背包 / 聊天 / 菜单）时暂停
- 游戏内门控：`player == null`（主菜单 / 加载中）时暂停
- 仅可攻击时左键连点、仅手持方块时右键连点

攻击门控判定与 `MCCombatStatusJni` 的 `forge1201` 映射一致：

```
canAttack = hitResult.getType() == ENTITY
         && target instanceof LivingEntity
         && target.isAlive()
         && target != 本地玩家
         && target.isAttackable()

canPlace  = player.getMainHandItem().getItem() instanceof BlockItem
```

---

## 本地保存

所有设置保存在 `data_directory()` 目录下的 `esp.ini`（UTF-8，首次运行自动生成）：
默认 `%APPDATA%\mc_esp\`，可通过 `-dir` / `MC_ESP_DATA_DIR` 覆盖。
菜单修改、滑块拖动结束、快捷键开关 ESP / 连点器 / 门控 / 切换方案都会立即写回；
4 套方案分别保存在 `[clickerProfile1]`..`[clickerProfile4]`。

---

## 性能

- 菜单使用离屏像素缓存：静态时每帧只 `memcpy` 面板区域。
- ESP / 菜单 / 门控全关时 SwapBuffers 钩子只做原子判断，不碰 JVM；JVM 符号空闲时
  每秒预热一次，避免第一次开菜单卡顿。
- 门控状态只在“连点器运行且相关门控开启”时每 5ms 读一次 JNI。
- 实体预投影数据从游戏线程到绘制线程改用 `swap`，不再每帧复制两个 `vector`。
- 宿主线程动态频率：菜单打开用 `renderHz`（默认 120），ESP / HUD 显示用 60Hz，
  全部关闭时降到 30Hz。
- 右下角状态栏采用离屏缓存 + 内容签名：静态时只 `memcpy` 小区域并跳过
  `UpdateLayeredWindow`，ESP/连点状态变化时才重画。
- `renderHz` 使用 Win10 高精度可等待定时器，避免 `Sleep(8)` 被系统按 15.6ms
  节拍合并回 60Hz。

---

## 构建

环境：Windows + MinGW-w64 x86_64 g++（本项目开发环境为 GCC 15.2）。

```bat
build.bat
```

产物：**`mc_esp.exe`**（单文件）。

构建过程：`build_tmp\mc_esp.dll`（中间 payload）→ `build_tmp\bin2h.exe` 转成
`build_tmp\payload.h` → 编译进 `loader\loader.cpp` 得到 `mc_esp.exe`。
`build_tmp\` 为编译中间目录，已加入 `.gitignore`。

`include\` 内为 JNI / JVMTI 头文件；DLL 不链接 `jvm.dll`，运行时通过
`JNI_GetCreatedJavaVMs` 动态定位 JVM。

### 冒烟测试（无需 Minecraft / JVM）

```bat
g++ -O2 -std=c++17 -static-libgcc -static-libstdc++ -s -o tools\smoke_host.exe tools\smoke_host.cpp -luser32
tools\smoke_host.exe

g++ -O2 -std=c++17 -static-libgcc -static-libstdc++ -s -Iinclude -Iinclude\win32 -o tools\smoke_mouse.exe tools\smoke_mouse.cpp src\common.cpp src\overlay.cpp -luser32 -lgdi32
tools\smoke_mouse.exe

g++ -O2 -std=c++17 -static-libgcc -static-libstdc++ -s -o tools\smoke_target.exe tools\smoke_target.cpp -luser32
tools\smoke_target.exe
mc_esp.exe -pid <smoke_target 打印的 PID> -dir build_tmp\smoke_data
```

---

## 目录结构

```
.
├── build.bat                一键构建单文件 mc_esp.exe
├── mc_esp.exe               单文件自注入 EXE（构建产物）
├── include/                 JNI/JVMTI 头文件
├── loader/
│   └── loader.cpp           单文件加载器：解包 payload + 目录握手 + 注入
├── src/
│   ├── common.h/.cpp        宽路径、日志、esp.ini 读写
│   ├── clicker.h/.cpp       连点器核心（PostMessage + 拟人化节奏）
│   ├── dllmain.cpp          DLL 入口 / PEB 隐藏 / 加载器目录握手
│   ├── esp.cpp              主循环 + Insert 菜单 + 覆盖层宿主
│   ├── glrender.h/.cpp      gdi32!SwapBuffers 钩子
│   ├── jvm.h/.cpp           JVM 附加 / 符号解析 / 实体与战斗状态采集
│   ├── math3d.h             3D 投影
│   ├── overlay.h/.cpp       GDI 透明覆盖层绘制
│   └── tebtest*.cpp         TEB / 线程起点伪装验证小程序（不参与构建）
└── tools/
    ├── bin2h.cpp            DLL -> payload.h 转换工具
    ├── smoke_host.cpp       中间 DLL 直接加载冒烟测试
    ├── smoke_mouse.cpp      真实鼠标消息冒烟测试
    └── smoke_target.cpp     单文件 EXE 注入目标冒烟测试
```

---

## 配置

`mc_esp.exe` 启动时从数据目录读取 `esp.ini`（默认 `%APPDATA%\mc_esp\`）；
不存在时自动生成默认配置。
默认：**ESP 关闭**、**左右键连点开启**、**连点器总开关关闭**。

- `[esp]`：ESP 渲染参数；`menuKey` 菜单键、`espKey` ESP 快捷键；
  `maxDistance / box3d / box2d / nameTags / tracer / filledBox / lineWidth /
  showPlayers / showMobs / showOthers / showTrajectory / trajectoryTicks /
  renderHz` 等。
- `[clicker]`：当前激活方案、`profileKey`（方案循环热键）。
- `[clickerProfile1]`..`[clickerProfile4]`：4 套连点器方案槽，含
  `inGameGate`（游戏内门控）。
- `[colors]`：ESP 颜色。

数值范围：

- 左右键 CPS：0.5..50（`cpsLeft10` / `cpsRight10` 为 CPS×10）
- `cpsMax`：20..500
- `randomRange`：1..5
- `humanizeMode`：0 均匀 / 1 双击 / 2 呼吸 / 3 疲劳
- `humanizeLevel`：1..5
- `autoStopSeconds`：1..3600
- ESP 最大距离：10..500
- ESP 线宽：1..5
- 轨迹 tick：5..63

---

## 安全说明

- DLL 会在 DllMain 中从 PEB 模块链表隐藏自身，并使用 `RtlUserThreadStart`
  伪装线程起点；ESP / 连点器门控数据采集在游戏自己的渲染线程内完成
  （`gdi32!SwapBuffers` 钩子 + 复用已有 JNIEnv），不新建附加到 JVM 的线程。
- 连点线程只向本进程的游戏窗口 `PostMessage`，不接触 JVM。
- 本项目仅供学习与技术研究。注入类工具可能违反部分服务器规则，请自行评估
  使用环境与账号风险。

## License

MIT
