#pragma once
#include <cstdint>
#include <Windows.h>
struct OptiPluginApiV2;
using PFN_SdhInit=bool (__cdecl*)(const OptiPluginApiV2*,void*);
class SDHPluginApi {
public:
    static void Init(HMODULE module,PFN_SdhInit init);
    static void Tick(uint32_t source);
};
