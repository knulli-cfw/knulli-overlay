/* VK_LAYER_KNULLI_overlay
 *
 * The Vulkan half of knulli-overlay: an implicit layer that draws the same
 * panel, status pill, clock and bezel onto the frame the application is about
 * to present.  A GL hook cannot reach a Vulkan application, and the loader
 * gives layers exactly the place a hook would want -- vkQueuePresentKHR, with
 * the swapchain image and the semaphores the frame is waiting on.
 *
 * It is deliberately narrow: a swapchain it cannot draw into is left alone,
 * and any failure sets the layer aside for that swapchain rather than taking
 * the application down with it.
 */
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include "ov_frame.h"
#include "ov_vk.h"

/* Not defined by vk_layer.h; the loader only needs the symbols visible. */
#define VK_LAYER_EXPORT __attribute__((visibility("default")))

struct instance_data {
    VkInstance                instance;
    PFN_vkGetInstanceProcAddr gpa;
    PFN_vkDestroyInstance     DestroyInstance;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR
                              GetPhysicalDeviceSurfaceCapabilitiesKHR;
    PFN_vkGetDisplayModePropertiesKHR GetDisplayModePropertiesKHR;
    PFN_vkCreateDisplayPlaneSurfaceKHR CreateDisplayPlaneSurfaceKHR;
    struct instance_data     *next;
};

struct device_data {
    VkDevice         device;
    VkPhysicalDevice gpu;
    uint32_t         queue_family;
    VkPhysicalDeviceMemoryProperties mem;
    ov_vk_fns        vk;

    PFN_vkGetDeviceProcAddr     gdpa;
    PFN_vkDestroyDevice         DestroyDevice;
    PFN_vkCreateSwapchainKHR    CreateSwapchainKHR;
    PFN_vkDestroySwapchainKHR   DestroySwapchainKHR;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR;
    PFN_vkQueuePresentKHR       QueuePresentKHR;

    struct device_data *next;
};

struct swapchain_data {
    VkSwapchainKHR      swapchain;
    struct device_data *dd;
    ov_vk              *renderer;
    VkExtent2D          extent;
    /* The private image a layer above copies from, per swapchain image. */
    VkImage             shadow[OV_VK_MAX_IMAGES];
    /* Rotated presentation: the application draws into images of its own that
     * we hand it, laid out the way round it thinks the screen is, and present
     * turns them into the real swapchain.  Empty when not rotating. */
    int                 rotated;
    VkExtent2D          app_extent;
    VkImage             owned[OV_VK_MAX_IMAGES];
    VkDeviceMemory      owned_mem[OV_VK_MAX_IMAGES];
    uint32_t            owned_count;
    struct swapchain_data *next;
};

/* Where in the frame the overlay is drawn.
 *
 * Normally it goes in before the present is passed down, and the present then
 * waits on our semaphore -- ordered, and nothing else writes the image.
 *
 * The PowerVR handhelds are not like that. Their flip latches a frame late, so
 * the image handed back by vkAcquireNextImageKHR is the one being scanned out;
 * that is why VK_LAYER_KNULLI_powervr_present exists, giving the application a
 * private shadow image and copying the finished frame across in one short
 * burst that fits inside blanking. Anything *we* write to the real image lands
 * in the same live buffer, and lands later than the copy -- which shows as the
 * bottom of the screen flickering, the part the beam has not reached yet.
 *
 * So when that layer is there, the overlay is recorded into its own command
 * buffer, drawn onto the shadow image just before the copy reads it. The copy
 * then carries the overlay across, and nothing of ours touches the buffer the
 * panel is scanning. */
enum { OV_VK_BEFORE_PRESENT, OV_VK_INTO_SHADOW };

#define OV_POWERVR_LAYER_JSON \
    "/usr/share/vulkan/implicit_layer.d/knulli_powervr_present.json"

static int g_mode = -1;
static int g_told_idle;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* Frame pacing, printed once a second under $OV_DEBUG.  "flickering" and
 * "doesn't match the refresh rate" are both things this can tell apart: a
 * frame interval that is steady at the panel's period is neither. */
