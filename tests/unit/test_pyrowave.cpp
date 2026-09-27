/**
 * @file tests/unit/test_pyrowave.cpp
 * @brief Native encoder bounds and DMA-BUF GPU import tests.
 */
#include "../tests_common.h"
#ifdef SUNSHINE_BUILD_PYROWAVE
  #include "src/platform/linux/graphics.h"
  #include "src/pyrowave.h"

  #include <cstdlib>
  #include <cstring>
  #include <dlfcn.h>
  #include <drm_fourcc.h>
  #include <fcntl.h>
  #include <fstream>
  #include <gbm.h>
  #include <PyroWave.h>

/**
 * @brief Invalid dimensions and budgets must fail before creating a device.
 */
TEST(PyroWave, RejectInvalidConfiguration) {
  video::pyrowave_session_t session;
  video::config_t config {};
  config.width = config.height = 64;
  config.framerate = 60;
  config.bitrate = 2000001;
  EXPECT_NE(session.init(config, false), 0);
  config.bitrate = 200000;
  config.width = 8193;
  EXPECT_NE(session.init(config, false), 0);
  config.width = 64;
  config.dynamicRange = 1;
  EXPECT_NE(session.init(config, false), 0);
}

/**
 * @brief Test a real DMA-BUF imported by the production scaling/encode path.
 */
TEST(PyroWave, DmaBufSdrHdr420444) {
  if (!std::getenv("SUNSHINE_TEST_PYROWAVE_GPU")) {
    GTEST_SKIP() << "Opt-in GPU test";
  }
  auto library = dlopen("libgbm.so.1", RTLD_NOW | RTLD_LOCAL);
  ASSERT_NE(library, nullptr);
  auto library_guard = util::fail_guard([&] {
    dlclose(library);
  });
  auto create = reinterpret_cast<decltype(&gbm_create_device)>(dlsym(library, "gbm_create_device"));
  auto destroy = reinterpret_cast<decltype(&gbm_device_destroy)>(dlsym(library, "gbm_device_destroy"));
  auto create_bo = reinterpret_cast<decltype(&gbm_bo_create)>(dlsym(library, "gbm_bo_create"));
  auto destroy_bo = reinterpret_cast<decltype(&gbm_bo_destroy)>(dlsym(library, "gbm_bo_destroy"));
  auto map = reinterpret_cast<decltype(&gbm_bo_map)>(dlsym(library, "gbm_bo_map"));
  auto unmap = reinterpret_cast<decltype(&gbm_bo_unmap)>(dlsym(library, "gbm_bo_unmap"));
  auto get_fd = reinterpret_cast<decltype(&gbm_bo_get_fd)>(dlsym(library, "gbm_bo_get_fd"));
  auto get_stride = reinterpret_cast<decltype(&gbm_bo_get_stride)>(dlsym(library, "gbm_bo_get_stride"));
  auto get_modifier = reinterpret_cast<decltype(&gbm_bo_get_modifier)>(dlsym(library, "gbm_bo_get_modifier"));
  ASSERT_TRUE(create && destroy && create_bo && destroy_bo && map && unmap && get_fd && get_stride && get_modifier);
  auto path = platf::resolve_render_device();
  int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
  ASSERT_GE(fd, 0) << path;
  auto fd_guard = util::fail_guard([&] {
    close(fd);
  });
  auto gbm = create(fd);
  ASSERT_NE(gbm, nullptr);
  auto gbm_guard = util::fail_guard([&] {
    destroy(gbm);
  });
  for (int extent : {64, 128}) {
    for (int hdr : {0, 1}) {
      for (int chroma : {0, 1}) {
        video::config_t config {};
        config.width = config.height = extent;
        config.framerate = 60;
        config.bitrate = 200000;
        config.dynamicRange = hdr;
        config.chromaSamplingType = chroma;
        video::pyrowave_session_t session;
        ASSERT_EQ(session.init(config, hdr != 0), 0);
        uint32_t fourcc = hdr ? DRM_FORMAT_XRGB2101010 : DRM_FORMAT_XRGB8888;
        auto bo = create_bo(gbm, extent, extent, fourcc, GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
        ASSERT_NE(bo, nullptr);
        auto bo_guard = util::fail_guard([&] {
          destroy_bo(bo);
        });
        egl::img_descriptor_t image;
        image.sd = {};
        for (auto &plane_fd : image.sd.fds) {
          plane_fd = -1;
        }
        image.sequence = 1;
        image.sd.width = image.sd.height = extent;
        image.sd.fourcc = fourcc;
        image.sd.modifier = get_modifier(bo);
        image.sd.pitches[0] = get_stride(bo);
        image.sd.fds[0] = get_fd(bo);
        ASSERT_GE(image.sd.fds[0], 0);
        for (int frame = 0; frame < 16; ++frame) {
          uint32_t stride;
          void *map_data = nullptr;
          auto pixels = static_cast<uint8_t *>(map(bo, 0, 0, extent, extent, GBM_BO_TRANSFER_WRITE, &stride, &map_data));
          ASSERT_NE(pixels, nullptr);
          for (int y = 0; y < extent; ++y) {
            for (int x = 0; x < extent; ++x) {
              uint32_t value = hdr ? (uint32_t((x + frame) * 8 % 1024) << 20) | (uint32_t(y * 8 % 1024) << 10) | 256u :
                                     (uint32_t((x + frame) * 4 % 256) << 16) | (uint32_t(y * 4 % 256) << 8) | 64u;
              std::memcpy(pixels + y * stride + x * 4, &value, 4);
            }
          }
          unmap(bo, map_data);
          ASSERT_EQ(session.convert(image), 0) << hdr << "/" << chroma;
          const auto &bytes = session.frame();
          ASSERT_GE(bytes.size(), 8u);
          ASSERT_LE(bytes.size(), PYROWAVE_MAX_FRAME_BYTES);
          uint32_t metadata;
          std::memcpy(&metadata, bytes.data() + 4, 4);
          EXPECT_EQ((metadata >> 27) & 7, hdr ? 7u : 0u);
          EXPECT_EQ((metadata >> 26) & 1, uint32_t(chroma));
          if (const auto output = std::getenv("SUNSHINE_TEST_PYROWAVE_OUTPUT")) {
            auto filename = std::string(output) + "/" + std::to_string(extent) + "-" + std::to_string(hdr) + "-" + std::to_string(chroma) + "-" + std::to_string(frame) + ".pyro";
            std::ofstream stream(filename, std::ios::binary);
            stream.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
            ASSERT_TRUE(stream.good());
          }
        }
      }
    }
  }
}
#endif
