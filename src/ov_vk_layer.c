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

static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *ci,
                         const VkAllocationCallbacks *alloc,
                         VkSwapchainKHR *out)
{
    struct device_data *dd;
    struct swapchain_data *sd;
    VkImage images[OV_VK_MAX_IMAGES];
    uint32_t count = OV_VK_MAX_IMAGES;
    VkResult r;

    pthread_mutex_lock(&g_lock);
    dd = find_device(device);
    pthread_mutex_unlock(&g_lock);
    if (!dd)
        return VK_ERROR_INITIALIZATION_FAILED;

    r = dd->CreateSwapchainKHR(device, ci, alloc, out);
    if (r != VK_SUCCESS)
        return r;

    sd = calloc(1, sizeof(*sd));
    if (!sd)
        return VK_SUCCESS;          /* no overlay, but the app is fine */
    sd->swapchain = *out;
    sd->dd = dd;
    sd->extent = ci->imageExtent;

    /* Drawing onto the image means using it as a colour attachment; a
     * swapchain that was not asked for that is left alone. */
    if (!(ci->imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
        ov_log("vulkan: swapchain has no colour attachment usage, no overlay");
    } else if (dd->GetSwapchainImagesKHR(device, *out, &count, images)
               == VK_SUCCESS && count > 0) {
        sd->renderer = ov_vk_create(device, &dd->vk, &dd->mem,
                                    dd->queue_family, ci->imageFormat,
                                    ci->imageExtent, images, count);
        ov_log("vulkan: swapchain %ux%u, %u images, overlay %s",
               ci->imageExtent.width, ci->imageExtent.height, count,
               sd->renderer ? "ready" : "unavailable");
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
        int rotation = ov_frame_rotation();
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
                         pi->pWaitSemaphores, pi->waitSemaphoreCount);
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
    HOOK(QueuePresentKHR);
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
    HOOK(QueuePresentKHR);
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
