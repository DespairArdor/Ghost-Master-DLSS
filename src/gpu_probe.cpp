#include <Windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <iostream>
using Microsoft::WRL::ComPtr;
int main() {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { std::cerr << "CreateDXGIFactory1 failed: " << std::hex << hr << '\n'; return 1; }
    bool supported = false;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        hr = factory->EnumAdapters1(i, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) return 2;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        ComPtr<ID3D12Device> device;
        hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        std::wcout << desc.Description << L" vendor=0x" << std::hex << desc.VendorId
                   << L" device=0x" << desc.DeviceId << L" D3D12_HRESULT=0x" << hr << L'\n';
        supported |= SUCCEEDED(hr) && desc.VendorId == 0x10de;
    }
    std::cout << "NVIDIA D3D12 adapter: " << (supported ? "yes" : "no")
              << "; DLSS capability still requires NGX query\n";
    return supported ? 0 : 3;
}
