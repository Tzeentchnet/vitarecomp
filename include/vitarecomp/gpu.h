/*
 * gpu.h - stable renderer-neutral C ABI.
 *
 * The ABI deliberately exposes value handles rather than C++ objects or native
 * API pointers. A handle is owned by the device that created it, remains valid
 * until its matching destroy function succeeds, and must never be interpreted
 * by callers. Handle value zero is always invalid.
 *
 * Every descriptor starts with struct_size and struct_version. Initialize both
 * with VITA_GPU_INIT(type). Input strings and pointed-to arrays are borrowed
 * only for the duration of the call unless a function explicitly says
 * otherwise.
 */

#ifndef VITARECOMP_GPU_H
#define VITARECOMP_GPU_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(VITA_GPU_SHARED)
#  if defined(VITA_GPU_BUILD)
#    define VITA_GPU_API __declspec(dllexport)
#  else
#    define VITA_GPU_API __declspec(dllimport)
#  endif
#else
#  define VITA_GPU_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define VITA_GPU_ABI_VERSION UINT32_C(1)
#define VITA_GPU_STRUCT_VERSION_1 UINT32_C(1)
#define VITA_GPU_MAX_COLOR_TARGETS UINT32_C(8)
#define VITA_GPU_SHADER_KEY_SIZE UINT32_C(32)
#define VITA_GPU_INIT(type) { (uint32_t)sizeof(type), VITA_GPU_STRUCT_VERSION_1 }

typedef uint32_t vita_gpu_bool;
#define VITA_GPU_FALSE UINT32_C(0)
#define VITA_GPU_TRUE UINT32_C(1)

typedef struct vita_gpu_struct_header {
    uint32_t struct_size;
    uint32_t struct_version;
} vita_gpu_struct_header;

#define VITA_GPU_DECLARE_HANDLE(name) \
    typedef struct name { uint64_t value; } name

VITA_GPU_DECLARE_HANDLE(vita_gpu_device);
VITA_GPU_DECLARE_HANDLE(vita_gpu_swapchain);
VITA_GPU_DECLARE_HANDLE(vita_gpu_buffer);
VITA_GPU_DECLARE_HANDLE(vita_gpu_texture);
VITA_GPU_DECLARE_HANDLE(vita_gpu_sampler);
VITA_GPU_DECLARE_HANDLE(vita_gpu_render_target);
VITA_GPU_DECLARE_HANDLE(vita_gpu_depth_target);
VITA_GPU_DECLARE_HANDLE(vita_gpu_shader_module);
VITA_GPU_DECLARE_HANDLE(vita_gpu_binding_layout);
VITA_GPU_DECLARE_HANDLE(vita_gpu_binding_set);
VITA_GPU_DECLARE_HANDLE(vita_gpu_pipeline);
VITA_GPU_DECLARE_HANDLE(vita_gpu_command_list);
VITA_GPU_DECLARE_HANDLE(vita_gpu_fence);
VITA_GPU_DECLARE_HANDLE(vita_gpu_readback);

#undef VITA_GPU_DECLARE_HANDLE

typedef enum vita_gpu_result {
    VITA_GPU_SUCCESS = 0,
    VITA_GPU_ERROR_INVALID_ARGUMENT = 1,
    VITA_GPU_ERROR_INVALID_STRUCT = 2,
    VITA_GPU_ERROR_UNSUPPORTED_ABI_VERSION = 3,
    VITA_GPU_ERROR_BACKEND_UNAVAILABLE = 4,
    VITA_GPU_ERROR_ADAPTER_NOT_FOUND = 5,
    VITA_GPU_ERROR_UNSUPPORTED = 6,
    VITA_GPU_ERROR_OUT_OF_MEMORY = 7,
    VITA_GPU_ERROR_INVALID_HANDLE = 8,
    VITA_GPU_ERROR_STALE_HANDLE = 9,
    VITA_GPU_ERROR_WRONG_OWNER = 10,
    VITA_GPU_ERROR_INVALID_STATE = 11,
    VITA_GPU_ERROR_RESOURCE_IN_USE = 12,
    VITA_GPU_ERROR_DEVICE_LOST = 13,
    VITA_GPU_ERROR_TIMEOUT = 14,
    VITA_GPU_ERROR_NATIVE = 15,
    VITA_GPU_ERROR_SHADER_COMPILER_UNAVAILABLE = 16,
    VITA_GPU_ERROR_SHADER_COMPILATION = 17,
    VITA_GPU_ERROR_IO = 18,
    VITA_GPU_ERROR_INTERNAL = 19,
    VITA_GPU_RESULT_FORCE_U32 = 0x7fffffff
} vita_gpu_result;

typedef enum vita_gpu_backend {
    VITA_GPU_BACKEND_NONE = 0,
    VITA_GPU_BACKEND_DX12 = 1,
    VITA_GPU_BACKEND_VULKAN = 2,
    VITA_GPU_BACKEND_FORCE_U32 = 0x7fffffff
} vita_gpu_backend;

typedef struct vita_gpu_error {
    uint32_t struct_size;
    uint32_t struct_version;
    vita_gpu_result result;
    vita_gpu_backend backend;
    int64_t native_code;
    char function_name[64];
    char message[256];
} vita_gpu_error;

typedef enum vita_gpu_adapter_type {
    VITA_GPU_ADAPTER_OTHER = 0,
    VITA_GPU_ADAPTER_INTEGRATED = 1,
    VITA_GPU_ADAPTER_DISCRETE = 2,
    VITA_GPU_ADAPTER_SOFTWARE = 3,
    VITA_GPU_ADAPTER_TYPE_FORCE_U32 = 0x7fffffff
} vita_gpu_adapter_type;

