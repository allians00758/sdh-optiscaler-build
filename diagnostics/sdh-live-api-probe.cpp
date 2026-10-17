// SDH Live Control API diagnostic. Does not modify renderer settings.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

static HMODULE s_module = nullptr;
static wchar_t s_path[32768] = {};
static const void* s_api = nullptr;
static void* s_plugin = nullptr;

typedef uint32_t (__cdecl *SnapshotFn)(void*, char*, uint32_t);
struct ApiProbe {
    uint32_t size;
    uint32_t version;
    void* v1[30];
    void* QueueSdhControl;
    void* PollSdhControl;
    SnapshotFn SnapshotSdhControl;
};

static void append(const char* msg) {
    if (!s_path[0]) {
        wchar_t full[32768] = {};
        auto n = GetModuleFileNameW(s_module, full, 32767);
        if (!n || n >= 32730) return;
        while (n && full[n-1] != L'\\' && full[n-1] != L'/') --n;
        if (!n) return;
        const wchar_t name[] = L"sdh-live-api-probe.log";
        for (unsigned j=0; j<sizeof(name)/sizeof(wchar_t); ++j) full[n+j] = name[j];
        lstrcpyW(s_path, full);
    }
    HANDLE f=CreateFileW(s_path, FILE_APPEND_DATA, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                         nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f==INVALID_HANDLE_VALUE) return;
    DWORD wr=0;
    WriteFile(f,msg,(DWORD)lstrlenA(msg),&wr,nullptr);
    CloseHandle(f);
}

static DWORD WINAPI snapshot_thread(void*) {
    Sleep(2000);
    auto api = reinterpret_cast<const ApiProbe*>(s_api);
    if (!api || !s_plugin || api->size < sizeof(ApiProbe) || !api->SnapshotSdhControl) return 0;
    for(int i=0; i<3; ++i) {
        char buf[1024] = {};
        auto n=api->SnapshotSdhControl(s_plugin,buf,sizeof(buf)-1);
        char line[128];
        sprintf_s(line, "snapshot #%d length=%u\n", i+1, n);
        append(line);
        if (n && n < sizeof(buf)) {
            buf[n]=0;
            append(buf);
            append("\n");
        }
        Sleep(1500);
    }
    return 0;
}

extern "C" __declspec(dllexport) void __cdecl InitializeASI(void) {
    append("InitializeASI called\n");
}

extern "C" __declspec(dllexport) bool __cdecl OptiPluginInit(const ApiProbe* api, void* plugin) {
    append("OptiPluginInit called\n");
    if (!api) { append("API pointer NULL\n"); return false; }
    char line[256];
    sprintf_s(line,"API size=%u version=%u expected_v2_size=%zu plugin=%s\n",
        api->size,api->version,sizeof(ApiProbe), plugin ? "yes":"no");
    append(line);
    if (api->size < sizeof(ApiProbe) || !plugin) {
        append("v2 API unavailable: table too small or plugin null\n");
        return false;
    }
    sprintf_s(line, "v2 callback presence: queue=%d poll=%d snapshot=%d\n",
        api->QueueSdhControl!=nullptr, api->PollSdhControl!=nullptr, api->SnapshotSdhControl!=nullptr);
    append(line);
    if (api->version < 2 || !api->SnapshotSdhControl) {
        append("v2 callbacks unavailable\n");
        return false;
    }
    s_api=api;
    s_plugin=plugin;
    HANDLE th=CreateThread(nullptr,0,snapshot_thread,nullptr,0,nullptr);
    if (th) CloseHandle(th);
    append("OptiPluginInit accepted\n");
    return true;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason==DLL_PROCESS_ATTACH) { s_module=(HMODULE)hinst; DisableThreadLibraryCalls(hinst); }
    return TRUE;
}
