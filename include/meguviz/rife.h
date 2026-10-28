#ifndef MEGUVIZ_RIFE_H
#define MEGUVIZ_RIFE_H
#include <meguviz/compute.h>

#ifdef __cplusplus
extern "C" {
#endif
#define MGR_API MGC_API
#define MGR_ABI_MAJOR 0u
#define MGR_ABI_MINOR 1u
typedef struct MgrModel MgrModel;
typedef struct MgrPlan MgrPlan;
typedef struct MgrSession MgrSession;
typedef struct MgrResult MgrResult;
enum { MGR_RESULT_INFERRED = 1, MGR_RESULT_ENDPOINT = 2, MGR_RESULT_GUARD_BYPASS = 3 };
enum { MGR_PRECISION_FP32_REFERENCE = 1, MGR_PRECISION_LEGACY_FP16 = 2 };
enum { MGR_GUARD_STATIC_DETAIL = 1, MGR_GUARD_MOTION = 2 };
typedef struct MgrModelDesc {
    MgcHeader header;
    const uint8_t *manifest_utf8;
    uint64_t manifest_size;
    const uint8_t *graph_data;
    uint64_t graph_size;
    const uint8_t *weights_data;
    uint64_t weights_size;
    MgcHash graph_sha256;
    MgcHash weights_sha256;
    MgcHash private_sdk_fingerprint;
    uint32_t required_flags;
    uint32_t reserved[3];
} MgrModelDesc;
typedef struct MgrPlanDesc {
    MgcHeader header;
    uint32_t width;
    uint32_t height;
    uint32_t input_format;
    uint32_t precision_profile;
    uint32_t flow_scale;
    uint32_t guard_flags;
    MgcHash effective_options_hash;
    uint32_t required_flags;
    uint32_t reserved[3];
} MgrPlanDesc;
typedef struct MgrOutputSpec {
    MgcHeader header;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t required_flags;
    uint64_t row_stride_bytes;
    uint32_t reserved[4];
} MgrOutputSpec;
typedef struct MgrRequest {
    MgcHeader header;
    uint64_t request_id;
    MgcLease *left;
    MgcLease *right;
    double alpha;
    MgrOutputSpec output;
    uint32_t required_flags;
    uint32_t reserved[3];
} MgrRequest;
typedef struct MgrResultInfo {
    MgcHeader header;
    uint64_t request_id;
    MgcLease *output;
    uint32_t frame_kind;
    uint32_t algorithm_accepted;
    uint32_t guard_outcome;
    uint32_t reserved[3];
} MgrResultInfo;
typedef void (*MgrCompletionFn)(void *context, uint64_t request_id, MgcStatus status, MgrResult *owned_result);
/* create validates hashes/sizes before parsing and copies all bytes; no network
 * or implicit model download. A model and plan are immutable after success. */
MGR_API MgcStatus mgr_model_create(MgcDomain *domain, const MgrModelDesc *desc, MgrModel **model);
MGR_API void mgr_model_retain(MgrModel *model);
MGR_API void mgr_model_release(MgrModel *model);
MGR_API MgcStatus mgr_plan_create(MgrModel *model, const MgrPlanDesc *desc, MgrPlan **plan);
MGR_API void mgr_plan_retain(MgrPlan *plan);
MGR_API void mgr_plan_release(MgrPlan *plan);
MGR_API MgcStatus mgr_session_create(MgrPlan *plan, const MgcSessionDesc *desc, MgrSession **session);
MGR_API MgcStatus mgr_session_close(MgrSession *session);
/* ACCEPTED takes one submission reference to each input; rejection takes none.
 * Only internal pool allocation is supported: no external output write target.
 * alpha is finite [0,1]; endpoints are ENDPOINT rather than inferred work.
 * callback follows Compute context/worker/retirement rules. Result ownership
 * transfers to callback and survives session drain/close until result_release. */
MGR_API MgcStatus mgr_submit(MgrSession *session, const MgrRequest *request, MgrCompletionFn completion,
                     void *context, MgcContextFn retain_context, MgcContextFn release_context,
                     MgcTicket **ticket);
MGR_API MgcStatus mgr_cancel(MgrSession *session, MgcTicket *ticket);
MGR_API MgcStatus mgr_drain(MgrSession *session, uint64_t timeout_ns);
MGR_API MgcStatus mgr_result_describe(MgrResult *result, MgrResultInfo *info);
MGR_API void mgr_result_release(MgrResult *result);
#ifdef __cplusplus
}
#endif
#endif
