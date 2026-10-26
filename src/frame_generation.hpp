#pragma once
#include <d3d12.h>
#include <array>
#include <filesystem>
#include <string>
#ifdef GM_FG_EXPORTS
#define GM_FG_API __declspec(dllexport)
#else
#define GM_FG_API __declspec(dllimport)
#endif

// DLSS Frame Generation through Streamline in manual-hooking mode.
// The real IDXGIFactory2::CreateSwapChainForHwnd v-table slot is redirected so the game's swap chain is created by the
// Streamline proxy factory. ReShade proxies the factory and calls the real one underneath, so the chain becomes
// game -> ReShade swap chain -> Streamline proxy swap chain -> DXGI, and ReShade's present events stay once per game frame.
// Lives in its own DLL, pinned once the hook is installed: ReShade unloads and reloads add-ons while the game creates and
// destroys probe devices, and a v-table slot must never point into an unloaded module.
class GM_FG_API FrameGeneration {
public:
    // Row-major, row-vector matrices (UE convention), without TAA jitter.
    struct Camera {
        std::array<float,16> view_to_clip{},clip_to_view{},clip_to_prev_clip{},prev_clip_to_clip{};
        std::array<float,3> position{},up{},right{},forward{};
        float jitter_x=0,jitter_y=0,near_plane=10,far_plane=100000,fov_y=1,aspect=1;
    };
    struct Status {
        bool initialized=false,loaded=false,hooked=false,swapchain=false,enabled=false;
        uint32_t dlssg_status=0,max_generated=0;
        uint64_t frames=0,presented=0; // Since the previous status() call.
        std::string error;
    };
    static FrameGeneration &instance();
    // Loads the signed Streamline runtime from `runtime`, calls slInit and redirects swap-chain creation.
    // Must run before the game creates its swap chain; later calls are ignored. Events go to <logs>/frame_generation.jsonl.
    bool initialize(const std::filesystem::path &runtime,const std::filesystem::path &logs);
    // Lets the hook find the native queue behind a wrapped (e.g. ReShade) queue.
    static void remember_queue(ID3D12CommandQueue *native);
    void set_enabled(bool enabled);
    // Present-thread calls, once per game frame.
    void before_present();
    void after_present();
    // Render-thread call after the frame's depth/motion inputs are recorded on `list` (both in NON_PIXEL_SHADER_RESOURCE state).
    // Motion vectors are in render pixels pointing to the previous position (DLSS convention).
    void tag(ID3D12GraphicsCommandList *list,ID3D12Resource *depth,ID3D12Resource *motion,unsigned width,unsigned height,
        const Camera &camera,bool reset);
    Status status();
    void shutdown();
    struct Impl; // Defined in frame_generation.cpp; also used by the swap-chain hook there.
private:
    FrameGeneration()=default;
    Impl *impl_=nullptr;
};
