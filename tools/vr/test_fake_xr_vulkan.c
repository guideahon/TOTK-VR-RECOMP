// fake_xr's Vulkan side, driven the way runtime/vr/xr_vulkan.c drives a runtime: the Vulkan instance and device made
// through XR_KHR_vulkan_enable2, a session on them, a swapchain, frames that clear its images to a known colour and
// show them as a quad layer, and fake_xr's capture of what it got. Needs a Vulkan driver (any GPU, or Mesa's lavapipe).
//   test_fake_xr_vulkan <fake_xr library> <scratch folder>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define XR_NO_PROTOTYPES
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include "../../tests/vr/vr_test.h"
#include "fake_xr/fake_xr.h"

#define CHECK_XR(call, expected)                                                                             \
    do {                                                                                                     \
        XrResult r_ = (call);                                                                                \
        vr_test_checks++;                                                                                    \
        if (r_ != (expected)) {                                                                              \
            vr_test_failures++;                                                                              \
            fprintf(stderr, "%s:%d: [%s] %s = %d, expected %s (%d)\n", __FILE__, __LINE__, vr_test_name, #call, \
                    (int)r_, #expected, (int)(expected));                                                    \
        }                                                                                                    \
    } while (0)

#define XR_FUNCS(X)                                                                                                     \
    X(CreateInstance) X(DestroyInstance) X(GetSystem) X(CreateSession) X(DestroySession) X(PollEvent) X(BeginSession) \
    X(RequestExitSession) X(EndSession) X(WaitFrame) X(BeginFrame) X(EndFrame) X(CreateReferenceSpace)                \
    X(DestroySpace) X(GetVulkanGraphicsRequirements2KHR) X(CreateVulkanInstanceKHR) X(GetVulkanGraphicsDevice2KHR)   \
    X(CreateVulkanDeviceKHR) X(EnumerateSwapchainFormats) X(CreateSwapchain) X(DestroySwapchain)                      \
    X(EnumerateSwapchainImages) X(AcquireSwapchainImage) X(WaitSwapchainImage) X(ReleaseSwapchainImage)
#define DECLARE(n) static PFN_xr##n xr##n;
XR_FUNCS(DECLARE)
static PFN_xrGetInstanceProcAddr gipa;
static char scratch[1024];

static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

static void load(const char *path) {
    PFN_xrNegotiateLoaderRuntimeInterface negotiate;
#ifdef _WIN32
    HMODULE lib = LoadLibraryA(path);
    if (!lib) { fprintf(stderr, "can't load %s\n", path); exit(2); }
    negotiate = (PFN_xrNegotiateLoaderRuntimeInterface)(void *)GetProcAddress(lib, "xrNegotiateLoaderRuntimeInterface");
#else
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "can't load %s: %s\n", path, dlerror()); exit(2); }
    negotiate = (PFN_xrNegotiateLoaderRuntimeInterface)dlsym(lib, "xrNegotiateLoaderRuntimeInterface");
#endif
    XrNegotiateLoaderInfo li = {XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION, sizeof li, 1, 1,
                                XR_MAKE_VERSION(1, 0, 0), XR_MAKE_VERSION(1, 0x3ff, 0xfff)};
    XrNegotiateRuntimeRequest rr = {XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST, XR_RUNTIME_INFO_STRUCT_VERSION,
                                    sizeof rr};
    if (!negotiate || negotiate(&li, &rr) != XR_SUCCESS) { fprintf(stderr, "negotiation failed\n"); exit(2); }
    gipa = rr.getInstanceProcAddr;
}

static XrInstance make_instance(void) {
    const char *ext[] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    snprintf(ci.applicationInfo.applicationName, sizeof ci.applicationInfo.applicationName, "test_fake_xr_vulkan");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = ext;
    XrInstance inst = XR_NULL_HANDLE;
    CHECK_XR(xrCreateInstance(&ci, &inst), XR_SUCCESS);
    return inst;
}

