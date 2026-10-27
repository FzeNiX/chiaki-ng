#include "rtxvsr.h"

#include <QLoggingCategory>

Q_DECLARE_LOGGING_CATEGORY(chiakiGui);

#if defined(CHIAKI_HAVE_RTX_VSR)

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>

#include <vulkan/vulkan.h>
#include <nvsdk_ngx_vk.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>

namespace {

// RTX Video SDK exposes Video Super Resolution as this NGX feature slot; the
// public NGX headers only name it "Reserved16". Its runtime is nvngx_vsr.dll,
// shipped with the RTX Video SDK and looked up next to chiaki.exe.
constexpr NVSDK_NGX_Feature kVsrFeature = NVSDK_NGX_Feature_Reserved16;
// Arbitrary but stable project id; NGX only uses it to tell applications apart.
constexpr const char *kNgxProjectId = "5c9a8e0e-3c0b-4c1f-9b55-6b1f6c0a7c21";
constexpr VkImageLayout kVsrLayout = VK_IMAGE_LAYOUT_GENERAL;
// Upscaled frames are cached per source frame so the frame mixer can reuse them.
constexpr size_t kMaxCachedFrames = 8;
constexpr size_t kCommandSlots = 3;
constexpr uint64_t kWaitTimeoutNs = 2000000000ull;
constexpr int kMaxScale = 4;

const char *ngxResultString(NVSDK_NGX_Result result)
{
    switch (result) {
    case NVSDK_NGX_Result_Success: return "Success";
    case NVSDK_NGX_Result_FAIL_FeatureNotSupported: return "FeatureNotSupported";
    case NVSDK_NGX_Result_FAIL_PlatformError: return "PlatformError";
    case NVSDK_NGX_Result_FAIL_FeatureAlreadyExists: return "FeatureAlreadyExists";
    case NVSDK_NGX_Result_FAIL_FeatureNotFound: return "FeatureNotFound (is nvngx_vsr.dll next to chiaki.exe?)";
    case NVSDK_NGX_Result_FAIL_InvalidParameter: return "InvalidParameter";
    case NVSDK_NGX_Result_FAIL_NotInitialized: return "NotInitialized";
    case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat: return "UnsupportedInputFormat";
    case NVSDK_NGX_Result_FAIL_OutOfDate: return "OutOfDate (update the NVIDIA driver)";
    case NVSDK_NGX_Result_FAIL_OutOfGPUMemory: return "OutOfGPUMemory";
    case NVSDK_NGX_Result_FAIL_UnsupportedFormat: return "UnsupportedFormat";
    case NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath: return "UnableToWriteToAppDataPath";
    case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature: return "UnableToInitializeFeature";
    default: return "Fail";
    }
}

} // namespace

struct RtxVsr::Impl
{
    struct Image
    {
        pl_tex tex = nullptr;
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        int w = 0;
        int h = 0;
    };

    struct CacheEntry
    {
        Image image;
        uint64_t key = 0;
        uint64_t last_use = 0;
        uint64_t done_value = 0;
    };

    struct CommandSlot
    {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        uint64_t done_value = 0;
    };

    pl_log log = nullptr;
    pl_vulkan vk = nullptr;
    pl_gpu gpu = nullptr;
    pl_renderer renderer = nullptr;
    pl_fmt fmt = nullptr;
    bool initialized = false;
    bool init_failed = false;
    bool ngx_initialized = false;
    bool hdr_warned = false;
    int eval_failures = 0;

    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    std::array<CommandSlot, kCommandSlots> cmd_slots = {};
    size_t next_slot = 0;

    VkSemaphore sem_in = VK_NULL_HANDLE;
    VkSemaphore sem_out = VK_NULL_HANDLE;
    VkSemaphore sem_done = VK_NULL_HANDLE;
    uint64_t in_value = 0;
    uint64_t out_value = 0;
    uint64_t done_value = 0;

    Image input;
    std::vector<CacheEntry> cache;
    uint64_t use_counter = 0;