typedef uint64_t vita_gpu_capability_flags;
#define VITA_GPU_CAPABILITY_GRAPHICS          (UINT64_C(1) << 0)
#define VITA_GPU_CAPABILITY_COMPUTE           (UINT64_C(1) << 1)
#define VITA_GPU_CAPABILITY_TIMELINE_FENCES   (UINT64_C(1) << 2)
#define VITA_GPU_CAPABILITY_DEBUG_MARKERS     (UINT64_C(1) << 3)
#define VITA_GPU_CAPABILITY_DEBUG_CAPTURE     (UINT64_C(1) << 4)
#define VITA_GPU_CAPABILITY_BC_TEXTURES       (UINT64_C(1) << 5)
#define VITA_GPU_CAPABILITY_ANISOTROPY        (UINT64_C(1) << 6)
#define VITA_GPU_CAPABILITY_SHADER_FLOAT16    (UINT64_C(1) << 7)
#define VITA_GPU_CAPABILITY_SHADER_INT64      (UINT64_C(1) << 8)

typedef struct vita_gpu_backend_info {
    uint32_t struct_size;
    uint32_t struct_version;
    vita_gpu_backend backend;
    vita_gpu_bool available;
    vita_gpu_bool validation_available;
    char name[16];
    char unavailable_reason[192];
} vita_gpu_backend_info;

typedef struct vita_gpu_adapter_info {
    uint32_t struct_size;
    uint32_t struct_version;
    uint32_t adapter_index;
    vita_gpu_adapter_type type;
    uint32_t vendor_id;
    uint32_t device_id;
    uint64_t dedicated_video_memory;
    uint64_t dedicated_system_memory;
    uint64_t shared_system_memory;
    uint64_t platform_id;
    uint8_t device_uuid[16];
    vita_gpu_capability_flags capabilities;
    char name[128];
} vita_gpu_adapter_info;

typedef struct vita_gpu_device_capabilities {
    uint32_t struct_size;
    uint32_t struct_version;
    vita_gpu_capability_flags flags;
    uint32_t max_texture_1d;
    uint32_t max_texture_2d;
    uint32_t max_texture_3d;
    uint32_t max_texture_array_layers;
    uint32_t max_color_targets;
    uint32_t max_binding_sets;
    uint32_t max_bindings_per_set;
    uint32_t max_push_constant_bytes;
    uint32_t max_sampler_anisotropy;
    uint32_t constant_buffer_alignment;
    uint32_t storage_buffer_alignment;
    uint32_t texture_row_pitch_alignment;
    uint32_t timestamp_period_numerator;
    uint32_t timestamp_period_denominator;
} vita_gpu_device_capabilities;

typedef uint64_t vita_gpu_device_flags;
#define VITA_GPU_DEVICE_ENABLE_VALIDATION (UINT64_C(1) << 0)
#define VITA_GPU_DEVICE_ENABLE_DEBUG_NAMES (UINT64_C(1) << 1)

typedef struct vita_gpu_device_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    vita_gpu_backend backend;
    uint32_t adapter_index;
    vita_gpu_device_flags flags;
    const char *application_name;
} vita_gpu_device_desc;

typedef enum vita_gpu_queue_type {
    VITA_GPU_QUEUE_GRAPHICS = 0,
    VITA_GPU_QUEUE_COMPUTE = 1,
    VITA_GPU_QUEUE_COPY = 2,
    VITA_GPU_QUEUE_TYPE_FORCE_U32 = 0x7fffffff
} vita_gpu_queue_type;

typedef enum vita_gpu_format {
    VITA_GPU_FORMAT_UNKNOWN = 0,
    VITA_GPU_FORMAT_R8_UNORM = 1,
    VITA_GPU_FORMAT_RG8_UNORM = 2,
    VITA_GPU_FORMAT_RGBA8_UNORM = 3,
    VITA_GPU_FORMAT_RGBA8_SRGB = 4,
    VITA_GPU_FORMAT_BGRA8_UNORM = 5,
    VITA_GPU_FORMAT_BGRA8_SRGB = 6,
    VITA_GPU_FORMAT_R16_FLOAT = 7,
    VITA_GPU_FORMAT_RG16_FLOAT = 8,
    VITA_GPU_FORMAT_RGBA16_FLOAT = 9,
    VITA_GPU_FORMAT_R32_FLOAT = 10,
    VITA_GPU_FORMAT_RG32_FLOAT = 11,
    VITA_GPU_FORMAT_RGB32_FLOAT = 12,
    VITA_GPU_FORMAT_RGBA32_FLOAT = 13,
    VITA_GPU_FORMAT_R16_UINT = 14,
    VITA_GPU_FORMAT_R32_UINT = 15,
    VITA_GPU_FORMAT_D16_UNORM = 16,
    VITA_GPU_FORMAT_D24_UNORM_S8_UINT = 17,
    VITA_GPU_FORMAT_D32_FLOAT = 18,
    VITA_GPU_FORMAT_D32_FLOAT_S8_UINT = 19,
    VITA_GPU_FORMAT_BC1_RGBA_UNORM = 20,
    VITA_GPU_FORMAT_BC1_RGBA_SRGB = 21,
    VITA_GPU_FORMAT_BC2_UNORM = 22,
    VITA_GPU_FORMAT_BC2_SRGB = 23,
    VITA_GPU_FORMAT_BC3_UNORM = 24,
    VITA_GPU_FORMAT_BC3_SRGB = 25,
    VITA_GPU_FORMAT_BC4_UNORM = 26,
    VITA_GPU_FORMAT_BC5_UNORM = 27,
    VITA_GPU_FORMAT_BC7_UNORM = 28,
    VITA_GPU_FORMAT_BC7_SRGB = 29,
    VITA_GPU_FORMAT_FORCE_U32 = 0x7fffffff
} vita_gpu_format;

