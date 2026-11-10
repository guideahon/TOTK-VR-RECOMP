// VR mode for TotK Recomp: the OpenXR instance and headset, the Vulkan device made through the runtime
// (XR_KHR_vulkan_enable2), the session on the game's graphics queue, and the game's frames on a virtual screen in the
// headset. Ported from Savage Wind VR (runtime/vr/vr.c, xr_vulkan.c); here the frame goes to the headset from
// vk_present_blit with a command buffer of its own, under vk_queue_lock like every use of gfx_queue.
//
// Phase 1 only: the flat game on a quad layer (TOTK_VR_SCREEN_WIDTH / _DISTANCE, metres). Head tracking and stereo
// need sites in the game's camera code (docs/vr.md, tools/vr/find_vr_sites.py).
#ifdef TOTK_VR
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "vk.h"
#include "vr.h"
#include "vr_internal.h"

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define MAX_IMAGES 8
#define SLOTS 3

static bool asked, enabled;
static char status[192] = "off";
static XrInstance g_inst = XR_NULL_HANDLE;
static XrSystemId g_system = XR_NULL_SYSTEM_ID;

static PFN_xrGetVulkanGraphicsRequirements2KHR GetRequirements;
static PFN_xrCreateVulkanInstanceKHR CreateInstance;
static PFN_xrGetVulkanGraphicsDevice2KHR GetDevice;
static PFN_xrCreateVulkanDeviceKHR CreateDevice;

static struct {
    XrSession session;
    XrSpace local, view;
    XrSessionState state;
    bool running;
    XrSwapchain sc;
    VkFormat sc_fmt;        // the swapchain's format
    VkExtent2D sc_ext;
    uint32_t n;
    VkImage images[MAX_IMAGES];
    VkPhysicalDevice phys;
    float screen_w, screen_dist;
    VkCommandPool pool;
    VkCommandBuffer cb[SLOTS];
    VkFence fence[SLOTS];
    unsigned slot;
    XrTime last_display;
    XrDuration period;
    uint64_t frames, rendered;
} g;

bool vr_enabled(void) { return enabled; }
const char *vr_status_text(void) { return status; }
XrInstance vr_xr_instance(void) { return enabled ? g_inst : XR_NULL_HANDLE; }
XrSession vr_xr_session(void) { return g.session; }
XrSpace vr_xr_local(void) { return g.local; }
XrSpace vr_xr_view(void) { return g.view; }
bool vr_session_running(void) { return g.running && g.session != XR_NULL_HANDLE; }
XrTime vr_next_display_time(void) { return g.last_display ? g.last_display + 2 * g.period : 0; }

bool vr_xr_ok(XrResult r, const char *what) {
    if (XR_SUCCEEDED(r)) return true;
    char name[XR_MAX_RESULT_STRING_SIZE] = "";
    if (g_inst == XR_NULL_HANDLE || XR_FAILED(xrResultToString(g_inst, r, name))) snprintf(name, sizeof name, "%d", (int)r);
    rt_log("vr: %s: %s", what, name);
    return false;
}

void vr_disable(const char *why) {
    if (!enabled) return;
    enabled = false;
    snprintf(status, sizeof status, "stopped: %s", why);
    rt_log("vr: VR stopped (%s): the game goes on flat", why);
}

static float env_float(const char *name, float def, float lo, float hi) {
    const char *v = getenv(name);
    float f = v && *v ? (float)atof(v) : def;
    return f >= lo && f <= hi ? f : def;
}

// ---- the instance and the headset ----
static bool runtime_has(const char *ext) {
    uint32_t n = 0;
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(NULL, 0, &n, NULL)) || !n) return false;
    XrExtensionProperties *p = calloc(n, sizeof *p);
    for (uint32_t i = 0; i < n; i++) p[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    bool has = false;
    if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(NULL, n, &n, p)))
        for (uint32_t i = 0; i < n; i++) has |= !strcmp(p[i].extensionName, ext);
    free(p);
    return has;
}

