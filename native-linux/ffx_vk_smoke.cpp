#include <dlfcn.h>
#include <cstring>
#include <cstdio>
#include "ffx_api/ffx_api.h"
#include "ffx_api/ffx_upscale.h"

int main(int argc,char **argv) {
    if(argc!=2)return 2;
    void *module=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    if(!module) { fprintf(stderr,"dlopen: %s\n",dlerror());return 3; }
    using Query=ffxReturnCode_t(*)(ffxContext*,ffxQueryDescHeader*);
    auto q=(Query)dlsym(module,"ffxQuery");
    const char *symbols[]={"ffxCreateContext","ffxConfigure","ffxDestroyContext","ffxDispatch"};
    if(!q)return 4;
    for(auto s:symbols)if(!dlsym(module,s)){fprintf(stderr,"missing %s\n",s);return 5;}
    uint64_t count=0;
    ffxQueryDescGetVersions v={};
    v.header.type=FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    v.createDescType=FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    v.outputCount=&count;
    if(q(nullptr,&v.header)!=FFX_API_RETURN_OK||count<1)return 6;
    uint64_t ids[8]={};const char *names[8]={};
    count=8;v.versionIds=ids;v.versionNames=names;
    if(q(nullptr,&v.header)!=FFX_API_RETURN_OK||count<1)return 7;
    printf("DittoSR native Vulkan FFX provider: %s (id 0x%llx)\n",
            names[0]?names[0]:"(unknown)",(unsigned long long)ids[0]);
    if(!names[0]||strstr(names[0],"DittoSR")==nullptr)return 8;
    float ratio=0;
    ffxQueryDescUpscaleGetUpscaleRatioFromQualityMode r={};
    r.header.type=FFX_API_QUERY_DESC_TYPE_UPSCALE_GETUPSCALERATIOFROMQUALITYMODE;
    r.qualityMode=FFX_UPSCALE_QUALITY_MODE_QUALITY;
    r.pOutUpscaleRatio=&ratio;
    if(q(nullptr,&r.header)!=FFX_API_RETURN_OK || ratio<1.49f || ratio>1.51f)return 9;
    printf("FFX quality query: %.2fx, all required exports found\n",ratio);
    dlclose(module);
    return 0;
}
