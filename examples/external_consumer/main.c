#include <meguviz/rife.h>
#include <stdio.h>
int main(void) {
    MgrModel *model = 0;
    MgrModelDesc desc = {0};
    desc.header = (MgcHeader)MGC_HEADER_INIT(MgrModelDesc);
    if (mgr_model_create(0, &desc, &model) != MGC_INVALID_ARGUMENT || model) return 1;
    puts("installed RIFE C ABI linked; invalid model correctly rejected");
    return 0;
}
