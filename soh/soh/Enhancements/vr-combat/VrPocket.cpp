extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
}
#include "VrPocket.h"
#include <vr_interface.h>
#include <cmath>

// Extracted verbatim from VrItemThrow.cpp (bombchu pocket, headset-verified September 16, 2026)
// so the boomerang's pocket is the same pocket. Any tuning here moves both items.

namespace VrPocket {

float WorldScale() {
    const float scale = VR_GetWorldScale();
    return scale < 1.0f ? 35.0f : scale;
}

void QuatRot(const float q[4], const float v[3], float out[3]) {
    const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
    const float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
    const float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
    out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
    out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
    out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

void QuatRotInv(const float q[4], const float v[3], float out[3]) {
    const float conj[4] = { -q[0], -q[1], -q[2], q[3] };
    QuatRot(conj, v, out);
}

bool Point(float out[3]) {
    float eye[3], forward[3], up[3];
    VR_GetCameraPose(eye, forward, up);
    const float scale = WorldScale();
    // Stable chest-height presentation in front of the player; looking up/down must
    // not move the target out of reach. Exact distances remain headset-tunable.
    const float length = std::sqrt(forward[0] * forward[0] + forward[2] * forward[2]);
    if (length < 0.01f) return false;
    out[0] = eye[0] + forward[0] / length * scale * 0.4f;
    out[1] = eye[1] - scale * 0.25f;
    out[2] = eye[2] + forward[2] / length * scale * 0.4f;
    return true;
}

bool Axes(float left[3], float up[3], float forward[3]) {
    float eye[3], camForward[3], camUp[3];
    VR_GetCameraPose(eye, camForward, camUp);
    const float length = std::sqrt(camForward[0] * camForward[0] + camForward[2] * camForward[2]);
    if (length < 0.01f) return false;
    forward[0] = camForward[0] / length;
    forward[1] = 0.0f;
    forward[2] = camForward[2] / length;
    up[0] = 0.0f;
    up[1] = 1.0f;
    up[2] = 0.0f;
    left[0] = forward[2];
    left[1] = 0.0f;
    left[2] = -forward[0];
    return true;
}

bool HandNear(int hand, const float point[3]) {
    float position[3], rotation[4];
    if (!VR_GetHandPose(hand, position, rotation)) return false;
    float distanceSq = 0.0f;
    for (int axis = 0; axis < 3; ++axis) {
        const float d = position[axis] - point[axis];
        distanceSq += d * d;
    }
    const float reach = WorldScale() * 0.15f;
    return distanceSq <= reach * reach;
}

void DefaultCarryLocal(float offset[3], float left[3], float up[3], float forward[3]) {
    offset[0] = offset[1] = offset[2] = 0.0f;
    left[0] = -1.0f; left[1] = 0.0f; left[2] = 0.0f;
    up[0] = 0.0f;    up[1] = 1.0f;   up[2] = 0.0f;
    forward[0] = 0.0f; forward[1] = 0.0f; forward[2] = -1.0f;
}

void Grab::Capture(int hand, const float point[3], const float left[3], const float up[3], const float forward[3]) {
    valid = false;
    float handPos[3], handRot[4];
    if (!VR_GetHandPose(hand, handPos, handRot)) return;
    const float offsetWorld[3] = { point[0] - handPos[0], point[1] - handPos[1], point[2] - handPos[2] };
    QuatRotInv(handRot, offsetWorld, offsetLocal);
    QuatRotInv(handRot, left, leftLocal);
    QuatRotInv(handRot, up, upLocal);
    QuatRotInv(handRot, forward, forwardLocal);
    valid = true;
}

bool Grab::Replay(int hand, float position[3], float left[3], float up[3], float forward[3]) const {
    float handPos[3], handRot[4];
    if (!VR_GetHandPose(hand, handPos, handRot)) return false;
    float offLocal[3], lLocal[3], uLocal[3], fLocal[3];
    if (valid) {
        for (int i = 0; i < 3; ++i) {
            offLocal[i] = offsetLocal[i];
            lLocal[i] = leftLocal[i];
            uLocal[i] = upLocal[i];
            fLocal[i] = forwardLocal[i];
        }
    } else {
        DefaultCarryLocal(offLocal, lLocal, uLocal, fLocal);
    }
    float offset[3];
    QuatRot(handRot, offLocal, offset);
    for (int i = 0; i < 3; ++i) position[i] = handPos[i] + offset[i];
    QuatRot(handRot, lLocal, left);
    QuatRot(handRot, uLocal, up);
    QuatRot(handRot, fLocal, forward);
    return true;
}

void SetRotFromAxes(Vec3s* rot, const float left[3], const float up[3], const float forward[3]) {
    MtxF mf{};
    // Columns (xx,yx,zx | xy,yy,zy | xz,yz,zz) = images of model X, Y, Z.
    mf.xx = left[0];    mf.yx = left[1];    mf.zx = left[2];
    mf.xy = up[0];      mf.yy = up[1];      mf.zy = up[2];
    mf.xz = forward[0]; mf.yz = forward[1]; mf.zz = forward[2];
    Matrix_MtxFToYXZRotS(&mf, rot, 0);
}

} // namespace VrPocket
