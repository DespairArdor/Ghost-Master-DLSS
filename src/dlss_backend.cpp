// This software contains source code provided by NVIDIA Corporation (SDK inline helpers).
#include "dlss_backend.hpp"
#include <cmath>
#include <cstdio>

NVSDK_NGX_Result DlssBackend::initialize(ID3D12Device *device, const std::filesystem::path &runtime, const std::filesystem::path &logs) {
    if(device_ || !device) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    std::filesystem::create_directories(logs);
    runtime_path_=std::filesystem::absolute(runtime).wstring();
    const wchar_t *paths[]={runtime_path_.c_str()};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo={paths,1};
    // This project's identifier; not an NVIDIA-issued application ID or an impersonated game's ID.
    auto result=NVSDK_NGX_D3D12_Init_with_ProjectID("9b048dd7-c4b2-4ec1-8110-7c13b89bc44b",
        NVSDK_NGX_ENGINE_TYPE_CUSTOM,"GhostMasterDLSS-prototype",std::filesystem::absolute(logs).c_str(),device,&info);
    if(NVSDK_NGX_FAILED(result)) return result;
    device_=device;
    result=NVSDK_NGX_D3D12_GetCapabilityParameters(&parameters_);
    if(NVSDK_NGX_FAILED(result)) { shutdown(); return result; }
    int available=0;
    result=parameters_->Get(NVSDK_NGX_Parameter_SuperSampling_Available,&available);
    if(NVSDK_NGX_FAILED(result) || !available) { shutdown(); return NVSDK_NGX_Result_FAIL_FeatureNotSupported; }
    return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result DlssBackend::create(ID3D12GraphicsCommandList *list,unsigned w,unsigned h,unsigned ow,unsigned oh) {
    if(!device_ || feature_ || !list || !w || !h || ow<w || oh<h) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    NVSDK_NGX_DLSS_Create_Params p{};
    p.Feature={w,h,ow,oh,NVSDK_NGX_PerfQuality_Value_MaxQuality};
    for(auto *mode:{NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality})
        parameters_->Set(mode,preset);
    p.InFeatureCreateFlags=NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
        NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    auto result=NGX_D3D12_CREATE_DLSS_EXT(list,1,1,&feature_,parameters_,&p);
    if(NVSDK_NGX_SUCCEED(result)) {width_=w;height_=h;}
    return result;
}
NVSDK_NGX_Result DlssBackend::evaluate(ID3D12GraphicsCommandList *list,ID3D12Resource *color,ID3D12Resource *depth,
    ID3D12Resource *motion,ID3D12Resource *output,float jitter_x,float jitter_y,float pre_exposure,bool reset,float frame_time_ms) {
    if(!feature_ || !list || !color || !depth || !motion || !output || !std::isfinite(jitter_x) || !std::isfinite(jitter_y) ||
       !std::isfinite(pre_exposure) || pre_exposure<=0) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    NVSDK_NGX_D3D12_DLSS_Eval_Params p{};
    p.Feature.pInColor=color; p.Feature.pInOutput=output; p.pInDepth=depth; p.pInMotionVectors=motion;
    p.InJitterOffsetX=jitter_x; p.InJitterOffsetY=jitter_y; p.InRenderSubrectDimensions={width_,height_};
    p.InMVScaleX=1; p.InMVScaleY=1; p.InPreExposure=pre_exposure; p.InExposureScale=1;
    p.InReset=reset?1:0; p.InFrameTimeDeltaInMsec=std::isfinite(frame_time_ms) && frame_time_ms>0 ? frame_time_ms : 16.6667f;
    return NGX_D3D12_EVALUATE_DLSS_EXT(list,feature_,parameters_,&p);
}
NVSDK_NGX_Result DlssBackend::release_feature() {
    if(!feature_)return NVSDK_NGX_Result_Success;
    const auto result=NVSDK_NGX_D3D12_ReleaseFeature(feature_);
    if(NVSDK_NGX_SUCCEED(result)){feature_=nullptr;width_=height_=0;}
    return result;
}
void DlssBackend::shutdown() {
    if(feature_) { std::fprintf(stderr,"NGX release feature\n"); release_feature(); }
    if(parameters_) { std::fprintf(stderr,"NGX destroy parameters\n"); NVSDK_NGX_D3D12_DestroyParameters(parameters_); parameters_=nullptr; }
    if(device_) { std::fprintf(stderr,"NGX shutdown device\n"); NVSDK_NGX_D3D12_Shutdown1(device_); device_=nullptr; }
    std::fprintf(stderr,"NGX shutdown complete\n");
}
