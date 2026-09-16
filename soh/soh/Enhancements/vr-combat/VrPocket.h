#pragma once

// The item POCKET (VR first person, selector mode): a presented item floats at a fixed point in
// front of the player (chest height, 0.4 m ahead along the flattened view direction) as a real,
// shrunken 3D model; a hand within reach that squeezes grip takes it, and from then on the
// model is rigidly parented to that hand exactly as it was taken. Shared by the bombchu
// (VrItemThrow.cpp, headset-verified) and the boomerang (VrBoomerang.cpp); the geometry lives
// here once so the two never drift apart.
//
// Requires the game headers (z64.h) to be included by the caller first, for Vec3s.

namespace VrPocket {

// Game units per real-world meter (falls back to 35 when VR reports none).
float WorldScale();

// Rotate v by the unit quaternion q (x, y, z, w) / by its conjugate (world -> the controller's
// own frame).
void QuatRot(const float q[4], const float v[3], float out[3]);
void QuatRotInv(const float q[4], const float v[3], float out[3]);

// The pocket point (world, game units): eye + flattened forward x 0.4 m, 0.25 m below the eye.
// Looking up/down must not move it out of reach. Ungated: callers apply their own item gates.
bool Point(float out[3]);

// The presented model's pose: level, forward = flattened camera forward, up = world +Y,
// left = (fz, 0, -fx) (the bombchu's own axis convention: +X left, +Y up, +Z forward).
bool Axes(float left[3], float up[3], float forward[3]);

// The hand's controller is within grab reach (15 cm) of the point.
bool HandNear(int hand, const float point[3]);

// Carried pose when no grab transform exists (a hold restored without an observed grab, or a
// catch straight out of the air): at the hand, model left = controller -X, up = +Y, forward =
// controller forward (grip-local -Z, the same axis the bottle mouth uses).
void DefaultCarryLocal(float offset[3], float left[3], float up[3], float forward[3]);

// Rigid grab transform: where the presented model sat relative to the grabbing hand (position
// offset and its three model axes, all in the controller's own frame), captured at the grab
// and replayed on the live hand pose every tick, so the model is parented to the hand exactly
// as it was taken and the angle you let go at is the angle it leaves at.
struct Grab {
    bool valid = false;
    float offsetLocal[3] = {};
    float leftLocal[3] = {};
    float upLocal[3] = {};
    float forwardLocal[3] = {};

    // Record the presented pose (point + axes, world) in the hand's frame. valid stays false
    // when the hand is untracked.
    void Capture(int hand, const float point[3], const float left[3], const float up[3], const float forward[3]);
    // The carried pose this tick on the current hand pose (DefaultCarryLocal when !valid).
    // False when the hand is untracked.
    bool Replay(int hand, float position[3], float left[3], float up[3], float forward[3]) const;
    void Clear() { valid = false; }
};

// Axis frame -> shape.rot, decomposed the same way EnBomChu_UpdateFloorPoly turns its axis
// frame into a rotation (Matrix_MtxFToYXZRotS), so visuals and any actor frame agree.
void SetRotFromAxes(Vec3s* rot, const float left[3], const float up[3], const float forward[3]);

} // namespace VrPocket
