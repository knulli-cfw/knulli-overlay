/* Draws an ov_drawlist onto a swapchain image with Vulkan.
 *
 * This is the Vulkan twin of ov_gl.c and takes the same drawing commands, so
 * the panel, the pill, the clock and the bezel look identical whichever API
 * the application presents with.  Nothing here links libvulkan: a layer is
 * handed its function pointers by the loader, and they arrive in ov_vk_fns.
 */
#ifndef OV_VK_H_INCLUDED
#define OV_VK_H_INCLUDED

#include <vulkan/vulkan.h>

#include "ov_layout.h"

/* Swapchains on these devices have two or three images; eight is slack. */
#define OV_VK_MAX_IMAGES 8
/* Present waits on one semaphore in practice; past this we stay out of the
 * way rather than dropping a wait on the floor. */
#define OV_VK_MAX_WAIT 8
/* Panel, pill, clock, notification: four corners, four render passes. */
#define OV_VK_MAX_GROUPS 4

/* Every device entry point the renderer calls.  The layer fills these in from
 * the next vkGetDeviceProcAddr in the chain -- one list, so the struct and the
 * loading loop cannot drift apart. */
#define OV_VK_DEVICE_FNS(X)         \
    X(DeviceWaitIdle)               \
    X(CreateImage)                  \
    X(DestroyImage)                 \
    X(CreateImageView)              \
    X(DestroyImageView)             \
    X(GetImageMemoryRequirements)   \
    X(BindImageMemory)              \
    X(CreateBuffer)                 \
    X(DestroyBuffer)                \
    X(GetBufferMemoryRequirements)  \
    X(BindBufferMemory)             \
    X(AllocateMemory)               \
    X(FreeMemory)                   \
    X(MapMemory)                    \
    X(UnmapMemory)                  \
    X(CreateSampler)                \
    X(DestroySampler)               \
    X(CreateDescriptorSetLayout)    \
    X(DestroyDescriptorSetLayout)   \
    X(CreateDescriptorPool)         \
    X(DestroyDescriptorPool)        \
    X(AllocateDescriptorSets)       \
    X(UpdateDescriptorSets)         \
    X(CreateShaderModule)           \
    X(DestroyShaderModule)          \
    X(CreatePipelineLayout)         \
    X(DestroyPipelineLayout)        \
    X(CreateGraphicsPipelines)      \
    X(DestroyPipeline)              \
    X(CreateRenderPass)             \
    X(DestroyRenderPass)            \
    X(CreateFramebuffer)            \
    X(DestroyFramebuffer)           \
    X(CreateCommandPool)            \
    X(DestroyCommandPool)           \
    X(AllocateCommandBuffers)       \
    X(FreeCommandBuffers)           \
    X(BeginCommandBuffer)           \
    X(EndCommandBuffer)             \
    X(ResetCommandBuffer)           \
    X(CmdBeginRenderPass)           \
    X(CmdEndRenderPass)             \
    X(CmdBindPipeline)              \
    X(CmdBindDescriptorSets)        \
    X(CmdPushConstants)             \
    X(CmdSetViewport)               \
    X(CmdSetScissor)                \
    X(CmdDraw)                      \
    X(CmdPipelineBarrier)           \
    X(CmdCopyBufferToImage)         \
    X(CmdCopyImageToBuffer)         \
    X(CmdCopyImage)                 \
    X(CreateSemaphore)              \
    X(DestroySemaphore)             \
    X(CreateFence)                  \
    X(DestroyFence)                 \
    X(WaitForFences)                \
    X(ResetFences)                  \
    X(QueueSubmit)

typedef struct {
#define OV_VK_FN(name) PFN_vk##name name;
    OV_VK_DEVICE_FNS(OV_VK_FN)
#undef OV_VK_FN
} ov_vk_fns;

typedef struct ov_vk ov_vk;

/* One renderer per swapchain.  Returns NULL when anything is missing, in which
 * case the layer simply presents the frame unchanged. */
ov_vk *ov_vk_create(VkDevice device, const ov_vk_fns *fn,
                    const VkPhysicalDeviceMemoryProperties *mem,
                    uint32_t queue_family, VkFormat format,
                    VkExtent2D extent, const VkImage *images,
                    uint32_t image_count);
void ov_vk_destroy(ov_vk *v);

/* The bezel, as straight RGBA8.  Uploaded on the next draw, which is the only
 * time there is a queue to upload on; a NULL image drops it. */
void     ov_vk_set_image(ov_vk *v, const void *rgba, int w, int h,
                         unsigned gen);
unsigned ov_vk_image_gen(const ov_vk *v);

/* Which of the swapchain images this is, or -1 when it is not one of them. */
int ov_vk_image_index(const ov_vk *v, VkImage image);

/* Lets present sample one of the application's own images (see ov_vk_draw's
 * blit_source).  Returns 0 if the image cannot be sampled. */
int ov_vk_set_source(ov_vk *v, uint32_t index, VkImage image);

/* Records and submits the overlay, waiting on the semaphores the application
 * meant to present with.  Returns the semaphore the caller must present with
 * instead, or VK_NULL_HANDLE when nothing was submitted and the present should
 * go ahead untouched.
 *
 * `target` is normally VK_NULL_HANDLE, meaning the swapchain image at `index`.
 * On a display whose flip latches a frame late, that image is the one being
 * scanned out, and a layer above hands the application a private shadow to
 * render into instead; passing that shadow here draws the overlay where the
 * application drew, and the copy carries it across.  `index` still picks the
 * command buffer and fence to use. */
VkSemaphore ov_vk_draw(ov_vk *v, VkQueue queue, uint32_t index, VkImage target,
                       const ov_drawlist *dl, int rotation,
                       const VkSemaphore *wait, uint32_t wait_count,
                       int blit_source);

#endif /* OV_VK_H_INCLUDED */
