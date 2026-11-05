// Private: factory functions for each backend.
#pragma once

#include "giant/rhi/rhi.h"

#include <memory>

namespace giant::rhi {

std::unique_ptr<Device> create_null_device();

#if GIANT_RHI_HAS_D3D12
std::unique_ptr<Device> create_d3d12_device(const DeviceDesc& desc);
#endif

} // namespace giant::rhi
