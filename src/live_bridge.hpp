#pragma once
#include "dlss_backend.hpp"
#include <wrl/client.h>
#include <array>
#include <fstream>
#include <functional>
#include <memory>

struct TemporalInputs {
    ID3D12Resource *color=nullptr,*depth=nullptr,*velocity=nullptr,*destination=nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE color_view{},depth_view{},velocity_view{};
    unsigned width=0,height=0,output_width=0,output_height=0;
    float jitter_x=0,jitter_y=0,pre_exposure=1;
    bool camera_only=false; // Velocity is UE's 1x1 dummy: reproject camera motion only, as the game's shader does.
    std::array<float,16> clip_to_previous{};
};
class LiveBridge {
public:
    using Log=std::function<void(const std::string &)>;
    LiveBridge(ID3D12Device *,std::filesystem::path,std::filesystem::path,Log);
    ~LiveBridge();
    void present(ID3D12CommandQueue *);
    // Always restores original command state through the supplied callback, even on failure.
    // Returns true only when DLSS output was copied to the game's temporal output.
    bool process(ID3D12GraphicsCommandList *,const TemporalInputs &,bool replace,const std::function<void()> &restore);
    void reset_history() { reset_=true; }
    void set_preset(unsigned preset) { backend_.preset=preset; }
    // Frame-generation inputs from the latest evaluation, left in NON_PIXEL_SHADER_RESOURCE state.
    ID3D12Resource *prepared_depth() const { return depth_.Get(); }
    ID3D12Resource *prepared_motion() const { return motion_.Get(); }
    bool last_reset() const { return last_reset_; }
    // Rebuilds the NGX feature through the resize path (e.g. after a preset change).
    void recreate() { if(initialized_ && !resize_pending_) {resize_pending_=true;resize_epoch_=epoch_;} }
    bool snapshots=true; // 64 MB readback + file write per generation at 4K: diagnostics only.
    bool uses_queue(ID3D12CommandQueue *queue) const { return queue_.Get()==queue; }
    struct Status { bool active,failed; unsigned width,height,output_width,output_height; UINT64 frames; bool resizing; };
    Status status() const {return {initialized_ && !resize_pending_ && !failed_ && generation_frames_ && epoch_-last_epoch_<=2,failed_,width_,height_,out_width_,out_height_,frames_,resize_pending_};}
private:
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    struct Slot { UINT64 fence=0; };
    ID3D12Device *device_;
    Ptr<ID3D12CommandQueue> queue_;
    DlssBackend backend_;
    Ptr<ID3D12RootSignature> root_;
    Ptr<ID3D12PipelineState> pipeline_;
    Ptr<ID3D12DescriptorHeap> heap_;
    Ptr<ID3D12Fence> fence_;
    Ptr<ID3D12Resource> color_,depth_,motion_,result_,readback_;
    std::array<Slot,16> slots_{};
    UINT increment_=0,width_=0,height_=0,out_width_=0,out_height_=0;
    UINT64 fence_value_=0,epoch_=0,last_epoch_=~0ull,frames_=0,snapshot_fence_=0;
    UINT64 generation_=0,generation_frames_=0,resize_epoch_=0;
    LARGE_INTEGER last_present_{};float frame_ms_=16.6667f; // Present-to-present time passed to DLSS.
    UINT snapshot_pitch_=0; UINT64 snapshot_bytes_=0;
    bool initialized_=false,reset_=true,failed_=false,snapshot_recorded_=false,snapshot_saved_=false;
    bool resize_pending_=false,last_reset_=false;
    std::filesystem::path runtime_,output_;
    Log log_;
    void initialize(ID3D12GraphicsCommandList *,const TemporalInputs &);
    bool wait(UINT64);
    void save_snapshot();
    void retire_resolution();
};
