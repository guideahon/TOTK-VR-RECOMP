// fake_xr's graphics: XR_KHR_vulkan_enable2, swapchains of Vulkan images on the app's device, and captures of what the
// app submits (FAKE_XR_CAPTURE). Included by fake_xr.c (it shares its handles and helpers).
//
// The rules an app must keep, and fake_xr checks:
//   - xrGetVulkanGraphicsRequirements2KHR before xrCreateSession (else XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING);
//   - the session's device is the one xrCreateVulkanDeviceKHR made from xrGetVulkanGraphicsDevice2KHR's GPU;
//   - swapchain images: acquire -> wait -> release, in order (XR_ERROR_CALL_ORDER_INVALID otherwise);
//   - a layer names a swapchain that has had an image released, and a rectangle inside it.
// A captured image is read on the session's queue during xrEndFrame (OpenXR lets a runtime use it there): the app's
// writes before the release are ordered before the copy by queue submission order and a full barrier.

#define FXVK_IMAGES 3
#define MAGIC_SWAPCHAIN 0x46585731u

typedef struct {
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
#define FXVK_DEVICE_FUNCS(X)                                                                                         \
    X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindImageMemory)  \
    X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) \
    X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer)  \
    X(CmdPipelineBarrier) X(CmdCopyImageToBuffer) X(GetDeviceQueue) X(QueueSubmit) X(CreateFence) X(DestroyFence)   \
    X(WaitForFences) X(ResetFences) X(ResetCommandBuffer) X(QueueWaitIdle)
#define FXVK_DECLARE(n) PFN_vk##n n;
    FXVK_DEVICE_FUNCS(FXVK_DECLARE)
#undef FXVK_DECLARE
} fxvk_fns;

typedef struct fxvk {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    uint32_t qfam, qindex;
    VkQueue queue;
    fxvk_fns f;
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkFence fence;
    int swapchains;  // alive
} fxvk;

typedef struct {
    uint32_t magic;
    session *s;
    VkFormat format;
    uint32_t w, h, layers;
    VkImage img[FXVK_IMAGES];
    VkDeviceMemory mem[FXVK_IMAGES];
    int state[FXVK_IMAGES];  // 0 free (the runtime's), 1 acquired, 2 waited
    int order[FXVK_IMAGES];  // acquired images, oldest first
    int n_order;
    int next;                // the next image to hand out
    int released;            // the last released image, -1 before the first release
} swapchain;

static swapchain *get_swapchain(XrSwapchain h) {
    swapchain *sc = (swapchain *)(uintptr_t)h;
    return sc && sc->magic == MAGIC_SWAPCHAIN ? sc : NULL;
}

