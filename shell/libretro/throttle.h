#pragma once

// Throttle state definitions - only include if not already defined in libretro.h
#ifndef RETRO_THROTTLE_NONE
/* During normal operation. Rate will be equal to the core's internal FPS. */
#define RETRO_THROTTLE_NONE              0

/* While paused or stepping single frames. Rate will be 0. */
#define RETRO_THROTTLE_FRAME_STEPPING    1

/* During fast forwarding.
 * Rate will be 0 if not specifically limited to a maximum speed. */
#define RETRO_THROTTLE_FAST_FORWARD      2

/* During slow motion. Rate will be less than the core's internal FPS. */
#define RETRO_THROTTLE_SLOW_MOTION       3

/* While rewinding recorded save states. Rate can vary depending on the rewind
 * speed or be 0 if the frontend is not aiming for a specific rate. */
#define RETRO_THROTTLE_REWINDING         4

/* While vsync is active in the video driver and the target refresh rate is
 * lower than the core's internal FPS. Rate is the target refresh rate. */
#define RETRO_THROTTLE_VSYNC             5

/* When the frontend does not throttle in any way. Rate will be 0.
 * An example could be if no vsync or audio output is active. */
#define RETRO_THROTTLE_UNBLOCKED         6
#endif

// Global throttle state variables
extern int throttle_state;
extern float throttle_rate;
