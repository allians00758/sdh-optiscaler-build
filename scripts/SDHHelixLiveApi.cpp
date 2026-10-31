// SPDX-License-Identifier: GPL-3.0-or-later
// SDH plugin API v2 adapter for HelixSR-Scaler 2.1.0.
// No game memory patching; state changes happen only at the renderer tick.
#include "pch.h"
#include "SDHPluginApi.h"
#include "Config.h"
#include "State.h"
#include "upscalers/IFeature.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

struct OptiPlugin { uint32_t signature = 0x53444832; };
struct OptiPluginApiV2 {
    uint32_t size;
    uint32_t version;
    void* v1[30]; // Stable SDK v1 ABI slots, intentionally unimplemented.
    bool (*queue)(OptiPlugin*, uint64_t, const char*);
    uint32_t (*poll)(OptiPlugin*, uint64_t, char*, uint32_t);
    uint32_t (*snapshot)(OptiPlugin*, char*, uint32_t);
};
namespace {
OptiPlugin s_owner;
std::atomic<uint64_t> s_tick{0};
std::atomic<uint32_t> s_source{0};
std::mutex s_mutex;
std::deque<std::pair<uint64_t,std::string>> s_pending;
std::map<uint64_t,std::string> s_results;
std::string s_render_snapshot;

std::map<std::string,std::string> Parse(std::string_view command) {
    std::map<std::string,std::string> result;
    size_t off=0;
    while(off<command.size()){
        size_t end=command.find('\n',off);
        if(end==std::string_view::npos)end=command.size();
        auto line=command.substr(off,end-off);
        if(!line.empty()&&line.back()=='\r')line.remove_suffix(1);
        auto eq=line.find('=');
        if(eq!=std::string_view::npos&&eq>0&&eq<96&&line.size()<256)
            result[std::string(line.substr(0,eq))]=std::string(line.substr(eq+1));
        off=end+1;
    }
    return result;
}
bool Flag(const std::map<std::string,std::string>& vals,const char* key){
    auto it=vals.find(key);
    return it!=vals.end()&&(it->second=="1"||it->second=="true");
}
bool Bool(std::string_view v,bool& out){
    if(v=="1"||v=="true"){out=true;return true;}
    if(v=="0"||v=="false"){out=false;return true;}
    return false;
}
bool Float(const std::string& s,float low,float high,float& out){
    try{size_t n=0;float f=std::stof(s,&n);if(n==s.size()&&std::isfinite(f)&&f>=low&&f<=high){out=f;return true;}}
    catch(...){}
    return false;
}
std::string Lower(std::string s){
    std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return (char)std::tolower(c);});return s;
}
void BackendChanged(State& s){
    for(auto& entry:s.changeBackend)entry.second=true;
}
const State::FfxVersionList& Versions(const State& s){
    return s.api==API::Vulkan?s.ffxUpscalerVersionsVk:s.ffxUpscalerVersionsDx12;
}
bool SelectFfx(const State& s,std::string wanted,int& out){
    wanted=Lower(wanted);
    auto& list=Versions(s).names;
    for(size_t i=0;i<list.size();++i){
        if(!list[i])continue;
        std::string found=Lower(list[i]);
        bool match=false;
        if(wanted=="hlx_fsr4"||wanted=="helixsr")
            match=found.rfind("hlx fsr 4",0)==0;
        else if(wanted=="fsr3.1.6"||wanted=="ffx316")
            match=found.find("3.1.6")!=std::string::npos;
        else if(wanted=="fsr2.2.1")
            match=found.find("2.2.1")!=std::string::npos;
        if(match){out=(int)i;return true;}
    }
    return false;
}
bool Queue(OptiPlugin* owner,uint64_t seq,const char* command){
    if(owner!=&s_owner||seq==0||!command)return false;
    size_t len=0;while(len<=8192&&command[len])++len;
    if(len==0||len>8192)return false;
    std::lock_guard<std::mutex> lk(s_mutex);
    if(s_pending.size()>=8)return false;
    s_pending.emplace_back(seq,std::string(command,len));
    return true;
}
uint32_t Poll(OptiPlugin* owner,uint64_t seq,char* out,uint32_t cap){
    if(owner!=&s_owner||!out||cap<2)return 0;
    std::lock_guard<std::mutex> lk(s_mutex);
    auto it=s_results.find(seq);if(it==s_results.end())return 0;
    size_t n=std::min((size_t)cap-1,it->second.size());
    memcpy(out,it->second.data(),n);out[n]=0;
    s_results.erase(it);return (uint32_t)n;
}
uint32_t Snapshot(OptiPlugin* owner,char* out,uint32_t cap){
    if(owner!=&s_owner||!out||cap<128)return 0;
    std::string result="core_tick="+std::to_string(s_tick.load())+"\n";
    result+="tick_source_code="+std::to_string(s_source.load())+"\n";
    {std::lock_guard<std::mutex> lk(s_mutex);result+=s_render_snapshot;}
    if(result.size()>=cap)return 0;
    memcpy(out,result.data(),result.size());out[result.size()]=0;
    return (uint32_t)result.size();
}
OptiPluginApiV2 s_api = { sizeof(OptiPluginApiV2),2,{},Queue,Poll,Snapshot };
std::string Apply(std::string_view command){
    const auto v=Parse(command);
    for(const auto& [key,val]:v){
        if(key.rfind("has_",0)==0&&(val=="1"||val=="true")){
            if(key!="has_upscaler"&&key!="has_backend"&&key!="has_upscaler_backend"&&
               key!="has_sharpness"&&key!="has_contrast"&&key!="has_motion_sharpness"&&
               key!="has_quality"&&key!="has_scale"&&key!="has_fg_enabled")
                return "ok=0\nerror=unsupported_live_flag:"+key+"\n";
        }
    }
    auto* cfg=Config::Instance();
    if(!cfg)return "ok=0\nerror=no_config\n";
    auto& state=State::Instance();
    bool changed=false;
    bool needReinit=false;
    if(Flag(v,"has_upscaler")||Flag(v,"has_backend")||Flag(v,"has_upscaler_backend")){
        auto it=v.find("upscaler");
        if(it==v.end())it=v.find("backend");
        if(it==v.end())return "ok=0\nerror=missing_upscaler\n";
        std::string code=Lower(it->second);
        Upscaler up=Upscaler::Reset;
        int index=-1;
        if(code=="hlx_dlss"||code=="helix_dlss"||code=="dlss")up=Upscaler::DLSS;
        else if(code=="xess")up=Upscaler::XeSS;
        else if(code=="fsr22")up=Upscaler::FSR22;
        else if(code=="fsr21")up=Upscaler::FSR21;
        else if(SelectFfx(state,code,index)){up=Upscaler::FFX;}
        else return "ok=0\nerror=unsupported_or_unavailable_upscaler\n";
        if(up==Upscaler::FFX)cfg->FfxUpscalerIndex=index;
        state.newBackend=up;
        BackendChanged(state);
        changed=true;needReinit=true;
    }
    if(Flag(v,"has_sharpness")){
        auto it=v.find("sharpness");auto enabled=v.find("sharpness_enabled");
        float amount=0.0f;bool use=false;
        if(it==v.end()||enabled==v.end()||!Float(it->second,0.0f,1.0f,amount)||!Bool(enabled->second,use))
            return "ok=0\nerror=invalid_sharpness\n";
        cfg->OverrideSharpness=use;cfg->Sharpness=amount;changed=true;
    }
    if(Flag(v,"has_contrast")){
        auto it=v.find("contrast");auto enabled=v.find("contrast_enabled");
        float amount=0.0f;bool use=false;
        if(it==v.end()||enabled==v.end()||!Float(it->second,-1.0f,1.0f,amount)||!Bool(enabled->second,use))
            return "ok=0\nerror=invalid_contrast\n";
        cfg->ContrastEnabled=use;cfg->Contrast=amount;changed=true;
    }
    if(Flag(v,"has_motion_sharpness")){
        auto it=v.find("motion_sharpness");auto enabled=v.find("motion_sharpness_enabled");
        float amount=0.0f;bool use=false;
        if(it==v.end()||enabled==v.end()||!Float(it->second,0.0f,1.0f,amount)||!Bool(enabled->second,use))
            return "ok=0\nerror=invalid_motion_sharpness\n";
        cfg->MotionSharpnessEnabled=use;cfg->MotionSharpness=amount;changed=true;
    }
    if(Flag(v,"has_scale")){
        auto it=v.find("scale");float amount=0.0f;
        if(it==v.end()||!Float(it->second,1.0f,4.0f,amount))return "ok=0\nerror=invalid_scale\n";
        cfg->OutputScalingEnabled=amount!=1.0f;
        cfg->OutputScalingMultiplier=amount;changed=true;
    }
    if(Flag(v,"has_quality")){
        auto enabled=v.find("quality_enabled");bool use=false;
        if(enabled==v.end()||!Bool(enabled->second,use))return "ok=0\nerror=invalid_quality\n";
        cfg->UpscaleRatioOverrideEnabled=use;
        if(use){
            auto ratio=v.find("quality_ratio");float amount=0;
            if(ratio==v.end()||!Float(ratio->second,1.0f,4.0f,amount))return "ok=0\nerror=invalid_quality_ratio\n";
            cfg->UpscaleRatioOverrideValue=amount;
        }
        changed=true;
    }
    if(Flag(v,"has_fg_enabled")){
        auto it=v.find("fg_enabled");bool on=false;
        if(it==v.end()||!Bool(it->second,on))return "ok=0\nerror=invalid_fg\n";
        cfg->FGEnabled=on;state.fgChanged=true;changed=true;
    }
    if(!changed)return "ok=0\nerror=no_supported_change\n";
    return needReinit?"ok=1\napply_stage=renderer_reinit_requested\n":"ok=1\napply_stage=render_config_updated\n";
}
}
void SDHPluginApi::Init(HMODULE h,PFN_SdhInit init){
    if(!h||!init)return;
    bool ready=false;
    __try{ready=init(&s_api,&s_owner);}__except(EXCEPTION_EXECUTE_HANDLER){ready=false;}
    if(ready)LOG_INFO("SDH HelixSR Live API v2 initialized");
}
void SDHPluginApi::Tick(uint32_t source){
    s_source.store(source);auto tick=s_tick.fetch_add(1)+1;
    if(tick%3==0){
        auto& state=State::Instance();auto* cfg=Config::Instance();
        std::string snap="capabilities=backend,sharpness,contrast,motion_sharpness,quality,scale,fg_enabled\n";
        snap+="api="+std::to_string((int)state.api)+"\n";
        snap+="ffx_available_versions=";
        bool first=true;
        for(const char* name:Versions(state).names){
            if(!name)continue;
            if(!first)snap+='|';first=false;snap+=name;
        }
        snap+="\n";
        if(state.currentFeature)
            snap+="active_upscaler_type="+std::to_string((int)state.currentFeature->GetUpscalerType())+"\n";
        if(cfg)
            snap+="requested_ffx_index="+std::to_string(cfg->FfxUpscalerIndex.value_or_default())+"\n";
        std::lock_guard<std::mutex> lk(s_mutex);s_render_snapshot=std::move(snap);
    }
    std::pair<uint64_t,std::string> pending;
    {std::lock_guard<std::mutex> lk(s_mutex);
     if(s_pending.empty())return;
     pending=std::move(s_pending.front());s_pending.pop_front();}
    std::string reply=Apply(pending.second);
    {std::lock_guard<std::mutex> lk(s_mutex);
     if(s_results.size()>64)s_results.erase(s_results.begin());
     s_results[pending.first]=std::move(reply);}
}
