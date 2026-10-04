// Vulkan 렌더러(오프스크린). 인스턴스·장치·렌더 패스·파이프라인을 한 번 만들고, 그릴 때마다 대상 이미지와 정점 버퍼를 만든다.
// 렌더 타깃: 색(RGBA8) + ID(R32_UINT) + 깊이(D32). ID 버퍼는 픽킹과 마우스 오버 강조에 쓴다(RND-48).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

#include "ofep/error.hpp"
#include "ofep/render.hpp"

namespace ofep {

namespace {

const std::uint32_t kVertSpv[] = {
#include "mesh.vert.inc"
};
const std::uint32_t kFragSpv[] = {
#include "mesh.frag.inc"
};

const VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
const VkFormat kIdFormat = VK_FORMAT_R32_UINT;
const VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

void check(VkResult r, const char* what) {
  if (r != VK_SUCCESS)
    throw Error("render_failed", std::string("Vulkan 오류(") + what + "): " + std::to_string(static_cast<int>(r)), {{"vk_result", static_cast<int>(r)}});
}

}  // namespace

struct Renderer::Impl {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice gpu = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  std::uint32_t queue_family = 0;
  VkQueue queue = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkRenderPass pass = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;  // 클리핑 평면 유니폼(set 0, binding 0)
  VkDescriptorPool desc_pool = VK_NULL_HANDLE;
  VkPipeline tri_pipeline = VK_NULL_HANDLE, line_pipeline = VK_NULL_HANDLE, blend_pipeline = VK_NULL_HANDLE;
  VkPipeline id_pipeline = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties props{};
  VkPhysicalDeviceMemoryProperties mem{};
  bool wide_lines = false;
  Json last = Json::object();
  VkDeviceSize last_gpu_bytes = 0;      // 마지막 프레임의 GPU 할당(오프스크린은 프레임마다 만들고 해제한다)
  VkDeviceSize persistent_gpu_bytes = 0;  // 창 스왑체인의 깊이·ID 이미지처럼 남아 있는 할당
  VkDeviceSize device_local_heap() const {
    VkDeviceSize total = 0;
    for (std::uint32_t i = 0; i < mem.memoryHeapCount; ++i)
      if (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) total += mem.memoryHeaps[i].size;
    return total;
  }
  bool surface_support = false, swapchain_support = false;

  ~Impl() {
    if (device) {
      vkDeviceWaitIdle(device);
      if (tri_pipeline) vkDestroyPipeline(device, tri_pipeline, nullptr);
      if (line_pipeline) vkDestroyPipeline(device, line_pipeline, nullptr);
      if (blend_pipeline) vkDestroyPipeline(device, blend_pipeline, nullptr);
      if (id_pipeline) vkDestroyPipeline(device, id_pipeline, nullptr);
      if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
      if (desc_pool) vkDestroyDescriptorPool(device, desc_pool, nullptr);
      if (set_layout) vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
      if (pass) vkDestroyRenderPass(device, pass, nullptr);
      if (pool) vkDestroyCommandPool(device, pool, nullptr);
      vkDestroyDevice(device, nullptr);
    }
    if (instance) vkDestroyInstance(instance, nullptr);
  }

  std::uint32_t memory_type(std::uint32_t bits, VkMemoryPropertyFlags want) const {
    for (std::uint32_t i = 0; i < mem.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & want) == want) return i;
    throw Error("render_failed", "맞는 GPU 메모리 종류가 없습니다");
  }