static void note_present(void)
{
    static struct timespec last;
    static double sum_ms, worst_ms;
    static unsigned frames, late, report;
    struct timespec now;
    double ms;

    if (!getenv("OV_DEBUG"))
        return;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (last.tv_sec) {
        ms = (now.tv_sec - last.tv_sec) * 1000.0 +
             (now.tv_nsec - last.tv_nsec) / 1000000.0;
        sum_ms += ms;
        if (ms > worst_ms)
            worst_ms = ms;
        if (ms > 24.0)              /* missed a 63Hz vblank */
            late++;
        frames++;
    }
    last = now;
    if (frames >= 60) {
        ov_log("vulkan: %u frames, %.1f ms average, %.1f ms worst, %u late "
               "(report %u)", frames, sum_ms / frames, worst_ms, late,
               ++report);
        frames = late = 0;
        sum_ms = worst_ms = 0.0;
    }
}

static int draw_mode(void)
{
    const char *force = getenv("OV_VK_INTO_SHADOW");
    const char *off;
    FILE *f;

    if (g_mode >= 0)
        return g_mode;
    if (force) {
        g_mode = atoi(force) ? OV_VK_INTO_SHADOW : OV_VK_BEFORE_PRESENT;
        return g_mode;
    }
    g_mode = OV_VK_BEFORE_PRESENT;
    off = getenv("KNULLI_DISABLE_POWERVR_PRESENT_LAYER");
    if (off && atoi(off))
        return g_mode;
    /* The manifest is the only thing a layer below can see of a layer above. */
    f = fopen(OV_POWERVR_LAYER_JSON, "r");
    if (f) {
        fclose(f);
        g_mode = OV_VK_INTO_SHADOW;
        ov_log("vulkan: powervr present layer is installed, drawing into the "
               "shadow image before its copy");
    }
    return g_mode;
}
static struct instance_data  *g_instances;
static struct device_data    *g_devices;
static struct swapchain_data *g_swapchains;

/* Quarter turns clockwise the presented image needs, 0..3.
 *
 * A panel mounted rotated cannot be turned by this driver -- KHR_display
 * reports IDENTITY as its only supported transform -- and the application
 * lays its whole world out, menus included, against the size the surface
 * reports.  So the surface is reported turned, the application draws into
 * images of that shape, and present rotates them onto the real swapchain.
 * That is why this is done here rather than per application: it covers
 * whatever the process draws, not just its emulated content.
 *
 * $OV_VK_ROTATE takes degrees (0, 90, 180, 270); unset means no rotation
 * and the layer stays a pass-through. */
static int rotate_quarters(void)
{
    static int cached = -1;
    const char *env;

    if (cached >= 0)
        return cached;
    cached = 0;
    env = getenv("OV_VK_ROTATE");
    if (env)
        cached = (atoi(env) / 90) & 3;
    if (cached)
        ov_log("vulkan: presenting rotated by %d degrees", cached * 90);
    return cached;
}

/* 90 and 270 swap width and height; 0 and 180 do not. */
static VkExtent2D turned(VkExtent2D e, int quarters)
{
    VkExtent2D out = e;

    if (quarters & 1) {
        out.width = e.height;
        out.height = e.width;
    }
    return out;
}

