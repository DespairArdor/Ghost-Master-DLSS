#include "dlss_backend.hpp"
#ifdef GM_BRIDGE_SMOKE
#include "live_bridge.hpp"
#endif
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
void check(HRESULT hr,const char *where) { if(FAILED(hr)) { std::cerr << where << " HRESULT 0x" << std::hex << hr << '\n'; throw std::runtime_error(where); } }
void ngx(NVSDK_NGX_Result r,const char *where) { std::cout << where << " NGX 0x" << std::hex << unsigned(r) << std::dec << std::endl; if(NVSDK_NGX_FAILED(r)) throw std::runtime_error(where); }
float half(uint16_t v) {
    unsigned exp=(v>>10)&31, mant=v&1023;
    float f=exp==31 ? (mant?NAN:INFINITY) : exp ? std::ldexp(float(1024+mant),int(exp)-25) : std::ldexp(float(mant),-24);
    return v&0x8000?-f:f;
}
struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 value=0;
    void submit() {
        check(list->Close(),"Close"); ID3D12CommandList *lists[]={list.Get()}; queue->ExecuteCommandLists(1,lists);
        check(queue->Signal(fence.Get(),++value),"Signal");
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!event) throw std::runtime_error("CreateEvent");
        HRESULT result=fence->SetEventOnCompletion(value,event);
        DWORD wait=SUCCEEDED(result)?WaitForSingleObject(event,30000):WAIT_FAILED; CloseHandle(event);
        check(result,"SetEventOnCompletion"); if(wait!=WAIT_OBJECT_0) throw std::runtime_error("GPU wait failed/timed out");
        check(device->GetDeviceRemovedReason(),"DeviceRemoved");
        check(allocator->Reset(),"AllocatorReset"); check(list->Reset(allocator.Get(),nullptr),"ListReset");
    }
    ComPtr<ID3D12Resource> buffer(UINT64 bytes,D3D12_HEAP_TYPE heap) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type=heap;
        D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width=bytes;
        desc.Height=1; desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;
        check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,heap==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r)),"Buffer");return r;
    }
    ComPtr<ID3D12Resource> texture(UINT w,UINT h,DXGI_FORMAT format,bool output=false) {
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=w;desc.Height=h;
        desc.DepthOrArraySize=1;desc.MipLevels=1;desc.Format=format;desc.SampleDesc.Count=1;
        desc.Flags=output?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE;
        ComPtr<ID3D12Resource> r;
        check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,output?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r)),"Texture"); return r;
    }
    void barrier(ID3D12Resource *r,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};list->ResourceBarrier(1,&b);
    }
    void fill(ID3D12Resource *r,const void *pixel,size_t pixel_size) {
        auto desc=r->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 total=0;
        device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&total);auto upload=buffer(total,D3D12_HEAP_TYPE_UPLOAD);
        void *mapped=nullptr;D3D12_RANGE read{0,0};check(upload->Map(0,&read,&mapped),"UploadMap");
        std::memset(mapped,0,size_t(total));
        for(UINT y=0;y<desc.Height;++y) for(UINT x=0;x<desc.Width;++x)
            std::memcpy(static_cast<char *>(mapped)+footprint.Offset+y*footprint.Footprint.RowPitch+x*pixel_size,pixel,pixel_size);
        upload->Unmap(0,nullptr);
        D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=footprint;
        D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=r;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);barrier(r,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);submit();
    }
};
int wmain(int argc,wchar_t **argv) {
    if(argc!=3) {std::cerr << "Usage: ngx_smoke <verified-runtime-directory> <output-directory>\n";return 2;}
    try {
        std::filesystem::path out=std::filesystem::absolute(argv[2]);std::filesystem::create_directories(out);
        Gpu g;ComPtr<IDXGIFactory1> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI");
        for(UINT i=0;;++i) {ComPtr<IDXGIAdapter1>a;if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 d{};a->GetDesc1(&d);
            if(d.VendorId==0x10de && SUCCEEDED(D3D12CreateDevice(a.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&g.device))))break;}
        if(!g.device) throw std::runtime_error("No NVIDIA D3D12 device");
        D3D12_COMMAND_QUEUE_DESC q{};check(g.device->CreateCommandQueue(&q,IID_PPV_ARGS(&g.queue)),"Queue");
        check(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&g.allocator)),"Allocator");
        check(g.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,g.allocator.Get(),nullptr,IID_PPV_ARGS(&g.list)),"List");
        check(g.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&g.fence)),"Fence");
