#include "giant/rhi/rhi.h"

#include "backends.h"

#include "giant/core/log.h"

namespace giant::rhi {

const char* to_string(Backend backend) {
    switch (backend) {
    case Backend::null: return "null";
    case Backend::d3d12: return "d3d12";
    }
    return "unknown";
}

Backend default_backend() {
#if GIANT_RHI_HAS_D3D12
    return Backend::d3d12;
#else
    return Backend::null;
#endif
}

std::unique_ptr<Device> create_device(const DeviceDesc& desc) {
    std::unique_ptr<Device> device;
    switch (desc.backend) {
    case Backend::null:
        device = create_null_device();
        break;
    case Backend::d3d12:
#if GIANT_RHI_HAS_D3D12
        device = create_d3d12_device(desc);
#else
        log_error("rhi", "the d3d12 backend is not built on this platform");
#endif
        break;
    }
    if (device == nullptr) {
        return nullptr;
    }

    const AdapterInfo& info = device->adapter();
    log_info("rhi", std::string("device: ") + to_string(device->backend()) + " on " + info.name +
                        (info.software ? " (software)" : "") + ", mesh shaders " +
                        (info.mesh_shaders ? "yes" : "no") + ", shader model 6.6 " +
                        (info.shader_model_6_6 ? "yes" : "no"));
    if (desc.backend != Backend::null && !info.meets_minimum_spec()) {
        log_warning("rhi", "this GPU is below the minimum spec (needs mesh shaders and shader "
                           "model 6.6); later milestones will not run on it");
    }
    return device;
}

} // namespace giant::rhi