static void stay_flat(const char *why) {
    snprintf(status, sizeof status, "%s", why);
    rt_log("vr: VR asked for, but %s: staying flat", why);
    if (g_inst != XR_NULL_HANDLE) xrDestroyInstance(g_inst);
    g_inst = XR_NULL_HANDLE;
}

void vr_init(void) {
    if (asked) return;
    asked = true;
    const char *e = getenv("TOTK_VR");
    if (!e || !*e || !strcmp(e, "0")) return;
    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    snprintf(ci.applicationInfo.applicationName, sizeof ci.applicationInfo.applicationName, "TotK Recomp");
    snprintf(ci.applicationInfo.engineName, sizeof ci.applicationInfo.engineName, "TotK Recomp");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    bool vulkan = runtime_has(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
    const char *ext[] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    ci.enabledExtensionCount = vulkan ? 1 : 0;
    ci.enabledExtensionNames = ext;
    char why[200];
    XrResult r = xrCreateInstance(&ci, &g_inst);
    if (XR_FAILED(r)) {
        g_inst = XR_NULL_HANDLE;
        snprintf(why, sizeof why, "no runtime (xrCreateInstance %d)", (int)r);
        stay_flat(why);
        return;
    }
    XrInstanceProperties ip = {XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(g_inst, &ip);
    if (!vulkan) {
        snprintf(why, sizeof why, "runtime %s has no XR_KHR_vulkan_enable2", ip.runtimeName);
        stay_flat(why);
        return;
    }
    XrSystemGetInfo gi = {XR_TYPE_SYSTEM_GET_INFO, NULL, XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    r = xrGetSystem(g_inst, &gi, &g_system);
    if (XR_FAILED(r)) {  // (XR_ERROR_FORM_FACTOR_UNAVAILABLE: the headset is off or not connected)
        snprintf(why, sizeof why, "runtime %s found no headset (xrGetSystem %d)", ip.runtimeName, (int)r);
        stay_flat(why);
        return;
    }
    XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(g_inst, g_system, &sp);
    snprintf(status, sizeof status, "runtime %s %u.%u.%u, headset %s", ip.runtimeName,
             XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
             XR_VERSION_PATCH(ip.runtimeVersion), sp.systemName);
    enabled = true;
    rt_log("vr: OpenXR %s", status);
}

// ---- Vulkan through the runtime ----
static bool load_fns(void) {
#define LOAD(var, name) \
    if (XR_FAILED(xrGetInstanceProcAddr(g_inst, name, (PFN_xrVoidFunction *)&var)) || !var) return false;
    LOAD(GetRequirements, "xrGetVulkanGraphicsRequirements2KHR")
    LOAD(CreateInstance, "xrCreateVulkanInstanceKHR")
    LOAD(GetDevice, "xrGetVulkanGraphicsDevice2KHR")
    LOAD(CreateDevice, "xrCreateVulkanDeviceKHR")
#undef LOAD
    return true;
}

VkResult vr_vk_create_instance(const VkInstanceCreateInfo *ci, VkInstance *out) {
    vr_init();
    if (!enabled) return vkCreateInstance(ci, NULL, out);
    if (!load_fns()) {
        vr_disable("the runtime gave no XR_KHR_vulkan_enable2 functions");
        return vkCreateInstance(ci, NULL, out);
    }
    XrGraphicsRequirementsVulkanKHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    if (!vr_xr_ok(GetRequirements(g_inst, g_system, &req), "xrGetVulkanGraphicsRequirements2KHR")) {
        vr_disable("no Vulkan requirements");
        return vkCreateInstance(ci, NULL, out);
    }
    XrVulkanInstanceCreateInfoKHR xci = {XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR, NULL, g_system, 0, vkGetInstanceProcAddr,
                                         ci, NULL};
    VkResult vr = VK_ERROR_INITIALIZATION_FAILED;
    if (!vr_xr_ok(CreateInstance(g_inst, &xci, out, &vr), "xrCreateVulkanInstanceKHR")) {
        vr_disable("the runtime could not make the Vulkan instance");
        return vkCreateInstance(ci, NULL, out);
    }
    return vr;
}

VkPhysicalDevice vr_vk_physical_device(VkInstance vk) {
    if (!enabled) return VK_NULL_HANDLE;
    XrVulkanGraphicsDeviceGetInfoKHR gi = {XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR, NULL, g_system, vk};
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    if (!vr_xr_ok(GetDevice(g_inst, &gi, &pd), "xrGetVulkanGraphicsDevice2KHR")) {
        vr_disable("the runtime named no GPU");
        return VK_NULL_HANDLE;
    }
    g.phys = pd;
    return pd;
}

VkResult vr_vk_create_device(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, VkDevice *out) {
    if (!enabled || pd != g.phys) {
        if (enabled) vr_disable("the game's GPU is not the headset's");
        return vkCreateDevice(pd, ci, NULL, out);
    }
    XrVulkanDeviceCreateInfoKHR xdi = {XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR, NULL, g_system, 0, vkGetInstanceProcAddr,
                                       pd, ci, NULL};
    VkResult vr = VK_ERROR_INITIALIZATION_FAILED;
    if (!vr_xr_ok(CreateDevice(g_inst, &xdi, out, &vr), "xrCreateVulkanDeviceKHR")) {
        vr_disable("the runtime could not make the Vulkan device");
        return vkCreateDevice(pd, ci, NULL, out);
    }
    return vr;
}

void vr_vk_session_start(void) {
    VkCtx *c = vk_ctx();
    if (!enabled || !c) return;
    XrGraphicsBindingVulkanKHR bind = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR, NULL, c->instance, c->phys, c->device,
                                       c->gfx_family, 0};
    XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO, &bind, 0, g_system};
    if (!vr_xr_ok(xrCreateSession(g_inst, &sci, &g.session), "xrCreateSession")) {
        vr_disable("no session");
        return;
    }
    XrReferenceSpaceCreateInfo rci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO, NULL, XR_REFERENCE_SPACE_TYPE_LOCAL,
                                      {{0, 0, 0, 1}, {0, 0, 0}}};
    if (!vr_xr_ok(xrCreateReferenceSpace(g.session, &rci, &g.local), "xrCreateReferenceSpace")) {
        vr_disable("no LOCAL space");
        return;
    }
    rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    xrCreateReferenceSpace(g.session, &rci, &g.view);
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = c->gfx_family};
    vkCreateCommandPool(c->device, &pci, NULL, &g.pool);
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g.pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = SLOTS};
    vkAllocateCommandBuffers(c->device, &ai, g.cb);
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    for (int i = 0; i < SLOTS; i++) vkCreateFence(c->device, &fi, NULL, &g.fence[i]);
    g.screen_w = env_float("TOTK_VR_SCREEN_WIDTH", 3.0f, 0.2f, 50.0f);
    g.screen_dist = env_float("TOTK_VR_SCREEN_DISTANCE", 2.5f, 0.3f, 50.0f);
    rt_log("vr: session on queue family %u; virtual screen %.1f m wide at %.1f m", c->gfx_family, g.screen_w,
           g.screen_dist);
    vr_input_setup(g.session);
}

