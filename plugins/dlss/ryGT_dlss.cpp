// SPDX-License-Identifier: MIT
//
// ryGT's optional DLSS plugin (dlss\ryGT_dlss.dll). It scales the finished game picture with
// NVIDIA DLSS through NVIDIA's NGX SDK, which is linked here and not in the GPL renderer.
//

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "ModeCache.hpp"
#include "TemporalHistory.hpp"
#include "rygt_dlss_api.h" // Vulkan first: the NGX headers use its types

#include "nvsdk_ngx_helpers.h"
#include "nvsdk_ngx_helpers_vk.h"
#include "nvsdk_ngx_vk.h"

namespace {

// A GUID-like project id for NGX's custom-engine path.
constexpr const char* ProjectId = "5f1a2b3c-4d5e-4f60-8a7b-9c0d1e2f3a4b";
constexpr const char* EngineVersion = "1.0";

std::string g_error = "not initialized";
std::wstring g_plugin_dir;
std::vector<std::wstring> g_search_paths;
std::vector<wchar_t*> g_search_path_ptrs;
NVSDK_NGX_FeatureCommonInfo g_common_info{};

std::vector<std::string> g_instance_extensions;
std::vector<const char*> g_instance_extension_ptrs;
std::vector<std::string> g_device_extensions;
std::vector<const char*> g_device_extension_ptrs;

VkInstance g_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_physical_device = VK_NULL_HANDLE;
VkDevice g_device = VK_NULL_HANDLE;
bool g_initialized = false;
NVSDK_NGX_Parameter* g_params = nullptr;
NVSDK_NGX_Handle* g_feature = nullptr;
uint32_t g_feature_in_w = 0, g_feature_in_h = 0, g_feature_out_w = 0, g_feature_out_h = 0;
RygtDlss::ModeCache<NVSDK_NGX_PerfQuality_Value> modeCache;
RygtDlss::TemporalHistory temporalHistory;
bool g_feature_temporal = false;
RygtDlssQuality g_temporal_quality = RYGT_DLSS_QUALITY_DLAA;
uint32_t g_temporal_flags = 0;

// Device functions the plugin itself uses.
PFN_vkGetPhysicalDeviceMemoryProperties p_vkGetPhysicalDeviceMemoryProperties = nullptr;
PFN_vkCreateImage p_vkCreateImage = nullptr;
PFN_vkDestroyImage p_vkDestroyImage = nullptr;
PFN_vkGetImageMemoryRequirements p_vkGetImageMemoryRequirements = nullptr;
PFN_vkAllocateMemory p_vkAllocateMemory = nullptr;
PFN_vkFreeMemory p_vkFreeMemory = nullptr;
PFN_vkBindImageMemory p_vkBindImageMemory = nullptr;
PFN_vkCreateImageView p_vkCreateImageView = nullptr;
PFN_vkDestroyImageView p_vkDestroyImageView = nullptr;
PFN_vkCmdPipelineBarrier p_vkCmdPipelineBarrier = nullptr;
PFN_vkCmdClearColorImage p_vkCmdClearColorImage = nullptr;
PFN_vkDeviceWaitIdle p_vkDeviceWaitIdle = nullptr;

// The zero motion vectors and flat depth DLSS requires, at the input size.
struct AuxImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    bool cleared = false;
};
AuxImage g_motion, g_depth;

void SetError(const std::string& message) {
    g_error = message;
}

std::string ResultText(NVSDK_NGX_Result result) {
    char text[64];
    std::snprintf(text, sizeof(text), "0x%08x", static_cast<unsigned>(result));
    return text;
}

void SetupCommonInfo() {
    if (!g_search_paths.empty()) {
        return;
    }
    wchar_t path[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&SetupCommonInfo), &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    g_plugin_dir = path;
    g_plugin_dir = g_plugin_dir.substr(0, g_plugin_dir.find_last_of(L"\\/"));
    // nvngx_dlss.dll is looked up next to this plugin.
    g_search_paths.push_back(g_plugin_dir);
    for (auto& p : g_search_paths) {
        g_search_path_ptrs.push_back(p.data());
    }
    g_common_info.PathListInfo.Path = g_search_path_ptrs.data();
    g_common_info.PathListInfo.Length = static_cast<unsigned int>(g_search_path_ptrs.size());
}

