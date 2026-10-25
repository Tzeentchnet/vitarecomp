#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "gpu_internal.hpp"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace vitarecomp::gpu {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::uint32_t kFrameContextCount = 3;
constexpr std::uint32_t kRtvDescriptorCount = 1024;
constexpr std::uint32_t kDsvDescriptorCount = 256;
constexpr std::uint32_t kResourceDescriptorCount = 8192;
constexpr std::uint32_t kSamplerDescriptorCount = 2048;
constexpr std::uint32_t kSamplerStagingDescriptorCount = 1024;
constexpr std::uint32_t kPushConstantRegisterSpace = 0x7fff;

Status dx_status(vita_gpu_result result, std::string message, HRESULT hr) {
    return Status::error(result, std::move(message), VITA_GPU_BACKEND_DX12,
                         static_cast<std::int64_t>(hr));
}

std::string hr_text(HRESULT hr) {
    char *message = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageA(
        flags, nullptr, static_cast<DWORD>(hr), 0,
        reinterpret_cast<char *>(&message), 0, nullptr);
    std::string text;
    if (length != 0 && message != nullptr) {
        text.assign(message, length);
        while (!text.empty() &&
               (text.back() == '\r' || text.back() == '\n')) {
            text.pop_back();
        }
    }
    if (message != nullptr) {
        LocalFree(message);
    }
    if (text.empty()) {
        std::ostringstream stream;
        stream << "HRESULT 0x" << std::hex
               << static_cast<std::uint32_t>(hr);
        text = stream.str();
    }
    return text;
}

std::string utf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string output(static_cast<std::size_t>(required), '\0');
    (void)WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), output.data(), required, nullptr,
        nullptr);
    return output;
}

std::wstring wide(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) {
        return {};
    }
    std::wstring output(static_cast<std::size_t>(required), L'\0');
    (void)MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), output.data(), required);
    return output;
}

template <std::size_t N>
void copy_text(char (&destination)[N], std::string_view source) {
    const std::size_t count = std::min(N - 1, source.size());
    if (count != 0) {
        std::memcpy(destination, source.data(), count);
    }
    destination[count] = '\0';
}

DXGI_FORMAT dxgi_format(vita_gpu_format format) {
    switch (format) {
    case VITA_GPU_FORMAT_R8_UNORM:
        return DXGI_FORMAT_R8_UNORM;
    case VITA_GPU_FORMAT_RG8_UNORM:
        return DXGI_FORMAT_R8G8_UNORM;
    case VITA_GPU_FORMAT_RGBA8_UNORM:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case VITA_GPU_FORMAT_RGBA8_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case VITA_GPU_FORMAT_BGRA8_UNORM:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case VITA_GPU_FORMAT_BGRA8_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case VITA_GPU_FORMAT_R16_FLOAT:
        return DXGI_FORMAT_R16_FLOAT;
    case VITA_GPU_FORMAT_RG16_FLOAT:
        return DXGI_FORMAT_R16G16_FLOAT;
    case VITA_GPU_FORMAT_RGBA16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case VITA_GPU_FORMAT_R32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case VITA_GPU_FORMAT_RG32_FLOAT:
        return DXGI_FORMAT_R32G32_FLOAT;
    case VITA_GPU_FORMAT_RGB32_FLOAT:
        return DXGI_FORMAT_R32G32B32_FLOAT;
    case VITA_GPU_FORMAT_RGBA32_FLOAT:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case VITA_GPU_FORMAT_R16_UINT:
        return DXGI_FORMAT_R16_UINT;
    case VITA_GPU_FORMAT_R32_UINT:
        return DXGI_FORMAT_R32_UINT;
    case VITA_GPU_FORMAT_D16_UNORM:
        return DXGI_FORMAT_D16_UNORM;
    case VITA_GPU_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case VITA_GPU_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_D32_FLOAT;
    case VITA_GPU_FORMAT_D32_FLOAT_S8_UINT:
        return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    case VITA_GPU_FORMAT_BC1_RGBA_UNORM:
        return DXGI_FORMAT_BC1_UNORM;
    case VITA_GPU_FORMAT_BC1_RGBA_SRGB:
        return DXGI_FORMAT_BC1_UNORM_SRGB;
    case VITA_GPU_FORMAT_BC2_UNORM:
        return DXGI_FORMAT_BC2_UNORM;
    case VITA_GPU_FORMAT_BC2_SRGB:
        return DXGI_FORMAT_BC2_UNORM_SRGB;
    case VITA_GPU_FORMAT_BC3_UNORM:
        return DXGI_FORMAT_BC3_UNORM;
    case VITA_GPU_FORMAT_BC3_SRGB:
        return DXGI_FORMAT_BC3_UNORM_SRGB;
    case VITA_GPU_FORMAT_BC4_UNORM:
        return DXGI_FORMAT_BC4_UNORM;
    case VITA_GPU_FORMAT_BC5_UNORM:
        return DXGI_FORMAT_BC5_UNORM;
    case VITA_GPU_FORMAT_BC7_UNORM:
        return DXGI_FORMAT_BC7_UNORM;
    case VITA_GPU_FORMAT_BC7_SRGB:
        return DXGI_FORMAT_BC7_UNORM_SRGB;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

bool depth_format(vita_gpu_format format) {
    return format >= VITA_GPU_FORMAT_D16_UNORM &&
           format <= VITA_GPU_FORMAT_D32_FLOAT_S8_UINT;
}

DXGI_FORMAT resource_format(vita_gpu_format format) {
    switch (format) {
    case VITA_GPU_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_TYPELESS;
    case VITA_GPU_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24G8_TYPELESS;
    case VITA_GPU_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_TYPELESS;
    case VITA_GPU_FORMAT_D32_FLOAT_S8_UINT:
        return DXGI_FORMAT_R32G8X24_TYPELESS;
    default:
        return dxgi_format(format);
    }
}

DXGI_FORMAT shader_resource_format(vita_gpu_format format) {
    switch (format) {
    case VITA_GPU_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    case VITA_GPU_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case VITA_GPU_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case VITA_GPU_FORMAT_D32_FLOAT_S8_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:
        return dxgi_format(format);
    }
}

DXGI_FORMAT swapchain_format(vita_gpu_format format) {
    switch (format) {
    case VITA_GPU_FORMAT_RGBA8_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case VITA_GPU_FORMAT_BGRA8_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:
        return dxgi_format(format);
    }
}

D3D12_RESOURCE_STATES resource_state(vita_gpu_resource_state state) {
    D3D12_RESOURCE_STATES output = D3D12_RESOURCE_STATE_COMMON;
    if ((state & VITA_GPU_RESOURCE_STATE_COPY_SOURCE) != 0) {
        output |= D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    if ((state & VITA_GPU_RESOURCE_STATE_COPY_DEST) != 0) {
        output |= D3D12_RESOURCE_STATE_COPY_DEST;
    }
    if ((state & (VITA_GPU_RESOURCE_STATE_VERTEX_BUFFER |
                  VITA_GPU_RESOURCE_STATE_CONSTANT_BUFFER)) != 0) {
        output |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    }
    if ((state & VITA_GPU_RESOURCE_STATE_INDEX_BUFFER) != 0) {
        output |= D3D12_RESOURCE_STATE_INDEX_BUFFER;
    }
    if ((state & VITA_GPU_RESOURCE_STATE_SHADER_READ) != 0) {
        output |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    if ((state & VITA_GPU_RESOURCE_STATE_SHADER_WRITE) != 0) {
        output |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    if ((state & VITA_GPU_RESOURCE_STATE_RENDER_TARGET) != 0) {
        output |= D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    if ((state & VITA_GPU_RESOURCE_STATE_DEPTH_READ) != 0) {
        output |= D3D12_RESOURCE_STATE_DEPTH_READ;
    }
    if ((state & VITA_GPU_RESOURCE_STATE_DEPTH_WRITE) != 0) {
        output |= D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }
    if ((state & VITA_GPU_RESOURCE_STATE_PRESENT) != 0) {
        output |= D3D12_RESOURCE_STATE_PRESENT;
    }
    return output;
}

D3D12_COMPARISON_FUNC comparison(vita_gpu_compare_op value) {
    return static_cast<D3D12_COMPARISON_FUNC>(
        static_cast<std::uint32_t>(value) + 1U);
}

D3D12_TEXTURE_ADDRESS_MODE address_mode(vita_gpu_address_mode value) {
    switch (value) {
    case VITA_GPU_ADDRESS_REPEAT:
        return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case VITA_GPU_ADDRESS_MIRROR_REPEAT:
        return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case VITA_GPU_ADDRESS_CLAMP_EDGE:
        return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case VITA_GPU_ADDRESS_CLAMP_BORDER:
        return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    default:
        return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    }
}

D3D12_BLEND blend_factor(vita_gpu_blend_factor value) {
    switch (value) {
    case VITA_GPU_BLEND_ZERO:
        return D3D12_BLEND_ZERO;
    case VITA_GPU_BLEND_ONE:
        return D3D12_BLEND_ONE;
    case VITA_GPU_BLEND_SRC_COLOR:
        return D3D12_BLEND_SRC_COLOR;
    case VITA_GPU_BLEND_INV_SRC_COLOR:
        return D3D12_BLEND_INV_SRC_COLOR;
    case VITA_GPU_BLEND_SRC_ALPHA:
        return D3D12_BLEND_SRC_ALPHA;
    case VITA_GPU_BLEND_INV_SRC_ALPHA:
        return D3D12_BLEND_INV_SRC_ALPHA;
    case VITA_GPU_BLEND_DST_COLOR:
        return D3D12_BLEND_DEST_COLOR;
    case VITA_GPU_BLEND_INV_DST_COLOR:
        return D3D12_BLEND_INV_DEST_COLOR;
    case VITA_GPU_BLEND_DST_ALPHA:
        return D3D12_BLEND_DEST_ALPHA;
    case VITA_GPU_BLEND_INV_DST_ALPHA:
        return D3D12_BLEND_INV_DEST_ALPHA;
    default:
        return D3D12_BLEND_ONE;
    }
}

D3D12_BLEND_OP blend_operation(vita_gpu_blend_op value) {
    switch (value) {
    case VITA_GPU_BLEND_OP_ADD:
        return D3D12_BLEND_OP_ADD;
    case VITA_GPU_BLEND_OP_SUBTRACT:
        return D3D12_BLEND_OP_SUBTRACT;
    case VITA_GPU_BLEND_OP_REVERSE_SUBTRACT:
        return D3D12_BLEND_OP_REV_SUBTRACT;
    case VITA_GPU_BLEND_OP_MIN:
        return D3D12_BLEND_OP_MIN;
    case VITA_GPU_BLEND_OP_MAX:
        return D3D12_BLEND_OP_MAX;
    default:
        return D3D12_BLEND_OP_ADD;
    }
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type(
    vita_gpu_primitive_topology value) {
    switch (value) {
    case VITA_GPU_TOPOLOGY_POINT_LIST:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    case VITA_GPU_TOPOLOGY_LINE_LIST:
    case VITA_GPU_TOPOLOGY_LINE_STRIP:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    case VITA_GPU_TOPOLOGY_TRIANGLE_LIST:
    case VITA_GPU_TOPOLOGY_TRIANGLE_STRIP:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    default:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
    }
}

D3D_PRIMITIVE_TOPOLOGY primitive_topology(
    vita_gpu_primitive_topology value) {
    switch (value) {
    case VITA_GPU_TOPOLOGY_POINT_LIST:
        return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    case VITA_GPU_TOPOLOGY_LINE_LIST:
        return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case VITA_GPU_TOPOLOGY_LINE_STRIP:
        return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case VITA_GPU_TOPOLOGY_TRIANGLE_LIST:
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case VITA_GPU_TOPOLOGY_TRIANGLE_STRIP:
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    default:
        return D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    }
}

D3D12_SHADER_VISIBILITY shader_visibility(
    vita_gpu_shader_stage_flags visibility) {
    if (visibility == VITA_GPU_SHADER_STAGE_VERTEX_BIT) {
        return D3D12_SHADER_VISIBILITY_VERTEX;
    }
    if (visibility == VITA_GPU_SHADER_STAGE_PIXEL_BIT) {
        return D3D12_SHADER_VISIBILITY_PIXEL;
    }
    return D3D12_SHADER_VISIBILITY_ALL;
}

D3D12_DESCRIPTOR_RANGE_TYPE range_type(vita_gpu_binding_type type) {
    switch (type) {
    case VITA_GPU_BINDING_CONSTANT_BUFFER:
        return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    case VITA_GPU_BINDING_STORAGE_BUFFER:
    case VITA_GPU_BINDING_STORAGE_TEXTURE:
        return D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    case VITA_GPU_BINDING_SAMPLED_TEXTURE:
        return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    case VITA_GPU_BINDING_SAMPLER:
        return D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    default:
        return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    }
}

D3D12_FILTER sampler_filter(const vita_gpu_sampler_desc &desc) {
    if (desc.max_anisotropy > 1) {
        return desc.compare_enable != 0 ? D3D12_FILTER_COMPARISON_ANISOTROPIC
                                        : D3D12_FILTER_ANISOTROPIC;
    }
    const D3D12_FILTER_TYPE min_filter =
        desc.min_filter == VITA_GPU_FILTER_LINEAR
            ? D3D12_FILTER_TYPE_LINEAR
            : D3D12_FILTER_TYPE_POINT;
    const D3D12_FILTER_TYPE mag_filter =
        desc.mag_filter == VITA_GPU_FILTER_LINEAR
            ? D3D12_FILTER_TYPE_LINEAR
            : D3D12_FILTER_TYPE_POINT;
    const D3D12_FILTER_TYPE mip_filter =
        desc.mipmap_mode == VITA_GPU_MIPMAP_LINEAR
            ? D3D12_FILTER_TYPE_LINEAR
            : D3D12_FILTER_TYPE_POINT;
    return D3D12_ENCODE_BASIC_FILTER(
        min_filter, mag_filter, mip_filter,
        desc.compare_enable != 0 ? D3D12_FILTER_REDUCTION_TYPE_COMPARISON
                                 : D3D12_FILTER_REDUCTION_TYPE_STANDARD);
}

D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

D3D12_RESOURCE_DESC buffer_resource_desc(
    std::uint64_t size, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc = {1, 0};
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return desc;
}

class DescriptorHeap {
public:
    HRESULT initialize(ID3D12Device *device,
                       D3D12_DESCRIPTOR_HEAP_TYPE type,
                       std::uint32_t count, bool shader_visible) {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = type;
        desc.NumDescriptors = count;
        desc.Flags = shader_visible
                         ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                         : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        const HRESULT hr =
            device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap_));
        if (FAILED(hr)) {
            return hr;
        }
        increment_ = device->GetDescriptorHandleIncrementSize(type);
        used_.assign(count, false);
        shader_visible_ = shader_visible;
        return S_OK;
    }

    std::optional<std::uint32_t> allocate(std::uint32_t count = 1) {
        if (count == 0) {
            return 0;
        }
        std::uint32_t run = 0;
        for (std::uint32_t index = 0; index < used_.size(); ++index) {
            run = used_[index] ? 0 : run + 1;
            if (run == count) {
                const std::uint32_t start = index + 1 - count;
                std::fill(used_.begin() + start,
                          used_.begin() + start + count, true);
                return start;
            }
        }
        return std::nullopt;
    }

    void release(std::uint32_t start, std::uint32_t count = 1) {
        if (count == 0 || start >= used_.size() ||
            count > used_.size() - start) {
            return;
        }
        std::fill(used_.begin() + start, used_.begin() + start + count,
                  false);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpu(std::uint32_t index) const {
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            heap_->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * increment_;
        return handle;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE gpu(std::uint32_t index) const {
        D3D12_GPU_DESCRIPTOR_HANDLE handle{};
        if (shader_visible_) {
            handle = heap_->GetGPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<UINT64>(index) * increment_;
        }
        return handle;
    }

    ID3D12DescriptorHeap *get() const { return heap_.Get(); }

private:
    ComPtr<ID3D12DescriptorHeap> heap_;
    std::vector<bool> used_;
    std::uint32_t increment_ = 0;
    bool shader_visible_ = false;
};

struct AdapterRecord {
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 desc{};
    vita_gpu_adapter_type type = VITA_GPU_ADAPTER_OTHER;
    vita_gpu_capability_flags capabilities = 0;
    bool warp = false;
    std::string name;
};

vita_gpu_capability_flags adapter_capabilities(IDXGIAdapter1 *adapter) {
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0,
                                 IID_PPV_ARGS(&device)))) {
        return 0;
    }
    vita_gpu_capability_flags flags =
        VITA_GPU_CAPABILITY_GRAPHICS | VITA_GPU_CAPABILITY_COMPUTE |
        VITA_GPU_CAPABILITY_TIMELINE_FENCES |
        VITA_GPU_CAPABILITY_DEBUG_MARKERS |
        VITA_GPU_CAPABILITY_BC_TEXTURES |
        VITA_GPU_CAPABILITY_ANISOTROPY;

    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
    if (SUCCEEDED(device->CheckFeatureSupport(
            D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1))) &&
        options1.Int64ShaderOps) {
        flags |= VITA_GPU_CAPABILITY_SHADER_INT64;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 options4{};
    if (SUCCEEDED(device->CheckFeatureSupport(
            D3D12_FEATURE_D3D12_OPTIONS4, &options4, sizeof(options4))) &&
        options4.Native16BitShaderOpsSupported) {
        flags |= VITA_GPU_CAPABILITY_SHADER_FLOAT16;
    }
    return flags;
}

bool adapter_usable(IDXGIAdapter1 *adapter) {
    ComPtr<ID3D12Device> device;
    return SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0,
                                       IID_PPV_ARGS(&device)));
}

class Dx12Device;

class Dx12Factory final : public BackendFactory {
public:
    Dx12Factory() {
        HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_));
        if (FAILED(hr)) {
            initialization_error_ =
                "CreateDXGIFactory2 failed: " + hr_text(hr);
            initialization_hr_ = hr;
            return;
        }
        enumerate_adapters();
        if (adapters_.empty()) {
            initialization_error_ =
                "DXGI found no Direct3D 12 capable hardware adapter or WARP";
            initialization_hr_ = DXGI_ERROR_NOT_FOUND;
        }
    }

    vita_gpu_backend backend() const noexcept override {
        return VITA_GPU_BACKEND_DX12;
    }

    std::string_view name() const noexcept override { return "dx12"; }
    bool test_only() const noexcept override { return false; }

    Status availability(bool &available, bool &validation_available,
                        std::string &reason) const override {
        available = initialization_error_.empty();
        ComPtr<ID3D12Debug> debug;
        validation_available =
            SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        reason = available ? std::string{} : initialization_error_;
        return Status::success();
    }

    Status adapter_count(std::uint32_t &count) const override {
        if (!initialization_error_.empty()) {
            return dx_status(VITA_GPU_ERROR_BACKEND_UNAVAILABLE,
                             initialization_error_, initialization_hr_);
        }
        count = static_cast<std::uint32_t>(adapters_.size());
        return Status::success();
    }

    Status adapter_info(std::uint32_t index,
                        vita_gpu_adapter_info &output) const override {
        if (index >= adapters_.size()) {
            return Status::error(VITA_GPU_ERROR_ADAPTER_NOT_FOUND,
                                 "DX12 adapter index is out of range",
                                 VITA_GPU_BACKEND_DX12);
        }
        const AdapterRecord &record = adapters_[index];
        output.adapter_index = index;
        output.type = record.type;
        output.vendor_id = record.desc.VendorId;
        output.device_id = record.desc.DeviceId;
        output.dedicated_video_memory = record.desc.DedicatedVideoMemory;
        output.dedicated_system_memory = record.desc.DedicatedSystemMemory;
        output.shared_system_memory = record.desc.SharedSystemMemory;
        output.platform_id =
            (static_cast<std::uint64_t>(
                 static_cast<std::uint32_t>(record.desc.AdapterLuid.HighPart))
             << 32U) |
            record.desc.AdapterLuid.LowPart;
        std::array<std::uint32_t, 4> uuid{
            record.desc.AdapterLuid.LowPart,
            static_cast<std::uint32_t>(record.desc.AdapterLuid.HighPart),
            record.desc.VendorId,
            record.desc.DeviceId};
        std::memcpy(output.device_uuid, uuid.data(), sizeof(output.device_uuid));
        output.capabilities = record.capabilities;
        copy_text(output.name, record.name);
        return Status::success();
    }

    Status create_device(const vita_gpu_device_desc &desc,
                         std::unique_ptr<BackendDevice> &output) const override;

private:
    void add_adapter(ComPtr<IDXGIAdapter1> adapter, bool warp) {
        if (!adapter || !adapter_usable(adapter.Get())) {
            return;
        }
        AdapterRecord record;
        record.adapter = std::move(adapter);
        if (FAILED(record.adapter->GetDesc1(&record.desc))) {
            return;
        }
        record.warp = warp;
        if (warp) {
            record.type = VITA_GPU_ADAPTER_SOFTWARE;
        } else if (record.desc.DedicatedVideoMemory != 0) {
            record.type = VITA_GPU_ADAPTER_DISCRETE;
        } else {
            record.type = VITA_GPU_ADAPTER_INTEGRATED;
        }
        record.capabilities = adapter_capabilities(record.adapter.Get());
        record.name = utf8(record.desc.Description);
        if (warp) {
            record.name += " [WARP software; explicit selection]";
        }
        adapters_.push_back(std::move(record));
    }

    void enumerate_adapters() {
        ComPtr<IDXGIFactory6> factory6;
        (void)factory_.As(&factory6);
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT hr =
                factory6
                    ? factory6->EnumAdapterByGpuPreference(
                          index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                          IID_PPV_ARGS(&adapter))
                    : factory_->EnumAdapters1(index, &adapter);
            if (hr == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(hr)) {
                break;
            }
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
                (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                add_adapter(std::move(adapter), false);
            }
        }

        ComPtr<IDXGIAdapter1> warp;
        if (SUCCEEDED(factory_->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) {
            add_adapter(std::move(warp), true);
        }
    }

    ComPtr<IDXGIFactory4> factory_;
    std::vector<AdapterRecord> adapters_;
    std::string initialization_error_;
    HRESULT initialization_hr_ = S_OK;

    friend class Dx12Device;
};