typedef uint64_t vita_gpu_resource_state;
#define VITA_GPU_RESOURCE_STATE_UNDEFINED       UINT64_C(0)
#define VITA_GPU_RESOURCE_STATE_COMMON          (UINT64_C(1) << 0)
#define VITA_GPU_RESOURCE_STATE_COPY_SOURCE     (UINT64_C(1) << 1)
#define VITA_GPU_RESOURCE_STATE_COPY_DEST       (UINT64_C(1) << 2)
#define VITA_GPU_RESOURCE_STATE_VERTEX_BUFFER   (UINT64_C(1) << 3)
#define VITA_GPU_RESOURCE_STATE_INDEX_BUFFER    (UINT64_C(1) << 4)
#define VITA_GPU_RESOURCE_STATE_CONSTANT_BUFFER (UINT64_C(1) << 5)
#define VITA_GPU_RESOURCE_STATE_SHADER_READ     (UINT64_C(1) << 6)
#define VITA_GPU_RESOURCE_STATE_SHADER_WRITE    (UINT64_C(1) << 7)
#define VITA_GPU_RESOURCE_STATE_RENDER_TARGET   (UINT64_C(1) << 8)
#define VITA_GPU_RESOURCE_STATE_DEPTH_READ      (UINT64_C(1) << 9)
#define VITA_GPU_RESOURCE_STATE_DEPTH_WRITE     (UINT64_C(1) << 10)
#define VITA_GPU_RESOURCE_STATE_PRESENT         (UINT64_C(1) << 11)

typedef enum vita_gpu_memory_type {
    VITA_GPU_MEMORY_GPU_ONLY = 0,
    VITA_GPU_MEMORY_CPU_UPLOAD = 1,
    VITA_GPU_MEMORY_CPU_READBACK = 2,
    VITA_GPU_MEMORY_TYPE_FORCE_U32 = 0x7fffffff
} vita_gpu_memory_type;

typedef uint64_t vita_gpu_buffer_usage;
#define VITA_GPU_BUFFER_VERTEX         (UINT64_C(1) << 0)
#define VITA_GPU_BUFFER_INDEX          (UINT64_C(1) << 1)
#define VITA_GPU_BUFFER_CONSTANT       (UINT64_C(1) << 2)
#define VITA_GPU_BUFFER_STORAGE        (UINT64_C(1) << 3)
#define VITA_GPU_BUFFER_INDIRECT       (UINT64_C(1) << 4)
#define VITA_GPU_BUFFER_COPY_SOURCE    (UINT64_C(1) << 5)
#define VITA_GPU_BUFFER_COPY_DEST      (UINT64_C(1) << 6)

typedef struct vita_gpu_buffer_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    const char *label;
    uint64_t byte_size;
    uint32_t element_stride;
    vita_gpu_format element_format;
    vita_gpu_buffer_usage usage;
    vita_gpu_memory_type memory_type;
    vita_gpu_resource_state initial_state;
    const uint8_t *initial_data;
    uint64_t initial_data_size;
} vita_gpu_buffer_desc;

typedef enum vita_gpu_texture_dimension {
    VITA_GPU_TEXTURE_1D = 0,
    VITA_GPU_TEXTURE_2D = 1,
    VITA_GPU_TEXTURE_3D = 2,
    VITA_GPU_TEXTURE_DIMENSION_FORCE_U32 = 0x7fffffff
} vita_gpu_texture_dimension;

typedef uint64_t vita_gpu_texture_usage;
#define VITA_GPU_TEXTURE_SAMPLED          (UINT64_C(1) << 0)
#define VITA_GPU_TEXTURE_STORAGE          (UINT64_C(1) << 1)
#define VITA_GPU_TEXTURE_RENDER_TARGET    (UINT64_C(1) << 2)
#define VITA_GPU_TEXTURE_DEPTH_TARGET     (UINT64_C(1) << 3)
#define VITA_GPU_TEXTURE_COPY_SOURCE      (UINT64_C(1) << 4)
#define VITA_GPU_TEXTURE_COPY_DEST        (UINT64_C(1) << 5)

typedef struct vita_gpu_subresource_data {
    uint32_t struct_size;
    uint32_t struct_version;
    const uint8_t *data;
    uint64_t data_size;
    uint32_t row_pitch;
    uint32_t slice_pitch;
} vita_gpu_subresource_data;

typedef struct vita_gpu_texture_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    const char *label;
    vita_gpu_texture_dimension dimension;
    uint32_t width;
    uint32_t height;
    uint32_t depth_or_layers;
    uint32_t mip_levels;
    uint32_t sample_count;
    vita_gpu_format format;
    vita_gpu_texture_usage usage;
    vita_gpu_memory_type memory_type;
    vita_gpu_resource_state initial_state;
    const vita_gpu_subresource_data *initial_data;
    uint32_t initial_data_count;
} vita_gpu_texture_desc;

typedef enum vita_gpu_filter {
    VITA_GPU_FILTER_NEAREST = 0,
    VITA_GPU_FILTER_LINEAR = 1,
    VITA_GPU_FILTER_FORCE_U32 = 0x7fffffff
} vita_gpu_filter;

typedef enum vita_gpu_mipmap_mode {
    VITA_GPU_MIPMAP_NEAREST = 0,
    VITA_GPU_MIPMAP_LINEAR = 1,
    VITA_GPU_MIPMAP_MODE_FORCE_U32 = 0x7fffffff
} vita_gpu_mipmap_mode;

typedef enum vita_gpu_address_mode {
    VITA_GPU_ADDRESS_REPEAT = 0,
    VITA_GPU_ADDRESS_MIRROR_REPEAT = 1,
    VITA_GPU_ADDRESS_CLAMP_EDGE = 2,
    VITA_GPU_ADDRESS_CLAMP_BORDER = 3,
    VITA_GPU_ADDRESS_MODE_FORCE_U32 = 0x7fffffff
} vita_gpu_address_mode;

typedef enum vita_gpu_compare_op {
    VITA_GPU_COMPARE_NEVER = 0,
    VITA_GPU_COMPARE_LESS = 1,
    VITA_GPU_COMPARE_EQUAL = 2,
    VITA_GPU_COMPARE_LESS_EQUAL = 3,
    VITA_GPU_COMPARE_GREATER = 4,
    VITA_GPU_COMPARE_NOT_EQUAL = 5,
    VITA_GPU_COMPARE_GREATER_EQUAL = 6,
    VITA_GPU_COMPARE_ALWAYS = 7,
    VITA_GPU_COMPARE_OP_FORCE_U32 = 0x7fffffff
} vita_gpu_compare_op;

