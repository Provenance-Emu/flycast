# MoltenVK Compatibility Improvements

This document outlines the changes made to improve compatibility with MoltenVK 1.2.11+ and address crashes during scene transitions, particularly when using Metal argument buffers.

## Background

When using MoltenVK with Metal argument buffers (set by `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=1`), the Flycast emulator was experiencing crashes during scene transitions. These crashes were related to memory management issues in the Vulkan driver implementation.

## Changes Made

### 1. Vulkan Context Detection

- Added detection for MoltenVK with Metal argument buffers in `vulkan_context.cpp`
- Implemented a flag system to identify when Metal argument buffers are in use
- Added logging to notify when this configuration is detected

### 2. Buffer Allocation Improvements

- Enhanced buffer allocation in `buffer.cpp` to be more conservative when using Metal argument buffers
- Added special handling for large buffers to reduce GPU memory pressure
- Implemented additional safety flags for memory allocations in this configuration

### 3. Texture Handling Optimizations

- Modified texture creation in `texture.cpp` to use more conservative settings with Metal argument buffers
- Reduced the size thresholds for linear tiling to avoid potential issues
- Added dedicated memory allocation for textures to prevent fragmentation

### 4. Memory Safety Enhancements

- Improved the VMA allocator's memory handling in `vmallocator.h`
- Implemented a safer approach to memory size determination that avoids direct inclusion of system headers
- Used conservative memory allocation strategies to prevent crashes during scene transitions

## Configuration Detection

The code now automatically detects when MoltenVK is being used with Metal argument buffers by checking for the presence of both:
- `VK_EXT_METAL_OBJECTS_EXTENSION_NAME` extension
- `VK_KHR_portability_subset` extension

When both extensions are present, the code applies more conservative memory management strategies to prevent crashes.

## Testing

These changes should be tested with MoltenVK 1.2.11+ on iOS devices, particularly with the following configurations:

1. With Metal argument buffers enabled (`MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=1`)
2. Without Metal argument buffers (`MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0`)

Focus testing on scene transitions that previously caused crashes, such as moving between different areas in games or loading new levels.