struct BufferObject {
    ComPtr<ID3D12Resource> resource;
    std::uint64_t size = 0;
    std::uint32_t stride = 0;
    vita_gpu_format format = VITA_GPU_FORMAT_UNKNOWN;
    vita_gpu_buffer_usage usage = 0;
    vita_gpu_memory_type memory_type = VITA_GPU_MEMORY_GPU_ONLY;
    vita_gpu_resource_state state = VITA_GPU_RESOURCE_STATE_UNDEFINED;
    vita_gpu_queue_type queue = VITA_GPU_QUEUE_GRAPHICS;
};

struct TextureObject {
    ComPtr<ID3D12Resource> resource;
    vita_gpu_texture_dimension dimension = VITA_GPU_TEXTURE_2D;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth_or_layers = 0;
    std::uint32_t mip_levels = 0;
    std::uint32_t sample_count = 1;
    vita_gpu_format format = VITA_GPU_FORMAT_UNKNOWN;
    vita_gpu_texture_usage usage = 0;
    vita_gpu_memory_type memory_type = VITA_GPU_MEMORY_GPU_ONLY;
    vita_gpu_resource_state state = VITA_GPU_RESOURCE_STATE_UNDEFINED;
    vita_gpu_queue_type queue = VITA_GPU_QUEUE_GRAPHICS;
    bool borrowed = false;
};

struct SamplerObject {
    std::uint32_t descriptor = 0;
};

struct RenderTargetObject {
    std::uint32_t descriptor = 0;
    NativeId texture;
};

struct DepthTargetObject {
    std::uint32_t descriptor = 0;
    NativeId texture;
    bool read_only_depth = false;
    bool read_only_stencil = false;
};

struct ShaderObject {
    vita_gpu_shader_stage stage = VITA_GPU_SHADER_VERTEX;
    std::vector<std::uint8_t> bytecode;
};

struct LayoutEntry {
    vita_gpu_binding_layout_entry source{};
    std::uint32_t heap_offset = 0;
};

struct BindingLayoutObject {
    std::uint32_t set_index = 0;
    std::vector<LayoutEntry> entries;
    std::uint32_t resource_count = 0;
    std::uint32_t sampler_count = 0;
};

struct BindingSetObject {
    NativeId layout;
    std::uint32_t resource_start = 0;
    std::uint32_t resource_count = 0;
    std::uint32_t sampler_start = 0;
    std::uint32_t sampler_count = 0;
};

struct PipelineSetBinding {
    NativeId layout;
    std::optional<std::uint32_t> resource_parameter;
    std::optional<std::uint32_t> sampler_parameter;
};

struct PipelineObject {
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12PipelineState> state;
    bool graphics = true;
    D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    std::unordered_map<std::uint32_t, std::uint32_t> vertex_strides;
    std::unordered_map<std::uint32_t, PipelineSetBinding> sets;
    std::optional<std::uint32_t> push_parameter;
    std::uint32_t push_constant_bytes = 0;
};

struct FenceObject {
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
};

struct ReadbackObject {
    ComPtr<ID3D12Resource> resource;
    std::uint64_t size = 0;
    bool mapped = false;
    const std::uint8_t *mapped_data = nullptr;
    std::uint32_t row_pitch = 0;
    std::uint32_t slice_pitch = 0;
};

struct SwapchainObject {
    ComPtr<IDXGISwapChain3> swapchain;
    HWND window = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t image_count = 0;
    vita_gpu_format format = VITA_GPU_FORMAT_UNKNOWN;
    vita_gpu_present_mode present_mode = VITA_GPU_PRESENT_FIFO;
    UINT flags = 0;
    std::vector<NativeId> textures;
    std::vector<NativeId> targets;
};

struct FrameContext {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> command_list;
    std::uint64_t fence_value = 0;
};

struct RootSignatureBuild {
    ComPtr<ID3D12RootSignature> signature;
    std::unordered_map<std::uint32_t, PipelineSetBinding> sets;
    std::optional<std::uint32_t> push_parameter;
};

class Dx12Device final : public BackendDevice {
public:
    ~Dx12Device() override {
        (void)wait_idle();
        for (auto &[id, fence] : fences_) {
            (void)id;
            if (fence.event != nullptr) {
                CloseHandle(fence.event);
                fence.event = nullptr;
            }
        }
        if (internal_event_ != nullptr) {
            CloseHandle(internal_event_);
            internal_event_ = nullptr;
        }
    }

    Status initialize(const ComPtr<IDXGIFactory4> &factory,
                      const AdapterRecord &adapter,
                      vita_gpu_device_flags flags) {
        factory_ = factory;
        validation_enabled_ =
            (flags & VITA_GPU_DEVICE_ENABLE_VALIDATION) != 0;

        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred_settings;
        if (SUCCEEDED(D3D12GetDebugInterface(
                IID_PPV_ARGS(&dred_settings)))) {
            dred_settings->SetAutoBreadcrumbsEnablement(
                D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred_settings->SetPageFaultEnablement(
                D3D12_DRED_ENABLEMENT_FORCED_ON);
        }

        if (validation_enabled_) {
            ComPtr<ID3D12Debug> debug;
            const HRESULT debug_hr =
                D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
            if (FAILED(debug_hr)) {
                return dx_status(
                    VITA_GPU_ERROR_BACKEND_UNAVAILABLE,
                    "DX12 validation was requested, but the D3D12 debug "
                    "layer is unavailable: " +
                        hr_text(debug_hr),
                    debug_hr);
            }
            debug->EnableDebugLayer();
        }

        HRESULT hr = D3D12CreateDevice(
            adapter.adapter.Get(), D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(&device_));
        if (FAILED(hr)) {
            return dx_status(VITA_GPU_ERROR_NATIVE,
                             "D3D12CreateDevice failed: " + hr_text(hr), hr);
        }

        if (validation_enabled_) {
            hr = device_.As(&info_queue_);
            if (FAILED(hr)) {
                return dx_status(
                    VITA_GPU_ERROR_BACKEND_UNAVAILABLE,
                    "DX12 validation was requested, but ID3D12InfoQueue is "
                    "unavailable: " +
                        hr_text(hr),
                    hr);
            }
            D3D12_INFO_QUEUE_FILTER filter{};
            std::array<D3D12_MESSAGE_SEVERITY, 2> denied{
                D3D12_MESSAGE_SEVERITY_INFO,
                D3D12_MESSAGE_SEVERITY_MESSAGE};
            filter.DenyList.NumSeverities =
                static_cast<UINT>(denied.size());
            filter.DenyList.pSeverityList = denied.data();
            (void)info_queue_->PushStorageFilter(&filter);
            info_queue_->ClearStoredMessages();
        }

        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        hr = device_->CreateCommandQueue(&queue_desc,
                                         IID_PPV_ARGS(&queue_));
        if (FAILED(hr)) {
            return native_failure("ID3D12Device::CreateCommandQueue", hr);
        }

        hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                  IID_PPV_ARGS(&internal_fence_));
        if (FAILED(hr)) {
            return native_failure("ID3D12Device::CreateFence", hr);
        }
        internal_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (internal_event_ == nullptr) {
            hr = HRESULT_FROM_WIN32(GetLastError());
            return native_failure("CreateEventW", hr);
        }

        const auto initialize_heap =
            [this](DescriptorHeap &heap, D3D12_DESCRIPTOR_HEAP_TYPE type,
                   std::uint32_t count, bool visible,
                   std::string_view name) -> Status {
            const HRESULT heap_hr =
                heap.initialize(device_.Get(), type, count, visible);
            if (FAILED(heap_hr)) {
                return native_failure(
                    "CreateDescriptorHeap(" + std::string(name) + ")",
                    heap_hr);
            }
            return Status::success();
        };
        Status status = initialize_heap(
            rtv_heap_, D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
            kRtvDescriptorCount, false, "RTV");
        if (!status.ok()) {
            return status;
        }
        status = initialize_heap(
            dsv_heap_, D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
            kDsvDescriptorCount, false, "DSV");
        if (!status.ok()) {
            return status;
        }
        status = initialize_heap(
            resource_heap_, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            kResourceDescriptorCount, true, "CBV/SRV/UAV");
        if (!status.ok()) {
            return status;
        }
        status = initialize_heap(
            sampler_heap_, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
            kSamplerDescriptorCount, true, "sampler");
        if (!status.ok()) {
            return status;
        }
        status = initialize_heap(
            sampler_staging_heap_, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
            kSamplerStagingDescriptorCount, false, "sampler staging");
        if (!status.ok()) {
            return status;
        }

        for (FrameContext &frame : frames_) {
            hr = device_->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&frame.allocator));
            if (FAILED(hr)) {
                return native_failure(
                    "ID3D12Device::CreateCommandAllocator", hr);
            }
            hr = device_->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, frame.allocator.Get(),
                nullptr, IID_PPV_ARGS(&frame.command_list));
            if (FAILED(hr)) {
                return native_failure(
                    "ID3D12Device::CreateCommandList", hr);
            }
            hr = frame.command_list->Close();
            if (FAILED(hr)) {
                return native_failure(
                    "ID3D12GraphicsCommandList::Close", hr);
            }
        }

        hr = device_->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&upload_allocator_));
        if (FAILED(hr)) {
            return native_failure(
                "ID3D12Device::CreateCommandAllocator(upload)", hr);
        }
        hr = device_->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, upload_allocator_.Get(),
            nullptr, IID_PPV_ARGS(&upload_list_));
        if (FAILED(hr)) {
            return native_failure(
                "ID3D12Device::CreateCommandList(upload)", hr);
        }
        hr = upload_list_->Close();
        if (FAILED(hr)) {
            return native_failure(
                "ID3D12GraphicsCommandList::Close(upload)", hr);
        }

        BOOL allow_tearing = FALSE;
        ComPtr<IDXGIFactory5> factory5;
        if (SUCCEEDED(factory_.As(&factory5)) &&
            SUCCEEDED(factory5->CheckFeatureSupport(
                DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing,
                sizeof(allow_tearing)))) {
            tearing_supported_ = allow_tearing != FALSE;
        }
        return check_validation_messages("DX12 device initialization");
    }

    vita_gpu_backend backend() const noexcept override {
        return VITA_GPU_BACKEND_DX12;
    }

    Status capabilities(vita_gpu_device_capabilities &output) override {
        output.flags =
            VITA_GPU_CAPABILITY_GRAPHICS | VITA_GPU_CAPABILITY_COMPUTE |
            VITA_GPU_CAPABILITY_TIMELINE_FENCES |
            VITA_GPU_CAPABILITY_DEBUG_MARKERS |
            VITA_GPU_CAPABILITY_BC_TEXTURES |
            VITA_GPU_CAPABILITY_ANISOTROPY;
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
        if (SUCCEEDED(device_->CheckFeatureSupport(
                D3D12_FEATURE_D3D12_OPTIONS1, &options1,
                sizeof(options1))) &&
            options1.Int64ShaderOps) {
            output.flags |= VITA_GPU_CAPABILITY_SHADER_INT64;
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS4 options4{};
        if (SUCCEEDED(device_->CheckFeatureSupport(
                D3D12_FEATURE_D3D12_OPTIONS4, &options4,
                sizeof(options4))) &&
            options4.Native16BitShaderOpsSupported) {
            output.flags |= VITA_GPU_CAPABILITY_SHADER_FLOAT16;
        }
        output.max_texture_1d = D3D12_REQ_TEXTURE1D_U_DIMENSION;
        output.max_texture_2d = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
        output.max_texture_3d = D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION;
        output.max_texture_array_layers =
            D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION;
        output.max_color_targets = D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT;
        output.max_binding_sets = 8;
        output.max_bindings_per_set = 256;
        output.max_push_constant_bytes = 256;
        output.max_sampler_anisotropy = D3D12_MAX_MAXANISOTROPY;
        output.constant_buffer_alignment =
            D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
        output.storage_buffer_alignment = 16;
        output.texture_row_pitch_alignment =
            D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
        UINT64 frequency = 0;
        if (SUCCEEDED(queue_->GetTimestampFrequency(&frequency)) &&
            frequency != 0) {
            const std::uint64_t divisor =
                std::gcd<std::uint64_t>(UINT64_C(1000000000), frequency);
            output.timestamp_period_numerator = static_cast<std::uint32_t>(
                UINT64_C(1000000000) / divisor);
            output.timestamp_period_denominator =
                static_cast<std::uint32_t>(frequency / divisor);
        } else {
            output.timestamp_period_numerator = 1;
            output.timestamp_period_denominator = 1;
        }
        return check_validation_messages("capabilities");
    }

    Status wait_idle() override {
        if (!queue_ || !internal_fence_) {
            return Status::success();
        }
        const std::uint64_t value = ++internal_fence_value_;
        HRESULT hr = queue_->Signal(internal_fence_.Get(), value);
        if (FAILED(hr)) {
            return native_failure("ID3D12CommandQueue::Signal(wait idle)", hr);
        }
        Status status = wait_internal(value);
        if (!status.ok()) {
            return status;
        }
        for (FrameContext &frame : frames_) {
            frame.fence_value = 0;
        }
        return check_validation_messages("wait idle");
    }

    Status create_swapchain(const vita_gpu_swapchain_desc &desc,
                            NativeId &output) override;
    Status resize_swapchain(NativeId swapchain, std::uint32_t width,
                            std::uint32_t height) override;
    Status acquire_swapchain(NativeId swapchain,
                             BackendSwapchainImage &output) override;
    Status present_swapchain(
        NativeId swapchain,
        const vita_gpu_present_desc &desc) override;

    Status create_buffer(const vita_gpu_buffer_desc &desc,
                         NativeId &output) override;
    Status create_texture(const vita_gpu_texture_desc &desc,
                          NativeId &output) override;
    Status create_sampler(const vita_gpu_sampler_desc &desc,
                          NativeId &output) override;
    Status create_render_target(
        const vita_gpu_render_target_desc &desc, NativeId texture,
        NativeId &output) override;
    Status create_depth_target(
        const vita_gpu_depth_target_desc &desc, NativeId texture,
        NativeId &output) override;
    Status create_shader_module(
        const vita_gpu_shader_module_desc &desc,
        NativeId &output) override;
    Status create_binding_layout(
        const vita_gpu_binding_layout_desc &desc,
        NativeId &output) override;
    Status create_binding_set(
        const vita_gpu_binding_set_desc &desc, NativeId layout,
        NativeId &output) override;
    Status update_binding_set(
        NativeId set,
        std::span<const BackendBindingWrite> writes) override;
    Status create_graphics_pipeline(
        const BackendGraphicsPipelineDesc &desc,
        NativeId &output) override;
    Status create_compute_pipeline(
        const BackendComputePipelineDesc &desc,
        NativeId &output) override;
    Status create_readback(const vita_gpu_readback_desc &desc,
                           NativeId &output) override;
    Status create_fence(const vita_gpu_fence_desc &desc,
                        NativeId &output) override;
    Status destroy_object(ObjectKind kind, NativeId object) override;

    Status submit(
        vita_gpu_queue_type queue,
        std::span<const RecordedCommandList *const> command_lists,
        std::span<const BackendFencePoint> waits,
        BackendFencePoint signal) override;
    Status fence_completed_value(NativeId fence,
                                 std::uint64_t &value) override;
    Status fence_wait(NativeId fence, std::uint64_t value,
                      std::uint64_t timeout_nanoseconds) override;

    Status map_readback(NativeId readback,
                        BackendReadbackMap &output) override;
    Status unmap_readback(NativeId readback) override;
    Status debug_capture_begin(
        const vita_gpu_debug_capture_desc &) override {
        return Status::error(
            VITA_GPU_ERROR_UNSUPPORTED,
            "DX12 debug capture requires external PIX integration; "
            "readback capture remains available",
            VITA_GPU_BACKEND_DX12);
    }
    Status debug_capture_end() override {
        return Status::error(
            VITA_GPU_ERROR_UNSUPPORTED,
            "DX12 debug capture is not active because PIX integration is "
            "unsupported",
            VITA_GPU_BACKEND_DX12);
    }

private:
    NativeId allocate_id() { return NativeId{next_id_++}; }
    Status native_failure(std::string_view operation, HRESULT hr) const;
    Status check_validation_messages(std::string_view operation);
    Status wait_internal(std::uint64_t value);
    Status execute_immediate(
        const std::function<Status(ID3D12GraphicsCommandList *)> &record);
    void set_name(ID3D12Object *object, const char *label);

    Status create_swapchain_images(SwapchainObject &swapchain);
    void release_swapchain_images(SwapchainObject &swapchain);
    Status upload_texture(TextureObject &texture,
                          const vita_gpu_texture_desc &desc);
    Status build_root_signature(
        std::span<const NativeId> layouts,
        std::uint32_t push_constant_bytes, bool graphics,
        RootSignatureBuild &output);
    Status translate_commands(
        ID3D12GraphicsCommandList *native,
        std::span<const RecordedCommandList *const> command_lists,
        std::map<std::pair<ObjectKind, std::uint64_t>,
                 std::pair<vita_gpu_resource_state, vita_gpu_queue_type>>
            &state_updates,
        std::map<std::uint64_t, std::pair<std::uint32_t, std::uint32_t>>
            &readback_updates);

    ComPtr<IDXGIFactory4> factory_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12InfoQueue> info_queue_;
    ComPtr<ID3D12Fence> internal_fence_;
    HANDLE internal_event_ = nullptr;
    std::uint64_t internal_fence_value_ = 0;
    std::array<FrameContext, kFrameContextCount> frames_;
    std::uint32_t next_frame_ = 0;
    ComPtr<ID3D12CommandAllocator> upload_allocator_;
    ComPtr<ID3D12GraphicsCommandList> upload_list_;
    DescriptorHeap rtv_heap_;
    DescriptorHeap dsv_heap_;
    DescriptorHeap resource_heap_;
    DescriptorHeap sampler_heap_;
    DescriptorHeap sampler_staging_heap_;
    bool validation_enabled_ = false;
    bool tearing_supported_ = false;
    std::uint64_t next_id_ = 1;

    std::unordered_map<std::uint64_t, SwapchainObject> swapchains_;
    std::unordered_map<std::uint64_t, BufferObject> buffers_;
    std::unordered_map<std::uint64_t, TextureObject> textures_;
    std::unordered_map<std::uint64_t, SamplerObject> samplers_;
    std::unordered_map<std::uint64_t, RenderTargetObject> render_targets_;
    std::unordered_map<std::uint64_t, DepthTargetObject> depth_targets_;
    std::unordered_map<std::uint64_t, ShaderObject> shaders_;
    std::unordered_map<std::uint64_t, BindingLayoutObject> binding_layouts_;
    std::unordered_map<std::uint64_t, BindingSetObject> binding_sets_;
    std::unordered_map<std::uint64_t, PipelineObject> pipelines_;
    std::unordered_map<std::uint64_t, FenceObject> fences_;
    std::unordered_map<std::uint64_t, ReadbackObject> readbacks_;
};