    NVSDK_NGX_Parameter *params = nullptr;
    NVSDK_NGX_Handle *feature = nullptr;
    std::wstring app_data_path;
    std::wstring feature_path;

    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = nullptr;
    PFN_vkGetDeviceQueue vkGetDeviceQueue = nullptr;
    PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool vkDestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
    PFN_vkResetCommandBuffer vkResetCommandBuffer = nullptr;
    PFN_vkQueueSubmit vkQueueSubmit = nullptr;
    PFN_vkWaitSemaphores vkWaitSemaphores = nullptr;
    PFN_vkCreateImageView vkCreateImageView = nullptr;
    PFN_vkDestroyImageView vkDestroyImageView = nullptr;

    bool loadFunctions();
    bool init(pl_vulkan vk);
    bool initNgx(VkCommandBuffer cmd);
    void shutdown();
    bool waitDone(uint64_t value);
    bool submit(VkCommandBuffer cmd, bool wait_images);
    bool ensureImage(Image &img, int w, int h);
    void destroyImage(Image &img);
    CacheEntry &acquireCacheEntry(uint64_t key);
    static NVSDK_NGX_Resource_VK makeResource(const Image &img, bool read_write);
    bool evaluate(Image &out, int in_w, int in_h, int quality);
};

bool RtxVsr::Impl::loadFunctions()
{
    vkGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        vk->get_proc_addr(vk->instance, "vkGetDeviceProcAddr"));
    if (!vkGetDeviceProcAddr)
        return false;
#define LOAD_DEVICE_PROC(name_) \
    name_ = reinterpret_cast<PFN_##name_>(vkGetDeviceProcAddr(vk->device, #name_)); \
    if (!name_) { \
        qCWarning(chiakiGui) << "[rtx-vsr] Missing Vulkan function" << #name_; \
        return false; \
    }
    LOAD_DEVICE_PROC(vkGetDeviceQueue)
    LOAD_DEVICE_PROC(vkCreateCommandPool)
    LOAD_DEVICE_PROC(vkDestroyCommandPool)
    LOAD_DEVICE_PROC(vkAllocateCommandBuffers)
    LOAD_DEVICE_PROC(vkBeginCommandBuffer)
    LOAD_DEVICE_PROC(vkEndCommandBuffer)
    LOAD_DEVICE_PROC(vkResetCommandBuffer)
    LOAD_DEVICE_PROC(vkQueueSubmit)
    LOAD_DEVICE_PROC(vkWaitSemaphores)
    LOAD_DEVICE_PROC(vkCreateImageView)
    LOAD_DEVICE_PROC(vkDestroyImageView)
#undef LOAD_DEVICE_PROC
    return true;
}

bool RtxVsr::Impl::init(pl_vulkan vk_)
{
    vk = vk_;
    gpu = vk->gpu;
    if (!loadFunctions())
        return false;

    fmt = pl_find_named_fmt(gpu, "rgba8");
    const enum pl_fmt_caps needed_caps = static_cast<enum pl_fmt_caps>(
        PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_STORABLE);
    if (!fmt || (fmt->caps & needed_caps) != needed_caps) {
        qCWarning(chiakiGui) << "[rtx-vsr] rgba8 format is not renderable/storable on this device";
        return false;
    }

    queue_family = vk->queue_graphics.index;
    vkGetDeviceQueue(vk->device, queue_family, 0, &queue);
    if (!queue)
        return false;

    VkCommandPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_family;
    if (vkCreateCommandPool(vk->device, &pool_info, nullptr, &pool) != VK_SUCCESS)
        return false;

    std::array<VkCommandBuffer, kCommandSlots> cmds = {};
    VkCommandBufferAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = static_cast<uint32_t>(cmds.size());
    if (vkAllocateCommandBuffers(vk->device, &alloc_info, cmds.data()) != VK_SUCCESS)
        return false;
    for (size_t i = 0; i < cmds.size(); i++)
        cmd_slots[i].cmd = cmds[i];

    struct pl_vulkan_sem_params sem_params = {};
    sem_params.type = VK_SEMAPHORE_TYPE_TIMELINE;
    sem_in = pl_vulkan_sem_create(gpu, &sem_params);
    sem_out = pl_vulkan_sem_create(gpu, &sem_params);
    sem_done = pl_vulkan_sem_create(gpu, &sem_params);
    if (!sem_in || !sem_out || !sem_done)
        return false;

    renderer = pl_renderer_create(log, gpu);
    if (!renderer)
        return false;

    // Feature DLLs (nvngx_vsr.dll) are searched next to the executable.
    feature_path = QDir::toNativeSeparators(QCoreApplication::applicationDirPath()).toStdWString();
    QString data_dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (data_dir.isEmpty())
        data_dir = QDir::tempPath();
    QDir().mkpath(data_dir);
    app_data_path = QDir::toNativeSeparators(data_dir).toStdWString();

    VkCommandBuffer cmd = cmd_slots[0].cmd;
    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &begin_info) != VK_SUCCESS)
        return false;
    const bool ngx_ok = initNgx(cmd);
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS || !ngx_ok)
        return false;
    if (!submit(cmd, false))
        return false;
    cmd_slots[0].done_value = done_value;
    if (!waitDone(done_value))
        return false;
    next_slot = 1;
    return true;
}