static int memory_type_for(const struct device_data *dd, uint32_t bits,
                           VkMemoryPropertyFlags want)
{
    uint32_t i;

    for (i = 0; i < dd->mem.memoryTypeCount; i++)
        if ((bits & (1u << i)) &&
            (dd->mem.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

static struct instance_data *find_instance(VkInstance i)
{
    struct instance_data *d;

    for (d = g_instances; d; d = d->next)
        if (d->instance == i)
            return d;
    return NULL;
}

static struct device_data *find_device(VkDevice v)
{
    struct device_data *d;

    for (d = g_devices; d; d = d->next)
        if (d->device == v)
            return d;
    return NULL;
}

static struct swapchain_data *find_swapchain(VkSwapchainKHR s)
{
    struct swapchain_data *d;

    for (d = g_swapchains; d; d = d->next)
        if (d->swapchain == s)
            return d;
    return NULL;
}

/* ------------------------------------------------------------------ */

static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateInstance(const VkInstanceCreateInfo *ci,
                     const VkAllocationCallbacks *alloc, VkInstance *out)
{
    VkLayerInstanceCreateInfo *chain = (VkLayerInstanceCreateInfo *)ci->pNext;
    PFN_vkGetInstanceProcAddr gpa;
    PFN_vkCreateInstance create;
    struct instance_data *id;
    VkResult r;

    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO
                      && chain->function == VK_LAYER_LINK_INFO))
        chain = (VkLayerInstanceCreateInfo *)chain->pNext;
    if (!chain)
        return VK_ERROR_INITIALIZATION_FAILED;

    gpa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

    create = (PFN_vkCreateInstance)gpa(NULL, "vkCreateInstance");
    if (!create)
        return VK_ERROR_INITIALIZATION_FAILED;
    r = create(ci, alloc, out);
    if (r != VK_SUCCESS)
        return r;

    id = calloc(1, sizeof(*id));
    if (!id)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    id->instance = *out;
    id->gpa = gpa;
    id->DestroyInstance = (PFN_vkDestroyInstance)gpa(*out, "vkDestroyInstance");
    id->GetPhysicalDeviceSurfaceCapabilitiesKHR =
        (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)
        gpa(*out, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    id->GetDisplayModePropertiesKHR =
        (PFN_vkGetDisplayModePropertiesKHR)
        gpa(*out, "vkGetDisplayModePropertiesKHR");
    id->CreateDisplayPlaneSurfaceKHR =
        (PFN_vkCreateDisplayPlaneSurfaceKHR)
        gpa(*out, "vkCreateDisplayPlaneSurfaceKHR");
    id->GetPhysicalDeviceMemoryProperties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)
        gpa(*out, "vkGetPhysicalDeviceMemoryProperties");

    pthread_mutex_lock(&g_lock);
    id->next = g_instances;
    g_instances = id;
    pthread_mutex_unlock(&g_lock);

    ov_frame_init();
    ov_log("vulkan: instance created, layer active");
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
layer_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *alloc)
{
    struct instance_data *id, **pp;
    PFN_vkDestroyInstance destroy = NULL;

    pthread_mutex_lock(&g_lock);
    for (pp = &g_instances; *pp; pp = &(*pp)->next)
        if ((*pp)->instance == instance) {
            id = *pp;
            *pp = id->next;
            destroy = id->DestroyInstance;
            free(id);
            break;
        }
    pthread_mutex_unlock(&g_lock);
    if (destroy)
        destroy(instance, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateDevice(VkPhysicalDevice gpu, const VkDeviceCreateInfo *ci,
                   const VkAllocationCallbacks *alloc, VkDevice *out)
{
    VkLayerDeviceCreateInfo *chain = (VkLayerDeviceCreateInfo *)ci->pNext;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkCreateDevice create;
    struct instance_data *id;
    struct device_data *dd;
    VkResult r;

    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO
                      && chain->function == VK_LAYER_LINK_INFO))
        chain = (VkLayerDeviceCreateInfo *)chain->pNext;
    if (!chain)
        return VK_ERROR_INITIALIZATION_FAILED;

    gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

    create = (PFN_vkCreateDevice)gipa(NULL, "vkCreateDevice");
    if (!create)
        return VK_ERROR_INITIALIZATION_FAILED;
    r = create(gpu, ci, alloc, out);
    if (r != VK_SUCCESS)
        return r;

    dd = calloc(1, sizeof(*dd));
    if (!dd)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    dd->device = *out;
    dd->gpu = gpu;
    dd->gdpa = gdpa;
    dd->queue_family = ci->queueCreateInfoCount
                       ? ci->pQueueCreateInfos[0].queueFamilyIndex : 0;

#define GD(name) dd->name = (PFN_vk##name)gdpa(*out, "vk" #name)
    GD(DestroyDevice);
    GD(CreateSwapchainKHR);
    GD(DestroySwapchainKHR);
    GD(GetSwapchainImagesKHR);
    GD(QueuePresentKHR);
#undef GD
#define OV_VK_FN(name) \
    dd->vk.name = (PFN_vk##name)gdpa(*out, "vk" #name);
    OV_VK_DEVICE_FNS(OV_VK_FN)
#undef OV_VK_FN

    ov_log("vulkan: device created, queue family %u", dd->queue_family);

    pthread_mutex_lock(&g_lock);
    /* vkCreateDevice does not say which instance the physical device came
     * from, and a game has one; take the newest that can answer. */
    for (id = g_instances; id; id = id->next)
        if (id->GetPhysicalDeviceMemoryProperties) {
            id->GetPhysicalDeviceMemoryProperties(gpu, &dd->mem);
            break;
        }
    dd->next = g_devices;
    g_devices = dd;
    pthread_mutex_unlock(&g_lock);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
layer_DestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc)
{
    struct device_data *dd = NULL, **pp;
    PFN_vkDestroyDevice destroy = NULL;

    pthread_mutex_lock(&g_lock);
    for (pp = &g_devices; *pp; pp = &(*pp)->next)
        if ((*pp)->device == device) {
            dd = *pp;
            *pp = dd->next;
            destroy = dd->DestroyDevice;
            break;
        }
    pthread_mutex_unlock(&g_lock);
    free(dd);
    if (destroy)
        destroy(device, alloc);
}

/* ------------------------------------------------------------------ */

/* Reports the surface turned, so the application lays itself out -- menus,
 * OSD and all -- for the screen as the player sees it rather than as the panel
 * is wired.  The extent is the only thing changed; the real one is restored in
 * CreateSwapchainKHR. */
static VKAPI_ATTR VkResult VKAPI_CALL
layer_GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice gpu,
                                              VkSurfaceKHR surface,
                                              VkSurfaceCapabilitiesKHR *caps)
{
    struct instance_data *id;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR next = NULL;
    int quarters = rotate_quarters();
    VkResult r;

    pthread_mutex_lock(&g_lock);
    for (id = g_instances; id; id = id->next)
        if (id->GetPhysicalDeviceSurfaceCapabilitiesKHR) {
            next = id->GetPhysicalDeviceSurfaceCapabilitiesKHR;
            break;
        }
    pthread_mutex_unlock(&g_lock);
    if (!next)
        return VK_ERROR_INITIALIZATION_FAILED;

    r = next(gpu, surface, caps);
    if (r != VK_SUCCESS || !(quarters & 1))
        return r;

    caps->currentExtent = turned(caps->currentExtent, quarters);
    caps->minImageExtent = turned(caps->minImageExtent, quarters);
    caps->maxImageExtent = turned(caps->maxImageExtent, quarters);
    return r;
}

/* An application that drives KHR_display itself -- PPSSPP does -- asks the
 * display for its modes rather than asking the surface how big it is, so the
 * lie in GetPhysicalDeviceSurfaceCapabilitiesKHR never reaches it.  Report the
 * modes turned as well, and it looks for, and finds, a mode the shape of the
 * screen the player sees. */
static VKAPI_ATTR VkResult VKAPI_CALL
layer_GetDisplayModePropertiesKHR(VkPhysicalDevice gpu, VkDisplayKHR display,
                                  uint32_t *count,
                                  VkDisplayModePropertiesKHR *props)
{
    struct instance_data *id;
    PFN_vkGetDisplayModePropertiesKHR next = NULL;
    int quarters = rotate_quarters();
    VkResult r;
    uint32_t i;

    pthread_mutex_lock(&g_lock);
    for (id = g_instances; id; id = id->next)
        if (id->GetDisplayModePropertiesKHR) {
            next = id->GetDisplayModePropertiesKHR;
            break;
        }
    pthread_mutex_unlock(&g_lock);
    if (!next)
        return VK_ERROR_INITIALIZATION_FAILED;

    r = next(gpu, display, count, props);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !props || !(quarters & 1))
        return r;

    for (i = 0; i < *count; i++) {
        VkExtent2D e = props[i].parameters.visibleRegion;

        props[i].parameters.visibleRegion.width = e.height;
        props[i].parameters.visibleRegion.height = e.width;
    }
    return r;
}