Status Dx12Device::native_failure(std::string_view operation,
                                  HRESULT hr) const {
    HRESULT removed_reason = device_ ? device_->GetDeviceRemovedReason()
                                     : S_OK;
    const bool removed =
        hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG ||
        hr == DXGI_ERROR_DEVICE_RESET || FAILED(removed_reason);
    if (!removed) {
        return dx_status(VITA_GPU_ERROR_NATIVE,
                         std::string(operation) + " failed: " + hr_text(hr),
                         hr);
    }

    if (SUCCEEDED(removed_reason)) {
        removed_reason = hr;
    }
    std::ostringstream message;
    message << operation << " detected a removed DX12 device; call HRESULT="
            << hr_text(hr) << ", GetDeviceRemovedReason="
            << hr_text(removed_reason);

    ComPtr<ID3D12DeviceRemovedExtendedData> dred;
    if (device_ && SUCCEEDED(device_.As(&dred))) {
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs{};
        if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs)) &&
            breadcrumbs.pHeadAutoBreadcrumbNode != nullptr) {
            const D3D12_AUTO_BREADCRUMB_NODE *node =
                breadcrumbs.pHeadAutoBreadcrumbNode;
            message << "; DRED breadcrumb";
            if (node->pCommandListDebugNameA != nullptr) {
                message << " list='" << node->pCommandListDebugNameA << "'";
            }
            if (node->pLastBreadcrumbValue != nullptr) {
                message << " completed=" << *node->pLastBreadcrumbValue
                        << "/" << node->BreadcrumbCount;
            }
        }
        D3D12_DRED_PAGE_FAULT_OUTPUT page_fault{};
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&page_fault)) &&
            page_fault.PageFaultVA != 0) {
            message << "; DRED page-fault VA=0x" << std::hex
                    << page_fault.PageFaultVA;
        }
    }
    return Status::error(VITA_GPU_ERROR_DEVICE_LOST, message.str(),
                         VITA_GPU_BACKEND_DX12,
                         static_cast<std::int64_t>(removed_reason));
}

Status Dx12Device::check_validation_messages(std::string_view operation) {
    if (!validation_enabled_ || !info_queue_) {
        return Status::success();
    }
    const UINT64 count = info_queue_->GetNumStoredMessages();
    std::string failure;
    std::int64_t message_id = 0;
    for (UINT64 index = 0; index < count; ++index) {
        SIZE_T length = 0;
        if (FAILED(info_queue_->GetMessage(index, nullptr, &length)) ||
            length == 0) {
            continue;
        }
        std::vector<std::uint8_t> storage(length);
        auto *message =
            reinterpret_cast<D3D12_MESSAGE *>(storage.data());
        if (FAILED(info_queue_->GetMessage(index, message, &length))) {
            continue;
        }
        if (message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ||
            message->Severity == D3D12_MESSAGE_SEVERITY_ERROR) {
            failure = message->pDescription != nullptr
                          ? message->pDescription
                          : "D3D12 validation emitted an error";
            message_id = static_cast<std::int64_t>(message->ID);
            break;
        }
    }
    info_queue_->ClearStoredMessages();
    if (!failure.empty()) {
        return Status::error(
            VITA_GPU_ERROR_NATIVE,
            std::string(operation) +
                " failed DX12 validation: " + failure,
            VITA_GPU_BACKEND_DX12, message_id);
    }
    return Status::success();
}

Status Dx12Device::wait_internal(std::uint64_t value) {
    if (internal_fence_->GetCompletedValue() >= value) {
        return Status::success();
    }
    HRESULT hr =
        internal_fence_->SetEventOnCompletion(value, internal_event_);
    if (FAILED(hr)) {
        return native_failure("ID3D12Fence::SetEventOnCompletion", hr);
    }
    const DWORD result = WaitForSingleObject(internal_event_, INFINITE);
    if (result != WAIT_OBJECT_0) {
        hr = result == WAIT_FAILED
                 ? HRESULT_FROM_WIN32(GetLastError())
                 : E_FAIL;
        return native_failure("WaitForSingleObject(internal fence)", hr);
    }
    return Status::success();
}

Status Dx12Device::execute_immediate(
    const std::function<Status(ID3D12GraphicsCommandList *)> &record) {
    HRESULT hr = upload_allocator_->Reset();
    if (FAILED(hr)) {
        return native_failure("ID3D12CommandAllocator::Reset(upload)", hr);
    }
    hr = upload_list_->Reset(upload_allocator_.Get(), nullptr);
    if (FAILED(hr)) {
        return native_failure(
            "ID3D12GraphicsCommandList::Reset(upload)", hr);
    }
    Status status = record(upload_list_.Get());
    if (!status.ok()) {
        (void)upload_list_->Close();
        return status;
    }
    hr = upload_list_->Close();
    if (FAILED(hr)) {
        return native_failure(
            "ID3D12GraphicsCommandList::Close(upload)", hr);
    }
    ID3D12CommandList *lists[]{upload_list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    const std::uint64_t value = ++internal_fence_value_;
    hr = queue_->Signal(internal_fence_.Get(), value);
    if (FAILED(hr)) {
        return native_failure(
            "ID3D12CommandQueue::Signal(upload)", hr);
    }
    status = wait_internal(value);
    if (!status.ok()) {
        return status;
    }
    return check_validation_messages("immediate upload");
}

void Dx12Device::set_name(ID3D12Object *object, const char *label) {
    if (object == nullptr || label == nullptr || *label == '\0') {
        return;
    }
    const std::wstring name = wide(label);
    if (!name.empty()) {
        (void)object->SetName(name.c_str());
    }
}

Status Dx12Device::create_buffer(const vita_gpu_buffer_desc &desc,
                                 NativeId &output) {
    D3D12_HEAP_TYPE heap_type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_STATES native_state = resource_state(desc.initial_state);
    if (desc.memory_type != VITA_GPU_MEMORY_GPU_ONLY &&
        (desc.usage & VITA_GPU_BUFFER_STORAGE) != 0) {
        return Status::error(
            VITA_GPU_ERROR_UNSUPPORTED,
            "DX12 storage buffers require GPU-only memory; upload and "
            "readback heaps cannot expose UAV descriptors",
            VITA_GPU_BACKEND_DX12);
    }
    if (desc.memory_type == VITA_GPU_MEMORY_CPU_UPLOAD) {
        const vita_gpu_resource_state unsupported_states =
            VITA_GPU_RESOURCE_STATE_COPY_DEST |
            VITA_GPU_RESOURCE_STATE_SHADER_WRITE;
        if ((desc.initial_state & unsupported_states) != 0) {
            return Status::error(
                VITA_GPU_ERROR_UNSUPPORTED,
                "DX12 upload-heap buffers cannot enter COPY_DEST or "
                "SHADER_WRITE state",
                VITA_GPU_BACKEND_DX12);
        }
        heap_type = D3D12_HEAP_TYPE_UPLOAD;
        native_state = D3D12_RESOURCE_STATE_GENERIC_READ;
    } else if (desc.memory_type == VITA_GPU_MEMORY_CPU_READBACK) {
        if (desc.initial_state != VITA_GPU_RESOURCE_STATE_COPY_DEST &&
            desc.initial_state != VITA_GPU_RESOURCE_STATE_COMMON) {
            return Status::error(
                VITA_GPU_ERROR_UNSUPPORTED,
                "DX12 readback-heap buffers support only COMMON or "
                "COPY_DEST contract state",
                VITA_GPU_BACKEND_DX12);
        }
        heap_type = D3D12_HEAP_TYPE_READBACK;
        native_state = D3D12_RESOURCE_STATE_COPY_DEST;
    } else if (desc.initial_data != nullptr) {
        native_state = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    D3D12_RESOURCE_FLAGS resource_flags = D3D12_RESOURCE_FLAG_NONE;
    if ((desc.usage & VITA_GPU_BUFFER_STORAGE) != 0 &&
        desc.memory_type == VITA_GPU_MEMORY_GPU_ONLY) {
        resource_flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    const D3D12_HEAP_PROPERTIES properties = heap_properties(heap_type);
    const D3D12_RESOURCE_DESC resource_desc =
        buffer_resource_desc(desc.byte_size, resource_flags);
    BufferObject buffer;
    HRESULT hr = device_->CreateCommittedResource(
        &properties, D3D12_HEAP_FLAG_NONE, &resource_desc, native_state,
        nullptr, IID_PPV_ARGS(&buffer.resource));
    if (FAILED(hr)) {
        return native_failure("CreateCommittedResource(buffer)", hr);
    }
    set_name(buffer.resource.Get(), desc.label);

    if (desc.initial_data != nullptr) {
        if (heap_type == D3D12_HEAP_TYPE_UPLOAD) {
            void *mapped = nullptr;
            D3D12_RANGE read_range{0, 0};
            hr = buffer.resource->Map(0, &read_range, &mapped);
            if (FAILED(hr)) {
                return native_failure("ID3D12Resource::Map(upload buffer)",
                                      hr);
            }
            std::memcpy(mapped, desc.initial_data,
                        static_cast<std::size_t>(desc.initial_data_size));
            D3D12_RANGE written_range{
                0, static_cast<SIZE_T>(desc.initial_data_size)};
            buffer.resource->Unmap(0, &written_range);
        } else {
            ComPtr<ID3D12Resource> upload;
            const D3D12_HEAP_PROPERTIES upload_properties =
                heap_properties(D3D12_HEAP_TYPE_UPLOAD);
            const D3D12_RESOURCE_DESC upload_desc =
                buffer_resource_desc(desc.initial_data_size);
            hr = device_->CreateCommittedResource(
                &upload_properties, D3D12_HEAP_FLAG_NONE, &upload_desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&upload));
            if (FAILED(hr)) {
                return native_failure(
                    "CreateCommittedResource(buffer upload)", hr);
            }
            void *mapped = nullptr;
            D3D12_RANGE read_range{0, 0};
            hr = upload->Map(0, &read_range, &mapped);
            if (FAILED(hr)) {
                return native_failure(
                    "ID3D12Resource::Map(buffer upload)", hr);
            }
            std::memcpy(mapped, desc.initial_data,
                        static_cast<std::size_t>(desc.initial_data_size));
            D3D12_RANGE written_range{
                0, static_cast<SIZE_T>(desc.initial_data_size)};
            upload->Unmap(0, &written_range);
            const D3D12_RESOURCE_STATES final_state =
                resource_state(desc.initial_state);
            Status status = execute_immediate(
                [&](ID3D12GraphicsCommandList *list) {
                    list->CopyBufferRegion(
                        buffer.resource.Get(), 0, upload.Get(), 0,
                        desc.initial_data_size);
                    if (final_state != D3D12_RESOURCE_STATE_COPY_DEST) {
                        D3D12_RESOURCE_BARRIER barrier{};
                        barrier.Type =
                            D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        barrier.Transition.pResource =
                            buffer.resource.Get();
                        barrier.Transition.Subresource =
                            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        barrier.Transition.StateBefore =
                            D3D12_RESOURCE_STATE_COPY_DEST;
                        barrier.Transition.StateAfter = final_state;
                        list->ResourceBarrier(1, &barrier);
                    }
                    return Status::success();
                });
            if (!status.ok()) {
                return status;
            }
        }
    }

    buffer.size = desc.byte_size;
    buffer.stride = desc.element_stride;
    buffer.format = desc.element_format;
    buffer.usage = desc.usage;
    buffer.memory_type = desc.memory_type;
    buffer.state = desc.initial_state;
    const NativeId id = allocate_id();
    buffers_.emplace(id.value, std::move(buffer));
    output = id;
    return check_validation_messages("create buffer");
}

Status Dx12Device::upload_texture(TextureObject &texture,
                                  const vita_gpu_texture_desc &desc) {
    if (depth_format(desc.format)) {
        return Status::error(
            VITA_GPU_ERROR_UNSUPPORTED,
            "DX12 initial data for depth/stencil textures is unsupported; "
            "upload through an application-defined copy path",
            VITA_GPU_BACKEND_DX12);
    }

    const D3D12_RESOURCE_DESC native_desc = texture.resource->GetDesc();
    const UINT count = desc.initial_data_count;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(count);
    std::vector<UINT> row_counts(count);
    std::vector<UINT64> row_sizes(count);
    UINT64 upload_size = 0;
    device_->GetCopyableFootprints(
        &native_desc, 0, count, 0, layouts.data(), row_counts.data(),
        row_sizes.data(), &upload_size);
    if (upload_size == 0) {
        return Status::error(
            VITA_GPU_ERROR_INVALID_ARGUMENT,
            "DX12 could not calculate texture upload footprints",
            VITA_GPU_BACKEND_DX12);
    }

    ComPtr<ID3D12Resource> upload;
    const D3D12_HEAP_PROPERTIES properties =
        heap_properties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC upload_desc =
        buffer_resource_desc(upload_size);
    HRESULT hr = device_->CreateCommittedResource(
        &properties, D3D12_HEAP_FLAG_NONE, &upload_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&upload));
    if (FAILED(hr)) {
        return native_failure(
            "CreateCommittedResource(texture upload)", hr);
    }

    std::uint8_t *mapped = nullptr;
    D3D12_RANGE read_range{0, 0};
    hr = upload->Map(0, &read_range,
                     reinterpret_cast<void **>(&mapped));
    if (FAILED(hr)) {
        return native_failure("ID3D12Resource::Map(texture upload)", hr);
    }
    for (UINT index = 0; index < count; ++index) {
        const vita_gpu_subresource_data &source =
            desc.initial_data[index];
        const UINT64 required =
            (static_cast<UINT64>(layouts[index].Footprint.Depth) - 1U) *
                source.slice_pitch +
            (static_cast<UINT64>(row_counts[index]) - 1U) *
                source.row_pitch +
            row_sizes[index];
        if (source.row_pitch < row_sizes[index] ||
            source.data_size < required) {
            upload->Unmap(0, nullptr);
            return Status::error(
                VITA_GPU_ERROR_INVALID_ARGUMENT,
                "texture initial-data pitches or size are smaller than the "
                "DX12 copy footprint",
                VITA_GPU_BACKEND_DX12);
        }
        std::uint8_t *destination =
            mapped + layouts[index].Offset;
        for (UINT z = 0; z < layouts[index].Footprint.Depth; ++z) {
            for (UINT row = 0; row < row_counts[index]; ++row) {
                const std::uint8_t *source_row =
                    source.data +
                    static_cast<std::size_t>(z) * source.slice_pitch +
                    static_cast<std::size_t>(row) * source.row_pitch;
                std::uint8_t *destination_row =
                    destination +
                    static_cast<std::size_t>(z) *
                        layouts[index].Footprint.RowPitch *
                        row_counts[index] +
                    static_cast<std::size_t>(row) *
                        layouts[index].Footprint.RowPitch;
                std::memcpy(destination_row, source_row,
                            static_cast<std::size_t>(row_sizes[index]));
            }
        }
    }
    D3D12_RANGE written_range{0, static_cast<SIZE_T>(upload_size)};
    upload->Unmap(0, &written_range);

    const D3D12_RESOURCE_STATES final_state =
        resource_state(desc.initial_state);
    return execute_immediate([&](ID3D12GraphicsCommandList *list) {
        for (UINT index = 0; index < count; ++index) {
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = texture.resource.Get();
            destination.Type =
                D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = index;
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = upload.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = layouts[index];
            list->CopyTextureRegion(&destination, 0, 0, 0, &source,
                                    nullptr);
        }
        if (final_state != D3D12_RESOURCE_STATE_COPY_DEST) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = texture.resource.Get();
            barrier.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore =
                D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter = final_state;
            list->ResourceBarrier(1, &barrier);
        }
        return Status::success();
    });
}