NVSDK_NGX_FeatureDiscoveryInfo DiscoveryInfo() {
    SetupCommonInfo();
    NVSDK_NGX_FeatureDiscoveryInfo info{};
    info.SDKVersion = NVSDK_NGX_Version_API;
    info.FeatureID = NVSDK_NGX_Feature_SuperSampling;
    info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    info.Identifier.v.ProjectDesc.ProjectId = ProjectId;
    info.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    info.Identifier.v.ProjectDesc.EngineVersion = EngineVersion;
    info.ApplicationDataPath = g_plugin_dir.c_str();
    info.FeatureInfo = &g_common_info;
    return info;
}

uint32_t FindMemoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties props{};
    p_vkGetPhysicalDeviceMemoryProperties(g_physical_device, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    return UINT32_MAX;
}

void DestroyAux(AuxImage& aux) {
    if (aux.view) p_vkDestroyImageView(g_device, aux.view, nullptr);
    if (aux.image) p_vkDestroyImage(g_device, aux.image, nullptr);
    if (aux.memory) p_vkFreeMemory(g_device, aux.memory, nullptr);
    aux = AuxImage{};
}

bool CreateAux(AuxImage& aux, VkFormat format, uint32_t width, uint32_t height) {
    DestroyAux(aux);
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (p_vkCreateImage(g_device, &info, nullptr, &aux.image) != VK_SUCCESS) {
        SetError("could not create a DLSS input image");
        return false;
    }
    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(g_device, aux.image, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX ||
        p_vkAllocateMemory(g_device, &alloc, nullptr, &aux.memory) != VK_SUCCESS ||
        p_vkBindImageMemory(g_device, aux.image, aux.memory, 0) != VK_SUCCESS) {
        SetError("could not allocate a DLSS input image");
        DestroyAux(aux);
        return false;
    }
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = aux.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (p_vkCreateImageView(g_device, &view, nullptr, &aux.view) != VK_SUCCESS) {
        SetError("could not create a DLSS input image view");
        DestroyAux(aux);
        return false;
    }
    aux.format = format;
    aux.width = width;
    aux.height = height;
    aux.cleared = false;
    return true;
}

// Zeroes the image once and leaves it readable by shaders.
void ClearAux(VkCommandBuffer cmd, AuxImage& aux) {
    if (aux.cleared) {
        return;
    }
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier to_transfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_transfer.srcAccessMask = 0;
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.image = aux.image;
    to_transfer.subresourceRange = range;
    p_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                           nullptr, 0, nullptr, 1, &to_transfer);
    const VkClearColorValue zero{};
    p_vkCmdClearColorImage(cmd, aux.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    VkImageMemoryBarrier to_read = to_transfer;
    to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    p_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                           0, nullptr, 0, nullptr, 1, &to_read);
    aux.cleared = true;
}

// The quality mode whose input size range holds the given input size for this output size.
bool QueryMode(uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h,
              NVSDK_NGX_PerfQuality_Value* mode) {
    if (in_w > out_w || in_h > out_h) {
        return false;
    }
    if (in_w == out_w && in_h == out_h) {
        *mode = NVSDK_NGX_PerfQuality_Value_DLAA;
        return true;
    }
    const NVSDK_NGX_PerfQuality_Value modes[] = {
        NVSDK_NGX_PerfQuality_Value_DLAA,        NVSDK_NGX_PerfQuality_Value_UltraQuality,
        NVSDK_NGX_PerfQuality_Value_MaxQuality,  NVSDK_NGX_PerfQuality_Value_Balanced,
        NVSDK_NGX_PerfQuality_Value_MaxPerf,     NVSDK_NGX_PerfQuality_Value_UltraPerformance,
    };
    for (const auto candidate : modes) {
        unsigned int opt_w = 0, opt_h = 0, max_w = 0, max_h = 0, min_w = 0, min_h = 0;
        float sharpness = 0.0f;
        if (NVSDK_NGX_FAILED(NGX_DLSS_GET_OPTIMAL_SETTINGS(g_params, out_w, out_h, candidate, &opt_w,
                                                           &opt_h, &max_w, &max_h, &min_w, &min_h,
                                                           &sharpness)) ||
            opt_w == 0) {
            continue;
        }
        if (in_w >= min_w && in_w <= max_w && in_h >= min_h && in_h <= max_h) {
            *mode = candidate;
            return true;
        }
    }
    return false;
}

