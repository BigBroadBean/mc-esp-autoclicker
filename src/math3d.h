#pragma once
// ============================================================
//  math3d.h — 3D 数学：世界坐标 → 屏幕坐标投影
//
//  采用与 Minecraft 相机一致的正向向量公式：
//    f = (-sin(yaw)*cos(pitch), -sin(pitch), cos(yaw)*cos(pitch))
//  由 f 构造相机基（right/up），将世界点变换到相机空间后透视投影。
// ============================================================
#include <cmath>
#include <cstdint>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

inline double mc_forward_x(double yaw_deg, double pitch_deg) {
    double yaw = yaw_deg * M_PI / 180.0, p = pitch_deg * M_PI / 180.0;
    return -std::sin(yaw) * std::cos(p);
}
inline double mc_forward_y(double pitch_deg) {
    return -std::sin(pitch_deg * M_PI / 180.0);
}
inline double mc_forward_z(double yaw_deg, double pitch_deg) {
    double yaw = yaw_deg * M_PI / 180.0, p = pitch_deg * M_PI / 180.0;
    return std::cos(yaw) * std::cos(p);
}

// 世界点投影到屏幕。
// 返回 true 且 sx/sy 在屏幕范围内（未裁剪，只保证点在相机前方）。
inline bool world_to_screen(double wx, double wy, double wz,
                            double cx, double cy, double cz,
                            double yaw_deg, double pitch_deg,
                            float fov_deg, int screen_w, int screen_h,
                            float& sx, float& sy) {
    double fx = mc_forward_x(yaw_deg, pitch_deg);
    double fy = mc_forward_y(pitch_deg);
    double fz = mc_forward_z(yaw_deg, pitch_deg);

    // right = normalize(f × up)，up=(0,1,0) → f × up = (-fz, 0, fx)
    double rx = -fz, ry = 0.0, rz = fx;
    double rl = std::sqrt(rx * rx + rz * rz);
    if (rl < 1e-9) return false;
    rx /= rl; rz /= rl;

    // up = r × f
    double ux = -rz * fy;
    double uy =  rz * fx - rx * fz;
    double uz =  rx * fy;

    double dx = wx - cx, dy = wy - cy, dz = wz - cz;

    double camX = dx * rx + dy * ry + dz * rz;
    double camY = dx * ux + dy * uy + dz * uz;
    double camZ = dx * fx + dy * fy + dz * fz;

    if (camZ < 0.01) return false;

    double tanHalf = std::tan(fov_deg * 0.5 * M_PI / 180.0);
    double aspect = (double)screen_w / (double)screen_h;

    double sx_n = camX / camZ / tanHalf / aspect;
    double sy_n = camY / camZ / tanHalf;

    sx = (float)((sx_n + 1.0) * 0.5 * screen_w);
    sy = (float)((1.0 - sy_n) * 0.5 * screen_h);
    return true;
}

// ------------------------------------------------------------
// 预计算相机基（right/up/forward + 投影常量），一帧算一次，
// 避免每个实体、每个角点都重复三角函数。
// ------------------------------------------------------------
struct CamBasis {
    double rx, ry, rz;    // right
    double ux, uy, uz;    // up
    double fx, fy, fz;    // forward
    double tanHalf, aspect;
    double halfW, halfH;
};

inline bool cam_basis(double yaw_deg, double pitch_deg,
                      float fov_deg, int screen_w, int screen_h,
                      CamBasis& b) {
    b.fx = mc_forward_x(yaw_deg, pitch_deg);
    b.fy = mc_forward_y(pitch_deg);
    b.fz = mc_forward_z(yaw_deg, pitch_deg);

    // 相机 up = 游戏旋转 R_y(-yaw)·R_x(pitch) 作用于世界 (0,1,0)。
    // 正常视角下与旧实现（right×forward）完全一致；在极点（pitch≈±90°）
    // 时精确复现游戏随 yaw 变化的 roll，保证盒子在极点处位置正确。
    // 推导依据：Minecraft Camera.setRotation 用同一四元数，getUpVector 即
    // R·(0,1,0)；而 forward = R·(0,0,1) 与本文件 mc_forward_* 逐位一致。
    const double yaw   = yaw_deg   * M_PI / 180.0;
    const double pitch = pitch_deg * M_PI / 180.0;
    const double sp = std::sin(pitch), cp = std::cos(pitch);
    const double sy = std::sin(yaw),   cy = std::cos(yaw);
    b.ux = -sy * sp;
    b.uy =  cp;
    b.uz =  cy * sp;

    // right = forward × up（正常视角与旧实现 (-fz,0,fx) 完全一致）
    double rx = b.fy * b.uz - b.fz * b.uy;
    double ry = b.fz * b.ux - b.fx * b.uz;
    double rz = b.fx * b.uy - b.fy * b.ux;
    double rl = std::sqrt(rx * rx + ry * ry + rz * rz);
    if (rl < 1e-9) return false;   // 防御性兜底（正常情况不会触发）
    b.rx = rx / rl; b.ry = ry / rl; b.rz = rz / rl;

    b.tanHalf = std::tan(fov_deg * 0.5 * M_PI / 180.0);
    b.aspect  = (double)screen_w / (double)screen_h;
    b.halfW   = (double)screen_w * 0.5;
    b.halfH   = (double)screen_h * 0.5;
    return true;
}

// 世界点 → 相机空间（camZ < 0.01 表示在相机后方）。
// 一次算出三轴分量，供有效性判断、近平面裁剪与投影复用。
inline void cam_to_camspace(const CamBasis& b,
                            double cx, double cy, double cz,
                            double wx, double wy, double wz,
                            double& camX, double& camY, double& camZ) {
    double dx = wx - cx, dy = wy - cy, dz = wz - cz;
    camX = dx * b.rx + dy * b.ry + dz * b.rz;
    camY = dx * b.ux + dy * b.uy + dz * b.uz;
    camZ = dx * b.fx + dy * b.fy + dz * b.fz;
}

// 相机空间 → 屏幕（camZ 必须 > 0.01，否则返回 false 不写 sx/sy）
inline bool camspace_to_screen(const CamBasis& b,
                               double camX, double camY, double camZ,
                               float& sx, float& sy) {
    if (camZ < 0.01) return false;
    double sx_n = camX / camZ / b.tanHalf / b.aspect;
    double sy_n = camY / camZ / b.tanHalf;
    sx = (float)((sx_n + 1.0) * b.halfW);
    sy = (float)((1.0 - sy_n) * b.halfH);
    return true;
}

// 用预计算基投影单个世界点（等价于 world_to_screen，但省去重复三角函数）。
inline bool cam_project(const CamBasis& b,
                        double cx, double cy, double cz,
                        double wx, double wy, double wz,
                        float& sx, float& sy) {
    double camX, camY, camZ;
    cam_to_camspace(b, cx, cy, cz, wx, wy, wz, camX, camY, camZ);
    return camspace_to_screen(b, camX, camY, camZ, sx, sy);
}