typedef struct vita_gpu_sampler_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    const char *label;
    vita_gpu_filter min_filter;
    vita_gpu_filter mag_filter;
    vita_gpu_mipmap_mode mipmap_mode;
    vita_gpu_address_mode address_u;
    vita_gpu_address_mode address_v;
    vita_gpu_address_mode address_w;
    float mip_lod_bias;
    float min_lod;
    float max_lod;
    uint32_t max_anisotropy;
    vita_gpu_bool compare_enable;
    vita_gpu_compare_op compare_op;
    float border_color[4];
} vita_gpu_sampler_desc;

typedef struct vita_gpu_render_target_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    const char *label;
    vita_gpu_texture texture;
    vita_gpu_format format;
    uint32_t mip_level;
    uint32_t first_array_layer;
    uint32_t array_layer_count;
} vita_gpu_render_target_desc;

typedef struct vita_gpu_depth_target_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    const char *label;
    vita_gpu_texture texture;
    vita_gpu_format format;
    uint32_t mip_level;
    uint32_t first_array_layer;
    uint32_t array_layer_count;
    vita_gpu_bool read_only_depth;
    vita_gpu_bool read_only_stencil;
} vita_gpu_depth_target_desc;

typedef enum vita_gpu_shader_stage {
    VITA_GPU_SHADER_VERTEX = 0,
    VITA_GPU_SHADER_PIXEL = 1,
    VITA_GPU_SHADER_COMPUTE = 2,
    VITA_GPU_SHADER_STAGE_FORCE_U32 = 0x7fffffff
} vita_gpu_shader_stage;

typedef uint64_t vita_gpu_shader_stage_flags;
#define VITA_GPU_SHADER_STAGE_VERTEX_BIT  (UINT64_C(1) << 0)
#define VITA_GPU_SHADER_STAGE_PIXEL_BIT   (UINT64_C(1) << 1)
#define VITA_GPU_SHADER_STAGE_COMPUTE_BIT (UINT64_C(1) << 2)
#define VITA_GPU_SHADER_STAGE_ALL_GRAPHICS \
    (VITA_GPU_SHADER_STAGE_VERTEX_BIT | VITA_GPU_SHADER_STAGE_PIXEL_BIT)
#define VITA_GPU_SHADER_STAGE_ALL \
    (VITA_GPU_SHADER_STAGE_ALL_GRAPHICS | VITA_GPU_SHADER_STAGE_COMPUTE_BIT)

typedef enum vita_gpu_shader_target {
    VITA_GPU_SHADER_TARGET_DXIL = 0,
    VITA_GPU_SHADER_TARGET_SPIRV = 1,
    VITA_GPU_SHADER_TARGET_FORCE_U32 = 0x7fffffff
} vita_gpu_shader_target;

typedef uint64_t vita_gpu_shader_flags;
#define VITA_GPU_SHADER_DEBUG             (UINT64_C(1) << 0)
#define VITA_GPU_SHADER_DISABLE_OPTIMIZE  (UINT64_C(1) << 1)
#define VITA_GPU_SHADER_WARNINGS_AS_ERROR (UINT64_C(1) << 2)

typedef struct vita_gpu_shader_define {
    uint32_t struct_size;
    uint32_t struct_version;
    const char *name;
    const char *value;
} vita_gpu_shader_define;

typedef struct vita_gpu_shader_request_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    const char *source_name;
    const char *source;
    uint64_t source_size;
    const char *entry_point;
    const char *profile;
    const vita_gpu_shader_define *defines;
    uint32_t define_count;
    vita_gpu_shader_stage stage;
    vita_gpu_shader_target target;
    vita_gpu_shader_flags flags;
} vita_gpu_shader_request_desc;

typedef struct vita_gpu_shader_key {
    uint32_t struct_size;
    uint32_t struct_version;
    uint8_t bytes[VITA_GPU_SHADER_KEY_SIZE];
} vita_gpu_shader_key;

typedef struct vita_gpu_shader_binary_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    vita_gpu_shader_stage stage;
    vita_gpu_shader_target target;
    const uint8_t *data;
    uint64_t data_size;
    vita_gpu_shader_key key;
} vita_gpu_shader_binary_desc;

typedef enum vita_gpu_shader_input {
    VITA_GPU_SHADER_INPUT_HLSL = 0,
    VITA_GPU_SHADER_INPUT_BINARY = 1,
    VITA_GPU_SHADER_INPUT_FORCE_U32 = 0x7fffffff
} vita_gpu_shader_input;

typedef struct vita_gpu_shader_module_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    const char *label;
    vita_gpu_shader_input input;
    const vita_gpu_shader_request_desc *hlsl;
    const vita_gpu_shader_binary_desc *binary;
} vita_gpu_shader_module_desc;

typedef enum vita_gpu_binding_type {
    VITA_GPU_BINDING_CONSTANT_BUFFER = 0,
    VITA_GPU_BINDING_STORAGE_BUFFER = 1,
    VITA_GPU_BINDING_SAMPLED_TEXTURE = 2,
    VITA_GPU_BINDING_STORAGE_TEXTURE = 3,
    VITA_GPU_BINDING_SAMPLER = 4,
    VITA_GPU_BINDING_TYPE_FORCE_U32 = 0x7fffffff
} vita_gpu_binding_type;

typedef struct vita_gpu_binding_layout_entry {
    uint32_t struct_size;
    uint32_t struct_version;
    uint32_t binding;
    uint32_t array_count;
    vita_gpu_binding_type type;
    vita_gpu_shader_stage_flags visibility;
} vita_gpu_binding_layout_entry;

typedef struct vita_gpu_binding_layout_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    const char *label;
    uint32_t set_index;
    const vita_gpu_binding_layout_entry *entries;
    uint32_t entry_count;
} vita_gpu_binding_layout_desc;

typedef struct vita_gpu_binding_set_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    const char *label;
    vita_gpu_binding_layout layout;
} vita_gpu_binding_set_desc;

typedef struct vita_gpu_binding_write {
    uint32_t struct_size;
    uint32_t struct_version;
    uint32_t binding;
    uint32_t array_element;
    vita_gpu_binding_type type;
    vita_gpu_buffer buffer;
    uint64_t buffer_offset;
    uint64_t buffer_size;
    vita_gpu_texture texture;
    vita_gpu_resource_state texture_state;
    vita_gpu_sampler sampler;
} vita_gpu_binding_write;