bool PickMode(uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h,
              NVSDK_NGX_PerfQuality_Value* mode) {
    const auto selected = modeCache.Get({in_w, in_h, out_w, out_h}, [&]() {
        NVSDK_NGX_PerfQuality_Value candidate{};
        if (!QueryMode(in_w, in_h, out_w, out_h, &candidate)) {
            return std::optional<NVSDK_NGX_PerfQuality_Value>{};
        }
        return std::optional<NVSDK_NGX_PerfQuality_Value>{candidate};
    });
    if (!selected) {
        return false;
    }
    *mode = *selected;
    return true;
}

void ReleaseFeature() {
    if (g_feature) {
        if (p_vkDeviceWaitIdle) p_vkDeviceWaitIdle(g_device);
        NVSDK_NGX_VULKAN_ReleaseFeature(g_feature);
        g_feature = nullptr;
    }
    temporalHistory.Invalidate();
    g_feature_temporal = false;
}

bool TemporalQuality(RygtDlssQuality quality, NVSDK_NGX_PerfQuality_Value& mode) {
    switch (quality) {
    case RYGT_DLSS_QUALITY_DLAA: mode = NVSDK_NGX_PerfQuality_Value_DLAA; return true;
    case RYGT_DLSS_QUALITY_QUALITY: mode = NVSDK_NGX_PerfQuality_Value_MaxQuality; return true;
    case RYGT_DLSS_QUALITY_BALANCED: mode = NVSDK_NGX_PerfQuality_Value_Balanced; return true;
    case RYGT_DLSS_QUALITY_PERFORMANCE: mode = NVSDK_NGX_PerfQuality_Value_MaxPerf; return true;
    case RYGT_DLSS_QUALITY_ULTRA_PERFORMANCE: mode = NVSDK_NGX_PerfQuality_Value_UltraPerformance; return true;
    default: SetError("invalid temporal DLSS quality mode"); return false;
    }
}

bool ValidImage(const RygtDlssImage& image) {
    return image.image && image.view && image.width && image.height;
}

bool ValidTemporalFrame(const RygtDlssFrame* frame) {
    constexpr uint32_t known_flags = RYGT_DLSS_FRAME_HDR | RYGT_DLSS_FRAME_DEPTH_INVERTED |
        RYGT_DLSS_FRAME_MOTION_JITTERED | RYGT_DLSS_FRAME_MOTION_OUTPUT_RESOLUTION |
        RYGT_DLSS_FRAME_RESET | RYGT_DLSS_FRAME_AUTO_EXPOSURE;
    if (!frame || frame->struct_size != sizeof(RygtDlssFrame) ||
        frame->version != RYGT_DLSS_TEMPORAL_API_VERSION) {
        SetError("temporal DLSS frame has an incompatible ABI");
        return false;
    }
    if ((frame->flags & ~known_flags) || !ValidImage(frame->color) || !ValidImage(frame->depth) ||
        !ValidImage(frame->motion) || !ValidImage(frame->output) ||
        frame->output.image == frame->color.image || frame->output.image == frame->depth.image ||
        frame->output.image == frame->motion.image ||
        frame->output.format != VK_FORMAT_R16G16B16A16_SFLOAT) {
        SetError("temporal DLSS requires distinct valid scene inputs and an RGBA16F output");
        return false;
    }
    const auto& input = frame->color;
    const auto& motion_size = (frame->flags & RYGT_DLSS_FRAME_MOTION_OUTPUT_RESOLUTION)
        ? frame->output : input;
    if (frame->depth.width != input.width || frame->depth.height != input.height ||
        frame->motion.width != motion_size.width || frame->motion.height != motion_size.height ||
        input.width > frame->output.width || input.height > frame->output.height ||
        (frame->motion.format != VK_FORMAT_R16G16_SFLOAT &&
         frame->motion.format != VK_FORMAT_R32G32_SFLOAT)) {
        SetError("temporal DLSS scene dimensions or motion format are inconsistent");
        return false;
    }
    if (!std::isfinite(frame->jitter_x) || !std::isfinite(frame->jitter_y) ||
        !std::isfinite(frame->motion_scale_x) || !std::isfinite(frame->motion_scale_y) ||
        frame->motion_scale_x == 0.0f || frame->motion_scale_y == 0.0f ||
        !std::isfinite(frame->pre_exposure) || frame->pre_exposure <= 0.0f ||
        !std::isfinite(frame->exposure_scale) || frame->exposure_scale <= 0.0f ||
        !std::isfinite(frame->frame_time_ms) || frame->frame_time_ms <= 0.0f) {
        SetError("temporal DLSS jitter, motion scale, exposure or frame time is invalid");
        return false;
    }
    switch (frame->depth.format) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
    case VK_FORMAT_R32_SFLOAT: return true;
    default: SetError("unsupported temporal DLSS depth format"); return false;
    }
}

} // namespace

