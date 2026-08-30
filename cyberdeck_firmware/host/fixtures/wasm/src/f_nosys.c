/* 予約したまま未実装の gfx_* は CDK_E_NOSYS (-7) を返す。 */
#include "cdk_app.h"

CDK_MAIN int cdk_main(int argc)
{
    (void)argc;
    cdk_printf("present=%d clear=%d", gfx_present(), gfx_clear(0));
    return 0;
}