bool RtxVsr::Impl::initNgx(VkCommandBuffer cmd)
{
    const wchar_t *paths[] = { feature_path.c_str() };
    NVSDK_NGX_FeatureCommonInfo common_info = {};
    common_info.PathListInfo.Path = paths;
    common_info.PathListInfo.Length = 1;

    NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        kNgxProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", app_data_path.c_str(),
        vk->instance, vk->phys_device, vk->device,
        vk->get_proc_addr, vkGetDeviceProcAddr, &common_info, NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(result)) {
        qCWarning(chiakiGui).nospace() << "[rtx-vsr] NGX init failed: " << ngxResultString(result)
            << " (0x" << Qt::hex << static_cast<unsigned>(result) << ")";
        return false;
    }
    ngx_initialized = true;

    result = NVSDK_NGX_VULKAN_AllocateParameters(&params);
    if (NVSDK_NGX_FAILED(result) || !params) {
        qCWarning(chiakiGui) << "[rtx-vsr] NGX parameter allocation failed:" << ngxResultString(result);
        params = nullptr;
        return false;
    }
    NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_CreationNodeMask, 1);
    NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_VisibilityNodeMask, 1);

    result = NVSDK_NGX_VULKAN_CreateFeature1(vk->device, cmd, kVsrFeature, params, &feature);
    if (NVSDK_NGX_FAILED(result) || !feature) {
        qCWarning(chiakiGui).nospace() << "[rtx-vsr] Creating the VSR feature failed: " << ngxResultString(result)
            << " (0x" << Qt::hex << static_cast<unsigned>(result) << ")";
        feature = nullptr;
        return false;
    }
    return true;
}

bool RtxVsr::Impl::waitDone(uint64_t value)
{
    if (!sem_done || value == 0)
        return true;
    VkSemaphoreWaitInfo wait_info = {};
    wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wait_info.semaphoreCount = 1;
    wait_info.pSemaphores = &sem_done;
    wait_info.pValues = &value;
    const VkResult res = vkWaitSemaphores(vk->device, &wait_info, kWaitTimeoutNs);
    if (res != VK_SUCCESS) {
        qCWarning(chiakiGui) << "[rtx-vsr] Waiting for VSR work failed, result" << static_cast<int>(res);
        return false;
    }
    return true;
}