typedef enum vita_gpu_vertex_step {
    VITA_GPU_VERTEX_PER_VERTEX = 0,
    VITA_GPU_VERTEX_PER_INSTANCE = 1,
    VITA_GPU_VERTEX_STEP_FORCE_U32 = 0x7fffffff
} vita_gpu_vertex_step;

typedef struct vita_gpu_vertex_binding_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    uint32_t binding;
    uint32_t stride;
    vita_gpu_vertex_step step;
} vita_gpu_vertex_binding_desc;

typedef struct vita_gpu_vertex_attribute_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    uint32_t location;
    uint32_t binding;
    vita_gpu_format format;
    uint32_t offset;
} vita_gpu_vertex_attribute_desc;

typedef enum vita_gpu_primitive_topology {
    VITA_GPU_TOPOLOGY_POINT_LIST = 0,
    VITA_GPU_TOPOLOGY_LINE_LIST = 1,
    VITA_GPU_TOPOLOGY_LINE_STRIP = 2,
    VITA_GPU_TOPOLOGY_TRIANGLE_LIST = 3,
    VITA_GPU_TOPOLOGY_TRIANGLE_STRIP = 4,
    VITA_GPU_TOPOLOGY_FORCE_U32 = 0x7fffffff
} vita_gpu_primitive_topology;

typedef enum vita_gpu_polygon_mode {
    VITA_GPU_POLYGON_FILL = 0,
    VITA_GPU_POLYGON_LINE = 1,
    VITA_GPU_POLYGON_MODE_FORCE_U32 = 0x7fffffff
} vita_gpu_polygon_mode;

typedef enum vita_gpu_cull_mode {
    VITA_GPU_CULL_NONE = 0,
    VITA_GPU_CULL_FRONT = 1,
    VITA_GPU_CULL_BACK = 2,
    VITA_GPU_CULL_MODE_FORCE_U32 = 0x7fffffff
} vita_gpu_cull_mode;

typedef enum vita_gpu_front_face {
    VITA_GPU_FRONT_COUNTER_CLOCKWISE = 0,
    VITA_GPU_FRONT_CLOCKWISE = 1,
    VITA_GPU_FRONT_FACE_FORCE_U32 = 0x7fffffff
} vita_gpu_front_face;

typedef struct vita_gpu_rasterizer_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    vita_gpu_polygon_mode polygon_mode;
    vita_gpu_cull_mode cull_mode;
    vita_gpu_front_face front_face;
    int32_t depth_bias;
    float depth_bias_clamp;
    float slope_scaled_depth_bias;
} vita_gpu_rasterizer_desc;

typedef enum vita_gpu_blend_factor {
    VITA_GPU_BLEND_ZERO = 0,
    VITA_GPU_BLEND_ONE = 1,
    VITA_GPU_BLEND_SRC_COLOR = 2,
    VITA_GPU_BLEND_INV_SRC_COLOR = 3,
    VITA_GPU_BLEND_SRC_ALPHA = 4,
    VITA_GPU_BLEND_INV_SRC_ALPHA = 5,
    VITA_GPU_BLEND_DST_COLOR = 6,
    VITA_GPU_BLEND_INV_DST_COLOR = 7,
    VITA_GPU_BLEND_DST_ALPHA = 8,
    VITA_GPU_BLEND_INV_DST_ALPHA = 9,
    VITA_GPU_BLEND_FACTOR_FORCE_U32 = 0x7fffffff
} vita_gpu_blend_factor;

typedef enum vita_gpu_blend_op {
    VITA_GPU_BLEND_OP_ADD = 0,
    VITA_GPU_BLEND_OP_SUBTRACT = 1,
    VITA_GPU_BLEND_OP_REVERSE_SUBTRACT = 2,
    VITA_GPU_BLEND_OP_MIN = 3,
    VITA_GPU_BLEND_OP_MAX = 4,
    VITA_GPU_BLEND_OP_FORCE_U32 = 0x7fffffff
} vita_gpu_blend_op;

typedef struct vita_gpu_blend_attachment_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    vita_gpu_bool blend_enable;
    vita_gpu_blend_factor source_color;
    vita_gpu_blend_factor destination_color;
    vita_gpu_blend_op color_op;
    vita_gpu_blend_factor source_alpha;
    vita_gpu_blend_factor destination_alpha;
    vita_gpu_blend_op alpha_op;
    uint32_t color_write_mask;
} vita_gpu_blend_attachment_desc;

typedef struct vita_gpu_depth_stencil_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    vita_gpu_bool depth_test_enable;
    vita_gpu_bool depth_write_enable;
    vita_gpu_compare_op depth_compare;
    vita_gpu_bool stencil_enable;
    uint8_t stencil_read_mask;
    uint8_t stencil_write_mask;
    uint16_t reserved;
} vita_gpu_depth_stencil_desc;

typedef struct vita_gpu_graphics_pipeline_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    const char *label;
    vita_gpu_shader_module vertex_shader;
    vita_gpu_shader_module pixel_shader;
    const vita_gpu_binding_layout *binding_layouts;
    uint32_t binding_layout_count;
    const vita_gpu_vertex_binding_desc *vertex_bindings;
    uint32_t vertex_binding_count;
    const vita_gpu_vertex_attribute_desc *vertex_attributes;
    uint32_t vertex_attribute_count;
    vita_gpu_primitive_topology topology;
    vita_gpu_rasterizer_desc rasterizer;
    vita_gpu_depth_stencil_desc depth_stencil;
    uint32_t color_target_count;
    vita_gpu_format color_formats[VITA_GPU_MAX_COLOR_TARGETS];
    vita_gpu_blend_attachment_desc blend[VITA_GPU_MAX_COLOR_TARGETS];
    vita_gpu_format depth_format;
    uint32_t sample_count;
    uint32_t push_constant_bytes;
} vita_gpu_graphics_pipeline_desc;

