#include <meguviz/rife.h>
#include <stdio.h>
#include <string.h>
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr,"failed at line %d\n",__LINE__); return 1; } } while (0)
int main(void) {
    MgrModel* model = (MgrModel*)1;
    MgrModelDesc desc;
    memset(&desc,0,sizeof(desc));
    desc.header = (MgcHeader)MGC_HEADER_INIT(MgrModelDesc);
    CHECK(mgr_model_create(0,&desc,&model) == MGC_INVALID_ARGUMENT && model == 0);
    model = (MgrModel*)1;
    desc.header.abi_major = 99;
    CHECK(mgr_model_create(0,&desc,&model) == MGC_UNSUPPORTED_ABI && model == 0);
    CHECK(mgr_plan_create(0,0,0) == MGC_INVALID_ARGUMENT);
    CHECK(mgr_session_create(0,0,0) == MGC_INVALID_ARGUMENT);
    CHECK(mgr_drain(0,0) == MGC_INVALID_ARGUMENT);
    CHECK(mgr_session_close(0) == MGC_INVALID_ARGUMENT);
    mgr_model_release(0); mgr_plan_release(0); mgr_result_release(0);
    puts("{\"result\":\"PASS\",\"scope\":\"pure C ABI rejection and null ownership\"}");
    return 0;
}