Status Dx12Device::create_texture(const vita_gpu_texture_desc &desc,
                                  NativeId &output) {
    if (desc.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        desc.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        desc.depth_or_layers >
            std::numeric_limits<std::uint16_t>::max() ||
        desc.mip_levels > std::numeric_limits<std::uint16_t>::max()) {
        return Status::error(
            VITA_GPU_ERROR_UNSUPPORTED,
            "texture dimensions exceed Direct3D 12 resource limits",
            VITA_GPU_BACKEND_DX12);
    }

    D3D12_RESOURCE_DESC resource_desc{};
    switch (desc.dimension) {
    case VITA_GPU_TEXTURE_1D:
        resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
        break;
    case VITA_GPU_TEXTURE_2D:
        resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        break;
    case VITA_GPU_TEXTURE_3D:
        resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        break;
    default:
        return Status::error(VITA_GPU_ERROR_INVALID_ARGUMENT,
                             "invalid DX12 texture dimension",
                             VITA_GPU_BACKEND_DX12);
    }
    resource_desc.Width = desc.width;
    resource_desc.Height = desc.height;
    resource_desc.DepthOrArraySize =
        static_cast<UINT16>(desc.depth_or_layers);
    resource_desc.MipLevels = static_cast<UINT16>(desc.mip_levels);
    resource_desc.Format = resource_format(desc.format);
    resource_desc.SampleDesc = {desc.sample_count, 0};
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if ((desc.usage & VITA_GPU_TEXTURE_RENDER_TARGET) != 0) {
        resource_desc.Flags |=
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }
    if ((desc.usage & VITA_GPU_TEXTURE_DEPTH_TARGET) != 0) {
        resource_desc.Flags |=
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    }
    if ((desc.usage & VITA_GPU_TEXTURE_STORAGE) != 0) {
        if (desc.sample_count > 1) {
            return Status::error(
                VITA_GPU_ERROR_UNSUPPORTED,
                "DX12 multisampled storage textures are unsupported",
                VITA_GPU_BACKEND_DX12);
        }
        resource_desc.Flags |=
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }

    D3D12_CLEAR_VALUE clear_value{};
    D3D12_CLEAR_VALUE *clear_pointer = nullptr;
    if (depth_format(desc.format)) {
        clear_value.Format = dxgi_format(desc.format);
        clear_value.DepthStencil = {1.0F, 0};
        clear_pointer = &clear_value;
    } else if ((desc.usage & VITA_GPU_TEXTURE_RENDER_TARGET) != 0) {
        clear_value.Format = dxgi_format(desc.format);
        clear_pointer = &clear_value;
    }

    const D3D12_HEAP_PROPERTIES properties =
        heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_RESOURCE_STATES initial_state =
        desc.initial_data_count != 0
            ? D3D12_RESOURCE_STATE_COPY_DEST
            : resource_state(desc.initial_state);
    TextureObject texture;
    HRESULT hr = device_->CreateCommittedResource(
        &properties, D3D12_HEAP_FLAG_NONE, &resource_desc, initial_state,
        clear_pointer, IID_PPV_ARGS(&texture.resource));
    if (FAILED(hr)) {
        return native_failure("CreateCommittedResource(texture)", hr);
    }
    set_name(texture.resource.Get(), desc.label);
    texture.dimension = desc.dimension;
    texture.width = desc.width;
    texture.height = desc.height;
    texture.depth_or_layers = desc.depth_or_layers;
    texture.mip_levels = desc.mip_levels;
    texture.sample_count = desc.sample_count;
    texture.format = desc.format;
    texture.usage = desc.usage;
    texture.memory_type = desc.memory_type;
    texture.state = desc.initial_state;

    if (desc.initial_data_count != 0) {
        Status status = upload_texture(texture, desc);
        if (!status.ok()) {
            return status;
        }
    }
    const NativeId id = allocate_id();
    textures_.emplace(id.value, std::move(texture));
    output = id;
    return check_validation_messages("create texture");
}

Status Dx12Device::create_sampler(const vita_gpu_sampler_desc &desc,
                                  NativeId &output) {
    const auto descriptor = sampler_staging_heap_.allocate();
    if (!descriptor.has_value()) {
        return Status::error(
            VITA_GPU_ERROR_OUT_OF_MEMORY,
            "DX12 sampler staging descriptor heap is exhausted",
            VITA_GPU_BACKEND_DX12);
    }
    D3D12_SAMPLER_DESC native{};
    native.Filter = sampler_filter(desc);
    native.AddressU = address_mode(desc.address_u);
    native.AddressV = address_mode(desc.address_v);
    native.AddressW = address_mode(desc.address_w);
    native.MipLODBias = desc.mip_lod_bias;
    native.MaxAnisotropy =
        std::min(desc.max_anisotropy,
                 static_cast<std::uint32_t>(D3D12_MAX_MAXANISOTROPY));
    native.ComparisonFunc = comparison(desc.compare_op);
    std::copy(std::begin(desc.border_color), std::end(desc.border_color),
              native.BorderColor);
    native.MinLOD = desc.min_lod;
    native.MaxLOD = desc.max_lod;
    device_->CreateSampler(
        &native, sampler_staging_heap_.cpu(*descriptor));
    const NativeId id = allocate_id();
    samplers_.emplace(id.value, SamplerObject{*descriptor});
    output = id;
    return check_validation_messages("create sampler");
}

Status Dx12Device::create_render_target(
    const vita_gpu_render_target_desc &desc, NativeId texture_id,
    NativeId &output) {
    const auto texture = textures_.find(texture_id.value);
    if (texture == textures_.end()) {
        return Status::error(VITA_GPU_ERROR_INVALID_HANDLE,
                             "DX12 render-target texture is missing",
                             VITA_GPU_BACKEND_DX12);
    }
    const auto descriptor = rtv_heap_.allocate();
    if (!descriptor.has_value()) {
        return Status::error(VITA_GPU_ERROR_OUT_OF_MEMORY,
                             "DX12 RTV descriptor heap is exhausted",
                             VITA_GPU_BACKEND_DX12);
    }
    const TextureObject &source = texture->second;
    D3D12_RENDER_TARGET_VIEW_DESC view{};
    view.Format = dxgi_format(desc.format);
    if (source.dimension == VITA_GPU_TEXTURE_1D) {
        if (source.depth_or_layers == 1) {
            view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1D;
            view.Texture1D.MipSlice = desc.mip_level;
        } else {
            view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
            view.Texture1DArray.MipSlice = desc.mip_level;
            view.Texture1DArray.FirstArraySlice =
                desc.first_array_layer;
            view.Texture1DArray.ArraySize = desc.array_layer_count;
        }
    } else if (source.dimension == VITA_GPU_TEXTURE_3D) {
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
        view.Texture3D.MipSlice = desc.mip_level;
        view.Texture3D.FirstWSlice = desc.first_array_layer;
        view.Texture3D.WSize = desc.array_layer_count;
    } else if (source.sample_count > 1) {
        if (source.depth_or_layers == 1) {
            view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
        } else {
            view.ViewDimension =
                D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
            view.Texture2DMSArray.FirstArraySlice =
                desc.first_array_layer;
            view.Texture2DMSArray.ArraySize = desc.array_layer_count;
        }
    } else if (source.depth_or_layers == 1) {
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipSlice = desc.mip_level;
        view.Texture2D.PlaneSlice = 0;
    } else {
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.MipSlice = desc.mip_level;
        view.Texture2DArray.FirstArraySlice =
            desc.first_array_layer;
        view.Texture2DArray.ArraySize = desc.array_layer_count;
        view.Texture2DArray.PlaneSlice = 0;
    }
    device_->CreateRenderTargetView(
        source.resource.Get(), &view, rtv_heap_.cpu(*descriptor));
    const NativeId id = allocate_id();
    render_targets_.emplace(
        id.value,
        RenderTargetObject{*descriptor, texture_id});
    output = id;
    return check_validation_messages("create render target");
}

Status Dx12Device::create_depth_target(
    const vita_gpu_depth_target_desc &desc, NativeId texture_id,
    NativeId &output) {
    const auto texture = textures_.find(texture_id.value);
    if (texture == textures_.end()) {
        return Status::error(VITA_GPU_ERROR_INVALID_HANDLE,
                             "DX12 depth-target texture is missing",
                             VITA_GPU_BACKEND_DX12);
    }
    const auto descriptor = dsv_heap_.allocate();
    if (!descriptor.has_value()) {
        return Status::error(VITA_GPU_ERROR_OUT_OF_MEMORY,
                             "DX12 DSV descriptor heap is exhausted",
                             VITA_GPU_BACKEND_DX12);
    }
    const TextureObject &source = texture->second;
    D3D12_DEPTH_STENCIL_VIEW_DESC view{};
    view.Format = dxgi_format(desc.format);
    if (desc.read_only_depth != 0) {
        view.Flags |= D3D12_DSV_FLAG_READ_ONLY_DEPTH;
    }
    if (desc.read_only_stencil != 0 &&
        (desc.format == VITA_GPU_FORMAT_D24_UNORM_S8_UINT ||
         desc.format == VITA_GPU_FORMAT_D32_FLOAT_S8_UINT)) {
        view.Flags |= D3D12_DSV_FLAG_READ_ONLY_STENCIL;
    }
    if (source.dimension == VITA_GPU_TEXTURE_1D) {
        if (source.depth_or_layers == 1) {
            view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1D;
            view.Texture1D.MipSlice = desc.mip_level;
        } else {
            view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY;
            view.Texture1DArray.MipSlice = desc.mip_level;
            view.Texture1DArray.FirstArraySlice =
                desc.first_array_layer;
            view.Texture1DArray.ArraySize = desc.array_layer_count;
        }
    } else if (source.sample_count > 1) {
        if (source.depth_or_layers == 1) {
            view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
        } else {
            view.ViewDimension =
                D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
            view.Texture2DMSArray.FirstArraySlice =
                desc.first_array_layer;
            view.Texture2DMSArray.ArraySize = desc.array_layer_count;
        }
    } else if (source.depth_or_layers == 1) {
        view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipSlice = desc.mip_level;
    } else {
        view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.MipSlice = desc.mip_level;
        view.Texture2DArray.FirstArraySlice =
            desc.first_array_layer;
        view.Texture2DArray.ArraySize = desc.array_layer_count;
    }
    device_->CreateDepthStencilView(
        source.resource.Get(), &view, dsv_heap_.cpu(*descriptor));
    const NativeId id = allocate_id();
    depth_targets_.emplace(
        id.value,
        DepthTargetObject{*descriptor, texture_id,
                          desc.read_only_depth != 0,
                          desc.read_only_stencil != 0});
    output = id;
    return check_validation_messages("create depth target");
}

Status Dx12Device::create_shader_module(
    const vita_gpu_shader_module_desc &desc, NativeId &output) {
    if (desc.input == VITA_GPU_SHADER_INPUT_HLSL) {
        return Status::error(
            VITA_GPU_ERROR_SHADER_COMPILER_UNAVAILABLE,
            "DX12 runtime HLSL compilation is unavailable; compile the "
            "framework HLSL deterministically with pinned DXC "
            "1.8.2505.32 and provide a DXIL binary module",
            VITA_GPU_BACKEND_DX12);
    }
    if (desc.binary == nullptr ||
        desc.binary->target != VITA_GPU_SHADER_TARGET_DXIL) {
        return Status::error(
            VITA_GPU_ERROR_INVALID_ARGUMENT,
            "DX12 shader modules require a DXIL binary",
            VITA_GPU_BACKEND_DX12);
    }
    ShaderObject shader;
    shader.stage = desc.binary->stage;
    shader.bytecode.assign(
        desc.binary->data,
        desc.binary->data + desc.binary->data_size);
    const NativeId id = allocate_id();
    shaders_.emplace(id.value, std::move(shader));
    output = id;
    return Status::success();
}

Status Dx12Device::create_readback(const vita_gpu_readback_desc &desc,
                                   NativeId &output) {
    ReadbackObject readback;
    const D3D12_HEAP_PROPERTIES properties =
        heap_properties(D3D12_HEAP_TYPE_READBACK);
    const D3D12_RESOURCE_DESC resource_desc =
        buffer_resource_desc(desc.byte_size);
    const HRESULT hr = device_->CreateCommittedResource(
        &properties, D3D12_HEAP_FLAG_NONE, &resource_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&readback.resource));
    if (FAILED(hr)) {
        return native_failure("CreateCommittedResource(readback)", hr);
    }
    set_name(readback.resource.Get(), desc.label);
    readback.size = desc.byte_size;
    const NativeId id = allocate_id();
    readbacks_.emplace(id.value, std::move(readback));
    output = id;
    return check_validation_messages("create readback");
}

Status Dx12Device::create_fence(const vita_gpu_fence_desc &desc,
                                NativeId &output) {
    FenceObject fence;
    HRESULT hr = device_->CreateFence(
        desc.initial_value, D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(&fence.fence));
    if (FAILED(hr)) {
        return native_failure("ID3D12Device::CreateFence", hr);
    }
    fence.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (fence.event == nullptr) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        return native_failure("CreateEventW(fence)", hr);
    }
    set_name(fence.fence.Get(), desc.label);
    const NativeId id = allocate_id();
    fences_.emplace(id.value, std::move(fence));
    output = id;
    return check_validation_messages("create fence");
}

Status Dx12Device::create_binding_layout(
    const vita_gpu_binding_layout_desc &desc, NativeId &output) {
    BindingLayoutObject layout;
    layout.set_index = desc.set_index;
    layout.entries.reserve(desc.entry_count);
    for (std::uint32_t index = 0; index < desc.entry_count; ++index) {
        const vita_gpu_binding_layout_entry &entry = desc.entries[index];
        LayoutEntry translated;
        translated.source = entry;
        if (entry.type == VITA_GPU_BINDING_SAMPLER) {
            translated.heap_offset = layout.sampler_count;
            layout.sampler_count += entry.array_count;
        } else {
            translated.heap_offset = layout.resource_count;
            layout.resource_count += entry.array_count;
        }
        layout.entries.push_back(translated);
    }
    if (layout.resource_count > kResourceDescriptorCount ||
        layout.sampler_count > kSamplerDescriptorCount) {
        return Status::error(
            VITA_GPU_ERROR_UNSUPPORTED,
            "binding layout exceeds the DX12 shader-visible descriptor "
            "heap capacity",
            VITA_GPU_BACKEND_DX12);
    }
    const NativeId id = allocate_id();
    binding_layouts_.emplace(id.value, std::move(layout));
    output = id;
    return Status::success();
}