typedef struct vita_gpu_compute_pipeline_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    const char *label;
    vita_gpu_shader_module compute_shader;
    const vita_gpu_binding_layout *binding_layouts;
    uint32_t binding_layout_count;
    uint32_t push_constant_bytes;
} vita_gpu_compute_pipeline_desc;

typedef enum vita_gpu_index_format {
    VITA_GPU_INDEX_UINT16 = 0,
    VITA_GPU_INDEX_UINT32 = 1,
    VITA_GPU_INDEX_FORMAT_FORCE_U32 = 0x7fffffff
} vita_gpu_index_format;

typedef enum vita_gpu_load_op {
    VITA_GPU_LOAD_PRESERVE = 0,
    VITA_GPU_LOAD_CLEAR = 1,
    VITA_GPU_LOAD_DISCARD = 2,
    VITA_GPU_LOAD_OP_FORCE_U32 = 0x7fffffff
} vita_gpu_load_op;

typedef enum vita_gpu_store_op {
    VITA_GPU_STORE_PRESERVE = 0,
    VITA_GPU_STORE_DISCARD = 1,
    VITA_GPU_STORE_OP_FORCE_U32 = 0x7fffffff
} vita_gpu_store_op;

typedef struct vita_gpu_color_attachment {
    uint32_t struct_size;
    uint32_t struct_version;
    vita_gpu_render_target target;
    vita_gpu_load_op load_op;
    vita_gpu_store_op store_op;
    float clear_color[4];
} vita_gpu_color_attachment;

typedef struct vita_gpu_depth_attachment {
    uint32_t struct_size;
    uint32_t struct_version;
    vita_gpu_depth_target target;
    vita_gpu_load_op depth_load_op;
    vita_gpu_store_op depth_store_op;
    vita_gpu_load_op stencil_load_op;
    vita_gpu_store_op stencil_store_op;
    float clear_depth;
    uint32_t clear_stencil;
} vita_gpu_depth_attachment;

typedef struct vita_gpu_render_pass_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    const vita_gpu_color_attachment *color_attachments;
    uint32_t color_attachment_count;
    const vita_gpu_depth_attachment *depth_attachment;
} vita_gpu_render_pass_desc;

typedef struct vita_gpu_viewport {
    uint32_t struct_size;
    uint32_t struct_version;
    float x;
    float y;
    float width;
    float height;
    float min_depth;
    float max_depth;
} vita_gpu_viewport;

typedef struct vita_gpu_scissor {
    uint32_t struct_size;
    uint32_t struct_version;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
} vita_gpu_scissor;

typedef enum vita_gpu_resource_kind {
    VITA_GPU_RESOURCE_BUFFER = 0,
    VITA_GPU_RESOURCE_TEXTURE = 1,
    VITA_GPU_RESOURCE_KIND_FORCE_U32 = 0x7fffffff
} vita_gpu_resource_kind;

typedef struct vita_gpu_resource_barrier {
    uint32_t struct_size;
    uint32_t struct_version;
    vita_gpu_resource_kind kind;
    vita_gpu_buffer buffer;
    vita_gpu_texture texture;
    vita_gpu_resource_state before;
    vita_gpu_resource_state after;
    uint32_t first_mip;
    uint32_t mip_count;
    uint32_t first_array_layer;
    uint32_t array_layer_count;
    vita_gpu_queue_type source_queue;
    vita_gpu_queue_type destination_queue;
} vita_gpu_resource_barrier;

typedef struct vita_gpu_vertex_buffer_binding {
    uint32_t struct_size;
    uint32_t struct_version;
    uint32_t binding;
    vita_gpu_buffer buffer;
    uint64_t offset;
} vita_gpu_vertex_buffer_binding;

typedef struct vita_gpu_index_buffer_binding {
    uint32_t struct_size;
    uint32_t struct_version;
    vita_gpu_buffer buffer;
    uint64_t offset;
    vita_gpu_index_format format;
} vita_gpu_index_buffer_binding;

typedef struct vita_gpu_push_constants_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    vita_gpu_shader_stage_flags stages;
    uint32_t offset;
    const uint8_t *data;
    uint32_t data_size;
} vita_gpu_push_constants_desc;

typedef struct vita_gpu_draw_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    uint32_t vertex_count;
    uint32_t instance_count;
    uint32_t first_vertex;
    uint32_t first_instance;
} vita_gpu_draw_desc;

typedef struct vita_gpu_draw_indexed_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    uint32_t index_count;
    uint32_t instance_count;
    uint32_t first_index;
    int32_t vertex_offset;
    uint32_t first_instance;
} vita_gpu_draw_indexed_desc;

typedef struct vita_gpu_dispatch_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    uint32_t group_count_x;
    uint32_t group_count_y;
    uint32_t group_count_z;
} vita_gpu_dispatch_desc;

typedef struct vita_gpu_buffer_copy_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    vita_gpu_buffer source;
    uint64_t source_offset;
    vita_gpu_buffer destination;
    uint64_t destination_offset;
    uint64_t byte_size;
} vita_gpu_buffer_copy_desc;

typedef struct vita_gpu_readback_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    const char *label;
    uint64_t byte_size;
} vita_gpu_readback_desc;

typedef struct vita_gpu_buffer_readback_copy_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    vita_gpu_buffer source;
    uint64_t source_offset;
    vita_gpu_readback destination;
    uint64_t destination_offset;
    uint64_t byte_size;
} vita_gpu_buffer_readback_copy_desc;

typedef struct vita_gpu_texture_readback_copy_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    vita_gpu_texture source;
    uint32_t mip_level;
    uint32_t array_layer;
    vita_gpu_readback destination;
    uint64_t destination_offset;
    uint32_t row_pitch;
    uint32_t slice_pitch;
} vita_gpu_texture_readback_copy_desc;

typedef struct vita_gpu_readback_map {
    uint32_t struct_size;
    uint32_t struct_version;
    const uint8_t *data;
    uint64_t byte_size;
    uint32_t row_pitch;
    uint32_t slice_pitch;
} vita_gpu_readback_map;

