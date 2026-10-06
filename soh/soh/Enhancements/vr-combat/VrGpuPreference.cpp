// SOH [VR] Ask hybrid-graphics drivers for the dedicated GPU. On NVIDIA Optimus and AMD switchable
// laptops the system default adapter is the integrated GPU unless the EXE exports these; the
// headset hangs off the dedicated one, and OpenXR refuses a device on any other adapter. This also
// covers VR switched on mid-session, when the D3D device already exists and the startup adapter
// probe (Fast3dWindow::Init) didn't run. They must live in the executable, not a static library.
#ifdef _WIN32
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif
