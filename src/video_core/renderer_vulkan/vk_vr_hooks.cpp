// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_vulkan/vk_vr_hooks.h"

namespace Vulkan {

namespace {
const VrHooks* g_hooks = nullptr;
}

const VrHooks* GetVrHooks() {
    return g_hooks;
}

void SetVrHooks(const VrHooks* hooks) {
    g_hooks = hooks;
}

std::mutex& GetVrQueueMutex() {
    static std::mutex m;
    return m;
}

std::mutex& GetVrCoreBusyMutex() {
    static std::mutex m;
    return m;
}

namespace {
VrWaitIdleFn g_vr_wait_idle = nullptr;
} // namespace

void SetVrWaitIdleFn(VrWaitIdleFn fn) {
    g_vr_wait_idle = fn;
}

VrWaitIdleFn GetVrWaitIdleFn() {
    return g_vr_wait_idle;
}

} // namespace Vulkan