// ---- instance-level: requirements, instance, GPU, device ----
static XrResult XRAPI_CALL fx_GetVulkanGraphicsRequirements2KHR(XrInstance h, XrSystemId id,
                                                                 XrGraphicsRequirementsVulkanKHR *req) {
    instance *in = get_instance(h);
    if (!in) return XR_ERROR_HANDLE_INVALID;
    if (!in->vulkan) return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (id != FAKE_XR_SYSTEM_ID) return XR_ERROR_SYSTEM_INVALID;
    if (!req || req->type != XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR) return XR_ERROR_VALIDATION_FAILURE;
    req->minApiVersionSupported = XR_MAKE_VERSION(1, 1, 0);
    req->maxApiVersionSupported = XR_MAKE_VERSION(1, 4, 0);
    in->vk_requirements = true;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_CreateVulkanInstanceKHR(XrInstance h, const XrVulkanInstanceCreateInfoKHR *ci,
                                                       VkInstance *out, VkResult *vr) {
    instance *in = get_instance(h);
    if (!in) return XR_ERROR_HANDLE_INVALID;
    if (!in->vulkan) return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (!ci || !out || !vr || ci->type != XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR || !ci->pfnGetInstanceProcAddr ||
        !ci->vulkanCreateInfo)
        return XR_ERROR_VALIDATION_FAILURE;
    if (ci->systemId != FAKE_XR_SYSTEM_ID) return XR_ERROR_SYSTEM_INVALID;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)ci->pfnGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create) return XR_ERROR_RUNTIME_FAILURE;
    *vr = create(ci->vulkanCreateInfo, ci->vulkanAllocator, out);
    if (*vr == VK_SUCCESS) {
        in->vk_inst = *out;
        in->vk_gipa = ci->pfnGetInstanceProcAddr;
        logf_(in, "vulkan instance created\n");
    }
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_GetVulkanGraphicsDevice2KHR(XrInstance h, const XrVulkanGraphicsDeviceGetInfoKHR *gi,
                                                           VkPhysicalDevice *out) {
    instance *in = get_instance(h);
    if (!in) return XR_ERROR_HANDLE_INVALID;
    if (!in->vulkan) return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (!gi || !out || gi->type != XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR || !gi->vulkanInstance)
        return XR_ERROR_VALIDATION_FAILURE;
    if (gi->systemId != FAKE_XR_SYSTEM_ID) return XR_ERROR_SYSTEM_INVALID;
    if (!in->vk_gipa || gi->vulkanInstance != in->vk_inst) return XR_ERROR_VALIDATION_FAILURE;  // ours only
    PFN_vkEnumeratePhysicalDevices en =
        (PFN_vkEnumeratePhysicalDevices)in->vk_gipa(in->vk_inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties props =
        (PFN_vkGetPhysicalDeviceProperties)in->vk_gipa(in->vk_inst, "vkGetPhysicalDeviceProperties");
    uint32_t n = 0;
    if (!en || !props || en(in->vk_inst, &n, NULL) != VK_SUCCESS || !n) return XR_ERROR_RUNTIME_FAILURE;
    VkPhysicalDevice pd[16];
    if (n > 16) n = 16;
    en(in->vk_inst, &n, pd);
    // FAKE_XR_GPU=<n> picks one; otherwise the first discrete GPU, then the first one (like a headset on its GPU)
    const char *want = getenv("FAKE_XR_GPU");
    int pick = want && *want ? atoi(want) : -1;
    if (pick < 0 || pick >= (int)n) {
        pick = 0;
        for (uint32_t i = 0; i < n; i++) {
            VkPhysicalDeviceProperties p;
            props(pd[i], &p);
            if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { pick = (int)i; break; }
        }
    }
    VkPhysicalDeviceProperties p;
    props(pd[pick], &p);
    logf_(in, "vulkan gpu %d: %s\n", pick, p.deviceName);
    in->vk_phys = pd[pick];
    *out = pd[pick];
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_CreateVulkanDeviceKHR(XrInstance h, const XrVulkanDeviceCreateInfoKHR *ci, VkDevice *out,
                                                     VkResult *vr) {
    instance *in = get_instance(h);
    if (!in) return XR_ERROR_HANDLE_INVALID;
    if (!in->vulkan) return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (!ci || !out || !vr || ci->type != XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR || !ci->pfnGetInstanceProcAddr ||
        !ci->vulkanCreateInfo || !ci->vulkanPhysicalDevice)
        return XR_ERROR_VALIDATION_FAILURE;
    if (ci->systemId != FAKE_XR_SYSTEM_ID) return XR_ERROR_SYSTEM_INVALID;
    if (ci->vulkanPhysicalDevice != in->vk_phys) return XR_ERROR_VALIDATION_FAILURE;  // must be the runtime's GPU
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)ci->pfnGetInstanceProcAddr(in->vk_inst, "vkCreateDevice");
    if (!create) return XR_ERROR_RUNTIME_FAILURE;
    *vr = create(ci->vulkanPhysicalDevice, ci->vulkanCreateInfo, ci->vulkanAllocator, out);
    if (*vr == VK_SUCCESS) {
        in->vk_dev = *out;
        logf_(in, "vulkan device created\n");
    }
    return XR_SUCCESS;
}

// ---- the session's device ----
static XrResult fxvk_session_create(session *s, const XrGraphicsBindingVulkanKHR *b) {
    instance *in = s->inst;
    if (!in->vk_requirements) return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
    if (!b->instance || !b->physicalDevice || !b->device) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    if (b->instance != in->vk_inst || b->physicalDevice != in->vk_phys || b->device != in->vk_dev)
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    fxvk *v = calloc(1, sizeof *v);
    v->inst = b->instance;
    v->phys = b->physicalDevice;
    v->dev = b->device;
    v->qfam = b->queueFamilyIndex;
    v->qindex = b->queueIndex;
    v->f.GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)in->vk_gipa(v->inst, "vkGetDeviceProcAddr");
    v->f.GetPhysicalDeviceMemoryProperties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)in->vk_gipa(v->inst, "vkGetPhysicalDeviceMemoryProperties");
    if (!v->f.GetDeviceProcAddr || !v->f.GetPhysicalDeviceMemoryProperties) { free(v); return XR_ERROR_RUNTIME_FAILURE; }
