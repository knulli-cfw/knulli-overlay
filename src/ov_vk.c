#include "ov_vk.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ov_atlas.h"
#include "ov_vk_spv.h"

/* Must match the push constant block in shaders/overlay.{vert,frag}. */
typedef struct {
    float   rect[4];
    float   uv[4];
    float   screen[2];
    float   half_size[2];
    float   fill[4];
    float   stroke[4];
    float   radius;
    float   border;
    int32_t mode;
    int32_t rot;
} ov_vk_push;

struct ov_vk {
    VkDevice   device;
    const ov_vk_fns *fn;
    VkPhysicalDeviceMemoryProperties mem;
    uint32_t   queue_family;
    VkExtent2D extent;

    VkRenderPass          pass;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool      desc_pool;
    VkDescriptorSet       set;
    VkPipelineLayout      layout;
    VkPipeline            pipeline;
    VkSampler             sampler;
    VkCommandPool         cmd_pool;

    /* The glyph atlas, and the bezel when there is one. */
    VkImage        atlas;
    VkDeviceMemory atlas_mem;
    VkImageView    atlas_view;
    int            atlas_ready;

    VkImage        image;
    VkDeviceMemory image_mem;
    VkImageView    image_view;
    unsigned       image_gen;
    const void    *pending;         /* not yet uploaded, owned by the caller */
    int            pending_w, pending_h;

    /* The application's own image for each swapchain image, when the layer is
     * rotating the presentation: a view and a descriptor set so the shader can
     * sample it as the full-screen source. */
    int             rot_src;        /* quarter turns the source is turned by */
    VkImage         src_image[OV_VK_MAX_IMAGES];
    VkImageView     src_view[OV_VK_MAX_IMAGES];
    VkDescriptorSet src_set[OV_VK_MAX_IMAGES];

    VkFormat format;
    /* Images that are not ours -- a layer above copying from its own shadow --
     * get a view and a framebuffer here the first time they turn up. */
    struct {
        VkImage       image;
        VkImageView   view;
        VkFramebuffer fb;
    } aux[OV_VK_MAX_IMAGES];

    uint32_t count;
    struct {
        VkImage         image;
        VkImageView     view;
        VkFramebuffer   fb;
        VkCommandBuffer cmd;
        VkFence         fence;
        VkSemaphore     sem;
        int             submitted;
    } frame[OV_VK_MAX_IMAGES];

    int ok;
    int logged;
    int dumped, drawn, dump_at;
    int full_pass;      /* $OV_VK_FULL_PASS: never scissor the render area */
};