bool RtxVsr::Impl::submit(VkCommandBuffer cmd, bool wait_images)
{
    const VkSemaphore wait_sems[] = { sem_in, sem_out };
    const uint64_t wait_values[] = { in_value, out_value };
    const VkPipelineStageFlags wait_stages[] = {
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
    };
    const uint64_t signal_value = done_value + 1;

    VkTimelineSemaphoreSubmitInfo timeline_info = {};
    timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timeline_info.waitSemaphoreValueCount = wait_images ? 2 : 0;
    timeline_info.pWaitSemaphoreValues = wait_values;
    timeline_info.signalSemaphoreValueCount = 1;
    timeline_info.pSignalSemaphoreValues = &signal_value;

    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.pNext = &timeline_info;
    submit_info.waitSemaphoreCount = wait_images ? 2 : 0;
    submit_info.pWaitSemaphores = wait_sems;
    submit_info.pWaitDstStageMask = wait_stages;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &sem_done;

    vk->lock_queue(vk, queue_family, 0);
    const VkResult res = vkQueueSubmit(queue, 1, &submit_info, VK_NULL_HANDLE);
    vk->unlock_queue(vk, queue_family, 0);
    if (res != VK_SUCCESS) {
        qCWarning(chiakiGui) << "[rtx-vsr] vkQueueSubmit failed, result" << static_cast<int>(res);
        return false;
    }
    done_value = signal_value;
    return true;
}

bool RtxVsr::Impl::ensureImage(Image &img, int w, int h)
{
    if (img.tex && img.w == w && img.h == h)
        return true;
    destroyImage(img);

    struct pl_tex_params tex_params = {};
    tex_params.w = w;
    tex_params.h = h;
    tex_params.format = fmt;
    tex_params.sampleable = true;
    tex_params.renderable = true;
    tex_params.storable = true;
    img.tex = pl_tex_create(gpu, &tex_params);
    if (!img.tex)
        return false;

    VkFormat vk_format = VK_FORMAT_UNDEFINED;
    img.image = pl_vulkan_unwrap(gpu, img.tex, &vk_format, nullptr);
    if (!img.image || vk_format != VK_FORMAT_R8G8B8A8_UNORM) {
        qCWarning(chiakiGui) << "[rtx-vsr] Unexpected texture format" << static_cast<int>(vk_format);
        destroyImage(img);
        return false;
    }

    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = img.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = vk_format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(vk->device, &view_info, nullptr, &img.view) != VK_SUCCESS) {
        img.view = VK_NULL_HANDLE;
        destroyImage(img);
        return false;
    }
    img.w = w;
    img.h = h;
    return true;
}

void RtxVsr::Impl::destroyImage(Image &img)
{
    // Callers make sure no VSR work referencing the image is still pending.
    if (img.view)
        vkDestroyImageView(vk->device, img.view, nullptr);
    if (img.tex)
        pl_tex_destroy(gpu, &img.tex);
    img = Image();
}

RtxVsr::Impl::CacheEntry &RtxVsr::Impl::acquireCacheEntry(uint64_t key)
{
    if (cache.size() < kMaxCachedFrames) {
        cache.emplace_back();
        cache.back().key = key;
        return cache.back();
    }
    auto oldest = std::min_element(cache.begin(), cache.end(), [](const CacheEntry &a, const CacheEntry &b) {
        return a.last_use < b.last_use;
    });
    oldest->key = key;
    return *oldest;
}

NVSDK_NGX_Resource_VK RtxVsr::Impl::makeResource(const Image &img, bool read_write)
{
    NVSDK_NGX_Resource_VK res = {};
    res.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
    res.ReadWrite = read_write;
    res.Resource.ImageViewInfo.ImageView = img.view;
    res.Resource.ImageViewInfo.Image = img.image;
    res.Resource.ImageViewInfo.SubresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    res.Resource.ImageViewInfo.SubresourceRange.levelCount = 1;
    res.Resource.ImageViewInfo.SubresourceRange.layerCount = 1;
    res.Resource.ImageViewInfo.Format = VK_FORMAT_R8G8B8A8_UNORM;
    res.Resource.ImageViewInfo.Width = static_cast<unsigned int>(img.w);
    res.Resource.ImageViewInfo.Height = static_cast<unsigned int>(img.h);
    return res;
}

