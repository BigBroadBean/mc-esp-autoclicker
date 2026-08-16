# MC ESP + 内置连点器

Minecraft 1.20.1 Forge 47.4.10 注入式 **ESP + 连点器** 一体化 DLL。

- 注入方式沿用本项目的 `injector.exe`（`CreateRemoteThread + LoadLibraryW`）
- ESP 绘制沿用本项目的 GDI 透明覆盖层（截图排除，游戏内直绘）
- 连点器功能移植自 `AutoClicker-main`
- 攻击/放置门控逻辑移植自 `MCCombatStatusJni`，但**不再发送 UDP / 共享内存**，
  由 DLL 在游戏渲染线程内直接读取并消费

> 已移除：自瞄（Aimbot）、多倍点击（multi-click）、滚轮点击（scroll-to-click）、
> 外部渲染共享内存（esp_shm）与外部消息传递。

---

## 功能

### ESP

- 玩家 / 生物 / 其他实体过滤
- 3D 包围盒（穿墙）、名字标签、Tracer、半透明填充
- 弹射物轨迹（箭 / 雪球 / 末影珍珠）
- 本地与其他玩家弓蓄力预判 + 落点标记
- 帧间平滑、聊天界面继续显示、失焦自动隐藏

### 连点器

- 4 套配置方案（Profiles），菜单内一键切换，整套连点参数独立保存
- 左右键独立连点，CPS 独立可调（0.1 CPS 精度）
- 6 / 10 / 15 / 20 / 30 / 40 CPS 快捷预设（左右键独立）
- 保持模式：无需按住鼠标即可连点
- 随机 CPS 波动（±1..±5）
- 拟人化节奏：均匀 / 双击连招 / 呼吸波动 / 疲劳递减，强度 1..5
- CPS 上限（20..500）
- 定时自动停止
- 光标门控：系统光标可见（背包 / 聊天 / 菜单）时暂停连点
- 仅能攻击时左键连点（准星目标可攻击才点左键）
- 仅手持方块时右键连点（主手物品为 `BlockItem` 才点右键）
- 连点 / 攻击门控 / 放置门控热键均可绑定
- 实时 CPS 与累计点击统计显示在菜单标题栏

攻击门控判定逻辑与 `MCCombatStatusJni` 的 `forge1201` 映射一致：

```
canAttack = hitResult.getType() == ENTITY
         && target instanceof LivingEntity
         && target.isAlive()
         && target != 本地玩家
         && target.isAttackable()

canPlace  = player.getMainHandItem().getItem() instanceof BlockItem
```

---

## 使用

1. 启动 Minecraft（1.20.1 Forge 47.4.10，64 位 Java），进入世界。
2. 运行 `Injector\injector.exe`：
   ```
   injector.exe                自动查找 Minecraft 并注入
   injector.exe -pid <PID>     注入指定 PID
   ```
3. 注入成功后按 **`Insert`** 呼出 / 关闭连点器菜单。

### Insert 菜单操作

| 输入 | 功能 |
| --- | --- |
| `Insert` | 呼出 / 关闭菜单 |
| 鼠标左键 | 点击标签页 / 行 / 布尔项；拖拽滑块；点面板外关闭菜单 |
| 鼠标滚轮 | 当前页内上 / 下选择项目 |
| 鼠标悬停 | 0.35 秒后显示当前功能的悬浮说明 |
| `Tab` | 循环切换功能页 |
| `↑` / `↓` | 当前页内选择项目 |
| `←` / `→` | 调整数值 / 循环选项 / 切换布尔项 |
| `Shift + ←/→` | 数值快速调整（CPS、CPS 上限、停止秒数） |
| `Enter` | 切换布尔项；在热键项目上按 Enter 后按任意键绑定 |
| `Esc` | 关闭菜单 / 取消热键绑定 |

菜单打开期间覆盖层会临时截获鼠标，同时自动暂停连点，关闭菜单后恢复鼠标穿透。

鼠标消息由 Overlay 的真实 `WndProc` 接收（`WM_LBUTTONDOWN/UP`、`WM_MOUSEMOVE`、
`WM_MOUSEWHEEL`、`WM_MOUSELEAVE`），不再使用 `GetAsyncKeyState` 模拟鼠标。

菜单按功能分页，子功能归入父功能页：

- **连点**：连点器总开关、左/右键开关、左/右键 CPS 与预设、保持模式、连点热键
- **门控**：攻击门控及其热键、放置门控及其热键、光标门控
- **高级**：随机 CPS、随机范围、拟人化节奏与强度、CPS 上限、定时停止
- **系统**：ESP 开关、配置方案

带滑块的菜单项：左/右键 CPS、随机范围、拟人化强度、CPS 上限、停止秒数。

菜单项：连点器总开关、ESP 开关、配置方案、左/右键连点、左/右键 CPS 与预设、
保持模式、连点热键、攻击门控、放置门控、光标门控、随机 CPS、随机范围、
拟人化模式与强度、CPS 上限、定时停止、门控热键。