typedef enum vita_gpu_window_system {
    VITA_GPU_WINDOW_WIN32 = 0,
    VITA_GPU_WINDOW_SYSTEM_FORCE_U32 = 0x7fffffff
} vita_gpu_window_system;

typedef struct vita_gpu_window_handle {
    uint32_t struct_size;
    uint32_t struct_version;
    vita_gpu_window_system system;
    void *native_window;
} vita_gpu_window_handle;

typedef enum vita_gpu_present_mode {
    VITA_GPU_PRESENT_FIFO = 0,
    VITA_GPU_PRESENT_IMMEDIATE = 1,
    VITA_GPU_PRESENT_MAILBOX = 2,
    VITA_GPU_PRESENT_MODE_FORCE_U32 = 0x7fffffff
} vita_gpu_present_mode;

typedef struct vita_gpu_swapchain_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    const char *label;
    vita_gpu_window_handle window;
    uint32_t width;
    uint32_t height;
    vita_gpu_format format;
    uint32_t image_count;
    vita_gpu_present_mode present_mode;
} vita_gpu_swapchain_desc;

typedef struct vita_gpu_swapchain_image {
    uint32_t struct_size;
    uint32_t struct_version;
    uint32_t image_index;
    vita_gpu_texture texture;
    vita_gpu_render_target render_target;
} vita_gpu_swapchain_image;

typedef struct vita_gpu_present_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    uint32_t image_index;
    vita_gpu_bool allow_tearing;
} vita_gpu_present_desc;

typedef struct vita_gpu_command_list_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    const char *label;
    vita_gpu_queue_type queue;
} vita_gpu_command_list_desc;

typedef struct vita_gpu_fence_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    const char *label;
    uint64_t initial_value;
} vita_gpu_fence_desc;

typedef struct vita_gpu_fence_point {
    uint32_t struct_size;
    uint32_t struct_version;
    vita_gpu_fence fence;
    uint64_t value;
} vita_gpu_fence_point;

typedef struct vita_gpu_submit_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    const vita_gpu_command_list *command_lists;
    uint32_t command_list_count;
    const vita_gpu_fence_point *waits;
    uint32_t wait_count;
    vita_gpu_fence_point signal;
} vita_gpu_submit_desc;

typedef uint64_t vita_gpu_debug_capture_flags;
#define VITA_GPU_DEBUG_CAPTURE_API (UINT64_C(1) << 0)
#define VITA_GPU_DEBUG_CAPTURE_FRAME (UINT64_C(1) << 1)

typedef struct vita_gpu_debug_capture_desc {
    uint32_t struct_size;
    uint32_t struct_version;
    const char *output_path;
    vita_gpu_debug_capture_flags flags;
} vita_gpu_debug_capture_desc;

VITA_GPU_API uint32_t vita_gpu_get_abi_version(void);
VITA_GPU_API const char *vita_gpu_result_string(vita_gpu_result result);
VITA_GPU_API void vita_gpu_error_reset(vita_gpu_error *error);