Status Dx12Device::create_binding_set(
    const vita_gpu_binding_set_desc &, NativeId layout_id,
    NativeId &output) {
    const auto layout = binding_layouts_.find(layout_id.value);
    if (layout == binding_layouts_.end()) {
        return Status::error(VITA_GPU_ERROR_INVALID_HANDLE,
                             "DX12 binding-set layout is missing",
                             VITA_GPU_BACKEND_DX12);
    }
    BindingSetObject set;
    set.layout = layout_id;
    set.resource_count = layout->second.resource_count;
    set.sampler_count = layout->second.sampler_count;
    if (set.resource_count != 0) {
        const auto allocation =
            resource_heap_.allocate(set.resource_count);
        if (!allocation.has_value()) {
            return Status::error(
                VITA_GPU_ERROR_OUT_OF_MEMORY,
                "DX12 shader-visible CBV/SRV/UAV heap is exhausted",
                VITA_GPU_BACKEND_DX12);
        }
        set.resource_start = *allocation;
    }
    if (set.sampler_count != 0) {
        const auto allocation =
            sampler_heap_.allocate(set.sampler_count);
        if (!allocation.has_value()) {
            resource_heap_.release(set.resource_start,
                                   set.resource_count);
            return Status::error(
                VITA_GPU_ERROR_OUT_OF_MEMORY,
                "DX12 shader-visible sampler heap is exhausted",
                VITA_GPU_BACKEND_DX12);
        }
        set.sampler_start = *allocation;
    }
    const NativeId id = allocate_id();
    binding_sets_.emplace(id.value, std::move(set));
    output = id;
    return Status::success();
}

Status Dx12Device::update_binding_set(
    NativeId set_id, std::span<const BackendBindingWrite> writes) {
    const auto set = binding_sets_.find(set_id.value);
    if (set == binding_sets_.end()) {
        return Status::error(VITA_GPU_ERROR_INVALID_HANDLE,
                             "DX12 binding set is missing",
                             VITA_GPU_BACKEND_DX12);
    }
    const auto layout = binding_layouts_.find(set->second.layout.value);
    if (layout == binding_layouts_.end()) {
        return Status::error(VITA_GPU_ERROR_INVALID_HANDLE,
                             "DX12 binding layout is missing",
                             VITA_GPU_BACKEND_DX12);
    }

    for (const BackendBindingWrite &write : writes) {
        const auto entry = std::find_if(
            layout->second.entries.begin(), layout->second.entries.end(),
            [&write](const LayoutEntry &candidate) {
                return candidate.source.binding == write.binding &&
                       candidate.source.type == write.type;
            });
        if (entry == layout->second.entries.end() ||
            write.array_element >= entry->source.array_count) {
            return Status::error(
                VITA_GPU_ERROR_INVALID_ARGUMENT,
                "DX12 binding write does not match its backend layout",
                VITA_GPU_BACKEND_DX12);
        }

        if (write.type == VITA_GPU_BINDING_SAMPLER) {
            const auto sampler = samplers_.find(write.sampler.value);
            if (sampler == samplers_.end()) {
                return Status::error(VITA_GPU_ERROR_INVALID_HANDLE,
                                     "DX12 sampler is missing",
                                     VITA_GPU_BACKEND_DX12);
            }
            const std::uint32_t destination =
                set->second.sampler_start + entry->heap_offset +
                write.array_element;
            device_->CopyDescriptorsSimple(
                1, sampler_heap_.cpu(destination),
                sampler_staging_heap_.cpu(sampler->second.descriptor),
                D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
            continue;
        }

        const std::uint32_t destination =
            set->second.resource_start + entry->heap_offset +
            write.array_element;
        const D3D12_CPU_DESCRIPTOR_HANDLE handle =
            resource_heap_.cpu(destination);
        if (write.type == VITA_GPU_BINDING_CONSTANT_BUFFER) {
            const auto buffer = buffers_.find(write.buffer.value);
            if (buffer == buffers_.end()) {
                return Status::error(VITA_GPU_ERROR_INVALID_HANDLE,
                                     "DX12 constant buffer is missing",
                                     VITA_GPU_BACKEND_DX12);
            }
            constexpr std::uint64_t alignment =
                D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
            const std::uint64_t aligned_size =
                (write.buffer_size + alignment - 1U) &
                ~(alignment - 1U);
            if ((write.buffer_offset % alignment) != 0 ||
                aligned_size > D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT *
                                   16ULL ||
                write.buffer_offset > buffer->second.size ||
                aligned_size >
                    buffer->second.size - write.buffer_offset) {
                return Status::error(
                    VITA_GPU_ERROR_INVALID_ARGUMENT,
                    "DX12 constant-buffer offset/size violates 256-byte "
                    "alignment or 64-KiB limits",
                    VITA_GPU_BACKEND_DX12);
            }
            D3D12_CONSTANT_BUFFER_VIEW_DESC view{};
            view.BufferLocation =
                buffer->second.resource->GetGPUVirtualAddress() +
                write.buffer_offset;
            view.SizeInBytes = static_cast<UINT>(aligned_size);
            device_->CreateConstantBufferView(&view, handle);
        } else if (write.type ==
                   VITA_GPU_BINDING_STORAGE_BUFFER) {
            const auto buffer = buffers_.find(write.buffer.value);
            if (buffer == buffers_.end()) {
                return Status::error(VITA_GPU_ERROR_INVALID_HANDLE,
                                     "DX12 storage buffer is missing",
                                     VITA_GPU_BACKEND_DX12);
            }
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
            view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            std::uint32_t stride = buffer->second.stride;
            if (buffer->second.format != VITA_GPU_FORMAT_UNKNOWN) {
                view.Format = dxgi_format(buffer->second.format);
                if (stride == 0) {
                    return Status::error(
                        VITA_GPU_ERROR_INVALID_ARGUMENT,
                        "typed DX12 storage buffer has zero stride",
                        VITA_GPU_BACKEND_DX12);
                }
            } else if (stride != 0) {
                view.Format = DXGI_FORMAT_UNKNOWN;
                view.Buffer.StructureByteStride = stride;
            } else {
                stride = 4;
                view.Format = DXGI_FORMAT_R32_TYPELESS;
                view.Buffer.Flags =
                    D3D12_BUFFER_UAV_FLAG_RAW;
            }
            if ((write.buffer_offset % stride) != 0 ||
                (write.buffer_size % stride) != 0) {
                return Status::error(
                    VITA_GPU_ERROR_INVALID_ARGUMENT,
                    "DX12 storage-buffer range is not element aligned",
                    VITA_GPU_BACKEND_DX12);
            }
            view.Buffer.FirstElement =
                write.buffer_offset / stride;
            const std::uint64_t elements =
                write.buffer_size / stride;
            if (elements > std::numeric_limits<UINT>::max()) {
                return Status::error(
                    VITA_GPU_ERROR_UNSUPPORTED,
                    "DX12 storage-buffer descriptor exceeds UINT element "
                    "capacity",
                    VITA_GPU_BACKEND_DX12);
            }
            view.Buffer.NumElements = static_cast<UINT>(elements);
            device_->CreateUnorderedAccessView(
                buffer->second.resource.Get(), nullptr, &view, handle);
        } else {
            const auto texture = textures_.find(write.texture.value);
            if (texture == textures_.end()) {
                return Status::error(VITA_GPU_ERROR_INVALID_HANDLE,
                                     "DX12 texture binding is missing",
                                     VITA_GPU_BACKEND_DX12);
            }
            const TextureObject &source = texture->second;
            if (write.type == VITA_GPU_BINDING_SAMPLED_TEXTURE) {
                D3D12_SHADER_RESOURCE_VIEW_DESC view{};
                view.Format = shader_resource_format(source.format);
                view.Shader4ComponentMapping =
                    D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                if (source.dimension == VITA_GPU_TEXTURE_1D) {
                    if (source.depth_or_layers == 1) {
                        view.ViewDimension =
                            D3D12_SRV_DIMENSION_TEXTURE1D;
                        view.Texture1D.MipLevels = source.mip_levels;
                    } else {
                        view.ViewDimension =
                            D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
                        view.Texture1DArray.MipLevels =
                            source.mip_levels;
                        view.Texture1DArray.ArraySize =
                            source.depth_or_layers;
                    }
                } else if (source.dimension == VITA_GPU_TEXTURE_3D) {
                    view.ViewDimension =
                        D3D12_SRV_DIMENSION_TEXTURE3D;
                    view.Texture3D.MipLevels = source.mip_levels;
                } else if (source.sample_count > 1) {
                    if (source.depth_or_layers == 1) {
                        view.ViewDimension =
                            D3D12_SRV_DIMENSION_TEXTURE2DMS;
                    } else {
                        view.ViewDimension =
                            D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
                        view.Texture2DMSArray.ArraySize =
                            source.depth_or_layers;
                    }
                } else if (source.depth_or_layers == 1) {
                    view.ViewDimension =
                        D3D12_SRV_DIMENSION_TEXTURE2D;
                    view.Texture2D.MipLevels = source.mip_levels;
                } else {
                    view.ViewDimension =
                        D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                    view.Texture2DArray.MipLevels =
                        source.mip_levels;
                    view.Texture2DArray.ArraySize =
                        source.depth_or_layers;
                }
                device_->CreateShaderResourceView(
                    source.resource.Get(), &view, handle);
            } else {
                if (source.sample_count > 1 ||
                    depth_format(source.format)) {
                    return Status::error(
                        VITA_GPU_ERROR_UNSUPPORTED,
                        "DX12 storage views do not support multisampled or "
                        "depth/stencil textures",
                        VITA_GPU_BACKEND_DX12);
                }
                D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
                view.Format = dxgi_format(source.format);
                if (source.dimension == VITA_GPU_TEXTURE_1D) {
                    if (source.depth_or_layers == 1) {
                        view.ViewDimension =
                            D3D12_UAV_DIMENSION_TEXTURE1D;
                    } else {
                        view.ViewDimension =
                            D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
                        view.Texture1DArray.ArraySize =
                            source.depth_or_layers;
                    }
                } else if (source.dimension == VITA_GPU_TEXTURE_3D) {
                    view.ViewDimension =
                        D3D12_UAV_DIMENSION_TEXTURE3D;
                    view.Texture3D.WSize = source.depth_or_layers;
                } else if (source.depth_or_layers == 1) {
                    view.ViewDimension =
                        D3D12_UAV_DIMENSION_TEXTURE2D;
                } else {
                    view.ViewDimension =
                        D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                    view.Texture2DArray.ArraySize =
                        source.depth_or_layers;
                }
                device_->CreateUnorderedAccessView(
                    source.resource.Get(), nullptr, &view, handle);
            }
        }
    }
    return check_validation_messages("update binding set");
}

Status Dx12Device::build_root_signature(
    std::span<const NativeId> layout_ids,
    std::uint32_t push_constant_bytes, bool graphics,
    RootSignatureBuild &output) {
    std::vector<std::vector<D3D12_DESCRIPTOR_RANGE>> tables;
    tables.reserve(layout_ids.size() * 2U);
    std::vector<D3D12_ROOT_PARAMETER> parameters;
    parameters.reserve(layout_ids.size() * 2U + 1U);
    std::uint32_t dword_cost = 0;

    for (NativeId layout_id : layout_ids) {
        const auto layout = binding_layouts_.find(layout_id.value);
        if (layout == binding_layouts_.end()) {
            return Status::error(VITA_GPU_ERROR_INVALID_HANDLE,
                                 "DX12 pipeline binding layout is missing",
                                 VITA_GPU_BACKEND_DX12);
        }
        PipelineSetBinding binding;
        binding.layout = layout_id;

        const auto add_table =
            [&](bool samplers,
                std::optional<std::uint32_t> &parameter_index) {
                tables.emplace_back();
                std::vector<D3D12_DESCRIPTOR_RANGE> &ranges =
                    tables.back();
                for (const LayoutEntry &entry :
                     layout->second.entries) {
                    if ((entry.source.type == VITA_GPU_BINDING_SAMPLER) !=
                        samplers) {
                        continue;
                    }
                    D3D12_DESCRIPTOR_RANGE range{};
                    range.RangeType = range_type(entry.source.type);
                    range.NumDescriptors = entry.source.array_count;
                    range.BaseShaderRegister = entry.source.binding;
                    range.RegisterSpace = layout->second.set_index;
                    range.OffsetInDescriptorsFromTableStart =
                        entry.heap_offset;
                    ranges.push_back(range);
                }
                if (ranges.empty()) {
                    tables.pop_back();
                    return;
                }
                D3D12_ROOT_PARAMETER parameter{};
                parameter.ParameterType =
                    D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                parameter.DescriptorTable.NumDescriptorRanges =
                    static_cast<UINT>(ranges.size());
                parameter.DescriptorTable.pDescriptorRanges =
                    ranges.data();
                parameter.ShaderVisibility =
                    D3D12_SHADER_VISIBILITY_ALL;
                parameter_index =
                    static_cast<std::uint32_t>(parameters.size());
                parameters.push_back(parameter);
                ++dword_cost;
            };
        add_table(false, binding.resource_parameter);
        add_table(true, binding.sampler_parameter);
        if (!output.sets.emplace(layout->second.set_index, binding).second) {
            return Status::error(
                VITA_GPU_ERROR_INVALID_ARGUMENT,
                "DX12 pipeline contains duplicate binding-set indices",
                VITA_GPU_BACKEND_DX12);
        }
    }

    if (push_constant_bytes != 0) {
        const std::uint32_t constant_count =
            push_constant_bytes / 4U;
        dword_cost += constant_count;
        if (dword_cost > 64) {
            return Status::error(
                VITA_GPU_ERROR_UNSUPPORTED,
                "DX12 root signature exceeds the 64-DWORD hardware limit",
                VITA_GPU_BACKEND_DX12);
        }
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameter.Constants.ShaderRegister = 0;
        parameter.Constants.RegisterSpace =
            kPushConstantRegisterSpace;
        parameter.Constants.Num32BitValues = constant_count;
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        output.push_parameter =
            static_cast<std::uint32_t>(parameters.size());
        parameters.push_back(parameter);
    }
    if (dword_cost > 64) {
        return Status::error(
            VITA_GPU_ERROR_UNSUPPORTED,
            "DX12 descriptor tables exceed the 64-DWORD root-signature "
            "limit",
            VITA_GPU_BACKEND_DX12);
    }

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = static_cast<UINT>(parameters.size());
    desc.pParameters = parameters.data();
    desc.Flags =
        graphics
            ? D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
            : D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3D12SerializeRootSignature(
        &desc, D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized, &errors);
    if (FAILED(hr)) {
        std::string message =
            "D3D12SerializeRootSignature failed: " + hr_text(hr);
        if (errors && errors->GetBufferPointer() != nullptr) {
            message += "; ";
            message.append(
                static_cast<const char *>(errors->GetBufferPointer()),
                errors->GetBufferSize());
        }
        return dx_status(VITA_GPU_ERROR_NATIVE, std::move(message), hr);
    }
    hr = device_->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&output.signature));
    if (FAILED(hr)) {
        return native_failure("ID3D12Device::CreateRootSignature", hr);
    }
    return Status::success();
}

