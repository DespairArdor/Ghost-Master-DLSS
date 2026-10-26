#include "live_bridge.hpp"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <vector>
namespace {
void hr(HRESULT r,const char *label) { if(FAILED(r)) {std::ostringstream s;s<<label<<" HRESULT="<<std::hex<<r;throw std::runtime_error(s.str());} }
void ng(NVSDK_NGX_Result r,const char *label) { if(NVSDK_NGX_FAILED(r)) {std::ostringstream s;s<<label<<" NGX="<<std::hex<<unsigned(r);throw std::runtime_error(s.str());} }
void transition(ID3D12GraphicsCommandList *c,ID3D12Resource *r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};c->ResourceBarrier(1,&barrier);
}
constexpr char preparation[]=R"(
Texture2D<float4> SourceColor : register(t0);
Texture2D<float> SourceDepth : register(t1);
Texture2D<float4> SourceVelocity : register(t2);
RWTexture2D<float4> Color : register(u0);
RWTexture2D<float> Depth : register(u1);
RWTexture2D<float2> Motion : register(u2);
cbuffer Parameters : register(b0) { row_major float4x4 ClipToPrevious; uint2 Size; uint CameraOnly; uint Padding; }
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=Size)) return;
    float z=SourceDepth.Load(int3(id.xy,0));
    float2 encoded=SourceVelocity.Load(int3(id.xy,0)).xy;
    float2 clip=(float2(id.xy)+0.5)/float2(Size)*float2(2,-2)+float2(-1,1);
    float4 previous=mul(float4(clip,z,1),ClipToPrevious);
    float2 delta=clip-previous.xy/max(abs(previous.w),1e-8)*sign(previous.w);
    if(!CameraOnly && encoded.x>0) {
        // This UE 5.5.1 shader uses the signed-square velocity encoding, verified in its DXIL.
        float2 v=encoded*(1.0/(0.499*0.5))-(32767.0/65535.0)/(0.499*0.5);
        delta=0.5*v*abs(v);
    }
    Color[id.xy]=SourceColor.Load(int3(id.xy,0)); Depth[id.xy]=z;
    Motion[id.xy]=delta*float2(-0.5*Size.x,0.5*Size.y);
})";
float half_value(uint16_t v) {unsigned e=(v>>10)&31,m=v&1023;float f=e==31?(m?NAN:INFINITY):e?std::ldexp(float(1024+m),int(e)-25):std::ldexp(float(m),-24);return v&0x8000?-f:f;}
}
LiveBridge::LiveBridge(ID3D12Device *d,std::filesystem::path runtime,std::filesystem::path output,Log log):device_(d),runtime_(std::move(runtime)),output_(std::move(output)),log_(std::move(log)) {}
bool LiveBridge::wait(UINT64 value) {
    if(!value)return true;
    const auto completed=fence_->GetCompletedValue();
    if(completed==UINT64_MAX)return false;
    if(completed>=value)return true;
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)return false;
    const auto result=fence_->SetEventOnCompletion(value,event);
    DWORD status=SUCCEEDED(result)?WaitForSingleObject(event,5000):WAIT_FAILED;CloseHandle(event);
    return status==WAIT_OBJECT_0 && fence_->GetCompletedValue()!=UINT64_MAX;
}
LiveBridge::~LiveBridge() {
    if(queue_ && fence_) {if(SUCCEEDED(queue_->Signal(fence_.Get(),++fence_value_)))wait(fence_value_);}
    backend_.shutdown();
    log_("{\"event\":\"dlss_shutdown\"}");
}
void LiveBridge::initialize(ID3D12GraphicsCommandList *cmd,const TemporalInputs &in) {
    width_=in.width;height_=in.height;out_width_=in.output_width;out_height_=in.output_height;
    if(!root_) {
    hr(device_->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence_)),"CreateFence");
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=16*6;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr(device_->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap_)),"DescriptorHeap");increment_=device_->GetDescriptorHandleIncrementSize(hd.Type);
    D3D12_DESCRIPTOR_RANGE ranges[2]{};ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,3,0,0,0};ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,3,0,0,3};
    D3D12_ROOT_PARAMETER params[2]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[0].DescriptorTable={2,ranges};
    params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[1].Constants={0,0,20};
    D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=2;rd.pParameters=params;
    Ptr<ID3DBlob> blob,error;
    hr(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"SerializeRootSignature");
    hr(device_->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root_)),"RootSignature");
    hr(D3DCompile(preparation,sizeof(preparation),"ghostmaster_prepare",nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&error),"CompilePreparation");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root_.Get();pd.CS={blob->GetBufferPointer(),blob->GetBufferSize()};
    hr(device_->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline_)),"ComputePipeline");
    ng(backend_.initialize(device_,runtime_,output_/"ngx"),"Initialize");
    }
    auto texture=[&](UINT w,UINT h,DXGI_FORMAT format,Ptr<ID3D12Resource>&resource) {
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=w;td.Height=h;td.DepthOrArraySize=1;td.MipLevels=1;td.Format=format;td.SampleDesc.Count=1;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        hr(device_->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&resource)),"WorkingTexture");
    };
    texture(width_,height_,DXGI_FORMAT_R16G16B16A16_FLOAT,color_);texture(width_,height_,DXGI_FORMAT_R32_FLOAT,depth_);texture(width_,height_,DXGI_FORMAT_R16G16_FLOAT,motion_);
    texture(out_width_,out_height_,DXGI_FORMAT_R16G16B16A16_FLOAT,result_);
    ng(backend_.create(cmd,width_,height_,out_width_,out_height_),"CreateFeature");initialized_=true;++generation_;
    log_("{\"event\":\"dlss_initialized\",\"generation\":"+std::to_string(generation_)+",\"width\":"+std::to_string(width_)+",\"height\":"+std::to_string(height_)+",\"output_width\":"+std::to_string(out_width_)+",\"output_height\":"+std::to_string(out_height_)+"}");
}
void LiveBridge::retire_resolution() {
    // Called at Present, after the game has submitted the recording that requested a resize.
    // The fence includes every earlier use of our textures, descriptors and NGX feature.
    if(!wait(fence_value_))throw std::runtime_error("Resize GPU fence timeout or device removed");
    if(snapshot_recorded_ && !snapshot_saved_)save_snapshot();
    ng(backend_.release_feature(),"ReleaseFeatureForResize");
    color_.Reset();depth_.Reset();motion_.Reset();result_.Reset();readback_.Reset();
    slots_={};generation_frames_=0;snapshot_fence_=0;snapshot_pitch_=0;snapshot_bytes_=0;
    snapshot_recorded_=snapshot_saved_=false;initialized_=false;resize_pending_=false;reset_=true;
    log_("{\"event\":\"dlss_resize_ready\",\"retired_generation\":"+std::to_string(generation_)+"}");
}
void LiveBridge::present(ID3D12CommandQueue *queue) {
    if(queue_ && queue_.Get()!=queue) {if(!failed_)log_("{\"event\":\"dlss_error\",\"message\":\"present_queue_changed\"}");failed_=true;return;}
    queue_=queue;
    LARGE_INTEGER now{},frequency{};QueryPerformanceCounter(&now);QueryPerformanceFrequency(&frequency);
    if(last_present_.QuadPart)frame_ms_=std::clamp(float(double(now.QuadPart-last_present_.QuadPart)*1000/frequency.QuadPart),1.0f,100.0f);
    last_present_=now;
    if(initialized_) {
        if(FAILED(queue_->Signal(fence_.Get(),++fence_value_))) {failed_=true;return;}
        if(last_epoch_==epoch_)slots_[epoch_%slots_.size()].fence=fence_value_;
        if(snapshot_recorded_ && !snapshot_fence_)snapshot_fence_=fence_value_;
        if(snapshot_fence_ && !snapshot_saved_ && fence_->GetCompletedValue()>=snapshot_fence_)save_snapshot();
        // UE may submit a recording one frame after it was made: keep old resources for two more presents.
        if(resize_pending_ && !failed_ && epoch_-resize_epoch_>=2) {
            try {retire_resolution();}
            catch(const std::exception &e) {failed_=true;log_(std::string("{\"event\":\"dlss_error\",\"message\":\"")+e.what()+"\"}");}
        }
    }
    ++epoch_;
}
bool LiveBridge::process(ID3D12GraphicsCommandList *cmd,const TemporalInputs &in,bool replace,const std::function<void()> &restore) {
    if(failed_ || resize_pending_ || !queue_ || last_epoch_==epoch_ || cmd->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)return false;
    if(initialized_ && (in.width!=width_ || in.height!=height_ || in.output_width!=out_width_ || in.output_height!=out_height_)) {
        resize_pending_=true;resize_epoch_=epoch_;
        log_("{\"event\":\"dlss_resize_requested\",\"width\":"+std::to_string(in.width)+",\"height\":"+std::to_string(in.height)+",\"output_width\":"+std::to_string(in.output_width)+",\"output_height\":"+std::to_string(in.output_height)+"}");
        return false; // Use the native temporal pass during this transition frame.
    }
    bool recorded=false;
    try {
        if(frames_ && epoch_-last_epoch_>2)reset_=true;
        if(!initialized_)initialize(cmd,in);
        const auto index=epoch_%slots_.size();if(!wait(slots_[index].fence))throw std::runtime_error("Descriptor slot fence timeout");
        const auto cpu=heap_->GetCPUDescriptorHandleForHeapStart();const auto gpu=heap_->GetGPUDescriptorHandleForHeapStart();
        auto cpu_at=[&](UINT i){return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr+(index*6+i)*increment_};};
        D3D12_CPU_DESCRIPTOR_HANDLE views[]={in.color_view,in.depth_view,in.velocity_view};
        for(UINT i=0;i<3;++i)device_->CopyDescriptorsSimple(1,cpu_at(i),views[i],D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        ID3D12Resource *targets[]={color_.Get(),depth_.Get(),motion_.Get()};
        for(UINT i=0;i<3;++i) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};desc.Format=targets[i]->GetDesc().Format;desc.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
            device_->CreateUnorderedAccessView(targets[i],nullptr,&desc,cpu_at(i+3));
            if(generation_frames_)transition(cmd,targets[i],D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        ID3D12DescriptorHeap *heaps[]={heap_.Get()};cmd->SetDescriptorHeaps(1,heaps);cmd->SetComputeRootSignature(root_.Get());cmd->SetPipelineState(pipeline_.Get());
        cmd->SetComputeRootDescriptorTable(0,{gpu.ptr+index*6*increment_});
        struct Constants {std::array<float,16> matrix;UINT w,h,pad[2];} constants{in.clip_to_previous,width_,height_,{in.camera_only?1u:0u,0}};
        cmd->SetComputeRoot32BitConstants(1,20,&constants,0);cmd->Dispatch((width_+7)/8,(height_+7)/8,1);recorded=true;
        for(auto *r:targets)transition(cmd,r,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        last_reset_=reset_;
        ng(backend_.evaluate(cmd,color_.Get(),depth_.Get(),motion_.Get(),result_.Get(),in.jitter_x,in.jitter_y,in.pre_exposure,reset_,frame_ms_),"Evaluate");
        reset_=false;++frames_;++generation_frames_;last_epoch_=epoch_;
        if(replace) {
            transition(cmd,result_.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(cmd,in.destination,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION src{},dst{};
            src.pResource=result_.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.pResource=in.destination;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
            transition(cmd,in.destination,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            transition(cmd,result_.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        if(snapshots && generation_frames_==60 && !snapshot_recorded_) {
            auto td=result_->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};device_->GetCopyableFootprints(&td,0,1,0,&fp,nullptr,nullptr,&snapshot_bytes_);snapshot_pitch_=fp.Footprint.RowPitch;
            D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=snapshot_bytes_;bd.Height=1;bd.DepthOrArraySize=1;bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            hr(device_->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback_)),"Readback");
            transition(cmd,result_.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=result_.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=readback_.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;
            cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);transition(cmd,result_.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);snapshot_recorded_=true;
        }
        if(generation_frames_<=3 || frames_%600==0)log_("{\"event\":\"dlss_evaluated\",\"generation\":"+std::to_string(generation_)+",\"count\":"+std::to_string(frames_)+",\"replaced\":"+(replace?"true":"false")+",\"jitter_x\":"+std::to_string(in.jitter_x)+",\"jitter_y\":"+std::to_string(in.jitter_y)+"}");
        restore();return replace;
    } catch(const std::exception &e) {
        // A partially recorded pass cannot be undone. Disable further injection and preserve the game's own pass.
        failed_=true;log_(std::string("{\"event\":\"dlss_error\",\"message\":\"")+e.what()+"\",\"gpu_work_recorded\":"+(recorded?"true":"false")+"}");restore();return false;
    }
}
void LiveBridge::save_snapshot() {
    void *mapped=nullptr;D3D12_RANGE range{0,size_t(snapshot_bytes_)};if(FAILED(readback_->Map(0,&range,&mapped)))return;
    std::vector<char> pixels(static_cast<size_t>(snapshot_bytes_),0);std::memcpy(pixels.data(),mapped,pixels.size());D3D12_RANGE written{0,0};readback_->Unmap(0,&written);
    const auto stem=generation_==1?std::string("dlss-output"):"dlss-output-g"+std::to_string(generation_)+"-"+std::to_string(out_width_)+"x"+std::to_string(out_height_);
    std::ofstream raw(output_/(stem+".rgba16f"),std::ios::binary),ppm(output_/(stem+".ppm"),std::ios::binary);
    ppm<<"P6\n"<<out_width_<<' '<<out_height_<<"\n255\n";size_t nonfinite=0;
    for(UINT y=0;y<out_height_;++y) {
        auto row=reinterpret_cast<const uint16_t *>(pixels.data()+y*snapshot_pitch_);raw.write(reinterpret_cast<const char *>(row),out_width_*8);
        for(UINT x=0;x<out_width_;++x)for(UINT c=0;c<3;++c) {
            float f=half_value(row[x*4+c]);if(!std::isfinite(f)){++nonfinite;f=0;}
            f=std::max(f,0.0f);unsigned char v=static_cast<unsigned char>(std::pow(f/(1+f),1/2.2f)*255);ppm.write(reinterpret_cast<const char *>(&v),1);
        }
    }
    snapshot_saved_=true;log_("{\"event\":\"dlss_snapshot\",\"generation\":"+std::to_string(generation_)+",\"width\":"+std::to_string(out_width_)+",\"height\":"+std::to_string(out_height_)+",\"file\":\""+stem+".rgba16f\",\"nonfinite_rgb\":"+std::to_string(nonfinite)+"}");
}