extern "C" {

__declspec(dllexport) int VKAPI_CALL RygtDlss_ApiVersion(void) {
    return RYGT_DLSS_API_VERSION;
}

__declspec(dllexport) int VKAPI_CALL RygtDlss_GetInstanceExtensions(uint32_t* count,
                                                                    const char* const** names) {
    const auto info = DiscoveryInfo();
    uint32_t n = 0;
    VkExtensionProperties* props = nullptr;
    const auto result = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&info, &n, &props);
    if (NVSDK_NGX_FAILED(result)) {
        SetError("instance extension query failed: " + ResultText(result));
        return 1;
    }
    g_instance_extensions.clear();
    for (uint32_t i = 0; i < n; ++i) g_instance_extensions.emplace_back(props[i].extensionName);
    g_instance_extension_ptrs.clear();
    for (auto& e : g_instance_extensions) g_instance_extension_ptrs.push_back(e.c_str());
    *count = static_cast<uint32_t>(g_instance_extension_ptrs.size());
    *names = g_instance_extension_ptrs.data();
    return 0;
}

__declspec(dllexport) int VKAPI_CALL RygtDlss_GetDeviceExtensions(VkInstance instance,
                                                                  VkPhysicalDevice physical_device,
                                                                  PFN_vkGetInstanceProcAddr,
                                                                  uint32_t* count,
                                                                  const char* const** names) {
    const auto info = DiscoveryInfo();
    uint32_t n = 0;
    VkExtensionProperties* props = nullptr;
    const auto result = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
        instance, physical_device, &info, &n, &props);
    if (NVSDK_NGX_FAILED(result)) {
        SetError("device extension query failed: " + ResultText(result));
        return 1;
    }
    g_device_extensions.clear();
    for (uint32_t i = 0; i < n; ++i) g_device_extensions.emplace_back(props[i].extensionName);
    g_device_extension_ptrs.clear();
    for (auto& e : g_device_extensions) g_device_extension_ptrs.push_back(e.c_str());
    *count = static_cast<uint32_t>(g_device_extension_ptrs.size());
    *names = g_device_extension_ptrs.data();
    return 0;
}

__declspec(dllexport) int VKAPI_CALL RygtDlss_Init(VkInstance instance,
                                                   VkPhysicalDevice physical_device,
                                                   VkDevice device, PFN_vkGetInstanceProcAddr gipa,
                                                   PFN_vkGetDeviceProcAddr gdpa,
                                                   const wchar_t* data_path) {
    modeCache.Reset();
    temporalHistory.Invalidate();
    SetupCommonInfo();
    g_instance = instance;
    g_physical_device = physical_device;
    g_device = device;

    p_vkGetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        gipa(instance, "vkGetPhysicalDeviceMemoryProperties"));
#define LOAD(name) p_##name = reinterpret_cast<PFN_##name>(gdpa(device, #name))
    LOAD(vkCreateImage);
    LOAD(vkDestroyImage);
    LOAD(vkGetImageMemoryRequirements);
    LOAD(vkAllocateMemory);
    LOAD(vkFreeMemory);
    LOAD(vkBindImageMemory);
    LOAD(vkCreateImageView);
    LOAD(vkDestroyImageView);
    LOAD(vkCmdPipelineBarrier);
    LOAD(vkCmdClearColorImage);
    LOAD(vkDeviceWaitIdle);
