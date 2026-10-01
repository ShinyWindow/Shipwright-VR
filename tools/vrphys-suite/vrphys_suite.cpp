// Scenario suite for the VR held-object physics. Each case drives the hand for 300 steps and
// reports the mean per-step blade movement once the hand is held still (or moving smoothly) —
// a stable sim should read ~0 mm for held cases and show no high-frequency component otherwise.
#include "fast/vr_physics.cpp"

#include <stdio.h>
#include <math.h>

static const float kScale = 35.0f;

static VrPhysContactPrim MakeTri(float ax, float ay, float az, float bx, float by, float bz, float cx,
                                 float cy, float cz, int id) {
    VrPhysContactPrim p{};
    p.type = VRPHYS_PRIM_TRI;
    p.a[0] = ax; p.a[1] = ay; p.a[2] = az;
    p.b[0] = bx; p.b[1] = by; p.b[2] = bz;
    p.c[0] = cx; p.c[1] = cy; p.c[2] = cz;
    p.radius = 0.0f;
    p.id = id;
    return p;
}

// Deterministic pseudo-noise standing in for controller tracking jitter (~0.3 mm).
static float Noise(int i, int salt) {
    const int n = (i * 1103515245 + salt * 12345 + 7919) & 0x7FFFFFFF;
    return ((float)(n % 2000) / 1000.0f - 1.0f) * 0.0003f;
}

struct Result {
    float heldJitterMm;
    float maxStepMm;
    int minContacts;
    int maxContacts;
    int zeroContactSteps; // steps with NO constraint while pressed — the lunge window
    float gripDevMm;      // max |grip - hand| over the hold: pivot-only promises ~0
};

// mode: 0 = tip into corner, 1 = blade CENTER across the corner edge, 2 = slide along the corner,
//       3 = FLAT WALL with the hand driven deep PAST the surface (what a player actually does:
//           the real hand is unconstrained and ends up inside the wall, so the spring runs at its
//           acceleration ceiling the whole time), 4 = same but the wrist also rotates
static float gTrailDeg = 0.0f; // blade angle lag vs hand orientation at end of run (friction probe)
static float gTipLagMm = 0.0f; // tip lag along the slide direction at end of run (friction probe)

