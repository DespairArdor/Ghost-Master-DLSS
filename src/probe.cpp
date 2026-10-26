// Observer build, or opt-in DLSS temporal-pass integration when GM_LIVE is defined.
#ifdef GM_LIVE
#include <imgui.h>
#endif
#include <reshade.hpp>
#include <d3d12.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cwchar>
#include <iterator>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <atomic>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>
#ifdef GM_LIVE
#include "live_bridge.hpp"
#include "frame_generation.hpp"
#include <cmath>
#include <wintrust.h>
#include <softpub.h>
#endif
using namespace reshade::api;
namespace fs = std::filesystem;
namespace {
std::mutex mutex;
fs::path output;
std::ofstream trace;
uint64_t frame = 0, lines = 0, dump_bytes = 0;
std::atomic<bool> capture{false};
bool f8_down = false;
// Shader dumps, dispatch capture windows and constant dumps. Always on in the probe; opt-in for the live addon.
bool diagnostics = true;
// CPU time spent inside our command-list/descriptor callbacks, summed over all render threads (includes lock waits).
std::atomic<uint64_t> addon_ticks{0}, descriptor_writes{0};
struct CpuTimer {
    LARGE_INTEGER start{};
    CpuTimer() { QueryPerformanceCounter(&start); }
    ~CpuTimer() { LARGE_INTEGER end{}; QueryPerformanceCounter(&end); addon_ticks += uint64_t(end.QuadPart-start.QuadPart); }
};
#ifdef GM_LIVE
fs::path runtime_directory,control_file;
std::string live_mode="off",startup_error;
// Registers only present/overlay callbacks: measures the game without our tracking cost.
bool baseline=false;
// NVSDK_NGX_DLSS_Hint_Render_Preset from ini Preset=J/K/L/M; 0 keeps NVIDIA's default per quality mode.
unsigned dlss_preset=0;
bool show_hud=true,screenshot_pending=false;
// The status line sits over ReShade's tab bar: hidden while the overlay is open.
std::atomic<bool> overlay_open{false};
// DLSS Frame Generation: FrameGeneration=1 loads Streamline at startup (needed before the swap chain exists);
// the menu switch only turns interpolation on/off while it is loaded.
bool fg_wanted=true,fg_enabled=true;
fs::path streamline_directory;
double shown_fg_ratio=0;uint32_t shown_fg_status=0;std::string shown_fg_error;bool shown_fg_swapchain=false;
// Settings file next to the addon; the ReShade menu writes its changes back here.
std::wstring settings_file;
// Render scale owned by the addon (ScaleSource=addon) instead of the game's Resolution Quality slider.
bool scale_override=false;
float scale_percent=66.6667f;
std::string snapshot_token;
// Present-to-present timing, logged every two seconds as a "perf" event for on/off comparisons.
LARGE_INTEGER perf_frequency{},perf_last{},perf_window{};
uint64_t perf_frames=0;double perf_max_ms=0,shown_fps=0,shown_ms=0,shown_addon_ms=0;
#endif
std::set<uint64_t> dumped;
constexpr uint64_t max_lines = 300000, max_dump_bytes = 256ull << 20;
struct Descriptor { descriptor_type type{}; resource_view view{}; buffer_range buffer{}; };
struct Param { std::vector<descriptor_range> ranges; };
struct SlotHash { size_t operator()(const std::pair<uint64_t,uint32_t> &k) const noexcept { return size_t(k.first*0x9e3779b97f4a7c15ull ^ k.second); } };
// Under `mutex`. resources/layouts/descriptors are tracked only with diagnostics: they need the full binding picture.
struct Device {
#ifdef GM_LIVE
    std::unique_ptr<LiveBridge> bridge;
    std::set<std::string> skips;
#endif
    std::unordered_map<uint64_t, resource_desc> resources;
    std::unordered_map<uint64_t, std::vector<Param>> layouts;
    std::unordered_map<std::pair<uint64_t, uint32_t>, Descriptor, SlotHash> descriptors;
};
// Root parameter tables bound on one command list. Fixed arrays: binds happen thousands of times per frame.
struct Tables {
    std::array<descriptor_table,64> table{};
    uint64_t mask=0;
    void clear() { mask=0; }
    void set(uint32_t param,descriptor_table t) { if(param<64) { table[param]=t; mask|=1ull<<param; } }
    bool get(uint32_t param,descriptor_table &t) const { if(param>=64 || !(mask>>param&1)) return false; t=table[param]; return true; }
    template<class F> void each(F f) const { for(uint64_t m=mask;m;m&=m-1) { const auto i=uint32_t(std::countr_zero(m)); f(i,table[i]); } }
};
// Per-command-list state lives in ReShade private data: a command list is recorded by one thread at a time, so no lock.
struct __declspec(uuid("8f7f2d1c-4b0e-4c55-9a64-2f0c6d3e7b51")) Command {
    uint64_t shader = 0, layout = 0, generation = 0, native_pipeline=0, graphics_layout=0;
    Tables tables, graphics_tables;
    std::map<std::pair<uint32_t,uint32_t>, Descriptor> roots;
};
std::unordered_map<device *, Device> devices;
// Compute pipeline -> shader hash. Read on every compute bind, so a shared lock separate from `mutex`.
std::shared_mutex shaders_mutex;
std::unordered_map<uint64_t, uint64_t> shaders;
bool is_taau(uint64_t hash) { return hash==0xfc434178cfb8e489ull || hash==0x0f1634b465b59707ull; }
Command &state(command_list *c) {
    if(auto *s=c->get_private_data<Command>()) return *s;
    return *c->create_private_data<Command>();
}
// Lock-free mirror of SRV/UAV descriptors: one atomic slot per heap entry holding the view handle with the type in the top byte.
// UE writes ~20k descriptors per frame from several threads; a map behind a mutex cost ~120 ms CPU per frame.
struct HeapMirror { uint32_t size=0; std::unique_ptr<std::atomic<uint64_t>[]> slots; };
std::shared_mutex heaps_mutex;
std::unordered_map<uint64_t, HeapMirror *> heaps;
std::vector<std::unique_ptr<HeapMirror>> heap_storage; // Kept until unload: other threads may still hold a pointer.
HeapMirror *mirror(uint64_t heap, uint64_t needed) {
    // Copies alternate between a CPU staging heap and the shader-visible heap: keep a few per thread.
    struct Cached { uint64_t heap=0; HeapMirror *mirror=nullptr; };
    thread_local std::array<Cached,4> cache{}; thread_local uint32_t next=0;
    for(const auto &c:cache) if(c.heap==heap && c.mirror && needed<=c.mirror->size) return c.mirror;
    if(!heap) return nullptr;
    auto remember=[&](HeapMirror *m) { cache[next++%cache.size()]={heap,m}; return m; };
    {
        std::shared_lock lock(heaps_mutex);
        if(auto it=heaps.find(heap); it!=heaps.end() && needed<=it->second->size) return remember(it->second);
    }
    const auto size=reinterpret_cast<ID3D12DescriptorHeap *>(heap)->GetDesc().NumDescriptors;
    if(needed>size) return nullptr;
    std::unique_lock lock(heaps_mutex);
    auto &entry=heaps[heap];
    if(!entry || entry->size<size) { // New heap, or a new heap reusing a freed heap's address.
        auto m=std::make_unique<HeapMirror>(); m->size=size; m->slots=std::make_unique<std::atomic<uint64_t>[]>(size);
        entry=m.get(); heap_storage.push_back(std::move(m));
    }
    return remember(entry);
}
uint64_t pack(descriptor_type type, resource_view view) {
    switch(type) {
    case descriptor_type::shader_resource_view: case descriptor_type::unordered_access_view:
        return view.handle>>56 ? 0 : view.handle | uint64_t(type)<<56;
    default: return 0;
    }
}
Descriptor unpack(uint64_t value) { Descriptor d; d.type=descriptor_type(value>>56); d.view={value&((1ull<<56)-1)}; return d; }
uint64_t hash_bytes(const void *data, size_t size) {
    uint64_t hash = 14695981039346656037ull;
    const auto bytes = static_cast<const uint8_t *>(data);
    for (size_t i=0; i<size; ++i) { hash ^= bytes[i]; hash *= 1099511628211ull; }
    return hash;
}
std::string hex(uint64_t value) { std::ostringstream s; s << std::hex << std::setw(16) << std::setfill('0') << value; return s.str(); }
std::string quote(const std::string &str) {
    std::ostringstream s; s << '"';
    for (unsigned char c : str) {
        if (c == '"' || c == '\\') s << '\\' << c;
        else if (c < 32 || c >= 127) s << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
        else s << c;
    }
    s << '"'; return s.str();
}
void log(const std::string &message) {
    if (trace && lines++ < max_lines) trace << message << '\n';
}
bool dx12(device *d) { return d->get_api() == device_api::d3d12; }
void init_resource(device *d,const resource_desc &desc,const subresource_data *,resource_usage,resource r) {
    if(!dx12(d)) return;
    std::lock_guard lock(mutex); devices[d].resources[r.handle]=desc;
}
void destroy_resource(device *d,resource r) { std::lock_guard lock(mutex); if(devices.contains(d)) devices[d].resources.erase(r.handle); }
Descriptor read_descriptor(const descriptor_table_update &u, uint32_t i) {
    Descriptor d; d.type=u.type;
    switch (u.type) {
    case descriptor_type::constant_buffer:
    case descriptor_type::shader_storage_buffer: d.buffer=static_cast<const buffer_range *>(u.descriptors)[i]; break;
    case descriptor_type::texture_shader_resource_view:
    case descriptor_type::texture_unordered_access_view:
    case descriptor_type::buffer_shader_resource_view:
    case descriptor_type::buffer_unordered_access_view: d.view=static_cast<const resource_view *>(u.descriptors)[i]; break;
    default: break;
    }
    return d;
}
std::pair<uint64_t,uint32_t> slot(device *d, descriptor_table t, uint32_t binding, uint32_t array = 0) {
    descriptor_heap h{}; uint32_t offset=0;
    d->get_descriptor_heap_offset(t,binding,array,&h,&offset);
    return {h.handle,offset};
}
#ifdef GM_LIVE
// Returns an empty string when the DLL has a valid Authenticode signature from NVIDIA.
std::string verify_runtime(const fs::path &dll) {
    std::error_code ec;if(!fs::exists(dll,ec))return "nvngx_dlss.dll not found";
    const auto file=dll.wstring();
    WINTRUST_FILE_INFO fi{sizeof(fi)};fi.pcwszFilePath=file.c_str();
    WINTRUST_DATA wd{sizeof(wd)};wd.dwUIChoice=WTD_UI_NONE;wd.fdwRevocationChecks=WTD_REVOKE_NONE;wd.dwUnionChoice=WTD_CHOICE_FILE;
    wd.pFile=&fi;wd.dwStateAction=WTD_STATEACTION_VERIFY;wd.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action=WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const auto trust=WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE),&action,&wd);
    std::string error;
    if(trust!=ERROR_SUCCESS)error="nvngx_dlss.dll signature invalid";
    else if(auto *provider=WTHelperProvDataFromStateData(wd.hWVTStateData)) {
        auto *signer=WTHelperGetProvSignerFromChain(provider,0,FALSE,0);
        wchar_t name[256]{};
        if(!signer || !signer->csCertChain || !CertGetNameStringW(signer->pasCertChain[0].pCert,CERT_NAME_SIMPLE_DISPLAY_TYPE,0,nullptr,name,256) ||
           std::wstring(name).find(L"NVIDIA")==std::wstring::npos)error="nvngx_dlss.dll is not signed by NVIDIA";
    } else error="nvngx_dlss.dll signer unavailable";
    wd.dwStateAction=WTD_STATEACTION_CLOSE;WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE),&action,&wd);
    return error;
}
#endif
#ifdef GM_LIVE
// r.ScreenPercentage of this exact UE 5.5.1 build (exe SHA256 0e5e8471...). Its TAutoConsoleVariable<float> is a static at
// RVA 0x912d8c0: +0 vtable (RVA 0x6f2e780, stored over the base-class one at 0xdf73dd), +8 IConsoleVariable*, +0x10 TConsoleVariableData<float>* (ShadowedValue[2]).
// The view-resolution code at RVA 0x39c76f0 reads ShadowedValue[0] every frame, so writing it applies immediately.
// Before touching memory, the registration code is compared byte for byte; any other build leaves the feature disabled.
constexpr uint8_t cvar_register_code[]={0xe8,0x0b,0xf6,0x29,0x00,0x48,0x8b,0x0d,0xb4,0x99,0x0b,0x08,0x48,0x8b,0x01,0x4c,0x8d,0x0d,0x7a,0x2d,0xad,0x06,
    0x48,0x89,0x5c,0x24,0x40,0x48,0x8d,0x15,0x9e,0x30,0xad,0x06,0x0f,0x57,0xd2,0x48,0x89,0x7c,0x24,0x30,0xc7,0x44,0x24,0x20,0x40,0x00,0x00,0x00,
    0xff,0x50,0x10,0x48,0x8d,0x0d,0xac,0x0b,0x39,0x07,0x48,0x89,0x05,0x75,0x65,0x33,0x08,0x48,0x89,0x0d,0x66,0x65,0x33,0x08};
