// SPDX-License-Identifier: GPL-3.0-or-later
//
// Lightweight logging facade so VR code does not depend on Citra's
// fmt-based logger directly. Mirrors the surface of utils/LogUtils.h
// from the Android tree (ALOGI / ALOGW / ALOGE / FAIL) so the eventual
// hoist into a shared platform-agnostic module is mechanical.

#pragma once

#include <cstdio>
#include <cstdlib>

#define VR_LOG_PREFIX "[CitraVR-PC] "

#if defined(_MSC_VER)
#  define VR_FUNC __FUNCSIG__
#else
#  define VR_FUNC __PRETTY_FUNCTION__
#endif

#define ALOGI(fmt, ...) std::fprintf(stdout, VR_LOG_PREFIX "I " fmt "\n", ##__VA_ARGS__)
#define ALOGW(fmt, ...) std::fprintf(stderr, VR_LOG_PREFIX "W " fmt "\n", ##__VA_ARGS__)
#define ALOGE(fmt, ...) std::fprintf(stderr, VR_LOG_PREFIX "E " fmt "\n", ##__VA_ARGS__)
#define ALOGD(fmt, ...) std::fprintf(stdout, VR_LOG_PREFIX "D " fmt "\n", ##__VA_ARGS__)
#define ALOGV(fmt, ...) ((void)0)

#define FAIL(fmt, ...)                                                                          \
    do {                                                                                        \
        std::fprintf(stderr, VR_LOG_PREFIX "FATAL " fmt " (%s)\n", ##__VA_ARGS__, VR_FUNC);     \
        std::abort();                                                                           \
    } while (0)
