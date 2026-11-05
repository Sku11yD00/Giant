// Tells the D3D12 loader to use the Agility SDK runtime from the D3D12
// folder next to the executable (see giant_d3d12_app in GiantModule.cmake).
// Must live in the executable itself: the loader looks for these exports
// there, not in libraries.
#include <windows.h>

extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = GIANT_D3D12_SDK_VERSION;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}