static XrSessionState poll_state(XrInstance inst) {
    XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
    if (xrPollEvent(inst, &ev) != XR_SUCCESS || ev.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
        return XR_SESSION_STATE_UNKNOWN;
    return ((XrEventDataSessionStateChanged *)&ev)->state;
}

typedef struct {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    uint32_t qfam;
    VkQueue queue;
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkFence fence;
} gpu;

// the Vulkan instance and device through the runtime; false when there is no Vulkan driver at all
static bool make_gpu(XrInstance inst, XrSystemId sys, gpu *g) {
    XrGraphicsRequirementsVulkanKHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    CHECK_XR(xrGetVulkanGraphicsRequirements2KHR(inst, sys, &req), XR_SUCCESS);
    CHECK(req.minApiVersionSupported <= XR_MAKE_VERSION(1, 3, 0));
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "test_fake_xr_vulkan",
                             .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    XrVulkanInstanceCreateInfoKHR xci = {XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR, NULL, sys, 0, vkGetInstanceProcAddr,
                                         &vci, NULL};
    VkResult vr = VK_ERROR_UNKNOWN;
    CHECK_XR(xrCreateVulkanInstanceKHR(inst, &xci, &g->inst, &vr), XR_SUCCESS);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "no Vulkan 1.3 instance (%d): no Vulkan driver here\n", vr);
        return false;
    }
    XrVulkanGraphicsDeviceGetInfoKHR gi = {XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR, NULL, sys, g->inst};
    CHECK_XR(xrGetVulkanGraphicsDevice2KHR(inst, &gi, &g->phys), XR_SUCCESS);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g->phys, &nq, NULL);
    VkQueueFamilyProperties qp[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(g->phys, &nq, qp);
    g->qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq && g->qfam == UINT32_MAX; i++)
        if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) g->qfam = i;
    CHECK(g->qfam != UINT32_MAX);
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g->qfam,
                                   .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci};
    // a device on another GPU than the runtime's is refused
    XrVulkanDeviceCreateInfoKHR xdi = {XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR, NULL, sys, 0, vkGetInstanceProcAddr,
                                       (VkPhysicalDevice)(uintptr_t)0x1234, &dci, NULL};
    CHECK_XR(xrCreateVulkanDeviceKHR(inst, &xdi, &g->dev, &vr), XR_ERROR_VALIDATION_FAILURE);
    xdi.vulkanPhysicalDevice = g->phys;
    CHECK_XR(xrCreateVulkanDeviceKHR(inst, &xdi, &g->dev, &vr), XR_SUCCESS);
    CHECK_EQ_INT(vr, VK_SUCCESS);
    vkGetDeviceQueue(g->dev, g->qfam, 0, &g->queue);
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g->qfam};
    vkCreateCommandPool(g->dev, &pi, NULL, &g->pool);
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g->pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    vkAllocateCommandBuffers(g->dev, &ai, &g->cb);
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(g->dev, &fi, NULL, &g->fence);
    return true;
}

static void free_gpu(gpu *g) {
    if (!g->dev) return;
    vkDeviceWaitIdle(g->dev);
    vkDestroyFence(g->dev, g->fence, NULL);
    vkDestroyCommandPool(g->dev, g->pool, NULL);
    vkDestroyDevice(g->dev, NULL);
    vkDestroyInstance(g->inst, NULL);
}

// clear image to a colour and hand it back in COLOR_ATTACHMENT_OPTIMAL, as XR_KHR_vulkan_enable2 wants
static void clear_image(gpu *g, VkImage img, const float rgba[4]) {
    vkResetCommandBuffer(g->cb, 0);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(g->cb, &bi);
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0,
                              .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                              .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = img, .subresourceRange = range};
    vkCmdPipelineBarrier(g->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         1, &b);
    VkClearColorValue c = {{rgba[0], rgba[1], rgba[2], rgba[3]}};
    vkCmdClearColorImage(g->cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(g->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         NULL, 0, NULL, 1, &b);
    vkEndCommandBuffer(g->cb);
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &g->cb};
    vkResetFences(g->dev, 1, &g->fence);
    vkQueueSubmit(g->queue, 1, &si, g->fence);
    vkWaitForFences(g->dev, 1, &g->fence, VK_TRUE, UINT64_MAX);
}

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
    if (argc < 3) {
        fprintf(stderr, "usage: %s <fake_xr library> <scratch folder>\n", argv[0]);
        return 2;
    }
    snprintf(scratch, sizeof scratch, "%s", argv[2]);
    char log_path[1200], cap_dir[1200];
    snprintf(log_path, sizeof log_path, "%s/vulkan.log", scratch);
    snprintf(cap_dir, sizeof cap_dir, "%s", scratch);
    set_env("FAKE_XR_LOG", log_path);
    set_env("FAKE_XR_CAPTURE", cap_dir);
    set_env("FAKE_XR_CAPTURE_EVERY", "1");
    load(argv[1]);
    if (XR_FAILED(gipa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&xrCreateInstance))) return 2;

    VR_TEST("vulkan instance");
    XrInstance inst = make_instance();
