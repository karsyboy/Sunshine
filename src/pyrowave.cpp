/**
 * @file src/pyrowave.cpp
 * @brief DMA-BUF to PyroWave scaling/encoding with explicit synchronization.
 */
#include "pyrowave.h"

#include "config.h"
#include "platform/linux/graphics.h"

#include <cstring>
#include <drm_fourcc.h>
#include <linux/dma-buf.h>
#include <mutex>
#include <PyroWave.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace video {
  pyrowave_session_t::~pyrowave_session_t() {
    if (encoder) {
      pyrowave_encoder_destroy(encoder);
    }
    if (device) {
      pyrowave_device_destroy(device);
    }
  }

  /**
   * @brief Create a Vulkan encoder device on Sunshine's selected capture GPU.
   * @param device Receives the owned device.
   * @return Standalone API result.
   */
  static pyrowave_result create_capture_device(pyrowave_device *device) {
    auto path = platf::resolve_render_device();
    VkApplicationInfo app {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    VkInstance instance {};
    if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS) {
      return PYROWAVE_ERROR_INVALID_ARGUMENT;
    }
    auto guard = util::fail_guard([&] {
      vkDestroyInstance(instance, nullptr);
    });
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || !count) {
      return PYROWAVE_ERROR_INVALID_ARGUMENT;
    }
    std::vector<VkPhysicalDevice> devices(count);
    if (vkEnumeratePhysicalDevices(instance, &count, devices.data()) != VK_SUCCESS) {
      return PYROWAVE_ERROR_INVALID_ARGUMENT;
    }
    struct stat node {};
    const bool node_path = !path.empty() && path.front() == '/';
    if (node_path && stat(path.c_str(), &node) < 0) {
      return PYROWAVE_ERROR_INVALID_ARGUMENT;
    }
    for (uint32_t i = 0; i < count; ++i) {
      VkPhysicalDeviceIDProperties id {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
      VkPhysicalDeviceDrmPropertiesEXT drm {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
      id.pNext = &drm;
      VkPhysicalDeviceProperties2 properties {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
      properties.pNext = &id;
      vkGetPhysicalDeviceProperties2(devices[i], &properties);
      bool match = node_path ? drm.hasRender && drm.renderMajor == int64_t(major(node.st_rdev)) && drm.renderMinor == int64_t(minor(node.st_rdev)) :
                               path.empty() || path == std::to_string(i) || std::string(properties.properties.deviceName).find(path) != std::string::npos;
      if (!match) {
        continue;
      }
      pyrowave_uuid uuid {}, driver {};
      std::memcpy(uuid.uuid, id.deviceUUID, VK_UUID_SIZE);
      std::memcpy(driver.uuid, id.driverUUID, VK_UUID_SIZE);
      BOOST_LOG(info) << "PyroWave capture GPU: " << properties.properties.deviceName;
      return pyrowave_create_device_by_compat(0, 0, &uuid, &driver, nullptr, device);
    }
    BOOST_LOG(error) << "No Vulkan device matches capture adapter: " << path;
    return PYROWAVE_ERROR_INVALID_ARGUMENT;
  }

  bool probe_pyrowave() {
    // Avoid allocating an encoder on every HTTP/RTSP capability request.
    static std::mutex mutex;
    static std::string adapter;
    static std::chrono::steady_clock::time_point checked {};
    static bool supported = false;
    std::lock_guard<std::mutex> lock(mutex);
    auto now = std::chrono::steady_clock::now();
    auto requested_adapter = platf::resolve_render_device();
    if (requested_adapter == adapter && now - checked < std::chrono::seconds(30)) {
      return supported;
    }
    pyrowave_session_t session;
    config_t config {};
    config.width = config.height = 128;
    config.framerate = 60;
    config.bitrate = 200000;
    supported = session.init(config, false) == 0;
    checked = now;
    adapter = std::move(requested_adapter);
    return supported;
  }

  int pyrowave_session_t::init(const config_t &config, bool hdr) {
    uint32_t major, minor, patch;
    pyrowave_get_api_version(&major, &minor, &patch);
    if (major != 0 || minor != 7) {
      BOOST_LOG(error) << "PyroWave requires the pinned API 0.7 build (color metadata and packet validation)";
      return -1;
    }
    rate.maximum_bitstream_size = LiPyroWaveFrameBudget(config.bitrate, config.framerate);
    if (!rate.maximum_bitstream_size || config.width <= 0 || config.height <= 0 || config.width > 8192 || config.height > 8192) {
      BOOST_LOG(error) << "Invalid PyroWave dimensions or bitrate/fps; maximum frame size is 3 MiB";
      return -1;
    }
    frame_rate = config.framerate;
    hdr_input = hdr;
    hdr_output = config.dynamicRange != 0;
    if (hdr_output && !hdr_input) {
      BOOST_LOG(error) << "PyroWave HDR requires a PQ HDR capture source";
      return -1;
    }
    // The C API owns a dedicated Vulkan device. DMA-BUF import avoids pixel readback.
    if (create_capture_device(&device) != PYROWAVE_SUCCESS) {
      BOOST_LOG(error) << "Failed to create PyroWave Vulkan device";
      return -1;
    }
    pyrowave_encoder_create_info info {};
    info.device = device;
    info.width = config.width;
    info.height = config.height;
    info.chroma = config.chromaSamplingType ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420;
    if (pyrowave_encoder_create(&info, &encoder) != PYROWAVE_SUCCESS) {
      BOOST_LOG(error) << "Failed to create PyroWave encoder (check subgroup and storageBuffer8BitAccess support)";
      return -1;
    }
    // Seed a black intra frame without requiring capture to have supplied a DMA-BUF yet.
    const size_t pixels = size_t(config.width) * config.height;
    std::vector<uint8_t> black(pixels, 0), neutral(config.chromaSamplingType ? pixels : pixels / 4, 128);
    pyrowave_cpu_buffer buffer {};
    buffer.width = config.width;
    buffer.height = config.height;
    buffer.format = config.chromaSamplingType ? PYROWAVE_CPU_BUFFER_FORMAT_YUV444P : PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
    buffer.data[0] = black.data();
    buffer.data[1] = buffer.data[2] = neutral.data();
    buffer.row_stride_in_bytes[0] = config.width;
    buffer.row_stride_in_bytes[1] = buffer.row_stride_in_bytes[2] = config.chromaSamplingType ? config.width : config.width / 2;
    buffer.plane_size_in_bytes[0] = black.size();
    buffer.plane_size_in_bytes[1] = buffer.plane_size_in_bytes[2] = neutral.size();
    pyrowave_color_metadata color {hdr_output, hdr_output, hdr_output, 0, 0};
    if (pyrowave_encoder_set_color_metadata(encoder, &color) != PYROWAVE_SUCCESS || pyrowave_encoder_encode_cpu_synchronous(encoder, &buffer, &rate) != PYROWAVE_SUCCESS) {
      return -1;
    }
    return packetize();
  }

  int pyrowave_session_t::packetize() {
    size_t count = 0;
    if (pyrowave_encoder_compute_num_packets(encoder, PYROWAVE_MAX_FRAME_BYTES, &count) != PYROWAVE_SUCCESS || count == 0 || count > 2) {
      return -1;
    }
    std::vector<pyrowave_packet> packets(count);
    encoded.resize(PYROWAVE_MAX_FRAME_BYTES);
    size_t written = 0;
    if (pyrowave_encoder_packetize(encoder, packets.data(), PYROWAVE_MAX_FRAME_BYTES, &written, encoded.data(), encoded.size()) != PYROWAVE_SUCCESS || written != 1 || packets[0].offset != 0 || packets[0].size < 8 || packets[0].size > encoded.size()) {
      BOOST_LOG(error) << "PyroWave encoded frame exceeds the transport limit";
      return -1;
    }
    encoded.resize(packets[0].size);
    return 0;
  }

  int pyrowave_session_t::convert(platf::img_t &img) {
    auto *descriptor = dynamic_cast<egl::img_descriptor_t *>(&img);
    if (!descriptor) {
      BOOST_LOG(error) << "PyroWave requires DMA-BUF capture (KMS, KWin or XDG portal); select a VAAPI/Vulkan capture path";
      return -1;
    }
    if (descriptor->sequence == 0) {
      return 0;
    }
    auto &sd = descriptor->sd;
    if (descriptor->y_invert || sd.width <= 0 || sd.height <= 0 || sd.modifier == DRM_FORMAT_MOD_INVALID || sd.fds[0] < 0) {
      BOOST_LOG(error) << "Unsupported PyroWave DMA-BUF descriptor (modifier, orientation or extent)";
      return -1;
    }
    VkFormat format;
    switch (sd.fourcc) {
      case DRM_FORMAT_XRGB8888:
      case DRM_FORMAT_ARGB8888:
        format = VK_FORMAT_B8G8R8A8_UNORM;
        break;
      case DRM_FORMAT_XBGR8888:
      case DRM_FORMAT_ABGR8888:
        format = VK_FORMAT_R8G8B8A8_UNORM;
        break;
      case DRM_FORMAT_XRGB2101010:
      case DRM_FORMAT_ARGB2101010:
        format = VK_FORMAT_A2R10G10B10_UNORM_PACK32;
        break;
      case DRM_FORMAT_XBGR2101010:
      case DRM_FORMAT_ABGR2101010:
        format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        break;
      default:
        BOOST_LOG(error) << "Unsupported PyroWave capture DRM format: " << sd.fourcc;
        return -1;
    }
    if (hdr_output && (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_R8G8B8A8_UNORM)) {
      BOOST_LOG(error) << "PyroWave HDR capture must provide at least 10-bit RGB";
      return -1;
    }
    // All modifier planes (including AMD DCC metadata) must share the allocation.
    std::array<VkSubresourceLayout, 4> layouts {};
    uint32_t planes = 0;
    struct stat base {};
    if (fstat(sd.fds[0], &base) < 0) {
      return -1;
    }
    for (; planes < 4 && sd.fds[planes] >= 0; ++planes) {
      struct stat plane {};
      if (fstat(sd.fds[planes], &plane) < 0 || plane.st_dev != base.st_dev || plane.st_ino != base.st_ino) {
        return -1;
      }
      layouts[planes].offset = sd.offsets[planes];
      layouts[planes].rowPitch = sd.pitches[planes];
    }
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
    modifier.drmFormatModifier = sd.modifier;
    modifier.drmFormatModifierPlaneCount = planes;
    modifier.pPlaneLayouts = layouts.data();
    VkImageCreateInfo image_info {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.pNext = &modifier;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {uint32_t(sd.width), uint32_t(sd.height), 1};
    image_info.mipLevels = image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    pyrowave_image_create_info imported {device, pyrowave_os_handle(dup(sd.fds[0])), VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, &image_info};
    if (int(imported.external_handle) < 0) {
      return -1;
    }
    pyrowave_image image {};
    auto result = pyrowave_image_create(&imported, &image);
    if (result != PYROWAVE_SUCCESS) {
      close(int(imported.external_handle));
      BOOST_LOG(error) << "PyroWave DMA-BUF import failed: " << result;
      return -1;
    }
    auto image_guard = util::fail_guard([&] {
      pyrowave_image_destroy(image);
    });
    // Export implicit DMA-BUF fences and wait on the GPU before sampling.
    dma_buf_export_sync_file fence {};
    fence.flags = DMA_BUF_SYNC_READ;
    if (ioctl(sd.fds[0], DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &fence) < 0) {
      BOOST_LOG(error) << "Cannot export capture DMA-BUF synchronization fence";
      return -1;
    }
    pyrowave_sync_object_create_info sync_info {};
    sync_info.device = device;
    sync_info.external_handle = fence.fd;
    sync_info.handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    sync_info.semaphore_type = VK_SEMAPHORE_TYPE_BINARY;
    sync_info.import_flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
    pyrowave_sync_object sync {};
    if (pyrowave_sync_object_create(&sync_info, &sync) != PYROWAVE_SUCCESS) {
      close(fence.fd);
      return -1;
    }
    auto sync_guard = util::fail_guard([&] {
      pyrowave_sync_object_destroy(sync);
    });
    pyrowave_gpu_external_reference reference {image, VK_QUEUE_FAMILY_FOREIGN_EXT};
    pyrowave_gpu_sync_operation acquire {&reference, 1, {pyrowave_sync_object_get_semaphore(sync), 0}};
    pyrowave_gpu_sync_operation release {&reference, 1, {}};
    pyrowave_scaled_encode_info scaled {};
    if (pyrowave_image_get_image_view(image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, &scaled.view) != PYROWAVE_SUCCESS) {
      return -1;
    }
    scaled.input_color_space = hdr_input ? VK_COLOR_SPACE_HDR10_ST2084_EXT : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    scaled.output_color_space = hdr_output ? VK_COLOR_SPACE_HDR10_ST2084_EXT : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    // HDR -> SDR conversion isn't a tone-map in PyroWave's scaler. Reject it.
    if (hdr_input && !hdr_output) {
      BOOST_LOG(error) << "Disable HDR on the capture display for SDR PyroWave";
      return -1;
    }
    scaled.intermediate_plane_format = VK_FORMAT_R16_UNORM;
    scaled.ycbcr_chroma_midpoint = 0.5f;
    auto start = std::chrono::steady_clock::now();
    if (img.frame_timestamp) {
      BOOST_LOG(debug) << "PyroWave capture-to-input us: " << std::chrono::duration_cast<std::chrono::microseconds>(start - *img.frame_timestamp).count();
    }
    result = pyrowave_encoder_encode_gpu_scaled_synchronous(encoder, &acquire, &release, &scaled, &rate);
    if (result != PYROWAVE_SUCCESS || packetize()) {
      BOOST_LOG(error) << "PyroWave encode failed: " << result;
      return -1;
    }
    // Packetization waits only for this encode fence. Capture cannot recycle the image earlier.
    BOOST_LOG(debug) << "PyroWave encode+readback us: " << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count() << ", bytes: " << encoded.size() << ", effective kbps: " << (uint64_t(encoded.size()) * 8 * frame_rate / 1000);
    if (config::sunshine.min_log_level <= 0 && start - last_stats >= std::chrono::seconds(1)) {
      pyrowave_device_report_performance_stats(device, [](void *, const char *message) {
        BOOST_LOG(debug) << "PyroWave GPU: " << message;
      },
                                               nullptr,
                                               true);
      last_stats = start;
    }
    return 0;
  }
}  // namespace video