#define FXVK_LOAD(n)                                                                    \
    v->f.n = (PFN_vk##n)v->f.GetDeviceProcAddr(v->dev, "vk" #n);                        \
    if (!v->f.n) { free(v); return XR_ERROR_RUNTIME_FAILURE; }
    FXVK_DEVICE_FUNCS(FXVK_LOAD)
#undef FXVK_LOAD
    v->f.GetDeviceQueue(v->dev, v->qfam, v->qindex, &v->queue);
    if (!v->queue) { free(v); return XR_ERROR_GRAPHICS_DEVICE_INVALID; }
    s->vk = v;
    logf_(in, "session vulkan queue family %u index %u\n", v->qfam, v->qindex);
    return XR_SUCCESS;
}

static void fxvk_session_destroy(session *s) {
    fxvk *v = s->vk;
    if (!v) return;
    if (v->pool) {
        v->f.DestroyFence(v->dev, v->fence, NULL);
        v->f.DestroyCommandPool(v->dev, v->pool, NULL);
    }
    free(v);
    s->vk = NULL;
}

static uint32_t fxvk_memory_type(fxvk *v, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    v->f.GetPhysicalDeviceMemoryProperties(v->phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

// ---- swapchains ----
static const int64_t swapchain_formats[] = {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM,
                                            VK_FORMAT_R8G8B8A8_UNORM};
#define SWAPCHAIN_FORMAT_COUNT (sizeof swapchain_formats / sizeof *swapchain_formats)

static XrResult XRAPI_CALL fx_EnumerateSwapchainFormats(XrSession h, uint32_t cap, uint32_t *count, int64_t *formats) {
    session *s = get_session(h);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!s->vk) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    ENUMERATE(cap, count, formats, swapchain_formats, (uint32_t)SWAPCHAIN_FORMAT_COUNT);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_DestroySwapchain(XrSwapchain h);

static XrResult XRAPI_CALL fx_CreateSwapchain(XrSession h, const XrSwapchainCreateInfo *ci, XrSwapchain *out) {
    session *s = get_session(h);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!ci || !out || ci->type != XR_TYPE_SWAPCHAIN_CREATE_INFO) return XR_ERROR_VALIDATION_FAILURE;
    if (s->lost) return XR_ERROR_SESSION_LOST;
    fxvk *v = s->vk;
    if (!v) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    bool known = false;
    for (size_t i = 0; i < SWAPCHAIN_FORMAT_COUNT; i++) known |= swapchain_formats[i] == ci->format;
    if (!known) return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
    if (!ci->width || !ci->height || ci->width > 4096 || ci->height > 4096 || ci->sampleCount != 1 || ci->faceCount != 1 ||
        ci->mipCount != 1 || !ci->arraySize || ci->arraySize > 2)
        return XR_ERROR_VALIDATION_FAILURE;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;  // (fake_xr reads them for captures)
    if (ci->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (ci->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (ci->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (ci->usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (ci->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (ci->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) return XR_ERROR_FEATURE_UNSUPPORTED;
    swapchain *sc = calloc(1, sizeof *sc);
    sc->magic = MAGIC_SWAPCHAIN;
    sc->s = s;
    sc->format = (VkFormat)ci->format;
    sc->w = ci->width;
    sc->h = ci->height;
    sc->layers = ci->arraySize;
    sc->released = -1;
    VkImageCreateFlags flags = (ci->createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT) ? VK_IMAGE_CREATE_PROTECTED_BIT : 0;
    if (flags) { free(sc); return XR_ERROR_FEATURE_UNSUPPORTED; }
    for (int i = 0; i < FXVK_IMAGES; i++) {
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = sc->format, .extent = {sc->w, sc->h, 1}, .mipLevels = 1,
                                .arrayLayers = sc->layers, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                                .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        if (v->f.CreateImage(v->dev, &ii, NULL, &sc->img[i]) != VK_SUCCESS) goto fail;
        VkMemoryRequirements mr;
        v->f.GetImageMemoryRequirements(v->dev, sc->img[i], &mr);
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                   .memoryTypeIndex = fxvk_memory_type(v, mr.memoryTypeBits,
                                                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = fxvk_memory_type(v, mr.memoryTypeBits, 0);
        if (v->f.AllocateMemory(v->dev, &ai, NULL, &sc->mem[i]) != VK_SUCCESS) goto fail;
        if (v->f.BindImageMemory(v->dev, sc->img[i], sc->mem[i], 0) != VK_SUCCESS) goto fail;
    }
    v->swapchains++;
    logf_(s->inst, "swapchain %ux%u x%u format %d\n", sc->w, sc->h, sc->layers, (int)sc->format);
    *out = (XrSwapchain)(uintptr_t)sc;
    return XR_SUCCESS;
fail:
    for (int i = 0; i < FXVK_IMAGES; i++) {
        if (sc->img[i]) v->f.DestroyImage(v->dev, sc->img[i], NULL);
        if (sc->mem[i]) v->f.FreeMemory(v->dev, sc->mem[i], NULL);
    }
    free(sc);
    return XR_ERROR_OUT_OF_MEMORY;
}

static XrResult XRAPI_CALL fx_DestroySwapchain(XrSwapchain h) {
    swapchain *sc = get_swapchain(h);
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    fxvk *v = sc->s->vk;
    if (v) {
        v->f.QueueWaitIdle(v->queue);  // (the app's last frames may still read them... in a real compositor too)
        for (int i = 0; i < FXVK_IMAGES; i++) {
            v->f.DestroyImage(v->dev, sc->img[i], NULL);
            v->f.FreeMemory(v->dev, sc->mem[i], NULL);
        }
        v->swapchains--;
    }
    sc->magic = 0;
    free(sc);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_EnumerateSwapchainImages(XrSwapchain h, uint32_t cap, uint32_t *count,
                                                        XrSwapchainImageBaseHeader *images) {
    swapchain *sc = get_swapchain(h);
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    if (!count) return XR_ERROR_VALIDATION_FAILURE;
    *count = FXVK_IMAGES;
    if (!cap) return XR_SUCCESS;
    if (cap < FXVK_IMAGES) return XR_ERROR_SIZE_INSUFFICIENT;
    XrSwapchainImageVulkanKHR *vi = (XrSwapchainImageVulkanKHR *)images;
    for (int i = 0; i < FXVK_IMAGES; i++) {
        if (vi[i].type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR) return XR_ERROR_VALIDATION_FAILURE;
        vi[i].image = sc->img[i];
    }
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_AcquireSwapchainImage(XrSwapchain h, const XrSwapchainImageAcquireInfo *ai,
                                                     uint32_t *index) {
    swapchain *sc = get_swapchain(h);
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    if ((ai && ai->type != XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO) || !index) return XR_ERROR_VALIDATION_FAILURE;
    if (sc->s->lost) return XR_ERROR_SESSION_LOST;
    if (sc->state[sc->next] != 0) return XR_ERROR_CALL_ORDER_INVALID;  // every image is the app's
    int i = sc->next;
    sc->state[i] = 1;
    sc->order[sc->n_order++] = i;
    sc->next = (sc->next + 1) % FXVK_IMAGES;
    *index = (uint32_t)i;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_WaitSwapchainImage(XrSwapchain h, const XrSwapchainImageWaitInfo *wi) {
    swapchain *sc = get_swapchain(h);
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    if (!wi || wi->type != XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO) return XR_ERROR_VALIDATION_FAILURE;
    if (sc->s->lost) return XR_ERROR_SESSION_LOST;
    for (int k = 0; k < sc->n_order; k++)  // the oldest acquired image not waited yet
        if (sc->state[sc->order[k]] == 1) {
            for (int j = 0; j < k; j++)
                if (sc->state[sc->order[j]] == 2) return XR_ERROR_CALL_ORDER_INVALID;  // one waited at a time
            sc->state[sc->order[k]] = 2;
            return XR_SUCCESS;
        }
    return XR_ERROR_CALL_ORDER_INVALID;
}

static XrResult XRAPI_CALL fx_ReleaseSwapchainImage(XrSwapchain h, const XrSwapchainImageReleaseInfo *ri) {
    swapchain *sc = get_swapchain(h);
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    if (ri && ri->type != XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO) return XR_ERROR_VALIDATION_FAILURE;
    if (sc->s->lost) return XR_ERROR_SESSION_LOST;
    if (!sc->n_order || sc->state[sc->order[0]] != 2) return XR_ERROR_CALL_ORDER_INVALID;  // the oldest, waited
    int i = sc->order[0];
    sc->state[i] = 0;
    memmove(sc->order, sc->order + 1, (size_t)(--sc->n_order) * sizeof *sc->order);
    sc->released = i;
    return XR_SUCCESS;
}

// ---- layers ----
static XrResult check_subimage(const XrSwapchainSubImage *si) {
    swapchain *sc = get_swapchain(si->swapchain);
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    if (sc->released < 0) return XR_ERROR_LAYER_INVALID;  // nothing released yet
    if (si->imageArrayIndex >= sc->layers) return XR_ERROR_VALIDATION_FAILURE;
    const XrRect2Di *r = &si->imageRect;
    if (r->offset.x < 0 || r->offset.y < 0 || r->extent.width <= 0 || r->extent.height <= 0 ||
        (uint32_t)(r->offset.x + r->extent.width) > sc->w || (uint32_t)(r->offset.y + r->extent.height) > sc->h)
        return XR_ERROR_SWAPCHAIN_RECT_INVALID;
    return XR_SUCCESS;
}

static XrResult fxvk_check_layer(session *s, const XrCompositionLayerBaseHeader *l) {
    (void)s;
    if (l->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
        const XrCompositionLayerQuad *q = (const XrCompositionLayerQuad *)l;
        if (!(q->size.width > 0) || !(q->size.height > 0)) return XR_ERROR_VALIDATION_FAILURE;
        return check_subimage(&q->subImage);
    }
    if (l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
        const XrCompositionLayerProjection *p = (const XrCompositionLayerProjection *)l;
        for (uint32_t i = 0; i < p->viewCount; i++) {
            if (p->views[i].type != XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW) return XR_ERROR_VALIDATION_FAILURE;
            XrResult r = check_subimage(&p->views[i].subImage);
            if (XR_FAILED(r)) return r;
        }
        return XR_SUCCESS;
    }
    return XR_ERROR_LAYER_INVALID;
}

// ---- captures: the released image of a layer, as a PPM and a line in the log ----
static bool fxvk_bytes_bgra(VkFormat f) { return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_B8G8R8A8_UNORM; }

static void fxvk_capture_one(session *s, const XrSwapchainSubImage *si, const char *tag, bool layer_alpha) {
    instance *in = s->inst;
    fxvk *v = s->vk;
    swapchain *sc = get_swapchain(si->swapchain);
    if (!v || !sc || sc->released < 0) return;
    if (!v->pool) {
        VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = v->qfam};
        if (v->f.CreateCommandPool(v->dev, &pi, NULL, &v->pool) != VK_SUCCESS) return;
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = v->pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        v->f.AllocateCommandBuffers(v->dev, &ai, &v->cb);
        VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        v->f.CreateFence(v->dev, &fi, NULL, &v->fence);
    }
    uint32_t w = (uint32_t)si->imageRect.extent.width, h = (uint32_t)si->imageRect.extent.height;
    VkDeviceSize size = (VkDeviceSize)w * h * 4;
    VkBuffer buf;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (v->f.CreateBuffer(v->dev, &bi, NULL, &buf) != VK_SUCCESS) return;
    VkMemoryRequirements mr;
    v->f.GetBufferMemoryRequirements(v->dev, buf, &mr);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = fxvk_memory_type(v, mr.memoryTypeBits,
                                                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory mem;
    if (mai.memoryTypeIndex == UINT32_MAX || v->f.AllocateMemory(v->dev, &mai, NULL, &mem) != VK_SUCCESS) {
        v->f.DestroyBuffer(v->dev, buf, NULL);
        return;
    }
    v->f.BindBufferMemory(v->dev, buf, mem, 0);
    VkImage img = sc->img[sc->released];
    v->f.ResetCommandBuffer(v->cb, 0);
    VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    v->f.BeginCommandBuffer(v->cb, &cbi);
    // the app hands images back in COLOR_ATTACHMENT_OPTIMAL (XR_KHR_vulkan_enable2)
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                              .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = img,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, si->imageArrayIndex, 1}};
    v->f.CmdPipelineBarrier(v->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                            NULL, 1, &b);
    VkBufferImageCopy rg = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, si->imageArrayIndex, 1},
                            .imageOffset = {si->imageRect.offset.x, si->imageRect.offset.y, 0},
                            .imageExtent = {w, h, 1}};
    v->f.CmdCopyImageToBuffer(v->cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &rg);
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    v->f.CmdPipelineBarrier(v->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                            NULL, 1, &b);
    v->f.EndCommandBuffer(v->cb);
    VkSubmitInfo sub = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &v->cb};
    v->f.ResetFences(v->dev, 1, &v->fence);
    if (v->f.QueueSubmit(v->queue, 1, &sub, v->fence) == VK_SUCCESS &&
        v->f.WaitForFences(v->dev, 1, &v->fence, VK_TRUE, 5000000000ull) == VK_SUCCESS) {
        uint8_t *px = NULL;
        v->f.MapMemory(v->dev, mem, 0, size, 0, (void **)&px);
        bool bgra = fxvk_bytes_bgra(sc->format);
        double sum[3] = {0, 0, 0};
        uint64_t lit = 0, opaque = 0, n = (uint64_t)w * h;
        for (uint64_t i = 0; i < n; i++) {
            uint8_t r = px[i * 4 + (bgra ? 2 : 0)], g = px[i * 4 + 1], bl = px[i * 4 + (bgra ? 0 : 2)];
            sum[0] += r; sum[1] += g; sum[2] += bl;
            if (r > 8 || g > 8 || bl > 8) lit++;
            if (px[i * 4 + 3] >= 128) opaque++;
        }
        // (opaque: the share of pixels with alpha at least a half; a layer blended by its alpha shows only those)
        logf_(in, "capture %s %ux%u mean %.1f %.1f %.1f lit %.1f%% opaque %.1f%%\n", tag, w, h, sum[0] / n, sum[1] / n,
              sum[2] / n, 100.0 * lit / n, 100.0 * opaque / n);
        if (in->cfg.capture_dir[0]) {
            char path[1100];
            snprintf(path, sizeof path, "%s/%s.ppm", in->cfg.capture_dir, tag);
            FILE *f = fopen(path, "wb");
            uint8_t *row = f ? malloc((size_t)w * 3) : NULL;
            if (f && row) {  // (a row at a time: one write per pixel took over a second on Windows)
                fprintf(f, "P6\n%u %u\n255\n", w, h);
                for (uint32_t y = 0; y < h; y++) {
                    const uint8_t *s = px + (uint64_t)y * w * 4;
                    for (uint32_t x = 0; x < w; x++) {
                        row[x * 3] = s[x * 4 + (bgra ? 2 : 0)];
                        row[x * 3 + 1] = s[x * 4 + 1];
                        row[x * 3 + 2] = s[x * 4 + (bgra ? 0 : 2)];
                    }
                    fwrite(row, 1, (size_t)w * 3, f);
                }
            }
            free(row);
            if (f) fclose(f);
            if (layer_alpha) {  // (and its alpha, as a grey PGM next to it)
                snprintf(path, sizeof path, "%s/%s_alpha.pgm", in->cfg.capture_dir, tag);
                FILE *fa = fopen(path, "wb");
                uint8_t *arow = fa ? malloc(w) : NULL;
                if (fa && arow) {
                    fprintf(fa, "P5\n%u %u\n255\n", w, h);
                    for (uint32_t y = 0; y < h; y++) {
                        for (uint32_t x = 0; x < w; x++) arow[x] = px[((uint64_t)y * w + x) * 4 + 3];
                        fwrite(arow, 1, w, fa);
                    }
                }
                free(arow);
                if (fa) fclose(fa);
            }
        }
        v->f.UnmapMemory(v->dev, mem);
    } else {
        logf_(in, "capture %s failed\n", tag);
    }
    v->f.DestroyBuffer(v->dev, buf, NULL);
    v->f.FreeMemory(v->dev, mem, NULL);
}

static void fxvk_capture(session *s, const XrFrameEndInfo *ei, uint64_t frame) {
    config *c = &s->inst->cfg;
    if (!s->vk || c->capture_every <= 0 || frame % (uint64_t)c->capture_every != 0) return;
    for (uint32_t i = 0; i < ei->layerCount; i++) {
        const XrCompositionLayerBaseHeader *l = ei->layers[i];
        char tag[96];
        if (l->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            snprintf(tag, sizeof tag, "frame%06llu_layer%u_quad", (unsigned long long)frame, i);
            fxvk_capture_one(s, &((const XrCompositionLayerQuad *)l)->subImage, tag,
                             (l->layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) != 0);
        } else if (l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const XrCompositionLayerProjection *p = (const XrCompositionLayerProjection *)l;
            for (uint32_t v = 0; v < p->viewCount; v++) {
                snprintf(tag, sizeof tag, "frame%06llu_layer%u_view%u", (unsigned long long)frame, i, v);
                fxvk_capture_one(s, &p->views[v].subImage, tag, false);
            }
        }
    }
}