#undef LOAD
    if (!p_vkGetPhysicalDeviceMemoryProperties || !p_vkCreateImage || !p_vkCmdClearColorImage) {
        SetError("missing Vulkan functions");
        return 1;
    }

    const wchar_t* path = data_path && *data_path ? data_path : g_plugin_dir.c_str();
    auto result = NVSDK_NGX_VULKAN_Init_with_ProjectID(ProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                        EngineVersion, path, instance,
                                                        physical_device, device, gipa, gdpa,
                                                        &g_common_info);
    if (NVSDK_NGX_FAILED(result)) {
        SetError("NGX init failed: " + ResultText(result) +
                 " (is nvngx_dlss.dll next to ryGT_dlss.dll, and is the GPU an RTX one?)");
        return 1;
    }
    g_initialized = true;
    result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&g_params);
    if (NVSDK_NGX_FAILED(result) || !g_params) {
        SetError("NGX capability query failed: " + ResultText(result));
        return 1;
    }
    int available = 0;
    result = NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    if (NVSDK_NGX_FAILED(result) || !available) {
        int needs_driver = 0;
        NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,
                                 &needs_driver);
        SetError(needs_driver ? "DLSS needs a newer NVIDIA driver"
                              : "DLSS is not available on this GPU");
        return 1;
    }
    SetError("");
    return 0;
}

__declspec(dllexport) int VKAPI_CALL RygtDlss_Supports(uint32_t in_w, uint32_t in_h,
                                                       uint32_t out_w, uint32_t out_h) {
    if (!g_initialized || !g_params) {
        return 0;
    }
    NVSDK_NGX_PerfQuality_Value mode{};
    return PickMode(in_w, in_h, out_w, out_h, &mode) ? 1 : 0;
}

__declspec(dllexport) int VKAPI_CALL RygtDlss_Evaluate(VkCommandBuffer cmd,
                                                       const RygtDlssImage* input,
                                                       const RygtDlssImage* output,
                                                       float sharpness) {
    if (!g_initialized || !g_params || !input || !output) {
        SetError("DLSS is not initialized");
        return 1;
    }
    if (!g_feature || g_feature_temporal || g_feature_in_w != input->width || g_feature_in_h != input->height ||
        g_feature_out_w != output->width || g_feature_out_h != output->height) {
        ReleaseFeature();
        NVSDK_NGX_PerfQuality_Value mode{};
        if (!PickMode(input->width, input->height, output->width, output->height, &mode)) {
            SetError("no DLSS mode scales this input size to this output size");
            return 1;
        }
        NVSDK_NGX_DLSS_Create_Params create{};
        create.Feature.InWidth = input->width;
        create.Feature.InHeight = input->height;
        create.Feature.InTargetWidth = output->width;
        create.Feature.InTargetHeight = output->height;
        create.Feature.InPerfQualityValue = mode;
        create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
        const auto result =
            NGX_VULKAN_CREATE_DLSS_EXT1(g_device, cmd, 1, 1, &g_feature, g_params, &create);
        if (NVSDK_NGX_FAILED(result) || !g_feature) {
            g_feature = nullptr;
            SetError("DLSS feature creation failed: " + ResultText(result));
            return 1;
        }
        g_feature_in_w = input->width;
        g_feature_in_h = input->height;
        g_feature_out_w = output->width;
        g_feature_out_h = output->height;
        if (!CreateAux(g_motion, VK_FORMAT_R16G16_SFLOAT, input->width, input->height) ||
            !CreateAux(g_depth, VK_FORMAT_R32_SFLOAT, input->width, input->height)) {
            return 1;
        }
    }
    ClearAux(cmd, g_motion);
    ClearAux(cmd, g_depth);

    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    auto color = NVSDK_NGX_Create_ImageView_Resource_VK(input->view, input->image, range,
                                                        input->format, input->width, input->height,
                                                        false);
    auto out = NVSDK_NGX_Create_ImageView_Resource_VK(output->view, output->image, range,
                                                      output->format, output->width,
                                                      output->height, true);
    auto motion = NVSDK_NGX_Create_ImageView_Resource_VK(g_motion.view, g_motion.image, range,
                                                         g_motion.format, g_motion.width,
                                                         g_motion.height, false);
    auto depth = NVSDK_NGX_Create_ImageView_Resource_VK(g_depth.view, g_depth.image, range,
                                                        g_depth.format, g_depth.width,
                                                        g_depth.height, false);
    NVSDK_NGX_VK_DLSS_Eval_Params eval{};
    eval.Feature.pInColor = &color;
    eval.Feature.pInOutput = &out;
    eval.Feature.InSharpness = sharpness;
    eval.pInDepth = &depth;
    eval.pInMotionVectors = &motion;
    eval.InJitterOffsetX = 0.0f;
    eval.InJitterOffsetY = 0.0f;
    eval.InRenderSubrectDimensions = {input->width, input->height};
    eval.InReset = 1; // no motion vectors: never reuse an earlier frame
    eval.InMVScaleX = 1.0f;
    eval.InMVScaleY = 1.0f;
    const auto result = NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, g_feature, g_params, &eval);
    if (NVSDK_NGX_FAILED(result)) {
        SetError("DLSS evaluation failed: " + ResultText(result));
        return 1;
    }
    return 0;
}

