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
static std::string sdh_last_request_values;

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

static std::string SdhNormalize(std::string value) {
    std::string result;
    for(unsigned char c:value) if(std::isalnum(c)) result+=static_cast<char>(std::tolower(c));
    return result;
}
// Capture only control IDs and short values, never paths or arbitrary payloads.
// Results are bounded so ASI snapshot readers with 1 KiB buffers continue to work.
static std::string SdhRequestDebug(const std::map<std::string,std::string>& vals) {
    std::string out;
    for(const auto& [key,val]:vals) {
        bool relevant=key.find("shader")!=std::string::npos ||
                      key.find("upscal")!=std::string::npos ||
                      key.find("backend")!=std::string::npos ||
                      key.find("latency")!=std::string::npos ||
                      key.find("barrier")!=std::string::npos ||
                      key.find("color_space")!=std::string::npos ||
                      key.find("fsr4_preset")!=std::string::npos;
        if(!relevant || key.rfind("has_",0)==0)continue;
        std::string cleaned;
        for(unsigned char c:val) if(c>=32 && c<127 && c!=';' && c!='=')
            cleaned+=static_cast<char>(c);
        if(cleaned.size()>50)cleaned.resize(50);
        std::string piece=key+":"+cleaned;
        if(out.size()+piece.size()+1>220)break;
        if(!out.empty())out+=';';
        out+=piece;
    }
    return out;
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
// FFX SDK versionNames can include an FSR/AMD name prefix rather than
// starting with the numeric version. Compare the reported implementation
// version, not a hard-coded assumed list index.
static std::string SdhFfxVersionNumber(const char* raw) {
    if(!raw)return "";
    std::string name=SdhLower(raw);
    const auto digit=name.find_first_of("0123456789");
    if(digit==std::string::npos)return "";
    return name.substr(digit);
}
static bool SdhChooseFfxVersion(const State& state, API api, std::string requested, int& selected) {
    // No fixed memory signatures: query the live FFX version list provided by OptiScaler.
    const auto& versions=(api==Vulkan)?state.ffxUpscalerVersionsVk:state.ffxUpscalerVersionsDx12;
    const bool requestMajor4=requested.rfind("fsr4",0)==0;
    const bool requestMajor3=requested.rfind("fsr3",0)==0;
    const bool requestMajor2=requested.rfind("fsr2.3",0)==0;
    if(!requestMajor4&&!requestMajor3&&!requestMajor2) return false;
    std::string version=requested.substr(3); // "fsr4.1" => "4.1"
    selected=-1;
    std::string best;
    for(size_t i=0;i<versions.names.size();++i) {
        if(!versions.names[i])continue;
        std::string item=SdhFfxVersionNumber(versions.names[i]);
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
        result+="last_request_values="+sdh_last_request_values+"\n";
    }
    if(result.size()>=cap)return 0;
    memcpy(output,result.data(),result.size());output[result.size()]=0;
    return static_cast<uint32_t>(result.size());
}
static std::string SdhApplyOnRenderThread(std::string_view command){
    auto vals=SdhParse(command);
    // An unrecognized has_* flag must fail the whole command, never fake success.
    constexpr std::string_view flags[]={"has_sharpness","has_contrast","has_motion_sharpness", "has_fg_enabled","has_quality","has_scale","has_shader","has_backend","has_upscaler","has_upscaler_backend","has_fg_input","has_fg_output","has_latencyflex_mode","has_resource_barrier_fix","has_color_space","has_fsr4_preset"};
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
        sdh_last_request_values=SdhRequestDebug(vals);
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
    // A single generic boolean cannot represent six independent D3D12
    // barrier overrides. Refuse rather than overwrite unrelated barriers.
    if(SdhFlag(vals,"has_resource_barrier_fix"))
        return "ok=0\nerror=resource_barrier_requires_per_resource_mapping\n";
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
        auto value=SdhFirst(vals,{"shader","sharpness_shader","shader_method",
                                  "sharpen_shader","sharpening_method","shader_mode"});
        auto name=SdhNormalize(value);
        if(name=="0" || name=="default" || name=="off" || name=="none" ||
           name=="auto" || name=="disabled" || name=="upscalersdefault" ||
           name=="upscalerdefault") shaderOn=false;
        else if(name=="1" || name=="rcas" || name=="cas" || name=="amdrcas")
            {shaderOn=true;shader=SharpenShader::RCAS;}
        else if(name=="2" || name=="da" || name=="depthaware" ||
                name=="depthawarercas" || name=="depthawarercas")
            {shaderOn=true;shader=SharpenShader::DepthAware;}
        else if(name=="3" || name=="lcda" || name=="das" ||
                name=="localcontrastdepthaware" || name=="depthawaredas" ||
                name=="localcontrast")
            {shaderOn=true;shader=SharpenShader::LocalContrastDepthAware;}
        else return "ok=0\nerror=invalid_shader\n";
    }
    bool hasBackend=SdhWantsBackend(vals);
    Upscaler backend=Upscaler::Reset;
    int ffxVersion=-1;
    bool ffxSpecified=false;
    auto& state=State::Instance();
    if(hasBackend) {
        auto code=SdhLower(SdhFirst(vals,{"backend","upscaler","upscaler_backend",
                                         "upscaler_type","upscaler_name"}));
        // Preserve versioned FFX names (fsr2.3, fsr3.1, fsr4.1) rather
        // than rewriting them to legacy DX11 "fsr31" or falling into
        // CodeToUpscalerFfx, which cannot resolve versioned FFX names.
        if(code.rfind("fsr",0)==0 && code.find('.')!=std::string::npos) {
            const auto digit=code.find_first_of("0123456789");
            if(digit!=std::string::npos)
                code="fsr"+code.substr(digit);
        }
        // Accept user-facing names as well as OptiScaler ini codes.
        const auto norm=SdhNormalize(code);
        if(norm=="xess")code="xess";
        else if(norm=="xess12")code="xess_12";
        else if(norm=="dlss")code="dlss";
        else if(norm=="dlss12")code="dlss_12";
        else if(norm=="fsr21" || norm=="fsr212")code="fsr21";
        else if(norm=="fsr22" || norm=="fsr221")code="fsr22";
        else if((norm=="fsr31" || norm=="fsr312") && code.find('.')==std::string::npos)
            code="fsr31";
        else if(norm=="fsr2112")code="fsr21_12";
        else if(norm=="fsr2212")code="fsr22_12";
        else if(norm=="ffx" || norm=="fsr3x" || norm=="fsr3x4")code="ffx";
        else if(norm=="ffx12")code="ffx_12";
        else if(norm=="fsr4" || norm=="fsr40")code="fsr4";
        else if(norm=="fsr3" || norm=="fsr30")code="fsr3";
        if(code.empty()) return "ok=0\nerror=missing_backend\n";
        // FFX 2.3 must take the *same* version-index path as FFX 3.x/4.x.
        // Previously fsr2.3 fell into CodeToUpscalerFfx("fsr2.3"),
        // returned Reset, and always failed invalid_or_unsupported_backend
        // even while capabilities advertised ffx2.
        const bool isVersionedFfx=(code.rfind("fsr4",0)==0 ||
                                   code.rfind("fsr3",0)==0 ||
                                   code.rfind("fsr2.3",0)==0);
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
    LFXMode latencyMode=LFXMode::Conservative;
    if(SdhFlag(vals,"has_latencyflex_mode")) {
        auto raw=SdhFirst(vals,{"latencyflex_mode","lfx_mode","mode"});
        auto mode=SdhNormalize(raw);
        if(mode=="0" || mode=="conservative") latencyMode=LFXMode::Conservative;
        else if(mode=="1" || mode=="aggressive") latencyMode=LFXMode::Aggressive;
        else if(mode=="2" || mode=="reflexids" || mode=="reflexid")
            latencyMode=LFXMode::ReflexIDs;
        else return "ok=0\nerror=invalid_latencyflex_mode\n";
    }

    // OptiScaler's FSR 4 menu only exposes these controls for a running
    // FFX feature reporting major version >=4. Reject old FSR backends.
    const bool wantsFsr4Space=SdhFlag(vals,"has_color_space");
    const bool wantsFsr4Preset=SdhFlag(vals,"has_fsr4_preset");
    std::string fsr4Space;
    int fsr4Preset=-1; // -1 is default / unset
    if(wantsFsr4Space || wantsFsr4Preset) {
        if(!state.currentFeature ||
           (state.currentFeature->GetUpscalerType()!=Upscaler::FFX &&
            state.currentFeature->GetUpscalerType()!=Upscaler::FFX_on12) ||
           state.currentFeature->Version().major<4)
            return "ok=0\nerror=fsr4_renderer_not_active\n";
        if(wantsFsr4Space) {
            fsr4Space=SdhNormalize(SdhFirst(vals,{"color_space","fsr4_color_space"}));
            if(fsr4Space!="auto" && fsr4Space!="linear" &&
               fsr4Space!="nonlinear" && fsr4Space!="srgb" && fsr4Space!="pq")
                return "ok=0\nerror=invalid_fsr4_color_space\n";
        }
        if(wantsFsr4Preset) {
            auto preset=SdhNormalize(SdhFirst(vals,{"fsr4_preset","preset"}));
            if(preset=="auto" || preset=="default" || preset.empty()) fsr4Preset=-1;
            else if(preset.size()==1 && preset[0]>='0' && preset[0]<='5')
                fsr4Preset=preset[0]-'0';
            else if(preset.size()==7 && preset.rfind("preset",0)==0 &&
                    preset[6]>='0' && preset[6]<='5')
                fsr4Preset=preset[6]-'0';
            else return "ok=0\nerror=invalid_fsr4_preset\n";
        }
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
    if(SdhFlag(vals,"has_latencyflex_mode")){
        cfg->FN_LatencyFlexMode=latencyMode;
        state.fakenvapiReloadLowLatency=true;
    }
    if(SdhFlag(vals,"has_shader")){
        cfg->RcasEnabled=shaderOn;
        if(shaderOn)cfg->SharpnessShader=shader;
    }
    bool recreate=wantsFsr4Space||wantsFsr4Preset;

    if(wantsFsr4Space) {
        if(fsr4Space=="auto") {
            cfg->FsrNonLinearColorSpace.reset();
            cfg->FsrNonLinearSRGB.reset();
            cfg->FsrNonLinearPQ.reset();
        } else {
            const bool isSrgb=fsr4Space=="srgb";
            const bool isPq=fsr4Space=="pq";
            cfg->FsrNonLinearColorSpace=(fsr4Space!="linear");
            cfg->FsrNonLinearSRGB=isSrgb;
            cfg->FsrNonLinearPQ=isPq;
        }
    }
    if(wantsFsr4Preset) {
        if(fsr4Preset<0)cfg->Fsr4Preset.reset();
        else cfg->Fsr4Preset=static_cast<uint32_t>(fsr4Preset);
    }
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
    if(recreate) return "ok=1\napply_stage=renderer_reinit_requested\n";
    if(SdhFlag(vals,"has_latencyflex_mode"))return "ok=1\napply_stage=low_latency_config_updated\n";
    return "ok=1\napply_stage=render_config_updated\n";
}
static void SdhLiveRenderTick(uint32_t source){
    sdh_tick_source.store(source,std::memory_order_release);
    auto tick=sdh_core_tick.fetch_add(1,std::memory_order_release)+1;
    if(tick%3==0){
        if(auto* cfg=Config::Instance()){
            auto& current=State::Instance();
            // Advertise FFX 2.3 live switching only when FFX SDK reports
            // an actual 2.3.x implementation for this renderer/API.
            const auto& candidates=(current.api==Vulkan)?current.ffxUpscalerVersionsVk:current.ffxUpscalerVersionsDx12;
            bool hasFfx2=false;
            for(const auto* name:candidates.names){
                if(name && SdhFfxVersionNumber(name).rfind("2.3",0)==0){hasFfx2=true;break;}
            }
            std::string snapshot="capabilities=sharpness,contrast,motion_sharpness,fg_enabled,quality,scale,shader,backend,latencyflex_mode";
            // Observe the versions OptiScaler's running FFX SDK really exposed.
            // This is the decisive diagnostic for 3.1.5/2.3.4 missing live.
            {
                const auto& ffx=(current.api==Vulkan)?current.ffxUpscalerVersionsVk:current.ffxUpscalerVersionsDx12;
                snapshot+="ffx_versions_count="+std::to_string(ffx.names.size())+"\n";
                snapshot+="ffx_available_versions=";
                size_t count=0;
                for(const char* name:ffx.names) {
                    if(!name || count>=10)continue;
                    auto version=SdhFfxVersionNumber(name);
                    if(version.empty() || version.size()>32)continue;
                    if(count++)snapshot+=",";
                    snapshot+=version;
                }
                snapshot+="\n";
                snapshot+="ffx_selected_index="+std::to_string(cfg->FfxUpscalerIndex.value_or_default())+"\n";
                snapshot+="graphics_api="+std::to_string(static_cast<int>(current.api))+"\n";
            }

            const bool fsr4Active=(current.currentFeature &&
                (current.currentFeature->GetUpscalerType()==Upscaler::FFX ||
                 current.currentFeature->GetUpscalerType()==Upscaler::FFX_on12) &&
                current.currentFeature->Version().major>=4);
            if(fsr4Active)snapshot+=",fsr4_color_space,fsr4_preset";
            if(hasFfx2)snapshot+=",ffx2";
            snapshot+="\n";
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
            if(fsr4Active) {
                std::string fsr4Space="linear";
                if(cfg->FsrNonLinearPQ.value_or_default())fsr4Space="pq";
                else if(cfg->FsrNonLinearSRGB.value_or_default())fsr4Space="srgb";
                else if(cfg->FsrNonLinearColorSpace.value_or_default())fsr4Space="non_linear";
                else if(!cfg->FsrNonLinearColorSpace.has_value() &&
                        !cfg->FsrNonLinearSRGB.has_value() &&
                        !cfg->FsrNonLinearPQ.has_value())fsr4Space="auto";
                snapshot+="color_space="+fsr4Space+"\n";
                snapshot+="fsr4_preset="+(cfg->Fsr4Preset.has_value() ?
                    std::to_string(cfg->Fsr4Preset.value()):std::string("default"))+"\n";
                snapshot+="active_fsr4_preset="+(current.currentFsr4Preset.has_value() ?
                    std::to_string(current.currentFsr4Preset.value()):std::string("unknown"))+"\n";
            }
            snapshot+="latencyflex_mode="+std::to_string(static_cast<int>(cfg->FN_LatencyFlexMode.value_or_default()))+"\n";
            snapshot+="shader="+std::string(cfg->RcasEnabled.value_or_default() ?
                (cfg->SharpnessShader.value_or_default()==SharpenShader::RCAS?"rcas":
                 cfg->SharpnessShader.value_or_default()==SharpenShader::DepthAware?"da":"lcda") :
                "default")+"\n";
            auto& state=State::Instance();
            snapshot+="active_backend="+(state.currentFeature ?
                UpscalerToCode(state.currentFeature->GetUpscalerType()):std::string("none"))+"\n";
            // Read the actual instantiated FFX version, not the requested
            // Config index. The selected index can change before reinit.
            if(state.currentFeature &&
               (state.currentFeature->GetUpscalerType()==Upscaler::FFX ||
                state.currentFeature->GetUpscalerType()==Upscaler::FFX_on12)){
                auto version=state.currentFeature->Version();
                snapshot+="active_ffx_version="+std::to_string(version.major)+"."+
                    std::to_string(version.minor)+"."+std::to_string(version.patch)+"\n";
            }

            snapshot+="requested_backend="+UpscalerToCode(state.newBackend)+"\n";
            if(state.currentFeature && (state.currentFeature->GetUpscalerType()==Upscaler::FFX ||
                                        state.currentFeature->GetUpscalerType()==Upscaler::FFX_on12)){
                const auto& versions=(state.api==Vulkan)?state.ffxUpscalerVersionsVk:state.ffxUpscalerVersionsDx12;
                int index=cfg->FfxUpscalerIndex.value_or_default();
                if(index>=0 && static_cast<size_t>(index)<versions.names.size() && versions.names[index])
                    snapshot+="selected_ffx_version="+std::string(versions.names[index])+"\n";
            }
            snapshot+="fg_input_active="+std::to_string(static_cast<int>(state.activeFgInput))+"\n";
            snapshot+="fg_output_active="+std::to_string(static_cast<int>(state.activeFgOutput))+"\n";
            // Requested pipeline vs actual initialized pipeline are distinct.
            const auto requestedFgInput=cfg->FGInput.value_or_default();
            const auto requestedFgOutput=cfg->FGOutput.value_or_default();
            snapshot+="fg_input_config="+std::to_string(static_cast<int>(requestedFgInput))+"\n";
            snapshot+="fg_output_config="+std::to_string(static_cast<int>(requestedFgOutput))+"\n";
            snapshot+="fg_pipeline_matches_config="+std::to_string((int)(
                requestedFgInput==state.activeFgInput && requestedFgOutput==state.activeFgOutput))+"\n";
            snapshot+="fg_pipeline_restart_needed="+std::to_string((int)(
                requestedFgInput!=state.activeFgInput || requestedFgOutput!=state.activeFgOutput))+"\n";
            snapshot+="fg_runtime_object="+std::to_string((int)(state.CurrentFG()!=nullptr))+"\n";
            snapshot+="fg_runtime_active="+std::to_string((int)(
                state.CurrentFG()!=nullptr && state.CurrentFG()->IsActive()))+"\n";
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
