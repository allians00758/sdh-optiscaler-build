// SDH Live Control v2 provider: included by PluginApi.cpp, inside anonymous namespace.
// Requires Config.h, State.h and <atomic>, <mutex>, <deque>, <map>, <charconv>, <cmath>, <sstream>.
// Runs mutations on OptiScaler's Present/render thread, not the ASI worker thread.

struct SdhPending {
    uint64_t seq;
    std::string text;
};
static std::mutex sdh_live_mutex;
static std::deque<SdhPending> sdh_pending;
static std::map<uint64_t, std::string> sdh_results;
static std::atomic<uint64_t> sdh_core_tick {0};
static std::atomic<uint32_t> sdh_tick_source {0};
static std::string sdh_last_render_snapshot;
static std::string sdh_last_request_flags;

static std::map<std::string,std::string> SdhParse(std::string_view input) {
    std::map<std::string,std::string> values;
    size_t start=0;
    while(start<input.size()) {
        size_t end=input.find('\n',start);
        if(end==std::string_view::npos) end=input.size();
        auto line=input.substr(start,end-start);
        if(!line.empty()&&line.back()=='\r') line.remove_suffix(1);
        auto equal=line.find('=');
        if(equal!=std::string_view::npos && equal>0 && equal<96 && line.size()<512)
            values[std::string(line.substr(0,equal))]=std::string(line.substr(equal+1));
        start=end+1;
    }
    return values;
}
static bool SdhFlag(const std::map<std::string,std::string>& values,const char* name){
    auto it=values.find(name);
    return it!=values.end() && (it->second=="1"||it->second=="true");
}
static std::string SdhGet(const std::map<std::string,std::string>& values,const char* name,const char* fallback=""){
    auto it=values.find(name); return it==values.end()?fallback:it->second;
}
static bool SdhBool(std::string_view value,bool& out){
    if(value=="1"||value=="true"){out=true;return true;}
    if(value=="0"||value=="false"){out=false;return true;}
    return false;
}
static bool SdhFloat(const std::string& value, float minimum,float maximum,float& out){
    try {
        size_t length=0; float v=std::stof(value,&length);
        if(length==value.size() && std::isfinite(v) && v>=minimum && v<=maximum) {out=v;return true;}
    }catch(...){}
    return false;
}

static std::string SdhLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}
static std::string SdhFirst(const std::map<std::string,std::string>& values,
                            std::initializer_list<const char*> names) {
    for (const char* name : names) {
        auto it=values.find(name);
        if(it!=values.end()&&!it->second.empty()) return it->second;
    }
    return "";
}
static bool SdhWantsBackend(const std::map<std::string,std::string>& vals) {
    return SdhFlag(vals,"has_backend") || SdhFlag(vals,"has_upscaler") ||
           SdhFlag(vals,"has_upscaler_backend");
}
static bool SdhBackendAllowed(Upscaler backend, API api) {
    switch(api) {
    case DX12:
        return backend==Upscaler::XeSS || backend==Upscaler::FSR21 ||
               backend==Upscaler::FSR22 || backend==Upscaler::FFX ||
               backend==Upscaler::DLSS;
    case DX11:
        return backend==Upscaler::XeSS || backend==Upscaler::FSR22 ||
               backend==Upscaler::FSR31 || backend==Upscaler::XeSS_on12 ||
               backend==Upscaler::FSR21_on12 || backend==Upscaler::FSR22_on12 ||
               backend==Upscaler::FFX_on12 || backend==Upscaler::DLSS ||
               backend==Upscaler::DLSS_on12;
    case Vulkan:
        return backend==Upscaler::XeSS || backend==Upscaler::FSR21 ||
               backend==Upscaler::FSR22 || backend==Upscaler::FFX ||
               backend==Upscaler::FSR21_on12 || backend==Upscaler::FFX_on12 ||
               backend==Upscaler::DLSS || backend==Upscaler::DLSS_on12;
    default: return false;
    }
}
static bool SdhChooseFfxVersion(const State& state, API api, std::string requested, int& selected) {
    // No fixed memory signatures: query the live FFX version list provided by OptiScaler.
    const auto& versions=(api==Vulkan)?state.ffxUpscalerVersionsVk:state.ffxUpscalerVersionsDx12;
    const bool requestMajor4=requested.rfind("fsr4",0)==0;
    const bool requestMajor3=requested.rfind("fsr3",0)==0;
    if(!requestMajor4&&!requestMajor3) return false;
    std::string version=requested.substr(3); // "fsr4.1" => "4.1"
    selected=-1;
    std::string best;
    for(size_t i=0;i<versions.names.size();++i) {
        if(!versions.names[i])continue;
        std::string item=SdhLower(versions.names[i]);
        if(item.rfind(version,0)!=0)continue;
        // Prefer highest reported version for an unqualified request.
        if(selected<0 || item>best) { selected=static_cast<int>(i);best=item; }
    }
    return selected>=0;
}