constexpr uint8_t cvar_ref_code[]={0x48,0x8b,0x0d,0xf2,0x64,0x33,0x08,0x48,0x8d,0x05,0xa3,0x73,0x13,0x06,0x48,0x89,0x05,0xdc,0x64,0x33,0x08,
    0x48,0x8b,0x01,0xff,0x50,0x70,0x48,0x8d,0x0d,0x3f,0x25,0x08,0x06,0x48,0x89,0x05,0xd8,0x64,0x33,0x08};
float *screen_percentage=nullptr;
float game_percentage=0; // The game's own value, restored when the user hands control back to the game slider.
bool scale_checked=false,scale_restore=false;
std::string scale_error="not checked yet";
bool read_pair(const float *p,float *out) { __try { out[0]=p[0];out[1]=p[1];return true; } __except(EXCEPTION_EXECUTE_HANDLER) { return false; } }
bool write_pair(float *p,float value) { __try { p[0]=value;p[1]=value;return true; } __except(EXCEPTION_EXECUTE_HANDLER) { return false; } }
void locate_screen_percentage() {
    scale_checked=true;
    struct Report { ~Report() { if(!screen_percentage)log("{\"event\":\"screen_percentage_unavailable\",\"reason\":"+quote(scale_error)+"}"); } } report;
    const auto *base=reinterpret_cast<const uint8_t *>(GetModuleHandleW(nullptr));
    const auto *dos=reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    const auto *nt=reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base+dos->e_lfanew);
    if(nt->OptionalHeader.SizeOfImage<0x912d8e0 || std::memcmp(base+0xdf7310,cvar_register_code,sizeof(cvar_register_code)) ||
       std::memcmp(base+0xdf73cf,cvar_ref_code,sizeof(cvar_ref_code))) {scale_error="unsupported game build";return;}
    const auto *object=reinterpret_cast<const uint64_t *>(base+0x912d8c0);
    if(object[0]!=uint64_t(base+0x6f2e780) || !object[1] || object[2]<=object[1] || object[2]>object[1]+1024) {scale_error="console variable not initialized";return;}
    float value[2]{};
    if(!read_pair(reinterpret_cast<const float *>(object[2]),value) || !std::isfinite(value[0]) || value[0]<0 || value[0]>400) {scale_error="console variable unreadable";return;}
    screen_percentage=reinterpret_cast<float *>(object[2]);game_percentage=value[0];scale_error.clear();
    std::ostringstream s;s<<"{\"event\":\"screen_percentage_found\",\"game_value\":"<<value[0]<<'}';log(s.str());
}
// Called at every present: keeps r.ScreenPercentage at the addon's value, from the first frame of the session.
void enforce_scale() {
    if(!scale_checked)locate_screen_percentage();
    if(!screen_percentage)return;
    float value[2]{};
    if(!read_pair(screen_percentage,value)) {screen_percentage=nullptr;scale_error="console variable unreadable";return;}
    if(scale_override) {
        if(value[0]!=scale_percent || value[1]!=scale_percent) {
            if(!write_pair(screen_percentage,scale_percent)) {screen_percentage=nullptr;scale_error="console variable not writable";return;}
            std::ostringstream s;s<<"{\"event\":\"screen_percentage_set\",\"from\":"<<value[0]<<",\"to\":"<<scale_percent<<'}';log(s.str());
        }
        scale_restore=true;
    } else if(scale_restore) {
        scale_restore=false;
        if(game_percentage>0 && write_pair(screen_percentage,game_percentage))log("{\"event\":\"screen_percentage_restored\"}");
    } else game_percentage=value[0]; // Follow the game slider while it is in charge.
}
// General 4x4 inverse (cofactors); returns identity for singular input.
std::array<float,16> inverse(const std::array<float,16> &m) {
    std::array<double,16> inv{};
    inv[0]=m[5]*m[10]*m[15]-m[5]*m[11]*m[14]-m[9]*m[6]*m[15]+m[9]*m[7]*m[14]+m[13]*m[6]*m[11]-m[13]*m[7]*m[10];
    inv[4]=-m[4]*m[10]*m[15]+m[4]*m[11]*m[14]+m[8]*m[6]*m[15]-m[8]*m[7]*m[14]-m[12]*m[6]*m[11]+m[12]*m[7]*m[10];
    inv[8]=m[4]*m[9]*m[15]-m[4]*m[11]*m[13]-m[8]*m[5]*m[15]+m[8]*m[7]*m[13]+m[12]*m[5]*m[11]-m[12]*m[7]*m[9];
    inv[12]=-m[4]*m[9]*m[14]+m[4]*m[10]*m[13]+m[8]*m[5]*m[14]-m[8]*m[6]*m[13]-m[12]*m[5]*m[10]+m[12]*m[6]*m[9];
    inv[1]=-m[1]*m[10]*m[15]+m[1]*m[11]*m[14]+m[9]*m[2]*m[15]-m[9]*m[3]*m[14]-m[13]*m[2]*m[11]+m[13]*m[3]*m[10];
    inv[5]=m[0]*m[10]*m[15]-m[0]*m[11]*m[14]-m[8]*m[2]*m[15]+m[8]*m[3]*m[14]+m[12]*m[2]*m[11]-m[12]*m[3]*m[10];
    inv[9]=-m[0]*m[9]*m[15]+m[0]*m[11]*m[13]+m[8]*m[1]*m[15]-m[8]*m[3]*m[13]-m[12]*m[1]*m[11]+m[12]*m[3]*m[9];
    inv[13]=m[0]*m[9]*m[14]-m[0]*m[10]*m[13]-m[8]*m[1]*m[14]+m[8]*m[2]*m[13]+m[12]*m[1]*m[10]-m[12]*m[2]*m[9];
    inv[2]=m[1]*m[6]*m[15]-m[1]*m[7]*m[14]-m[5]*m[2]*m[15]+m[5]*m[3]*m[14]+m[13]*m[2]*m[7]-m[13]*m[3]*m[6];
    inv[6]=-m[0]*m[6]*m[15]+m[0]*m[7]*m[14]+m[4]*m[2]*m[15]-m[4]*m[3]*m[14]-m[12]*m[2]*m[7]+m[12]*m[3]*m[6];
    inv[10]=m[0]*m[5]*m[15]-m[0]*m[7]*m[13]-m[4]*m[1]*m[15]+m[4]*m[3]*m[13]+m[12]*m[1]*m[7]-m[12]*m[3]*m[5];
    inv[14]=-m[0]*m[5]*m[14]+m[0]*m[6]*m[13]+m[4]*m[1]*m[14]-m[4]*m[2]*m[13]-m[12]*m[1]*m[6]+m[12]*m[2]*m[5];
    inv[3]=-m[1]*m[6]*m[11]+m[1]*m[7]*m[10]+m[5]*m[2]*m[11]-m[5]*m[3]*m[10]-m[9]*m[2]*m[7]+m[9]*m[3]*m[6];
    inv[7]=m[0]*m[6]*m[11]-m[0]*m[7]*m[10]-m[4]*m[2]*m[11]+m[4]*m[3]*m[10]+m[8]*m[2]*m[7]-m[8]*m[3]*m[6];
    inv[11]=-m[0]*m[5]*m[11]+m[0]*m[7]*m[9]+m[4]*m[1]*m[11]-m[4]*m[3]*m[9]-m[8]*m[1]*m[7]+m[8]*m[3]*m[5];
    inv[15]=m[0]*m[5]*m[10]-m[0]*m[6]*m[9]-m[4]*m[1]*m[10]+m[4]*m[2]*m[9]+m[8]*m[1]*m[6]-m[8]*m[2]*m[5];
    const double det=m[0]*inv[0]+m[1]*inv[4]+m[2]*inv[8]+m[3]*inv[12];
    std::array<float,16> r{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    if(std::abs(det)<1e-20)return r;
    for(int i=0;i<16;++i)r[i]=float(inv[i]/det);
    return r;
}
// Camera for DLSS-G from the UE 5.5 View uniform buffer (float4 registers, verified against captured buffers):
// [20] ViewToTranslatedWorld, [32] ViewToClipNoAA, [136] ClipToPrevClip. Translated world puts the camera at the origin.
FrameGeneration::Camera camera_from_view(const float *view,float jitter_x,float jitter_y) {
    auto block=[&](int reg) {std::array<float,16> m{};std::copy_n(view+reg*4,16,m.begin());return m;};
    FrameGeneration::Camera c;
    c.view_to_clip=block(32);c.clip_to_view=inverse(c.view_to_clip);
    c.clip_to_prev_clip=block(136);c.prev_clip_to_clip=inverse(c.clip_to_prev_clip);
    const auto to_world=block(20);
    c.right={to_world[0],to_world[1],to_world[2]};c.up={to_world[4],to_world[5],to_world[6]};
    c.forward={to_world[8],to_world[9],to_world[10]};c.position={to_world[12],to_world[13],to_world[14]};
    c.near_plane=c.view_to_clip[14]>0?c.view_to_clip[14]:10.0f;
    c.fov_y=c.view_to_clip[5]>0?2*std::atan(1/c.view_to_clip[5]):1.0f;
    c.aspect=c.view_to_clip[0]>0?c.view_to_clip[5]/c.view_to_clip[0]:1.0f;
    c.jitter_x=jitter_x;c.jitter_y=jitter_y;
    return c;
}
std::string preset_name(unsigned preset) { return preset>=10 && preset<=13 ? std::string(1,char('J'+preset-10)) : "default"; }
void save_settings() {
    if(settings_file.empty())return;
    auto put=[](const wchar_t *key,const std::string &value) {WritePrivateProfileStringW(L"DLSS",key,fs::path(value).wstring().c_str(),settings_file.c_str());};
    std::ostringstream percent;percent<<std::fixed<<std::setprecision(1)<<scale_percent;
    put(L"Mode",live_mode);put(L"ShowStatus",show_hud?"1":"0");put(L"Preset",preset_name(dlss_preset));
    put(L"ScaleSource",scale_override?"addon":"game");put(L"ScreenPercentage",percent.str());
    put(L"FrameGeneration",fg_enabled?"1":"0");
}
#endif
void on_device(device *d) {
    if (!dx12(d)) return;
    std::lock_guard lock(mutex);
    if (!trace.is_open()) {
        std::error_code ec; fs::create_directories(diagnostics ? output / "shaders" : output,ec);
        trace.open(output / "capture.jsonl",std::ios::trunc);
#ifdef GM_LIVE
        log("{\"event\":\"start\",\"mode\":\"live_prototype\",\"api\":\"d3d12\"}");
#else
        log("{\"event\":\"start\",\"mode\":\"diagnostic_only\",\"api\":\"d3d12\"}");
        reshade::log::message(reshade::log::level::info,"GhostMaster probe active (diagnostic only). F8 captures one frame; automatic sampling every 600 presents.");
#endif
    }
    devices.try_emplace(d);
#ifdef GM_LIVE
    if(baseline)return;
    startup_error=verify_runtime(runtime_directory/"nvngx_dlss.dll");
    if(startup_error.empty()) {
        auto &bridge=devices[d].bridge;
        bridge=std::make_unique<LiveBridge>(reinterpret_cast<ID3D12Device *>(d->get_native()),runtime_directory,output,log);
        bridge->snapshots=diagnostics;bridge->set_preset(dlss_preset);
        log("{\"event\":\"dlss_settings\",\"preset\":"+std::to_string(dlss_preset)+"}");
    }
    else log("{\"event\":\"dlss_disabled\",\"reason\":"+quote(startup_error)+"}");
    // The game creates and destroys probe devices first; Streamline and the swap-chain hook must be ready before the real
    // swap chain is created, and survive the add-on being unloaded in between (ghostmaster_fg.dll pins itself).
    if(fg_wanted) {
        auto &fg=FrameGeneration::instance();
        if(!fg.status().initialized) {
            const bool ok=fg.initialize(streamline_directory,output/"streamline");
            log(std::string("{\"event\":\"fg_initialize\",\"ok\":")+(ok?"true":"false")+",\"error\":"+quote(fg.status().error)+"}");
        }
        fg.set_enabled(fg_enabled);
    }
#endif
}
void destroy_device(device *d) { std::lock_guard lock(mutex); devices.erase(d); }
#ifdef GM_LIVE
void init_queue(command_queue *queue) {
    if(dx12(queue->get_device()) && (queue->get_type()&command_queue_type::graphics)!=0)
        FrameGeneration::remember_queue(reinterpret_cast<ID3D12CommandQueue *>(queue->get_native()));
}
void finish_present(command_queue *,swapchain *) { if(fg_wanted)FrameGeneration::instance().after_present(); }
#endif
void init_pipeline(device *d,pipeline_layout,uint32_t count,const pipeline_subobject *objects,pipeline p) {
    if (!dx12(d)) return;
    for (uint32_t i=0;i<count;++i) if (objects[i].type == pipeline_subobject_type::compute_shader && objects[i].count) {
        const auto &s=*static_cast<const shader_desc *>(objects[i].data);
        if (!s.code || !s.code_size) continue;
        auto hash=hash_bytes(s.code,s.code_size);
        {std::unique_lock lock(shaders_mutex); shaders[p.handle]=hash;}
        if (!diagnostics) continue;
        std::lock_guard lock(mutex);
        if (!dumped.contains(hash) && dump_bytes+s.code_size<=max_dump_bytes) {
            std::ofstream f(output/"shaders"/(hex(hash)+".cso"),std::ios::binary);
            f.write(static_cast<const char *>(s.code),s.code_size);
            if (f) { dumped.insert(hash); dump_bytes+=s.code_size; }
        }
        log("{\"event\":\"pipeline\",\"pipeline\":\""+hex(p.handle)+"\",\"shader\":\""+hex(hash)+"\",\"bytes\":"+std::to_string(s.code_size)+"}");
    }
}
void destroy_pipeline(device *,pipeline p) { std::unique_lock lock(shaders_mutex); shaders.erase(p.handle); }
void init_layout(device *d,uint32_t n,const pipeline_layout_param *p,pipeline_layout l) {
    if (!dx12(d)) return;
    std::lock_guard lock(mutex); auto &params=devices[d].layouts[l.handle]; params.resize(n);
    for(uint32_t i=0;i<n;++i) {
        if(p[i].type==pipeline_layout_param_type::descriptor_table || p[i].type==pipeline_layout_param_type::push_descriptors_with_ranges)
            params[i].ranges.assign(p[i].descriptor_table.ranges,p[i].descriptor_table.ranges+p[i].descriptor_table.count);
        else if(p[i].type==pipeline_layout_param_type::descriptor_table_with_flags || p[i].type==pipeline_layout_param_type::push_descriptors_with_ranges_and_flags)
            for(uint32_t j=0;j<p[i].descriptor_table_with_flags.count;++j) params[i].ranges.push_back(p[i].descriptor_table_with_flags.ranges[j]);
        else if(p[i].type==pipeline_layout_param_type::push_descriptors) params[i].ranges.push_back(p[i].push_descriptors);
    }
}
void destroy_layout(device *d,pipeline_layout l) { std::lock_guard lock(mutex); if(devices.contains(d)) devices[d].layouts.erase(l.handle); }
bool update_tables(device *d,uint32_t count,const descriptor_table_update *updates) {
    if (!dx12(d)) return false;
    CpuTimer timer;
    uint64_t written=0;
    for(uint32_t i=0;i<count;++i) {
        const auto &u=updates[i]; const auto key=slot(d,u.table,u.binding,u.array_offset);
        if(auto *m=mirror(key.first,uint64_t(key.second)+u.count)) {
            const bool view=u.type!=descriptor_type::constant_buffer && u.type!=descriptor_type::shader_storage_buffer && u.type!=descriptor_type::sampler;
            for(uint32_t j=0;j<u.count;++j) m->slots[key.second+j].store(view?pack(u.type,static_cast<const resource_view *>(u.descriptors)[j]):0,std::memory_order_relaxed);
        }
        written+=u.count;
    }
    descriptor_writes+=written;
    if(diagnostics) {
        std::lock_guard lock(mutex); auto &data=devices[d];
        for(uint32_t i=0;i<count;++i) {
            const auto &u=updates[i]; auto key=slot(d,u.table,u.binding,u.array_offset);
            for(uint32_t j=0;j<u.count;++j) data.descriptors[{key.first,key.second+j}]=read_descriptor(u,j);
        }
    }
    return false;
}
bool copy_tables(device *d,uint32_t count,const descriptor_table_copy *copies) {
    if (!dx12(d)) return false;
    CpuTimer timer;
    uint64_t written=0;
    for(uint32_t i=0;i<count;++i) {
        const auto &c=copies[i];
        const auto from=slot(d,c.source_table,c.source_binding,c.source_array_offset),to=slot(d,c.dest_table,c.dest_binding,c.dest_array_offset);
        auto *source=mirror(from.first,uint64_t(from.second)+c.count),*dest=mirror(to.first,uint64_t(to.second)+c.count);
        written+=c.count;
        if(!dest) continue;
        // Overlapping ranges in one heap: copy in the safe direction.
        const bool backwards=source==dest && from.second<to.second;
        for(uint32_t k=0;k<c.count;++k) {
            const uint32_t j=backwards?c.count-1-k:k;
            dest->slots[to.second+j].store(source?source->slots[from.second+j].load(std::memory_order_relaxed):0,std::memory_order_relaxed);
        }
    }
    descriptor_writes+=written;
    if(!diagnostics) return false;
    std::lock_guard lock(mutex); auto &data=devices[d];
    for(uint32_t i=0;i<count;++i) {
        const auto &c=copies[i]; auto from=slot(d,c.source_table,c.source_binding,c.source_array_offset);
        auto to=slot(d,c.dest_table,c.dest_binding,c.dest_array_offset);
        std::vector<Descriptor> temporary(c.count);
        for(uint32_t j=0;j<c.count;++j) {
            auto it=data.descriptors.find({from.first,from.second+j});
            if(it!=data.descriptors.end()) temporary[j]=it->second;
        }
        for(uint32_t j=0;j<c.count;++j) data.descriptors[{to.first,to.second+j}]=temporary[j];
    }
    return false;
}
void bind_pipeline(command_list *c,pipeline_stage stage,pipeline p) {
    if (!dx12(c->get_device()) || uint32_t(stage & pipeline_stage::compute_shader)==0) return;
    CpuTimer timer;
    auto &cmd=state(c); cmd.native_pipeline=p.handle;
    std::shared_lock lock(shaders_mutex);
    auto it=shaders.find(p.handle); cmd.shader=it==shaders.end()?0:it->second;
}
void change_layout(Command &c,uint64_t l) { if(c.layout!=l) { c.layout=l; c.tables.clear(); c.roots.clear(); } }
void bind_tables(command_list *c,shader_stage stage,pipeline_layout l,uint32_t first,uint32_t count,const descriptor_table *tables,uint32_t,const uint32_t *) {
    if(!dx12(c->get_device()))return;
    CpuTimer timer;
    auto &cmd=state(c);
    if(uint32_t(stage & shader_stage::compute)==0) {
        if(cmd.graphics_layout!=l.handle) {cmd.graphics_layout=l.handle;cmd.graphics_tables.clear();}
        for(uint32_t i=0;i<count;++i)cmd.graphics_tables.set(first+i,tables[i]);return;
    }
    change_layout(cmd,l.handle);
    for(uint32_t i=0;i<count;++i) cmd.tables.set(first+i,tables[i]);
}
void push(command_list *c,shader_stage stage,pipeline_layout l,uint32_t param,const descriptor_table_update &u) {
    if(!dx12(c->get_device()) || uint32_t(stage & shader_stage::compute)==0) return;
    CpuTimer timer;
    auto &cmd=state(c); change_layout(cmd,l.handle);
    for(uint32_t i=0;i<u.count;++i) cmd.roots[{param,u.binding+i}]=read_descriptor(u,i);
}
void reset(command_list *c) { CpuTimer timer; auto &cmd=state(c); const auto generation=cmd.generation+1; cmd=Command{}; cmd.generation=generation; }
void destroy_command(command_list *c) { if(c->get_private_data<Command>()) c->destroy_private_data<Command>(); }
std::string describe(device *d,const Descriptor &desc,uint32_t param,uint32_t reg,uint32_t space) {
    resource r=desc.view.handle?d->get_resource_from_view(desc.view):desc.buffer.buffer;
    if(!r.handle) return {};
    const auto resource_it=devices[d].resources.find(r.handle);
    if(resource_it==devices[d].resources.end()) return {};
    const auto rd=resource_it->second;
    std::ostringstream s;
    s << "{\"param\":" << param << ",\"register\":" << reg << ",\"space\":" << space
      << ",\"type\":" << uint32_t(desc.type) << ",\"resource\":\"" << hex(r.handle) << "\"";
    if(rd.type==resource_type::buffer) s << ",\"buffer_bytes\":" << rd.buffer.size << ",\"offset\":" << desc.buffer.offset << ",\"range_bytes\":" << desc.buffer.size;
    else s << ",\"width\":" << rd.texture.width << ",\"height\":" << rd.texture.height << ",\"format\":" << uint32_t(rd.texture.format);
    // Names are optional: shipping builds often remove them. Never infer semantic identity from dimensions alone.
    char name[512]{}; UINT bytes=sizeof(name)-1;
    auto native=reinterpret_cast<ID3D12Resource *>(r.handle);
    if(SUCCEEDED(native->GetPrivateData(WKPDID_D3DDebugObjectName,&bytes,name))) s << ",\"name\":" << quote(std::string(name,strnlen_s(name,sizeof(name))));
    s << '}'; return s.str();
}
void capture_constants(device *d,const Command &cmd) {
    if(cmd.shader!=0xfc434178cfb8e489ull && cmd.shader!=0x0f1634b465b59707ull && cmd.shader!=0xb5e23456f2847e5eull) return;
    for(const auto &[key,desc]:cmd.roots) {
        if(desc.type!=descriptor_type::constant_buffer || !desc.buffer.buffer.handle) continue;
        auto it=devices[d].resources.find(desc.buffer.buffer.handle);
        if(it==devices[d].resources.end()) continue;
        auto *r=reinterpret_cast<ID3D12Resource *>(desc.buffer.buffer.handle);
        D3D12_HEAP_PROPERTIES hp{}; D3D12_HEAP_FLAGS flags{};
        if(FAILED(r->GetHeapProperties(&hp,&flags)) || hp.Type!=D3D12_HEAP_TYPE_UPLOAD) continue;
        uint64_t bytes=key.first==3?312:6012; // Observed reflected sizes; scoped to the two exact shader hashes above.
        if(desc.buffer.offset>it->second.buffer.size || bytes>it->second.buffer.size-desc.buffer.offset) continue;
        void *mapped=nullptr; D3D12_RANGE read{SIZE_T(desc.buffer.offset),SIZE_T(desc.buffer.offset+bytes)};
        if(FAILED(r->Map(0,&read,&mapped)) || !mapped) continue;
        std::error_code ec; fs::create_directories(output/"constants",ec);
        auto file=std::to_string(frame)+"-"+hex(cmd.shader)+"-p"+std::to_string(key.first)+".bin";
        std::ofstream f(output/"constants"/file,std::ios::binary);
        f.write(static_cast<const char *>(mapped)+desc.buffer.offset,bytes);
        D3D12_RANGE written{0,0}; r->Unmap(0,&written);
        log("{\"event\":\"constants\",\"frame\":"+std::to_string(frame)+",\"file\":"+quote(file)+",\"param\":"+std::to_string(key.first)+"}");
    }
}
#ifdef GM_LIVE
void destroy_queue(command_queue *queue) {
    std::lock_guard lock(mutex);auto it=devices.find(queue->get_device());
    if(it!=devices.end() && it->second.bridge && it->second.bridge->uses_queue(reinterpret_cast<ID3D12CommandQueue *>(queue->get_native())))it->second.bridge.reset();
}
void runtime_present(effect_runtime *runtime) {
    bool take=false;uint64_t number=0;std::string mode;
    {std::lock_guard lock(mutex);take=screenshot_pending;screenshot_pending=false;number=frame;mode=live_mode;}
    if(!take)return;
    // ReShade may invoke command-list callbacks here: never hold our state mutex across this call.
    uint32_t w=0,h=0;runtime->get_screenshot_width_and_height(&w,&h);
    if(!w || !h || w>8192 || h>8192)return;
    const auto fmt=runtime->get_device()->get_resource_desc(runtime->get_current_back_buffer()).texture.format;
    const bool ten_bit=fmt==format::r10g10b10a2_unorm;
    const bool bgra=fmt==format::b8g8r8a8_unorm || fmt==format::b8g8r8a8_unorm_srgb || fmt==format::b8g8r8x8_unorm || fmt==format::b8g8r8x8_unorm_srgb;
    if(!ten_bit && !bgra && fmt!=format::r8g8b8a8_unorm && fmt!=format::r8g8b8a8_unorm_srgb) {
        std::lock_guard lock(mutex);log("{\"event\":\"screen_snapshot\",\"success\":false,\"reason\":\"unsupported_backbuffer_format\",\"format\":"+std::to_string(uint32_t(fmt))+"}");return;
    }
    std::vector<unsigned char> pixels(size_t(w)*h*4);
    bool success=runtime->capture_screenshot(pixels.data());
    auto name="screen-"+std::to_string(number)+"-"+mode+".ppm";
    if(success) {
        std::ofstream f(output/name,std::ios::binary);f<<"P6\n"<<w<<' '<<h<<"\n255\n";
        std::vector<unsigned char> rgb(size_t(w)*h*3);
        for(size_t i=0,j=0;i<pixels.size();i+=4,j+=3) {
            if(ten_bit) {
                uint32_t v=0;std::memcpy(&v,pixels.data()+i,4);
                for(unsigned c=0;c<3;++c)rgb[j+c]=static_cast<unsigned char>((((v>>(c*10))&1023)*255+511)/1023);
            } else {rgb[j]=pixels[i+(bgra?2:0)];rgb[j+1]=pixels[i+1];rgb[j+2]=pixels[i+(bgra?0:2)];}
        }
        f.write(reinterpret_cast<const char *>(rgb.data()),rgb.size());
        success=bool(f);
    }
    std::lock_guard lock(mutex);log("{\"event\":\"screen_snapshot\",\"success\":"+std::string(success?"true":"false")+",\"format\":"+std::to_string(uint32_t(fmt))+",\"file\":"+quote(name)+"}");
}
void set_mode(const std::string &next) {
    if(!control_file.empty()) {std::ofstream f(control_file);f<<next;} // Picked up by present() within 30 frames.
    else {live_mode=next;for(auto &[_,dev]:devices)if(dev.bridge)dev.bridge->reset_history();log("{\"event\":\"mode\",\"value\":"+quote(live_mode)+"}");}
}
// Add-ons tab of the ReShade overlay (Home key).
void draw_settings(effect_runtime *runtime) {
    std::lock_guard lock(mutex);
    bool changed=false;
    bool enabled=live_mode=="replace";
    if(ImGui::Checkbox("DLSS Super Resolution (F6)",&enabled)) {set_mode(enabled?"replace":"off");if(control_file.empty())changed=true;}
    changed|=ImGui::Checkbox("Show status line (F7)",&show_hud);
    ImGui::Spacing();ImGui::SeparatorText("Render resolution");
    int source=scale_override?1:0;
    if(ImGui::RadioButton("Game slider (Resolution Quality)",&source,0) | ImGui::RadioButton("Set here",&source,1)) {scale_override=source==1;changed=true;}
    if(scale_override) {
        struct Mode {const char *name;float percent;};
        constexpr Mode modes[]={{"Quality",66.7f},{"Balanced",58.0f},{"Performance",50.0f},{"Ultra Performance",33.3f}};
        for(size_t i=0;i<std::size(modes);++i) {
            if(i)ImGui::SameLine();
            if(ImGui::Button(modes[i].name)) {scale_percent=modes[i].percent;changed=true;}
        }
        ImGui::SliderFloat("Input scale",&scale_percent,33.3f,99.0f,"%.1f %%",ImGuiSliderFlags_AlwaysClamp);
        changed|=ImGui::IsItemDeactivatedAfterEdit();
        ImGui::TextDisabled("Applied immediately and at every launch. The game slider is ignored while this is selected.");
    }
    if(!screen_percentage)ImGui::TextColored(ImVec4(1,.45f,.35f,1),"Render scale control unavailable: %s",scale_error.c_str());
    ImGui::Spacing();ImGui::SeparatorText("Frame Generation (DLSS-G 2x)");
    if(ImGui::Checkbox("Frame Generation",&fg_enabled)) {changed=true;if(fg_wanted)FrameGeneration::instance().set_enabled(fg_enabled);}
    if(!fg_wanted)ImGui::TextDisabled(fg_enabled?"Restart the game to load Frame Generation.":"Not loaded (FrameGeneration=0).");
    else if(!shown_fg_error.empty())ImGui::TextColored(ImVec4(1,.45f,.35f,1),"Unavailable: %s",shown_fg_error.c_str());
    else if(!shown_fg_swapchain)ImGui::TextDisabled("Waiting for the game's swap chain (restart if the game was already running).");
    else if(fg_enabled) {
        if(shown_fg_ratio>1.5)ImGui::Text("Active: %.0f FPS rendered -> %.0f FPS shown",shown_fps,shown_fps*shown_fg_ratio);
        else ImGui::TextDisabled("Not interpolating yet (status %u). Needs DLSS active and the game window focused.",shown_fg_status);
    }
    ImGui::TextDisabled("Requires DLSS SR active. UI may smear slightly on generated frames.");
    ImGui::Spacing();ImGui::SeparatorText("DLSS model");
    const char *presets[]={"Default (NVIDIA)","J","K - transformer, best quality","L - ultra performance","M - performance"};
    int preset=dlss_preset>=10 && dlss_preset<=13?int(dlss_preset-9):0;
    if(ImGui::Combo("Preset",&preset,presets,int(std::size(presets)))) {
        dlss_preset=preset?unsigned(preset+9):0;changed=true;
        for(auto &[_,dev]:devices)if(dev.bridge){dev.bridge->set_preset(dlss_preset);dev.bridge->recreate();}
        log("{\"event\":\"dlss_settings\",\"preset\":"+std::to_string(dlss_preset)+"}");
    }
    ImGui::Spacing();ImGui::SeparatorText("Now");
    if(auto it=devices.find(runtime->get_device());it!=devices.end() && it->second.bridge) {
        const auto st=it->second.bridge->status();
        if(st.frames && st.output_width)ImGui::Text("%u x %u -> %u x %u (%.1f %%)",st.width,st.height,st.output_width,st.output_height,100.0*st.width/st.output_width);
        ImGui::Text("%s",st.failed?"Error - game's own pass in use":st.resizing?"Switching resolution":st.active?(enabled?"DLSS active":"Shadow test"):"Waiting for a supported scene");
    }
    if(shown_fps>0)ImGui::Text("%.1f FPS (%.2f ms)",shown_fps,shown_ms);
    if(changed)save_settings();
}
bool open_overlay(effect_runtime *,bool open,input_source) { overlay_open=open; return false; }
void draw_status(effect_runtime *runtime) {
    std::lock_guard lock(mutex);
    if(runtime->is_key_pressed(VK_F7)) {show_hud=!show_hud;save_settings();}
    if(runtime->is_key_pressed(VK_F6))set_mode(live_mode=="replace"?"off":"replace");
    if(runtime->is_key_pressed(VK_F8))screenshot_pending=true;
    if(!show_hud || overlay_open)return;
    auto it=devices.find(runtime->get_device());if(it==devices.end())return;
    if(baseline) {
        ImGui::SetNextWindowPos(ImVec2(12,12),ImGuiCond_Always);ImGui::SetNextWindowBgAlpha(.8f);
        ImGui::Begin("GhostMaster DLSS status",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoNav|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::TextColored(ImVec4(.6f,.8f,1,1),"BASELINE: addon idle, game native");if(shown_fps>0)ImGui::Text("%.1f FPS (%.2f ms)",shown_fps,shown_ms);ImGui::End();return;
    }
    if(!it->second.bridge) {
        if(startup_error.empty())return;
        ImGui::SetNextWindowPos(ImVec2(12,12),ImGuiCond_Always);ImGui::SetNextWindowBgAlpha(.8f);
        ImGui::Begin("GhostMaster DLSS status",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoNav|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::TextColored(ImVec4(1,.4f,.35f,1),"DLSS SR: disabled - %s",startup_error.c_str());ImGui::End();return;
    }
    auto status=it->second.bridge->status();
    const bool enabled=live_mode=="replace" && status.active;
    const char *label=enabled?"DLSS SR: ON":live_mode=="off"?"DLSS SR: OFF":status.failed?"DLSS SR: ERROR - native fallback":status.resizing?"DLSS SR: switching resolution":status.active?"DLSS SR: SHADOW TEST":"DLSS SR: waiting for supported scene";
    ImGui::SetNextWindowPos(ImVec2(12,12),ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(.8f);
    ImGui::Begin("GhostMaster DLSS status",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoNav|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(enabled?ImVec4(.35f,1,.4f,1):ImVec4(1,.85f,.35f,1),"%s",label);
    if(status.frames)ImGui::Text("%u x %u -> %u x %u | %llu frames",status.width,status.height,status.output_width,status.output_height,status.frames);
    if(shown_fps>0) {
        if(diagnostics)ImGui::Text("%.1f FPS (%.2f ms) | addon CPU %.2f ms/frame",shown_fps,shown_ms,shown_addon_ms);
        else ImGui::Text("%.1f FPS (%.2f ms)",shown_fps,shown_ms);
    }
    if(fg_wanted && fg_enabled && shown_fg_ratio>1.5)ImGui::TextColored(ImVec4(.35f,1,.4f,1),"Frame Generation: %.0f FPS shown",shown_fps*shown_fg_ratio);
    ImGui::TextUnformatted("F6: toggle DLSS   F7: hide status   F8: screenshot");
    ImGui::End();
}
bool live_dispatch(command_list *c,Command &cmd) {
    auto *d=c->get_device();auto &data=devices[d];
    if(live_mode=="off" || !data.bridge || !is_taau(cmd.shader))return false;
    auto skip=[&](std::string reason) {if(data.skips.insert(reason).second)log("{\"event\":\"dlss_input_skip\",\"reason\":"+quote(reason)+"}");return false;};
    auto descriptor=[&](uint32_t param,uint32_t binding)->Descriptor {
        descriptor_table table{};if(!cmd.tables.get(param,table))return {};
        const auto key=slot(d,table,binding);auto *m=mirror(key.first,uint64_t(key.second)+1);
        return m?unpack(m->slots[key.second].load(std::memory_order_relaxed)):Descriptor{};
    };
    auto native_view=[&](resource_view v) {
        // ReShade's update_descriptor_tables values are already original CPU view handles.
        // Only table addresses are encoded; passing a resource_view to the table resolver is invalid.
        return D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(v.handle)};
    };
    auto resource_from=[&](const Descriptor &desc) -> ID3D12Resource * {
        if(!desc.view.handle)return nullptr;
        return reinterpret_cast<ID3D12Resource *>(d->get_resource_from_view(desc.view).handle);
    };
    auto color=descriptor(0,1),depth=descriptor(0,2),velocity=descriptor(0,3),dest=descriptor(2,0);
    TemporalInputs inputs;
    inputs.color=resource_from(color);inputs.depth=resource_from(depth);inputs.velocity=resource_from(velocity);inputs.destination=resource_from(dest);
    if(!inputs.color || !inputs.depth || !inputs.velocity || !inputs.destination)return skip("missing_resource");
    auto read=[&](uint32_t param,void *target,size_t bytes) {
        auto it=cmd.roots.find({param,0});if(it==cmd.roots.end() || it->second.type!=descriptor_type::constant_buffer)return false;
        auto &b=it->second.buffer;if(!b.buffer.handle)return false;
        auto *native=reinterpret_cast<ID3D12Resource *>(b.buffer.handle);const auto bd=native->GetDesc();
        if(bd.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER || b.offset>bd.Width || bytes>bd.Width-b.offset)return false;
        D3D12_HEAP_PROPERTIES hp{};D3D12_HEAP_FLAGS flags{};
        if(FAILED(native->GetHeapProperties(&hp,&flags)) || hp.Type!=D3D12_HEAP_TYPE_UPLOAD)return false;
        void *mapped=nullptr;D3D12_RANGE range{size_t(b.offset),size_t(b.offset+bytes)};
        if(FAILED(native->Map(0,&range,&mapped)) || !mapped)return false;
        std::memcpy(target,static_cast<char *>(mapped)+b.offset,bytes);D3D12_RANGE written{0,0};native->Unmap(0,&written);return true;
    };
    std::array<float,78> local{};std::array<float,1503> view{};
    if(!read(3,local.data(),sizeof(local)) || !read(4,view.data(),sizeof(view)))return skip("constant_read");
    for(auto index:{36,37,44,45,68,69,74,75})if(!std::isfinite(local[index]))return skip("nonfinite_parameters");
    // b0[9] is allocation size, b0[17] is view size. UE pads low-resolution allocations.
    if(local[68]<64 || local[69]<64 || local[44]>8192 || local[45]>8192 || local[44]<local[68] || local[45]<local[69])return skip("view_dimensions");
    if(local[68]!=view[148*4] || local[69]!=view[148*4+1])return skip("view_size_mismatch");
    inputs.width=unsigned(local[68]);inputs.height=unsigned(local[69]);inputs.output_width=unsigned(local[44]);inputs.output_height=unsigned(local[45]);
    inputs.jitter_x=local[74];inputs.jitter_y=local[75];inputs.pre_exposure=view[155*4+2];
    if(!std::isfinite(inputs.pre_exposure) || inputs.pre_exposure<=0 || std::abs(inputs.jitter_x)>2 || std::abs(inputs.jitter_y)>2)return false;
    if(std::abs(inputs.jitter_x-view[144*4]*inputs.width*.5f)>.001f || std::abs(inputs.jitter_y+view[144*4+1]*inputs.height*.5f)>.001f)return skip("jitter_mismatch");
    if(view[147*4]!=0 || view[147*4+1]!=0)return false; // Nonzero view rectangles not yet supported.
    const auto cd=inputs.color->GetDesc(),dd=inputs.depth->GetDesc(),vd=inputs.velocity->GetDesc(),od=inputs.destination->GetDesc();
    auto fits=[&](const D3D12_RESOURCE_DESC &r) {return r.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D && r.DepthOrArraySize==1 && r.SampleDesc.Count==1 && r.Width>=inputs.width && r.Width<inputs.width+16 && r.Height>=inputs.height && r.Height<inputs.height+16;};
    // UE binds a 1x1 dummy when no velocity was rendered (menus, static scenes, first frames after a resize).
    inputs.camera_only=vd.Width==1 && vd.Height==1 && vd.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    if(!fits(cd) || !fits(dd) || !(inputs.camera_only || (fits(vd) && vd.Format==DXGI_FORMAT_R16G16B16A16_UNORM)) ||
       od.Width!=inputs.output_width || od.Height!=inputs.output_height || od.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT || od.DepthOrArraySize!=1)return skip("texture_shape_format:color="+std::to_string(cd.Width)+"x"+std::to_string(cd.Height)+",depth="+std::to_string(dd.Width)+"x"+std::to_string(dd.Height)+",velocity="+std::to_string(vd.Width)+"x"+std::to_string(vd.Height)+"/"+std::to_string(vd.Format)+",output="+std::to_string(od.Width)+"x"+std::to_string(od.Height)+"/"+std::to_string(od.Format)+"/mips="+std::to_string(od.MipLevels));
    std::copy_n(view.data()+136*4,16,inputs.clip_to_previous.begin());
    for(float v:inputs.clip_to_previous)if(!std::isfinite(v))return false;
    inputs.color_view=native_view(color.view);inputs.depth_view=native_view(depth.view);inputs.velocity_view=native_view(velocity.view);
    if(!inputs.color_view.ptr || !inputs.depth_view.ptr || !inputs.velocity_view.ptr)return skip("native_view_resolution");
    auto *native=reinterpret_cast<ID3D12GraphicsCommandList *>(c->get_native());
    auto restore=[&]() {
        std::set<ID3D12DescriptorHeap *> unique;
        cmd.tables.each([&](uint32_t,descriptor_table table) {auto key=slot(d,table,0);if(key.first)unique.insert(reinterpret_cast<ID3D12DescriptorHeap *>(key.first));});
        if(unique.size()>2)return; // The original D3D12 state cannot legitimately have more than two heaps.
        std::vector<ID3D12DescriptorHeap *> heaps(unique.begin(),unique.end());native->SetDescriptorHeaps(UINT(heaps.size()),heaps.data());
        native->SetComputeRootSignature(reinterpret_cast<ID3D12RootSignature *>(cmd.layout));
        native->SetPipelineState(reinterpret_cast<ID3D12PipelineState *>(cmd.native_pipeline));
        cmd.tables.each([&](uint32_t param,descriptor_table table) {native->SetComputeRootDescriptorTable(param,{table.handle});});
        cmd.graphics_tables.each([&](uint32_t param,descriptor_table table) {
            auto key=slot(d,table,0);if(unique.contains(reinterpret_cast<ID3D12DescriptorHeap *>(key.first)))native->SetGraphicsRootDescriptorTable(param,{table.handle});
        });
        for(const auto &[key,desc]:cmd.roots) {
            if(desc.type==descriptor_type::constant_buffer && desc.buffer.buffer.handle)
                native->SetComputeRootConstantBufferView(key.first,reinterpret_cast<ID3D12Resource *>(desc.buffer.buffer.handle)->GetGPUVirtualAddress()+desc.buffer.offset);
            else if(desc.type==descriptor_type::buffer_shader_resource_view)native->SetComputeRootShaderResourceView(key.first,desc.view.handle);
            else if(desc.type==descriptor_type::buffer_unordered_access_view)native->SetComputeRootUnorderedAccessView(key.first,desc.view.handle);
        }
    };
    const bool replaced=data.bridge->process(native,inputs,live_mode=="replace",restore);
    if(replaced && fg_wanted && fg_enabled)
        FrameGeneration::instance().tag(native,data.bridge->prepared_depth(),data.bridge->prepared_motion(),inputs.width,inputs.height,
            camera_from_view(view.data(),inputs.jitter_x,inputs.jitter_y),data.bridge->last_reset());
    return replaced;
}
#endif
bool dispatch(command_list *c,uint32_t x,uint32_t y,uint32_t z) {
    if(!dx12(c->get_device())) return false;
    CpuTimer timer;
    auto &cmd=state(c);
#ifdef GM_LIVE
    if(is_taau(cmd.shader)) {std::lock_guard lock(mutex);if(live_dispatch(c,cmd))return true;}
#endif
    if(!capture.load(std::memory_order_relaxed)) return false;
    std::lock_guard lock(mutex);
    if(lines>=max_lines) return false;
    auto *d=c->get_device(); auto &data=devices[d];
    std::ostringstream s; s << "{\"event\":\"dispatch\",\"frame\":" << frame << ",\"command\":\"" << hex(reinterpret_cast<uint64_t>(c))
        << "\",\"generation\":" << cmd.generation << ",\"shader\":\"" << hex(cmd.shader) << "\",\"groups\":[" << x << ',' << y << ',' << z << "],\"bindings\":[";
    bool comma=false;
    auto append=[&](const Descriptor &v,uint32_t param,uint32_t reg,uint32_t space) { auto str=describe(d,v,param,reg,space); if(!str.empty()) { if(comma) s << ','; s << str; comma=true; } };
    auto layout=data.layouts.find(cmd.layout);
    if(layout!=data.layouts.end()) cmd.tables.each([&](uint32_t param,descriptor_table table) {
        if(param>=layout->second.size()) return;
        for(const auto &range:layout->second[param].ranges) {
            // Unbounded/bindless tables require a separate investigation; explicitly mark truncation.
            auto base=slot(d,table,range.binding);
            for(uint32_t i=0;i<std::min(range.count,128u);++i) {
                auto it=data.descriptors.find({base.first,base.second+i});
                if(it!=data.descriptors.end()) {
                    const auto type=it->second.type;
                    const bool compatible=type==range.type ||
                        (range.type==descriptor_type::shader_resource_view && type==descriptor_type::buffer_shader_resource_view) ||
                        (range.type==descriptor_type::unordered_access_view && type==descriptor_type::buffer_unordered_access_view);
                    if(compatible) append(it->second,param,range.dx_register_index+i,range.dx_register_space);
                }
            }
        }
    });
    for(const auto &[key,v]:cmd.roots) {
        uint32_t reg=key.second,space=0;
        if(layout!=data.layouts.end() && key.first<layout->second.size() && !layout->second[key.first].ranges.empty()) {
            auto &r=layout->second[key.first].ranges[0]; reg=r.dx_register_index+key.second; space=r.dx_register_space;
        }
        append(v,key.first,reg,space);
    }
    s << "],\"descriptor_range_limit\":128}"; log(s.str());
    capture_constants(d,cmd);
    return false;
}
void present(command_queue *queue,swapchain *,const rect *,const rect *,uint32_t,const rect *) {
    (void)queue;
    std::lock_guard lock(mutex); ++frame;
#ifdef GM_LIVE
    if(fg_wanted)FrameGeneration::instance().before_present();
    auto device_it=devices.find(queue->get_device());
    if(device_it!=devices.end() && device_it->second.bridge)device_it->second.bridge->present(reinterpret_cast<ID3D12CommandQueue *>(queue->get_native()));
    LARGE_INTEGER now{};QueryPerformanceCounter(&now);
    if(!perf_frequency.QuadPart){QueryPerformanceFrequency(&perf_frequency);perf_window=now;}
    else {
        perf_max_ms=std::max(perf_max_ms,double(now.QuadPart-perf_last.QuadPart)*1000/perf_frequency.QuadPart);++perf_frames;
        const double seconds=double(now.QuadPart-perf_window.QuadPart)/perf_frequency.QuadPart;
        if(seconds>=2) {
            shown_fps=perf_frames/seconds;shown_ms=seconds*1000/perf_frames;
            shown_addon_ms=double(addon_ticks.exchange(0))*1000/perf_frequency.QuadPart/perf_frames;
            const auto writes=descriptor_writes.exchange(0)/perf_frames;
            std::ostringstream s;s<<std::fixed<<std::setprecision(2)<<"{\"event\":\"perf\",\"frame\":"<<frame<<",\"mode\":"<<quote(live_mode)<<",\"fps\":"<<shown_fps<<",\"avg_ms\":"<<shown_ms<<",\"max_ms\":"<<perf_max_ms<<",\"addon_cpu_ms\":"<<shown_addon_ms<<",\"descriptor_writes\":"<<writes<<",\"baseline\":"<<(baseline?"true":"false");
            if(device_it!=devices.end() && device_it->second.bridge) {
                const auto st=device_it->second.bridge->status();
                s<<",\"dlss_active\":"<<(st.active?"true":"false")<<",\"input\":\""<<st.width<<'x'<<st.height<<"\",\"output\":\""<<st.output_width<<'x'<<st.output_height<<'"';
            }
            if(fg_wanted) {
                const auto st=FrameGeneration::instance().status();
                shown_fg_ratio=st.frames?double(st.presented)/st.frames:0;shown_fg_status=st.dlssg_status;shown_fg_error=st.error;shown_fg_swapchain=st.swapchain;
                s<<",\"fg_enabled\":"<<(fg_enabled?"true":"false")<<",\"fg_swapchain\":"<<(st.swapchain?"true":"false")<<",\"fg_presented_per_frame\":"<<shown_fg_ratio
                 <<",\"fg_status\":"<<st.dlssg_status<<",\"fg_output_fps\":"<<shown_fps*shown_fg_ratio;
                if(!st.error.empty())s<<",\"fg_error\":"<<quote(st.error);
            }
            s<<'}';log(s.str());perf_window=now;perf_frames=0;perf_max_ms=0;
        }
    }
    perf_last=now;
    if(!baseline)enforce_scale();
    if(frame%30==1 && !control_file.empty()) {
        std::ifstream f(control_file);std::string mode,token;f>>mode>>token;if(mode!="shadow" && mode!="replace")mode="off";
        if(!token.empty() && token!=snapshot_token){snapshot_token=token;screenshot_pending=true;}
        if(mode!=live_mode) {live_mode=mode;for(auto &[_,d]:devices)if(d.bridge)d.bridge->reset_history();log("{\"event\":\"mode\",\"value\":"+quote(mode)+"}");}
    }
#endif
    bool down=(GetAsyncKeyState(VK_F8)&0x8000)!=0;
    capture=diagnostics && ((frame%600==120) || (down&&!f8_down)); f8_down=down;
    const bool capturing=capture;
    if(capturing) log("{\"event\":\"capture_window\",\"frame\":"+std::to_string(frame)+"}");
    if(trace) trace.flush();
}
}
#ifdef GM_LIVE
extern "C" __declspec(dllexport) const char *NAME="Ghost Master DLSS";
extern "C" __declspec(dllexport) const char *DESCRIPTION="NVIDIA DLSS Super Resolution and DLSS Frame Generation for Ghost Master: Resurrection (Steam build, UE 5.5.1). Settings in the DLSS SR tab.";
#else
extern "C" __declspec(dllexport) const char *NAME="GhostMaster TSR Probe";
extern "C" __declspec(dllexport) const char *DESCRIPTION="Diagnostic compute shader/resource capture. Does not enable DLSS.";
#endif
BOOL APIENTRY DllMain(HMODULE module,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        std::vector<wchar_t> path(32768),custom(32768);
        GetModuleFileNameW(module,path.data(),32768);
        const auto addon_directory=fs::path(path.data()).parent_path();
        output=addon_directory/"ghostmaster-capture";
#ifdef GM_LIVE
        // Standalone install: settings next to the addon.
        // Environment variables from tools/launch-live.ps1 take precedence for isolated test runs.
        const auto ini=(addon_directory/"ghostmaster_dlss.ini").wstring();
        auto setting=[&](const wchar_t *key,const wchar_t *fallback) {
            GetPrivateProfileStringW(L"DLSS",key,fallback,custom.data(),32768,ini.c_str());return std::wstring(custom.data());
        };
        live_mode=fs::path(setting(L"Mode",L"replace")).string();
        if(live_mode!="shadow" && live_mode!="replace")live_mode="off";
        diagnostics=setting(L"Diagnostics",L"0")==L"1";
        show_hud=setting(L"ShowStatus",L"1")!=L"0";
        if(const auto preset=setting(L"Preset",L"default");preset.size()==1 && towupper(preset[0])>=L'J' && towupper(preset[0])<=L'M')
            dlss_preset=10+unsigned(towupper(preset[0])-L'J');
        scale_override=setting(L"ScaleSource",L"game")==L"addon";
        if(const float percent=std::wcstof(setting(L"ScreenPercentage",L"66.7").c_str(),nullptr);std::isfinite(percent))scale_percent=std::clamp(percent,33.3f,99.0f);
        settings_file=ini;
        fg_wanted=fg_enabled=setting(L"FrameGeneration",L"1")!=L"0";
        // Relative paths are relative to the add-on folder, not the game's working directory.
        auto folder=[&](const wchar_t *key,const fs::path &fallback) {
            const fs::path value=setting(key,L"");
            return value.empty()?fallback:value.is_relative()?addon_directory/value:value;
        };
        streamline_directory=folder(L"StreamlineDir",addon_directory/"streamline");
        // The game does not ship nvngx_dlss.dll: the release carries it in .\streamline. Once Streamline has initialized
        // NGX, NGX only searches that folder anyway, so DLSS SR and DLSS-G use the same signed runtime.
        std::error_code exists_error;
        runtime_directory=folder(L"RuntimeDir",fs::exists(streamline_directory/"nvngx_dlss.dll",exists_error)?streamline_directory:addon_directory);
        output=folder(L"LogDir",addon_directory/"ghostmaster-dlss");
        auto n=GetEnvironmentVariableW(L"GHOSTMASTER_RUNTIME_DIR",custom.data(),32768);if(n>0 && n<32768)runtime_directory=custom.data();
        n=GetEnvironmentVariableW(L"GHOSTMASTER_CONTROL_FILE",custom.data(),32768);if(n>0 && n<32768)control_file=custom.data();
        n=GetEnvironmentVariableW(L"GHOSTMASTER_DIAGNOSTICS",custom.data(),32768);if(n>0 && n<32768)diagnostics=custom[0]==L'1';
        n=GetEnvironmentVariableW(L"GHOSTMASTER_BASELINE",custom.data(),32768);if(n>0 && n<32768)baseline=custom[0]==L'1';
        n=GetEnvironmentVariableW(L"GHOSTMASTER_STREAMLINE_DIR",custom.data(),32768);if(n>0 && n<32768)streamline_directory=custom.data();
        n=GetEnvironmentVariableW(L"GHOSTMASTER_FRAME_GENERATION",custom.data(),32768);if(n>0 && n<32768)fg_wanted=fg_enabled=custom[0]==L'1';
        if(baseline)diagnostics=false,fg_wanted=false;
#endif
        const auto length=GetEnvironmentVariableW(L"GHOSTMASTER_CAPTURE_DIR",custom.data(),32768);
        if(length>0 && length<32768) output=custom.data();
        if(!reshade::register_addon(module)) return FALSE;
#define ON(event, callback) reshade::register_event<reshade::addon_event::event>(callback)
        ON(init_device,on_device); ON(destroy_device,destroy_device);
#ifdef GM_LIVE
        if(baseline) {ON(present,present);ON(reshade_overlay,draw_status);return TRUE;}
#endif
        if(diagnostics) {
            ON(init_resource,init_resource); ON(destroy_resource,destroy_resource);
            ON(init_pipeline_layout,init_layout); ON(destroy_pipeline_layout,destroy_layout);
        }
        ON(init_pipeline,init_pipeline); ON(destroy_pipeline,destroy_pipeline);
        ON(update_descriptor_tables,update_tables); ON(copy_descriptor_tables,copy_tables);
        ON(bind_pipeline,bind_pipeline); ON(bind_descriptor_tables,bind_tables); ON(push_descriptors,push);
        ON(reset_command_list,reset); ON(destroy_command_list,destroy_command);
        ON(dispatch,dispatch); ON(present,present);
#ifdef GM_LIVE
        ON(reshade_overlay,draw_status); ON(reshade_present,runtime_present);
        ON(reshade_open_overlay,open_overlay);
        // Own tab in the overlay, plus the same panel under the addon's entry in the Add-ons tab.
        reshade::register_overlay("DLSS SR",draw_settings);
        reshade::register_overlay(nullptr,draw_settings);
        ON(destroy_command_queue,destroy_queue);
        ON(init_command_queue,init_queue); ON(finish_present,finish_present);
#endif
#undef ON
    } else if(reason==DLL_PROCESS_DETACH) reshade::unregister_addon(module);
    return TRUE;
}
