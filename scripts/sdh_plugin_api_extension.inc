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
static std::string sdh_last_render_snapshot;

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
    {
        // Read cached values only: live Config must not be read off render thread.
        std::lock_guard<std::mutex> lock(sdh_live_mutex);
        result+=sdh_last_render_snapshot;
    }
    if(result.size()>=cap)return 0;
    memcpy(output,result.data(),result.size());output[result.size()]=0;
    return static_cast<uint32_t>(result.size());
}
static std::string SdhApplyOnRenderThread(std::string_view command){
    auto vals=SdhParse(command);
    // An unrecognized has_* flag must fail the whole command, never fake success.
    constexpr std::string_view flags[]={"has_sharpness","has_contrast","has_motion_sharpness", "has_fg_enabled","has_quality","has_scale"};
    bool requested=false;
    for(const auto& [name,value]:vals){
        if(name.rfind("has_",0)!=0 || (value!="1"&&value!="true"))continue;
        requested=true;
        if(std::find(std::begin(flags),std::end(flags),std::string_view(name))==std::end(flags))
            return "ok=0\nerror=unsupported_live_field:"+name+"\n";
    }
    if(!requested)return "ok=0\nerror=no_supported_changes\n";
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
    // All validation precedes mutation: a failed command changes nothing.
    auto* cfg=Config::Instance();
    if(!cfg)return "ok=0\nerror=config_unavailable\n";
    auto& state=State::Instance();
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
    bool recreate=false;
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
        state.newBackend=state.currentFeature->GetUpscalerType();
        for(auto& value:state.changeBackend)value.second=true;
    }
    // 'ok' means Config updated on render thread, not that a renderer was
    // definitively recreated; expose requested recreation separately.
    return recreate?"ok=1\napply_stage=renderer_reinit_requested\n":"ok=1\napply_stage=render_config_updated\n";
}
static void SdhLiveRenderTick(){
    auto tick=sdh_core_tick.fetch_add(1,std::memory_order_release)+1;
    if(tick%3==0){
        if(auto* cfg=Config::Instance()){
            std::string snapshot="capabilities=sharpness,contrast,motion_sharpness,fg_enabled,quality,scale\n";
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