#define RESOLVE(n) gipa(inst, "xr" #n, (PFN_xrVoidFunction *)&xr##n);
    XR_FUNCS(RESOLVE)
#undef RESOLVE
    XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO, NULL, XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    XrSystemId sys = 0;
    CHECK_XR(xrGetSystem(inst, &sgi, &sys), XR_SUCCESS);

    // a session before the requirements call is refused
    VR_TEST("requirements first");
    XrGraphicsBindingVulkanKHR bind = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO, &bind, 0, sys};
    XrSession s = XR_NULL_HANDLE;
    CHECK_XR(xrCreateSession(inst, &sci, &s), XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING);

    VR_TEST("vulkan device");
    gpu g = {0};
    if (!make_gpu(inst, sys, &g)) {
        xrDestroyInstance(inst);
        printf("test_fake_xr_vulkan: skipped (no Vulkan driver)\n");
        return 0;
    }
    bind.instance = g.inst;
    bind.physicalDevice = g.phys;
    bind.device = g.dev;
    bind.queueFamilyIndex = g.qfam;
    bind.queueIndex = 0;
    CHECK_XR(xrCreateSession(inst, &sci, &s), XR_SUCCESS);
    while (poll_state(inst) != XR_SESSION_STATE_READY) {}
    XrSessionBeginInfo sbi = {XR_TYPE_SESSION_BEGIN_INFO, NULL, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
    CHECK_XR(xrBeginSession(s, &sbi), XR_SUCCESS);
    XrReferenceSpaceCreateInfo rci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO, NULL, XR_REFERENCE_SPACE_TYPE_LOCAL,
                                      {{0, 0, 0, 1}, {0, 0, 0}}};
    XrSpace local = XR_NULL_HANDLE;
    CHECK_XR(xrCreateReferenceSpace(s, &rci, &local), XR_SUCCESS);

    VR_TEST("swapchain");
    int64_t formats[16];
    uint32_t nf = 0;
    CHECK_XR(xrEnumerateSwapchainFormats(s, 16, &nf, formats), XR_SUCCESS);
    bool has_srgb = false;
    for (uint32_t i = 0; i < nf; i++) has_srgb |= formats[i] == VK_FORMAT_B8G8R8A8_SRGB;
    CHECK(has_srgb);
    XrSwapchainCreateInfo swci = {XR_TYPE_SWAPCHAIN_CREATE_INFO, NULL, 0,
                                  XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
                                  VK_FORMAT_B8G8R8A8_SRGB, 1, 64, 32, 1, 1, 1};
    XrSwapchain sc = XR_NULL_HANDLE;
    swci.format = VK_FORMAT_D32_SFLOAT;
    CHECK_XR(xrCreateSwapchain(s, &swci, &sc), XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED);
    swci.format = VK_FORMAT_B8G8R8A8_SRGB;
    CHECK_XR(xrCreateSwapchain(s, &swci, &sc), XR_SUCCESS);
    uint32_t ni = 0;
    CHECK_XR(xrEnumerateSwapchainImages(sc, 0, &ni, NULL), XR_SUCCESS);
    XrSwapchainImageVulkanKHR imgs[8];
    for (int i = 0; i < 8; i++) imgs[i] = (XrSwapchainImageVulkanKHR){XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR};
    CHECK(ni >= 2 && ni <= 8);
    CHECK_XR(xrEnumerateSwapchainImages(sc, ni, &ni, (XrSwapchainImageBaseHeader *)imgs), XR_SUCCESS);

    VR_TEST("image order");
    CHECK_XR(xrReleaseSwapchainImage(sc, NULL), XR_ERROR_CALL_ORDER_INVALID);  // nothing acquired
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, NULL, XR_INFINITE_DURATION};
    CHECK_XR(xrWaitSwapchainImage(sc, &wi), XR_ERROR_CALL_ORDER_INVALID);

    VR_TEST("frames");
    XrCompositionLayerQuad quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.space = local;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = sc;
    quad.subImage.imageRect = (XrRect2Di){{0, 0}, {64, 32}};
    quad.pose = (XrPosef){{0, 0, 0, 1}, {0, 0, -2}};
    quad.size = (XrExtent2Df){2.0f, 1.0f};
    const XrCompositionLayerBaseHeader *layers[] = {(const XrCompositionLayerBaseHeader *)&quad};
    const float colours[3][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {1, 0.5f, 0, 1}};
    for (int f = 0; f < 6; f++) {
        XrFrameState fs = {XR_TYPE_FRAME_STATE};
        CHECK_XR(xrWaitFrame(s, NULL, &fs), XR_SUCCESS);
        CHECK_XR(xrBeginFrame(s, NULL), XR_SUCCESS);
        uint32_t idx = 99;
        CHECK_XR(xrAcquireSwapchainImage(sc, NULL, &idx), XR_SUCCESS);
        CHECK(idx < ni);
        CHECK_XR(xrWaitSwapchainImage(sc, &wi), XR_SUCCESS);
        clear_image(&g, imgs[idx].image, colours[f % 3]);
        CHECK_XR(xrReleaseSwapchainImage(sc, NULL), XR_SUCCESS);
        XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO, NULL, fs.predictedDisplayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
                             1, layers};
        if (f == 0) {  // a rectangle outside the image is refused, and the frame can still be ended
            quad.subImage.imageRect.extent.width = 65;
            CHECK_XR(xrEndFrame(s, &ei), XR_ERROR_SWAPCHAIN_RECT_INVALID);
            quad.subImage.imageRect.extent.width = 64;
        }
        CHECK_XR(xrEndFrame(s, &ei), XR_SUCCESS);
        XrSessionState st;
        while ((st = poll_state(inst)) != XR_SESSION_STATE_UNKNOWN) {}
    }

    VR_TEST("captures");
    char *log = read_file(log_path);
    CHECK(log != NULL);
    // frames 1..6 cleared red, green, orange, red ...; an sRGB image stores 0.5 as 188
    CHECK(log && strstr(log, "capture frame000001_layer0_quad 64x32 mean 255.0 0.0 0.0 lit 100.0%") != NULL);
    CHECK(log && strstr(log, "capture frame000002_layer0_quad 64x32 mean 0.0 255.0 0.0 lit 100.0%") != NULL);
    CHECK(log && strstr(log, "capture frame000003_layer0_quad 64x32 mean 255.0 188.0 0.0 lit 100.0%") != NULL);
    if (log && !strstr(log, "capture frame000003")) fprintf(stderr, "--- fake_xr log:\n%s\n", log);
    free(log);
    char ppm[1300];
    snprintf(ppm, sizeof ppm, "%s/frame000002_layer0_quad.ppm", cap_dir);
    char *img = read_file(ppm);
    CHECK(img && !strncmp(img, "P6\n64 32\n255\n", 13));
    free(img);

    VR_TEST("exit");
    CHECK_XR(xrRequestExitSession(s), XR_SUCCESS);
    XrSessionState st;
    while ((st = poll_state(inst)) != XR_SESSION_STATE_STOPPING && st != XR_SESSION_STATE_UNKNOWN) {}
    CHECK_EQ_INT(st, XR_SESSION_STATE_STOPPING);
    CHECK_XR(xrEndSession(s), XR_SUCCESS);
    CHECK_XR(xrDestroySwapchain(sc), XR_SUCCESS);
    CHECK_XR(xrDestroySpace(local), XR_SUCCESS);
    CHECK_XR(xrDestroySession(s), XR_SUCCESS);
    free_gpu(&g);
    CHECK_XR(xrDestroyInstance(inst), XR_SUCCESS);
    set_env("FAKE_XR_LOG", NULL);
    set_env("FAKE_XR_CAPTURE", NULL);
    set_env("FAKE_XR_CAPTURE_EVERY", NULL);
    return VR_TEST_END("test_fake_xr_vulkan");
}
