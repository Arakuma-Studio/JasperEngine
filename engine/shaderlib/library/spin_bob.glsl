// The motion of library:spin_bob, shared by its colour (spin_bob.vert) and depth
// (spin_bob_depth.vert) programs, so its shadow and depth pre-pass follow it exactly.
// Include after the surface include; the stage passes its own model matrix.

#define FRAME_VS
#include "includes/frame.glsl"
#include "includes/motion.glsl"

uniform u_vs_customParams {
    float spinSpeed;
    float bobHeight;
    float bobFrequency;
    float phaseVariation;
} customParams;

struct SpinBob {
    vec3 pivot;  // the object's (instance's) origin
    vec3 axis;   // its up axis
    float angle; // spin, radians
    vec3 offset; // bob, along the axis
};

SpinBob spinBob(mat4 modelMatrix){
    SpinBob motion;
    motion.pivot = motionLocalOrigin();
    motion.axis = motionLocalUp();

    // per object, from where it stands: a row of pickups does not move in lockstep
    float phase = motionPhase(motionObjectOrigin(modelMatrix)) * customParams.phaseVariation;
    float time = frame.time.x;
    // in cycles, wrapped before the trigonometry so a long run keeps its precision
    motion.angle = fract(time * customParams.spinSpeed / 360.0 + phase) * MOTION_TWO_PI;
    float bob = sin(fract(time * customParams.bobFrequency + phase) * MOTION_TWO_PI);
    // the height is in world units, whatever the object's scale
    float axisScale = length(mat3(modelMatrix) * motion.axis);
    motion.offset = motion.axis * (customParams.bobHeight * bob / max(axisScale, 1e-6));
    return motion;
}

vec3 spinBobPosition(vec3 position, mat4 modelMatrix){
    SpinBob motion = spinBob(modelMatrix);
    return motion.pivot + motionRotate(position - motion.pivot, motion.axis, motion.angle) + motion.offset;
}

vec3 spinBobNormal(vec3 normal, mat4 modelMatrix){
    SpinBob motion = spinBob(modelMatrix);
    return motionRotate(normal, motion.axis, motion.angle);
}
