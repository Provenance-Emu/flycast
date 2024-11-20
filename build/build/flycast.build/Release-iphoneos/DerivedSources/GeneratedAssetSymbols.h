#import <Foundation/Foundation.h>

#if __has_attribute(swift_private)
#define AC_SWIFT_PRIVATE __attribute__((swift_private))
#else
#define AC_SWIFT_PRIVATE
#endif

/// The "ABXYPad" asset catalog image resource.
static NSString * const ACImageNameABXYPad AC_SWIFT_PRIVATE = @"ABXYPad";

/// The "DPad" asset catalog image resource.
static NSString * const ACImageNameDPad AC_SWIFT_PRIVATE = @"DPad";

/// The "JoystickBackground" asset catalog image resource.
static NSString * const ACImageNameJoystickBackground AC_SWIFT_PRIVATE = @"JoystickBackground";

/// The "JoystickButton" asset catalog image resource.
static NSString * const ACImageNameJoystickButton AC_SWIFT_PRIVATE = @"JoystickButton";

/// The "LTrigger" asset catalog image resource.
static NSString * const ACImageNameLTrigger AC_SWIFT_PRIVATE = @"LTrigger";

/// The "RTrigger" asset catalog image resource.
static NSString * const ACImageNameRTrigger AC_SWIFT_PRIVATE = @"RTrigger";

/// The "Start" asset catalog image resource.
static NSString * const ACImageNameStart AC_SWIFT_PRIVATE = @"Start";

/// The "flycast" asset catalog image resource.
static NSString * const ACImageNameFlycast AC_SWIFT_PRIVATE = @"flycast";

/// The "menuicon" asset catalog image resource.
static NSString * const ACImageNameMenuicon AC_SWIFT_PRIVATE = @"menuicon";

#undef AC_SWIFT_PRIVATE
