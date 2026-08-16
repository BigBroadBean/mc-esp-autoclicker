#pragma once
// ============================================================
//  glrender.h — 游戏内直绘数据采集钩子
//  钩住 gdi32!SwapBuffers：游戏每帧在【已附加 JVM 的渲染线程】上调用，
//  在钩子内用 GetEnv 复用该线程的 JNIEnv 采集实体数据，从而
//  完全避免“新建原生线程 + AttachCurrentThread”触发在线反作弊检测。
// ============================================================

// 安装 SwapBuffers 钩子（幂等）。成功返回 true。
bool gl_install_hook();
// 卸载钩子：恢复 gdi32!SwapBuffers 原始字节（卸载 DLL 前必须调用）。
void gl_remove_hook();
// 等待游戏渲染线程彻底离开本模块的 SwapHook（含 g_realSwap 调用）。
// 卸载流程在 gl_remove_hook() 之后调用它：钩子已还原，不再有新的 SwapHook
// 进入，这里只需等当前在途的一次调用跑完（计数归零），返回后即可安全
// 注销 .pdata / 释放映射内存，杜绝“游戏线程仍在执行已释放模块代码”的崩溃。
void gl_wait_hook_idle(DWORD timeoutMs);