static void drop_swapchain(void) {
    VkCtx *c = vk_ctx();
    if (g.sc == XR_NULL_HANDLE) return;
    vk_queue_lock();
    vkDeviceWaitIdle(c->device);  // (only on a resize or the end: its images may still be read)
    vk_queue_unlock();
    xrDestroySwapchain(g.sc);
    g.sc = XR_NULL_HANDLE;
    g.n = 0;
}

static void end_session(const char *why) {
    drop_swapchain();
    if (g.local) xrDestroySpace(g.local);
    if (g.view) xrDestroySpace(g.view);
    if (g.session) xrDestroySession(g.session);
    g.local = g.view = XR_NULL_HANDLE;
    g.session = XR_NULL_HANDLE;
    g.running = false;
    vr_disable(why);
}

void vr_vk_release(void) {
    VkCtx *c = vk_ctx();
    if (!c) return;
    if (g.session) end_session("the game's device went away");
    if (g.pool) {
        for (int i = 0; i < SLOTS; i++) vkDestroyFence(c->device, g.fence[i], NULL);
        vkDestroyCommandPool(c->device, g.pool, NULL);
        g.pool = VK_NULL_HANDLE;
    }
}

static void poll_events(void) {
    XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
    while (g.session && xrPollEvent(g_inst, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            XrSessionState st = ((XrEventDataSessionStateChanged *)&ev)->state;
            g.state = st;
            rt_log("vr: session state %d", (int)st);
            if (st == XR_SESSION_STATE_READY && !g.running) {
                XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO, NULL, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
                if (vr_xr_ok(xrBeginSession(g.session, &bi), "xrBeginSession")) g.running = true;
            } else if (st == XR_SESSION_STATE_STOPPING && g.running) {
                vr_xr_ok(xrEndSession(g.session), "xrEndSession");
                g.running = false;
            } else if (st == XR_SESSION_STATE_EXITING) {
                end_session("the runtime ended the session");
            } else if (st == XR_SESSION_STATE_LOSS_PENDING) {
                end_session("the session was lost");
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            end_session("the runtime is going away");
        }
        ev = (XrEventDataBuffer){XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// ---- the virtual screen: an sRGB swapchain of the frame's size. 8-bit frames are copied byte for byte into the sRGB
// twin of their format (their bytes are already gamma-encoded); others (a float HDR target) are blitted, which encodes ----
static VkFormat srgb_twin(VkFormat f) {
    switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    default: return VK_FORMAT_R8G8B8A8_SRGB;
    }
}
static bool copyable(VkFormat f) {
    return f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_UNORM ||
           f == VK_FORMAT_R8G8B8A8_SRGB;
}

static bool make_swapchain(VkExtent2D ext, VkFormat want) {
    uint32_t n = 0;
    int64_t formats[64];
    if (!vr_xr_ok(xrEnumerateSwapchainFormats(g.session, 64, &n, formats), "xrEnumerateSwapchainFormats")) return false;
    bool offered = false;
    for (uint32_t k = 0; k < n; k++) offered |= formats[k] == want;
    if (!offered) {
        static bool said;
        if (!said) rt_log("vr: the runtime offers no swapchain in format %d: nothing for the headset", (int)want);
        said = true;
        return false;
    }
    XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO, NULL, 0,
                                XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT, want, 1,
                                ext.width, ext.height, 1, 1, 1};
    if (!vr_xr_ok(xrCreateSwapchain(g.session, &ci, &g.sc), "xrCreateSwapchain")) return false;
    XrSwapchainImageVulkanKHR imgs[MAX_IMAGES];
    for (int k = 0; k < MAX_IMAGES; k++) imgs[k] = (XrSwapchainImageVulkanKHR){XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR};
    if (!vr_xr_ok(xrEnumerateSwapchainImages(g.sc, MAX_IMAGES, &g.n, (XrSwapchainImageBaseHeader *)imgs),
                  "xrEnumerateSwapchainImages")) {
        xrDestroySwapchain(g.sc);
        g.sc = XR_NULL_HANDLE;
        return false;
    }
    for (uint32_t k = 0; k < g.n; k++) g.images[k] = imgs[k].image;
    g.sc_ext = ext;
    g.sc_fmt = want;
    rt_log("vr: virtual screen swapchain %ux%u format %d, %u images", ext.width, ext.height, (int)want, g.n);
    return true;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 ss,
                    VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa,
                               .dstStageMask = ds, .dstAccessMask = da, .oldLayout = from, .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                           .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}

void vr_present(VkImage src, VkImageLayout layout, VkFormat fmt, int w, int h) {
    VkCtx *c = vk_ctx();
    if (!g.session || !c || !src || w <= 0 || h <= 0) return;
    poll_events();
    if (!g.running || !g.session) return;
    XrFrameState fs = {XR_TYPE_FRAME_STATE};
    if (!vr_xr_ok(xrWaitFrame(g.session, NULL, &fs), "xrWaitFrame")) return;  // (paces the game to the headset)
    if (!vr_xr_ok(xrBeginFrame(g.session, NULL), "xrBeginFrame")) return;
    g.last_display = fs.predictedDisplayTime;
    g.period = fs.predictedDisplayPeriod;
    g.frames++;
    XrCompositionLayerQuad quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader *layers[1] = {(const XrCompositionLayerBaseHeader *)&quad};
    uint32_t nl = 0;
    VkExtent2D ext = {(uint32_t)w, (uint32_t)h};
    VkFormat want = srgb_twin(fmt);
    if (g.sc && (g.sc_ext.width != ext.width || g.sc_ext.height != ext.height || g.sc_fmt != want)) drop_swapchain();
    uint32_t idx = 0;
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, NULL, XR_INFINITE_DURATION};
    if (fs.shouldRender && (g.sc || make_swapchain(ext, want)) &&
        vr_xr_ok(xrAcquireSwapchainImage(g.sc, NULL, &idx), "xrAcquireSwapchainImage") &&
        vr_xr_ok(xrWaitSwapchainImage(g.sc, &wi), "xrWaitSwapchainImage")) {
        unsigned s = g.slot++ % SLOTS;
        vkWaitForFences(c->device, 1, &g.fence[s], VK_TRUE, UINT64_MAX);
        vkResetFences(c->device, 1, &g.fence[s]);
        VkCommandBuffer cb = g.cb[s];
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(cb, &bi);
        VkImage dst = g.images[idx];
        // the frame: in layout already (TRANSFER_SRC_OPTIMAL or GENERAL); its writes, submitted earlier on this
        // queue, made visible to the copy
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                               .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
        VkDependencyInfo mdi = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdPipelineBarrier2(cb, &mdi);
        barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        if (copyable(fmt)) {
            VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {ext.width, ext.height, 1}};
            vkCmdCopyImage(cb, src, layout, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
        } else {
            VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .srcOffsets = {{0, 0, 0}, {w, h, 1}},
                              .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .dstOffsets = {{0, 0, 0}, {w, h, 1}}};
            vkCmdBlitImage(cb, src, layout, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
        }
        // the runtime takes its images back as colour attachments (XR_KHR_vulkan_enable2)
        barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        vkEndCommandBuffer(cb);
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
        vk_queue_lock();
        vkQueueSubmit(c->gfx_queue, 1, &si, g.fence[s]);
        bool released = vr_xr_ok(xrReleaseSwapchainImage(g.sc, NULL), "xrReleaseSwapchainImage");
        vk_queue_unlock();
        if (released) {
            quad.space = g.local;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = g.sc;
            quad.subImage.imageRect = (XrRect2Di){{0, 0}, {w, h}};
            quad.pose = (XrPosef){{0, 0, 0, 1}, {0, 0, -g.screen_dist}};
            quad.size = (XrExtent2Df){g.screen_w, g.screen_w * (float)h / (float)w};
            nl = 1;
            g.rendered++;
        }
    }
    XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO, NULL, fs.predictedDisplayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, nl, layers};
    vk_queue_lock();  // (the runtime may use the queue in xrEndFrame)
    vr_xr_ok(xrEndFrame(g.session, &ei), "xrEndFrame");
    vk_queue_unlock();
}

void vr_stats(uint64_t *frames, uint64_t *rendered, int *state) {
    *frames = g.frames, *rendered = g.rendered, *state = (int)g.state;
}
#endif