/* ... and the surface it then asks for is described in those turned terms, so
 * put the extent back the way the display wants it. */
static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateDisplayPlaneSurfaceKHR(VkInstance instance,
                                   const VkDisplaySurfaceCreateInfoKHR *ci,
                                   const VkAllocationCallbacks *alloc,
                                   VkSurfaceKHR *out)
{
    struct instance_data *id;
    PFN_vkCreateDisplayPlaneSurfaceKHR next = NULL;
    VkDisplaySurfaceCreateInfoKHR ours;
    int quarters = rotate_quarters();

    pthread_mutex_lock(&g_lock);
    for (id = g_instances; id; id = id->next)
        if (id->CreateDisplayPlaneSurfaceKHR) {
            next = id->CreateDisplayPlaneSurfaceKHR;
            break;
        }
    pthread_mutex_unlock(&g_lock);
    if (!next)
        return VK_ERROR_INITIALIZATION_FAILED;

    ours = *ci;
    if (quarters & 1) {
        ours.imageExtent = turned(ci->imageExtent, quarters);
        ov_log("vulkan: display surface asked for %ux%u, making it %ux%u",
               ci->imageExtent.width, ci->imageExtent.height,
               ours.imageExtent.width, ours.imageExtent.height);
    }
    return next(instance, &ours, alloc, out);
}

