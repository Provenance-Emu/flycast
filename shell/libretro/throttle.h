#pragma once

#include <libretro.h>

// Throttle state definitions - only define if not already defined in libretro.h
#ifndef RETRO_THROTTLE_NONE
#define RETRO_THROTTLE_NONE      0
#endif

#ifndef RETRO_THROTTLE_FRAME_STEPPING
#define RETRO_THROTTLE_FRAME_STEPPING 1
#endif

#ifndef RETRO_THROTTLE_NORMAL
#define RETRO_THROTTLE_NORMAL    2
#endif

#ifndef RETRO_THROTTLE_FAST_FORWARD
#define RETRO_THROTTLE_FAST_FORWARD 3
#endif

#ifndef RETRO_THROTTLE_SLOW_MOTION
#define RETRO_THROTTLE_SLOW_MOTION  4
#endif

#ifndef RETRO_THROTTLE_REWINDING
#define RETRO_THROTTLE_REWINDING    5
#endif

#ifndef RETRO_THROTTLE_VSYNC
#define RETRO_THROTTLE_VSYNC        6
#endif

#ifndef RETRO_THROTTLE_UNBLOCKED
#define RETRO_THROTTLE_UNBLOCKED    7
#endif

// Define the throttle state struct if not already defined
#ifndef RETRO_ENVIRONMENT_GET_THROTTLE_STATE
#define RETRO_ENVIRONMENT_GET_THROTTLE_STATE 56

struct retro_throttle_state
{
   uint32_t mode;
   float rate;
};
#endif

// Global throttle state variables
extern int throttle_state;
extern float throttle_rate;
