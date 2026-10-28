#include <meguviz/rife.h>
#include <meguviz/backend_ncnn_sdk.hpp>
#include "backends/ncnn/legacy/frame_engine.h"
#include "private/data.hpp"
#include "private/catalog.hpp"
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>

namespace legacy = meguviz::rife::legacy;
using mgr_detail::J;

struct MgrModel {
    std::atomic<unsigned> refs{1};
    MgcDomain* domain = nullptr;
    std::string graph, weights;
    MgcHash fingerprint{};
    ~MgrModel() { mgc_domain_release(domain); }
};
struct MgrPlan {
    std::atomic<unsigned> refs{1};
    MgrModel* model = nullptr;
    MgrPlanDesc desc{};
    std::unique_ptr<legacy::Engine> engine;
    ~MgrPlan();
};
struct MgrSession { MgrPlan* plan; MgcSession* compute; };
struct MgrResult { MgcLease* output = nullptr; MgrResultInfo info{}; ~MgrResult() { mgc_lease_release(output); } };

namespace {
template<class T> bool valid(const T* d) {
    return d && d->header.abi_major == MGC_ABI_MAJOR && d->header.struct_size >= sizeof(T);
}
template<class T> MgcStatus read_desc(const T* source, size_t minimum, T& copy) {
    if (!source) return MGC_INVALID_ARGUMENT;
    if (source->header.abi_major != MGC_ABI_MAJOR || source->header.struct_size < minimum) return MGC_UNSUPPORTED_ABI;
    copy = {};
    std::memcpy(&copy, source, std::min<size_t>(source->header.struct_size, sizeof(T)));
    copy.header.struct_size = sizeof(T);
    return MGC_SUCCESS;
}
template<class T, size_t N> bool zero(const T (&v)[N]) { for (auto x : v) if (x) return false; return true; }
std::string hex(const MgcHash& h) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out; out.reserve(64);
    for (auto b : h.bytes) { out += digits[b >> 4]; out += digits[b & 15]; }
    return out;
}
MgcStatus code(legacy::Status s) {
    switch (s.code) {
        case legacy::Code::Ok: return MGC_SUCCESS;
        case legacy::Code::InvalidArgument: return MGC_INVALID_ARGUMENT;
        case legacy::Code::Busy: return MGC_BUSY;
        case legacy::Code::AllocationFailed: return MGC_OUT_OF_MEMORY;
        case legacy::Code::UnsupportedBackend: case legacy::Code::GpuUnavailable: return MGC_UNSUPPORTED_CAPABILITY;
        case legacy::Code::InvalidModel: case legacy::Code::ModelLoadFailed: case legacy::Code::ModelTooLarge: return MGC_MODEL_MISMATCH;
        default: return MGC_INTERNAL_ERROR;
    }
}
#if NCNN_VULKAN
using DomainDevice = ncnn::VulkanDevice;
#else
using DomainDevice = void;
#endif
MgcStatus destroy_engine(void* p, const DomainDevice*) {
    static_cast<MgrPlan*>(p)->engine.reset();
    return MGC_SUCCESS;
}
MgcStatus create_engine(void* p, const DomainDevice* device) {
    auto& plan = *static_cast<MgrPlan*>(p);
    if (device) return MGC_UNSUPPORTED_CAPABILITY;
    legacy::Options options;
    options.backend = device ? legacy::Backend::Vulkan : legacy::Backend::Cpu;
    options.maxPixels = size_t(plan.desc.width) * plan.desc.height;
#if NCNN_VULKAN
    options.domainDevice = device;
#endif
    const bool reference = plan.desc.precision_profile == MGR_PRECISION_FP32_REFERENCE;
    if (!device && (!reference || plan.desc.flow_scale != 1 || plan.desc.guard_flags)) return MGC_UNSUPPORTED_CAPABILITY;
    legacy::Status status;
    auto engine = legacy::Engine::create(options, &status);
    if (!engine) return code(status);
    const bool gpuIo = !reference;
    status = engine->configure(!reference, int(plan.desc.flow_scale), gpuIo,
        (plan.desc.guard_flags & MGR_GUARD_STATIC_DETAIL) != 0, gpuIo && plan.desc.flow_scale > 1);
    if (!status.ok()) return code(status);
    engine->setMotionGuard((plan.desc.guard_flags & MGR_GUARD_MOTION) != 0);
    status = engine->load({reinterpret_cast<const uint8_t*>(plan.model->graph.data()), plan.model->graph.size(),
        reinterpret_cast<const uint8_t*>(plan.model->weights.data()), plan.model->weights.size()});
    if (!status.ok()) return code(status);
    plan.engine = std::move(engine);
    return MGC_SUCCESS;
}
struct Job {
    std::atomic<unsigned> refs{1};
    MgrPlan* plan;
    MgrRequest request;
    MgrCompletionFn callback;
    void* context;
    MgcContextFn retain_context;
    MgcContextFn release_context;
    std::unique_ptr<MgrResult> result;
    ~Job() { mgr_plan_release(plan); }
};
void retain_job(void* p) {
    auto& job = *static_cast<Job*>(p);
    job.refs.fetch_add(1);
    if (job.retain_context) job.retain_context(job.context);
}
void release_job(void* p) {
    auto* job = static_cast<Job*>(p);
    if (job->release_context) job->release_context(job->context);
    if (job->refs.fetch_sub(1) == 1) delete job;
}
void drop_caller_job(Job* job) { if (job->refs.fetch_sub(1) == 1) delete job; }
MgcStatus read_info(MgcLease* lease, MgcLeaseInfo& info) {
    info = {}; info.header = MGC_HEADER_INIT(MgcLeaseInfo);
    return mgc_lease_info(lease, &info);
}
bool image_fits(const MgcLeaseInfo& info, const MgrPlanDesc& p) {
    const uint64_t row = uint64_t(p.width) * 3;
    return info.format == MGC_FORMAT_RGB8 && info.width == p.width && info.height == p.height &&
        info.row_stride_bytes >= row && info.row_stride_bytes <= UINT64_MAX / p.height &&
        (uint64_t(p.height) - 1) * info.row_stride_bytes + row <= info.byte_length &&
        (info.access & MGC_ACCESS_READ) && info.dependency_kind == MGC_DEPENDENCY_READY;
}
MgcStatus execute_job(void* p, MgcTicket* ticket) {
    auto& job = *static_cast<Job*>(p);
    if (mgc_private_ticket_cancelled(ticket)) return MGC_CANCELLED;
    try {
        MgcLeaseInfo left{}, right{};
        auto s = read_info(job.request.left, left); if (s != MGC_SUCCESS) return s;
        s = read_info(job.request.right, right); if (s != MGC_SUCCESS) return s;
        void *a = nullptr, *b = nullptr, *out = nullptr;
        uint64_t aSize = 0, bSize = 0, outSize = 0;
        s = mgc_lease_map(job.request.left, &a, &aSize); if (s != MGC_SUCCESS) return s;
        struct Unmap { MgcLease* lease; ~Unmap() { if (lease) mgc_lease_unmap(lease); } } ua{job.request.left};
        Unmap ub{nullptr};
        if (job.request.right == job.request.left) { b = a; bSize = aSize; }
        else {
            s = mgc_lease_map(job.request.right, &b, &bSize); if (s != MGC_SUCCESS) return s;
            ub.lease = job.request.right;
        }
        MgcResourceDesc resource{}; resource.header = MGC_HEADER_INIT(MgcResourceDesc);
        resource.width = job.request.output.width; resource.height = job.request.output.height;
        resource.format = MGC_FORMAT_RGB8; resource.row_stride_bytes = job.request.output.row_stride_bytes;
        resource.capacity_bytes = resource.row_stride_bytes * resource.height;
        MgcResource* allocation = nullptr;
        s = mgc_resource_create(job.plan->model->domain, &resource, &allocation); if (s != MGC_SUCCESS) return s;
        struct ResourceOwner { MgcResource* p; ~ResourceOwner() { mgc_resource_release(p); } } ro{allocation};
        MgcLeaseDesc ld{}; ld.header = MGC_HEADER_INIT(MgcLeaseDesc);
        ld.content_id = 1; ld.byte_length = resource.capacity_bytes; ld.access = MGC_ACCESS_WRITE;
        MgcLease* writing = nullptr;
        s = mgc_lease_create(allocation, &ld, &writing); if (s != MGC_SUCCESS) return s;
        struct LeaseOwner { MgcLease* p; ~LeaseOwner() { mgc_lease_release(p); } } wo{writing};
        s = mgc_lease_map(writing, &out, &outSize); if (s != MGC_SUCCESS) return s;
        {
            Unmap uo{writing};
            const auto status = job.plan->engine->processRgb8(
                {static_cast<const uint8_t*>(a), int(resource.width), int(resource.height), size_t(left.row_stride_bytes), size_t(aSize)},
                {static_cast<const uint8_t*>(b), int(resource.width), int(resource.height), size_t(right.row_stride_bytes), size_t(bSize)},
                float(job.request.alpha),
                {static_cast<uint8_t*>(out), int(resource.width), int(resource.height), size_t(resource.row_stride_bytes), size_t(outSize)});
            s = code(status);
        }
        if (s != MGC_SUCCESS || mgc_private_ticket_cancelled(ticket)) return s == MGC_SUCCESS ? MGC_CANCELLED : s;
        mgc_lease_release(wo.p); wo.p = nullptr;
        auto result = std::make_unique<MgrResult>();
        ld.access = MGC_ACCESS_READ;
        s = mgc_lease_create(allocation, &ld, &result->output); if (s != MGC_SUCCESS) return s;
        result->info.header = MGC_HEADER_INIT(MgrResultInfo);
        result->info.request_id = job.request.request_id;
        result->info.output = result->output;
        result->info.algorithm_accepted = 1;
        result->info.frame_kind = job.request.alpha == 0 || job.request.alpha == 1 ? MGR_RESULT_ENDPOINT : MGR_RESULT_INFERRED;
        result->info.guard_outcome = job.plan->engine->patchedTiles() > 0 ? 1u : 0u;
        job.result = std::move(result);
        return MGC_SUCCESS;
    } catch (const std::bad_alloc&) { return MGC_OUT_OF_MEMORY; }
    catch (...) { return MGC_INTERNAL_ERROR; }
}
void complete_job(void* p, const MgcCompletion* c) {
    auto& job = *static_cast<Job*>(p);
    auto* result = c->terminal == MGC_TERMINAL_SUCCESS && c->status == MGC_SUCCESS && !c->result_discarded ? job.result.release() : nullptr;
    const auto status = result || c->status != MGC_SUCCESS ? c->status :
        c->result_discarded ? MGC_CANCELLED : MGC_INTERNAL_ERROR;
    job.callback(job.context, job.request.request_id, status, result);
}
}

