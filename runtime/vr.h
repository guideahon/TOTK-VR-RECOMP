// VR mode (OpenXR) for TotK Recomp, ported from Savage Wind VR (github.com/guideahon/Savage-Wind-VR, runtime/vr/).
// Built only with `tools/build_game.py --vr` (TOTK_VR); a plain build has none of it. Turned on at run time with
// TOTK_VR=1. Without an OpenXR runtime or a headset the game stays flat and the log says why ("vr:" lines).
// What it does and what is still to come: docs/vr.md.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef TOTK_VR
#include <vulkan/vulkan.h>

void vr_init(void);                    // TOTK_VR: open the OpenXR runtime, find the headset (once, before Vulkan)
bool vr_enabled(void);                 // VR asked for, the runtime and the headset answered, nothing failed since
const char *vr_status_text(void);      // "off", "no runtime ...", "runtime <name>, headset <name>" ...

// runtime/vk_device.c: each falls back to the plain Vulkan call when VR is off
VkResult vr_vk_create_instance(const VkInstanceCreateInfo *ci, VkInstance *out);
VkPhysicalDevice vr_vk_physical_device(VkInstance inst);   // the headset's GPU, or VK_NULL_HANDLE
VkResult vr_vk_create_device(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, VkDevice *out);
void vr_vk_session_start(void);        // after the device and its queues (vk_ctx() is set)
void vr_vk_release(void);              // before the device goes away

// runtime/gfx_present.c vk_present_blit: the game's frame (src in layout: TRANSFER_SRC_OPTIMAL or GENERAL, its
// writes submitted to gfx_queue) to the headset
void vr_present(VkImage src, VkImageLayout layout, VkFormat fmt, int w, int h);

// runtime/input.c: the VR controllers added to the game's input (Switch button bits, sticks -32767..32767, +y up)
void vr_input_apply(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
#endif
