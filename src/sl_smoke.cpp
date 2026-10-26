// Synthetic DLSS Frame Generation test through FrameGeneration, the module the game addon uses:
// native device, queue and factory; the module's v-table redirect turns a plain CreateSwapChainForHwnd into a
// Streamline proxy swap chain. A stripe pattern scrolls 8 px per frame with matching motion vectors; passes when
// DLSS-G reports two presented frames per rendered frame with status eOk.
#include "frame_generation.hpp"
#include "dlss_backend.hpp"
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace {
void check(HRESULT hr,const char *where) { if(FAILED(hr)) { std::cerr << where << " HRESULT 0x" << std::hex << hr << std::dec << '\n'; throw std::runtime_error(where); } }
constexpr char shader[]=R"(
RWTexture2D<float4> Color : register(u0);
RWTexture2D<float> Depth : register(u1);
RWTexture2D<float2> Motion : register(u2);
cbuffer Frame : register(b0) { uint FrameIndex; uint Speed; uint2 Size; }
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=Size)) return;
    // Content moves +Speed px per frame; DLSS motion vectors point to the previous position.
    int x=int(id.x)-int(FrameIndex*Speed);
    float stripe=((x>>5)&1)?1.0:0.15;
    float band=(id.y/90)&1?0.8:1.0;
    Color[id.xy]=float4(stripe*band,stripe*0.6,0.3+0.5*(1-stripe),1);
    Depth[id.xy]=0.5;
    Motion[id.xy]=float2(-float(Speed),0);
})";
LRESULT CALLBACK window_proc(HWND w,UINT m,WPARAM wp,LPARAM lp) { if(m==WM_DESTROY){PostQuitMessage(0);return 0;} return DefWindowProcW(w,m,wp,lp); }
}
int wmain(int argc,wchar_t **argv) {
    if(argc!=3 && argc!=4) {std::cerr << "Usage: sl_smoke <streamline-bin-x64> <output-directory> [dlss-runtime-directory]\n";return 2;}
    try {
        const fs::path bin=fs::absolute(argv[1]),out=fs::absolute(argv[2]);fs::create_directories(out);
        auto &fg=FrameGeneration::instance();
        if(!fg.initialize(bin,out))
            throw std::runtime_error("FrameGeneration::initialize: "+fg.status().error);

        ComPtr<IDXGIFactory6> factory;check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"CreateDXGIFactory2");
        ComPtr<IDXGIAdapter1> adapter;check(factory->EnumAdapterByGpuPreference(0,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter)),"Adapter");
        ComPtr<ID3D12Device> device;check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)),"D3D12CreateDevice");
        D3D12_COMMAND_QUEUE_DESC qd{};ComPtr<ID3D12CommandQueue> queue;check(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)),"CreateCommandQueue");
        FrameGeneration::remember_queue(queue.Get());

        constexpr UINT width=1280,height=720,buffers=3;
        WNDCLASSW wc{};wc.lpfnWndProc=window_proc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"GhostMasterSlSmoke";RegisterClassW(&wc);
        RECT rc{0,0,LONG(width),LONG(height)};AdjustWindowRect(&rc,WS_OVERLAPPEDWINDOW,FALSE);
        HWND hwnd=CreateWindowW(wc.lpszClassName,L"DLSS Frame Generation test",WS_OVERLAPPEDWINDOW|WS_VISIBLE,100,100,rc.right-rc.left,rc.bottom-rc.top,nullptr,nullptr,wc.hInstance,nullptr);
        // DLSS-G only interpolates for the focused window ("DLSS-G disabled: window not focused" in sl.log).
        if(HWND current=GetForegroundWindow();current!=hwnd) {
            const DWORD self=GetCurrentThreadId(),other=GetWindowThreadProcessId(current,nullptr);
            AttachThreadInput(self,other,TRUE);BringWindowToTop(hwnd);SetForegroundWindow(hwnd);SetFocus(hwnd);AttachThreadInput(self,other,FALSE);
        }
        // The game's back-buffer format.
        DXGI_SWAP_CHAIN_DESC1 sd{};sd.Width=width;sd.Height=height;sd.Format=DXGI_FORMAT_R10G10B10A2_UNORM;sd.SampleDesc.Count=1;
        sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.BufferCount=buffers;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;sd.Flags=DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        // Plain DXGI call, as the game (and ReShade underneath it) makes it.
        ComPtr<IDXGISwapChain1> swap1;check(factory->CreateSwapChainForHwnd(queue.Get(),hwnd,&sd,nullptr,nullptr,&swap1),"CreateSwapChainForHwnd");
        if(!fg.status().swapchain) throw std::runtime_error("swap chain was not routed through Streamline: "+fg.status().error);
        ComPtr<IDXGISwapChain4> swap;check(swap1.As(&swap),"IDXGISwapChain4");
        ComPtr<ID3D12Resource> backbuffers[buffers];
        for(UINT i=0;i<buffers;++i)check(swap->GetBuffer(i,IID_PPV_ARGS(&backbuffers[i])),"GetBuffer");
        fg.set_enabled(true);
        // Optional: initialize our own NGX DLSS SR after Streamline owns NGX, as the game addon does.
        // Kept alive for the whole run like the addon's bridge: NGX shutdown for the device would also stop DLSS-G.
        DlssBackend dlss;
        if(argc==4) {
            const auto result=dlss.initialize(device.Get(),fs::absolute(argv[3]),out/"ngx");
            std::ofstream(out/"dlss_after_sl.json") << "{\"dlss_init_result\":\"0x" << std::hex << unsigned(result) << "\"}\n";
            std::cout << "DLSS after SL: 0x" << std::hex << unsigned(result) << std::dec << std::endl;
        }

        auto texture=[&](DXGI_FORMAT format) {
            D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=width;td.Height=height;td.DepthOrArraySize=1;td.MipLevels=1;
            td.Format=format;td.SampleDesc.Count=1;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            ComPtr<ID3D12Resource> r;check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&r)),"Texture");return r;
        };
        auto color=texture(DXGI_FORMAT_R10G10B10A2_UNORM),depth=texture(DXGI_FORMAT_R32_FLOAT),motion=texture(DXGI_FORMAT_R16G16_FLOAT);
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=3;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> heap;check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"Heap");
        const auto inc=device->GetDescriptorHandleIncrementSize(hd.Type);
        ID3D12Resource *targets[]={color.Get(),depth.Get(),motion.Get()};
        for(UINT i=0;i<3;++i) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};ud.Format=targets[i]->GetDesc().Format;ud.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(targets[i],nullptr,&ud,{heap->GetCPUDescriptorHandleForHeapStart().ptr+i*inc});
        }
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,3,0,0,0};
        D3D12_ROOT_PARAMETER params[2]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[0].DescriptorTable={1,&range};
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[1].Constants={0,0,4};
        D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=2;rd.pParameters=params;
        ComPtr<ID3DBlob> blob,error;check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"SerializeRootSignature");
        ComPtr<ID3D12RootSignature> root;check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"RootSignature");
        check(D3DCompile(shader,sizeof(shader),"sl_smoke",nullptr,nullptr,"main","cs_5_0",0,0,&blob,&error),"Compile");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={blob->GetBufferPointer(),blob->GetBufferSize()};
        ComPtr<ID3D12PipelineState> pipeline;check(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)),"Pipeline");

        ComPtr<ID3D12CommandAllocator> allocators[buffers];
        for(auto &a:allocators)check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a)),"Allocator");
        ComPtr<ID3D12GraphicsCommandList> list;check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocators[0].Get(),nullptr,IID_PPV_ARGS(&list)),"List");list->Close();
        ComPtr<ID3D12Fence> fence;check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"Fence");
        UINT64 fence_values[buffers]{},fence_value=0;HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        auto wait=[&](UINT64 v){if(v && fence->GetCompletedValue()<v){fence->SetEventOnCompletion(v,event);WaitForSingleObject(event,5000);}};
        auto barrier=[&](ID3D12Resource *r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){D3D12_RESOURCE_BARRIER x{};x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;x.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};list->ResourceBarrier(1,&x);};

        FrameGeneration::Camera camera;
        for(auto *m:{&camera.view_to_clip,&camera.clip_to_view,&camera.clip_to_prev_clip,&camera.prev_clip_to_clip})*m={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        camera.up={0,1,0};camera.right={1,0,0};camera.forward={0,0,1};camera.aspect=float(width)/height;

        constexpr uint32_t frames=600,speed=8;
        uint64_t rendered=0,presented=0;uint32_t last_status=0,max_generate=0;std::string last_error;
        LARGE_INTEGER frequency,start,end;QueryPerformanceFrequency(&frequency);
        std::ofstream report_progress(out/"progress.txt");
        for(uint32_t frame=0;frame<frames;++frame) {
            MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
            if(frame==120) {QueryPerformanceCounter(&start);fg.status();}
            const UINT index=swap->GetCurrentBackBufferIndex();
            wait(fence_values[index]);
            check(allocators[index]->Reset(),"AllocatorReset");check(list->Reset(allocators[index].Get(),nullptr),"ListReset");
            if(frame) for(auto *r:{depth.Get(),motion.Get()})barrier(r,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ID3D12DescriptorHeap *heaps[]={heap.Get()};list->SetDescriptorHeaps(1,heaps);
            list->SetComputeRootSignature(root.Get());list->SetPipelineState(pipeline.Get());
            list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
            const uint32_t constants[]={frame,speed,width,height};list->SetComputeRoot32BitConstants(1,4,constants,0);
            list->Dispatch((width+7)/8,(height+7)/8,1);
            barrier(color.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
            for(auto *r:{depth.Get(),motion.Get()})barrier(r,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            barrier(backbuffers[index].Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyResource(backbuffers[index].Get(),color.Get());
            barrier(backbuffers[index].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PRESENT);
            barrier(color.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            fg.tag(list.Get(),depth.Get(),motion.Get(),width,height,camera,frame==0);
            check(list->Close(),"Close");ID3D12CommandList *lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);
            fg.before_present();
            const HRESULT present=swap->Present(0,DXGI_PRESENT_ALLOW_TEARING);
            fg.after_present();
            check(present,"Present");
            check(queue->Signal(fence.Get(),++fence_value),"Signal");fence_values[index]=fence_value;
            if(frame>=120 && frame%60==0) {
                const auto st=fg.status();rendered+=st.frames;presented+=st.presented;last_status=st.dlssg_status;max_generate=st.max_generated;last_error=st.error;
                report_progress << "frame " << frame << " status " << st.dlssg_status << " rendered " << st.frames << " presented " << st.presented << " error '" << st.error << "'" << std::endl;
            }
        }
        QueryPerformanceCounter(&end);
        const auto st=fg.status();rendered+=st.frames;presented+=st.presented;
        const double seconds=double(end.QuadPart-start.QuadPart)/frequency.QuadPart;
        const double ratio=rendered?double(presented)/rendered:0;
        const bool pass=last_status==0 && ratio>1.8 && last_error.empty() && st.error.empty();
        fg.set_enabled(false);fg.before_present();
        check(queue->Signal(fence.Get(),++fence_value),"Signal");wait(fence_value);
        std::ofstream report(out/"result.json");
        report << "{\"test\":\"fg_module_vtable_redirect\",\"rendered_frames\":" << rendered << ",\"presented_frames\":" << presented
               << ",\"presented_per_rendered\":" << ratio << ",\"rendered_fps\":" << rendered/seconds << ",\"status\":" << last_status
               << ",\"max_frames_to_generate\":" << max_generate << ",\"window_focused_at_end\":" << (GetForegroundWindow()==hwnd?"true":"false")
               << ",\"passed\":" << (pass?"true":"false") << "}\n";
        std::cout << "presented/rendered=" << ratio << " status=" << last_status << " passed=" << pass << std::endl;
        for(auto &b:backbuffers)b.Reset();swap.Reset();swap1.Reset();
        fg.shutdown();
        if(argc==4)dlss.shutdown();
        DestroyWindow(hwnd);
        return pass?0:1;
    } catch(const std::exception &e) {std::cerr << "FAILED: " << e.what() << '\n';return 1;}
}