#ifdef GM_BRIDGE_SMOKE
        // Second phase changes input and output resolution on the same bridge, as a game resolution change does.
        constexpr UINT sizes[2][4]={{640,360,1280,720},{960,540,1920,1080}};
        UINT w=0,h=0,ow=0,oh=0;
        std::ofstream trace(out/"bridge.jsonl");
        LiveBridge bridge(g.device.Get(),argv[1],out,[&](const std::string&s){trace<<s<<std::endl;std::cout<<s<<std::endl;});
        int restored=0,evaluations=0,expected_restores=0;
        ComPtr<ID3D12Resource> color,depth,motion,output;ComPtr<ID3D12DescriptorHeap> heap;
        for(int phase=0;phase<2;++phase) {
        w=sizes[phase][0];h=sizes[phase][1];ow=sizes[phase][2];oh=sizes[phase][3];
        // Phase 1 also exercises the 1x1 dummy velocity path.
        color=g.texture(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT);depth=g.texture(w,h,DXGI_FORMAT_R32_FLOAT);output=g.texture(ow,oh,DXGI_FORMAT_R16G16B16A16_FLOAT,true);
        motion=phase?g.texture(1,1,DXGI_FORMAT_B8G8R8A8_UNORM):g.texture(w,h,DXGI_FORMAT_R16G16B16A16_UNORM);
        uint16_t rgb[]={0x3400,0x3800,0x3c00,0x3c00},mv[]={32767,32767,0,0};uint8_t black[4]{};float z=0.5f;
        g.fill(color.Get(),rgb,sizeof(rgb));g.fill(depth.Get(),&z,sizeof(z));
        if(phase)g.fill(motion.Get(),black,sizeof(black));else g.fill(motion.Get(),mv,sizeof(mv));
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.NumDescriptors=3;
        check(g.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"InputViews");
        auto base=heap->GetCPUDescriptorHandleForHeapStart();auto increment=g.device->GetDescriptorHandleIncrementSize(hd.Type);
        TemporalInputs in;in.color=color.Get();in.depth=depth.Get();in.velocity=motion.Get();in.destination=output.Get();
        in.width=w;in.height=h;in.output_width=ow;in.output_height=oh;in.camera_only=phase==1;
        in.clip_to_previous={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        in.color_view=base;in.depth_view={base.ptr+increment};in.velocity_view={base.ptr+2*increment};
        g.device->CreateShaderResourceView(color.Get(),nullptr,in.color_view);g.device->CreateShaderResourceView(depth.Get(),nullptr,in.depth_view);g.device->CreateShaderResourceView(motion.Get(),nullptr,in.velocity_view);
        int replaced=0,transition=0;
        for(int i=0;replaced<61;++i) {
            if(i>70)throw std::runtime_error("Bridge did not resume after resolution change");
            bridge.present(g.queue.Get());const bool replace=phase || i>=4;
            const bool result=bridge.process(g.list.Get(),in,replace,[&]{++restored;});
            if(phase && !replaced && !result) {++transition;g.submit();continue;} // Native pass while the old feature retires.
            if(result!=replace)throw std::runtime_error("Bridge failed replacement");
            ++expected_restores;++evaluations;replaced+=replace;g.submit();
        }
        if(phase && (!transition || transition>4))throw std::runtime_error("Unexpected resolution transition length "+std::to_string(transition));
        if(phase && bridge.status().output_width!=ow)throw std::runtime_error("Bridge kept old resolution");
        }
        bridge.present(g.queue.Get());g.submit();bridge.present(g.queue.Get());
        if(restored!=expected_restores)throw std::runtime_error("Missing command-state restoration callback");
#else
        constexpr UINT w=640,h=360,ow=1280,oh=720;
        DlssBackend backend;ngx(backend.initialize(g.device.Get(),argv[1],out/"ngx"),"Initialize");
        auto color=g.texture(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT),depth=g.texture(w,h,DXGI_FORMAT_R32_FLOAT),motion=g.texture(w,h,DXGI_FORMAT_R16G16_FLOAT),output=g.texture(ow,oh,DXGI_FORMAT_R16G16B16A16_FLOAT,true);
        uint16_t rgb[]={0x3400,0x3800,0x3c00,0x3c00},mv[]={0,0};float z=0.5f;
        g.fill(color.Get(),rgb,sizeof(rgb));g.fill(depth.Get(),&z,sizeof(z));g.fill(motion.Get(),mv,sizeof(mv));
        ngx(backend.create(g.list.Get(),w,h,ow,oh),"Create 640x360 -> 1280x720");g.submit();
        for(int i=0;i<8;++i) {ngx(backend.evaluate(g.list.Get(),color.Get(),depth.Get(),motion.Get(),output.Get(),0,0,1,i==0),"Evaluate");g.submit();}
#endif
        auto desc=output->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 total=0;
        g.device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&total);auto readback=g.buffer(total,D3D12_HEAP_TYPE_READBACK);
        g.barrier(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=output.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;
        g.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);g.submit();
        void *mapped=nullptr;D3D12_RANGE range{0,size_t(total)};check(readback->Map(0,&range,&mapped),"ReadbackMap");
        std::vector<char> pixels(static_cast<size_t>(total),0);
        std::memcpy(pixels.data(),mapped,size_t(total));
        D3D12_RANGE written{0,0};readback->Unmap(0,&written);
        size_t finite=0,nonzero=0;double error=0;const float expected[]={0.25f,0.5f,1.0f};
        for(UINT y=0;y<oh;++y) {auto row=reinterpret_cast<const uint16_t *>(pixels.data()+footprint.Offset+y*footprint.Footprint.RowPitch);
            for(UINT x=0;x<ow;++x) for(UINT c=0;c<3;++c) {float v=half(row[x*4+c]);finite+=std::isfinite(v);nonzero+=std::isfinite(v)&&v>0.001f;error+=std::abs(v-expected[c]);}}
        double mae=error/(ow*oh*3.0);bool pass=finite==size_t(ow)*oh*3 && nonzero==finite && mae<0.1;
#ifdef GM_BRIDGE_SMOKE
        constexpr auto test_name="bridge_uniform_frame_with_resize";
#else
        constexpr auto test_name="synthetic_uniform_frame";constexpr int evaluations=8;
#endif
        std::ofstream report(out/"result.json");report << "{\"test\":\""<<test_name<<"\",\"evaluations\":"<<evaluations<<",\"width\":"<<ow<<",\"height\":"<<oh<<",\"finite_channels\":" << finite << ",\"nonzero_channels\":" << nonzero << ",\"mean_absolute_error\":" << mae << ",\"passed\":" << (pass?"true":"false") << ",\"game_integration_verified\":false}\n";
        report.flush();
        std::cout << "Finite=" << finite << " nonzero=" << nonzero << " MAE=" << mae << " passed=" << pass << std::endl;
        std::cout << "Readback analyzed; shutting down NGX" << std::endl;
#ifndef GM_BRIDGE_SMOKE
        backend.shutdown();
#endif
        return pass?0:1;
    } catch(const std::exception &e) {std::cerr << "FAILED: " << e.what() << '\n';return 1;}
}