bool RtxVsr::Impl::evaluate(Image &out, int in_w, int in_h, int quality)
{
    CommandSlot &slot = cmd_slots[next_slot];
    next_slot = (next_slot + 1) % cmd_slots.size();
    if (!waitDone(slot.done_value))
        return false;

    struct pl_vulkan_hold_params hold_in = {};
    hold_in.tex = input.tex;
    hold_in.layout = kVsrLayout;
    hold_in.qf = VK_QUEUE_FAMILY_IGNORED;
    hold_in.semaphore = { sem_in, in_value + 1 };
    struct pl_vulkan_hold_params hold_out = hold_in;
    hold_out.tex = out.tex;
    hold_out.semaphore = { sem_out, out_value + 1 };
    if (!pl_vulkan_hold_ex(gpu, &hold_in))
        return false;
    in_value++;
    if (!pl_vulkan_hold_ex(gpu, &hold_out)) {
        struct pl_vulkan_release_params release = {};
        release.tex = input.tex;
        release.layout = kVsrLayout;
        release.qf = VK_QUEUE_FAMILY_IGNORED;
        release.semaphore = { sem_in, in_value };
        pl_vulkan_release_ex(gpu, &release);
        return false;
    }
    out_value++;

    NVSDK_NGX_Resource_VK in_res = makeResource(input, false);
    NVSDK_NGX_Resource_VK out_res = makeResource(out, true);
    NVSDK_NGX_Parameter_SetVoidPointer(params, NVSDK_NGX_Parameter_Input1, &in_res);
    NVSDK_NGX_Parameter_SetVoidPointer(params, NVSDK_NGX_Parameter_Output, &out_res);
    NVSDK_NGX_Parameter_SetUI(params, "Rect.X", 0);
    NVSDK_NGX_Parameter_SetUI(params, "Rect.Y", 0);
    NVSDK_NGX_Parameter_SetUI(params, "Rect.W", static_cast<unsigned int>(in_w));
    NVSDK_NGX_Parameter_SetUI(params, "Rect.H", static_cast<unsigned int>(in_h));
    NVSDK_NGX_Parameter_SetUI(params, "OutRect.X", 0);
    NVSDK_NGX_Parameter_SetUI(params, "OutRect.Y", 0);
    NVSDK_NGX_Parameter_SetUI(params, "OutRect.W", static_cast<unsigned int>(out.w));
    NVSDK_NGX_Parameter_SetUI(params, "OutRect.H", static_cast<unsigned int>(out.h));
    NVSDK_NGX_Parameter_SetUI(params, "VSR.QualityLevel", static_cast<unsigned int>(quality));

    bool ok = vkResetCommandBuffer(slot.cmd, 0) == VK_SUCCESS;
    if (ok) {
        VkCommandBufferBeginInfo begin_info = {};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        ok = vkBeginCommandBuffer(slot.cmd, &begin_info) == VK_SUCCESS;
    }
    if (ok) {
        const NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_EvaluateFeature_C(slot.cmd, feature, params, nullptr);
        if (NVSDK_NGX_FAILED(result)) {
            qCWarning(chiakiGui).nospace() << "[rtx-vsr] Evaluating VSR failed: " << ngxResultString(result)
                << " (0x" << Qt::hex << static_cast<unsigned>(result) << ")";
            ok = false;
        }
        ok = vkEndCommandBuffer(slot.cmd) == VK_SUCCESS && ok;
    }
    // Always hand both images back to libplacebo; on failure there is nothing
    // to wait for (the holds already completed on libplacebo's side).
    ok = ok && submit(slot.cmd, true);
    if (ok)
        slot.done_value = done_value;

    struct pl_vulkan_release_params release = {};
    release.layout = kVsrLayout;
    release.qf = VK_QUEUE_FAMILY_IGNORED;
    if (ok)
        release.semaphore = { sem_done, done_value };
    else
        release.semaphore = { sem_out, out_value };
    release.tex = out.tex;
    pl_vulkan_release_ex(gpu, &release);
    if (!ok)
        release.semaphore = { sem_in, in_value };
    release.tex = input.tex;
    pl_vulkan_release_ex(gpu, &release);
    return ok;
}

