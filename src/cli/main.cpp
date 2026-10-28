#include <meguviz/rife.h>
#include <meguviz/backend_ncnn_sdk.hpp>
#include "private/data.hpp"
#include "private/catalog.hpp"
#include "build_identity.hpp"
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

using namespace mgr_detail;
namespace fs = std::filesystem;
namespace {
void check(MgcStatus s, const char* phase) { require(s == MGC_SUCCESS, 9, std::string(phase) + ":" + std::to_string(s)); }
MgcHash digest(const std::string& s) {
    require(valid_hash(s), 4, "invalid_sha256"); MgcHash h{};
    for (size_t i=0;i<32;i++) h.bytes[i] = uint8_t(std::stoul(s.substr(i*2,2), nullptr,16));
    return h;
}
struct Image { uint32_t w=0,h=0; std::string pixels; };
Image ppm(const fs::path& path) {
    const auto bytes = read(path,128*1024*1024); size_t at=0;
    auto token = [&]() {
        for (;;) {
            while (at<bytes.size() && (bytes[at]==' '||bytes[at]=='\t'||bytes[at]=='\r'||bytes[at]=='\n')) at++;
            if (at<bytes.size() && bytes[at]=='#') { while(at<bytes.size()&&bytes[at]!='\n') at++; }
            else break;
        }
        const auto begin=at;
        while(at<bytes.size() && bytes[at]!=' '&&bytes[at]!='\t'&&bytes[at]!='\r'&&bytes[at]!='\n') at++;
        require(at>begin&&at-begin<32,4,"ppm_token"); return bytes.substr(begin,at-begin);
    };
    require(token()=="P6",4,"ppm_requires_p6");
    const auto sw=token(), sh=token();
    require(!sw.empty()&&!sh.empty()&&std::all_of(sw.begin(),sw.end(),[](char c){return c>='0'&&c<='9';})&&
        std::all_of(sh.begin(),sh.end(),[](char c){return c>='0'&&c<='9';}),4,"ppm_dimensions");
    const auto w=std::stoul(sw),h=std::stoul(sh);
    require(w>0&&h>0&&w<=4096&&h<=4096&&uint64_t(w)*h<=4096u*2160u,4,"ppm_dimensions");
    require(token()=="255"&&at<bytes.size(),4,"ppm_rgb8_required");
    if(bytes[at]=='\r'&&at+1<bytes.size()&&bytes[at+1]=='\n') at+=2; else at++;
    require(bytes.size()-at==uint64_t(w)*h*3,4,"ppm_payload_size");
    return {uint32_t(w),uint32_t(h),bytes.substr(at)};
}
std::string encode(const Image& image) { return "P6\n"+std::to_string(image.w)+" "+std::to_string(image.h)+"\n255\n"+image.pixels; }
struct Runtime {
    MgcRuntime* runtime=nullptr; MgcDomain* domain=nullptr; MgrModel* model=nullptr; MgrPlan* plan=nullptr;
    MgrSession* session=nullptr; MgcLease *left=nullptr,*right=nullptr; MgcTicket* ticket=nullptr; MgrResult* result=nullptr;
    ~Runtime() {
        if(session) { mgr_drain(session,UINT64_MAX); mgr_session_close(session); }
        mgr_result_release(result); mgc_ticket_release(ticket); mgc_lease_release(left);mgc_lease_release(right);
        mgr_plan_release(plan);mgr_model_release(model);mgc_domain_release(domain);
        if(runtime) { mgc_runtime_close(runtime);mgc_runtime_release(runtime); }
    }
};
MgcLease* upload(MgcDomain* domain,const Image& image) {
    MgcResourceDesc r{};r.header=MGC_HEADER_INIT(MgcResourceDesc);r.capacity_bytes=image.pixels.size();
    r.width=image.w;r.height=image.h;r.format=MGC_FORMAT_RGB8;r.row_stride_bytes=uint64_t(image.w)*3;
    MgcResource* resource=nullptr;check(mgc_resource_create(domain,&r,&resource),"resource");
    MgcLeaseDesc d{};d.header=MGC_HEADER_INIT(MgcLeaseDesc);d.byte_length=r.capacity_bytes;d.access=MGC_ACCESS_WRITE;d.content_id=1;
    MgcLease* lease=nullptr;auto s=mgc_lease_create(resource,&d,&lease);
    if(s!=MGC_SUCCESS){mgc_resource_release(resource);check(s,"write_lease");}
    void* ptr=nullptr;uint64_t size=0;s=mgc_lease_map(lease,&ptr,&size);
    if(s==MGC_SUCCESS){std::memcpy(ptr,image.pixels.data(),image.pixels.size());s=mgc_lease_unmap(lease);}
    mgc_lease_release(lease);lease=nullptr;
    if(s==MGC_SUCCESS){d.access=MGC_ACCESS_READ;s=mgc_lease_create(resource,&d,&lease);}
    mgc_resource_release(resource);check(s,"input_upload");return lease;
}
struct Completion { MgcStatus status=MGC_INTERNAL_ERROR; MgrResult* result=nullptr; unsigned count=0; };
void completed(void* c,uint64_t,MgcStatus status,MgrResult* result) {
    auto& value=*static_cast<Completion*>(c);value.status=status;value.result=result;value.count++;
}
J interpolate(const J& spec) {
    keys(spec,{"schema_version","model_directory","left","right","alpha","output","precision_profile"});
    require(spec.number("schema_version")==1,4,"run_schema");
    require(spec.text("precision_profile")=="CPU_FP32_REFERENCE",3,"only_explicit_reference_qualified_on_host");
    size_t consumed=0;const auto alpha=std::stod(spec.at("alpha").str(),&consumed);
    require(consumed==spec.at("alpha").str().size()&&std::isfinite(alpha)&&alpha>=0&&alpha<=1,4,"alpha_range");
    const auto a=ppm(fs::u8path(spec.at("left").str())),b=ppm(fs::u8path(spec.at("right").str()));
    require(a.w==b.w&&a.h==b.h,4,"shape_mismatch");
    const auto manifest=Parser(meguviz::rife::catalog_manifest).parse();
    const auto modelDir=fs::u8path(spec.at("model_directory").str());
    const auto graph=read(modelDir/"flownet.param",1024*1024),weights=read(modelDir/"flownet.bin",32*1024*1024);
    Runtime r;MgcRuntimeDesc rd{};rd.header=MGC_HEADER_INIT(MgcRuntimeDesc);rd.provider=MGC_PROVIDER_NCNN_CPU;
    rd.memory_budget_bytes=256u*1024u*1024u;check(mgc_runtime_create(&rd,&r.runtime),"runtime");
    MgcDomainDesc dd{};dd.header=MGC_HEADER_INIT(MgcDomainDesc);dd.private_sdk_fingerprint=digest(meguviz::private_sdk_fingerprint());
    check(mgc_domain_acquire(r.runtime,&dd,&r.domain),"domain");
    MgrModelDesc md{};md.header=MGC_HEADER_INIT(MgrModelDesc);
    md.manifest_utf8=reinterpret_cast<const uint8_t*>(meguviz::rife::catalog_manifest);md.manifest_size=std::strlen(meguviz::rife::catalog_manifest);
    md.graph_data=reinterpret_cast<const uint8_t*>(graph.data());md.graph_size=graph.size();
    md.weights_data=reinterpret_cast<const uint8_t*>(weights.data());md.weights_size=weights.size();
    md.graph_sha256=digest(manifest.at("graph").text("sha256"));md.weights_sha256=digest(manifest.at("weights").text("sha256"));
    md.private_sdk_fingerprint=dd.private_sdk_fingerprint;check(mgr_model_create(r.domain,&md,&r.model),"model");
    MgrPlanDesc pd{};pd.header=MGC_HEADER_INIT(MgrPlanDesc);pd.width=a.w;pd.height=a.h;pd.input_format=MGC_FORMAT_RGB8;
    pd.precision_profile=MGR_PRECISION_FP32_REFERENCE;pd.flow_scale=1;check(mgr_plan_create(r.model,&pd,&r.plan),"plan");
    MgcSessionDesc sd{};sd.header=MGC_HEADER_INIT(MgcSessionDesc);sd.max_in_flight=1;check(mgr_session_create(r.plan,&sd,&r.session),"session");
    r.left=upload(r.domain,a);r.right=upload(r.domain,b);
    MgrRequest request{};request.header=MGC_HEADER_INIT(MgrRequest);request.request_id=1;request.alpha=alpha;
    request.left=r.left;request.right=r.right;mgc_lease_retain(r.left);mgc_lease_retain(r.right);
    request.output.header=MGC_HEADER_INIT(MgrOutputSpec);request.output.width=a.w;request.output.height=a.h;
    request.output.format=MGC_FORMAT_RGB8;request.output.row_stride_bytes=uint64_t(a.w)*3;
    Completion completion;
    const auto began=std::chrono::steady_clock::now();
    const auto accepted=mgr_submit(r.session,&request,completed,&completion,nullptr,nullptr,&r.ticket);
    if(accepted!=MGC_ACCEPTED){mgc_lease_release(request.left);mgc_lease_release(request.right);check(accepted,"submit");}
    check(mgr_drain(r.session,UINT64_MAX),"drain");
    const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-began).count();
    r.result=completion.result;check(completion.status,"inference");require(completion.count==1&&r.result,9,"completion_or_output_missing");
    MgrResultInfo info{};info.header=MGC_HEADER_INIT(MgrResultInfo);check(mgr_result_describe(r.result,&info),"result");
    check(mgr_session_close(r.session),"close_session_with_owned_output");r.session=nullptr;
    mgr_plan_release(r.plan);r.plan=nullptr;mgr_model_release(r.model);r.model=nullptr;
    mgc_ticket_release(r.ticket);r.ticket=nullptr;
    mgc_lease_release(r.left);r.left=nullptr;mgc_lease_release(r.right);r.right=nullptr;
    mgc_domain_release(r.domain);r.domain=nullptr;
    require(mgc_runtime_close(r.runtime)==MGC_BUSY,9,"output_must_pin_domain_after_session_close");
    void* pixels=nullptr;uint64_t size=0;check(mgc_lease_map(info.output,&pixels,&size),"output_map");
    Image output{a.w,a.h,std::string(static_cast<const char*>(pixels),size_t(size))};check(mgc_lease_unmap(info.output),"output_unmap");
    const auto encoded=encode(output);write(fs::u8path(spec.at("output").str()),encoded);
    return J::O{{"result","PASS"},{"scope","CPU_FP32_REFERENCE_REAL_MODEL_ONLY"},{"algorithm_accepted",true},
        {"frame_kind",int64_t(info.frame_kind)},{"input_left_sha256",sha256(a.pixels)},{"input_right_sha256",sha256(b.pixels)},
        {"output_sha256",sha256(encoded)},{"pixel_sha256",sha256(output.pixels)},{"output_bytes",int64_t(encoded.size())},
        {"width",int64_t(a.w)},{"height",int64_t(a.h)},{"alpha",spec.at("alpha")},{"service_wall_ns",int64_t(elapsed)},
        {"private_sdk_fingerprint",meguviz::private_sdk_fingerprint()},{"model_graph_sha256",sha256(graph)},
        {"build_fingerprint",meguviz::rife::build_fingerprint},
        {"effective_options_hash",sha256(meguviz::rife::reference_options)},
        {"output_survived_session_close",true},{"runtime_close_with_owned_output","BUSY"},
        {"model_weights_sha256",sha256(weights)},{"performance_qualification","INCONCLUSIVE"},
        {"vulkan_legacy_equivalence","INCONCLUSIVE"},{"guards","CPU full-graph reference; reduced-flow motion/tile profile not supported"}};
}
}
int main(int argc,char** argv) {
    std::string report;
    try {
        require(argc>=2,2,"usage: mgr-cli inspect-model | interpolate --run-spec FILE --report FILE");
        const std::string command=argv[1];std::string runSpec;
        for(int i=2;i<argc;i+=2){require(i+1<argc,2,"missing_argument");std::string key=argv[i];
            if(key=="--run-spec") {require(runSpec.empty(),2,"duplicate_run_spec");runSpec=argv[i+1];}
            else if(key=="--report") {require(report.empty(),2,"duplicate_report");report=argv[i+1];}
            else throw Error(2,"unknown_argument");}
        J output;
        if(command=="inspect-model") output=Parser(meguviz::rife::catalog_manifest).parse();
        else if(command=="interpolate") {require(!runSpec.empty(),2,"run_spec_required");output=interpolate(load(fs::u8path(runSpec)));}
        else if(command=="--version-json") output=J::O{{"version","0.1.0-dev"},{"scope","CPU reference implementation"},{"build_fingerprint",meguviz::rife::build_fingerprint},{"private_sdk_fingerprint",meguviz::private_sdk_fingerprint()}};
        else throw Error(3,"unsupported_command");
        if(!report.empty())write(fs::u8path(report),dump(output)+"\n");std::cout<<dump(output)<<'\n';return 0;
    } catch(const Error& e) {
        J failure=J::O{{"result","FAIL"},{"cause",e.what()},{"exit_code",e.code}};
        if(!report.empty())try{write(fs::u8path(report),dump(failure)+"\n");}catch(...){}
        std::cerr<<dump(failure)<<'\n';return e.code;
    } catch(const std::exception& e) {std::cerr<<"{\"result\":\"FAIL\",\"cause\":\"invalid_input_or_internal_error\"}\n";return 12;}
}