默认热键：

| 功能 | 默认按键 |
| --- | --- |
| 菜单 | `Insert` |
| 连点器启停 | 鼠标中键（`VK_MBUTTON`） |
| 攻击门控 | `F6` |
| 放置门控 | `F7` |

菜单中的修改会立即写入 DLL 同目录的 `esp.ini`。

### 性能优化

- 菜单使用离屏像素缓存：静态时每帧仅 memcpy 面板区域，不再重画 20+ 行文字与填充。
- 只有菜单状态 / 鼠标悬停 / 拖拽变化时才重绘缓存，动态状态每 250ms 刷新一次。
- `renderHz`（默认 120）现在真正驱动菜单宿主线程，并改用 Win10 高精度可等待定时器，
  实测约 116Hz，避免 `Sleep(8)` 被系统按 15.6ms 节拍合并回 60Hz。
- ESP / 菜单 / 门控全关时 SwapBuffers 钩子只做原子判断，不再触碰 JVM；
  JVM 符号会在空闲时每秒预热一次，避免首次开菜单时卡顿。

---

## 构建

环境：Windows + MinGW-w64 x86_64 g++（本项目开发环境为 GCC 15.2）。

```bat
build.bat
```

产物：

- `mc_esp.dll`
- `Injector\mc_esp.dll`
- `Injector\injector.exe`

`include\` 内为 JNI / JVMTI 头文件，项目编译时通过 `-Iinclude -Iinclude\win32` 引用。
DLL 不链接 `jvm.dll`，运行时通过 `JNI_GetCreatedJavaVMs` 动态定位 JVM。

---

## 目录结构

```
.
├── build.bat                一键构建
├── mc_esp.dll               注入 DLL（构建产物）
├── include/                 JNI/JVMTI 头文件
├── src/
│   ├── common.h/.cpp        宽路径、日志、esp.ini 读写
│   ├── clicker.h/.cpp       连点器核心（PostMessage + 拟人化节奏）
│   ├── dllmain.cpp          DLL 入口 / PEB 隐藏 / 卸载
│   ├── esp.cpp              主循环 + Insert 菜单 + 覆盖层宿主
│   ├── glrender.h/.cpp      gdi32!SwapBuffers 钩子
│   ├── jvm.h/.cpp           JVM 附加 / 符号解析 / 实体与战斗状态采集
│   ├── math3d.h             3D 投影
│   ├── overlay.h/.cpp       GDI 透明覆盖层绘制
│   └── tebtest*.cpp         TEB / 线程起点伪装验证小程序（不参与 DLL 构建）
├── Injector/
│   ├── injector.cpp         注入器源码
│   ├── build.bat            单独重编注入器
│   ├── esp.ini              默认配置（随 DLL 同目录读取）
│   ├── injector.exe         注入器（构建产物）
│   └── mc_esp.dll           随注入器分发的 DLL（构建产物）
└── tools/
    ├── smoke_host.cpp       无 JVM 冒烟测试宿主（GLFW30 假窗口 + LoadLibrary）
    └── smoke_mouse.cpp      真实鼠标消息冒烟测试（WM_LBUTTONDOWN/UP/MOVE/WHEEL）
```

---

## 配置

DLL 启动时读取自身同目录 `esp.ini`；不存在时自动生成默认配置。

- `[esp]`：ESP 渲染参数，`menuKey` 为菜单呼出键；`renderHz` 使用高精度可等待定时器驱动宿主循环。
- 默认：**ESP 关闭**、**左右键连点开启**、**连点器总开关关闭**。
- `[clicker]`：当前激活方案与热键，`activeProfile` 为当前方案（0..3）。
- `[clickerProfile1]`..`[clickerProfile4]`：4 套连点器方案槽。
- `[colors]`：ESP 颜色。

数值范围：

- 左右键 CPS：0.5..50（`cpsLeft10` / `cpsRight10` 为 CPS×10）
- `cpsMax`：20..500
- `randomRange`：1..5
- `humanizeMode`：0 均匀 / 1 双击 / 2 呼吸 / 3 疲劳
- `humanizeLevel`：1..5
- `autoStopSeconds`：1..3600

---

## 安全说明

- DLL 会在 DllMain 中从 PEB 模块链表隐藏自身，并使用 `RtlUserThreadStart`
  伪装线程起点；ESP / 连点器门控的数据采集在游戏自己的渲染线程内完成
  （`gdi32!SwapBuffers` 钩子 + 复用已有 JNIEnv），不新建附加到 JVM 的线程。
- 连点线程只向本进程的游戏窗口 `PostMessage`，不接触 JVM。
- 本项目仅供学习与技术研究。注入类工具可能违反部分服务器规则，请自行评估
  使用环境与账号风险。

## License

MIT