static Result RunCase(float hz, int mode, bool handNoise, float angFreq = 30.0f,
                      float maxAngAccel = 3000.0f, float maxAccel = 400.0f, float friction = 0.15f,
                      float passSpeed = 0.0f) {
    vrphys_reset();
    const float turn[4] = { 0, 0, 0, 1 };
    const float turnOff[3] = { 0, 0, 0 };
    const float anchor[3] = { 0, 0, 0 };

    VrPhysContactPrim prims[4];
    if (mode == 8 || mode == 10 || mode == 11) {
        // Ledge: floor at y=0 for x <= 35, then a sheer drop (side face at x=35, facing +x).
        prims[0] = MakeTri(-70, 0, -70, -70, 0, 70, 35, 0, -70, 10);
        prims[1] = MakeTri(35, 0, 70, 35, 0, -70, -70, 0, 70, 10);
        prims[2] = MakeTri(35, 0, -70, 35, 0, 70, 35, -70, -70, 11);
        prims[3] = MakeTri(35, -70, 70, 35, -70, -70, 35, 0, 70, 11);
    } else {
        prims[0] = MakeTri(35, -70, -70, 35, -70, 70, 35, 70, -70, 1);
        prims[1] = MakeTri(35, 70, 70, 35, 70, -70, 35, -70, 70, 1);
        prims[2] = MakeTri(-70, -70, 35, -70, 70, 35, 70, -70, 35, 2);
        prims[3] = MakeTri(70, 70, 35, 70, -70, 35, -70, 70, 35, 2);
    }
    vrphys_set_contact_prims(prims, 4);

    // Mirrors the game's live defaults (VrSwing.cpp) so the harness tests what actually ships.
    VrPhysObjectDesc desc{};
    desc.primary_hand = 1;
    desc.secondary_hand = -1;
    desc.lin_freq_hz = 14.0f;
    desc.lin_zeta = 1.0f;
    desc.ang_freq_hz = angFreq;
    desc.ang_zeta = 1.0f;
    desc.max_accel_mps2 = maxAccel;
    desc.blade_radius_m = 0.4f / kScale;
    desc.touch_tolerance_m = 0.3f / kScale;
    desc.max_ang_accel = maxAngAccel;
    desc.pivot_only = true; // mirrors the game default (gVrPhysPivotOnly 1)
    // Flat blade: 4 game units wide (the Blade Width default), width along local Z,
    // pointed over the last 20% — mirrors the game's flat-rectangle collider.
    desc.grip_local_edge_m[2] = 2.0f / kScale;
    desc.tip_taper_frac = 0.2f;
    desc.passthrough_speed_mps = passSpeed;
    if (mode == 1 || mode == 3 || mode == 4 || mode == 5 || mode == 6 || mode == 7 || mode == 8 ||
        mode == 9 || mode == 10 || mode == 11 || mode == 12) {
        // Blade lies along +X, pointing at wall A.
        desc.grip_local_tip_m[0] = 1.13f;
    } else {
        desc.grip_local_tip_m[0] = 0.8f;
        desc.grip_local_tip_m[2] = 0.8f;
    }
    desc.contact_enabled = true;
    desc.friction = friction;
    vrphys_set_object(VRPHYS_SLOT_WEAPON, &desc);

    const float dt = 1.0f / hz;
    const uint64_t dtns = (uint64_t)(dt * 1e9f);
    uint64_t t = 0;
    float hx = 0, hy = 0, hz_ = 0;
    if (mode == 1) {
        hx = 0.30f;
        hz_ = 0.50f; // start near wall B, blade pointing +X toward wall A
    } else if (mode == 3 || mode == 4) {
        hx = -0.60f; // blade tip starts short of wall A (x = 1.0)
        hz_ = -0.50f;
    } else if (mode == 5 || mode == 6 || mode == 7) {
        hx = -0.60f;
        hz_ = (mode == 7) ? 0.30f : -0.50f; // mode 7 also jams into wall B
    } else if (mode == 8) {
        hx = -0.55f;
        hy = 0.45f; // blade pitched 25 deg down: tip drags the floor, then crosses the lip
    } else if (mode == 9) {
        hx = 0.0f; // tip 13cm past the wall plane: pressed via pivot from the start
        hz_ = -0.50f;
    } else if (mode == 10 || mode == 11) {
        hx = -0.55f; // over the mode-8 floor, blade level: press straight down onto it
        hy = 0.05f;  // just above the surface; the press drives 19cm below it
        hz_ = -0.30f;
    } else if (mode == 12) {
        hx = 0.20f; // tip 33cm past the wall plane: a fast sweep must pass, a rest must pivot
        hz_ = -1.20f;
    }
    float quat[4] = { 0, 0, 0, 1 };
    if (mode == 8) {
        const float half = -25.0f * 3.14159265f / 180.0f * 0.5f;
        quat[2] = sinf(half);
        quat[3] = cosf(half);
    } else if (mode == 11) {
        // EDGE-ON: roll 90 deg about the blade axis so the thin edge faces the floor.
        const float half = 90.0f * 3.14159265f / 180.0f * 0.5f;
        quat[0] = sinf(half);
        quat[3] = cosf(half);
    }

    Result r{ 0, 0, 99, 0, 0, 0.0f };
    float prev[3] = { 0, 0, 0 };
    float pd[3] = { 0, 0, 0 };
    bool haveD = false;
    bool havePrev = false;
    double sum = 0;
    int samples = 0;
    const int drive = 90;

    for (int step = 0; step < 300; step++) {
        if (step < drive) {
            if (mode == 1) {
                hz_ += 0.006f; // press the blade's SIDE into wall B, tip near the corner
                hx += 0.004f;
            } else if (mode == 3 || mode == 4) {
                hx += 0.010f; // drive the HAND straight on and well past the wall plane
            } else if (mode == 5 || mode == 6 || mode == 7) {
                hx += 0.045f; // SHOVE: hand ends ~2.5 m beyond the wall, an absurd push
                if (mode == 7) {
                    hz_ += 0.010f;
                }
            } else if (mode == 8) {
                hx += 0.010f; // drag forward; the tip passes the lip around step 53
            } else if (mode == 10 || mode == 11) {
                // no drive phase: hold level over the floor, then press down
            } else if (mode == 12) {
                hz_ += 0.030f; // 2.7 m/s lateral sweep, tip buried past the wall plane
            } else {
                hx += 0.006f;
                hz_ += 0.006f;
            }
        } else if (mode == 2) {
            hy += 0.004f; // slide along the corner crease while pressed in
        } else if (mode == 8 && step < 150) {
            hy -= 0.005f; // press down onto the lip once the tip hangs past it
        } else if (mode == 9 && step >= drive) {
            hy += 0.004f; // drag the pressed tip sideways along the wall
        } else if ((mode == 10 || mode == 11) && step < 150) {
            hy -= 0.004f; // press the level blade down onto the floor (ends 30cm below it)
        }
        // Hand velocities must be CONSISTENT with the hand's motion — the runtime reports real
        // measured velocities, and the spring's damping term tracks them. Feeding zeros while
        // the pose moves makes the damper fight the motion, which is a property of the test
        // rather than of the game.
        float ang[3] = { 0, 0, 0 };
        if (mode == 4 || mode == 6 || mode == 7) {
            // Wrist rotation while pressed: yaw back and forth ~0.3 rad at ~1.5 Hz.
            const float w = 9.4f;
            const float a = 0.3f * sinf((float)step * dt * w);
            quat[0] = 0.0f;
            quat[1] = sinf(a * 0.5f);
            quat[2] = 0.0f;
            quat[3] = cosf(a * 0.5f);
            ang[1] = 0.3f * w * cosf((float)step * dt * w); // d(angle)/dt about Y
        }
        float pos[3] = { hx, hy, hz_ };
        if (handNoise) {
            pos[0] += Noise(step, 1);
            pos[1] += Noise(step, 2);
            pos[2] += Noise(step, 3);
        }
        float lin[3] = { 0, 0, 0 };
        if (step < drive) {
            if (mode == 1) {
                lin[0] = 0.004f / dt;
                lin[2] = 0.006f / dt;
            } else if (mode == 3 || mode == 4) {
                lin[0] = 0.010f / dt;
            } else if (mode == 5 || mode == 6 || mode == 7) {
                lin[0] = 0.045f / dt;
                if (mode == 7) {
                    lin[2] = 0.010f / dt;
                }
            } else if (mode == 8) {
                lin[0] = 0.010f / dt;
            } else if (mode == 10 || mode == 11) {
                // stationary during the (empty) drive phase
            } else if (mode == 12) {
                lin[2] = 0.030f / dt;
            } else {
                lin[0] = 0.006f / dt;
                lin[2] = 0.006f / dt;
            }
        } else if (mode == 2) {
            lin[1] = 0.004f / dt;
        } else if (mode == 8 && step < 150) {
            lin[1] = -0.005f / dt;
        } else if (mode == 9 && step >= drive) {
            lin[1] = 0.004f / dt;
        } else if ((mode == 10 || mode == 11) && step < 150) {
            lin[1] = -0.004f / dt;
        }
        t += dtns;
        vrphys_push_hand_sample(1, pos, quat, lin, ang, true, t);
#ifdef VRPHYS_TRACE_CORR
        g_vrphys_trace = (mode == 8 && hz > 80.0f && hz < 100.0f && !handNoise && step == 160);
        if (g_vrphys_trace) printf("  --- step %d ---\n", step);
#endif
        vrphys_step(dt, turn, turnOff, anchor, kScale, true);

        float opos[3], oq[4], ov[3], oa[3];
        vrphys_get_object_pose(VRPHYS_SLOT_WEAPON, opos, oq, ov, oa);
        float cp[12], cn[12];
        const int nc = vrphys_get_object_contacts(VRPHYS_SLOT_WEAPON, cp, cn, 4);

        // JITTER = the high-frequency component only: the discrete second difference of position
        // (how much the motion CHANGES direction/speed per step). Total per-step motion is the
        // wrong measure whenever the hand is deliberately moving or rotating — smooth tracking of
        // a moving hand would score as "jitter" and mask what we're hunting.
        if (havePrev && step > 20) {
            const float dx = (opos[0] - prev[0]) / kScale;
            const float dy = (opos[1] - prev[1]) / kScale;
            const float dz = (opos[2] - prev[2]) / kScale;
            const float d = sqrtf(dx * dx + dy * dy + dz * dz) * 1000.0f;
            if (d > r.maxStepMm) {
                r.maxStepMm = d;
            }
            if (haveD) {
                const float jx = dx - pd[0], jy = dy - pd[1], jz = dz - pd[2];
                const float j = sqrtf(jx * jx + jy * jy + jz * jz) * 1000.0f;
                if (step >= 150) {
                    sum += j;
                    samples++;
                }
            }
            pd[0] = dx; pd[1] = dy; pd[2] = dz;
            haveD = true;
        }
        if (step >= 120) {
            if (nc < r.minContacts) r.minContacts = nc;
            if (nc > r.maxContacts) r.maxContacts = nc;
            if (nc == 0) r.zeroContactSteps++;
        }
        if (step >= 150) {
            const float gx = opos[0] / kScale - pos[0];
            const float gy = opos[1] / kScale - pos[1];
            const float gz = opos[2] / kScale - pos[2];
            const float gd = sqrtf(gx * gx + gy * gy + gz * gz) * 1000.0f;
            if (gd > r.gripDevMm) r.gripDevMm = gd;
        }
#ifdef TRACE_MODE8
        if (mode == 8 && hz > 80.0f && hz < 100.0f && !handNoise && ((step >= 40 && step % 10 == 0) || (step >= 118 && step <= 126))) {
            const float bl = 1.13f;
            float fwd[3] = { 1, 0, 0 };
            // rotate local +x by oq
            const float qx = oq[0], qy = oq[1], qz = oq[2], qw = oq[3];
            const float tx2 = 2*(qy*fwd[2]-qz*fwd[1]), ty2 = 2*(qz*fwd[0]-qx*fwd[2]), tz2 = 2*(qx*fwd[1]-qy*fwd[0]);
            const float dx = fwd[0]+qw*tx2+(qy*tz2-qz*ty2), dy = fwd[1]+qw*ty2+(qz*tx2-qx*ty2), dz = fwd[2]+qw*tz2+(qx*ty2-qy*tx2);
            printf("  s%3d hand(%.3f,%.3f) root(%.3f,%.3f) tip(%.3f,%.3f) nc=%d\n",
                   step, hx, hy, opos[0]/kScale, opos[1]/kScale,
                   opos[0]/kScale + dx*bl, opos[1]/kScale + dy*bl, nc);
        }
#endif

        havePrev = true;
        for (int i = 0; i < 3; i++) prev[i] = opos[i];
        if (step == 299) {
            // Blade trail: angle between simulated and hand orientation (friction probe).
            const float dq = fabsf(oq[0] * quat[0] + oq[1] * quat[1] + oq[2] * quat[2] + oq[3] * quat[3]);
            gTrailDeg = 2.0f * acosf(dq > 1.0f ? 1.0f : dq) * 180.0f / 3.14159265f;
            // Tip lag along the slide direction (y): how far the dragged tip trails the hand.
            const float qx = oq[0], qy = oq[1], qz = oq[2], qw = oq[3];
            const float fx = 1, fy = 0, fz = 0;
            const float tx2 = 2 * (qy * fz - qz * fy), ty2 = 2 * (qz * fx - qx * fz),
                        tz2 = 2 * (qx * fy - qy * fx);
            const float dy = fy + qw * ty2 + (qz * tx2 - qx * tz2);
            gTipLagMm = (hy - (opos[1] / kScale + dy * 1.13f)) * 1000.0f;
        }
    }
    r.heldJitterMm = (float)(sum / (samples ? samples : 1));
    return r;
}