void RtxVsr::Impl::shutdown()
{
    if (vk && vkWaitSemaphores)
        waitDone(done_value);
    if (feature)
        NVSDK_NGX_VULKAN_ReleaseFeature(feature);
    feature = nullptr;
    if (params)
        NVSDK_NGX_VULKAN_DestroyParameters(params);
    params = nullptr;
    if (ngx_initialized)
        NVSDK_NGX_VULKAN_Shutdown1(vk->device);
    ngx_initialized = false;

    if (vk && vkDestroyImageView) {
        for (auto &entry : cache)
            destroyImage(entry.image);
        destroyImage(input);
    }
    cache.clear();
    if (pool)
        vkDestroyCommandPool(vk->device, pool, nullptr);
    pool = VK_NULL_HANDLE;
    cmd_slots = {};
    if (gpu) {
        pl_vulkan_sem_destroy(gpu, &sem_in);
        pl_vulkan_sem_destroy(gpu, &sem_out);
        pl_vulkan_sem_destroy(gpu, &sem_done);
    }
    pl_renderer_destroy(&renderer);
    in_value = out_value = done_value = 0;
    initialized = false;
    vk = nullptr;
    gpu = nullptr;
}

bool RtxVsr::compiledIn()
{
    return true;
}

void RtxVsr::appendRequiredExtensions(std::vector<const char *> &instance_exts,
                                      std::vector<const char *> &device_exts)
{
    unsigned int inst_count = 0, dev_count = 0;
    const char **inst = nullptr, **dev = nullptr;
    const NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_RequiredExtensions(&inst_count, &inst, &dev_count, &dev);
    if (NVSDK_NGX_FAILED(result)) {
        qCWarning(chiakiGui) << "[rtx-vsr] Could not query NGX Vulkan extensions:" << ngxResultString(result);
        return;
    }
    auto append_unique = [](std::vector<const char *> &list, const char *ext) {
        if (!ext)
            return;
        const bool present = std::any_of(list.begin(), list.end(), [ext](const char *e) {
            return e && std::strcmp(e, ext) == 0;
        });
        if (!present)
            list.push_back(ext);
    };
    for (unsigned int i = 0; i < inst_count; i++)
        append_unique(instance_exts, inst[i]);
    for (unsigned int i = 0; i < dev_count; i++)
        append_unique(device_exts, dev[i]);
}

RtxVsr::RtxVsr(pl_log log)
    : impl(std::make_unique<Impl>())
{
    impl->log = log;
}

RtxVsr::~RtxVsr()
{
    destroy();
}

bool RtxVsr::ensureInitialized(pl_vulkan vk)
{
    if (impl->initialized)
        return true;
    if (impl->init_failed || !vk)
        return false;
    if (!impl->init(vk)) {
        qCWarning(chiakiGui) << "[rtx-vsr] RTX Video Super Resolution is unavailable, falling back to libplacebo scaling";
        impl->shutdown();
        impl->init_failed = true;
        return false;
    }
    impl->initialized = true;
    qCInfo(chiakiGui) << "[rtx-vsr] RTX Video Super Resolution initialized";
    return true;
}

bool RtxVsr::failed() const
{
    return impl->init_failed;
}

void RtxVsr::destroy()
{
    impl->shutdown();
}

bool RtxVsr::process(const struct pl_frame *src, uint64_t signature,
                     int out_w, int out_h, int quality,
                     struct pl_frame *out_frame, uint64_t *out_signature)
{
    Impl &d = *impl;
    if (!d.initialized || !src || src->num_planes <= 0)
        return false;
    if (pl_color_space_is_hdr(&src->color)) {
        // The VSR input/output used here is 8-bit SDR; leave HDR streams to libplacebo.
        if (!d.hdr_warned) {
            qCInfo(chiakiGui) << "[rtx-vsr] HDR stream detected, RTX VSR is skipped for HDR content";
            d.hdr_warned = true;
        }
        return false;
    }

    const int in_w = static_cast<int>(std::lround(std::fabs(pl_rect_w(src->crop))));
    const int in_h = static_cast<int>(std::lround(std::fabs(pl_rect_h(src->crop))));
    if (in_w <= 0 || in_h <= 0)
        return false;
    out_w = std::min(out_w, in_w * kMaxScale);
    out_h = std::min(out_h, in_h * kMaxScale);
    if (out_w <= in_w || out_h <= in_h)
        return false;
    quality = std::clamp(quality, 1, 4);

