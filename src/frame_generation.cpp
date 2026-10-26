#include "frame_generation.hpp"
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include <dxgi1_6.h>
#include <softpub.h>
#include <wintrust.h>
#include <wrl/client.h>
#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
namespace {
// Private data on a native command queue pointing to itself; wrappers forward GetPrivateData to the native object.
constexpr GUID native_queue_guid={0x5d1f7a43,0x2c8e,0x4b19,{0x9e,0x61,0x3f,0x0a,0x7c,0x52,0xd4,0x18}};
constexpr UINT create_swapchain_for_hwnd_slot=15; // IDXGIFactory2::CreateSwapChainForHwnd
using CreateSwapChainForHwnd=HRESULT(STDMETHODCALLTYPE *)(IDXGIFactory2 *,IUnknown *,HWND,const DXGI_SWAP_CHAIN_DESC1 *,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *,IDXGIOutput *,IDXGISwapChain1 **);
thread_local bool inside_streamline=false;
bool signed_by_nvidia(const fs::path &file) {
    const auto path=file.wstring();
    WINTRUST_FILE_INFO fi{sizeof(fi)};fi.pcwszFilePath=path.c_str();
    WINTRUST_DATA wd{sizeof(wd)};wd.dwUIChoice=WTD_UI_NONE;wd.fdwRevocationChecks=WTD_REVOKE_NONE;wd.dwUnionChoice=WTD_CHOICE_FILE;
    wd.pFile=&fi;wd.dwStateAction=WTD_STATEACTION_VERIFY;wd.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action=WINTRUST_ACTION_GENERIC_VERIFY_V2;
    bool ok=WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE),&action,&wd)==ERROR_SUCCESS;
    if(ok) {
        auto *provider=WTHelperProvDataFromStateData(wd.hWVTStateData);
        auto *signer=provider?WTHelperGetProvSignerFromChain(provider,0,FALSE,0):nullptr;
        wchar_t name[256]{};
        ok=signer && signer->csCertChain && CertGetNameStringW(signer->pasCertChain[0].pCert,CERT_NAME_SIMPLE_DISPLAY_TYPE,0,nullptr,name,256) &&
           std::wstring(name).find(L"NVIDIA")!=std::wstring::npos;
    }
    wd.dwStateAction=WTD_STATEACTION_CLOSE;WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE),&action,&wd);
    return ok;
}
sl::float4x4 matrix(const std::array<float,16> &m) {
    sl::float4x4 r;for(uint32_t i=0;i<4;++i)r.setRow(i,{m[i*4],m[i*4+1],m[i*4+2],m[i*4+3]});return r;
}
std::ofstream sl_log;
std::mutex sl_log_mutex;
void on_sl_log(sl::LogType type,const char *message) {
    std::lock_guard lock(sl_log_mutex);
    if(sl_log)sl_log<<(type==sl::LogType::eError?"E ":type==sl::LogType::eWarn?"W ":"I ")<<message<<std::flush;
}
}
struct FrameGeneration::Impl {
    std::ofstream events;std::mutex events_mutex;
    void log(const std::string &line) { std::lock_guard lock(events_mutex); if(events)events<<line<<std::endl; }
    HMODULE module=nullptr;
    PFun_slInit *init{};PFun_slShutdown *shutdown{};PFun_slSetD3DDevice *set_device{};PFun_slUpgradeInterface *upgrade{};
    PFun_slGetFeatureFunction *feature_function{};PFun_slGetNewFrameToken *new_frame{};PFun_slSetTagForFrame *set_tags{};
    PFun_slSetConstants *set_constants{};PFun_slIsFeatureLoaded *loaded{};
    PFun_slDLSSGSetOptions *dlssg_set{};PFun_slDLSSGGetState *dlssg_state{};
    PFun_slReflexSetOptions *reflex_set{};PFun_slReflexSleep *reflex_sleep{};PFun_slPCLSetMarker *marker{};
    void **slot=nullptr;CreateSwapChainForHwnd original=nullptr;
    std::mutex mutex; // Frame token and Streamline calls shared between the present and render threads.
    sl::FrameToken *token=nullptr;uint32_t frame_index=0;
    bool swapchain=false,features=false,options_dirty=true,tagged=false;
    uint32_t backbuffer_width=0,backbuffer_height=0;
    bool in_present=false; // before_present seen without after_present (finish_present event missing on older ReShade).
    std::atomic<bool> enabled{false};
    uint32_t dlssg_status=0,max_generated=0;uint64_t frames=0,presented=0;
    std::string error;
    void fail(const std::string &message) { if(error.empty()){error=message;log("{\"event\":\"fg_error\",\"message\":\""+message+"\"}");} }
    template<class T> bool core(T *&f,const char *name) { f=reinterpret_cast<T *>(GetProcAddress(module,name)); if(!f)fail(std::string("missing ")+name); return f!=nullptr; }
    template<class T> bool feature(T *&f,sl::Feature id,const char *name) {
        void *p=nullptr; if(feature_function(id,name,p)!=sl::Result::eOk || !p) {fail(std::string("missing ")+name);return false;}
        f=reinterpret_cast<T *>(p);return true;
    }
    void begin_frame() {
        if(new_frame(token,&frame_index)!=sl::Result::eOk) {token=nullptr;return;}
        reflex_sleep(*token);
        marker(sl::PCLMarker::eSimulationStart,*token);marker(sl::PCLMarker::eSimulationEnd,*token);
        marker(sl::PCLMarker::eRenderSubmitStart,*token);
        tagged=false;
    }
    // Device-dependent setup, done once the game's device is known (inside swap-chain creation).
    bool attach(ID3D12Device *device) {
        if(features)return true;
        if(set_device(device)!=sl::Result::eOk) {fail("slSetD3DDevice failed");return false;}
        bool dlssg=false;loaded(sl::kFeatureDLSS_G,dlssg);
        if(!dlssg) {fail("DLSS-G plugin not loaded (see sl.log)");return false;}
        if(!feature(dlssg_set,sl::kFeatureDLSS_G,"slDLSSGSetOptions") || !feature(dlssg_state,sl::kFeatureDLSS_G,"slDLSSGGetState") ||
           !feature(reflex_set,sl::kFeatureReflex,"slReflexSetOptions") || !feature(reflex_sleep,sl::kFeatureReflex,"slReflexSleep") ||
           !feature(marker,sl::kFeaturePCL,"slPCLSetMarker"))return false;
        sl::ReflexOptions reflex{};reflex.mode=sl::ReflexMode::eLowLatency;
        if(reflex_set(reflex)!=sl::Result::eOk) {fail("slReflexSetOptions failed");return false;}
        features=true;return true;
    }
};
namespace {
FrameGeneration::Impl *active=nullptr;
HRESULT STDMETHODCALLTYPE create_swapchain_for_hwnd(IDXGIFactory2 *factory,IUnknown *device,HWND hwnd,const DXGI_SWAP_CHAIN_DESC1 *desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen,IDXGIOutput *output,IDXGISwapChain1 **swapchain) {
    auto *impl=active;
    // Re-entry from the Streamline proxy, a second swap chain, or setup failure: plain DXGI.
    if(inside_streamline || !impl || impl->swapchain || !impl->error.empty())return impl->original(factory,device,hwnd,desc,fullscreen,output,swapchain);
    ComPtr<ID3D12CommandQueue> queue;
    if(!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))))return impl->original(factory,device,hwnd,desc,fullscreen,output,swapchain);
    ID3D12CommandQueue *native=nullptr;UINT size=sizeof(native);
    if(FAILED(queue->GetPrivateData(native_queue_guid,&size,&native)) || size!=sizeof(native) || !native)native=queue.Get();
    ComPtr<ID3D12Device> d3d;
    if(FAILED(native->GetDevice(IID_PPV_ARGS(&d3d))) || !impl->attach(d3d.Get()))return impl->original(factory,device,hwnd,desc,fullscreen,output,swapchain);
    IDXGIFactory2 *proxy=factory;proxy->AddRef();
    if(impl->upgrade(reinterpret_cast<void **>(&proxy))!=sl::Result::eOk) {proxy->Release();impl->fail("slUpgradeInterface(factory) failed");return impl->original(factory,device,hwnd,desc,fullscreen,output,swapchain);}
    inside_streamline=true;
    const HRESULT hr=proxy->CreateSwapChainForHwnd(native,hwnd,desc,fullscreen,output,swapchain);
    inside_streamline=false;
    proxy->Release();
    if(FAILED(hr)) {
        std::ostringstream s;s<<"proxy CreateSwapChainForHwnd failed 0x"<<std::hex<<hr;impl->fail(s.str());
        return impl->original(factory,device,hwnd,desc,fullscreen,output,swapchain);
    }
    {
        std::lock_guard lock(impl->mutex);
        impl->swapchain=true;impl->backbuffer_width=desc->Width;impl->backbuffer_height=desc->Height;impl->begin_frame();
    }
    std::ostringstream s;s<<"{\"event\":\"fg_swapchain\",\"width\":"<<desc->Width<<",\"height\":"<<desc->Height<<",\"format\":"<<desc->Format<<",\"buffers\":"<<desc->BufferCount<<'}';
    impl->log(s.str());
    return hr;
}
}
FrameGeneration &FrameGeneration::instance() { static FrameGeneration fg; return fg; }
void FrameGeneration::remember_queue(ID3D12CommandQueue *native) { if(native)native->SetPrivateData(native_queue_guid,sizeof(native),&native); }
bool FrameGeneration::initialize(const fs::path &runtime,const fs::path &logs) {
    if(impl_)return impl_->error.empty();
    impl_=new Impl;auto &i=*impl_;
    std::error_code ec;fs::create_directories(logs,ec);
    i.events.open(logs/"frame_generation.jsonl",std::ios::app);
    {std::lock_guard lock(sl_log_mutex);sl_log.open(logs/"sl.log",std::ios::trunc);}
    const auto interposer=runtime/"sl.interposer.dll";
    if(!signed_by_nvidia(interposer)) {i.fail("sl.interposer.dll missing or not signed by NVIDIA");return false;}
    i.module=LoadLibraryExW(interposer.c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
    if(!i.module) {i.fail("LoadLibrary sl.interposer.dll failed");return false;}
    if(!i.core(i.init,"slInit") || !i.core(i.shutdown,"slShutdown") || !i.core(i.set_device,"slSetD3DDevice") || !i.core(i.upgrade,"slUpgradeInterface") ||
       !i.core(i.feature_function,"slGetFeatureFunction") || !i.core(i.new_frame,"slGetNewFrameToken") || !i.core(i.set_tags,"slSetTagForFrame") ||
       !i.core(i.set_constants,"slSetConstants") || !i.core(i.loaded,"slIsFeatureLoaded"))return false;
    static const sl::Feature features[]={sl::kFeatureDLSS_G,sl::kFeatureReflex,sl::kFeaturePCL};
    static std::wstring plugins,logs_path;plugins=runtime.wstring();logs_path=logs.wstring();
    static const wchar_t *plugin_paths[1];plugin_paths[0]=plugins.c_str();
    sl::Preferences pref{};
    pref.logLevel=sl::LogLevel::eDefault;pref.logMessageCallback=on_sl_log;
    pref.pathsToPlugins=plugin_paths;pref.numPathsToPlugins=1;pref.pathToLogsAndData=logs_path.c_str();
    // No OTA downloads: only the verified local plugins are loaded.
    pref.flags=sl::PreferenceFlags::eDisableCLStateTracking|sl::PreferenceFlags::eUseManualHooking|sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.featuresToLoad=features;pref.numFeaturesToLoad=3;
    // Same identity as our NGX DLSS path; SL needs both fields to take the ProjectID route.
    pref.engine=sl::EngineType::eCustom;pref.projectId="9b048dd7-c4b2-4ec1-8110-7c13b89bc44b";pref.engineVersion="GhostMasterDLSS-prototype";
    pref.renderAPI=sl::RenderAPI::eD3D12;
    if(i.init(pref,sl::kSDKVersion)!=sl::Result::eOk) {i.fail("slInit failed");return false;}
    // Redirect the real factory's v-table slot. All DXGI factories share it, including the one ReShade wraps.
    HMODULE dxgi=GetModuleHandleW(L"dxgi.dll");
    wchar_t system[MAX_PATH]{};GetSystemDirectoryW(system,MAX_PATH);
    HMODULE system_dxgi=LoadLibraryExW((fs::path(system)/"dxgi.dll").c_str(),nullptr,0);
    if(system_dxgi)dxgi=system_dxgi;
    auto create=reinterpret_cast<decltype(&CreateDXGIFactory2)>(GetProcAddress(dxgi,"CreateDXGIFactory2"));
    ComPtr<IDXGIFactory2> factory;
    if(!create || FAILED(create(0,IID_PPV_ARGS(&factory)))) {i.fail("CreateDXGIFactory2 failed");return false;}
    i.slot=*reinterpret_cast<void ***>(factory.Get())+create_swapchain_for_hwnd_slot;
    DWORD protect=0;
    if(!VirtualProtect(i.slot,sizeof(void *),PAGE_READWRITE,&protect)) {i.fail("VirtualProtect failed");return false;}
    i.original=reinterpret_cast<CreateSwapChainForHwnd>(*i.slot);
    // Pin this module before any v-table slot points into it.
    HMODULE self=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(&create_swapchain_for_hwnd),&self)) {
        VirtualProtect(i.slot,sizeof(void *),protect,&protect);i.fail("could not pin ghostmaster_fg.dll");return false;
    }
    active=impl_;
    *i.slot=reinterpret_cast<void *>(&create_swapchain_for_hwnd);
    VirtualProtect(i.slot,sizeof(void *),protect,&protect);
    i.log("{\"event\":\"fg_initialized\"}");
    return true;
}
void FrameGeneration::set_enabled(bool enabled) {
    if(!impl_)return;
    if(impl_->enabled.exchange(enabled)!=enabled) {std::lock_guard lock(impl_->mutex);impl_->options_dirty=true;}
}
void FrameGeneration::before_present() {
    if(!impl_ || !impl_->swapchain)return;
    if(impl_->in_present)after_present();
    auto &i=*impl_;std::lock_guard lock(i.mutex);
    i.in_present=true;
    if(i.options_dirty && i.features) {
        sl::DLSSGOptions options{};options.mode=i.enabled?sl::DLSSGMode::eOn:sl::DLSSGMode::eOff;options.numFramesToGenerate=1;
        if(i.dlssg_set(sl::ViewportHandle{0},options)==sl::Result::eOk) {
            i.options_dirty=false;i.log(std::string("{\"event\":\"fg_mode\",\"enabled\":")+(i.enabled?"true":"false")+"}");
        } else i.fail("slDLSSGSetOptions failed");
    }
    if(!i.token)return;
    i.marker(sl::PCLMarker::eRenderSubmitEnd,*i.token);i.marker(sl::PCLMarker::ePresentStart,*i.token);
}
void FrameGeneration::after_present() {
    if(!impl_ || !impl_->swapchain)return;
    auto &i=*impl_;std::lock_guard lock(i.mutex);
    if(!i.in_present)return;
    i.in_present=false;
    if(i.token)i.marker(sl::PCLMarker::ePresentEnd,*i.token);
    if(i.features) {
        sl::DLSSGState state{};
        if(i.dlssg_state(sl::ViewportHandle{0},state,nullptr)==sl::Result::eOk) {
            i.dlssg_status=uint32_t(state.status);i.max_generated=state.numFramesToGenerateMax;i.presented+=state.numFramesActuallyPresented;
        }
    }
    ++i.frames;++i.frame_index;
    i.begin_frame();
}
void FrameGeneration::tag(ID3D12GraphicsCommandList *list,ID3D12Resource *depth,ID3D12Resource *motion,unsigned width,unsigned height,
    const Camera &camera,bool reset) {
    if(!impl_ || !impl_->swapchain || !impl_->features)return;
    auto &i=*impl_;std::lock_guard lock(i.mutex);
    if(!i.token || i.tagged)return;
    const sl::Extent extent{0,0,width,height},backbuffer{0,0,i.backbuffer_width,i.backbuffer_height};
    sl::Resource depth_resource{sl::ResourceType::eTex2d,depth,nullptr,nullptr,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    sl::Resource motion_resource{sl::ResourceType::eTex2d,motion,nullptr,nullptr,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    sl::ResourceTag tags[]={{&depth_resource,sl::kBufferTypeDepth,sl::ResourceLifecycle::eValidUntilPresent,&extent},
        {&motion_resource,sl::kBufferTypeMotionVectors,sl::ResourceLifecycle::eValidUntilPresent,&extent},
        {nullptr,sl::kBufferTypeBackbuffer,sl::ResourceLifecycle::eValidUntilPresent,&backbuffer}};
    if(i.set_tags(*i.token,sl::ViewportHandle{0},tags,3,list)!=sl::Result::eOk) {i.fail("slSetTagForFrame failed");return;}
    sl::Constants c{};
    c.cameraViewToClip=matrix(camera.view_to_clip);c.clipToCameraView=matrix(camera.clip_to_view);
    c.clipToLensClip=matrix({1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1});
    c.clipToPrevClip=matrix(camera.clip_to_prev_clip);c.prevClipToClip=matrix(camera.prev_clip_to_clip);
    c.jitterOffset={camera.jitter_x,camera.jitter_y};c.mvecScale={1.0f/width,1.0f/height};c.cameraPinholeOffset={0,0};
    c.cameraPos={camera.position[0],camera.position[1],camera.position[2]};
    c.cameraUp={camera.up[0],camera.up[1],camera.up[2]};c.cameraRight={camera.right[0],camera.right[1],camera.right[2]};
    c.cameraFwd={camera.forward[0],camera.forward[1],camera.forward[2]};
    c.cameraNear=camera.near_plane;c.cameraFar=camera.far_plane;c.cameraFOV=camera.fov_y;c.cameraAspectRatio=camera.aspect;
    c.depthInverted=sl::Boolean::eTrue;c.cameraMotionIncluded=sl::Boolean::eTrue;c.motionVectors3D=sl::Boolean::eFalse;
    c.reset=reset?sl::Boolean::eTrue:sl::Boolean::eFalse;c.motionVectorsJittered=sl::Boolean::eFalse;
    if(i.set_constants(c,*i.token,sl::ViewportHandle{0})!=sl::Result::eOk) {i.fail("slSetConstants failed");return;}
    i.tagged=true;
}
FrameGeneration::Status FrameGeneration::status() {
    Status s;if(!impl_)return s;
    auto &i=*impl_;std::lock_guard lock(i.mutex);
    s.initialized=true;s.loaded=i.module!=nullptr;s.hooked=i.original!=nullptr;s.swapchain=i.swapchain;s.enabled=i.enabled;
    s.dlssg_status=i.dlssg_status;s.max_generated=i.max_generated;s.frames=i.frames;s.presented=i.presented;s.error=i.error;
    i.frames=0;i.presented=0;
    return s;
}
void FrameGeneration::shutdown() {
    if(!impl_)return;
    auto &i=*impl_;
    if(i.slot && i.original) {DWORD protect=0;if(VirtualProtect(i.slot,sizeof(void *),PAGE_READWRITE,&protect)){*i.slot=reinterpret_cast<void *>(i.original);VirtualProtect(i.slot,sizeof(void *),protect,&protect);}}
    if(i.features && i.swapchain) {sl::DLSSGOptions off{};off.mode=sl::DLSSGMode::eOff;i.dlssg_set(sl::ViewportHandle{0},off);}
    if(i.shutdown)i.shutdown();
    i.features=false;i.swapchain=false;i.token=nullptr;
}