// --------------------------------------------------------------------------
// Visual-mesh harvest scenarios: the selection layer (which tris reach the solver) has its own
// failure modes independent of the solver — ranking against a mis-transformed blade after snap
// turns, and stereo eye-duplicates crowding unique geometry out of the selection slots.
// --------------------------------------------------------------------------

static void FeedMeshTri(float ax, float ay, float az, float bx, float by, float bz, float cx,
                        float cy, float cz) {
    const float a[3] = { ax, ay, az }, b[3] = { bx, by, bz }, c[3] = { cx, cy, cz };
    vrphys_mesh_consider_tri(a, b, c);
}

static VrPhysObjectDesc MeshCaseDesc() {
    VrPhysObjectDesc desc{};
    desc.primary_hand = 1;
    desc.secondary_hand = -1;
    desc.lin_freq_hz = 14.0f;
    desc.lin_zeta = 1.0f;
    desc.ang_freq_hz = 30.0f;
    desc.ang_zeta = 1.0f;
    desc.max_accel_mps2 = 400.0f;
    desc.max_ang_accel = 3000.0f;
    desc.blade_radius_m = 0.4f / kScale;
    desc.touch_tolerance_m = 0.3f / kScale;
    desc.pivot_only = true;
    desc.grip_local_tip_m[0] = 1.13f;
    desc.grip_local_edge_m[2] = 2.0f / kScale;
    desc.tip_taper_frac = 0.2f;
    desc.contact_enabled = true;
    desc.friction = 0.15f;
    return desc;
}

