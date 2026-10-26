#pragma once
#include <d3d12.h>
#include <nvsdk_ngx_helpers.h>
#include <filesystem>

// Caller owns synchronization, resource barriers and command-state restoration.
// All calls must be serialized; shutdown requires completion of all GPU work.
class DlssBackend {
public:
    DlssBackend() = default;
    DlssBackend(const DlssBackend &) = delete;
    DlssBackend &operator=(const DlssBackend &) = delete;
    NVSDK_NGX_Result initialize(ID3D12Device *, const std::filesystem::path &runtime, const std::filesystem::path &logs);
    NVSDK_NGX_Result create(ID3D12GraphicsCommandList *, unsigned width, unsigned height, unsigned output_width, unsigned output_height);
    // Caller must wait for all submissions using the feature before releasing it.
    NVSDK_NGX_Result release_feature();
    NVSDK_NGX_Result evaluate(ID3D12GraphicsCommandList *, ID3D12Resource *color, ID3D12Resource *depth,
        ID3D12Resource *motion, ID3D12Resource *output, float jitter_x, float jitter_y, float pre_exposure, bool reset,
        float frame_time_ms = 16.6667f);
    void shutdown();
    // NVSDK_NGX_DLSS_Hint_Render_Preset value applied to every quality mode at the next create(); 0 = NVIDIA default.
    unsigned preset = 0;
private:
    ID3D12Device *device_ = nullptr;
    NVSDK_NGX_Parameter *parameters_ = nullptr;
    NVSDK_NGX_Handle *feature_ = nullptr;
    unsigned width_ = 0, height_ = 0;
    std::wstring runtime_path_;
};