VITA_GPU_API uint32_t vita_gpu_backend_count(void);
VITA_GPU_API vita_gpu_result vita_gpu_get_backend_info(
    uint32_t index, vita_gpu_backend_info *info, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_backend_from_name(
    const char *name, vita_gpu_backend *backend, vita_gpu_error *error);
VITA_GPU_API const char *vita_gpu_backend_name(vita_gpu_backend backend);
VITA_GPU_API vita_gpu_result vita_gpu_adapter_count(
    vita_gpu_backend backend, uint32_t *count, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_get_adapter_info(
    vita_gpu_backend backend, uint32_t index, vita_gpu_adapter_info *info,
    vita_gpu_error *error);

VITA_GPU_API vita_gpu_result vita_gpu_create_device(
    const vita_gpu_device_desc *desc, vita_gpu_device *device,
    vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_destroy_device(
    vita_gpu_device device, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_device_get_capabilities(
    vita_gpu_device device, vita_gpu_device_capabilities *capabilities,
    vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_device_wait_idle(
    vita_gpu_device device, vita_gpu_error *error);

VITA_GPU_API vita_gpu_result vita_gpu_create_swapchain(
    vita_gpu_device device, const vita_gpu_swapchain_desc *desc,
    vita_gpu_swapchain *swapchain, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_destroy_swapchain(
    vita_gpu_device device, vita_gpu_swapchain swapchain,
    vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_swapchain_acquire(
    vita_gpu_device device, vita_gpu_swapchain swapchain,
    vita_gpu_swapchain_image *image, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_swapchain_resize(
    vita_gpu_device device, vita_gpu_swapchain swapchain, uint32_t width,
    uint32_t height, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_swapchain_present(
    vita_gpu_device device, vita_gpu_swapchain swapchain,
    const vita_gpu_present_desc *desc, vita_gpu_error *error);

VITA_GPU_API vita_gpu_result vita_gpu_create_buffer(
    vita_gpu_device device, const vita_gpu_buffer_desc *desc,
    vita_gpu_buffer *buffer, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_destroy_buffer(
    vita_gpu_device device, vita_gpu_buffer buffer, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_create_texture(
    vita_gpu_device device, const vita_gpu_texture_desc *desc,
    vita_gpu_texture *texture, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_destroy_texture(
    vita_gpu_device device, vita_gpu_texture texture, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_create_sampler(
    vita_gpu_device device, const vita_gpu_sampler_desc *desc,
    vita_gpu_sampler *sampler, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_destroy_sampler(
    vita_gpu_device device, vita_gpu_sampler sampler, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_create_render_target(
    vita_gpu_device device, const vita_gpu_render_target_desc *desc,
    vita_gpu_render_target *target, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_destroy_render_target(
    vita_gpu_device device, vita_gpu_render_target target,
    vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_create_depth_target(
    vita_gpu_device device, const vita_gpu_depth_target_desc *desc,
    vita_gpu_depth_target *target, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_destroy_depth_target(
    vita_gpu_device device, vita_gpu_depth_target target,
    vita_gpu_error *error);

VITA_GPU_API vita_gpu_result vita_gpu_shader_request_key(
    const vita_gpu_shader_request_desc *request, vita_gpu_shader_key *key,
    vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_create_shader_module(
    vita_gpu_device device, const vita_gpu_shader_module_desc *desc,
    vita_gpu_shader_module *shader, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_destroy_shader_module(
    vita_gpu_device device, vita_gpu_shader_module shader,
    vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_create_binding_layout(
    vita_gpu_device device, const vita_gpu_binding_layout_desc *desc,
    vita_gpu_binding_layout *layout, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_destroy_binding_layout(
    vita_gpu_device device, vita_gpu_binding_layout layout,
    vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_create_binding_set(
    vita_gpu_device device, const vita_gpu_binding_set_desc *desc,
    vita_gpu_binding_set *set, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_update_binding_set(
    vita_gpu_device device, vita_gpu_binding_set set,
    const vita_gpu_binding_write *writes, uint32_t write_count,
    vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_destroy_binding_set(
    vita_gpu_device device, vita_gpu_binding_set set, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_create_graphics_pipeline(
    vita_gpu_device device, const vita_gpu_graphics_pipeline_desc *desc,
    vita_gpu_pipeline *pipeline, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_create_compute_pipeline(
    vita_gpu_device device, const vita_gpu_compute_pipeline_desc *desc,
    vita_gpu_pipeline *pipeline, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_destroy_pipeline(
    vita_gpu_device device, vita_gpu_pipeline pipeline, vita_gpu_error *error);

VITA_GPU_API vita_gpu_result vita_gpu_create_readback(
    vita_gpu_device device, const vita_gpu_readback_desc *desc,
    vita_gpu_readback *readback, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_destroy_readback(
    vita_gpu_device device, vita_gpu_readback readback, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_map_readback(
    vita_gpu_device device, vita_gpu_readback readback,
    vita_gpu_readback_map *map, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_unmap_readback(
    vita_gpu_device device, vita_gpu_readback readback, vita_gpu_error *error);

VITA_GPU_API vita_gpu_result vita_gpu_create_command_list(
    vita_gpu_device device, const vita_gpu_command_list_desc *desc,
    vita_gpu_command_list *command_list, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_destroy_command_list(
    vita_gpu_device device, vita_gpu_command_list command_list,
    vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_command_begin(
    vita_gpu_device device, vita_gpu_command_list command_list,
    vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_command_end(
    vita_gpu_device device, vita_gpu_command_list command_list,
    vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_command_reset(
    vita_gpu_device device, vita_gpu_command_list command_list,
    vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_cmd_barriers(
    vita_gpu_device device, vita_gpu_command_list command_list,
    const vita_gpu_resource_barrier *barriers, uint32_t barrier_count,
    vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_cmd_begin_render_pass(
    vita_gpu_device device, vita_gpu_command_list command_list,
    const vita_gpu_render_pass_desc *desc, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_cmd_end_render_pass(
    vita_gpu_device device, vita_gpu_command_list command_list,
    vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_cmd_bind_pipeline(
    vita_gpu_device device, vita_gpu_command_list command_list,
    vita_gpu_pipeline pipeline, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_cmd_bind_set(
    vita_gpu_device device, vita_gpu_command_list command_list,
    uint32_t set_index, vita_gpu_binding_set set, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_cmd_set_viewport(
    vita_gpu_device device, vita_gpu_command_list command_list,
    const vita_gpu_viewport *viewport, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_cmd_set_scissor(
    vita_gpu_device device, vita_gpu_command_list command_list,
    const vita_gpu_scissor *scissor, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_cmd_set_vertex_buffers(
    vita_gpu_device device, vita_gpu_command_list command_list,
    const vita_gpu_vertex_buffer_binding *bindings, uint32_t binding_count,
    vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_cmd_set_index_buffer(
    vita_gpu_device device, vita_gpu_command_list command_list,
    const vita_gpu_index_buffer_binding *binding, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_cmd_push_constants(
    vita_gpu_device device, vita_gpu_command_list command_list,
    const vita_gpu_push_constants_desc *desc, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_cmd_draw(
    vita_gpu_device device, vita_gpu_command_list command_list,
    const vita_gpu_draw_desc *desc, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_cmd_draw_indexed(
    vita_gpu_device device, vita_gpu_command_list command_list,
    const vita_gpu_draw_indexed_desc *desc, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_cmd_dispatch(
    vita_gpu_device device, vita_gpu_command_list command_list,
    const vita_gpu_dispatch_desc *desc, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_cmd_copy_buffer(
    vita_gpu_device device, vita_gpu_command_list command_list,
    const vita_gpu_buffer_copy_desc *desc, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_cmd_copy_buffer_to_readback(
    vita_gpu_device device, vita_gpu_command_list command_list,
    const vita_gpu_buffer_readback_copy_desc *desc, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_cmd_copy_texture_to_readback(
    vita_gpu_device device, vita_gpu_command_list command_list,
    const vita_gpu_texture_readback_copy_desc *desc, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_cmd_debug_marker(
    vita_gpu_device device, vita_gpu_command_list command_list,
    const char *label, uint32_t rgba_color, vita_gpu_error *error);

VITA_GPU_API vita_gpu_result vita_gpu_create_fence(
    vita_gpu_device device, const vita_gpu_fence_desc *desc,
    vita_gpu_fence *fence, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_destroy_fence(
    vita_gpu_device device, vita_gpu_fence fence, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_fence_completed_value(
    vita_gpu_device device, vita_gpu_fence fence, uint64_t *value,
    vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_fence_wait(
    vita_gpu_device device, vita_gpu_fence fence, uint64_t value,
    uint64_t timeout_nanoseconds, vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_submit(
    vita_gpu_device device, vita_gpu_queue_type queue,
    const vita_gpu_submit_desc *desc, vita_gpu_error *error);

VITA_GPU_API vita_gpu_result vita_gpu_debug_capture_begin(
    vita_gpu_device device, const vita_gpu_debug_capture_desc *desc,
    vita_gpu_error *error);
VITA_GPU_API vita_gpu_result vita_gpu_debug_capture_end(
    vita_gpu_device device, vita_gpu_error *error);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VITARECOMP_GPU_H */