// A 90-degree accumulated snap turn with a 2 m pivot offset (a few snap turns' worth): tri
// selection must rank against the blade's TRUE world position. 48 small decoy tris tile the
// exact spot where a rotate-before-offset transform bug places the blade (displaced by
// scale*(R*off - off) = (-70, 0, -70), ~99 units); 16 real wall tris sit where the blade
// actually is. If ranking uses the phantom segment, the decoys win every selection slot from
// the first step (the wall never even earns sticky preference), the real wall never reaches
// the solver, and driving the blade through it records zero contacts.
static int RunMeshTurnCase() {
    vrphys_reset();
    const float s45 = sinf(3.14159265f / 4.0f);
    const float turn[4] = { 0.0f, s45, 0.0f, s45 }; // +90 deg yaw: R*(x,y,z) = (z, y, -x)
    const float turnOff[3] = { 2.0f, 0.0f, 0.0f };  // meters, world-space, applied AFTER R
    const float anchor[3] = { 0.0f, 0.0f, 0.0f };
    vrphys_set_contact_prims(nullptr, 0); // mesh-only case

    VrPhysObjectDesc desc = MeshCaseDesc();
    vrphys_set_object(VRPHYS_SLOT_WEAPON, &desc);

    const float dt = 1.0f / 90.0f;
    uint64_t t = 0;
    float hx = -0.60f;
    int zeroContactPressSteps = 0;

    for (int step = 0; step < 120; step++) {
        if (step > 0 && step < 90) {
            hx += 0.010f; // drive the blade (world -Z at x=17.5) through the wall plane
        }
        const float pos[3] = { hx, 0.0f, 0.0f };
        const float quat[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        const float lin[3] = { (step > 0 && step < 90) ? 0.010f / dt : 0.0f, 0.0f, 0.0f };
        const float ang[3] = { 0.0f, 0.0f, 0.0f };
        t += (uint64_t)(dt * 1e9f);
        vrphys_push_hand_sample(1, pos, quat, lin, ang, true, t);

        // The game feeds region + tris in true world units: world = (R*raw + off) * scale.
        // True blade line: x = 70, running toward -Z. Phantom (buggy) line: x = 0, 70 deeper.
        const float midRawX = hx + 1.13f * 0.5f;
        const float midW[3] = { 2.0f * kScale, 0.0f, -midRawX * kScale };
        vrphys_mesh_set_region(midW, 150.0f, true);
        // Real wall: 4-unit tiles at z = -35 around the blade line (x = 70), facing +Z.
        for (int gx = 0; gx < 2; gx++) {
            for (int gy = -2; gy < 2; gy++) {
                const float x0 = 66.0f + gx * 4.0f;
                const float y0 = gy * 4.0f;
                FeedMeshTri(x0, y0, -35, x0 + 4, y0, -35, x0, y0 + 4, -35);
                FeedMeshTri(x0 + 4, y0, -35, x0 + 4, y0 + 4, -35, x0, y0 + 4, -35);
            }
        }
        // Decoys: tiles on z = -90 within ~10 units of x=0/y=0 — sitting on the phantom
        // segment (which sweeps z -88..-120 there), ranking ~70 units better than every real
        // tile against it, yet 70 units from the true blade so they never contact under
        // correct selection.
        for (int gx = -3; gx < 3; gx++) {
            for (int gy = -3; gy < 3; gy++) {
                const float cx = gx * 4.0f + 2.0f;
                const float cy = gy * 4.0f + 2.0f;
                if (cx * cx + cy * cy > 10.5f * 10.5f) {
                    continue;
                }
                const float x0 = cx - 2.0f, y0 = cy - 2.0f, z = -90.0f;
                FeedMeshTri(x0, y0, z, x0 + 4, y0, z, x0, y0 + 4, z);
                FeedMeshTri(x0 + 4, y0, z, x0 + 4, y0 + 4, z, x0, y0 + 4, z);
            }
        }
        vrphys_step(dt, turn, turnOff, anchor, kScale, true);

        float cp[12], cn[12];
        const int nc = vrphys_get_object_contacts(VRPHYS_SLOT_WEAPON, cp, cn, 4);
        if (step >= 60 && step < 90 && nc == 0) {
            zeroContactPressSteps++; // blade is through the wall plane here: must be touching
        }
    }
    return zeroContactPressSteps;
}

// Stereo harvest: both eyes feed identical world-space tris, so every tri arrives twice.
// 24 unique floor tris fed twice must ALL reach the solver — if eye-duplicates burn
// selection slots, only ~16 unique survive.
static int RunMeshStereoCase() {
    vrphys_reset();
    const float turn[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    const float turnOff[3] = { 0.0f, 0.0f, 0.0f };
    const float anchor[3] = { 0.0f, 0.0f, 0.0f };
    vrphys_set_contact_prims(nullptr, 0);

    VrPhysObjectDesc desc = MeshCaseDesc();
    vrphys_set_object(VRPHYS_SLOT_WEAPON, &desc);

    const float dt = 1.0f / 90.0f;
    uint64_t t = 0;
    for (int step = 0; step < 5; step++) {
        const float pos[3] = { 0.0f, 0.0f, 0.0f };
        const float quat[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        const float lin[3] = { 0.0f, 0.0f, 0.0f };
        const float ang[3] = { 0.0f, 0.0f, 0.0f };
        t += (uint64_t)(dt * 1e9f);
        vrphys_push_hand_sample(1, pos, quat, lin, ang, true, t);

        const float midW[3] = { 0.565f * kScale, 0.0f, 0.0f }; // blade mid, world units
        vrphys_mesh_set_region(midW, 150.0f, true);
        for (int eye = 0; eye < 2; eye++) { // both eyes harvest the same 24 tris
            for (int gx = 0; gx < 6; gx++) {
                for (int gz = -1; gz < 1; gz++) {
                    const float x0 = gx * 4.0f;
                    const float z0 = gz * 4.0f;
                    FeedMeshTri(x0, -2, z0, x0, -2, z0 + 4, x0 + 4, -2, z0);
                    FeedMeshTri(x0, -2, z0 + 4, x0 + 4, -2, z0 + 4, x0 + 4, -2, z0);
                }
            }
        }
        vrphys_step(dt, turn, turnOff, anchor, kScale, true);
    }
    float tribuf[32 * 9];
    return vrphys_mesh_get_debug_tris(tribuf, 32); // unique tris that reached the solver
}

// --------------------------------------------------------------------------
// Bone-capsule scenarios: enemies/NPCs collide as capsules fitted to their skeletons (their
// render meshes are unsealed multi-shell tri soup that churns the contact set). The cases
// that matter: resting in the crease where two bones meet, a limb that MOVES under the blade
// at game-tick cadence, and a thin bone crossing the blade between its point samples.
// --------------------------------------------------------------------------

static VrPhysContactPrim MakeCapsule(float ax, float ay, float az, float bx, float by, float bz,
                                     float radius) {
    VrPhysContactPrim p{};
    p.type = VRPHYS_PRIM_CAPSULE;
    p.a[0] = ax; p.a[1] = ay; p.a[2] = az;
    p.b[0] = bx; p.b[1] = by; p.b[2] = bz;
    p.radius = radius;
    p.id = 3 << 12; // flesh
    return p;
}

// mode 0: ELBOW — blade pressed down into the crease of two bones meeting at a joint.
// mode 1: MOVING limb — bone oscillates +-2 units at 1.2 Hz, re-pushed every 4th step (the
//         20 Hz game tick); blade held pressed onto it. Some motion is BY DESIGN (the surface
//         moves); the failure modes are ringing and contact dropout.
// mode 2: THIN bone (radius 1.5) crossing the blade mid-span, between point samples — only
//         the continuous closest-pair contact can catch it.
static Result RunCapsuleCase(int mode) {
    vrphys_reset();
    const float turn[4] = { 0, 0, 0, 1 };
    const float turnOff[3] = { 0, 0, 0 };
    const float anchor[3] = { 0, 0, 0 };

    VrPhysObjectDesc desc = MeshCaseDesc();
    vrphys_set_object(VRPHYS_SLOT_WEAPON, &desc);

    auto pushPrims = [&](float yoff) {
        VrPhysContactPrim prims[2];
        int np = 0;
        if (mode == 0) {
            prims[np++] = MakeCapsule(20, 0, -30, 38, 0, 0, 4.0f);
            prims[np++] = MakeCapsule(38, 0, 0, 20, 0, 30, 4.0f);
        } else if (mode == 1) {
            prims[np++] = MakeCapsule(35, yoff, -30, 35, yoff, 30, 4.0f);
        } else {
            prims[np++] = MakeCapsule(15, 0, -30, 15, 0, 30, 1.5f);
        }
        vrphys_set_contact_prims(prims, np);
    };
    pushPrims(0.0f);

    const float dt = 1.0f / 90.0f;
    uint64_t t = 0;
    // All modes: start clear above the bone(s) and press down; the press ends well below the
    // rest height so the blade stays loaded for the whole measurement window.
    float hy = (mode == 0) ? 0.35f : 0.15f;
    const float hyEnd = (mode == 0) ? -0.10f : ((mode == 1) ? -0.02f : -0.08f);
    const int pressSteps = 90;
    const float hyStep = (hyEnd - hy) / (float)pressSteps;

    Result r{ 0, 0, 99, 0, 0, 0.0f };
    float prev[3] = { 0, 0, 0 };
    float pd[3] = { 0, 0, 0 };
    bool haveD = false, havePrev = false;
    double sum = 0;
    int samples = 0;

    for (int step = 0; step < 300; step++) {
        if (step < pressSteps) {
            hy += hyStep;
        }
        if (mode == 1 && (step % 4) == 0) { // 20 Hz game tick: the limb animates
            pushPrims(2.0f * sinf(2.0f * 3.14159265f * 1.2f * (float)step * dt));
        }
        const float pos[3] = { 0.0f, hy, 0.0f };
        const float quat[4] = { 0, 0, 0, 1 };
        const float lin[3] = { 0.0f, (step < pressSteps) ? hyStep / dt : 0.0f, 0.0f };
        const float ang[3] = { 0, 0, 0 };
        t += (uint64_t)(dt * 1e9f);
        vrphys_push_hand_sample(1, pos, quat, lin, ang, true, t);
        vrphys_step(dt, turn, turnOff, anchor, kScale, true);

        float opos[3], oq[4], ov[3], oa[3];
        vrphys_get_object_pose(VRPHYS_SLOT_WEAPON, opos, oq, ov, oa);
        float cp[12], cn[12];
        const int nc = vrphys_get_object_contacts(VRPHYS_SLOT_WEAPON, cp, cn, 4);

        // Pivot-only nails the GRIP to the (stationary) hand — the whole response is
        // rotation, so measure the TIP, where capsule contact actually shows.
        const float qx = oq[0], qy = oq[1], qz = oq[2], qw = oq[3];
        const float ty2 = 2.0f * (qz * 1.0f), tz2 = 2.0f * (-qy * 1.0f); // 2*(qv x +x)
        const float dxr = 1.0f + qw * 0.0f + (qy * tz2 - qz * ty2);
        const float dyr = qw * ty2 + (qz * 0.0f - qx * tz2);
        const float dzr = qw * tz2 + (qx * ty2 - qy * 0.0f);
        const float tipP[3] = { opos[0] + dxr * 1.13f * kScale, opos[1] + dyr * 1.13f * kScale,
                                opos[2] + dzr * 1.13f * kScale };

        if (havePrev && step > 20) {
            const float dx = (tipP[0] - prev[0]) / kScale;
            const float dy = (tipP[1] - prev[1]) / kScale;
            const float dz = (tipP[2] - prev[2]) / kScale;
            const float d = sqrtf(dx * dx + dy * dy + dz * dz) * 1000.0f;
            if (d > r.maxStepMm) {
                r.maxStepMm = d;
            }
            if (haveD) {
                const float jx = dx - pd[0], jy = dy - pd[1], jz = dz - pd[2];
                const float j = sqrtf(jx * jx + jy * jy + jz * jz) * 1000.0f;
                if (step >= 150) {
                    sum += j;
                    samples++;
                }
            }
            pd[0] = dx; pd[1] = dy; pd[2] = dz;
            haveD = true;
        }
        if (step >= 120) {
            if (nc < r.minContacts) r.minContacts = nc;
            if (nc > r.maxContacts) r.maxContacts = nc;
            if (nc == 0) r.zeroContactSteps++;
        }
        havePrev = true;
        for (int i = 0; i < 3; i++) prev[i] = tipP[i];
    }
    r.heldJitterMm = (float)(sum / (samples ? samples : 1));
    return r;
}

// Cosmetic weight lag ("Ancient Dungeon wiggle"): during a constant 6 rad/s swing the SERVED
// (rendered) pose must trail the SIM pose by ~ang_vel * lag (8.6 deg at 25 ms), rebound past
// zero when the swing stops (the wiggle), and settle to exactly zero at rest. The sim pose —
// what contacts and damage use — never lags, which is the whole point.
static void RunWeightLagCase() {
    vrphys_reset();
    const float turn[4] = { 0, 0, 0, 1 };
    const float turnOff[3] = { 0, 0, 0 };
    const float anchor[3] = { 0, 0, 0 };
    vrphys_set_contact_prims(nullptr, 0);
    VrPhysObjectDesc desc = MeshCaseDesc();
    desc.visual_lag_s = 0.025f;
    desc.visual_snap_hz = 5.0f;
    vrphys_set_object(VRPHYS_SLOT_WEAPON, &desc);

    const float dt = 1.0f / 90.0f;
    uint64_t t = 0;
    float theta = 0.0f;
    double steadySum = 0;
    int steadyN = 0;
    float reboundPeak = 0.0f;
    bool crossed = false;
    float residual = 0.0f;
    for (int step = 0; step < 300; step++) {
        const bool swinging = step < 150;
        if (swinging) {
            theta += 6.0f * dt;
        }
        const float quat[4] = { 0, sinf(theta * 0.5f), 0, cosf(theta * 0.5f) };
        const float pos[3] = { 0, 0, 0 };
        const float lin[3] = { 0, 0, 0 };
        const float ang[3] = { 0, swinging ? 6.0f : 0.0f, 0 };
        t += (uint64_t)(dt * 1e9f);
        vrphys_push_hand_sample(1, pos, quat, lin, ang, true, t);
        vrphys_step(dt, turn, turnOff, anchor, kScale, true);

        float opos[3], oq[4], ov[3], oa[3];
        vrphys_get_object_pose(VRPHYS_SLOT_WEAPON, opos, oq, ov, oa); // sim pose (identity turn)
        float sp[3], sq[4];
        vrphys_get_hand_sim_pose_raw(1, sp, sq); // served pose (wiggle applied)
        float dq = fabsf(oq[0] * sq[0] + oq[1] * sq[1] + oq[2] * sq[2] + oq[3] * sq[3]);
        const float wig = 2.0f * acosf(dq > 1.0f ? 1.0f : dq) * 180.0f / 3.14159265f;
        if (step >= 100 && step < 150) {
            steadySum += wig;
            steadyN++;
        }
        if (step >= 150) {
            if (!crossed && wig < 1.0f) {
                crossed = true;
            } else if (crossed && wig > reboundPeak) {
                reboundPeak = wig;
            }
        }
        if (step == 299) {
            residual = wig;
        }
    }
    printf("  WEIGHT wiggle (25 ms lag)    | steady lag %5.2f deg (expect ~8.6) | rebound %4.2f "
           "deg | residual %6.4f deg (expect ~0)\n",
           (float)(steadySum / (steadyN ? steadyN : 1)), reboundPeak, residual);
}

// ---- Two-handed hold + heavy hammer (Megaton Hammer milestone) ----
// Geometry mirrors the game's hammer at world scale 35: handle along grip-local +X, head centre
// 0.54 m out, head axis along local Y (spike end -0.28 m .. face end +0.22 m about the handle
// line), head radius 0.114 m; the second hand grips the butt 0.27 m behind the lead hand.
static const float kHammerHeadX = 0.54f;
static const float kHammerButtX = -0.27f;

static VrPhysObjectDesc HammerDesc(float angFreq, float linFreq, bool twoHand, float gravity) {
    VrPhysObjectDesc d{};
    d.primary_hand = 1;
    d.secondary_hand = twoHand ? 0 : -1;
    d.lin_freq_hz = linFreq;
    d.lin_zeta = 1.0f;
    d.ang_freq_hz = angFreq;
    d.ang_zeta = 0.8f;
    d.max_accel_mps2 = 400.0f;
    d.max_ang_accel = 3000.0f;
    d.grip_local_root_m[0] = kHammerHeadX;
    d.grip_local_root_m[1] = -0.28f;
    d.grip_local_tip_m[0] = kHammerHeadX;
    d.grip_local_tip_m[1] = 0.22f;
    d.blade_radius_m = 0.114f;
    d.touch_tolerance_m = 0.3f / kScale;
    d.pivot_only = true;
    d.contact_enabled = true;
    d.friction = 0.5f;
    d.passthrough_speed_mps = 0.0f; // a hammer never cuts through anything
    d.grip_local_secondary_m[0] = kHammerButtX;
    d.grip_local_com_m[0] = kHammerHeadX;
    d.gravity_scale = gravity;
    return d;
}

static float AngleDeg(const V3& a, const V3& b) {
    float c = vdot(a, b) / (len(a) * len(b));
    c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
    return acosf(c) * 180.0f / 3.14159265f;
}

static void PushHand(int hand, const V3& p, const Q4& q, const V3& v, const V3& w, uint64_t t) {
    const float pos[3] = { p.x, p.y, p.z };
    const float quat[4] = { q.x, q.y, q.z, q.w };
    const float lin[3] = { v.x, v.y, v.z };
    const float ang[3] = { w.x, w.y, w.z };
    vrphys_push_hand_sample(hand, pos, quat, lin, ang, true, t);
}

static Q4 ObjQuat() {
    float p[3], q[4], v[3], w[3];
    vrphys_get_object_pose(VRPHYS_SLOT_WEAPON, p, q, v, w);
    return { q[0], q[1], q[2], q[3] };
}

// Lead hand fixed at the origin with a fixed wrist; the off hand orbits it in the XY plane at the
// butt distance. The handle (+X) must point from the off hand through the lead hand: ~0 error at
// rest, a small spring lag while orbiting, and the roll must stay with the lead wrist.
static void RunTwoHandAimCase() {
    vrphys_reset();
    const float turn[4] = { 0, 0, 0, 1 };
    const float turnOff[3] = { 0, 0, 0 };
    const float anchor[3] = { 0, 0, 0 };
    vrphys_set_contact_prims(nullptr, 0);
    VrPhysObjectDesc d = HammerDesc(8.0f, 12.0f, true, 0.0f);
    vrphys_set_object(VRPHYS_SLOT_WEAPON, &d);
    const float dt = 1.0f / 90.0f;
    uint64_t t = 0;
    const float r = -kHammerButtX;
    float phi = 0.0f; // off hand starts straight behind: (-r, 0, 0)
    double movingErr = 0;
    int movingN = 0;
    float restErr = 0.0f;
    float rollErr = 0.0f;
    for (int step = 0; step < 400; step++) {
        const bool orbit = step >= 100 && step < 250;
        const float rate = orbit ? 1.5f : 0.0f; // rad/s about +Z
        phi += rate * dt;
        const V3 p2 = { -r * cosf(phi), -r * sinf(phi), 0.0f };
        const V3 v2 = { r * sinf(phi) * rate, -r * cosf(phi) * rate, 0.0f };
        t += (uint64_t)(dt * 1e9f);
        PushHand(1, kV3Zero, kQIdent, kV3Zero, kV3Zero, t);
        PushHand(0, p2, kQIdent, v2, kV3Zero, t);
        vrphys_step(dt, turn, turnOff, anchor, kScale, true);
        const Q4 oq = ObjQuat();
        const V3 handle = qrot(oq, { 1.0f, 0.0f, 0.0f });
        const V3 want = mul(p2, -1.0f);
        const float err = AngleDeg(handle, want);
        if (step >= 150 && step < 250) {
            movingErr += err;
            movingN++;
        }
        if (step == 399) {
            restErr = err;
            // The lead wrist never rolled: the object's local +Z must stay perpendicular to the
            // orbit plane (world +Z), whatever the handle direction.
            rollErr = AngleDeg(qrot(oq, { 0.0f, 0.0f, 1.0f }), { 0.0f, 0.0f, 1.0f });
        }
    }
    printf("  TWO-HAND aim (off hand orbits)| moving lag %5.2f deg (8 Hz spring, 1.5 rad/s) | rest "
           "error %6.3f deg (expect ~0) | roll drift %6.3f deg (expect ~0)\n",
           (float)(movingErr / (movingN ? movingN : 1)), restErr, rollErr);
}

// Tracking noise on BOTH hands, held still: the served off-hand pin must not jitter more than the
// hands do (the aim runs over only ~27 cm of span, so noise could be amplified down the handle).
static void RunTwoHandPinNoiseCase() {
    vrphys_reset();
    const float turn[4] = { 0, 0, 0, 1 };
    const float turnOff[3] = { 0, 0, 0 };
    const float anchor[3] = { 0, 0, 0 };
    vrphys_set_contact_prims(nullptr, 0);
    VrPhysObjectDesc d = HammerDesc(8.0f, 12.0f, true, 0.0f);
    vrphys_set_object(VRPHYS_SLOT_WEAPON, &d);
    const float dt = 1.0f / 90.0f;
    uint64_t t = 0;
    float prevPin[3] = { 0, 0, 0 };
    V3 prevHead = kV3Zero;
    double pinSum = 0, headSum = 0;
    int n = 0;
    for (int step = 0; step < 300; step++) {
        const V3 p1 = { Noise(step, 1), Noise(step, 2), Noise(step, 3) };
        const V3 p2 = { kHammerButtX + Noise(step, 4), Noise(step, 5), Noise(step, 6) };
        t += (uint64_t)(dt * 1e9f);
        PushHand(1, p1, kQIdent, kV3Zero, kV3Zero, t);
        PushHand(0, p2, kQIdent, kV3Zero, kV3Zero, t);
        vrphys_step(dt, turn, turnOff, anchor, kScale, true);
        float pin[3], pq[4];
        const bool served = vrphys_get_hand_sim_pose_raw(0, pin, pq);
        float op[3], oq[4], ov[3], ow[3];
        vrphys_get_object_pose(VRPHYS_SLOT_WEAPON, op, oq, ov, ow);
        const V3 head = add(v3(op), mul(qrot(q4(oq), { kHammerHeadX, 0, 0 }), kScale));
        if (step >= 100 && served) {
            pinSum += len(sub(v3(pin), v3(prevPin))) * 1000.0f;
            headSum += len(sub(head, prevHead)) / kScale * 1000.0f;
            n++;
        }
        memcpy(prevPin, pin, sizeof(prevPin));
        prevHead = head;
    }
    printf("  TWO-HAND noise, held still   | off-hand pin HF %6.3f mm/step | head HF %6.3f mm/step "
           "(hand noise ~0.3 mm)\n",
           (float)(pinSum / (n ? n : 1)), (float)(headSum / (n ? n : 1)));
}

// Taking hold with the second hand at an angle off the current handle line (a 15 deg re-aim),
// then letting go: the object must swing over smoothly — no single-step snap.
static void RunTwoHandGrabReleaseCase() {
    vrphys_reset();
    const float turn[4] = { 0, 0, 0, 1 };
    const float turnOff[3] = { 0, 0, 0 };
    const float anchor[3] = { 0, 0, 0 };
    vrphys_set_contact_prims(nullptr, 0);
    VrPhysObjectDesc one = HammerDesc(3.5f, 10.0f, false, 0.0f);
    VrPhysObjectDesc two = HammerDesc(8.0f, 12.0f, true, 0.0f);
    vrphys_set_object(VRPHYS_SLOT_WEAPON, &one);
    const float dt = 1.0f / 90.0f;
    uint64_t t = 0;
    const float a = 15.0f * 3.14159265f / 180.0f;
    const V3 p2 = { kHammerButtX * cosf(a), kHammerButtX * sinf(a), 0.0f };
    Q4 prev = kQIdent;
    float maxGrab = 0.0f, maxRelease = 0.0f, settled = 0.0f;
    for (int step = 0; step < 450; step++) {
        if (step == 150) {
            vrphys_set_object(VRPHYS_SLOT_WEAPON, &two);
        } else if (step == 300) {
            vrphys_set_object(VRPHYS_SLOT_WEAPON, &one);
        }
        t += (uint64_t)(dt * 1e9f);
        PushHand(1, kV3Zero, kQIdent, kV3Zero, kV3Zero, t);
        PushHand(0, p2, kQIdent, kV3Zero, kV3Zero, t);
        vrphys_step(dt, turn, turnOff, anchor, kScale, true);
        const Q4 oq = ObjQuat();
        const float stepDeg = AngleDeg(qrot(oq, { 1, 0, 0 }), qrot(prev, { 1, 0, 0 }));
        if (step >= 150 && step < 300 && stepDeg > maxGrab) {
            maxGrab = stepDeg;
        }
        if (step >= 300 && stepDeg > maxRelease) {
            maxRelease = stepDeg;
        }
        if (step == 299) {
            settled = AngleDeg(qrot(oq, { 1, 0, 0 }), { 1, 0, 0 });
        }
        prev = oq;
    }
    printf("  TWO-HAND grab/release 15 deg | max step on grab %5.2f deg | on release %5.2f deg "
           "(smooth: a few deg) | re-aimed to %5.2f deg (expect 15)\n",
           maxGrab, maxRelease, settled);
}

// One hand holding the handle level: gravity on the head must sag it by g / (L w^2).
static void RunDroopCase() {
    const float freqs[2] = { 3.5f, 8.0f };
    for (float f : freqs) {
        vrphys_reset();
        const float turn[4] = { 0, 0, 0, 1 };
        const float turnOff[3] = { 0, 0, 0 };
        const float anchor[3] = { 0, 0, 0 };
        vrphys_set_contact_prims(nullptr, 0);
        VrPhysObjectDesc d = HammerDesc(f, 10.0f, false, 1.0f);
        vrphys_set_object(VRPHYS_SLOT_WEAPON, &d);
        const float dt = 1.0f / 90.0f;
        uint64_t t = 0;
        for (int step = 0; step < 400; step++) {
            t += (uint64_t)(dt * 1e9f);
            PushHand(1, kV3Zero, kQIdent, kV3Zero, kV3Zero, t);
            vrphys_step(dt, turn, turnOff, anchor, kScale, true);
        }
        const V3 handle = qrot(ObjQuat(), { 1, 0, 0 });
        const float sag = asinf(-handle.y) * 180.0f / 3.14159265f;
        const float w = 2.0f * 3.14159265f * f;
        const float expect = atanf(9.81f / (kHammerHeadX * w * w)) * 180.0f / 3.14159265f;
        printf("  DROOP one hand, %4.1f Hz grip  | sag %5.2f deg (expect ~%4.2f)\n", f, sag, expect);
    }
}

// Overhead slam onto a floor: the lead hand swings the hammer down about +Z at 9 rad/s (head
// ~4.9 m/s) and keeps going 35 deg past the point where the head meets the floor. The head must
// stop ON the floor (no tunnelling, no pass-through), stay constrained every pressed step, and
// rest without jitter.
static void RunHammerSlamCase(float hz) {
    vrphys_reset();
    const float turn[4] = { 0, 0, 0, 1 };
    const float turnOff[3] = { 0, 0, 0 };
    const float anchor[3] = { 0, 0, 0 };
    // Floor at world y = -0.45 m (hand height 0.45 above the floor), in world units.
    const float fy = -0.45f * kScale;
    VrPhysContactPrim prims[2];
    prims[0] = MakeTri(-100, fy, -100, -100, fy, 100, 100, fy, -100, 10);
    prims[1] = MakeTri(100, fy, 100, 100, fy, -100, -100, fy, 100, 10);
    vrphys_set_contact_prims(prims, 2);
    VrPhysObjectDesc d = HammerDesc(8.0f, 12.0f, false, 0.0f);
    vrphys_set_object(VRPHYS_SLOT_WEAPON, &d);
    const float dt = 1.0f / hz;
    uint64_t t = 0;
    // Hammer starts pointing straight up (handle +X rotated +90 deg about Z) and swings over the
    // top toward -X and down; rotating +Z moves the head along its local +Y — the flat face leads.
    float theta = 1.5708f;
    const float rate = 9.0f;
    const float thetaEnd = 4.91f; // past straight down: the hand drives the head deep "into" the floor
    float minHeadBottom = 1e9f;
    int firstContact = -1;
    int zeroAfter = 0;
    double restSum = 0;
    int restN = 0;
    V3 prevTip = kV3Zero;
    float impact = 0.0f;
    for (int step = 0; step < (int)(hz * 3.0f); step++) {
        const bool swinging = theta < thetaEnd;
        if (swinging) {
            theta += rate * dt;
        }
        const Q4 q = { 0, 0, sinf(theta * 0.5f), cosf(theta * 0.5f) };
        const V3 w = { 0, 0, swinging ? rate : 0.0f };
        t += (uint64_t)(dt * 1e9f);
        PushHand(1, kV3Zero, q, kV3Zero, w, t);
        vrphys_step(dt, turn, turnOff, anchor, kScale, true);
        VrPhysEvent ev[8];
        const int ne = vrphys_drain_events(ev, 8);
        for (int e = 0; e < ne; e++) {
            if (ev[e].type == VRPHYS_EV_CONTACT_BEGIN && impact == 0.0f) {
                impact = ev[e].impact_mps;
            }
        }
        const SlotState& sl = g_slots[VRPHYS_SLOT_WEAPON];
        const V3 r0 = add(sl.pos_m, qrot(sl.quat, v3(d.grip_local_root_m)));
        const V3 r1 = add(sl.pos_m, qrot(sl.quat, v3(d.grip_local_tip_m)));
        const float bottom = fminf(r0.y, r1.y) - d.blade_radius_m;
        if (bottom < minHeadBottom) {
            minHeadBottom = bottom;
        }
        if (sl.contact_count > 0 && firstContact < 0) {
            firstContact = step;
        }
        if (firstContact >= 0 && sl.contact_count == 0) {
            zeroAfter++;
        }
        // Rest = the last second, once the swing's spring transient has settled.
        if (step >= (int)(hz * 2.0f)) {
            restSum += len(sub(r1, prevTip)) * 1000.0f;
            restN++;
        }
        prevTip = r1;
    }
    printf("  HAMMER slam @%3.0fHz 4.9 m/s   | head lowest %6.1f mm vs floor (expect >= -~15) | "
           "zero-contact after hit %d (expect 0) | rest HF %6.3f mm | impact %.2f\n",
           hz, (minHeadBottom + 0.45f) * 1000.0f, zeroAfter, (float)(restSum / (restN ? restN : 1)),
           impact);
}

// The weight a player FEELS: the lead wrist snaps 100 deg in 0.2 s (a hard swing) and stops dead.
// Velocity feed-forward makes any spring track a steady rotation exactly, so heaviness comes from
// the torque limit (max angular acceleration): how far the head trails mid-swing, how far its
// momentum carries it past the stop, and how long it takes to settle.
static void RunHeavySwingCase(const char* label, float angFreq, float zeta, float maxAngAccel) {
    vrphys_reset();
    const float turn[4] = { 0, 0, 0, 1 };
    const float turnOff[3] = { 0, 0, 0 };
    const float anchor[3] = { 0, 0, 0 };
    vrphys_set_contact_prims(nullptr, 0);
    VrPhysObjectDesc d = HammerDesc(angFreq, 12.0f, false, 0.0f);
    d.ang_zeta = zeta;
    d.max_ang_accel = maxAngAccel;
    vrphys_set_object(VRPHYS_SLOT_WEAPON, &d);
    const float dt = 1.0f / 90.0f;
    uint64_t t = 0;
    const float total = 100.0f * 3.14159265f / 180.0f;
    const int swingSteps = 18; // 0.2 s
    float theta = 0.0f;
    float maxLag = 0.0f, overshoot = 0.0f, peakHeadMps = 0.0f;
    int settleStep = -1;
    for (int step = 0; step < 300; step++) {
        const bool swinging = step < swingSteps;
        // Smooth (sine-velocity) swing profile, like a real arm.
        float rate = 0.0f;
        if (swinging) {
            rate = total / (swingSteps * dt) * 0.5f * 3.14159265f * sinf(3.14159265f * (step + 0.5f) / swingSteps);
            theta += rate * dt;
        }
        const Q4 q = { 0, 0, sinf(theta * 0.5f), cosf(theta * 0.5f) };
        t += (uint64_t)(dt * 1e9f);
        PushHand(1, kV3Zero, q, kV3Zero, { 0, 0, rate }, t);
        vrphys_step(dt, turn, turnOff, anchor, kScale, true);
        const V3 handle = qrot(ObjQuat(), { 1, 0, 0 });
        const float objTheta = atan2f(handle.y, handle.x);
        const float lagDeg = (theta - objTheta) * 180.0f / 3.14159265f;
        if (swinging && lagDeg > maxLag) {
            maxLag = lagDeg;
        }
        if (!swinging && -lagDeg > overshoot) {
            overshoot = -lagDeg;
        }
        if (!swinging && settleStep < 0 && fabsf(lagDeg) < 1.0f) {
            // settled = first time inside 1 deg AND stays there for the rest of the run
            settleStep = step;
        } else if (settleStep >= 0 && fabsf(lagDeg) >= 1.0f) {
            settleStep = -1;
        }
        const SlotState& sl = g_slots[VRPHYS_SLOT_WEAPON];
        const float headMps = len(sl.ang_vel_rps) * kHammerHeadX;
        if (headMps > peakHeadMps) {
            peakHeadMps = headMps;
        }
    }
    printf("  %-26s | trail %5.1f deg | overshoot %5.1f deg | settle %4.0f ms | peak head %4.1f m/s "
           "(hand's head %4.1f)\n",
           label, maxLag, overshoot, settleStep < 0 ? -1.0f : (settleStep - swingSteps) * dt * 1000.0f,
           peakHeadMps, total / (swingSteps * dt) * 0.5f * 3.14159265f * kHammerHeadX);
}

int main() {
    struct Case {
        const char* name;
        float hz;
        int mode;
        bool noise;
    };
    const Case cases[] = {
        { "tip into corner        @ 90Hz", 90.0f, 0, false },
        { "tip into corner        @ 72Hz", 72.0f, 0, false },
        { "tip into corner        @120Hz", 120.0f, 0, false },
        { "BLADE CENTER at corner @ 90Hz", 90.0f, 1, false },
        { "BLADE CENTER at corner @ 72Hz", 72.0f, 1, false },
        { "BLADE CENTER + hand noise    ", 90.0f, 1, true },
        { "slide along corner     @ 90Hz", 90.0f, 2, false },
        { "slide along corner + noise   ", 90.0f, 2, true },
        { "FLAT WALL hand pushed through", 90.0f, 3, false },
        { "FLAT WALL through @120Hz     ", 120.0f, 3, false },
        { "FLAT WALL through + noise    ", 90.0f, 3, true },
        { "FLAT WALL + wrist rotation   ", 90.0f, 4, false },
        { "FLAT WALL + rotation @120Hz  ", 120.0f, 4, false },
        { "SHOVE 2.5m past wall         ", 90.0f, 5, false },
        { "SHOVE 2.5m + rotation        ", 90.0f, 6, false },
        { "SHOVE 2.5m + rotation @120   ", 120.0f, 6, false },
        { "SHOVE into CORNER + rotation ", 90.0f, 7, false },
        { "LEDGE LIP drag over @ 90Hz   ", 90.0f, 8, false },
        { "LEDGE LIP drag over @ 72Hz   ", 72.0f, 8, false },
        { "LEDGE LIP drag over + noise  ", 90.0f, 8, true },
        { "FLAT side pressed on floor   ", 90.0f, 10, false },
        { "FLAT side on floor + noise   ", 90.0f, 10, true },
        { "EDGE-ON pressed on floor     ", 90.0f, 11, false },
        { "EDGE-ON on floor + noise     ", 90.0f, 11, true },
    };
    const Case passCases[] = {
        { "SWEEP fast thru wall, then rest", 90.0f, 12, false },
    };
    printf("%-32s | HF jitter   | max step | grip dev  | contacts | zero-contact steps\n", "case");
    printf("---------------------------------+-------------+----------+-----------+----------+-------------------\n");
    for (const Case& c : cases) {
        const Result r = RunCase(c.hz, c.mode, c.noise);
        printf("%-32s | %7.3f mm  | %6.2f mm | %6.2f mm | %d-%d      | %d\n", c.name, r.heldJitterMm,
               r.maxStepMm, r.gripDevMm, r.minContacts, r.maxContacts, r.zeroContactSteps);
    }
    printf("\nHF jitter   should be ~0 for held cases; sliding cases move smoothly by design.\n");

    printf("\n=== PASS-THROUGH (threshold 2.2 m/s; sweep at 2.7 must NOT contact, rest must) ===\n");
    for (const Case& c : passCases) {
        const Result r = RunCase(c.hz, c.mode, c.noise, 30.0f, 3000.0f, 400.0f, 0.15f, 2.2f);
        printf("%-32s | HF %7.3f mm | grip dev %5.2f mm | contacts %d-%d | zero-contact %d\n",
               c.name, r.heldJitterMm, r.gripDevMm, r.minContacts, r.maxContacts, r.zeroContactSteps);
    }

    printf("\n=== VISUAL-MESH selection (turned-context ranking + stereo duplicate slots) ===\n");
    const int turnZero = RunMeshTurnCase();
    printf("  snap-turned tri ranking      | zero-contact press steps %d (expect 0)\n", turnZero);
    const int stereoUniq = RunMeshStereoCase();
    printf("  stereo duplicate harvest     | unique tris reaching solver %d/24 (expect 24)\n",
           stereoUniq);

    printf("\n=== BONE CAPSULES (skeleton-fitted enemy collision) ===\n");
    {
        const Result elbow = RunCapsuleCase(0);
        printf("  elbow crease pressed hold    | HF %7.3f mm | max step %6.2f mm | contacts %d-%d "
               "| zero-contact %d\n",
               elbow.heldJitterMm, elbow.maxStepMm, elbow.minContacts, elbow.maxContacts,
               elbow.zeroContactSteps);
        const Result moving = RunCapsuleCase(1);
        printf("  MOVING limb (20 Hz anim)     | HF %7.3f mm | max step %6.2f mm | contacts %d-%d "
               "| zero-contact %d\n",
               moving.heldJitterMm, moving.maxStepMm, moving.minContacts, moving.maxContacts,
               moving.zeroContactSteps);
        const Result thin = RunCapsuleCase(2);
        printf("  THIN bone between samples    | HF %7.3f mm | contacts %d-%d | zero-contact %d "
               "(expect 0)\n",
               thin.heldJitterMm, thin.minContacts, thin.maxContacts, thin.zeroContactSteps);
        RunWeightLagCase();
    }

    printf("\n=== TWO-HANDED HOLD + HAMMER (Megaton Hammer) ===\n");
    RunTwoHandAimCase();
    RunTwoHandPinNoiseCase();
    RunTwoHandGrabReleaseCase();
    RunDroopCase();
    RunHammerSlamCase(72.0f);
    RunHammerSlamCase(90.0f);
    RunHammerSlamCase(120.0f);
    printf("  -- heavy-swing feel (100 deg wrist snap in 0.2 s, then stop) --\n");
    RunHeavySwingCase("sword (30 Hz, z1, 3000)", 30.0f, 1.0f, 3000.0f);
    RunHeavySwingCase("soft spring only (4 Hz)", 4.0f, 0.8f, 3000.0f);
    RunHeavySwingCase("1H 6 Hz z.7 accel 60", 6.0f, 0.7f, 60.0f);
    RunHeavySwingCase("1H 6 Hz z.7 accel 90", 6.0f, 0.7f, 90.0f);
    RunHeavySwingCase("1H 5 Hz z.6 accel 70", 5.0f, 0.6f, 70.0f);
    RunHeavySwingCase("2H 9 Hz z.8 accel 180", 9.0f, 0.8f, 180.0f);
    RunHeavySwingCase("2H 9 Hz z.8 accel 250", 9.0f, 0.8f, 250.0f);
    RunHeavySwingCase("2H 10 Hz z.85 accel 300", 10.0f, 0.85f, 300.0f);

    printf("\n=== FRICTION A/B (tip pressed on flat wall, hand drags sideways at 0.36 m/s) ===\n");
    const float frics[] = { 0.0f, 0.15f, 0.3f, 0.6f, 0.9f };
    for (float f : frics) {
        const Result fr = RunCase(90.0f, 9, false, 30.0f, 3000.0f, 400.0f, f);
        printf("  friction %.2f | tip lag %7.2f mm | HF %7.3f mm | grip dev %5.2f mm\n", f,
               gTipLagMm, fr.heldJitterMm, fr.gripDevMm);
    }

    printf("\n=== OLD defaults (the build that felt good) vs NEW defaults ===\n");
    printf("config                        | rot-press HF | rot zero | ctr-corner HF | ctr zero\n");
    printf("------------------------------+--------------+----------+---------------+---------\n");
    struct Cfg {
        const char* label;
        float angF;
        float maxAngA;
        float maxAcc;
    };
    const Cfg cfgs[] = {
        { "OLD  14 /   500 /  400", 14.0f, 500.0f, 400.0f },
        { "NEW  25 / 20000 / 2000", 25.0f, 20000.0f, 2000.0f },
        { "only maxAcc raised    ", 14.0f, 500.0f, 2000.0f },
        { "only angFreq raised   ", 25.0f, 500.0f, 400.0f },
        { "only angClamp raised  ", 14.0f, 20000.0f, 400.0f },
        { "CAND 25 /  3000 /  400", 25.0f, 3000.0f, 400.0f },
        { "CAND 30 /  3000 /  400", 30.0f, 3000.0f, 400.0f },
        { "CAND 25 /  3000 /  600", 25.0f, 3000.0f, 600.0f },
        { "CAND 25 /  3000 /  800", 25.0f, 3000.0f, 800.0f },
    };
    for (const Cfg& cf : cfgs) {
        const Result rot = RunCase(90.0f, 4, false, cf.angF, cf.maxAngA, cf.maxAcc);
        const Result ctr = RunCase(90.0f, 1, false, cf.angF, cf.maxAngA, cf.maxAcc);
        printf("%-29s | %9.2f mm | %8d | %10.2f mm | %d\n", cf.label, rot.heldJitterMm,
               rot.zeroContactSteps, ctr.heldJitterMm, ctr.zeroContactSteps);
    }
    return 0;
}
