// Local hang diagnostic, restricted to this project's synthetic test process.
#include <Windows.h>
#include <TlHelp32.h>
#include <DbgHelp.h>
#include <filesystem>
#include <iostream>
#include <vector>
int wmain(int argc,wchar_t **argv) {
    if(argc!=2) return 2;
    DWORD pid=wcstoul(argv[1],nullptr,10);
    HANDLE process=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,FALSE,pid);
    if(!process) {std::cerr << "OpenProcess " << GetLastError();return 3;}
    wchar_t path[1024]{}; DWORD length=1024;
    if(!QueryFullProcessImageNameW(process,0,path,&length) || std::filesystem::path(path).filename()!=L"ngx_smoke.exe") {CloseHandle(process);return 4;}
    SymSetOptions(SYMOPT_UNDNAME|SYMOPT_DEFERRED_LOADS|SYMOPT_FAIL_CRITICAL_ERRORS);
    SymInitialize(process,nullptr,TRUE);
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);THREADENTRY32 e{};e.dwSize=sizeof(e);
    if(Thread32First(snapshot,&e)) do {
        if(e.th32OwnerProcessID!=pid)continue;
        HANDLE thread=OpenThread(THREAD_GET_CONTEXT|THREAD_SUSPEND_RESUME|THREAD_QUERY_INFORMATION,FALSE,e.th32ThreadID);
        if(!thread)continue;
        if(SuspendThread(thread)==DWORD(-1)) {CloseHandle(thread);continue;}
        CONTEXT context{};context.ContextFlags=CONTEXT_FULL;
        if(GetThreadContext(thread,&context)) {
            std::cout << "Thread " << std::dec << e.th32ThreadID << '\n';
            STACKFRAME64 frame{};frame.AddrPC={context.Rip,0,AddrModeFlat};frame.AddrStack={context.Rsp,0,AddrModeFlat};frame.AddrFrame={context.Rbp,0,AddrModeFlat};
            for(int i=0;i<24;++i) {
                DWORD64 displacement=0; alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO)+1024]{};
                auto symbol=reinterpret_cast<SYMBOL_INFO *>(storage);symbol->SizeOfStruct=sizeof(SYMBOL_INFO);symbol->MaxNameLen=1024;
                IMAGEHLP_MODULE64 module{};module.SizeOfStruct=sizeof(module);SymGetModuleInfo64(process,frame.AddrPC.Offset,&module);
                std::cout << std::hex << frame.AddrPC.Offset << ' ' << module.ModuleName << "+0x" << (frame.AddrPC.Offset-module.BaseOfImage);
                if(SymFromAddr(process,frame.AddrPC.Offset,&displacement,symbol))std::cout << ' ' << symbol->Name << "+0x" << displacement;
                std::cout << '\n';
                if(!StackWalk64(IMAGE_FILE_MACHINE_AMD64,process,thread,&frame,&context,nullptr,SymFunctionTableAccess64,SymGetModuleBase64,nullptr))break;
            }
        }
        ResumeThread(thread);CloseHandle(thread);
    }while(Thread32Next(snapshot,&e));
    CloseHandle(snapshot);SymCleanup(process);CloseHandle(process);return 0;
}