/* The application only ever sees the images it draws into, which for a rotated
 * presentation are ours and not the swapchain's. */
static VKAPI_ATTR VkResult VKAPI_CALL
layer_GetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain,
                            uint32_t *count, VkImage *images)
{
    struct swapchain_data *sd;
    struct device_data *dd;
    uint32_t i, n;

    pthread_mutex_lock(&g_lock);
    dd = find_device(device);
    sd = find_swapchain(swapchain);
    pthread_mutex_unlock(&g_lock);
    if (!dd)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!sd || !sd->rotated)
        return dd->GetSwapchainImagesKHR(device, swapchain, count, images);

    if (!images) {
        *count = sd->owned_count;
        return VK_SUCCESS;
    }
    n = *count < sd->owned_count ? *count : sd->owned_count;
    for (i = 0; i < n; i++)
        images[i] = sd->owned[i];
    *count = n;
    return n < sd->owned_count ? VK_INCOMPLETE : VK_SUCCESS;
}

/* One image the application draws into, in its own orientation. */
static int make_owned_image(struct swapchain_data *sd, VkFormat format,
                            VkImageUsageFlags usage, uint32_t index)
{
    struct device_data *dd = sd->dd;
    VkImageCreateInfo ii;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo mi;
    int type;

    memset(&ii, 0, sizeof(ii));
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent.width = sd->app_extent.width;
    ii.extent.height = sd->app_extent.height;
    ii.extent.depth = 1;
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    /* What the application asked of a swapchain image, plus the sampling the
     * rotate at present needs. */
    ii.usage = usage | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (dd->vk.CreateImage(dd->device, &ii, NULL, &sd->owned[index]) != VK_SUCCESS)
        return 0;
    dd->vk.GetImageMemoryRequirements(dd->device, sd->owned[index], &req);
    type = memory_type_for(dd, req.memoryTypeBits,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0)
        return 0;
    memset(&mi, 0, sizeof(mi));
    mi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mi.allocationSize = req.size;
    mi.memoryTypeIndex = (uint32_t)type;
    if (dd->vk.AllocateMemory(dd->device, &mi, NULL, &sd->owned_mem[index])
            != VK_SUCCESS)
        return 0;
    return dd->vk.BindImageMemory(dd->device, sd->owned[index],
                                  sd->owned_mem[index], 0) == VK_SUCCESS;
}