static void vk_log(const char *fmt, ...)
{
    va_list ap;

    if (!getenv("OV_DEBUG"))
        return;
    fprintf(stderr, "knulli-overlay: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static int memory_type(const ov_vk *v, uint32_t bits, VkMemoryPropertyFlags want)
{
    uint32_t i;

    for (i = 0; i < v->mem.memoryTypeCount; i++)
        if ((bits & (1u << i)) &&
            (v->mem.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

/* --- one-off resources ---------------------------------------------------- */

static int make_render_pass_layout(ov_vk *v, VkFormat format,
                                   VkImageLayout layout, VkRenderPass *out)
{
    VkAttachmentDescription att;
    VkAttachmentReference ref;
    VkSubpassDescription sub;
    VkSubpassDependency dep;
    VkRenderPassCreateInfo ci;

    memset(&att, 0, sizeof(att));
    att.format = format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    /* The frame is already finished and in its presentable layout: load it,
     * draw on top, hand it back exactly as it was found. */
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = layout;
    att.finalLayout = layout;

    memset(&ref, 0, sizeof(ref));
    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    memset(&sub, 0, sizeof(sub));
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;

    memset(&dep, 0, sizeof(dep));
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    /* What wrote this image is not ours to know: the application rendered it,
     * or a layer above copied it in with a transfer.  loadOp = LOAD has to see
     * that write, so the dependency covers everything rather than just colour
     * attachment output -- without it the load reads stale tiles and the frame
     * comes back black around whatever we draw. */
    dep.srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;

    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = 1;
    ci.pAttachments = &att;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    ci.dependencyCount = 1;
    ci.pDependencies = &dep;
    return v->fn->CreateRenderPass(v->device, &ci, NULL, out) == VK_SUCCESS;
}

static int make_render_pass(ov_vk *v, VkFormat format)
{
    /* The shadow a layer above hands the application is in the same layout as
     * a swapchain image -- presentable -- so one render pass covers both. */
    return make_render_pass_layout(v, format, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                   &v->pass);
}

static int make_descriptors(ov_vk *v)
{
    VkDescriptorSetLayoutBinding bind[2];
    VkDescriptorSetLayoutCreateInfo li;
    VkDescriptorPoolSize size;
    VkDescriptorPoolCreateInfo pi;
    VkDescriptorSetAllocateInfo ai;
    VkSamplerCreateInfo si;

    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (v->fn->CreateSampler(v->device, &si, NULL, &v->sampler) != VK_SUCCESS)
        return 0;

    memset(bind, 0, sizeof(bind));
    bind[0].binding = 0;            /* glyph atlas */
    bind[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bind[0].descriptorCount = 1;
    bind[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bind[1] = bind[0];
    bind[1].binding = 1;            /* bezel, or the atlas again as a stand-in */

    memset(&li, 0, sizeof(li));
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 2;
    li.pBindings = bind;
    if (v->fn->CreateDescriptorSetLayout(v->device, &li, NULL,
                                         &v->set_layout) != VK_SUCCESS)
        return 0;

    memset(&size, 0, sizeof(size));
    size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    /* One set for the overlay itself, plus one per swapchain image for the
     * rotate at present. */
    size.descriptorCount = 2 * (1 + OV_VK_MAX_IMAGES);

    memset(&pi, 0, sizeof(pi));
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 1 + OV_VK_MAX_IMAGES;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    if (v->fn->CreateDescriptorPool(v->device, &pi, NULL,
                                    &v->desc_pool) != VK_SUCCESS)
        return 0;

    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = v->desc_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &v->set_layout;
    return v->fn->AllocateDescriptorSets(v->device, &ai, &v->set) == VK_SUCCESS;
}

/* Points both bindings at whatever is available: the bezel when it is loaded,
 * the atlas as a stand-in when it is not (the shader never samples it then,
 * but the descriptor still has to be valid). */
static void write_descriptors(ov_vk *v)
{
    VkDescriptorImageInfo info[2];
    VkWriteDescriptorSet write[2];
    int i;

    memset(info, 0, sizeof(info));
    info[0].sampler = v->sampler;
    info[0].imageView = v->atlas_view;
    info[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    info[1] = info[0];
    if (v->image_view)
        info[1].imageView = v->image_view;

    memset(write, 0, sizeof(write));
    for (i = 0; i < 2; i++) {
        write[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write[i].dstSet = v->set;
        write[i].dstBinding = (uint32_t)i;
        write[i].descriptorCount = 1;
        write[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write[i].pImageInfo = &info[i];
    }
    v->fn->UpdateDescriptorSets(v->device, 2, write, 0, NULL);
}

static int make_pipeline(ov_vk *v)
{
    VkShaderModuleCreateInfo smi;
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stage[2];
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vp;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineColorBlendAttachmentState blend;
    VkPipelineColorBlendStateCreateInfo cb;
    VkPipelineDynamicStateCreateInfo dyn;
    VkDynamicState dyn_state[2] = { VK_DYNAMIC_STATE_VIEWPORT,
                                    VK_DYNAMIC_STATE_SCISSOR };
    VkPushConstantRange push;
    VkPipelineLayoutCreateInfo pli;
    VkGraphicsPipelineCreateInfo pci;
    int ok = 0;

    memset(&smi, 0, sizeof(smi));
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = sizeof(ov_vk_vert_spv);
    smi.pCode = ov_vk_vert_spv;
    if (v->fn->CreateShaderModule(v->device, &smi, NULL, &vert) != VK_SUCCESS)
        return 0;
    smi.codeSize = sizeof(ov_vk_frag_spv);
    smi.pCode = ov_vk_frag_spv;
    if (v->fn->CreateShaderModule(v->device, &smi, NULL, &frag) != VK_SUCCESS)
        goto out;

    memset(&push, 0, sizeof(push));
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = sizeof(ov_vk_push);

    memset(&pli, 0, sizeof(pli));
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &v->set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    if (v->fn->CreatePipelineLayout(v->device, &pli, NULL,
                                    &v->layout) != VK_SUCCESS)
        goto out;

    memset(stage, 0, sizeof(stage));
    stage[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stage[0].module = vert;
    stage[0].pName = "main";
    stage[1] = stage[0];
    stage[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stage[1].module = frag;

    memset(&vi, 0, sizeof(vi));     /* the quad comes from gl_VertexIndex */
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    memset(&ia, 0, sizeof(ia));
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    memset(&vp, 0, sizeof(vp));
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = vp.scissorCount = 1;

    memset(&rs, 0, sizeof(rs));
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    memset(&ms, 0, sizeof(ms));
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    memset(&blend, 0, sizeof(blend));
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    memset(&cb, 0, sizeof(cb));
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;

    memset(&dyn, 0, sizeof(dyn));
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_state;

    memset(&pci, 0, sizeof(pci));
    pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pci.stageCount = 2;
    pci.pStages = stage;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &dyn;
    pci.layout = v->layout;
    pci.renderPass = v->pass;
    ok = v->fn->CreateGraphicsPipelines(v->device, VK_NULL_HANDLE, 1, &pci,
                                        NULL, &v->pipeline) == VK_SUCCESS;
out:
    if (vert)
        v->fn->DestroyShaderModule(v->device, vert, NULL);
    if (frag)
        v->fn->DestroyShaderModule(v->device, frag, NULL);
    return ok;
}

static int make_frames(ov_vk *v, VkFormat format, const VkImage *images,
                       uint32_t count)
{
    VkCommandPoolCreateInfo pi;
    VkCommandBufferAllocateInfo ai;
    VkFenceCreateInfo fi;
    VkSemaphoreCreateInfo si;
    uint32_t i;

    memset(&pi, 0, sizeof(pi));
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = v->queue_family;
    if (v->fn->CreateCommandPool(v->device, &pi, NULL,
                                 &v->cmd_pool) != VK_SUCCESS)
        return 0;

    memset(&fi, 0, sizeof(fi));
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    for (i = 0; i < count; i++) {
        VkImageViewCreateInfo vi;
        VkFramebufferCreateInfo fbi;

        memset(&vi, 0, sizeof(vi));
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        if (v->fn->CreateImageView(v->device, &vi, NULL,
                                   &v->frame[i].view) != VK_SUCCESS)
            return 0;
        v->frame[i].image = images[i];

        memset(&fbi, 0, sizeof(fbi));
        fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbi.renderPass = v->pass;
        fbi.attachmentCount = 1;
        fbi.pAttachments = &v->frame[i].view;
        fbi.width = v->extent.width;
        fbi.height = v->extent.height;
        fbi.layers = 1;
        if (v->fn->CreateFramebuffer(v->device, &fbi, NULL,
                                     &v->frame[i].fb) != VK_SUCCESS)
            return 0;

        memset(&ai, 0, sizeof(ai));
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = v->cmd_pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (v->fn->AllocateCommandBuffers(v->device, &ai,
                                          &v->frame[i].cmd) != VK_SUCCESS)
            return 0;
        if (v->fn->CreateFence(v->device, &fi, NULL,
                               &v->frame[i].fence) != VK_SUCCESS)
            return 0;
        if (v->fn->CreateSemaphore(v->device, &si, NULL,
                                   &v->frame[i].sem) != VK_SUCCESS)
            return 0;
    }
    v->count = count;
    return 1;
}

ov_vk *ov_vk_create(VkDevice device, const ov_vk_fns *fn,
                    const VkPhysicalDeviceMemoryProperties *mem,
                    uint32_t queue_family, VkFormat format,
                    VkExtent2D extent, const VkImage *images,
                    uint32_t image_count)
{
    ov_vk *v;

    if (!device || !fn || image_count == 0 || image_count > OV_VK_MAX_IMAGES ||
        extent.width == 0 || extent.height == 0)
        return NULL;
    v = calloc(1, sizeof(*v));
    if (!v)
        return NULL;
    v->device = device;
    v->fn = fn;
    v->mem = *mem;
    v->queue_family = queue_family;
    v->extent = extent;
    v->format = format;

    if (!make_render_pass(v, format) || !make_descriptors(v) ||
        !make_pipeline(v) || !make_frames(v, format, images, image_count)) {
        ov_vk_destroy(v);
        return NULL;
    }
    v->ok = 1;
    return v;
}

void ov_vk_destroy(ov_vk *v)
{
    const ov_vk_fns *fn;
    uint32_t i, si;

    if (!v)
        return;
    fn = v->fn;
    fn->DeviceWaitIdle(v->device);
    for (si = 0; si < OV_VK_MAX_IMAGES; si++)
        if (v->src_view[si])
            fn->DestroyImageView(v->device, v->src_view[si], NULL);
    for (i = 0; i < v->count; i++) {
        if (v->frame[i].sem)
            fn->DestroySemaphore(v->device, v->frame[i].sem, NULL);
        if (v->frame[i].fence)
            fn->DestroyFence(v->device, v->frame[i].fence, NULL);
        if (v->frame[i].fb)
            fn->DestroyFramebuffer(v->device, v->frame[i].fb, NULL);
        if (v->frame[i].view)
            fn->DestroyImageView(v->device, v->frame[i].view, NULL);
    }
    if (v->cmd_pool)
        fn->DestroyCommandPool(v->device, v->cmd_pool, NULL);
    if (v->pipeline)
        fn->DestroyPipeline(v->device, v->pipeline, NULL);
    if (v->layout)
        fn->DestroyPipelineLayout(v->device, v->layout, NULL);
    if (v->desc_pool)
        fn->DestroyDescriptorPool(v->device, v->desc_pool, NULL);
    if (v->set_layout)
        fn->DestroyDescriptorSetLayout(v->device, v->set_layout, NULL);
    if (v->sampler)
        fn->DestroySampler(v->device, v->sampler, NULL);
    if (v->pass)
        fn->DestroyRenderPass(v->device, v->pass, NULL);
    for (i = 0; i < OV_VK_MAX_IMAGES; i++) {
        if (v->aux[i].fb)
            fn->DestroyFramebuffer(v->device, v->aux[i].fb, NULL);
        if (v->aux[i].view)
            fn->DestroyImageView(v->device, v->aux[i].view, NULL);
    }
    if (v->atlas_view)
        fn->DestroyImageView(v->device, v->atlas_view, NULL);
    if (v->atlas)
        fn->DestroyImage(v->device, v->atlas, NULL);
    if (v->atlas_mem)
        fn->FreeMemory(v->device, v->atlas_mem, NULL);
    if (v->image_view)
        fn->DestroyImageView(v->device, v->image_view, NULL);
    if (v->image)
        fn->DestroyImage(v->device, v->image, NULL);
    if (v->image_mem)
        fn->FreeMemory(v->device, v->image_mem, NULL);
    free(v);
}

/* --- textures ------------------------------------------------------------- */

/* Creates an image, fills it from `data` through a staging buffer and leaves it
 * ready to sample.  Uploads are rare -- the atlas once, the bezel when the
 * game changes -- so this waits for the copy rather than tracking it. */
static int upload_texture(ov_vk *v, VkQueue queue, VkFormat format,
                          const void *data, int w, int h, int bpp,
                          VkImage *out_image, VkDeviceMemory *out_mem,
                          VkImageView *out_view)
{
    const ov_vk_fns *fn = v->fn;
    VkDeviceSize size = (VkDeviceSize)w * h * bpp;
    VkImageCreateInfo ii;
    VkBufferCreateInfo bi;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo mi;
    VkCommandBufferAllocateInfo ai;
    VkCommandBufferBeginInfo bbi;
    VkImageMemoryBarrier bar;
    VkBufferImageCopy copy;
    VkSubmitInfo submit;
    VkFenceCreateInfo fi;
    VkImageViewCreateInfo vi;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_mem = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    void *mapped = NULL;
    int type, ok = 0;

    memset(&ii, 0, sizeof(ii));
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent.width = (uint32_t)w;
    ii.extent.height = (uint32_t)h;
    ii.extent.depth = 1;
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (fn->CreateImage(v->device, &ii, NULL, &image) != VK_SUCCESS)
        goto out;

    fn->GetImageMemoryRequirements(v->device, image, &req);
    type = memory_type(v, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0)
        goto out;
    memset(&mi, 0, sizeof(mi));
    mi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mi.allocationSize = req.size;
    mi.memoryTypeIndex = (uint32_t)type;
    if (fn->AllocateMemory(v->device, &mi, NULL, &image_mem) != VK_SUCCESS ||
        fn->BindImageMemory(v->device, image, image_mem, 0) != VK_SUCCESS)
        goto out;

    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (fn->CreateBuffer(v->device, &bi, NULL, &staging) != VK_SUCCESS)
        goto out;
    fn->GetBufferMemoryRequirements(v->device, staging, &req);
    type = memory_type(v, req.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type < 0)
        goto out;
    mi.allocationSize = req.size;
    mi.memoryTypeIndex = (uint32_t)type;
    if (fn->AllocateMemory(v->device, &mi, NULL, &staging_mem) != VK_SUCCESS ||
        fn->BindBufferMemory(v->device, staging, staging_mem, 0) != VK_SUCCESS ||
        fn->MapMemory(v->device, staging_mem, 0, size, 0, &mapped) != VK_SUCCESS)
        goto out;
    memcpy(mapped, data, (size_t)size);
    fn->UnmapMemory(v->device, staging_mem);

    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = v->cmd_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (fn->AllocateCommandBuffers(v->device, &ai, &cmd) != VK_SUCCESS)
        goto out;

    memset(&bbi, 0, sizeof(bbi));
    bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (fn->BeginCommandBuffer(cmd, &bbi) != VK_SUCCESS)
        goto out;

    memset(&bar, 0, sizeof(bar));
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = image;
    bar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bar.subresourceRange.levelCount = bar.subresourceRange.layerCount = 1;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    fn->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                           1, &bar);

    memset(&copy, 0, sizeof(copy));
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = (uint32_t)w;
    copy.imageExtent.height = (uint32_t)h;
    copy.imageExtent.depth = 1;
    fn->CmdCopyBufferToImage(cmd, staging, image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    fn->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL,
                           0, NULL, 1, &bar);
    if (fn->EndCommandBuffer(cmd) != VK_SUCCESS)
        goto out;

    memset(&fi, 0, sizeof(fi));
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (fn->CreateFence(v->device, &fi, NULL, &fence) != VK_SUCCESS)
        goto out;
    memset(&submit, 0, sizeof(submit));
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    if (fn->QueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS)
        goto out;
    fn->WaitForFences(v->device, 1, &fence, VK_TRUE, UINT64_MAX);

    memset(&vi, 0, sizeof(vi));
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = vi.subresourceRange.layerCount = 1;
    if (fn->CreateImageView(v->device, &vi, NULL, out_view) != VK_SUCCESS)
        goto out;

    *out_image = image;
    *out_mem = image_mem;
    ok = 1;
out:
    if (fence)
        fn->DestroyFence(v->device, fence, NULL);
    if (cmd)
        fn->FreeCommandBuffers(v->device, v->cmd_pool, 1, &cmd);
    if (staging)
        fn->DestroyBuffer(v->device, staging, NULL);
    if (staging_mem)
        fn->FreeMemory(v->device, staging_mem, NULL);
    if (!ok) {
        if (image)
            fn->DestroyImage(v->device, image, NULL);
        if (image_mem)
            fn->FreeMemory(v->device, image_mem, NULL);
    }
    return ok;
}

void ov_vk_set_image(ov_vk *v, const void *rgba, int w, int h, unsigned gen)
{
    if (!v)
        return;
    /* Kept as a pointer, not a copy: the bezel that owns these pixels outlives
     * the swapchain, and the upload happens on the next draw, where there is a
     * queue to do it on. */
    v->pending = rgba;
    v->pending_w = w;
    v->pending_h = h;
    v->image_gen = gen;
    if (!rgba)
        v->pending_w = v->pending_h = 0;
}

unsigned ov_vk_image_gen(const ov_vk *v)
{
    return v ? v->image_gen : 0;
}

/* --- drawing -------------------------------------------------------------- */

static int textures_ready(ov_vk *v, VkQueue queue)
{
    if (!v->atlas_ready) {
        if (!upload_texture(v, queue, VK_FORMAT_R8_UNORM, ov_atlas_pixels,
                            OV_ATLAS_W, OV_ATLAS_H, 1, &v->atlas,
                            &v->atlas_mem, &v->atlas_view)) {
            vk_log("vulkan: atlas upload failed, overlay off for this device");
            return 0;
        }
        v->atlas_ready = 1;
        write_descriptors(v);
    }
    if (v->pending) {
        const void *data = v->pending;
        int w = v->pending_w, h = v->pending_h;

        v->pending = NULL;
        /* Nothing may be reading the old descriptor while it is rewritten;
         * this happens once per bezel, not per frame. */
        v->fn->DeviceWaitIdle(v->device);
        if (v->image_view)
            v->fn->DestroyImageView(v->device, v->image_view, NULL);
        if (v->image)
            v->fn->DestroyImage(v->device, v->image, NULL);
        if (v->image_mem)
            v->fn->FreeMemory(v->device, v->image_mem, NULL);
        v->image_view = VK_NULL_HANDLE;
        v->image = VK_NULL_HANDLE;
        v->image_mem = VK_NULL_HANDLE;
        if (w > 0 && h > 0 &&
            !upload_texture(v, queue, VK_FORMAT_R8G8B8A8_UNORM, data, w, h, 4,
                            &v->image, &v->image_mem, &v->image_view))
            vk_log("vulkan: bezel upload failed");
        write_descriptors(v);
    }
    return 1;
}

/* The overlay is a few small widgets in the corners of a big screen, and on a
 * tile-based GPU what a render pass costs is the tiles it touches, not the
 * triangles in it: loadOp = LOAD pulls every tile of the render area in and
 * writes it back.  One pass around everything would span the full width of the
 * panel for a clock on the left and a pill on the right, so the commands are
 * grouped into the boxes they actually occupy and each group gets its own
 * pass. */
struct ov_vk_group {
    VkRect2D area;
    float    x0, y0, x1, y1;
};

/* How many pixels a merge may add before a pass of its own is cheaper.  A pass
 * is not free -- a tiler kicks the whole pipeline for one -- so a merge that
 * adds less than a small band's worth is still the better deal. */
#define OV_VK_MERGE_SLACK (96.0f * 96.0f)

/* The pixels a group would gain by taking this command. */
static float merge_cost(const struct ov_vk_group *g, const ov_cmd *c)
{
    float x0 = g->x0 < c->x ? g->x0 : c->x;
    float y0 = g->y0 < c->y ? g->y0 : c->y;
    float x1 = g->x1 > c->x + c->w ? g->x1 : c->x + c->w;
    float y1 = g->y1 > c->y + c->h ? g->y1 : c->y + c->h;

    return (x1 - x0) * (y1 - y0) - (g->x1 - g->x0) * (g->y1 - g->y0);
}

static void clip_group(const ov_vk *v, struct ov_vk_group *g)
{
    float x0 = g->x0 - 1.0f, y0 = g->y0 - 1.0f;   /* slack for antialiasing */
    float x1 = g->x1 + 1.0f, y1 = g->y1 + 1.0f;

    if (x0 < 0.0f) x0 = 0.0f;
    if (y0 < 0.0f) y0 = 0.0f;
    if (x1 > (float)v->extent.width)  x1 = (float)v->extent.width;
    if (y1 > (float)v->extent.height) y1 = (float)v->extent.height;
    g->area.offset.x = (int32_t)x0;
    g->area.offset.y = (int32_t)y0;
    g->area.extent.width = (uint32_t)(x1 - x0 + 0.5f);
    g->area.extent.height = (uint32_t)(y1 - y0 + 0.5f);
}

/* Groups the commands into the boxes they occupy, so a clock in one corner and
 * a pill in the other do not drag a pass across everything between them, and a
 * bezel's four strips do not add up to the whole screen. */
static int group_commands(const ov_vk *v, const ov_drawlist *dl, int rotation,
                          struct ov_vk_group *group, unsigned char *of,
                          int max_groups)
{
    int count = 0, i, g;

    memset(of, 0, (size_t)dl->count);
    /* The boxes are in the layout's frame, which a rotated display turns, so a
     * rotated screen keeps the whole frame as its area. */
    if ((rotation & 3) || v->full_pass || dl->count == 0) {
        group[0].x0 = group[0].y0 = 0.0f;
        group[0].x1 = (float)v->extent.width;
        group[0].y1 = (float)v->extent.height;
        clip_group(v, &group[0]);
        return 1;
    }

    for (i = 0; i < dl->count; i++) {
        const ov_cmd *c = &dl->cmd[i];
        float best = 0.0f;
        int pick = -1;

        /* Join the group that grows least by taking this box.  Adjacency is
         * not the test -- a bezel's thin side strip touches its top strip, and
         * merging those two would drag the pass down the whole screen -- so
         * what counts is how many pixels the merge adds. */
        for (g = 0; g < count; g++) {
            float grow = merge_cost(&group[g], c);

            if (pick < 0 || grow < best) {
                best = grow;
                pick = g;
            }
        }
        if (pick >= 0 && best <= OV_VK_MERGE_SLACK) {
            g = pick;
        } else if (count < max_groups) {
            g = count++;
            group[g].x0 = c->x;
            group[g].y0 = c->y;
            group[g].x1 = c->x + c->w;
            group[g].y1 = c->y + c->h;
        } else {
            g = pick;               /* out of passes: the cheapest merge wins */
        }
        if (c->x < group[g].x0) group[g].x0 = c->x;
        if (c->y < group[g].y0) group[g].y0 = c->y;
        if (c->x + c->w > group[g].x1) group[g].x1 = c->x + c->w;
        if (c->y + c->h > group[g].y1) group[g].y1 = c->y + c->h;
        of[i] = (unsigned char)g;
    }
    for (g = 0; g < count; g++)
        clip_group(v, &group[g]);
    return count;
}

/* Reads a presented image back to a file, once, when $OV_VK_DUMP names one.
 *
 * On this hardware the framebuffer device is not what the panel scans out for a
 * Vulkan application -- knulli-screenshot can show a perfectly good frame while
 * the screen shows something else -- so this reads the swapchain image itself,
 * which is the thing the display engine reads. */
static void dump_image(ov_vk *v, VkQueue queue, uint32_t index)
{
    const char *path = getenv("OV_VK_DUMP");
    /* $OV_VK_DUMP_SOURCE reads the image the application drew into rather than
     * the one being presented, which is what tells a blit that went wrong from
     * an application that never drew. */
    int want_source = getenv("OV_VK_DUMP_SOURCE") != NULL;
    VkImage dump_src;
    uint32_t dump_w, dump_h;
    const ov_vk_fns *fn = v->fn;
    VkDeviceSize size;
    VkBufferCreateInfo bi;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo mi;
    VkCommandBufferAllocateInfo ai;
    VkCommandBufferBeginInfo bbi;
    VkImageMemoryBarrier bar;
    VkBufferImageCopy copy;
    VkSubmitInfo submit;
    VkFenceCreateInfo fi;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    void *mapped = NULL;
    FILE *f;
    int type;

    if (!path || !*path || v->dumped)
        return;
    /* Not the first frame: at start-up the image has only just been acquired
     * and nothing has been copied into it yet.  $OV_VK_DUMP_FRAME picks which
     * one, counting the frames we have drawn. */
    if (++v->drawn < v->dump_at)
        return;
    v->dumped = 1;

    /* The source is the application's own image, which is its way round; the
     * presented one is the panel's. */
    dump_src = (want_source && index < OV_VK_MAX_IMAGES && v->src_image[index])
               ? v->src_image[index] : v->frame[index].image;
    if (dump_src != v->frame[index].image) {
        dump_w = (v->rot_src & 1) ? v->extent.height : v->extent.width;
        dump_h = (v->rot_src & 1) ? v->extent.width : v->extent.height;
    } else {
        dump_w = v->extent.width;
        dump_h = v->extent.height;
    }
    size = (VkDeviceSize)dump_w * dump_h * 4;

    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (fn->CreateBuffer(v->device, &bi, NULL, &buf) != VK_SUCCESS)
        goto out;
    fn->GetBufferMemoryRequirements(v->device, buf, &req);
    type = memory_type(v, req.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type < 0)
        goto out;
    memset(&mi, 0, sizeof(mi));
    mi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mi.allocationSize = req.size;
    mi.memoryTypeIndex = (uint32_t)type;
    if (fn->AllocateMemory(v->device, &mi, NULL, &mem) != VK_SUCCESS ||
        fn->BindBufferMemory(v->device, buf, mem, 0) != VK_SUCCESS)
        goto out;

    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = v->cmd_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (fn->AllocateCommandBuffers(v->device, &ai, &cmd) != VK_SUCCESS)
        goto out;
    memset(&bbi, 0, sizeof(bbi));
    bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    fn->BeginCommandBuffer(cmd, &bbi);

    memset(&bar, 0, sizeof(bar));
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bar.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = dump_src;
    bar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bar.subresourceRange.levelCount = bar.subresourceRange.layerCount = 1;
    fn->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                           1, &bar);

    memset(&copy, 0, sizeof(copy));
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = dump_w;
    copy.imageExtent.height = dump_h;
    copy.imageExtent.depth = 1;
    fn->CmdCopyImageToBuffer(cmd, dump_src,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1,
                             &copy);

    bar.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bar.dstAccessMask = 0;
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    fn->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL,
                           0, NULL, 1, &bar);
    fn->EndCommandBuffer(cmd);

    memset(&fi, 0, sizeof(fi));
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (fn->CreateFence(v->device, &fi, NULL, &fence) != VK_SUCCESS)
        goto out;
    memset(&submit, 0, sizeof(submit));
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    if (fn->QueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS)
        goto out;
    fn->WaitForFences(v->device, 1, &fence, VK_TRUE, UINT64_MAX);

    if (fn->MapMemory(v->device, mem, 0, size, 0, &mapped) == VK_SUCCESS) {
        f = fopen(path, "wb");
        if (f) {
            fwrite(mapped, 1, (size_t)size, f);
            fclose(f);
            vk_log("vulkan: wrote %s (%ux%u, 4 bytes per pixel, %s image)",
                   path, dump_w, dump_h,
                   dump_src == v->frame[index].image ? "presented"
                                                     : "application's own");
        }
        fn->UnmapMemory(v->device, mem);
    }
out:
    if (fence)
        fn->DestroyFence(v->device, fence, NULL);
    if (cmd)
        fn->FreeCommandBuffers(v->device, v->cmd_pool, 1, &cmd);
    if (buf)
        fn->DestroyBuffer(v->device, buf, NULL);
    if (mem)
        fn->FreeMemory(v->device, mem, NULL);
}

int ov_vk_image_index(const ov_vk *v, VkImage image)
{
    uint32_t i;

    if (!v || image == VK_NULL_HANDLE)
        return -1;
    for (i = 0; i < v->count; i++)
        if (v->frame[i].image == image)
            return (int)i;
    return -1;
}

/* The framebuffer for an image we did not make: one view and one framebuffer
 * per image, kept for as long as the swapchain lives. */
static VkFramebuffer aux_framebuffer(ov_vk *v, VkImage image)
{
    VkImageViewCreateInfo vi;
    VkFramebufferCreateInfo fbi;
    int i, slot = -1;

    for (i = 0; i < OV_VK_MAX_IMAGES; i++) {
        if (v->aux[i].image == image)
            return v->aux[i].fb;
        if (!v->aux[i].image && slot < 0)
            slot = i;
    }
    if (slot < 0)
        return VK_NULL_HANDLE;

    memset(&vi, 0, sizeof(vi));
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = v->format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = vi.subresourceRange.layerCount = 1;
    if (v->fn->CreateImageView(v->device, &vi, NULL,
                               &v->aux[slot].view) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    memset(&fbi, 0, sizeof(fbi));
    fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbi.renderPass = v->pass;
    fbi.attachmentCount = 1;
    fbi.pAttachments = &v->aux[slot].view;
    fbi.width = v->extent.width;
    fbi.height = v->extent.height;
    fbi.layers = 1;
    if (v->fn->CreateFramebuffer(v->device, &fbi, NULL,
                                 &v->aux[slot].fb) != VK_SUCCESS) {
        v->fn->DestroyImageView(v->device, v->aux[slot].view, NULL);
        v->aux[slot].view = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    v->aux[slot].image = image;
    vk_log("vulkan: drawing into a shadow image, %ux%u", v->extent.width,
           v->extent.height);
    return v->aux[slot].fb;
}

/* Gives the shader a way to sample one of the application's images, so that
 * present can turn it onto the real swapchain.  Done once per image, when the
 * swapchain is created. */
int ov_vk_set_source(ov_vk *v, uint32_t index, VkImage image)
{
    VkImageViewCreateInfo vi;
    VkDescriptorSetAllocateInfo ai;
    VkDescriptorImageInfo info[2];
    VkWriteDescriptorSet write[2];
    int i;

    if (!v || !v->ok || index >= OV_VK_MAX_IMAGES)
        return 0;
    if (v->src_set[index])
        return 1;

    memset(&vi, 0, sizeof(vi));
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = v->format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    if (v->fn->CreateImageView(v->device, &vi, NULL, &v->src_view[index])
            != VK_SUCCESS)
        return 0;

    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = v->desc_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &v->set_layout;
    if (v->fn->AllocateDescriptorSets(v->device, &ai, &v->src_set[index])
            != VK_SUCCESS) {
        v->src_set[index] = VK_NULL_HANDLE;
        return 0;
    }

    memset(info, 0, sizeof(info));
    info[0].sampler = v->sampler;
    info[0].imageView = v->atlas_view;      /* binding 0 is never sampled here */
    info[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    info[1] = info[0];
    info[1].imageView = v->src_view[index];

    memset(write, 0, sizeof(write));
    for (i = 0; i < 2; i++) {
        write[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write[i].dstSet = v->src_set[index];
        write[i].dstBinding = (uint32_t)i;
        write[i].descriptorCount = 1;
        write[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write[i].pImageInfo = &info[i];
    }
    v->fn->UpdateDescriptorSets(v->device, 2, write, 0, NULL);
    v->src_image[index] = image;
    return 1;
}

/* The drawing itself, into whichever framebuffer and render pass the caller
 * has picked; shared by the two ways the overlay reaches the screen. */
static void record_passes(ov_vk *v, VkCommandBuffer cmd, VkRenderPass pass,
                          VkFramebuffer fb, const ov_drawlist *dl,
                          int rotation, VkImageMemoryBarrier *between,
                          VkDescriptorSet source)
{
    struct ov_vk_group group[OV_VK_MAX_GROUPS];
    unsigned char group_of[OV_MAX_CMDS];
    const ov_vk_fns *fn = v->fn;
    VkRenderPassBeginInfo rp;
    VkViewport viewport;
    VkRect2D scissor;
    ov_vk_push push;
    float screen_w, screen_h;
    int groups, g;
    uint32_t i;

    /* The layout works in screen space, so a rotated display swapped the two
     * before laying out and the shader turns the result back. */
    screen_w = (float)((rotation & 1) ? v->extent.height : v->extent.width);
    screen_h = (float)((rotation & 1) ? v->extent.width : v->extent.height);

    groups = group_commands(v, dl, rotation, group, group_of,
                            OV_VK_MAX_GROUPS);

    /* Turning the application's frame onto the panel covers everything, so the
     * per-widget grouping that keeps a tiler cheap has nothing left to save:
     * one pass over the whole screen, with the source drawn under the overlay. */
    if (source) {
        groups = 1;
        group[0].area.offset.x = 0;
        group[0].area.offset.y = 0;
        group[0].area.extent = v->extent;
        for (i = 0; i < (uint32_t)dl->count; i++)
            group_of[i] = 0;
    }

    memset(&viewport, 0, sizeof(viewport));
    viewport.width = (float)v->extent.width;
    viewport.height = (float)v->extent.height;
    viewport.maxDepth = 1.0f;

    memset(&rp, 0, sizeof(rp));
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = pass;
    rp.framebuffer = fb;

    for (g = 0; g < groups; g++) {
        if (g > 0 && between)
            fn->CmdPipelineBarrier(cmd,
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   0, 0, NULL, 0, NULL, 1, between);
        rp.renderArea = group[g].area;
        scissor = rp.renderArea;
        fn->CmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
        fn->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, v->pipeline);
        fn->CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  v->layout, 0, 1, &v->set, 0, NULL);
        fn->CmdSetViewport(cmd, 0, 1, &viewport);
        fn->CmdSetScissor(cmd, 0, 1, &scissor);

        /* The application's frame, turned to match the panel.  Screen space is
         * already the application's way round, so this is simply the whole of
         * it; the shader's rot does the turning. */
        if (source) {
            fn->CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      v->layout, 0, 1, &source, 0, NULL);
            memset(&push, 0, sizeof(push));
            push.rect[2] = screen_w;
            push.rect[3] = screen_h;
            push.uv[2] = 1.0f;
            push.uv[3] = 1.0f;
            push.screen[0] = screen_w;
            push.screen[1] = screen_h;
            push.fill[0] = push.fill[1] = push.fill[2] = push.fill[3] = 1.0f;
            push.mode = 2;
            push.rot = rotation;
            fn->CmdPushConstants(cmd, v->layout,
                                 VK_SHADER_STAGE_VERTEX_BIT
                                 | VK_SHADER_STAGE_FRAGMENT_BIT,
                                 0, sizeof(push), &push);
            fn->CmdDraw(cmd, 4, 1, 0, 0);
            fn->CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      v->layout, 0, 1, &v->set, 0, NULL);
        }

        for (i = 0; i < (uint32_t)dl->count; i++) {
            const ov_cmd *c = &dl->cmd[i];

            if (group_of[i] != g)
                continue;
            if (c->image && !v->image_view)
                continue;           /* nothing uploaded yet */
            memset(&push, 0, sizeof(push));
            push.rect[0] = c->x;
            push.rect[1] = c->y;
            push.rect[2] = c->w;
            push.rect[3] = c->h;
            if (c->image) {
                memcpy(push.uv, c->uv, sizeof(push.uv));
                push.mode = 2;
            } else if (c->glyph >= 0) {
                const ov_glyph *gy = &ov_atlas_glyphs[c->glyph];

                push.uv[0] = (float)gy->x / OV_ATLAS_W;
                push.uv[1] = (float)gy->y / OV_ATLAS_H;
                push.uv[2] = (float)gy->w / OV_ATLAS_W;
                push.uv[3] = (float)gy->h / OV_ATLAS_H;
                push.mode = 1;
            }
            push.screen[0] = screen_w;
            push.screen[1] = screen_h;
            push.half_size[0] = c->w * 0.5f;
            push.half_size[1] = c->h * 0.5f;
            memcpy(push.fill, c->fill, sizeof(push.fill));
            memcpy(push.stroke, c->stroke, sizeof(push.stroke));
            push.radius = c->radius;
            push.border = c->border;
            push.rot = rotation & 3;
            fn->CmdPushConstants(cmd, v->layout,
                                 VK_SHADER_STAGE_VERTEX_BIT |
                                 VK_SHADER_STAGE_FRAGMENT_BIT,
                                 0, sizeof(push), &push);
            fn->CmdDraw(cmd, 4, 1, 0, 0);
        }
        fn->CmdEndRenderPass(cmd);
    }

    if (!v->logged) {
        v->logged = 1;
        vk_log("vulkan: %d commands in %d pass(es), first %ux%u at %d,%d of "
               "%ux%u", dl->count, groups, group[0].area.extent.width,
               group[0].area.extent.height, group[0].area.offset.x,
               group[0].area.offset.y, v->extent.width, v->extent.height);
    }
}

VkSemaphore ov_vk_draw(ov_vk *v, VkQueue queue, uint32_t index, VkImage target,
                       const ov_drawlist *dl, int rotation,
                       const VkSemaphore *wait, uint32_t wait_count,
                       int blit_source)
{
    VkDescriptorSet source = VK_NULL_HANDLE;
    VkFramebuffer fb;
    VkImage image;
    VkPipelineStageFlags stages[OV_VK_MAX_WAIT];
    VkCommandBufferBeginInfo bi;
    VkSubmitInfo submit;
    VkImageMemoryBarrier barrier;
    const ov_vk_fns *fn;
    VkCommandBuffer cmd;
    uint32_t i;
    VkResult r;

    if (v && blit_source && index < OV_VK_MAX_IMAGES) {
        source = v->src_set[index];
        v->rot_src = rotation;
    }
    if (!v || !v->ok || !dl || (dl->count == 0 && !source)
        || index >= v->count) {
        vk_log("vulkan: nothing to submit (ok %d, commands %d, index %u/%u)",
               v ? v->ok : -1, dl ? dl->count : -1, index, v ? v->count : 0);
        return VK_NULL_HANDLE;
    }
    fn = v->fn;
    if (!textures_ready(v, queue)) {
        v->ok = 0;
        return VK_NULL_HANDLE;
    }
    if (target == VK_NULL_HANDLE || target == v->frame[index].image) {
        image = v->frame[index].image;
        fb = v->frame[index].fb;
    } else {
        image = target;
        fb = aux_framebuffer(v, target);
        if (!fb)
            return VK_NULL_HANDLE;
    }
    if (wait_count > OV_VK_MAX_WAIT) {
        vk_log("vulkan: %u wait semaphores, more than we pass on", wait_count);
        return VK_NULL_HANDLE;      /* stay out rather than drop one */
    }

    if (v->frame[index].submitted &&
        fn->WaitForFences(v->device, 1, &v->frame[index].fence, VK_TRUE,
                          UINT64_MAX) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    fn->ResetFences(v->device, 1, &v->frame[index].fence);
    cmd = v->frame[index].cmd;
    fn->ResetCommandBuffer(cmd, 0);

    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (fn->BeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
        vk_log("vulkan: vkBeginCommandBuffer failed");
        return VK_NULL_HANDLE;
    }

    /* Belt as well as braces: the same visibility the subpass dependency asks
     * for, spelled out as a barrier on the image itself.  The layout does not
     * change -- the frame is presentable before and after. */
    memset(&barrier, 0, sizeof(barrier));
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    fn->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                           0, NULL, 0, NULL, 1, &barrier);

    /* The application finished with its own image by handing it to present,
     * so it is in PRESENT_SRC_KHR; sampling it needs it read-only, and the
     * application gets it back the way it left it. */
    if (source) {
        VkImageMemoryBarrier src_bar = barrier;

        src_bar.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        src_bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        src_bar.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        src_bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        src_bar.image = v->src_image[index];
        fn->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                               0, NULL, 0, NULL, 1, &src_bar);
    }

    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    record_passes(v, cmd, v->pass, fb, dl, rotation, &barrier, source);

    if (source) {
        VkImageMemoryBarrier src_bar = barrier;

        src_bar.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        src_bar.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        src_bar.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        src_bar.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        src_bar.image = v->src_image[index];
        fn->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                               0, NULL, 0, NULL, 1, &src_bar);
    }

    if (fn->EndCommandBuffer(cmd) != VK_SUCCESS) {
        vk_log("vulkan: vkEndCommandBuffer failed");
        return VK_NULL_HANDLE;
    }

    for (i = 0; i < wait_count; i++)
        stages[i] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    memset(&submit, 0, sizeof(submit));
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = wait_count;
    submit.pWaitSemaphores = wait_count ? wait : NULL;
    submit.pWaitDstStageMask = wait_count ? stages : NULL;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &v->frame[index].sem;
    r = fn->QueueSubmit(queue, 1, &submit, v->frame[index].fence);
    if (r != VK_SUCCESS) {
        vk_log("vulkan: vkQueueSubmit failed (%d)", (int)r);
        return VK_NULL_HANDLE;
    }
    v->frame[index].submitted = 1;
    if (getenv("OV_VK_DUMP") && !v->dumped) {
        fn->WaitForFences(v->device, 1, &v->frame[index].fence, VK_TRUE,
                          UINT64_MAX);
        dump_image(v, queue, index);
    }
    return v->frame[index].sem;
}
