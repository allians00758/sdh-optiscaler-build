// SDH live bridge for HelixSR-Scaler 1.1.0, pinned upstream.
#include "sdh/SDHLiveTick.h"
#include "Config.h"
#include "State.h"
#include "helixsr/HelixSR.h"
#include "hooks/Streamline_Hooks.h"
#include "upscalers/IFeature.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace SDHLive {
namespace {
struct Job { std::string command,result; bool done=false; };
std::mutex mutex;
std::condition_variable cv;
std::deque<std::shared_ptr<Job>> jobs;
std::atomic<unsigned long long> ticks{0};
std::atomic<unsigned long long> last_tick{0};
std::atomic<unsigned> source_code{0};
std::string latest;
std::atomic<int> last_fg_saved_ratio{-1};
std::atomic<unsigned long long> latest_fg_apply_tick{0};
std::atomic<unsigned long long> latest_backend_apply_tick{0};
std::string backend_target;
std::string last_apply_stage;
unsigned long long millis(){return GetTickCount64();}
std::map<std::string,std::string> fields(std::string_view command){
    std::map<std::string,std::string> result;
    while(!command.empty()){
        const auto end=command.find('\n');
        auto line=command.substr(0,end);
        if(!line.empty()&&line.back()=='\r')line.remove_suffix(1);
        const auto eq=line.find('=');
        if(eq>0&&eq!=std::string_view::npos&&eq<100&&line.size()<512)
            result[std::string(line.substr(0,eq))]=std::string(line.substr(eq+1));
        if(end==std::string_view::npos)break;
        command.remove_prefix(end+1);
    }
    return result;
}
std::string value(const std::map<std::string,std::string>& v,const char* key){
    auto i=v.find(key);return i==v.end()?std::string():i->second;
}
bool has(const std::map<std::string,std::string>& v,const char* key){
    return value(v,key)=="1";
}
bool boolean(const std::string& input,bool& out){
    if(input=="1"||input=="true"){out=true;return true;}
    if(input=="0"||input=="false"){out=false;return true;}
    return false;
}
bool scalar(const std::string& input,float lo,float hi,float& out){
    try{
        size_t length=0; const auto v=std::stof(input,&length);
        if(length==input.size()&&std::isfinite(v)&&v>=lo&&v<=hi){out=v;return true;}
    }catch(...){}
    return false;
}
std::string apply(std::string_view command){
    const auto v=fields(command);
    constexpr const char* supported[]={
        "has_sharpness","has_contrast","has_motion_sharpness",
        "has_quality","has_scale","has_shader","has_backend","has_upscaler",
        "has_fg_enabled"
    };
    bool requested=false;
    for(auto& [key,val]:v){
        if(key.rfind("has_",0)!=0||(val!="1"&&val!="true"))continue;
        requested=true;
        if(std::find(std::begin(supported),std::end(supported),key)==std::end(supported))
            return "ok=0\nerror=unsupported_live_field:"+key+"\n";
    }
    if(!requested)return "ok=0\nerror=no_supported_changes\n";
    // The provider route cannot change live. The already active NvngxFG
    // route CAN be toggled with the same Streamline ratio override as the hotkey.
    bool requestedFgEnabled=false;
    if(has(v,"has_fg_enabled")&&!boolean(value(v,"fg_enabled"),requestedFgEnabled))
        return "ok=0\nerror=invalid_fg_enabled\n";
    auto* cfg=Config::Instance();
    if(!cfg)return "ok=0\nerror=no_active_config\n";
    auto& s=State::Instance();
    bool enabledSharp=false,enabledContrast=false,enabledMotion=false,enabledQuality=false;
    bool shaderOn=false;
    SharpenShader requestedShader=SharpenShader::RCAS;
    float sharp=0,contrast=0,motion=0,quality=0,scale=0;
    if(has(v,"has_sharpness")&&
       (!boolean(value(v,"sharpness_enabled"),enabledSharp)||
        !scalar(value(v,"sharpness"),0.f,1.f,sharp)))
        return "ok=0\nerror=invalid_sharpness\n";
    if(has(v,"has_contrast")&&
       (!boolean(value(v,"contrast_enabled"),enabledContrast)||
        !scalar(value(v,"contrast"),-2.f,2.f,contrast)))
        return "ok=0\nerror=invalid_contrast\n";
    if(has(v,"has_motion_sharpness")&&
       (!boolean(value(v,"motion_sharpness_enabled"),enabledMotion)||
        !scalar(value(v,"motion_sharpness"),-1.f,1.f,motion)))
        return "ok=0\nerror=invalid_motion_sharpness\n";
    if(has(v,"has_quality")&&
       (!boolean(value(v,"quality_enabled"),enabledQuality)||
        (enabledQuality&&!scalar(value(v,"quality_ratio"),1.f,3.f,quality))))
        return "ok=0\nerror=invalid_quality\n";
    if(has(v,"has_scale")&&!scalar(value(v,"scale"),0.5f,3.f,scale))
        return "ok=0\nerror=invalid_scale\n";
    // This matches the upstream HelixSR-Scaler [Sharpness] menu: switching
    // shader is a render-thread Config mutation, not a library/proxy reload.
    if(has(v,"has_shader")){
        auto method=value(v,"shader");
        std::transform(method.begin(),method.end(),method.begin(),
            [](unsigned char c){return (char)std::tolower(c);});
        if(method=="off"||method=="none"||method=="auto"||method=="default")shaderOn=false;
        else if(method=="rcas"||method=="cas"){shaderOn=true;requestedShader=SharpenShader::RCAS;}
        else if(method=="da"||method=="da_rcas"||method=="depthaware"){shaderOn=true;requestedShader=SharpenShader::DepthAware;}
        else if(method=="lcda"||method=="da_das"||method=="localcontrastdepthaware"){shaderOn=true;requestedShader=SharpenShader::LocalContrastDepthAware;}
        else return "ok=0\nerror=invalid_shader\n";
    }
    const bool changeBackend=has(v,"has_backend")||has(v,"has_upscaler");
    Upscaler newBackend=Upscaler::Reset;
    if(changeBackend){
        std::string code=value(v,"backend");
        if(code.empty())code=value(v,"upscaler");
        std::transform(code.begin(),code.end(),code.begin(),[](unsigned char c){return (char)std::tolower(c);});
        if(code=="helixsr"||code=="ffx"||code=="fsr3.1"||code=="fsr31"){
            if(!HelixSR::FsrDllFound())
                return "ok=0\nerror=helixsr_ffx_not_loaded\n";
            newBackend=s.api==DX11?Upscaler::FFX_on12:Upscaler::FFX;
        }else if(code=="xess")newBackend=Upscaler::XeSS;
        else if(code=="fsr22")newBackend=Upscaler::FSR22;
        else return "ok=0\nerror=unsupported_backend\n";
        if(s.api!=DX11&&s.api!=DX12&&s.api!=Vulkan)
            return "ok=0\nerror=graphics_api_unavailable\n";
        if(!s.currentFeature||s.changeBackend.empty())
            return "ok=0\nerror=active_upscaler_context_unavailable\n";
    }
    if(has(v,"has_fg_enabled")){
        if(s.activeFgInput!=FGInput::NvngxFG ||
           s.activeFgNvngx!=FGNvngxReplacement::HelixSR)
            return "ok=0\nerror=requires_restart_fg_pipeline\n";
        if(!s.dlssgMfgMax.has_value()||s.dlssgMfgMax.value()<1)
            return "ok=0\nerror=dlssg_ratio_override_unavailable\n";
    }
    // Validate all inputs before touching the live renderer configuration.
    if(has(v,"has_fg_enabled")){
        auto& ratio=cfg->FGDLSSGOverrideInterpolationCount;
        if(!requestedFgEnabled){
            if(!ratio.has_value()||ratio.value()!=0){
                last_fg_saved_ratio.store(ratio.has_value()?ratio.value():-1);
                ratio=0;
            }
        }else if(ratio.has_value()&&ratio.value()==0){
            const int saved=last_fg_saved_ratio.load();
            if(saved>0&&saved<=s.dlssgMfgMax.value())ratio=saved;
            else ratio.reset();
        }
        StreamlineHooks::updateDlssgOptions();
        latest_fg_apply_tick.store(ticks.load());
    }
    if(has(v,"has_sharpness")){cfg->OverrideSharpness=enabledSharp;cfg->Sharpness=sharp;}
    if(has(v,"has_shader")){
        cfg->RcasEnabled=shaderOn;
        if(shaderOn)cfg->SharpnessShader=requestedShader;
    }
    if(has(v,"has_contrast")){cfg->ContrastEnabled=enabledContrast;cfg->Contrast=contrast;}
    if(has(v,"has_motion_sharpness")){cfg->MotionSharpnessEnabled=enabledMotion;cfg->MotionSharpness=motion;}
    if(has(v,"has_quality")){
        cfg->UpscaleRatioOverrideEnabled=enabledQuality;
        if(enabledQuality)cfg->UpscaleRatioOverrideValue=quality;
    }
    if(has(v,"has_scale")){
        cfg->OutputScalingEnabled=scale!=1.f;
        cfg->OutputScalingMultiplier=scale;
    }
    if(changeBackend){
        {std::lock_guard<std::mutex> lock(mutex);
            backend_target=value(v,"upscaler");
            if(backend_target.empty())backend_target=value(v,"backend");
        }
        latest_backend_apply_tick.store(ticks.load());
        if(newBackend==Upscaler::FFX||newBackend==Upscaler::FFX_on12)
            cfg->FfxUpscalerIndex=0;
        if(s.api==DX12)cfg->Dx12Upscaler=newBackend;
        else if(s.api==DX11)cfg->Dx11Upscaler=newBackend;
        else cfg->VulkanUpscaler=newBackend;
    }
    const bool recreate=changeBackend||has(v,"has_quality")||has(v,"has_scale");
    if(recreate){
        s.newBackend=changeBackend?newBackend:s.currentFeature->GetUpscalerType();
        for(auto& item:s.changeBackend)item.second=true;
        {std::lock_guard<std::mutex> lock(mutex);last_apply_stage="renderer_reinit_requested";}
        return "ok=1\napply_stage=renderer_reinit_requested\n";
    }
    if(has(v,"has_fg_enabled")){
        {std::lock_guard<std::mutex> lock(mutex);last_apply_stage="dlssg_override_updated";}
        return "ok=1\napply_stage=dlssg_override_updated\n";
    }
    {std::lock_guard<std::mutex> lock(mutex);last_apply_stage="render_config_updated";}
    return "ok=1\napply_stage=render_config_updated\n";
}
std::string collect(){
    auto* cfg=Config::Instance();
    if(!cfg)return "core_tick=0\n";
    auto& s=State::Instance();
    std::ostringstream out;
    out<<"core_tick="<<ticks.load()<<"\n";
    out<<"tick_source_code="<<source_code.load()<<"\n";
    out<<"capabilities=sharpness,contrast,motion_sharpness,quality,scale,shader,backend\n";
    out<<"active_backend=";
    if(HelixSR::IsActive())out<<"helixsr";
    else if(s.currentFeature)out<<UpscalerToCode(s.currentFeature->GetUpscalerType());
    else out<<"unknown";
    out<<"\n";
    out<<"shader_enabled="<<int(cfg->RcasEnabled.value_or_default())<<"\n";
    out<<"shader="<<(cfg->SharpnessShader.value_or_default()==SharpenShader::DepthAware?"da_rcas":
        cfg->SharpnessShader.value_or_default()==SharpenShader::LocalContrastDepthAware?"da_das":"rcas")<<"\n";
    out<<"sharpness_enabled="<<int(cfg->OverrideSharpness.value_or_default())<<"\n";
    out<<"sharpness="<<cfg->Sharpness.value_or_default()<<"\n";
    out<<"contrast_enabled="<<int(cfg->ContrastEnabled.value_or_default())<<"\n";
    out<<"contrast="<<cfg->Contrast.value_or_default()<<"\n";
    out<<"motion_sharpness_enabled="<<int(cfg->MotionSharpnessEnabled.value_or_default())<<"\n";
    out<<"motion_sharpness="<<cfg->MotionSharpness.value_or_default()<<"\n";
    out<<"quality_enabled="<<int(cfg->UpscaleRatioOverrideEnabled.value_or_default())<<"\n";
    out<<"quality_ratio="<<cfg->UpscaleRatioOverrideValue.value_or_default()<<"\n";
    out<<"scale="<<(cfg->OutputScalingEnabled.value_or_default()?cfg->OutputScalingMultiplier.value_or_default():1.f)<<"\n";
    // Distinguish selected pipeline from an actually running FG renderer.
    out<<"fg_input_active="<<int(s.activeFgInput)<<"\n";
    out<<"fg_provider_active="<<int(s.activeFgNvngx)<<"\n";
    out<<"helixsr_fg_pipeline_selected="<<int(
        s.activeFgInput==FGInput::NvngxFG&&s.activeFgNvngx==FGNvngxReplacement::HelixSR)<<"\n";
    out<<"fg_config_enabled="<<int(cfg->IsFGEnabled())<<"\n";
    const auto forcedOff=cfg->FGDLSSGOverrideInterpolationCount.has_value()&&
        cfg->FGDLSSGOverrideInterpolationCount.value()==0;
    out<<"fg_override_forced_off="<<int(forcedOff)<<"\n";
    out<<"fg_override_count="<<(cfg->FGDLSSGOverrideInterpolationCount.has_value()?
        cfg->FGDLSSGOverrideInterpolationCount.value():-1)<<"\n";
    out<<"fg_live_enabled="<<int(s.activeFgInput==FGInput::NvngxFG&&
        s.activeFgNvngx==FGNvngxReplacement::HelixSR&&!forcedOff)<<"\n";
    out<<"fg_live_toggle_tick="<<latest_fg_apply_tick.load()<<"\n";
    out<<"upscaler_request_tick="<<latest_backend_apply_tick.load()<<"\n";
    {
        std::lock_guard<std::mutex> lock(mutex);
        out<<"upscaler_requested="<<backend_target<<"\n";
        out<<"last_apply_stage="<<last_apply_stage<<"\n";
    }
    // Live renderer route, distinct from saved INI and from proved generated frames.
    out<<"fg_input="<<(s.activeFgInput==FGInput::NvngxFG?"nvngxfg":
        s.activeFgInput==FGInput::NoFG?"nofg":"auto")<<"\n";
    out<<"fg_output=auto\n";
    out<<"fg_provider_requested="<<(s.activeFgNvngx==FGNvngxReplacement::HelixSR?"helixsr":"auto")<<"\n";
    out<<"fg_enabled="<<int(cfg->IsFGEnabled())<<"\n";
    out<<"helix_int8="<<(cfg->HelixSRInt8.has_value()?cfg->HelixSRInt8.value():"auto")<<"\n";
    out<<"helix_wave_size="<<(cfg->HelixSRWaveSize.has_value()?std::to_string(cfg->HelixSRWaveSize.value()):"auto")<<"\n";
    out<<"helix_fg_preset="<<(cfg->HelixSRFGPreset.has_value()?std::to_string(cfg->HelixSRFGPreset.value()):"auto")<<"\n";
    out<<"helix_quickflow="<<(cfg->HelixSRQuickFlow.has_value()?std::to_string(cfg->HelixSRQuickFlow.value()):"auto")<<"\n";

    out<<"graphics_api="<<int(s.api)<<"\n";
    out<<"helix_scaler_sdh_bridge=1\n";
    return out.str();
}
} // internal
void RenderTick(uint32_t source){
    source_code.store(source);
    const auto frames=ticks.fetch_add(1)+1;
    last_tick.store(millis());
    if(frames%3==0){const auto state=collect();std::lock_guard<std::mutex> lock(mutex);latest=state;}
    std::shared_ptr<Job> job;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if(jobs.empty())return;
        job=jobs.front();jobs.pop_front();
    }
    std::string result;
    try{result=apply(job->command);}
    catch(...){result="ok=0\nerror=exception\n";}
    {
        std::lock_guard<std::mutex> lock(mutex);
        job->result=std::move(result);job->done=true;
    }
    cv.notify_all();
}
} // SDHLive
extern "C" {
__declspec(dllexport) uint32_t __cdecl SDH_Control_Version(){
    const auto last=SDHLive::last_tick.load();
    return last&&SDHLive::millis()-last<5000?11u:0u;
}
__declspec(dllexport) uint32_t __cdecl SDH_Control_ConfigPathW(wchar_t* out,uint32_t capacity){
    if(!out||capacity<2)return 0;
    const auto path=Config::Instance()->IniPath().wstring();
    if(path.empty()||path.size()+1>=capacity)return 0;
    std::copy(path.begin(),path.end(),out);
    out[path.size()]=0;
    return static_cast<uint32_t>(path.size());
}
__declspec(dllexport) void* __cdecl SDH_Control_RealSwapchain(){
    return State::Instance().currentRealSwapchain;
}
__declspec(dllexport) uint32_t __cdecl SDH_Control_Apply(const char* command,char* error,uint32_t capacity){
    if(!command||!error||capacity<2)return 0;
    error[0]=0;
    const auto len=strnlen(command,8192);
    if(len==8192){strncpy_s(error,capacity,"command_too_long",_TRUNCATE);return 0;}
    auto job=std::make_shared<SDHLive::Job>();
    job->command.assign(command,len);
    {
        std::lock_guard<std::mutex> lock(SDHLive::mutex);
        if(SDHLive::jobs.size()>8){strncpy_s(error,capacity,"busy",_TRUNCATE);return 0;}
        SDHLive::jobs.push_back(job);
    }
    std::unique_lock<std::mutex> lock(SDHLive::mutex);
    if(!SDHLive::cv.wait_for(lock,std::chrono::milliseconds(1200),[&]{return job->done;})){
        auto i=std::find(SDHLive::jobs.begin(),SDHLive::jobs.end(),job);
        if(i!=SDHLive::jobs.end())SDHLive::jobs.erase(i);
        strncpy_s(error,capacity,"render_thread_timeout",_TRUNCATE);
        return 0;
    }
    if(job->result.rfind("ok=1",0)==0)return 1;
    auto at=job->result.find("error=");
    auto reason=at==std::string::npos?std::string("unknown_error"):job->result.substr(at+6);
    const auto end=reason.find('\n');
    if(end!=std::string::npos)reason.resize(end);
    strncpy_s(error,capacity,reason.c_str(),_TRUNCATE);
    return 0;
}
__declspec(dllexport) uint32_t __cdecl SDH_Control_Snapshot(char* dst,uint32_t capacity){
    if(!dst||capacity<2)return 0;
    std::lock_guard<std::mutex> lock(SDHLive::mutex);
    const auto& snapshot=SDHLive::latest;
    if(snapshot.empty()||snapshot.size()+1>=capacity)return 0;
    std::memcpy(dst,snapshot.data(),snapshot.size());
    dst[snapshot.size()]=0;
    return static_cast<uint32_t>(snapshot.size());
}
}