static void free_owned_images(struct swapchain_data *sd)
{
    struct device_data *dd = sd->dd;
    uint32_t i;

    for (i = 0; i < sd->owned_count; i++) {
        if (sd->owned[i])
            dd->vk.DestroyImage(dd->device, sd->owned[i], NULL);
        if (sd->owned_mem[i])
            dd->vk.FreeMemory(dd->device, sd->owned_mem[i], NULL);
        sd->owned[i] = VK_NULL_HANDLE;
        sd->owned_mem[i] = VK_NULL_HANDLE;
    }
    sd->owned_count = 0;
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *ci,
                         const VkAllocationCallbacks *alloc,
                         VkSwapchainKHR *out)
{
    struct device_data *dd;
    struct swapchain_data *sd;
    VkImage images[OV_VK_MAX_IMAGES];
    VkSwapchainCreateInfoKHR real_ci;
    uint32_t count = OV_VK_MAX_IMAGES;
    int quarters;
    VkResult r;

    pthread_mutex_lock(&g_lock);
    dd = find_device(device);
    pthread_mutex_unlock(&g_lock);
    if (!dd)
        return VK_ERROR_INITIALIZATION_FAILED;

    /* The application asked for the size we reported, which is the screen as
     * the player sees it; the swapchain itself has to be the panel's own way
     * round.  Only the extent differs -- everything else it asked for is what
     * our own images are made with. */
    quarters = rotate_quarters();
    real_ci = *ci;
    if (quarters & 1)
        real_ci.imageExtent = turned(ci->imageExtent, quarters);

    r = dd->CreateSwapchainKHR(device, &real_ci, alloc, out);
    if (r != VK_SUCCESS)
        return r;

    sd = calloc(1, sizeof(*sd));
    if (!sd)
        return VK_SUCCESS;          /* no overlay, but the app is fine */
    sd->swapchain = *out;
    sd->dd = dd;
    sd->extent = real_ci.imageExtent;
    sd->app_extent = ci->imageExtent;

    /* Drawing onto the image means using it as a colour attachment; a
     * swapchain that was not asked for that is left alone. */
    if (!(ci->imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
        ov_log("vulkan: swapchain has no colour attachment usage, no overlay");
    } else if (dd->GetSwapchainImagesKHR(device, *out, &count, images)
               == VK_SUCCESS && count > 0) {
        sd->renderer = ov_vk_create(device, &dd->vk, &dd->mem,
                                    dd->queue_family, real_ci.imageFormat,
                                    real_ci.imageExtent, images, count);
        ov_log("vulkan: swapchain %ux%u, %u images, overlay %s",
               real_ci.imageExtent.width, real_ci.imageExtent.height, count,
               sd->renderer ? "ready" : "unavailable");

        if (quarters & 1) {
            uint32_t i;

            sd->owned_count = count;
            for (i = 0; i < count; i++)
                if (!make_owned_image(sd, ci->imageFormat, ci->imageUsage, i)) {
                    ov_log("vulkan: cannot make a %ux%u image to draw into, "
                           "presenting unrotated",
                           sd->app_extent.width, sd->app_extent.height);
                    free_owned_images(sd);
                    break;
                }
            sd->rotated = sd->owned_count == count;
            for (i = 0; sd->rotated && i < count; i++)
                if (!ov_vk_set_source(sd->renderer, i, sd->owned[i])) {
                    ov_log("vulkan: cannot sample image %u, presenting "
                           "unrotated", i);
                    sd->rotated = 0;
                    free_owned_images(sd);
                }
            if (sd->rotated)
                ov_log("vulkan: application draws %ux%u, panel is %ux%u",
                       sd->app_extent.width, sd->app_extent.height,
                       sd->extent.width, sd->extent.height);
        }
    }

    pthread_mutex_lock(&g_lock);
    sd->next = g_swapchains;
    g_swapchains = sd;
    pthread_mutex_unlock(&g_lock);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
layer_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                          const VkAllocationCallbacks *alloc)
{
    struct swapchain_data *sd = NULL, **pp;
    struct device_data *dd;

    pthread_mutex_lock(&g_lock);
    dd = find_device(device);
    for (pp = &g_swapchains; *pp; pp = &(*pp)->next)
        if ((*pp)->swapchain == swapchain) {
            sd = *pp;
            *pp = sd->next;
            break;
        }
    pthread_mutex_unlock(&g_lock);

    if (sd) {
        if (dd)
            dd->vk.DeviceWaitIdle(dd->device);
        free_owned_images(sd);
        ov_vk_destroy(sd->renderer);
        free(sd);
    }
    if (dd)
        dd->DestroySwapchainKHR(device, swapchain, alloc);
}

/* The drawlist for this swapchain, bezel uploaded if it has changed. */
static void build_drawlist(struct swapchain_data *sd, const ov_frame *frame,
                           int rotation, ov_drawlist *dl)
{
    const ov_bezel *bezel = ov_frame_bezel();
    int w = (int)sd->extent.width;
    int h = (int)sd->extent.height;

    if (rotation & 1) {
        int t = w;
        w = h;
        h = t;
    }
    if (ov_vk_image_gen(sd->renderer) != bezel->gen)
        ov_vk_set_image(sd->renderer, frame->have_bezel ? bezel->img.rgba : NULL,
                        bezel->img.w, bezel->img.h, bezel->gen);
    ov_frame_build(dl, frame, w, h);
}

/* The shadow copy on its way into a command buffer.  Nothing is drawn here:
 * all this takes from it is which private image the layer above copies from
 * for each of our swapchain images, so that the next frame can draw the
 * overlay onto that image directly, before the copy and outside the short
 * window the copy has to fit in. */
static VKAPI_ATTR void VKAPI_CALL
layer_CmdCopyImage(VkCommandBuffer cmd, VkImage src, VkImageLayout src_layout,
                   VkImage dst, VkImageLayout dst_layout, uint32_t count,
                   const VkImageCopy *regions)
{
    struct device_data *dd = NULL;
    struct swapchain_data *sd;

    pthread_mutex_lock(&g_lock);
    for (dd = g_devices; dd && !dd->vk.CmdCopyImage; dd = dd->next)
        ;
    for (sd = g_swapchains; sd; sd = sd->next) {
        int index = ov_vk_image_index(sd->renderer, dst);

        if (index < 0 || ov_vk_image_index(sd->renderer, src) >= 0)
            continue;               /* not a copy into one of ours */
        if (sd->shadow[index] != src) {
            sd->shadow[index] = src;
            ov_log("vulkan: image %d is copied from a shadow", index);
        }
        break;
    }
    pthread_mutex_unlock(&g_lock);

    if (dd)
        dd->vk.CmdCopyImage(cmd, src, src_layout, dst, dst_layout, count,
                            regions);
}


/* $OV_VK_DEBUG_HOOKS only: which views the application builds a framebuffer
 * from, and whether they are views of the images we handed it.  These hooks sit
 * on paths a running game uses constantly, so they are never on by default. */
static int debug_hooks(void)
{
    static int on = -1;

    if (on < 0)
        on = getenv("OV_VK_DEBUG_HOOKS") != NULL;
    return on;
}

static VkImageView g_our_views[OV_VK_MAX_IMAGES];

static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateImageView(VkDevice device, const VkImageViewCreateInfo *ci,
                      const VkAllocationCallbacks *alloc, VkImageView *out)
{
    struct device_data *dd;
    struct swapchain_data *sd;
    VkResult r;
    int mine = -1;

    pthread_mutex_lock(&g_lock);
    dd = find_device(device);
    for (sd = g_swapchains; sd && mine < 0; sd = sd->next) {
        uint32_t i;

        for (i = 0; i < sd->owned_count; i++)
            if (sd->owned[i] == ci->image) {
                mine = (int)i;
                break;
            }
    }
    pthread_mutex_unlock(&g_lock);
    if (!dd)
        return VK_ERROR_INITIALIZATION_FAILED;

    r = dd->vk.CreateImageView(device, ci, alloc, out);
    if (r == VK_SUCCESS && mine >= 0 && mine < OV_VK_MAX_IMAGES) {
        g_our_views[mine] = *out;
        ov_log("vulkan: application made view %p of our image %d",
               (void *)*out, mine);
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo *ci,
                        const VkAllocationCallbacks *alloc, VkFramebuffer *out)
{
    struct device_data *dd;
    uint32_t a, i;

    pthread_mutex_lock(&g_lock);
    dd = find_device(device);
    pthread_mutex_unlock(&g_lock);
    if (!dd)
        return VK_ERROR_INITIALIZATION_FAILED;

    for (a = 0; a < ci->attachmentCount; a++) {
        int mine = -1;

        for (i = 0; i < OV_VK_MAX_IMAGES; i++)
            if (g_our_views[i] && g_our_views[i] == ci->pAttachments[a])
                mine = (int)i;
        ov_log("vulkan: framebuffer %ux%u attachment %u view %p -> %s",
               ci->width, ci->height, a, (void *)ci->pAttachments[a],
               mine >= 0 ? "OURS" : "not ours");
    }
    return dd->vk.CreateFramebuffer(device, ci, alloc, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pi)
{
    struct swapchain_data *sd = NULL;
    struct device_data *dd = NULL;
    VkSemaphore sem = VK_NULL_HANDLE;
    VkPresentInfoKHR ours;
    ov_frame frame;
    ov_drawlist dl;

    pthread_mutex_lock(&g_lock);
    /* One swapchain is the case worth handling; with several, the wait
     * semaphores would have to be split between them, and no handheld here
     * presents to two displays. */
    if (pi->swapchainCount == 1)
        sd = find_swapchain(pi->pSwapchains[0]);
    if (sd)
        dd = sd->dd;
    if (!dd)
        for (dd = g_devices; dd && !dd->QueuePresentKHR; dd = dd->next)
            ;
    pthread_mutex_unlock(&g_lock);
    if (!dd)
        return VK_ERROR_INITIALIZATION_FAILED;

    if (sd && sd->renderer && ov_frame_poll(&frame)) {
        /* When we are the ones turning the frame, that angle governs the
         * overlay too, so the clock and notifications sit the same way up as
         * everything else; $OV_ROTATE stays for the boards where something
         * below us does the turning. */
        int rotation = sd->rotated ? rotate_quarters() : ov_frame_rotation();
        uint32_t index = pi->pImageIndices[0];
        VkImage target = VK_NULL_HANDLE;

        /* On a display whose flip latches a frame late the swapchain image is
         * live, and the application has been given a shadow to render into;
         * draw on the shadow instead and let the copy carry the overlay
         * across.  The first frame has no shadow to aim at yet -- the copy is
         * what tells us -- so it goes without. */
        if (draw_mode() == OV_VK_INTO_SHADOW) {
            pthread_mutex_lock(&g_lock);
            target = index < OV_VK_MAX_IMAGES ? sd->shadow[index]
                                              : VK_NULL_HANDLE;
            pthread_mutex_unlock(&g_lock);
            if (target == VK_NULL_HANDLE)
                goto present;
        }
        build_drawlist(sd, &frame, rotation, &dl);
        sem = ov_vk_draw(sd->renderer, queue, index, target, &dl, rotation,
                         pi->pWaitSemaphores, pi->waitSemaphoreCount,
                         sd->rotated);
    } else if (sd && sd->renderer && !g_told_idle) {
        g_told_idle = 1;
        ov_log("vulkan: present with nothing to draw");
    }

present:
    note_present();
    if (sem == VK_NULL_HANDLE)
        return dd->QueuePresentKHR(queue, pi);

    /* Our submit already waited on what the application was waiting for, so
     * the present now waits on us instead. */
    ours = *pi;
    ours.waitSemaphoreCount = 1;
    ours.pWaitSemaphores = &sem;
    return dd->QueuePresentKHR(queue, &ours);
}

/* ------------------------------------------------------------------ */

#define HOOK(name) if (!strcmp(pName, "vk" #name)) \
    return (PFN_vkVoidFunction)layer_##name

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
layer_GetDeviceProcAddr(VkDevice device, const char *pName)
{
    struct device_data *dd;

    HOOK(GetDeviceProcAddr);
    HOOK(DestroyDevice);
    HOOK(CreateSwapchainKHR);
    HOOK(DestroySwapchainKHR);
    HOOK(GetSwapchainImagesKHR);
    HOOK(QueuePresentKHR);
    if (debug_hooks()) {
        HOOK(CreateImageView);
        HOOK(CreateFramebuffer);
    }
    if (draw_mode() == OV_VK_INTO_SHADOW)
        HOOK(CmdCopyImage);

    pthread_mutex_lock(&g_lock);
    dd = find_device(device);
    pthread_mutex_unlock(&g_lock);
    return dd ? dd->gdpa(device, pName) : NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
layer_GetInstanceProcAddr(VkInstance instance, const char *pName)
{
    struct instance_data *id;

    HOOK(GetInstanceProcAddr);
    HOOK(CreateInstance);
    HOOK(DestroyInstance);
    HOOK(CreateDevice);
    HOOK(DestroyDevice);
    HOOK(GetDeviceProcAddr);
    HOOK(CreateSwapchainKHR);
    HOOK(DestroySwapchainKHR);
    HOOK(GetSwapchainImagesKHR);
    HOOK(QueuePresentKHR);
    if (rotate_quarters() & 1) {
        HOOK(GetPhysicalDeviceSurfaceCapabilitiesKHR);
        HOOK(GetDisplayModePropertiesKHR);
        HOOK(CreateDisplayPlaneSurfaceKHR);
    }
    if (draw_mode() == OV_VK_INTO_SHADOW)
        HOOK(CmdCopyImage);

    pthread_mutex_lock(&g_lock);
    id = find_instance(instance);
    pthread_mutex_unlock(&g_lock);
    return id ? id->gpa(instance, pName) : NULL;
}

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *pName)
{
    return layer_GetInstanceProcAddr(instance, pName);
}

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *pName)
{
    return layer_GetDeviceProcAddr(device, pName);
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *iface)
{
    if (iface->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;
    iface->loaderLayerInterfaceVersion = 2;
    iface->pfnGetInstanceProcAddr = layer_GetInstanceProcAddr;
    iface->pfnGetDeviceProcAddr = layer_GetDeviceProcAddr;
    iface->pfnGetPhysicalDeviceProcAddr = NULL;
    return VK_SUCCESS;
}