Status Dx12Device::create_graphics_pipeline(
    const BackendGraphicsPipelineDesc &backend_desc,
    NativeId &output) {
    const vita_gpu_graphics_pipeline_desc &desc = *backend_desc.source;
    const auto vertex = shaders_.find(backend_desc.vertex_shader.value);
    const auto pixel = shaders_.find(backend_desc.pixel_shader.value);
    if (vertex == shaders_.end() || pixel == shaders_.end() ||
        vertex->second.stage != VITA_GPU_SHADER_VERTEX ||
        pixel->second.stage != VITA_GPU_SHADER_PIXEL) {
        return Status::error(
            VITA_GPU_ERROR_INVALID_ARGUMENT,
            "DX12 graphics pipeline requires matching vertex and pixel "
            "DXIL modules",
            VITA_GPU_BACKEND_DX12);
    }
    RootSignatureBuild root;
    Status status = build_root_signature(
        backend_desc.binding_layouts, desc.push_constant_bytes, true, root);
    if (!status.ok()) {
        return status;
    }

    std::unordered_map<std::uint32_t, vita_gpu_vertex_binding_desc>
        vertex_bindings;
    for (std::uint32_t index = 0; index < desc.vertex_binding_count;
         ++index) {
        if (!vertex_bindings.emplace(
                desc.vertex_bindings[index].binding,
                desc.vertex_bindings[index])
                 .second) {
            return Status::error(
                VITA_GPU_ERROR_INVALID_ARGUMENT,
                "DX12 graphics pipeline has duplicate vertex bindings",
                VITA_GPU_BACKEND_DX12);
        }
    }
    std::vector<D3D12_INPUT_ELEMENT_DESC> input_elements;
    input_elements.reserve(desc.vertex_attribute_count);
    for (std::uint32_t index = 0; index < desc.vertex_attribute_count;
         ++index) {
        const vita_gpu_vertex_attribute_desc &attribute =
            desc.vertex_attributes[index];
        const auto binding = vertex_bindings.find(attribute.binding);
        if (binding == vertex_bindings.end()) {
            return Status::error(
                VITA_GPU_ERROR_INVALID_ARGUMENT,
                "DX12 vertex attribute references an undeclared binding",
                VITA_GPU_BACKEND_DX12);
        }
        D3D12_INPUT_ELEMENT_DESC element{};
        element.SemanticName = "TEXCOORD";
        element.SemanticIndex = attribute.location;
        element.Format = dxgi_format(attribute.format);
        element.InputSlot = attribute.binding;
        element.AlignedByteOffset = attribute.offset;
        element.InputSlotClass =
            binding->second.step == VITA_GPU_VERTEX_PER_INSTANCE
                ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        element.InstanceDataStepRate =
            binding->second.step == VITA_GPU_VERTEX_PER_INSTANCE ? 1U : 0U;
        input_elements.push_back(element);
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC native{};
    native.pRootSignature = root.signature.Get();
    native.VS = {vertex->second.bytecode.data(),
                 vertex->second.bytecode.size()};
    native.PS = {pixel->second.bytecode.data(),
                 pixel->second.bytecode.size()};
    native.BlendState.AlphaToCoverageEnable = FALSE;
    native.BlendState.IndependentBlendEnable =
        desc.color_target_count > 1 ? TRUE : FALSE;
    for (std::uint32_t index = 0; index < desc.color_target_count;
         ++index) {
        const vita_gpu_blend_attachment_desc &source = desc.blend[index];
        D3D12_RENDER_TARGET_BLEND_DESC &target =
            native.BlendState.RenderTarget[index];
        target.BlendEnable = source.blend_enable != 0;
        target.LogicOpEnable = FALSE;
        target.SrcBlend = blend_factor(source.source_color);
        target.DestBlend = blend_factor(source.destination_color);
        target.BlendOp = blend_operation(source.color_op);
        target.SrcBlendAlpha = blend_factor(source.source_alpha);
        target.DestBlendAlpha =
            blend_factor(source.destination_alpha);
        target.BlendOpAlpha = blend_operation(source.alpha_op);
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
        target.RenderTargetWriteMask =
            static_cast<UINT8>(source.color_write_mask);
        native.RTVFormats[index] = dxgi_format(desc.color_formats[index]);
    }

    native.SampleMask = UINT_MAX;
    native.RasterizerState.FillMode =
        desc.rasterizer.polygon_mode == VITA_GPU_POLYGON_LINE
            ? D3D12_FILL_MODE_WIREFRAME
            : D3D12_FILL_MODE_SOLID;
    switch (desc.rasterizer.cull_mode) {
    case VITA_GPU_CULL_FRONT:
        native.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
        break;
    case VITA_GPU_CULL_BACK:
        native.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        break;
    default:
        native.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        break;
    }
    native.RasterizerState.FrontCounterClockwise =
        desc.rasterizer.front_face ==
                VITA_GPU_FRONT_COUNTER_CLOCKWISE
            ? TRUE
            : FALSE;
    native.RasterizerState.DepthBias = desc.rasterizer.depth_bias;
    native.RasterizerState.DepthBiasClamp =
        desc.rasterizer.depth_bias_clamp;
    native.RasterizerState.SlopeScaledDepthBias =
        desc.rasterizer.slope_scaled_depth_bias;
    native.RasterizerState.DepthClipEnable = TRUE;
    native.RasterizerState.MultisampleEnable =
        desc.sample_count > 1 ? TRUE : FALSE;
    native.RasterizerState.AntialiasedLineEnable = FALSE;
    native.RasterizerState.ForcedSampleCount = 0;
    native.RasterizerState.ConservativeRaster =
        D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    native.DepthStencilState.DepthEnable =
        desc.depth_stencil.depth_test_enable != 0;
    native.DepthStencilState.DepthWriteMask =
        desc.depth_stencil.depth_write_enable != 0
            ? D3D12_DEPTH_WRITE_MASK_ALL
            : D3D12_DEPTH_WRITE_MASK_ZERO;
    native.DepthStencilState.DepthFunc =
        comparison(desc.depth_stencil.depth_compare);
    native.DepthStencilState.StencilEnable =
        desc.depth_stencil.stencil_enable != 0;
    native.DepthStencilState.StencilReadMask =
        desc.depth_stencil.stencil_read_mask;
    native.DepthStencilState.StencilWriteMask =
        desc.depth_stencil.stencil_write_mask;
    native.DepthStencilState.FrontFace = {
        D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
        D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    native.DepthStencilState.BackFace =
        native.DepthStencilState.FrontFace;

    native.InputLayout = {
        input_elements.data(),
        static_cast<UINT>(input_elements.size())};
    native.PrimitiveTopologyType = topology_type(desc.topology);
    native.NumRenderTargets =
        static_cast<UINT>(desc.color_target_count);
    native.DSVFormat = dxgi_format(desc.depth_format);
    native.SampleDesc = {desc.sample_count, 0};
    native.NodeMask = 0;
    native.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    PipelineObject pipeline;
    HRESULT hr = device_->CreateGraphicsPipelineState(
        &native, IID_PPV_ARGS(&pipeline.state));
    if (FAILED(hr)) {
        return native_failure(
            "ID3D12Device::CreateGraphicsPipelineState", hr);
    }
    set_name(pipeline.state.Get(), desc.label);
    pipeline.root_signature = std::move(root.signature);
    pipeline.graphics = true;
    pipeline.topology = primitive_topology(desc.topology);
    pipeline.sets = std::move(root.sets);
    pipeline.push_parameter = root.push_parameter;
    pipeline.push_constant_bytes = desc.push_constant_bytes;
    for (const auto &[binding, source] : vertex_bindings) {
        pipeline.vertex_strides.emplace(binding, source.stride);
    }
    const NativeId id = allocate_id();
    pipelines_.emplace(id.value, std::move(pipeline));
    output = id;
    return check_validation_messages("create graphics pipeline");
}

Status Dx12Device::create_compute_pipeline(
    const BackendComputePipelineDesc &backend_desc,
    NativeId &output) {
    const vita_gpu_compute_pipeline_desc &desc = *backend_desc.source;
    const auto shader = shaders_.find(backend_desc.compute_shader.value);
    if (shader == shaders_.end() ||
        shader->second.stage != VITA_GPU_SHADER_COMPUTE) {
        return Status::error(
            VITA_GPU_ERROR_INVALID_ARGUMENT,
            "DX12 compute pipeline requires a compute DXIL module",
            VITA_GPU_BACKEND_DX12);
    }
    RootSignatureBuild root;
    Status status = build_root_signature(
        backend_desc.binding_layouts, desc.push_constant_bytes, false, root);
    if (!status.ok()) {
        return status;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC native{};
    native.pRootSignature = root.signature.Get();
    native.CS = {shader->second.bytecode.data(),
                 shader->second.bytecode.size()};
    PipelineObject pipeline;
    const HRESULT hr = device_->CreateComputePipelineState(
        &native, IID_PPV_ARGS(&pipeline.state));
    if (FAILED(hr)) {
        return native_failure(
            "ID3D12Device::CreateComputePipelineState", hr);
    }
    set_name(pipeline.state.Get(), desc.label);
    pipeline.root_signature = std::move(root.signature);
    pipeline.graphics = false;
    pipeline.sets = std::move(root.sets);
    pipeline.push_parameter = root.push_parameter;
    pipeline.push_constant_bytes = desc.push_constant_bytes;
    const NativeId id = allocate_id();
    pipelines_.emplace(id.value, std::move(pipeline));
    output = id;
    return check_validation_messages("create compute pipeline");
}

void Dx12Device::release_swapchain_images(
    SwapchainObject &swapchain) {
    for (NativeId target_id : swapchain.targets) {
        const auto target = render_targets_.find(target_id.value);
        if (target != render_targets_.end()) {
            rtv_heap_.release(target->second.descriptor);
            render_targets_.erase(target);
        }
    }
    for (NativeId texture_id : swapchain.textures) {
        textures_.erase(texture_id.value);
    }
    swapchain.targets.clear();
    swapchain.textures.clear();
}

Status Dx12Device::create_swapchain_images(
    SwapchainObject &swapchain) {
    swapchain.textures.reserve(swapchain.image_count);
    swapchain.targets.reserve(swapchain.image_count);
    for (std::uint32_t index = 0; index < swapchain.image_count; ++index) {
        ComPtr<ID3D12Resource> resource;
        const HRESULT hr =
            swapchain.swapchain->GetBuffer(index, IID_PPV_ARGS(&resource));
        if (FAILED(hr)) {
            release_swapchain_images(swapchain);
            return native_failure("IDXGISwapChain::GetBuffer", hr);
        }
        const auto descriptor = rtv_heap_.allocate();
        if (!descriptor.has_value()) {
            release_swapchain_images(swapchain);
            return Status::error(
                VITA_GPU_ERROR_OUT_OF_MEMORY,
                "DX12 RTV descriptor heap is exhausted by swapchain images",
                VITA_GPU_BACKEND_DX12);
        }
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = dxgi_format(swapchain.format);
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipSlice = 0;
        view.Texture2D.PlaneSlice = 0;
        device_->CreateRenderTargetView(resource.Get(), &view,
                                        rtv_heap_.cpu(*descriptor));

        TextureObject texture;
        texture.resource = std::move(resource);
        texture.dimension = VITA_GPU_TEXTURE_2D;
        texture.width = swapchain.width;
        texture.height = swapchain.height;
        texture.depth_or_layers = 1;
        texture.mip_levels = 1;
        texture.sample_count = 1;
        texture.format = swapchain.format;
        texture.usage = VITA_GPU_TEXTURE_RENDER_TARGET |
                        VITA_GPU_TEXTURE_COPY_SOURCE;
        texture.memory_type = VITA_GPU_MEMORY_GPU_ONLY;
        texture.state = VITA_GPU_RESOURCE_STATE_PRESENT;
        texture.borrowed = true;
        const NativeId texture_id = allocate_id();
        textures_.emplace(texture_id.value, std::move(texture));
        const NativeId target_id = allocate_id();
        render_targets_.emplace(
            target_id.value,
            RenderTargetObject{*descriptor, texture_id});
        swapchain.textures.push_back(texture_id);
        swapchain.targets.push_back(target_id);
    }
    return check_validation_messages("create swapchain images");
}

Status Dx12Device::create_swapchain(
    const vita_gpu_swapchain_desc &desc, NativeId &output) {
    const HWND window = static_cast<HWND>(desc.window.native_window);
    if (!IsWindow(window)) {
        return Status::error(VITA_GPU_ERROR_INVALID_ARGUMENT,
                             "DX12 swapchain HWND is not a live Win32 window",
                             VITA_GPU_BACKEND_DX12);
    }
    const DXGI_FORMAT format = swapchain_format(desc.format);
    if (format != DXGI_FORMAT_R8G8B8A8_UNORM &&
        format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
        return Status::error(
            VITA_GPU_ERROR_UNSUPPORTED,
            "DX12 flip-model swapchains support RGBA8, BGRA8, or "
            "RGBA16_FLOAT contract formats",
            VITA_GPU_BACKEND_DX12);
    }

    DXGI_SWAP_CHAIN_DESC1 native{};
    native.Width = desc.width;
    native.Height = desc.height;
    native.Format = format;
    native.Stereo = FALSE;
    native.SampleDesc = {1, 0};
    native.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    native.BufferCount = desc.image_count;
    native.Scaling = DXGI_SCALING_STRETCH;
    native.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    native.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    native.Flags = tearing_supported_
                       ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
                       : 0U;

    ComPtr<IDXGISwapChain1> swapchain1;
    HRESULT hr = factory_->CreateSwapChainForHwnd(
        queue_.Get(), window, &native, nullptr, nullptr, &swapchain1);
    if (FAILED(hr)) {
        return native_failure("IDXGIFactory::CreateSwapChainForHwnd", hr);
    }
    (void)factory_->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);

    SwapchainObject swapchain;
    hr = swapchain1.As(&swapchain.swapchain);
    if (FAILED(hr)) {
        return native_failure("IDXGISwapChain1::QueryInterface", hr);
    }
    swapchain.window = window;
    swapchain.width = desc.width;
    swapchain.height = desc.height;
    swapchain.image_count = desc.image_count;
    swapchain.format = desc.format;
    swapchain.present_mode = desc.present_mode;
    swapchain.flags = native.Flags;
    Status status = create_swapchain_images(swapchain);
    if (!status.ok()) {
        return status;
    }
    const NativeId id = allocate_id();
    swapchains_.emplace(id.value, std::move(swapchain));
    output = id;
    return Status::success();
}

Status Dx12Device::resize_swapchain(NativeId swapchain_id,
                                    std::uint32_t width,
                                    std::uint32_t height) {
    const auto found = swapchains_.find(swapchain_id.value);
    if (found == swapchains_.end()) {
        return Status::error(VITA_GPU_ERROR_INVALID_HANDLE,
                             "DX12 swapchain is missing",
                             VITA_GPU_BACKEND_DX12);
    }
    Status status = wait_idle();
    if (!status.ok()) {
        return status;
    }
    SwapchainObject &swapchain = found->second;
    release_swapchain_images(swapchain);
    const HRESULT hr = swapchain.swapchain->ResizeBuffers(
        swapchain.image_count, width, height,
        swapchain_format(swapchain.format), swapchain.flags);
    if (FAILED(hr)) {
        return native_failure("IDXGISwapChain::ResizeBuffers", hr);
    }
    swapchain.width = width;
    swapchain.height = height;
    status = create_swapchain_images(swapchain);
    if (!status.ok()) {
        return status;
    }
    return check_validation_messages("resize swapchain");
}

Status Dx12Device::acquire_swapchain(
    NativeId swapchain_id, BackendSwapchainImage &output) {
    const auto found = swapchains_.find(swapchain_id.value);
    if (found == swapchains_.end()) {
        return Status::error(VITA_GPU_ERROR_INVALID_HANDLE,
                             "DX12 swapchain is missing",
                             VITA_GPU_BACKEND_DX12);
    }
    const UINT index =
        found->second.swapchain->GetCurrentBackBufferIndex();
    if (index >= found->second.textures.size()) {
        return Status::error(
            VITA_GPU_ERROR_INTERNAL,
            "DX12 swapchain returned an out-of-range back-buffer index",
            VITA_GPU_BACKEND_DX12);
    }
    output.image_index = index;
    output.texture = found->second.textures[index];
    output.render_target = found->second.targets[index];
    return check_validation_messages("acquire swapchain image");
}

Status Dx12Device::present_swapchain(
    NativeId swapchain_id, const vita_gpu_present_desc &desc) {
    const auto found = swapchains_.find(swapchain_id.value);
    if (found == swapchains_.end()) {
        return Status::error(VITA_GPU_ERROR_INVALID_HANDLE,
                             "DX12 swapchain is missing",
                             VITA_GPU_BACKEND_DX12);
    }
    SwapchainObject &swapchain = found->second;
    if (desc.image_index !=
        swapchain.swapchain->GetCurrentBackBufferIndex()) {
        return Status::error(
            VITA_GPU_ERROR_INVALID_STATE,
            "DX12 present index does not match the current back buffer",
            VITA_GPU_BACKEND_DX12);
    }
    if (desc.allow_tearing != 0 && !tearing_supported_) {
        return Status::error(
            VITA_GPU_ERROR_UNSUPPORTED,
            "DX12 tearing was requested but DXGI does not support it",
            VITA_GPU_BACKEND_DX12);
    }
    if (desc.allow_tearing != 0 &&
        swapchain.present_mode == VITA_GPU_PRESENT_FIFO) {
        return Status::error(
            VITA_GPU_ERROR_INVALID_ARGUMENT,
            "DX12 FIFO presentation cannot request tearing",
            VITA_GPU_BACKEND_DX12);
    }
    const UINT sync_interval =
        swapchain.present_mode == VITA_GPU_PRESENT_FIFO ? 1U : 0U;
    const UINT flags =
        desc.allow_tearing != 0
            ? DXGI_PRESENT_ALLOW_TEARING
            : 0U;
    const HRESULT hr =
        swapchain.swapchain->Present(sync_interval, flags);
    if (FAILED(hr)) {
        return native_failure("IDXGISwapChain::Present", hr);
    }
    return check_validation_messages("present");
}

Status Dx12Device::destroy_object(ObjectKind kind, NativeId object) {
    switch (kind) {
    case ObjectKind::swapchain: {
        const auto found = swapchains_.find(object.value);
        if (found == swapchains_.end()) {
            break;
        }
        Status status = wait_idle();
        if (!status.ok()) {
            return status;
        }
        release_swapchain_images(found->second);
        swapchains_.erase(found);
        return Status::success();
    }
    case ObjectKind::buffer:
        if (buffers_.erase(object.value) != 0) {
            return Status::success();
        }
        break;
    case ObjectKind::texture: {
        const auto found = textures_.find(object.value);
        if (found != textures_.end() && found->second.borrowed) {
            return Status::error(
                VITA_GPU_ERROR_INVALID_STATE,
                "DX12 swapchain textures are borrowed",
                VITA_GPU_BACKEND_DX12);
        }
        if (found != textures_.end()) {
            textures_.erase(found);
            return Status::success();
        }
        break;
    }
    case ObjectKind::sampler: {
        const auto found = samplers_.find(object.value);
        if (found != samplers_.end()) {
            sampler_staging_heap_.release(found->second.descriptor);
            samplers_.erase(found);
            return Status::success();
        }
        break;
    }
    case ObjectKind::render_target: {
        const auto found = render_targets_.find(object.value);
        if (found != render_targets_.end()) {
            rtv_heap_.release(found->second.descriptor);
            render_targets_.erase(found);
            return Status::success();
        }
        break;
    }
    case ObjectKind::depth_target: {
        const auto found = depth_targets_.find(object.value);
        if (found != depth_targets_.end()) {
            dsv_heap_.release(found->second.descriptor);
            depth_targets_.erase(found);
            return Status::success();
        }
        break;
    }
    case ObjectKind::shader:
        if (shaders_.erase(object.value) != 0) {
            return Status::success();
        }
        break;
    case ObjectKind::binding_layout:
        if (binding_layouts_.erase(object.value) != 0) {
            return Status::success();
        }
        break;
    case ObjectKind::binding_set: {
        const auto found = binding_sets_.find(object.value);
        if (found != binding_sets_.end()) {
            resource_heap_.release(found->second.resource_start,
                                   found->second.resource_count);
            sampler_heap_.release(found->second.sampler_start,
                                  found->second.sampler_count);
            binding_sets_.erase(found);
            return Status::success();
        }
        break;
    }
    case ObjectKind::pipeline:
        if (pipelines_.erase(object.value) != 0) {
            return Status::success();
        }
        break;
    case ObjectKind::fence: {
        const auto found = fences_.find(object.value);
        if (found != fences_.end()) {
            if (found->second.event != nullptr) {
                CloseHandle(found->second.event);
            }
            fences_.erase(found);
            return Status::success();
        }
        break;
    }
    case ObjectKind::readback: {
        const auto found = readbacks_.find(object.value);
        if (found != readbacks_.end()) {
            if (found->second.mapped) {
                return Status::error(
                    VITA_GPU_ERROR_INVALID_STATE,
                    "DX12 readback must be unmapped before destruction",
                    VITA_GPU_BACKEND_DX12);
            }
            readbacks_.erase(found);
            return Status::success();
        }
        break;
    }
    }
    return Status::error(VITA_GPU_ERROR_INVALID_HANDLE,
                         "DX12 backend object is missing or has the wrong type",
                         VITA_GPU_BACKEND_DX12);
}

Status Dx12Device::fence_completed_value(NativeId fence_id,
                                         std::uint64_t &value) {
    const auto fence = fences_.find(fence_id.value);
    if (fence == fences_.end()) {
        return Status::error(VITA_GPU_ERROR_INVALID_HANDLE,
                             "DX12 fence is missing",
                             VITA_GPU_BACKEND_DX12);
    }
    value = fence->second.fence->GetCompletedValue();
    if (value == std::numeric_limits<std::uint64_t>::max()) {
        return native_failure("ID3D12Fence::GetCompletedValue",
                              DXGI_ERROR_DEVICE_REMOVED);
    }
    return check_validation_messages("fence completed value");
}

Status Dx12Device::fence_wait(NativeId fence_id, std::uint64_t value,
                              std::uint64_t timeout_nanoseconds) {
    const auto fence = fences_.find(fence_id.value);
    if (fence == fences_.end()) {
        return Status::error(VITA_GPU_ERROR_INVALID_HANDLE,
                             "DX12 fence is missing",
                             VITA_GPU_BACKEND_DX12);
    }
    if (fence->second.fence->GetCompletedValue() >= value) {
        return check_validation_messages("fence wait");
    }
    HRESULT hr = fence->second.fence->SetEventOnCompletion(
        value, fence->second.event);
    if (FAILED(hr)) {
        return native_failure("ID3D12Fence::SetEventOnCompletion", hr);
    }
    DWORD timeout = INFINITE;
    if (timeout_nanoseconds !=
        std::numeric_limits<std::uint64_t>::max()) {
        const std::uint64_t milliseconds =
            (timeout_nanoseconds + UINT64_C(999999)) /
            UINT64_C(1000000);
        timeout = static_cast<DWORD>(std::min<std::uint64_t>(
            milliseconds, static_cast<std::uint64_t>(INFINITE - 1U)));
    }
    const DWORD result =
        WaitForSingleObject(fence->second.event, timeout);
    if (result == WAIT_TIMEOUT) {
        return Status::error(VITA_GPU_ERROR_TIMEOUT,
                             "DX12 fence wait timed out",
                             VITA_GPU_BACKEND_DX12);
    }
    if (result != WAIT_OBJECT_0) {
        hr = result == WAIT_FAILED
                 ? HRESULT_FROM_WIN32(GetLastError())
                 : E_FAIL;
        return native_failure("WaitForSingleObject(fence)", hr);
    }
    return check_validation_messages("fence wait");
}

Status Dx12Device::map_readback(NativeId readback_id,
                                BackendReadbackMap &output) {
    const auto readback = readbacks_.find(readback_id.value);
    if (readback == readbacks_.end()) {
        return Status::error(VITA_GPU_ERROR_INVALID_HANDLE,
                             "DX12 readback is missing",
                             VITA_GPU_BACKEND_DX12);
    }
    if (readback->second.mapped) {
        return Status::error(VITA_GPU_ERROR_INVALID_STATE,
                             "DX12 readback is already mapped",
                             VITA_GPU_BACKEND_DX12);
    }
    D3D12_RANGE range{
        0, static_cast<SIZE_T>(readback->second.size)};
    void *data = nullptr;
    const HRESULT hr =
        readback->second.resource->Map(0, &range, &data);
    if (FAILED(hr)) {
        return native_failure("ID3D12Resource::Map(readback)", hr);
    }
    readback->second.mapped = true;
    readback->second.mapped_data =
        static_cast<const std::uint8_t *>(data);
    output.data = readback->second.mapped_data;
    output.byte_size = readback->second.size;
    output.row_pitch = readback->second.row_pitch;
    output.slice_pitch = readback->second.slice_pitch;
    return check_validation_messages("map readback");
}

Status Dx12Device::unmap_readback(NativeId readback_id) {
    const auto readback = readbacks_.find(readback_id.value);
    if (readback == readbacks_.end()) {
        return Status::error(VITA_GPU_ERROR_INVALID_HANDLE,
                             "DX12 readback is missing",
                             VITA_GPU_BACKEND_DX12);
    }
    if (!readback->second.mapped) {
        return Status::error(VITA_GPU_ERROR_INVALID_STATE,
                             "DX12 readback is not mapped",
                             VITA_GPU_BACKEND_DX12);
    }
    D3D12_RANGE written{0, 0};
    readback->second.resource->Unmap(0, &written);
    readback->second.mapped = false;
    readback->second.mapped_data = nullptr;
    return check_validation_messages("unmap readback");
}

Status Dx12Device::translate_commands(
    ID3D12GraphicsCommandList *native,
    std::span<const RecordedCommandList *const> command_lists,
    std::map<std::pair<ObjectKind, std::uint64_t>,
             std::pair<vita_gpu_resource_state, vita_gpu_queue_type>>
        &state_updates,
    std::map<std::uint64_t, std::pair<std::uint32_t, std::uint32_t>>
        &readback_updates) {
    struct ActivePass {
        std::vector<std::pair<NativeId, vita_gpu_store_op>> colors;
        std::optional<std::pair<NativeId, BackendDepthAttachment>> depth;
    };
    std::optional<ActivePass> active_pass;
    PipelineObject *bound_pipeline = nullptr;

    const auto tracked_state =
        [&](ObjectKind kind, std::uint64_t id,
            vita_gpu_resource_state &state,
            vita_gpu_queue_type &queue) -> Status {
            const auto update = state_updates.find({kind, id});
            if (update != state_updates.end()) {
                state = update->second.first;
                queue = update->second.second;
                return Status::success();
            }
            if (kind == ObjectKind::buffer) {
                const auto buffer = buffers_.find(id);
                if (buffer == buffers_.end()) {
                    return Status::error(
                        VITA_GPU_ERROR_INVALID_HANDLE,
                        "DX12 command references a missing buffer",
                        VITA_GPU_BACKEND_DX12);
                }
                state = buffer->second.state;
                queue = buffer->second.queue;
                return Status::success();
            }
            const auto texture = textures_.find(id);
            if (texture == textures_.end()) {
                return Status::error(
                    VITA_GPU_ERROR_INVALID_HANDLE,
                    "DX12 command references a missing texture",
                    VITA_GPU_BACKEND_DX12);
            }
            state = texture->second.state;
            queue = texture->second.queue;
            return Status::success();
        };

    const auto texture_for_target =
        [&](NativeId target_id) -> TextureObject * {
            const auto target = render_targets_.find(target_id.value);
            if (target == render_targets_.end()) {
                return nullptr;
            }
            const auto texture =
                textures_.find(target->second.texture.value);
            return texture == textures_.end() ? nullptr
                                               : &texture->second;
        };

    ID3D12DescriptorHeap *descriptor_heaps[]{
        resource_heap_.get(), sampler_heap_.get()};
    native->SetDescriptorHeaps(
        static_cast<UINT>(std::size(descriptor_heaps)),
        descriptor_heaps);

    for (const RecordedCommandList *recorded : command_lists) {
        if (recorded == nullptr) {
            return Status::error(VITA_GPU_ERROR_INVALID_ARGUMENT,
                                 "DX12 submission contains a null command list",
                                 VITA_GPU_BACKEND_DX12);
        }
        bound_pipeline = nullptr;
        active_pass.reset();
        for (const CommandPayload &payload : recorded->commands) {
            Status status = std::visit(
                [&](const auto &command) -> Status {
                    using T = std::decay_t<decltype(command)>;
                    if constexpr (std::is_same_v<T, BarrierCommand>) {
                        ID3D12Resource *resource = nullptr;
                        vita_gpu_memory_type memory_type =
                            VITA_GPU_MEMORY_GPU_ONLY;
                        if (command.kind == VITA_GPU_RESOURCE_BUFFER) {
                            const auto buffer =
                                buffers_.find(command.resource.value);
                            if (buffer == buffers_.end()) {
                                return Status::error(
                                    VITA_GPU_ERROR_INVALID_HANDLE,
                                    "DX12 barrier buffer is missing",
                                    VITA_GPU_BACKEND_DX12);
                            }
                            resource = buffer->second.resource.Get();
                            memory_type = buffer->second.memory_type;
                        } else {
                            const auto texture =
                                textures_.find(command.resource.value);
                            if (texture == textures_.end()) {
                                return Status::error(
                                    VITA_GPU_ERROR_INVALID_HANDLE,
                                    "DX12 barrier texture is missing",
                                    VITA_GPU_BACKEND_DX12);
                            }
                            resource = texture->second.resource.Get();
                        }
                        const ObjectKind kind =
                            command.kind == VITA_GPU_RESOURCE_BUFFER
                                ? ObjectKind::buffer
                                : ObjectKind::texture;
                        vita_gpu_resource_state current =
                            VITA_GPU_RESOURCE_STATE_UNDEFINED;
                        vita_gpu_queue_type current_queue =
                            VITA_GPU_QUEUE_GRAPHICS;
                        Status tracked = tracked_state(
                            kind, command.resource.value, current,
                            current_queue);
                        if (!tracked.ok()) {
                            return tracked;
                        }
                        if (current != command.before ||
                            current_queue != command.source_queue ||
                            command.destination_queue != recorded->queue) {
                            return Status::error(
                                VITA_GPU_ERROR_INVALID_STATE,
                                "DX12 barrier state or queue does not match "
                                "backend tracking",
                                VITA_GPU_BACKEND_DX12);
                        }
                        bool emit = true;
                        if (kind == ObjectKind::buffer &&
                            memory_type == VITA_GPU_MEMORY_CPU_UPLOAD) {
                            if ((command.after &
                                 (VITA_GPU_RESOURCE_STATE_COPY_DEST |
                                  VITA_GPU_RESOURCE_STATE_SHADER_WRITE)) !=
                                0) {
                                return Status::error(
                                    VITA_GPU_ERROR_UNSUPPORTED,
                                    "DX12 upload-heap buffer cannot "
                                    "transition to a write state",
                                    VITA_GPU_BACKEND_DX12);
                            }
                            emit = false;
                        } else if (
                            kind == ObjectKind::buffer &&
                            memory_type == VITA_GPU_MEMORY_CPU_READBACK) {
                            if (command.after !=
                                    VITA_GPU_RESOURCE_STATE_COMMON &&
                                command.after !=
                                    VITA_GPU_RESOURCE_STATE_COPY_DEST) {
                                return Status::error(
                                    VITA_GPU_ERROR_UNSUPPORTED,
                                    "DX12 readback-heap buffer cannot "
                                    "transition out of COPY_DEST",
                                    VITA_GPU_BACKEND_DX12);
                            }
                            emit = false;
                        }
                        const D3D12_RESOURCE_STATES before =
                            resource_state(command.before);
                        const D3D12_RESOURCE_STATES after =
                            resource_state(command.after);
                        if (emit && before != after) {
                            D3D12_RESOURCE_BARRIER barrier{};
                            barrier.Type =
                                D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                            barrier.Transition.pResource = resource;
                            barrier.Transition.Subresource =
                                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                            barrier.Transition.StateBefore = before;
                            barrier.Transition.StateAfter = after;
                            native->ResourceBarrier(1, &barrier);
                        }
                        state_updates[{kind, command.resource.value}] = {
                            command.after, command.destination_queue};
                        return Status::success();
                    } else if constexpr (
                        std::is_same_v<T, BeginRenderPassCommand>) {
                        if (active_pass.has_value()) {
                            return Status::error(
                                VITA_GPU_ERROR_INVALID_STATE,
                                "DX12 render pass is already active",
                                VITA_GPU_BACKEND_DX12);
                        }
                        ActivePass pass;
                        std::array<D3D12_CPU_DESCRIPTOR_HANDLE,
                                   VITA_GPU_MAX_COLOR_TARGETS>
                            rtvs{};
                        for (std::size_t index = 0;
                             index < command.colors.size(); ++index) {
                            const BackendColorAttachment &attachment =
                                command.colors[index];
                            const auto target = render_targets_.find(
                                attachment.target.value);
                            TextureObject *texture =
                                texture_for_target(attachment.target);
                            if (target == render_targets_.end() ||
                                texture == nullptr) {
                                return Status::error(
                                    VITA_GPU_ERROR_INVALID_HANDLE,
                                    "DX12 render-pass target is missing",
                                    VITA_GPU_BACKEND_DX12);
                            }
                            rtvs[index] =
                                rtv_heap_.cpu(target->second.descriptor);
                            if (attachment.load_op ==
                                VITA_GPU_LOAD_CLEAR) {
                                native->ClearRenderTargetView(
                                    rtvs[index],
                                    attachment.clear_color.data(), 0,
                                    nullptr);
                            } else if (attachment.load_op ==
                                       VITA_GPU_LOAD_DISCARD) {
                                native->DiscardResource(
                                    texture->resource.Get(), nullptr);
                            }
                            pass.colors.emplace_back(
                                attachment.target, attachment.store_op);
                        }

                        D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
                        const D3D12_CPU_DESCRIPTOR_HANDLE *dsv_pointer =
                            nullptr;
                        if (command.depth.has_value()) {
                            const BackendDepthAttachment &attachment =
                                *command.depth;
                            const auto target = depth_targets_.find(
                                attachment.target.value);
                            if (target == depth_targets_.end()) {
                                return Status::error(
                                    VITA_GPU_ERROR_INVALID_HANDLE,
                                    "DX12 depth target is missing",
                                    VITA_GPU_BACKEND_DX12);
                            }
                            const auto texture = textures_.find(
                                target->second.texture.value);
                            if (texture == textures_.end()) {
                                return Status::error(
                                    VITA_GPU_ERROR_INVALID_HANDLE,
                                    "DX12 depth texture is missing",
                                    VITA_GPU_BACKEND_DX12);
                            }
                            dsv = dsv_heap_.cpu(
                                target->second.descriptor);
                            dsv_pointer = &dsv;
                            D3D12_CLEAR_FLAGS clear_flags =
                                static_cast<D3D12_CLEAR_FLAGS>(0);
                            if (attachment.depth_load_op ==
                                VITA_GPU_LOAD_CLEAR) {
                                clear_flags |=
                                    D3D12_CLEAR_FLAG_DEPTH;
                            } else if (attachment.depth_load_op ==
                                       VITA_GPU_LOAD_DISCARD) {
                                native->DiscardResource(
                                    texture->second.resource.Get(),
                                    nullptr);
                            }
                            if (attachment.stencil_load_op ==
                                VITA_GPU_LOAD_CLEAR) {
                                clear_flags |=
                                    D3D12_CLEAR_FLAG_STENCIL;
                            } else if (
                                attachment.stencil_load_op ==
                                VITA_GPU_LOAD_DISCARD) {
                                native->DiscardResource(
                                    texture->second.resource.Get(),
                                    nullptr);
                            }
                            if (clear_flags != 0) {
                                native->ClearDepthStencilView(
                                    dsv, clear_flags,
                                    attachment.clear_depth,
                                    static_cast<UINT8>(
                                        attachment.clear_stencil),
                                    0, nullptr);
                            }
                            pass.depth.emplace(
                                attachment.target, attachment);
                        }
                        native->OMSetRenderTargets(
                            static_cast<UINT>(command.colors.size()),
                            rtvs.data(), FALSE, dsv_pointer);
                        active_pass = std::move(pass);
                        return Status::success();
                    } else if constexpr (
                        std::is_same_v<T, EndRenderPassCommand>) {
                        if (!active_pass.has_value()) {
                            return Status::error(
                                VITA_GPU_ERROR_INVALID_STATE,
                                "DX12 has no active render pass to end",
                                VITA_GPU_BACKEND_DX12);
                        }
                        for (const auto &[target_id, store] :
                             active_pass->colors) {
                            if (store == VITA_GPU_STORE_DISCARD) {
                                TextureObject *texture =
                                    texture_for_target(target_id);
                                if (texture != nullptr) {
                                    native->DiscardResource(
                                        texture->resource.Get(), nullptr);
                                }
                            }
                        }
                        if (active_pass->depth.has_value()) {
                            const auto &[target_id, attachment] =
                                *active_pass->depth;
                            if (attachment.depth_store_op ==
                                    VITA_GPU_STORE_DISCARD ||
                                attachment.stencil_store_op ==
                                    VITA_GPU_STORE_DISCARD) {
                                const auto target = depth_targets_.find(
                                    target_id.value);
                                if (target != depth_targets_.end()) {
                                    const auto texture = textures_.find(
                                        target->second.texture.value);
                                    if (texture != textures_.end()) {
                                        native->DiscardResource(
                                            texture->second.resource.Get(),
                                            nullptr);
                                    }
                                }
                            }
                        }
                        active_pass.reset();
                        bound_pipeline = nullptr;
                        return Status::success();
                    } else if constexpr (
                        std::is_same_v<T, BindPipelineCommand>) {
                        const auto pipeline =
                            pipelines_.find(command.pipeline.value);
                        if (pipeline == pipelines_.end()) {
                            return Status::error(
                                VITA_GPU_ERROR_INVALID_HANDLE,
                                "DX12 pipeline is missing",
                                VITA_GPU_BACKEND_DX12);
                        }
                        bound_pipeline = &pipeline->second;
                        native->SetPipelineState(
                            bound_pipeline->state.Get());
                        if (bound_pipeline->graphics) {
                            native->SetGraphicsRootSignature(
                                bound_pipeline->root_signature.Get());
                            native->IASetPrimitiveTopology(
                                bound_pipeline->topology);
                        } else {
                            native->SetComputeRootSignature(
                                bound_pipeline->root_signature.Get());
                        }
                        return Status::success();
                    } else if constexpr (
                        std::is_same_v<T, BindSetCommand>) {
                        if (bound_pipeline == nullptr) {
                            return Status::error(
                                VITA_GPU_ERROR_INVALID_STATE,
                                "DX12 binding set requires a pipeline",
                                VITA_GPU_BACKEND_DX12);
                        }
                        const auto set =
                            binding_sets_.find(command.set.value);
                        const auto binding =
                            bound_pipeline->sets.find(command.set_index);
                        if (set == binding_sets_.end() ||
                            binding == bound_pipeline->sets.end() ||
                            binding->second.layout != set->second.layout) {
                            return Status::error(
                                VITA_GPU_ERROR_INVALID_ARGUMENT,
                                "DX12 binding set does not match the "
                                "pipeline root signature",
                                VITA_GPU_BACKEND_DX12);
                        }
                        if (binding->second.resource_parameter.has_value()) {
                            const auto gpu = resource_heap_.gpu(
                                set->second.resource_start);
                            if (bound_pipeline->graphics) {
                                native->SetGraphicsRootDescriptorTable(
                                    *binding->second.resource_parameter,
                                    gpu);
                            } else {
                                native->SetComputeRootDescriptorTable(
                                    *binding->second.resource_parameter,
                                    gpu);
                            }
                        }
                        if (binding->second.sampler_parameter.has_value()) {
                            const auto gpu = sampler_heap_.gpu(
                                set->second.sampler_start);
                            if (bound_pipeline->graphics) {
                                native->SetGraphicsRootDescriptorTable(
                                    *binding->second.sampler_parameter,
                                    gpu);
                            } else {
                                native->SetComputeRootDescriptorTable(
                                    *binding->second.sampler_parameter,
                                    gpu);
                            }
                        }
                        return Status::success();
                    } else if constexpr (
                        std::is_same_v<T, ViewportCommand>) {
                        const D3D12_VIEWPORT viewport{
                            command.viewport.x,
                            command.viewport.y,
                            command.viewport.width,
                            command.viewport.height,
                            command.viewport.min_depth,
                            command.viewport.max_depth};
                        native->RSSetViewports(1, &viewport);
                        return Status::success();
                    } else if constexpr (
                        std::is_same_v<T, ScissorCommand>) {
                        const std::int64_t right =
                            static_cast<std::int64_t>(command.scissor.x) +
                            command.scissor.width;
                        const std::int64_t bottom =
                            static_cast<std::int64_t>(command.scissor.y) +
                            command.scissor.height;
                        if (right > std::numeric_limits<LONG>::max() ||
                            bottom > std::numeric_limits<LONG>::max()) {
                            return Status::error(
                                VITA_GPU_ERROR_INVALID_ARGUMENT,
                                "DX12 scissor rectangle exceeds LONG range",
                                VITA_GPU_BACKEND_DX12);
                        }
                        const D3D12_RECT scissor{
                            command.scissor.x, command.scissor.y,
                            static_cast<LONG>(right),
                            static_cast<LONG>(bottom)};
                        native->RSSetScissorRects(1, &scissor);
                        return Status::success();
                    } else if constexpr (
                        std::is_same_v<T, VertexBufferCommand>) {
                        if (bound_pipeline == nullptr ||
                            !bound_pipeline->graphics) {
                            return Status::error(
                                VITA_GPU_ERROR_INVALID_STATE,
                                "DX12 vertex buffer requires a graphics "
                                "pipeline",
                                VITA_GPU_BACKEND_DX12);
                        }
                        const auto buffer =
                            buffers_.find(command.buffer.value);
                        const auto stride =
                            bound_pipeline->vertex_strides.find(
                                command.binding);
                        if (buffer == buffers_.end() ||
                            stride ==
                                bound_pipeline->vertex_strides.end() ||
                            command.offset >= buffer->second.size) {
                            return Status::error(
                                VITA_GPU_ERROR_INVALID_ARGUMENT,
                                "DX12 vertex buffer binding is invalid",
                                VITA_GPU_BACKEND_DX12);
                        }
                        const std::uint64_t remaining =
                            buffer->second.size - command.offset;
                        D3D12_VERTEX_BUFFER_VIEW view{};
                        view.BufferLocation =
                            buffer->second.resource
                                ->GetGPUVirtualAddress() +
                            command.offset;
                        view.SizeInBytes = static_cast<UINT>(
                            std::min<std::uint64_t>(
                                remaining,
                                std::numeric_limits<UINT>::max()));
                        view.StrideInBytes = stride->second;
                        native->IASetVertexBuffers(command.binding, 1,
                                                   &view);
                        return Status::success();
                    } else if constexpr (
                        std::is_same_v<T, IndexBufferCommand>) {
                        const auto buffer =
                            buffers_.find(command.buffer.value);
                        if (buffer == buffers_.end() ||
                            command.offset >= buffer->second.size) {
                            return Status::error(
                                VITA_GPU_ERROR_INVALID_ARGUMENT,
                                "DX12 index buffer binding is invalid",
                                VITA_GPU_BACKEND_DX12);
                        }
                        D3D12_INDEX_BUFFER_VIEW view{};
                        view.BufferLocation =
                            buffer->second.resource
                                ->GetGPUVirtualAddress() +
                            command.offset;
                        view.SizeInBytes = static_cast<UINT>(
                            std::min<std::uint64_t>(
                                buffer->second.size - command.offset,
                                std::numeric_limits<UINT>::max()));
                        view.Format =
                            command.format == VITA_GPU_INDEX_UINT16
                                ? DXGI_FORMAT_R16_UINT
                                : DXGI_FORMAT_R32_UINT;
                        native->IASetIndexBuffer(&view);
                        return Status::success();
                    } else if constexpr (
                        std::is_same_v<T, PushConstantsCommand>) {
                        if (bound_pipeline == nullptr ||
                            !bound_pipeline->push_parameter.has_value() ||
                            command.offset + command.data.size() >
                                bound_pipeline->push_constant_bytes) {
                            return Status::error(
                                VITA_GPU_ERROR_INVALID_ARGUMENT,
                                "DX12 push constants exceed the active "
                                "pipeline root constants",
                                VITA_GPU_BACKEND_DX12);
                        }
                        const UINT count = static_cast<UINT>(
                            command.data.size() / 4U);
                        const UINT offset = command.offset / 4U;
                        if (bound_pipeline->graphics) {
                            native->SetGraphicsRoot32BitConstants(
                                *bound_pipeline->push_parameter, count,
                                command.data.data(), offset);
                        } else {
                            native->SetComputeRoot32BitConstants(
                                *bound_pipeline->push_parameter, count,
                                command.data.data(), offset);
                        }
                        return Status::success();
                    } else if constexpr (
                        std::is_same_v<T, DrawCommand>) {
                        if (bound_pipeline == nullptr ||
                            !bound_pipeline->graphics ||
                            !active_pass.has_value()) {
                            return Status::error(
                                VITA_GPU_ERROR_INVALID_STATE,
                                "DX12 draw requires a graphics pipeline and "
                                "render pass",
                                VITA_GPU_BACKEND_DX12);
                        }
                        native->DrawInstanced(
                            command.draw.vertex_count,
                            command.draw.instance_count,
                            command.draw.first_vertex,
                            command.draw.first_instance);
                        return Status::success();
                    } else if constexpr (
                        std::is_same_v<T, DrawIndexedCommand>) {
                        if (bound_pipeline == nullptr ||
                            !bound_pipeline->graphics ||
                            !active_pass.has_value()) {
                            return Status::error(
                                VITA_GPU_ERROR_INVALID_STATE,
                                "DX12 indexed draw requires a graphics "
                                "pipeline and render pass",
                                VITA_GPU_BACKEND_DX12);
                        }
                        native->DrawIndexedInstanced(
                            command.draw.index_count,
                            command.draw.instance_count,
                            command.draw.first_index,
                            command.draw.vertex_offset,
                            command.draw.first_instance);
                        return Status::success();
                    } else if constexpr (
                        std::is_same_v<T, DispatchCommand>) {
                        if (bound_pipeline == nullptr ||
                            bound_pipeline->graphics ||
                            active_pass.has_value()) {
                            return Status::error(
                                VITA_GPU_ERROR_INVALID_STATE,
                                "DX12 dispatch requires a compute pipeline "
                                "outside a render pass",
                                VITA_GPU_BACKEND_DX12);
                        }
                        native->Dispatch(
                            command.dispatch.group_count_x,
                            command.dispatch.group_count_y,
                            command.dispatch.group_count_z);
                        return Status::success();
                    } else if constexpr (
                        std::is_same_v<T, CopyBufferCommand>) {
                        const auto source =
                            buffers_.find(command.source.value);
                        const auto destination =
                            buffers_.find(command.destination.value);
                        if (source == buffers_.end() ||
                            destination == buffers_.end()) {
                            return Status::error(
                                VITA_GPU_ERROR_INVALID_HANDLE,
                                "DX12 buffer copy resource is missing",
                                VITA_GPU_BACKEND_DX12);
                        }
                        native->CopyBufferRegion(
                            destination->second.resource.Get(),
                            command.destination_offset,
                            source->second.resource.Get(),
                            command.source_offset, command.byte_size);
                        return Status::success();
                    } else if constexpr (
                        std::is_same_v<T, CopyBufferReadbackCommand>) {
                        const auto source =
                            buffers_.find(command.source.value);
                        const auto destination =
                            readbacks_.find(command.destination.value);
                        if (source == buffers_.end() ||
                            destination == readbacks_.end()) {
                            return Status::error(
                                VITA_GPU_ERROR_INVALID_HANDLE,
                                "DX12 buffer readback resource is missing",
                                VITA_GPU_BACKEND_DX12);
                        }
                        native->CopyBufferRegion(
                            destination->second.resource.Get(),
                            command.destination_offset,
                            source->second.resource.Get(),
                            command.source_offset, command.byte_size);
                        readback_updates[command.destination.value] = {
                            static_cast<std::uint32_t>(
                                std::min<std::uint64_t>(
                                    command.byte_size,
                                    std::numeric_limits<
                                        std::uint32_t>::max())),
                            static_cast<std::uint32_t>(
                                std::min<std::uint64_t>(
                                    command.byte_size,
                                    std::numeric_limits<
                                        std::uint32_t>::max()))};
                        return Status::success();
                    } else if constexpr (
                        std::is_same_v<T, CopyTextureReadbackCommand>) {
                        const auto source =
                            textures_.find(command.source.value);
                        const auto destination =
                            readbacks_.find(command.destination.value);
                        if (source == textures_.end() ||
                            destination == readbacks_.end()) {
                            return Status::error(
                                VITA_GPU_ERROR_INVALID_HANDLE,
                                "DX12 texture readback resource is missing",
                                VITA_GPU_BACKEND_DX12);
                        }
                        if (depth_format(source->second.format)) {
                            return Status::error(
                                VITA_GPU_ERROR_UNSUPPORTED,
                                "DX12 depth/stencil texture readback is "
                                "unsupported by the contract's single-plane "
                                "copy descriptor",
                                VITA_GPU_BACKEND_DX12);
                        }
                        if ((command.destination_offset %
                             D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) != 0 ||
                            (command.row_pitch %
                             D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) != 0) {
                            return Status::error(
                                VITA_GPU_ERROR_INVALID_ARGUMENT,
                                "DX12 texture readback offset must be "
                                "512-byte aligned and row pitch 256-byte "
                                "aligned",
                                VITA_GPU_BACKEND_DX12);
                        }
                        const D3D12_RESOURCE_DESC source_desc =
                            source->second.resource->GetDesc();
                        const UINT subresource =
                            source->second.dimension ==
                                    VITA_GPU_TEXTURE_3D
                                ? command.mip_level
                                : command.mip_level +
                                      command.array_layer *
                                          source->second.mip_levels;
                        D3D12_PLACED_SUBRESOURCE_FOOTPRINT calculated{};
                        UINT rows = 0;
                        UINT64 row_size = 0;
                        UINT64 total_size = 0;
                        device_->GetCopyableFootprints(
                            &source_desc, subresource, 1, 0,
                            &calculated, &rows, &row_size, &total_size);
                        (void)total_size;
                        const UINT64 required =
                            static_cast<UINT64>(command.row_pitch) * rows *
                            calculated.Footprint.Depth;
                        if (command.row_pitch < row_size ||
                            command.slice_pitch < required ||
                            command.destination_offset >
                                destination->second.size ||
                            required >
                                destination->second.size -
                                    command.destination_offset) {
                            return Status::error(
                                VITA_GPU_ERROR_INVALID_ARGUMENT,
                                "DX12 texture readback footprint does not "
                                "fit the destination",
                                VITA_GPU_BACKEND_DX12);
                        }
                        calculated.Offset = command.destination_offset;
                        calculated.Footprint.RowPitch =
                            command.row_pitch;
                        D3D12_TEXTURE_COPY_LOCATION destination_location{};
                        destination_location.pResource =
                            destination->second.resource.Get();
                        destination_location.Type =
                            D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                        destination_location.PlacedFootprint =
                            calculated;
                        D3D12_TEXTURE_COPY_LOCATION source_location{};
                        source_location.pResource =
                            source->second.resource.Get();
                        source_location.Type =
                            D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                        source_location.SubresourceIndex = subresource;
                        native->CopyTextureRegion(
                            &destination_location, 0, 0, 0,
                            &source_location, nullptr);
                        readback_updates[command.destination.value] = {
                            command.row_pitch, command.slice_pitch};
                        return Status::success();
                    } else if constexpr (
                        std::is_same_v<T, DebugMarkerCommand>) {
                        native->SetMarker(
                            command.rgba_color, command.label.data(),
                            static_cast<UINT>(command.label.size()));
                        return Status::success();
                    } else {
                        return Status::error(
                            VITA_GPU_ERROR_UNSUPPORTED,
                            "DX12 encountered an unsupported command payload",
                            VITA_GPU_BACKEND_DX12);
                    }
                },
                payload);
            if (!status.ok()) {
                return status;
            }
        }
        if (active_pass.has_value()) {
            return Status::error(
                VITA_GPU_ERROR_INVALID_STATE,
                "DX12 command list ended with an open render pass",
                VITA_GPU_BACKEND_DX12);
        }
    }
    return Status::success();
}

Status Dx12Device::submit(
    vita_gpu_queue_type,
    std::span<const RecordedCommandList *const> command_lists,
    std::span<const BackendFencePoint> waits,
    BackendFencePoint signal) {
    const auto signal_fence = fences_.find(signal.fence.value);
    if (signal_fence == fences_.end()) {
        return Status::error(VITA_GPU_ERROR_INVALID_HANDLE,
                             "DX12 signal fence is missing",
                             VITA_GPU_BACKEND_DX12);
    }
    FrameContext &frame = frames_[next_frame_];
    next_frame_ = (next_frame_ + 1U) % kFrameContextCount;
    if (frame.fence_value != 0 &&
        internal_fence_->GetCompletedValue() < frame.fence_value) {
        Status status = wait_internal(frame.fence_value);
        if (!status.ok()) {
            return status;
        }
    }
    HRESULT hr = frame.allocator->Reset();
    if (FAILED(hr)) {
        return native_failure("ID3D12CommandAllocator::Reset", hr);
    }
    hr = frame.command_list->Reset(frame.allocator.Get(), nullptr);
    if (FAILED(hr)) {
        return native_failure("ID3D12GraphicsCommandList::Reset", hr);
    }

    std::map<std::pair<ObjectKind, std::uint64_t>,
             std::pair<vita_gpu_resource_state, vita_gpu_queue_type>>
        state_updates;
    std::map<std::uint64_t, std::pair<std::uint32_t, std::uint32_t>>
        readback_updates;
    Status status = translate_commands(
        frame.command_list.Get(), command_lists, state_updates,
        readback_updates);
    if (!status.ok()) {
        (void)frame.command_list->Close();
        return status;
    }
    hr = frame.command_list->Close();
    if (FAILED(hr)) {
        return native_failure("ID3D12GraphicsCommandList::Close", hr);
    }

    for (const BackendFencePoint &wait : waits) {
        const auto fence = fences_.find(wait.fence.value);
        if (fence == fences_.end()) {
            return Status::error(VITA_GPU_ERROR_INVALID_HANDLE,
                                 "DX12 wait fence is missing",
                                 VITA_GPU_BACKEND_DX12);
        }
        hr = queue_->Wait(fence->second.fence.Get(), wait.value);
        if (FAILED(hr)) {
            return native_failure("ID3D12CommandQueue::Wait", hr);
        }
    }
    ID3D12CommandList *lists[]{frame.command_list.Get()};
    queue_->ExecuteCommandLists(1, lists);
    hr = queue_->Signal(signal_fence->second.fence.Get(), signal.value);
    if (FAILED(hr)) {
        return native_failure("ID3D12CommandQueue::Signal", hr);
    }
    frame.fence_value = ++internal_fence_value_;
    hr = queue_->Signal(internal_fence_.Get(), frame.fence_value);
    if (FAILED(hr)) {
        return native_failure(
            "ID3D12CommandQueue::Signal(frame context)", hr);
    }

    for (const auto &[key, value] : state_updates) {
        if (key.first == ObjectKind::buffer) {
            const auto buffer = buffers_.find(key.second);
            if (buffer != buffers_.end()) {
                buffer->second.state = value.first;
                buffer->second.queue = value.second;
            }
        } else {
            const auto texture = textures_.find(key.second);
            if (texture != textures_.end()) {
                texture->second.state = value.first;
                texture->second.queue = value.second;
            }
        }
    }
    for (const auto &[id, pitches] : readback_updates) {
        const auto readback = readbacks_.find(id);
        if (readback != readbacks_.end()) {
            readback->second.row_pitch = pitches.first;
            readback->second.slice_pitch = pitches.second;
        }
    }
    return check_validation_messages("submit");
}

Status Dx12Factory::create_device(
    const vita_gpu_device_desc &desc,
    std::unique_ptr<BackendDevice> &output) const {
    if (!initialization_error_.empty()) {
        return dx_status(VITA_GPU_ERROR_BACKEND_UNAVAILABLE,
                         initialization_error_, initialization_hr_);
    }
    if (desc.adapter_index >= adapters_.size()) {
        return Status::error(VITA_GPU_ERROR_ADAPTER_NOT_FOUND,
                             "DX12 adapter index is out of range",
                             VITA_GPU_BACKEND_DX12);
    }
    auto device = std::make_unique<Dx12Device>();
    Status status = device->initialize(
        factory_, adapters_[desc.adapter_index], desc.flags);
    if (!status.ok()) {
        return status;
    }
    output = std::move(device);
    return Status::success();
}

} // namespace

std::shared_ptr<BackendFactory> make_dx12_backend_factory() {
    return std::make_shared<Dx12Factory>();
}

} // namespace vitarecomp::gpu