MgrPlan::~MgrPlan() {
    meguviz::with_ncnn_domain(model->domain, destroy_engine, this);
    mgr_model_release(model);
}
extern "C" {
MgcStatus mgr_model_create(MgcDomain* domain, const MgrModelDesc* d, MgrModel** out) {
    if (!out) return MGC_INVALID_ARGUMENT; *out = nullptr;
    MgrModelDesc copied{};
    const auto abi = read_desc(d, offsetof(MgrModelDesc, required_flags) + sizeof(uint32_t), copied);
    if (abi != MGC_SUCCESS) return abi;
    d = &copied;
    if (d->required_flags) return MGC_UNSUPPORTED_ABI;
    if (!domain || !d->manifest_utf8 || !d->graph_data || !d->weights_data || !d->manifest_size ||
        d->manifest_size > 65536 || !d->graph_size || d->graph_size > 1024*1024 || !d->weights_size ||
        d->weights_size > 32*1024*1024 || d->required_flags || !zero(d->reserved)) return MGC_INVALID_ARGUMENT;
    try {
        const auto manifest = mgr_detail::Parser(std::string(reinterpret_cast<const char*>(d->manifest_utf8), size_t(d->manifest_size))).parse();
        const auto expected = mgr_detail::Parser(meguviz::rife::catalog_manifest).parse();
        if (mgr_detail::dump(manifest) != mgr_detail::dump(expected)) return MGC_MODEL_MISMATCH;
        const std::string graph(reinterpret_cast<const char*>(d->graph_data), size_t(d->graph_size));
        const std::string weights(reinterpret_cast<const char*>(d->weights_data), size_t(d->weights_size));
        if (d->graph_size != uint64_t(manifest.at("graph").number("size_bytes")) ||
            d->weights_size != uint64_t(manifest.at("weights").number("size_bytes")) ||
            mgr_detail::sha256(graph) != manifest.at("graph").text("sha256") ||
            mgr_detail::sha256(weights) != manifest.at("weights").text("sha256") ||
            hex(d->graph_sha256) != manifest.at("graph").text("sha256") ||
            hex(d->weights_sha256) != manifest.at("weights").text("sha256")) return MGC_MODEL_MISMATCH;
        if (hex(d->private_sdk_fingerprint) != meguviz::private_sdk_fingerprint()) return MGC_ARTIFACT_MISMATCH;
        auto model = std::make_unique<MgrModel>();
        mgc_domain_retain(domain); model->domain = domain;
        model->graph = graph; model->weights = weights; model->fingerprint = d->private_sdk_fingerprint;
        *out = model.release(); return MGC_SUCCESS;
    } catch (const std::bad_alloc&) { return MGC_OUT_OF_MEMORY; }
    catch (...) { return MGC_MODEL_MISMATCH; }
}
void mgr_model_retain(MgrModel* m) { if (m) m->refs.fetch_add(1); }
void mgr_model_release(MgrModel* m) { if (m && m->refs.fetch_sub(1) == 1) delete m; }
MgcStatus mgr_plan_create(MgrModel* model, const MgrPlanDesc* d, MgrPlan** out) {
    if (!out) return MGC_INVALID_ARGUMENT; *out = nullptr;
    MgrPlanDesc copied{};
    const auto abi = read_desc(d, offsetof(MgrPlanDesc, required_flags) + sizeof(uint32_t), copied);
    if (abi != MGC_SUCCESS) return abi;
    d = &copied;
    if (d->required_flags) return MGC_UNSUPPORTED_ABI;
    if (!model || !d->width || !d->height || d->width > 4096 || d->height > 4096 ||
        uint64_t(d->width) * d->height > 4096u * 2160u || d->input_format != MGC_FORMAT_RGB8 ||
        d->required_flags || !zero(d->reserved) || (d->guard_flags & ~3u) ||
        (d->flow_scale != 1 && d->flow_scale != 2 && d->flow_scale != 4 && d->flow_scale != 8) ||
        (d->precision_profile != MGR_PRECISION_FP32_REFERENCE && d->precision_profile != MGR_PRECISION_LEGACY_FP16)) return MGC_INVALID_ARGUMENT;
    // The first implemented reference is explicit; unqualified GPU extraction is not advertised.
    if (d->precision_profile != MGR_PRECISION_FP32_REFERENCE || d->flow_scale != 1 || d->guard_flags) return MGC_UNSUPPORTED_CAPABILITY;
    try {
        const auto options_hash = mgr_detail::sha256(meguviz::rife::reference_options);
        if (!zero(d->effective_options_hash.bytes) && hex(d->effective_options_hash) != options_hash) return MGC_ARTIFACT_MISMATCH;
        auto plan = std::make_unique<MgrPlan>();
        mgr_model_retain(model); plan->model = model; plan->desc = *d;
        for (size_t i = 0; i < 32; ++i) plan->desc.effective_options_hash.bytes[i] =
            uint8_t(std::stoul(options_hash.substr(i * 2, 2), nullptr, 16));
        const auto s = meguviz::with_ncnn_domain(model->domain, create_engine, plan.get());
        if (s != MGC_SUCCESS) return s;
        *out = plan.release(); return MGC_SUCCESS;
    } catch (const std::bad_alloc&) { return MGC_OUT_OF_MEMORY; }
    catch (...) { return MGC_INTERNAL_ERROR; }
}
void mgr_plan_retain(MgrPlan* p) { if (p) p->refs.fetch_add(1); }
void mgr_plan_release(MgrPlan* p) { if (p && p->refs.fetch_sub(1) == 1) delete p; }
MgcStatus mgr_session_create(MgrPlan* plan, const MgcSessionDesc* d, MgrSession** out) {
    if (!out) return MGC_INVALID_ARGUMENT; *out = nullptr;
    if (!plan) return MGC_INVALID_ARGUMENT;
    MgcSessionDesc copied{};
    const auto abi = read_desc(d, offsetof(MgcSessionDesc, required_flags) + sizeof(uint32_t), copied);
    if (abi != MGC_SUCCESS) return abi;
    d = &copied;
    try {
        auto session = std::make_unique<MgrSession>();
        const auto s = mgc_session_create(plan->model->domain, d, &session->compute);
        if (s != MGC_SUCCESS) return s;
        mgr_plan_retain(plan); session->plan = plan;
        *out = session.release(); return MGC_SUCCESS;
    } catch (...) { return MGC_OUT_OF_MEMORY; }
}
MgcStatus mgr_session_close(MgrSession* s) {
    if (!s) return MGC_INVALID_ARGUMENT;
    auto status = mgc_session_close(s->compute);
    if (status != MGC_SUCCESS) return status;
    mgr_plan_release(s->plan); delete s; return MGC_SUCCESS;
}
MgcStatus mgr_submit(MgrSession* s, const MgrRequest* d, MgrCompletionFn completion, void* context,
                     MgcContextFn retain_context, MgcContextFn release_context, MgcTicket** ticket) {
    if (!ticket) return MGC_INVALID_ARGUMENT; *ticket = nullptr;
    if (!s) return MGC_INVALID_ARGUMENT;
    MgrRequest copied{};
    auto abi = read_desc(d, offsetof(MgrRequest, required_flags) + sizeof(uint32_t), copied);
    if (abi != MGC_SUCCESS) return abi;
    d = &copied;
    MgrOutputSpec output{};
    abi = read_desc(&d->output, offsetof(MgrOutputSpec, row_stride_bytes) + sizeof(uint64_t), output);
    if (abi != MGC_SUCCESS) return abi;
    copied.output = output;
    if (d->required_flags || d->output.required_flags) return MGC_UNSUPPORTED_ABI;
    if (!completion || !d->left || !d->right || !std::isfinite(d->alpha) || d->alpha < 0 || d->alpha > 1 ||
        bool(retain_context) != bool(release_context) || d->required_flags || !zero(d->reserved) ||
        d->output.required_flags || !zero(d->output.reserved) || d->output.format != MGC_FORMAT_RGB8 ||
        d->output.width != s->plan->desc.width || d->output.height != s->plan->desc.height ||
        d->output.row_stride_bytes < uint64_t(d->output.width)*3 ||
        d->output.row_stride_bytes > (128u*1024u*1024u)/d->output.height) return MGC_INVALID_ARGUMENT;
    MgcLeaseInfo a{}, b{};
    auto status = read_info(d->left, a); if (status != MGC_SUCCESS) return status;
    status = read_info(d->right, b); if (status != MGC_SUCCESS) return status;
    if (a.domain_id != b.domain_id || a.domain_generation != b.domain_generation) return MGC_INCOMPATIBLE_DOMAIN;
    uint64_t domain_id = 0, generation = 0;
    status = mgc_domain_identity(s->plan->model->domain, &domain_id, &generation);
    if (status != MGC_SUCCESS) return status;
    if (a.domain_id != domain_id || a.domain_generation != generation) return MGC_INCOMPATIBLE_DOMAIN;
    if (!image_fits(a, s->plan->desc) || !image_fits(b, s->plan->desc)) return MGC_INVALID_ARGUMENT;
    try {
        auto* job = new Job{};
        job->plan = s->plan; mgr_plan_retain(job->plan);
        job->request = *d; job->callback = completion; job->context = context;
        job->retain_context = retain_context; job->release_context = release_context;
        MgcLease* leases[] = {d->left, d->right};
        MgcHostJobDesc desc{}; desc.header = MGC_HEADER_INIT(MgcHostJobDesc);
        desc.request_id = d->request_id; desc.leases = leases; desc.lease_count = 2;
        desc.execute = execute_job; desc.completion = complete_job; desc.context = job;
        desc.retain_context = retain_job; desc.release_context = release_job;
        status = mgc_private_submit_host_job(s->compute, &desc, ticket);
        drop_caller_job(job); return status;
    } catch (const std::bad_alloc&) { return MGC_OUT_OF_MEMORY; }
    catch (...) { return MGC_INTERNAL_ERROR; }
}
MgcStatus mgr_cancel(MgrSession*, MgcTicket* t) { return mgc_ticket_cancel(t); }
MgcStatus mgr_drain(MgrSession* s, uint64_t ns) { return s ? mgc_session_drain(s->compute, ns) : MGC_INVALID_ARGUMENT; }
MgcStatus mgr_result_describe(MgrResult* r, MgrResultInfo* info) {
    if (!r || !info) return MGC_INVALID_ARGUMENT;
    if (info->header.abi_major != MGC_ABI_MAJOR || info->header.struct_size < offsetof(MgrResultInfo, reserved)) return MGC_UNSUPPORTED_ABI;
    const auto size = info->header.struct_size;
    auto value = r->info; value.header.struct_size = size;
    std::memcpy(info, &value, std::min<size_t>(size, sizeof(value))); return MGC_SUCCESS;
}
void mgr_result_release(MgrResult* r) { delete r; }
}