__declspec(dllexport) int VKAPI_CALL RygtDlss_TemporalApiVersion(void) {
    return RYGT_DLSS_TEMPORAL_API_VERSION;
}

__declspec(dllexport) int VKAPI_CALL RygtDlss_GetOptimalSettings(
    uint32_t outputWidth, uint32_t outputHeight, RygtDlssQuality quality, RygtDlssOptimalSettings* settings) {
    NVSDK_NGX_PerfQuality_Value mode{};
    if (!g_initialized || !g_params || !outputWidth || !outputHeight || !settings ||
        settings->struct_size != sizeof(RygtDlssOptimalSettings)) {
        SetError("invalid or uninitialized temporal DLSS settings query");
        return 1;
    }
    if (!TemporalQuality(quality, mode)) return 1;
    RygtDlssOptimalSettings result{};
    result.struct_size = sizeof(result);
    float unused_sharpness{};
    const auto status = NGX_DLSS_GET_OPTIMAL_SETTINGS(g_params, outputWidth, outputHeight, mode,
        &result.optimal_width, &result.optimal_height, &result.maximum_width, &result.maximum_height,
        &result.minimum_width, &result.minimum_height, &unused_sharpness);
    if (NVSDK_NGX_FAILED(status) || !result.optimal_width || !result.optimal_height) {
        SetError("temporal DLSS optimal-settings query failed: " + ResultText(status));
        return 1;
    }
    *settings = result;
    SetError("");
    return 0;
}