  void init() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "open-fep";
    app.apiVersion = VK_API_VERSION_1_1;
    // 창에 그리기 위한 확장(있을 때만)
    std::uint32_t ext_n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &ext_n, nullptr);
    std::vector<VkExtensionProperties> exts(ext_n);
    vkEnumerateInstanceExtensionProperties(nullptr, &ext_n, exts.data());
    std::vector<const char*> enabled;
    for (const char* want : {"VK_KHR_surface", "VK_KHR_win32_surface"})
      for (const VkExtensionProperties& e : exts)
        if (std::strcmp(e.extensionName, want) == 0) enabled.push_back(want);
    surface_support = enabled.size() == 2;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<std::uint32_t>(enabled.size()), ici.ppEnabledExtensionNames = enabled.data();
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS)
      throw Error("render_unavailable", "Vulkan 을 쓸 수 없습니다(드라이버가 없거나 지원하지 않는 장치)");

    // GPU 선택: 그래픽 큐가 있는 장치 가운데 외장 GPU 를 먼저 고른다(RND-01)
    std::uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> gpus(n);
    vkEnumeratePhysicalDevices(instance, &n, gpus.data());
    int best_score = -1;
    for (VkPhysicalDevice d : gpus) {
      std::uint32_t qn = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
      std::vector<VkQueueFamilyProperties> q(qn);
      vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, q.data());
      for (std::uint32_t i = 0; i < qn; ++i) {
        if (!(q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        const int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (score > best_score) best_score = score, gpu = d, queue_family = i, props = p;
        break;
      }
    }
    if (!gpu) throw Error("render_unavailable", "그래픽을 그릴 수 있는 GPU 가 없습니다");
    vkGetPhysicalDeviceMemoryProperties(gpu, &mem);

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family, qci.queueCount = 1, qci.pQueuePriorities = &priority;
    std::uint32_t dext_n = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &dext_n, nullptr);
    std::vector<VkExtensionProperties> dexts(dext_n);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &dext_n, dexts.data());
    std::vector<const char*> denabled;
    for (const VkExtensionProperties& e : dexts)
      if (std::strcmp(e.extensionName, "VK_KHR_swapchain") == 0) denabled.push_back("VK_KHR_swapchain");
    swapchain_support = surface_support && !denabled.empty();
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkPhysicalDeviceFeatures supported{}, features{};
    vkGetPhysicalDeviceFeatures(gpu, &supported);
    features.wideLines = supported.wideLines;
    // 두 색 타깃의 쓰기 마스크가 다르다(선은 ID를 덮지 않음).
    features.independentBlend = supported.independentBlend;
    wide_lines = features.wideLines && props.limits.lineWidthRange[1] >= 2.0f;
    dci.pEnabledFeatures = &features;
    dci.queueCreateInfoCount = 1, dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<std::uint32_t>(denabled.size()), dci.ppEnabledExtensionNames = denabled.data();
    check(vkCreateDevice(gpu, &dci, nullptr, &device), "vkCreateDevice");
    vkGetDeviceQueue(device, queue_family, 0, &queue);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = queue_family;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    check(vkCreateCommandPool(device, &pci, nullptr, &pool), "vkCreateCommandPool");

    pass = make_pass(kColorFormat, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    // 클리핑 평면 유니폼 버퍼(RND-19): 프레임마다 버퍼 둘(장면용·화면 고정 요소용 빈 것)과 디스크립터 셋 둘을 만든다
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sli.bindingCount = 1, sli.pBindings = &binding;
    check(vkCreateDescriptorSetLayout(device, &sli, nullptr, &set_layout), "vkCreateDescriptorSetLayout");
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, dpi.maxSets = 64, dpi.poolSizeCount = 1, dpi.pPoolSizes = &pool_size;
    check(vkCreateDescriptorPool(device, &dpi, nullptr, &desc_pool), "vkCreateDescriptorPool");

    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, 128};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1, pli.pSetLayouts = &set_layout;
    pli.pushConstantRangeCount = 1, pli.pPushConstantRanges = &range;
    check(vkCreatePipelineLayout(device, &pli, nullptr, &layout), "vkCreatePipelineLayout");

    tri_pipeline = make_pipeline(pass, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true);
    line_pipeline = make_pipeline(pass, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, false);
    blend_pipeline = make_pipeline(pass, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, true);
    id_pipeline = make_pipeline(pass, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true, false, true);
  }

  // 렌더 패스: 색, ID, 깊이. 그린 뒤 ID 는 읽어 낼 수 있는 상태로, 색은 지정한 상태(읽기 또는 화면 표시)로 둔다.
  VkRenderPass make_pass(VkFormat color_format, VkImageLayout color_final) {
    VkAttachmentDescription att[3]{};
    for (int i = 0; i < 3; ++i) {
      att[i].samples = VK_SAMPLE_COUNT_1_BIT;
      att[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      att[i].storeOp = i < 2 ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
      att[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      att[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      att[i].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      att[i].finalLayout = i < 2 ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    att[0].format = color_format, att[1].format = kIdFormat, att[2].format = kDepthFormat;
    att[0].finalLayout = color_final;
    const VkAttachmentReference color_refs[2] = {{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
    const VkAttachmentReference depth_ref = {2, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 2, sub.pColorAttachments = color_refs, sub.pDepthStencilAttachment = &depth_ref;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    // 화면에 낼 때는 그리기가 끝난 뒤 표시가 시작되게 의존을 둔다
    VkSubpassDependency dep[2]{};
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL, dep[0].dstSubpass = 0;
    dep[0].srcStageMask = dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[1].srcSubpass = 0, dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    rpi.attachmentCount = 3, rpi.pAttachments = att, rpi.subpassCount = 1, rpi.pSubpasses = &sub;
    rpi.dependencyCount = 2, rpi.pDependencies = dep;
    VkRenderPass out;
    check(vkCreateRenderPass(device, &rpi, nullptr, &out), "vkCreateRenderPass");
    return out;
  }

  VkShaderModule shader(const std::uint32_t* code, std::size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes, ci.pCode = code;
    VkShaderModule m;
    check(vkCreateShaderModule(device, &ci, nullptr, &m), "vkCreateShaderModule");
    return m;
  }

  // blend: 투명 면용 — 색을 섞고 깊이는 읽기만 한다(ID 버퍼에도 쓰지 않는다)
  VkPipeline make_pipeline(VkRenderPass target_pass, VkPrimitiveTopology topology, bool write_id, bool blend = false, bool id_only = false) {
    const VkShaderModule vs = shader(kVertSpv, sizeof kVertSpv), fs = shader(kFragSpv, sizeof kFragSpv);
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT, stages[0].module = vs, stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, stages[1].module = fs, stages[1].pName = "main";

    const VkVertexInputBindingDescription binding{0, sizeof(RenderVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attrs[4] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(RenderVertex, pos)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(RenderVertex, normal)},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(RenderVertex, color)},
        {3, 0, VK_FORMAT_R32_UINT, offsetof(RenderVertex, id)},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1, vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 4, vi.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = topology;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1, vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;  // 쉘의 뒷면도 그린다(색으로 구분)
    // 바깥에서 볼 때 반시계 방향으로 감긴 면이 앞면이다. 투영 행렬이 y 를 뒤집어 화면의 위쪽을 맞추므로
    // (Vulkan 은 y 가 아래) 감기는 방향은 그대로 남는다 — 그림으로 확인했다(뒷면 색이 섞이지 않는다).
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    // 면 기울기에 따른 픽셀 안의 깊이 차이를 보정하여 같은 면의 에지가 가려지지 않게 한다.
    rs.depthBiasEnable = !id_only && topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    rs.depthBiasConstantFactor = 1.0f;
    rs.depthBiasSlopeFactor = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE, ds.depthWriteEnable = blend || topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST ? VK_FALSE : VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState att[2]{};
    att[0].colorWriteMask = id_only ? 0 : 0xF;
    if (blend) {
      att[0].blendEnable = VK_TRUE;
      att[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, att[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      att[0].colorBlendOp = VK_BLEND_OP_ADD;
      att[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, att[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, att[0].alphaBlendOp = VK_BLEND_OP_ADD;
    }
    att[1].colorWriteMask = write_id ? 0xF : 0;  // 선·투명 면은 ID 버퍼에 쓰지 않는다(그 아래 면의 ID 가 남는다)
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 2, cb.pAttachments = att;
    const VkDynamicState dyn[4] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 4, dy.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.stageCount = 2, ci.pStages = stages;
    ci.pVertexInputState = &vi, ci.pInputAssemblyState = &ia, ci.pViewportState = &vp, ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms, ci.pDepthStencilState = &ds, ci.pColorBlendState = &cb, ci.pDynamicState = &dy;
    ci.layout = layout, ci.renderPass = target_pass;
    VkPipeline p;
    const VkResult r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &p);
    vkDestroyShaderModule(device, vs, nullptr);
    vkDestroyShaderModule(device, fs, nullptr);
    check(r, "vkCreateGraphicsPipelines");
    return p;
  }

  // --- 그릴 때마다 만드는 자원(범위를 벗어나면 해제)
  struct Frame {
    VkDevice device;
    VkDeviceSize bytes = 0;  // 이 프레임이 GPU 에 할당한 바이트(이미지·버퍼)
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkBuffer> buffers;
    std::vector<VkDeviceMemory> memory;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> sets;
    ~Frame() {
      if (!sets.empty()) vkFreeDescriptorSets(device, desc_pool, static_cast<std::uint32_t>(sets.size()), sets.data());
      if (fence) vkDestroyFence(device, fence, nullptr);
      if (cmd) vkFreeCommandBuffers(device, pool, 1, &cmd);
      if (fb) vkDestroyFramebuffer(device, fb, nullptr);
      for (VkImageView v : views) vkDestroyImageView(device, v, nullptr);
      for (VkImage i : images) vkDestroyImage(device, i, nullptr);
      for (VkBuffer b : buffers) vkDestroyBuffer(device, b, nullptr);
      for (VkDeviceMemory m : memory) vkFreeMemory(device, m, nullptr);
    }
  };

  VkImageView image(Frame& f, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, int w, int h, VkImage* out) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D, ci.format = format;
    ci.extent = {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 1};
    ci.mipLevels = 1, ci.arrayLayers = 1, ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL, ci.usage = usage;
    VkImage img;
    check(vkCreateImage(device, &ci, nullptr, &img), "vkCreateImage");
    f.images.push_back(img);
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, img, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size, ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory m;
    check(vkAllocateMemory(device, &ai, nullptr, &m), "vkAllocateMemory");
    f.memory.push_back(m);
    f.bytes += req.size;
    check(vkBindImageMemory(device, img, m, 0), "vkBindImageMemory");
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img, vi.viewType = VK_IMAGE_VIEW_TYPE_2D, vi.format = format;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    VkImageView view;
    check(vkCreateImageView(device, &vi, nullptr, &view), "vkCreateImageView");
    f.views.push_back(view);
    if (out) *out = img;
    return view;
  }

  VkBuffer buffer(Frame& f, VkDeviceSize size, VkBufferUsageFlags usage, VkDeviceMemory* out_mem) {
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size ? size : 4, ci.usage = usage;
    VkBuffer b;
    check(vkCreateBuffer(device, &ci, nullptr, &b), "vkCreateBuffer");
    f.buffers.push_back(b);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory m;
    check(vkAllocateMemory(device, &ai, nullptr, &m), "vkAllocateMemory");
    f.memory.push_back(m);
    f.bytes += req.size;
    check(vkBindBufferMemory(device, b, m, 0), "vkBindBufferMemory");
    *out_mem = m;
    return b;
  }

  // 장면의 정점을 버퍼 하나에 올린다: 면 → 선 → 강조 → 화면 고정 면 → 화면 고정 선.
  struct Uploaded {
    VkBuffer buffer = VK_NULL_HANDLE;
    std::uint32_t tri = 0, line = 0, transparent = 0, overlay = 0, hud_tri = 0, hud_line = 0;
    VkDescriptorSet clip_set = VK_NULL_HANDLE, none_set = VK_NULL_HANDLE;  // 클리핑 평면 / 없음(화면 고정 요소)
  };
  // 클리핑 평면 유니폼(std140: vec4[8] + ivec4) 하나를 만들어 디스크립터 셋에 잇는다
  VkDescriptorSet clip_uniform(Frame& f, const std::vector<std::array<float, 4>>& planes) {
    float data[36] = {};
    const int n = std::min<int>(static_cast<int>(planes.size()), kMaxClipPlanes);
    for (int i = 0; i < n; ++i)
      for (int k = 0; k < 4; ++k) data[i * 4 + k] = planes[static_cast<std::size_t>(i)][static_cast<std::size_t>(k)];
    std::memcpy(data + 32, &n, sizeof n);
    VkDeviceMemory m;
    const VkBuffer b = buffer(f, sizeof data, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &m);
    void* p = nullptr;
    check(vkMapMemory(device, m, 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory");
    std::memcpy(p, data, sizeof data);
    vkUnmapMemory(device, m);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = desc_pool, ai.descriptorSetCount = 1, ai.pSetLayouts = &set_layout;
    VkDescriptorSet set;
    check(vkAllocateDescriptorSets(device, &ai, &set), "vkAllocateDescriptorSets");
    f.desc_pool = desc_pool, f.sets.push_back(set);
    VkDescriptorBufferInfo bi{b, 0, sizeof data};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set, w.dstBinding = 0, w.descriptorCount = 1, w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, w.pBufferInfo = &bi;
    vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
    return set;
  }
  Uploaded upload(Frame& f, const RenderScene& scene) {
    Uploaded u;
    u.clip_set = clip_uniform(f, scene.clip_planes);
    u.none_set = clip_uniform(f, {});
    const std::vector<RenderVertex>* parts[6] = {&scene.triangles, &scene.lines, &scene.transparent, &scene.overlay, &scene.hud_triangles,
                                                 &scene.hud_lines};
    std::uint32_t* counts[6] = {&u.tri, &u.line, &u.transparent, &u.overlay, &u.hud_tri, &u.hud_line};
    std::size_t total = 0;
    for (int k = 0; k < 6; ++k) *counts[k] = static_cast<std::uint32_t>(parts[k]->size()), total += parts[k]->size();
    VkDeviceMemory vmem;
    u.buffer = buffer(f, total * sizeof(RenderVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vmem);
    if (total) {
      void* p = nullptr;
      check(vkMapMemory(device, vmem, 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory");
      char* dst = static_cast<char*>(p);
      for (int k = 0; k < 6; ++k) {
        if (parts[k]->empty()) continue;
        std::memcpy(dst, parts[k]->data(), parts[k]->size() * sizeof(RenderVertex));
        dst += parts[k]->size() * sizeof(RenderVertex);
      }
      vkUnmapMemory(device, vmem);
    }
    return u;
  }
  // 그리기 명령: 3D 장면 뒤에 화면 고정 요소를 단위 변환으로 그린다(깊이는 0 에 가까워 맨 위에 온다).
  void draw(VkCommandBuffer cmd, const Uploaded& u, VkPipeline tri, VkPipeline line, VkPipeline blend, VkPipeline ids,
            const float mvp[16], const float view[16], std::uint32_t width, std::uint32_t height) {
    float push[32];
    std::memcpy(push, mvp, 64), std::memcpy(push + 16, view, 64);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 128, push);
    const VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &u.buffer, &zero);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &u.clip_set, 0, nullptr);
    std::uint32_t first = 0;
    vkCmdSetDepthBias(cmd, 1.0f, 0.0f, 1.0f);
    if (u.tri) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tri), vkCmdDraw(cmd, u.tri, 1, first, 0);
    first += u.tri;
    if (u.line) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, line), vkCmdDraw(cmd, u.line, 1, first, 0);
    first += u.line;
    if (u.tri) {
      // 색의 에지 보정이 픽킹 면을 바꾸지 않게 원래 깊이로 ID를 다시 그린다.
      // 깊이도 복원하므로 이후 투명 면은 보정 전의 실제 불투명 면에 가려진다.
      VkClearAttachment clear[2]{};
      clear[0].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, clear[0].clearValue.depthStencil.depth = 1.0f;
      clear[1].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, clear[1].colorAttachment = 1;
      const VkClearRect rect{{{0, 0}, {width, height}}, 0, 1};
      vkCmdClearAttachments(cmd, 2, clear, 1, &rect);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ids);
      vkCmdDraw(cmd, u.tri, 1, 0, 0);
    }
    vkCmdSetDepthBias(cmd, 0.0f, 0.0f, 0.0f);  // 투명·강조·HUD는 원래 깊이로 표시한다.
    if (u.transparent) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blend), vkCmdDraw(cmd, u.transparent, 1, first, 0);
    first += u.transparent;
    if (u.overlay) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tri), vkCmdDraw(cmd, u.overlay, 1, first, 0);
    first += u.overlay;
    if (u.hud_tri + u.hud_line) {
      static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
      std::memcpy(push, identity, 64), std::memcpy(push + 16, identity, 64);
      vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 128, push);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &u.none_set, 0, nullptr);  // 화면 고정 요소는 자르지 않는다
      if (u.hud_tri) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tri), vkCmdDraw(cmd, u.hud_tri, 1, first, 0);
      first += u.hud_tri;
      if (u.hud_line) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, line), vkCmdDraw(cmd, u.hud_line, 1, first, 0);
    }
  }

  RenderImage render(const RenderScene& scene, const float mvp[16], const float view[16], int w, int h) {
    const std::uint32_t limit = props.limits.maxImageDimension2D;
    if (w < 1 || h < 1 || static_cast<std::uint32_t>(w) > limit || static_cast<std::uint32_t>(h) > limit)
      throw Error("out_of_range", "이미지 크기가 범위를 벗어났습니다(1 ~ " + std::to_string(limit) + ")", {{"param", "width"}});
    Frame f{device};
    f.pool = pool;
    VkImage color_img, id_img;
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const VkImageView views[3] = {
        image(f, kColorFormat, usage, VK_IMAGE_ASPECT_COLOR_BIT, w, h, &color_img),
        image(f, kIdFormat, usage, VK_IMAGE_ASPECT_COLOR_BIT, w, h, &id_img),
        image(f, kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, w, h, nullptr),
    };
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = pass, fci.attachmentCount = 3, fci.pAttachments = views;
    fci.width = static_cast<std::uint32_t>(w), fci.height = static_cast<std::uint32_t>(h), fci.layers = 1;
    check(vkCreateFramebuffer(device, &fci, nullptr, &f.fb), "vkCreateFramebuffer");

    const Uploaded up = upload(f, scene);
    VkDeviceMemory cmem, imem;
    const VkDeviceSize pixels = static_cast<VkDeviceSize>(w) * static_cast<VkDeviceSize>(h);
    const VkBuffer cbuf = buffer(f, pixels * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &cmem);
    const VkBuffer ibuf = buffer(f, pixels * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &imem);

    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool, cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, cai.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(device, &cai, &f.cmd), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(f.cmd, &bi), "vkBeginCommandBuffer");

    VkClearValue clear[3]{};
    for (int k = 0; k < 4; ++k) clear[0].color.float32[k] = scene.background[static_cast<std::size_t>(k)];
    clear[1].color.uint32[0] = 0;
    clear[2].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = pass, rbi.framebuffer = f.fb;
    rbi.renderArea = {{0, 0}, {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)}};
    rbi.clearValueCount = 3, rbi.pClearValues = clear;
    vkCmdBeginRenderPass(f.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0, 0, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
    vkCmdSetViewport(f.cmd, 0, 1, &viewport);
    vkCmdSetScissor(f.cmd, 0, 1, &rbi.renderArea);
    vkCmdSetLineWidth(f.cmd, wide_lines ? static_cast<float>(scene.pixel_scale) : 1.0f);
    draw(f.cmd, up, tri_pipeline, line_pipeline, blend_pipeline, id_pipeline, mvp, view, w, h);
    vkCmdEndRenderPass(f.cmd);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 1};
    vkCmdCopyImageToBuffer(f.cmd, color_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, cbuf, 1, &region);
    vkCmdCopyImageToBuffer(f.cmd, id_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ibuf, 1, &region);
    check(vkEndCommandBuffer(f.cmd), "vkEndCommandBuffer");

    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(vkCreateFence(device, &fi, nullptr, &f.fence), "vkCreateFence");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1, si.pCommandBuffers = &f.cmd;
    check(vkQueueSubmit(queue, 1, &si, f.fence), "vkQueueSubmit");
    check(vkWaitForFences(device, 1, &f.fence, VK_TRUE, 60ull * 1000 * 1000 * 1000), "vkWaitForFences");

    RenderImage out;
    out.width = w, out.height = h;
    out.rgba.resize(static_cast<std::size_t>(pixels) * 4);
    out.ids.resize(static_cast<std::size_t>(pixels));
    void* p = nullptr;
    check(vkMapMemory(device, cmem, 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory");
    std::memcpy(out.rgba.data(), p, out.rgba.size());
    vkUnmapMemory(device, cmem);
    check(vkMapMemory(device, imem, 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory");
    std::memcpy(out.ids.data(), p, out.ids.size() * 4);
    vkUnmapMemory(device, imem);
    last = stats(up, w, h);
    last["gpu_bytes"] = static_cast<std::uint64_t>(f.bytes);
    last_gpu_bytes = f.bytes;
    return out;
  }
  static Json stats(const Uploaded& u, int w, int h) {
    const std::size_t total = u.tri + u.line + u.transparent + u.overlay + u.hud_tri + u.hud_line;
    return Json{{"width", w}, {"height", h}, {"triangles", u.tri / 3}, {"lines", u.line / 2}, {"transparent", u.transparent / 3},
                {"hud_triangles", u.hud_tri / 3}, {"hud_lines", u.hud_line / 2},
                {"draws", (u.tri ? 2 : 0) + (u.line ? 1 : 0) + (u.transparent ? 1 : 0) + (u.overlay ? 1 : 0) + (u.hud_tri ? 1 : 0) + (u.hud_line ? 1 : 0)},
                {"vertex_bytes", total * sizeof(RenderVertex)}};
  }
  // ------------------------------------------------------------ 창에 그리기(스왑체인, RND-02)
  struct Window {
    Impl& r;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D extent{0, 0};
    VkExtent2D render_extent{0, 0};
    int pixel_scale = 1;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkPipeline tri = VK_NULL_HANDLE, line = VK_NULL_HANDLE, blend = VK_NULL_HANDLE, ids = VK_NULL_HANDLE;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    VkImage depth = VK_NULL_HANDLE, id = VK_NULL_HANDLE, color = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE, id_view = VK_NULL_HANDLE, color_view = VK_NULL_HANDLE;
    VkDeviceMemory depth_mem = VK_NULL_HANDLE, id_mem = VK_NULL_HANDLE, color_mem = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE, finished = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    bool rendered = false;  // ID 이미지에 마지막 프레임이 들어 있다
    std::uint64_t frames = 0;

    explicit Window(Impl& impl) : r(impl) {}
    ~Window() {
      vkDeviceWaitIdle(r.device);
      destroy_swapchain();
      if (acquired) vkDestroySemaphore(r.device, acquired, nullptr);
      if (finished) vkDestroySemaphore(r.device, finished, nullptr);
      if (cmd) vkFreeCommandBuffers(r.device, r.pool, 1, &cmd);
      if (tri) vkDestroyPipeline(r.device, tri, nullptr);
      if (line) vkDestroyPipeline(r.device, line, nullptr);
      if (blend) vkDestroyPipeline(r.device, blend, nullptr);
      if (ids) vkDestroyPipeline(r.device, ids, nullptr);
      if (pass) vkDestroyRenderPass(r.device, pass, nullptr);
      if (surface) vkDestroySurfaceKHR(r.instance, surface, nullptr);
    }

    VkDeviceSize persistent_bytes = 0;
    void destroy_swapchain() {
      r.persistent_gpu_bytes -= std::min(r.persistent_gpu_bytes, persistent_bytes), persistent_bytes = 0;
      for (VkFramebuffer f : framebuffers) vkDestroyFramebuffer(r.device, f, nullptr);
      for (VkImageView v : views) vkDestroyImageView(r.device, v, nullptr);
      framebuffers.clear(), views.clear(), images.clear();
      for (VkImageView* v : {&depth_view, &id_view, &color_view})
        if (*v) vkDestroyImageView(r.device, *v, nullptr), *v = VK_NULL_HANDLE;
      for (VkImage* i : {&depth, &id, &color})
        if (*i) vkDestroyImage(r.device, *i, nullptr), *i = VK_NULL_HANDLE;
      for (VkDeviceMemory* m : {&depth_mem, &id_mem, &color_mem})
        if (*m) vkFreeMemory(r.device, *m, nullptr), *m = VK_NULL_HANDLE;
      if (swapchain) vkDestroySwapchainKHR(r.device, swapchain, nullptr), swapchain = VK_NULL_HANDLE;
      rendered = false;
    }

    void attach(void* native) {
#ifdef _WIN32
      VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
      sci.hinstance = GetModuleHandleW(nullptr), sci.hwnd = static_cast<HWND>(native);
      check(vkCreateWin32SurfaceKHR(r.instance, &sci, nullptr, &surface), "vkCreateWin32SurfaceKHR");
#else
      (void)native;
      throw Error("render_unavailable", "이 플랫폼의 창 연동은 아직 없습니다");
#endif
      VkBool32 ok = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(r.gpu, r.queue_family, surface, &ok);
      if (!ok) throw Error("render_unavailable", "이 GPU 큐는 창에 그릴 수 없습니다");
      std::uint32_t n = 0;
      vkGetPhysicalDeviceSurfaceFormatsKHR(r.gpu, surface, &n, nullptr);
      std::vector<VkSurfaceFormatKHR> formats(n);
      vkGetPhysicalDeviceSurfaceFormatsKHR(r.gpu, surface, &n, formats.data());
      format = formats.empty() || formats[0].format == VK_FORMAT_UNDEFINED ? VK_FORMAT_B8G8R8A8_UNORM : formats[0].format;
      for (const VkSurfaceFormatKHR& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) format = f.format;
      // 화면이 바뀔 때만 출력하므로 MAILBOX에서도 무제한 렌더링하지 않는다.
      // 지원 장치에서는 오래된 프레임의 표시 순서를 기다리는 지연을 줄인다.
      check(vkGetPhysicalDeviceSurfacePresentModesKHR(r.gpu, surface, &n, nullptr), "vkGetPhysicalDeviceSurfacePresentModesKHR");
      std::vector<VkPresentModeKHR> modes(n);
      check(vkGetPhysicalDeviceSurfacePresentModesKHR(r.gpu, surface, &n, modes.data()), "vkGetPhysicalDeviceSurfacePresentModesKHR");
      if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end()) present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
      VkFormatProperties format_props{};
      vkGetPhysicalDeviceFormatProperties(r.gpu, format, &format_props);
      const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                           VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
      if ((format_props.optimalTilingFeatures & required) != required)
        throw Error("render_unavailable", "이 화면 형식은 GPU 이미지 축소를 지원하지 않습니다");
      pass = r.make_pass(format, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      tri = r.make_pipeline(pass, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true);
      line = r.make_pipeline(pass, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, false);
      blend = r.make_pipeline(pass, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, true);
      ids = r.make_pipeline(pass, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true, false, true);
      VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      check(vkCreateSemaphore(r.device, &si, nullptr, &acquired), "vkCreateSemaphore");
      check(vkCreateSemaphore(r.device, &si, nullptr, &finished), "vkCreateSemaphore");
      VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      cai.commandPool = r.pool, cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, cai.commandBufferCount = 1;
      check(vkAllocateCommandBuffers(r.device, &cai, &cmd), "vkAllocateCommandBuffers");
    }

    VkImage make_image(VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkDeviceMemory& mem_out, VkImageView& view_out) {
      VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      ci.imageType = VK_IMAGE_TYPE_2D, ci.format = fmt, ci.extent = {render_extent.width, render_extent.height, 1};
      ci.mipLevels = 1, ci.arrayLayers = 1, ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.tiling = VK_IMAGE_TILING_OPTIMAL, ci.usage = usage;
      VkImage img;
      check(vkCreateImage(r.device, &ci, nullptr, &img), "vkCreateImage");
      VkMemoryRequirements req;
      vkGetImageMemoryRequirements(r.device, img, &req);
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      ai.allocationSize = req.size, ai.memoryTypeIndex = r.memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      check(vkAllocateMemory(r.device, &ai, nullptr, &mem_out), "vkAllocateMemory");
      r.persistent_gpu_bytes += req.size, persistent_bytes += req.size;
      check(vkBindImageMemory(r.device, img, mem_out, 0), "vkBindImageMemory");
      VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      vi.image = img, vi.viewType = VK_IMAGE_VIEW_TYPE_2D, vi.format = fmt, vi.subresourceRange = {aspect, 0, 1, 0, 1};
      check(vkCreateImageView(r.device, &vi, nullptr, &view_out), "vkCreateImageView");
      return img;
    }

    // 창 크기에 맞춰 스왑체인을 (다시) 만든다. 크기가 0 이면(최소화) false.
    bool recreate() {
      vkDeviceWaitIdle(r.device);
      destroy_swapchain();
      VkSurfaceCapabilitiesKHR caps;
      check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r.gpu, surface, &caps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
      extent = caps.currentExtent;
      if (extent.width == 0 || extent.height == 0 || extent.width == 0xFFFFFFFFu) return false;
      render_extent = {extent.width * pixel_scale, extent.height * pixel_scale};
      if (render_extent.width > r.props.limits.maxImageDimension2D || render_extent.height > r.props.limits.maxImageDimension2D ||
          render_extent.width > r.props.limits.maxFramebufferWidth || render_extent.height > r.props.limits.maxFramebufferHeight)
        throw Error("out_of_range", "안티앨리어싱 렌더 타깃이 GPU 최대 해상도를 넘습니다");
      if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        throw Error("render_unavailable", "이 창은 GPU 이미지 전송을 지원하지 않습니다");
      std::uint32_t count = caps.minImageCount + 1;
      if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
      VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
      sci.surface = surface, sci.minImageCount = count, sci.imageFormat = format;
      sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, sci.imageExtent = extent, sci.imageArrayLayers = 1;
      sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
      sci.preTransform = caps.currentTransform, sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
      sci.presentMode = present_mode, sci.clipped = VK_TRUE;
      check(vkCreateSwapchainKHR(r.device, &sci, nullptr, &swapchain), "vkCreateSwapchainKHR");
      std::uint32_t n = 0;
      vkGetSwapchainImagesKHR(r.device, swapchain, &n, nullptr);
      images.resize(n);
      vkGetSwapchainImagesKHR(r.device, swapchain, &n, images.data());
      depth = make_image(kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, depth_mem, depth_view);
      id = make_image(kIdFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, id_mem, id_view);
      color = make_image(format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, color_mem, color_view);
      for (VkImage img : images) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img, vi.viewType = VK_IMAGE_VIEW_TYPE_2D, vi.format = format, vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view;
        check(vkCreateImageView(r.device, &vi, nullptr, &view), "vkCreateImageView");
        views.push_back(view);
        const VkImageView atts[3] = {color_view, id_view, depth_view};
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = pass, fci.attachmentCount = 3, fci.pAttachments = atts;
        fci.width = render_extent.width, fci.height = render_extent.height, fci.layers = 1;
        VkFramebuffer fb;
        check(vkCreateFramebuffer(r.device, &fci, nullptr, &fb), "vkCreateFramebuffer");
        framebuffers.push_back(fb);
      }
      return true;
    }

    bool prepare(int scale) {
      VkSurfaceCapabilitiesKHR caps{};
      check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r.gpu, surface, &caps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
      if (!caps.currentExtent.width || !caps.currentExtent.height || caps.currentExtent.width == 0xFFFFFFFFu) return false;
      if (!swapchain || pixel_scale != scale || caps.currentExtent.width != extent.width || caps.currentExtent.height != extent.height) {
        pixel_scale = scale;
        return recreate();
      }
      return true;
    }

    // 한 프레임을 그려 화면에 낸다. 창 크기를 돌려준다(0 이면 그리지 않음).
    std::array<int, 2> present(const RenderScene& scene, const float mvp[16], const float view[16]) {
      if (!prepare(scene.pixel_scale)) return {0, 0};
      std::uint32_t index = 0;
      VkResult res = vkAcquireNextImageKHR(r.device, swapchain, 1000000000ull, acquired, VK_NULL_HANDLE, &index);
      if (res == VK_ERROR_OUT_OF_DATE_KHR) {
        if (!recreate()) return {0, 0};
        res = vkAcquireNextImageKHR(r.device, swapchain, 1000000000ull, acquired, VK_NULL_HANDLE, &index);
      }
      if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) check(res, "vkAcquireNextImageKHR");

      Frame f{r.device};  // 정점 버퍼는 프레임마다 만든다(그린 뒤 큐를 기다리므로 바로 지워도 된다)
      f.pool = r.pool;
      const Uploaded up = r.upload(f, scene);
      check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");
      VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      check(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");
      VkClearValue clear[3]{};
      for (int k = 0; k < 4; ++k) clear[0].color.float32[k] = scene.background[static_cast<std::size_t>(k)];
      clear[2].depthStencil = {1.0f, 0};
      VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
      rbi.renderPass = pass, rbi.framebuffer = framebuffers[index], rbi.renderArea = {{0, 0}, render_extent};
      rbi.clearValueCount = 3, rbi.pClearValues = clear;
      vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
      const VkViewport viewport{0, 0, static_cast<float>(render_extent.width), static_cast<float>(render_extent.height), 0.0f, 1.0f};
      vkCmdSetViewport(cmd, 0, 1, &viewport);
      vkCmdSetScissor(cmd, 0, 1, &rbi.renderArea);
      vkCmdSetLineWidth(cmd, r.wide_lines ? static_cast<float>(pixel_scale) : 1.0f);
      r.draw(cmd, up, tri, line, blend, ids, mvp, view, render_extent.width, render_extent.height);
      vkCmdEndRenderPass(cmd);
      // 색만 선형 축소한다. 정수 ID는 보간하지 않고 픽킹 때 원래 해상도에서 표본을 읽는다.
      VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = images[index];
      barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      // acquire 세마포어를 기다리는 단계와 레이아웃 전환의 출발 단계를 연결한다.
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
      VkImageBlit region{};
      region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.srcOffsets[1] = {static_cast<int>(render_extent.width), static_cast<int>(render_extent.height), 1};
      region.dstOffsets[1] = {static_cast<int>(extent.width), static_cast<int>(extent.height), 1};
      vkCmdBlitImage(cmd, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     1, &region, pixel_scale > 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
      barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, barrier.dstAccessMask = 0;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
      check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

      const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
      VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      si.waitSemaphoreCount = 1, si.pWaitSemaphores = &acquired, si.pWaitDstStageMask = &wait_stage;
      si.commandBufferCount = 1, si.pCommandBuffers = &cmd, si.signalSemaphoreCount = 1, si.pSignalSemaphores = &finished;
      check(vkQueueSubmit(r.queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit");
      VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
      pi.waitSemaphoreCount = 1, pi.pWaitSemaphores = &finished, pi.swapchainCount = 1, pi.pSwapchains = &swapchain, pi.pImageIndices = &index;
      res = vkQueuePresentKHR(r.queue, &pi);
      vkQueueWaitIdle(r.queue);  // 지금은 프레임마다 기다린다(자원을 바로 지우기 위해). 파이프라이닝은 뒤에
      if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
        const std::array<int, 2> size{static_cast<int>(extent.width), static_cast<int>(extent.height)};
        recreate();
        return size;
      }
      check(res, "vkQueuePresentKHR");
      rendered = true, ++frames;
      r.last = Impl::stats(up, static_cast<int>(extent.width), static_cast<int>(extent.height));
      r.last["frames"] = frames;
      r.last["antialiasing"] = pixel_scale > 1 ? "ssaa2" : "none";
      r.last["render_width"] = render_extent.width, r.last["render_height"] = render_extent.height;
      r.last["gpu_bytes"] = static_cast<std::uint64_t>(f.bytes);
      r.last_gpu_bytes = f.bytes;
      return {static_cast<int>(extent.width), static_cast<int>(extent.height)};
    }

    // 마지막 프레임의 ID 버퍼에서 한 픽셀을 읽는다.
    std::uint32_t pick(int x, int y) {
      if (!rendered || x < 0 || y < 0 || static_cast<std::uint32_t>(x) >= extent.width || static_cast<std::uint32_t>(y) >= extent.height) return 0;
      Frame f{r.device};
      f.pool = r.pool;
      VkDeviceMemory pixel_mem;
      const VkBuffer buf = r.buffer(f, 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &pixel_mem);
      VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      cai.commandPool = r.pool, cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, cai.commandBufferCount = 1;
      check(vkAllocateCommandBuffers(r.device, &cai, &f.cmd), "vkAllocateCommandBuffers");
      VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      check(vkBeginCommandBuffer(f.cmd, &bi), "vkBeginCommandBuffer");
      VkBufferImageCopy region{};
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageOffset = {x * pixel_scale, y * pixel_scale, 0}, region.imageExtent = {1, 1, 1};
      vkCmdCopyImageToBuffer(f.cmd, id, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
      check(vkEndCommandBuffer(f.cmd), "vkEndCommandBuffer");
      VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      si.commandBufferCount = 1, si.pCommandBuffers = &f.cmd;
      check(vkQueueSubmit(r.queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit");
      vkQueueWaitIdle(r.queue);
      std::uint32_t value = 0;
      void* p = nullptr;
      check(vkMapMemory(r.device, pixel_mem, 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory");
      std::memcpy(&value, p, 4);
      vkUnmapMemory(r.device, pixel_mem);
      return value;
    }
  };
  std::unique_ptr<Window> window;
};

Renderer::Renderer() : impl_(new Impl) { impl_->init(); }
Renderer::~Renderer() = default;

void Renderer::attach_window(void* native_handle) {
  if (!impl_->swapchain_support) throw Error("render_unavailable", "이 장치는 창에 그릴 수 없습니다(스왑체인 확장 없음)");
  impl_->window.reset();
  auto w = std::make_unique<Impl::Window>(*impl_);
  w->attach(native_handle);
  impl_->window = std::move(w);
}
void Renderer::detach_window() { impl_->window.reset(); }
bool Renderer::has_window() const { return impl_->window != nullptr; }
std::array<int, 2> Renderer::present(const RenderScene& scene, const float mvp[16], const float view[16]) {
  if (!impl_->window) throw Error("invalid_state", "창이 붙어 있지 않습니다");
  return impl_->window->present(scene, mvp, view);
}
std::array<int, 2> Renderer::window_size() const {
  if (!impl_->window) return {0, 0};
  return {static_cast<int>(impl_->window->extent.width), static_cast<int>(impl_->window->extent.height)};
}
std::array<int, 2> Renderer::prepare_window(int pixel_scale) {
  if (!impl_->window) throw Error("invalid_state", "창이 붙어 있지 않습니다");
  if (!impl_->window->prepare(pixel_scale)) return {0, 0};
  return window_size();
}
std::uint32_t Renderer::pick_window(int x, int y) { return impl_->window ? impl_->window->pick(x, y) : 0; }
RenderImage Renderer::render(const RenderScene& scene, const float mvp[16], const float view[16], int width, int height) {
  return impl_->render(scene, mvp, view, width, height);
}

Json Renderer::info() const {
  const auto& p = impl_->props;
  static const char* types[] = {"other", "integrated", "discrete", "virtual", "cpu"};
  return Json{{"gpu", p.deviceName},
              {"gpu_type", types[p.deviceType <= 4 ? p.deviceType : 0]},
              {"api", std::to_string(VK_VERSION_MAJOR(p.apiVersion)) + "." + std::to_string(VK_VERSION_MINOR(p.apiVersion)) + "." +
                          std::to_string(VK_VERSION_PATCH(p.apiVersion))},
              {"max_image_size", p.limits.maxImageDimension2D},
              {"gpu_memory", Json{{"device_local_heap", static_cast<std::uint64_t>(impl_->device_local_heap())},
                                  {"last_frame", static_cast<std::uint64_t>(impl_->last_gpu_bytes)},
                                  {"persistent", static_cast<std::uint64_t>(impl_->persistent_gpu_bytes)}}},
              {"last_frame", impl_->last}};
}

}  // namespace ofep
