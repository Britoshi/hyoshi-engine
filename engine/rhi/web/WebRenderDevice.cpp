#include "WebGlRenderDevice.h"
#include "WebGpuRenderDevice.h"

#include <emscripten/em_js.h>

#include <memory>

// The body is JavaScript, which clang-format would break.
// clang-format off

// The page asks for a WebGPU device before the module starts (platforms/web/index.html.in).
EM_JS(int, HasWebGpuDevice, (), { return Module["preinitializedWebGPUDevice"] ? 1 : 0; });

// clang-format on

namespace hyoshi::rhi
{

// WebGPU where the page got a device; otherwise WebGL 2 (older browsers, and cars').
std::unique_ptr<IRenderDevice> CreateRenderDevice()
{
    if (HasWebGpuDevice() != 0)
    {
        return std::make_unique<WebGpuRenderDevice>();
    }
    return std::make_unique<WebGlRenderDevice>();
}

} // namespace hyoshi::rhi