__declspec(dllexport) int VKAPI_CALL RygtDlss_EvaluateTemporal(VkCommandBuffer cmd,
                                                              const RygtDlssFrame* frame) {
    if (!g_initialized || !g_params || !cmd) {
        SetError("temporal DLSS is not initialized or command buffer is null");
        return 1;
    }
    if (!ValidTemporalFrame(frame)) return 1;
    NVSDK_NGX_PerfQuality_Value mode{};
    if (!TemporalQuality(frame->quality, mode)) return 1;
    const uint32_t stable_flags = frame->flags & ~RYGT_DLSS_FRAME_RESET;
    const auto& input = frame->color;
    const auto& output = frame->output;
    if (!g_feature || !g_feature_temporal || g_feature_in_w != input.width ||
        g_feature_in_h != input.height || g_feature_out_w != output.width ||
        g_feature_out_h != output.height || g_temporal_quality != frame->quality ||
        g_temporal_flags != stable_flags) {
        RygtDlssOptimalSettings settings{sizeof(RygtDlssOptimalSettings)};
        if (RygtDlss_GetOptimalSettings(output.width, output.height, frame->quality, &settings)) return 1;
        if (input.width < settings.minimum_width || input.width > settings.maximum_width ||
            input.height < settings.minimum_height || input.height > settings.maximum_height) {
            SetError("render dimensions are outside the selected temporal DLSS mode's range");
            return 1;
        }
        ReleaseFeature();
        for (const char* key : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                               NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                               NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                               NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                               NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance}) {
            NVSDK_NGX_Parameter_SetUI(g_params, key, NVSDK_NGX_DLSS_Hint_Render_Preset_M);
        }
        NVSDK_NGX_DLSS_Create_Params create{};
        create.Feature.InWidth = input.width;
        create.Feature.InHeight = input.height;
        create.Feature.InTargetWidth = output.width;
        create.Feature.InTargetHeight = output.height;
        create.Feature.InPerfQualityValue = mode;
        create.InFeatureCreateFlags = 0;
        if (!(frame->flags & RYGT_DLSS_FRAME_MOTION_OUTPUT_RESOLUTION))
            create.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
        if (frame->flags & RYGT_DLSS_FRAME_HDR)
            create.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
        if (frame->flags & RYGT_DLSS_FRAME_DEPTH_INVERTED)
            create.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
        if (frame->flags & RYGT_DLSS_FRAME_MOTION_JITTERED)
            create.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
        if (frame->flags & RYGT_DLSS_FRAME_AUTO_EXPOSURE)
            create.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        const auto status = NGX_VULKAN_CREATE_DLSS_EXT1(g_device, cmd, 1, 1, &g_feature, g_params, &create);
        if (NVSDK_NGX_FAILED(status) || !g_feature) {
            g_feature = nullptr;
            SetError("DLSS 4.5 temporal feature creation failed: " + ResultText(status));
            return 1;
        }
        g_feature_in_w = input.width; g_feature_in_h = input.height;
        g_feature_out_w = output.width; g_feature_out_h = output.height;
        g_feature_temporal = true;
        g_temporal_quality = frame->quality;
        g_temporal_flags = stable_flags;
    }
    const VkImageSubresourceRange color_range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const VkImageSubresourceRange depth_range{
        static_cast<VkImageAspectFlags>(frame->depth.format == VK_FORMAT_R32_SFLOAT
            ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT),
        0, 1, 0, 1};
    const auto wrap = [](const RygtDlssImage& image, VkImageSubresourceRange range, bool writable) {
        return NVSDK_NGX_Create_ImageView_Resource_VK(image.view, image.image, range,
            image.format, image.width, image.height, writable);
    };
    auto color = wrap(input, color_range, false);
    auto depth = wrap(frame->depth, depth_range, false);
    auto motion = wrap(frame->motion, color_range, false);
    auto out = wrap(output, color_range, true);
    NVSDK_NGX_VK_DLSS_Eval_Params eval{};
    eval.Feature.pInColor = &color;
    eval.Feature.pInOutput = &out;
    eval.pInDepth = &depth;
    eval.pInMotionVectors = &motion;
    eval.InJitterOffsetX = frame->jitter_x;
    eval.InJitterOffsetY = frame->jitter_y;
    eval.InRenderSubrectDimensions = {input.width, input.height};
    eval.InReset = temporalHistory.NeedsReset(frame->frame_id, frame->flags & RYGT_DLSS_FRAME_RESET);
    eval.InMVScaleX = frame->motion_scale_x;
    eval.InMVScaleY = frame->motion_scale_y;
    eval.InPreExposure = frame->pre_exposure;
    eval.InExposureScale = frame->exposure_scale;
    eval.InFrameTimeDeltaInMsec = frame->frame_time_ms;
    const auto status = NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, g_feature, g_params, &eval);
    if (NVSDK_NGX_FAILED(status)) {
        temporalHistory.Invalidate();
        SetError("DLSS 4.5 temporal evaluation failed: " + ResultText(status));
        return 1;
    }
    temporalHistory.Commit(frame->frame_id);
    SetError("");
    return 0;
}

__declspec(dllexport) void VKAPI_CALL RygtDlss_Shutdown(void) {
    modeCache.Reset();
    ReleaseFeature();
    if (g_device) {
        DestroyAux(g_motion);
        DestroyAux(g_depth);
    }
    if (g_params) {
        NVSDK_NGX_VULKAN_DestroyParameters(g_params);
        g_params = nullptr;
    }
    if (g_initialized) {
        NVSDK_NGX_VULKAN_Shutdown1(g_device);
        g_initialized = false;
    }
}

__declspec(dllexport) const char* VKAPI_CALL RygtDlss_LastError(void) {
    return g_error.c_str();
}

} // extern "C"
