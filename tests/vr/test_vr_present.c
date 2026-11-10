// runtime/vr.c and vr_input.c end to end against fake_xr, without the game: the Vulkan instance and device made through
// the runtime as vk_device.c makes them, the session started, and "the game's frames" (an image cleared to a known
// colour, in TRANSFER_SRC_OPTIMAL as vk_present_blit hands it over) sent through vr_present; fake_xr's capture of the
// virtual screen must have that colour. The rest of the runtime is stubbed (rt_log, vk_ctx, the queue lock).
//   test_vr_present <scratch folder>      (XR_RUNTIME_JSON set to fake_xr's manifest)
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vk.h"
#include "vr.h"
#include "vr_internal.h"
#include "vr_test.h"

// ---- the runtime, stubbed
static VkCtx ctx;
static bool ready;
VkCtx *vk_ctx(void) { return ready ? &ctx : NULL; }
void vk_queue_lock(void) {}
void vk_queue_unlock(void) {}
void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("[host] ", stdout);
    vprintf(fmt, ap);
    fputc('\n', stdout);
    va_end(ap);
}
#include <windows.h>
uint64_t plat_now_ns(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}

// vk_rec.h sends the vkCmd* calls through the recorder (rc_*), which makes them at once for a command buffer that is
// not the replay's (vr.c's own): here straight to Vulkan
#undef vkCmdPipelineBarrier2
#undef vkCmdCopyImage
#undef vkCmdBlitImage
#undef vkCmdClearColorImage
#undef vkCmdPipelineBarrier
void rc_vkCmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *d) { vkCmdPipelineBarrier2(cb, d); }
void rc_vkCmdCopyImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n,
                       const VkImageCopy *r) { vkCmdCopyImage(cb, s, sl, d, dl, n, r); }
void rc_vkCmdBlitImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n,
                       const VkImageBlit *r, VkFilter f) { vkCmdBlitImage(cb, s, sl, d, dl, n, r, f); }
void rc_vkCmdClearColorImage(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearColorValue *c, uint32_t n,
                             const VkImageSubresourceRange *r) { vkCmdClearColorImage(cb, i, l, c, n, r); }
void rc_vkCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags s, VkPipelineStageFlags d, VkDependencyFlags f,
                             uint32_t nm, const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b,
                             uint32_t ni, const VkImageMemoryBarrier *i) { vkCmdPipelineBarrier(cb, s, d, f, nm, m, nb, b, ni, i); }

static void set_env(const char *k, const char *v) { _putenv_s(k, v ? v : ""); }

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = calloc(1, n + 1);
    size_t got = fread(s, 1, n, f);
    s[got] = 0;
    fclose(f);
    return s;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    char log[1200];
    snprintf(log, sizeof log, "%s/fake_xr.log", argv[1]);
    set_env("TOTK_VR", "1");
    set_env("FAKE_XR_LOG", log);
    set_env("FAKE_XR_CAPTURE", argv[1]);
    set_env("FAKE_XR_CAPTURE_EVERY", "5");

    VR_TEST("instance and device through the runtime");
    vr_init();
    CHECK(vr_enabled());
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    CHECK_EQ_INT(vr_vk_create_instance(&ici, &ctx.instance), VK_SUCCESS);
    ctx.phys = vr_vk_physical_device(ctx.instance);
    CHECK(ctx.phys != VK_NULL_HANDLE);
    if (!ctx.phys) return VR_TEST_END("test_vr_present");
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.phys, &nq, NULL);
    VkQueueFamilyProperties qp[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.phys, &nq, qp);
    ctx.gfx_family = 0;
    for (uint32_t i = 0; i < nq; i++)
        if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { ctx.gfx_family = i; break; }
    float prio = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = ctx.gfx_family,
                                  .queueCount = 1, .pQueuePriorities = &prio};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .synchronization2 = VK_TRUE};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qi};
    CHECK_EQ_INT(vr_vk_create_device(ctx.phys, &dci, &ctx.device), VK_SUCCESS);
    vkGetDeviceQueue(ctx.device, ctx.gfx_family, 0, &ctx.gfx_queue);
    ready = true;
    vr_vk_session_start();

    VR_TEST("the game's frames to the virtual screen");
    // "the game's frame": 320x180 R8G8B8A8_UNORM cleared to (1, 0.5, 0) and left in TRANSFER_SRC_OPTIMAL
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {320, 180, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VkImage frame;
    vkCreateImage(ctx.device, &ii, NULL, &frame);
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(ctx.device, frame, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(ctx.phys, &mp);
    uint32_t type = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (mr.memoryTypeBits & (1u << i)) { type = i; break; }
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = type};
    VkDeviceMemory mem;
    vkAllocateMemory(ctx.device, &mai, NULL, &mem);
    vkBindImageMemory(ctx.device, frame, mem, 0);
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = ctx.gfx_family};
    VkCommandPool pool;
    vkCreateCommandPool(ctx.device, &pci, NULL, &pool);
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(ctx.device, &cai, &cb);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &bi);
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = frame, .subresourceRange = range};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkClearColorValue cc = {{1.0f, 0.5f, 0.0f, 1.0f}};
    vkCmdClearColorImage(cb, frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &range);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    vkQueueSubmit(ctx.gfx_queue, 1, &si, VK_NULL_HANDLE);
    for (int i = 0; i < 40; i++) vr_present(frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_FORMAT_R8G8B8A8_UNORM, 320, 180);
    uint64_t frames = 0, rendered = 0;
    int state = 0;
    vr_stats(&frames, &rendered, &state);
    CHECK(frames >= 35);
    CHECK(rendered >= 30);
    CHECK_EQ_INT(state, 5);  // (XR_SESSION_STATE_FOCUSED)
    vkDeviceWaitIdle(ctx.device);

    VR_TEST("what the headset got");
    char *l = read_file(log);
    CHECK(l && strstr(l, "swapchain 320x180 x1 format 43") != NULL);  // (R8G8B8A8_SRGB: the frame's bytes kept)
    // the UNORM bytes (255, 128, 0) copied as they are into the sRGB swapchain, read back as stored
    // (0.5 cleared into UNORM is 127 or 128, by the driver's rounding)
    CHECK(l && (strstr(l, "_layer0_quad 320x180 mean 255.0 127.0 0.0 lit 100.0%") ||
                strstr(l, "_layer0_quad 320x180 mean 255.0 128.0 0.0 lit 100.0%")));
    CHECK(l && strstr(l, "layers 1 quad") != NULL);
    if (l && !strstr(l, "mean 255.0 128.0")) fprintf(stderr, "%s\n", l);
    free(l);

    VR_TEST("the controllers are read");
    uint64_t buttons = 0;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    vr_input_apply(&buttons, &lx, &ly, &rx, &ry);  // (fake_xr's Touch controllers at rest: nothing pressed)
    CHECK_EQ_INT(buttons, 0);

    vr_vk_release();
    vkDestroyCommandPool(ctx.device, pool, NULL);
    vkDestroyImage(ctx.device, frame, NULL);
    vkFreeMemory(ctx.device, mem, NULL);
    vkDestroyDevice(ctx.device, NULL);
    vkDestroyInstance(ctx.instance, NULL);
    return VR_TEST_END("test_vr_present");
}