static bool SdhQueue(OptiPlugin* plugin,uint64_t seq,const char* command){
    if(!plugin||!seq||!command) return false;
    size_t len=0;while(len<8193 && command[len])++len;
    if(len==0||len>8192)return false;
    std::lock_guard<std::mutex> lock(sdh_live_mutex);
    if(sdh_pending.size()>=8)return false;
    sdh_pending.push_back({seq,std::string(command,len)});
    return true;
}
static uint32_t SdhPoll(OptiPlugin* plugin,uint64_t seq,char* output,uint32_t cap){
    if(!plugin||!output||cap<2)return 0;
    std::lock_guard<std::mutex> lock(sdh_live_mutex);
    auto it=sdh_results.find(seq);
    if(it==sdh_results.end())return 0;
    auto len=std::min(static_cast<size_t>(cap-1),it->second.size());
    memcpy(output,it->second.data(),len);output[len]=0;
    sdh_results.erase(it);
    return static_cast<uint32_t>(len);
}
static uint32_t SdhSnapshot(OptiPlugin* plugin,char* output,uint32_t cap){
    if(!plugin||!output||cap<128)return 0;
    std::string result="core_tick="+std::to_string(sdh_core_tick.load(std::memory_order_acquire))+"\n";
    result+="tick_source_code="+std::to_string(sdh_tick_source.load(std::memory_order_acquire))+"\n";
    {
        // Read cached values only: live Config must not be read off render thread.
        std::lock_guard<std::mutex> lock(sdh_live_mutex);
        result+=sdh_last_render_snapshot;
        result+="last_request_flags="+sdh_last_request_flags+"\n";
    }
    if(result.size()>=cap)return 0;
    memcpy(output,result.data(),result.size());output[result.size()]=0;
    return static_cast<uint32_t>(result.size());
}
static std::string SdhApplyOnRenderThread(std::string_view command){
    auto vals=SdhParse(command);
    // An unrecognized has_* flag must fail the whole command, never fake success.
    constexpr std::string_view flags[]={"has_sharpness","has_contrast","has_motion_sharpness", "has_fg_enabled","has_quality","has_scale","has_shader","has_backend","has_upscaler","has_upscaler_backend","has_fg_input","has_fg_output"};
    bool requested=false;
    {
        std::string names;
        for(const auto& [key,val]:vals){
            if(key.rfind("has_",0)==0 && (val=="1" || val=="true")){
                if(!names.empty())names+=",";
                names+=key;
            }
        }
        std::lock_guard<std::mutex> guard(sdh_live_mutex);
        sdh_last_request_flags=std::move(names);
    }
    for(const auto& [name,value]:vals){
        if(name.rfind("has_",0)!=0 || (value!="1"&&value!="true"))continue;
        requested=true;
        if(std::find(std::begin(flags),std::end(flags),std::string_view(name))==std::end(flags))
            return "ok=0\nerror=unsupported_live_field:"+name+"\n";
    }
    if(!requested)return "ok=0\nerror=no_supported_changes\n";
    // OptiScaler's own menu requires a process restart for FGInput/FGOutput.
    // Never tell Decky the pipeline switched when only the option changed.
    if(SdhFlag(vals,"has_fg_input") || SdhFlag(vals,"has_fg_output"))
        return "ok=0\nerror=requires_restart_fg_pipeline\napply_stage=ini_restart_required\n";
    float sharp=0,contrast=0,motion=0,quality=0,scale=0;
    bool sharpEnabled=false,contrastEnabled=false,motionEnabled=false,fgEnabled=false,qualityEnabled=false;
    if(SdhFlag(vals,"has_sharpness") &&
       (!SdhBool(SdhGet(vals,"sharpness_enabled"),sharpEnabled)||
        !SdhFloat(SdhGet(vals,"sharpness"),0.f,1.f,sharp)))return "ok=0\nerror=invalid_sharpness\n";
    if(SdhFlag(vals,"has_contrast") &&
       (!SdhBool(SdhGet(vals,"contrast_enabled"),contrastEnabled)||
        !SdhFloat(SdhGet(vals,"contrast"),-2.f,2.f,contrast)))return "ok=0\nerror=invalid_contrast\n";
    if(SdhFlag(vals,"has_motion_sharpness") &&
       (!SdhBool(SdhGet(vals,"motion_sharpness_enabled"),motionEnabled)||
        !SdhFloat(SdhGet(vals,"motion_sharpness"),-1.f,1.f,motion)))return "ok=0\nerror=invalid_motion_sharpness\n";
    if(SdhFlag(vals,"has_fg_enabled") &&
       !SdhBool(SdhGet(vals,"fg_enabled"),fgEnabled))return "ok=0\nerror=invalid_fg_enabled\n";
    if(SdhFlag(vals,"has_quality") &&
       (!SdhBool(SdhGet(vals,"quality_enabled"),qualityEnabled)||
        (qualityEnabled&&!SdhFloat(SdhGet(vals,"quality_ratio"),1.f,3.f,quality))))
            return "ok=0\nerror=invalid_quality\n";
    if(SdhFlag(vals,"has_scale") && !SdhFloat(SdhGet(vals,"scale"),0.5f,3.f,scale))
        return "ok=0\nerror=invalid_scale\n";
    // Validate shader mode and requested upscaler before any mutation.
    bool shaderOn=false;
    SharpenShader shader=SharpenShader::RCAS;
    if(SdhFlag(vals,"has_shader")) {
        auto name=SdhLower(SdhFirst(vals,{"shader","sharpness_shader","shader_method"}));
        if(name=="0" || name=="default" || name=="off" || name=="none" || name=="auto")
            shaderOn=false;
        else if(name=="1" || name=="rcas") {shaderOn=true;shader=SharpenShader::RCAS;}
        else if(name=="2" || name=="da" || name=="depthaware" || name=="depth_aware")
            {shaderOn=true;shader=SharpenShader::DepthAware;}
        else if(name=="3" || name=="lcda" || name=="localcontrastdepthaware")
            {shaderOn=true;shader=SharpenShader::LocalContrastDepthAware;}
        else return "ok=0\nerror=invalid_shader\n";
    }
    bool hasBackend=SdhWantsBackend(vals);
    Upscaler backend=Upscaler::Reset;
    int ffxVersion=-1;
    bool ffxSpecified=false;
    auto& state=State::Instance();
    if(hasBackend) {
        auto code=SdhLower(SdhFirst(vals,{"backend","upscaler","upscaler_backend"}));
        if(code.empty()) return "ok=0\nerror=missing_backend\n";
        const bool isVersionedFfx=(code.rfind("fsr4",0)==0 || code.rfind("fsr3",0)==0);
        if(isVersionedFfx) {
            if(!SdhChooseFfxVersion(state,state.api,code,ffxVersion))
                return "ok=0\nerror=ffx_version_unavailable\n";
            backend=(state.api==DX11)?Upscaler::FFX_on12:Upscaler::FFX;
            ffxSpecified=true;
        } else {
            backend=CodeToUpscalerFfx(code);
        }
        if(backend==Upscaler::Reset || !SdhBackendAllowed(backend,state.api))
            return "ok=0\nerror=invalid_or_unsupported_backend\n";
        if((backend==Upscaler::DLSS || backend==Upscaler::DLSS_on12) &&
           !state.NVNGX_DLSS_Path.has_value())
            return "ok=0\nerror=dlss_binary_unavailable\n";
        if(!state.currentFeature || state.changeBackend.empty())
            return "ok=0\nerror=active_upscaler_context_unavailable\n";
    }
    // All validation precedes mutation: a failed command changes nothing.
    auto* cfg=Config::Instance();
    if(!cfg)return "ok=0\nerror=config_unavailable\n";
    if(SdhFlag(vals,"has_sharpness")){
        cfg->OverrideSharpness=sharpEnabled;
        cfg->Sharpness=sharp;
    }
    if(SdhFlag(vals,"has_contrast")){
        cfg->ContrastEnabled=contrastEnabled;
        cfg->Contrast=contrast;
    }
    if(SdhFlag(vals,"has_motion_sharpness")){
        cfg->MotionSharpnessEnabled=motionEnabled;
        cfg->MotionSharpness=motion;
    }
    if(SdhFlag(vals,"has_fg_enabled")){
        cfg->FGEnabled=fgEnabled;
        state.fgSettingsChanged=true;
    }
    if(SdhFlag(vals,"has_shader")){
        cfg->RcasEnabled=shaderOn;
        if(shaderOn)cfg->SharpnessShader=shader;
    }
    bool recreate=false;
    if(hasBackend){
        if(ffxSpecified)cfg->FfxUpscalerIndex=ffxVersion;
        if(state.api==DX12)cfg->Dx12Upscaler=backend;
        else if(state.api==DX11)cfg->Dx11Upscaler=backend;
        else if(state.api==Vulkan)cfg->VulkanUpscaler=backend;
        state.newBackend=backend;
        for(auto& value:state.changeBackend)value.second=true;
        recreate=true;
    }
    if(SdhFlag(vals,"has_quality")){
        cfg->UpscaleRatioOverrideEnabled=qualityEnabled;
        if(qualityEnabled)cfg->UpscaleRatioOverrideValue=quality;
        recreate=true;
    }
    if(SdhFlag(vals,"has_scale")){
        cfg->OutputScalingEnabled=(scale!=1.f);
        cfg->OutputScalingMultiplier=scale;
        recreate=true;
    }
    if(recreate && state.currentFeature){
        // Keep the explicitly requested backend when the same transaction
        // changes quality/scale. The old code overwrote it with the active one.
        if(!hasBackend)state.newBackend=state.currentFeature->GetUpscalerType();
        for(auto& value:state.changeBackend)value.second=true;
    }
    // 'ok' means Config updated on render thread, not that a renderer was
    // definitively recreated; expose requested recreation separately.
    return recreate?"ok=1\napply_stage=renderer_reinit_requested\n":"ok=1\napply_stage=render_config_updated\n";
}
static void SdhLiveRenderTick(uint32_t source){
    sdh_tick_source.store(source,std::memory_order_release);
    auto tick=sdh_core_tick.fetch_add(1,std::memory_order_release)+1;
    if(tick%3==0){
        if(auto* cfg=Config::Instance()){
            std::string snapshot="capabilities=sharpness,contrast,motion_sharpness,fg_enabled,quality,scale,shader,backend\n";
            snapshot+="sharpness_enabled="+std::to_string((int)cfg->OverrideSharpness.value_or_default())+"\n";
            snapshot+="sharpness="+std::to_string(cfg->Sharpness.value_or_default())+"\n";
            snapshot+="contrast_enabled="+std::to_string((int)cfg->ContrastEnabled.value_or_default())+"\n";
            snapshot+="contrast="+std::to_string(cfg->Contrast.value_or_default())+"\n";
            snapshot+="motion_sharpness_enabled="+std::to_string((int)cfg->MotionSharpnessEnabled.value_or_default())+"\n";
            snapshot+="motion_sharpness="+std::to_string(cfg->MotionSharpness.value_or_default())+"\n";
            snapshot+="quality_enabled="+std::to_string((int)cfg->UpscaleRatioOverrideEnabled.value_or_default())+"\n";
            snapshot+="quality_ratio="+std::to_string(cfg->UpscaleRatioOverrideValue.value_or_default())+"\n";
            snapshot+="scale_enabled="+std::to_string((int)cfg->OutputScalingEnabled.value_or_default())+"\n";
            snapshot+="scale="+std::to_string(cfg->OutputScalingMultiplier.value_or_default())+"\n";
            snapshot+="fg_enabled="+std::to_string((int)cfg->IsFGEnabled())+"\n";
            snapshot+="shader="+std::string(cfg->RcasEnabled.value_or_default() ?
                (cfg->SharpnessShader.value_or_default()==SharpenShader::RCAS?"rcas":
                 cfg->SharpnessShader.value_or_default()==SharpenShader::DepthAware?"da":"lcda") :
                "default")+"\n";
            auto& state=State::Instance();
            snapshot+="active_backend="+(state.currentFeature ?
                UpscalerToCode(state.currentFeature->GetUpscalerType()):std::string("none"))+"\n";
            snapshot+="requested_backend="+UpscalerToCode(state.newBackend)+"\n";
            snapshot+="fg_input_active="+std::to_string(static_cast<int>(state.activeFgInput))+"\n";
            snapshot+="fg_output_active="+std::to_string(static_cast<int>(state.activeFgOutput))+"\n";
            std::lock_guard<std::mutex> lock(sdh_live_mutex);
            sdh_last_render_snapshot=std::move(snapshot);
        }
    }
    SdhPending job;
    {
        std::lock_guard<std::mutex> lock(sdh_live_mutex);
        if(sdh_pending.empty())return;
        job=std::move(sdh_pending.front());sdh_pending.pop_front();
    }
    std::string result;
    try{result=SdhApplyOnRenderThread(job.text);}catch(...){result="ok=0\nerror=unexpected_exception\n";}
    std::lock_guard<std::mutex> lock(sdh_live_mutex);
    sdh_results[job.seq]=std::move(result);
    while(sdh_results.size()>32)sdh_results.erase(sdh_results.begin());
}
