/**
 * @file src/pyrowave.h
 * @brief Native Vulkan PyroWave encoder session, independent of FFmpeg codecs.
 */
#pragma once
#include "video.h"

#include <cstdint>
#include <vulkan/vulkan.h>

// The standalone API requires Vulkan types to be declared first.
#include <pyrowave.h>

namespace video {
  /**
   * @brief Map a DRM RGB FourCC to the equivalent Vulkan image format.
   * @param fourcc DRM format supplied by the capture backend.
   * @return Vulkan format, or VK_FORMAT_UNDEFINED when unsupported.
   */
  VkFormat pyrowave_vk_format(std::uint32_t fourcc);

  /**
   * @brief Intra-only encoder that imports captured DMA-BUF images.
   */
  class pyrowave_session_t final: public encode_session_t {
  public:
    /**
     * @brief Release encoder before its Vulkan device.
     */
    ~pyrowave_session_t() override;
    /**
     * @brief Initialize a session.
     * @param config Negotiated video settings.
     * @param hdr Capture uses PQ.
     * @return Zero on success.
     */
    int init(const config_t &config, bool hdr);
    /**
     * @brief Import, scale and encode while capture owns the image.
     * @param img Captured image.
     * @return Zero on success.
     */
    int convert(platf::img_t &img) override;

    /**
     * @brief Every frame is an IDR; requests require no transition.
     */
    void request_idr_frame() override {}

    /**
     * @brief Every frame remains independently decodable.
     */
    void request_normal_frame() override {}

    /**
     * @brief No reference frames exist.
     * @param first_frame Ignored.
     * @param last_frame Ignored.
     */
    void invalidate_ref_frames(int64_t first_frame, int64_t last_frame) override {}

    /**
     * @brief Return the last complete frame, including its sequence header.
     * @return Encoded frame bytes.
     */
    const std::vector<uint8_t> &frame() const {
      return encoded;
    }

  private:
    /**
     * @brief Wait for encoding and packetize the complete frame.
     * @return Zero on success.
     */
    int packetize();
    pyrowave_device device {};  ///< Owned Vulkan device.
    pyrowave_encoder encoder {};  ///< Owned encoder.
    pyrowave_rate_control rate {};  ///< Maximum bytes per encoded frame.
    std::chrono::steady_clock::time_point last_stats {};  ///< Last GPU timing report.
    int frame_rate {};  ///< Negotiated frames per second.
    bool hdr_input {};  ///< Captured RGB is BT.2020/PQ.
    bool hdr_output {};  ///< Requested output is BT.2020/PQ.
    std::vector<uint8_t> encoded;  ///< Most recently encoded frame.
  };

  /**
   * @brief Probe standalone encoder features.
   * @return True when creation succeeds.
   */
  bool probe_pyrowave();
}  // namespace video