    // Mix the target geometry into the key so a resize never reuses stale output.
    const uint64_t key = signature
        ^ (static_cast<uint64_t>(out_w) * 0x9E3779B97F4A7C15ull)
        ^ (static_cast<uint64_t>(out_h) << 32)
        ^ (static_cast<uint64_t>(quality) << 60);

    auto fill_frame = [&](Impl::CacheEntry &entry) {
        entry.last_use = ++d.use_counter;
        *out_frame = {};
        out_frame->num_planes = 1;
        out_frame->planes[0].texture = entry.image.tex;
        out_frame->planes[0].components = 3;
        out_frame->planes[0].component_mapping[0] = PL_CHANNEL_R;
        out_frame->planes[0].component_mapping[1] = PL_CHANNEL_G;
        out_frame->planes[0].component_mapping[2] = PL_CHANNEL_B;
        out_frame->repr = pl_color_repr_rgb;
        out_frame->color = pl_color_space_bt709;
        out_frame->crop = { 0.0f, 0.0f, static_cast<float>(entry.image.w), static_cast<float>(entry.image.h) };
        *out_signature = entry.key;
    };

    for (auto &entry : d.cache) {
        if (entry.key == key && entry.image.tex) {
            fill_frame(entry);
            return true;
        }
    }

    // Resizing destroys the image view, so pending VSR work must not use it anymore.
    if (d.input.tex && (d.input.w != in_w || d.input.h != in_h) && !d.waitDone(d.done_value))
        return false;
    if (!d.ensureImage(d.input, in_w, in_h))
        return false;

    struct pl_frame in_target = {};
    in_target.num_planes = 1;
    in_target.planes[0].texture = d.input.tex;
    in_target.planes[0].components = 4;
    for (int i = 0; i < 4; i++)
        in_target.planes[0].component_mapping[i] = i;
    in_target.repr = pl_color_repr_rgb;
    in_target.color = pl_color_space_bt709;
    struct pl_render_params render_params = pl_render_default_params;
    render_params.frame_mixer = nullptr;
    if (!pl_render_image(d.renderer, src, &in_target, &render_params)) {
        qCWarning(chiakiGui) << "[rtx-vsr] Failed to prepare the VSR input frame";
        return false;
    }

    Impl::CacheEntry &entry = d.acquireCacheEntry(key);
    // A recycled entry is several frames old, so this wait normally returns at once.
    if (!d.waitDone(entry.done_value) || !d.ensureImage(entry.image, out_w, out_h)) {
        entry.key = 0;
        return false;
    }
    if (!d.evaluate(entry.image, in_w, in_h, quality)) {
        entry.key = 0;
        // Don't retry (and log) every frame if the feature keeps failing.
        if (++d.eval_failures >= 3) {
            qCWarning(chiakiGui) << "[rtx-vsr] Disabling RTX Video Super Resolution after repeated failures";
            d.initialized = false;
            d.init_failed = true;
        }
        return false;
    }
    d.eval_failures = 0;
    entry.done_value = d.done_value;
    fill_frame(entry);
    return true;
}

#else // !CHIAKI_HAVE_RTX_VSR

struct RtxVsr::Impl {};

bool RtxVsr::compiledIn() { return false; }
void RtxVsr::appendRequiredExtensions(std::vector<const char *> &, std::vector<const char *> &) {}
RtxVsr::RtxVsr(pl_log) {}
RtxVsr::~RtxVsr() = default;
bool RtxVsr::ensureInitialized(pl_vulkan) { return false; }
bool RtxVsr::failed() const { return true; }
void RtxVsr::destroy() {}
bool RtxVsr::process(const struct pl_frame *, uint64_t, int, int, int, struct pl_frame *, uint64_t *) { return false; }

#endif // CHIAKI_HAVE_RTX_VSR
