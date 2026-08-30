/* いちばん短いユーザーアプリ。 */
#include "cdk_app.h"

CDK_MAIN int cdk_main(int argc)
{
    cdk_print("こんにちは、wasm から\r\n");

    int cols = 0, rows = 0;
    con_size(&cols, &rows);
    cdk_printf("端末は %d桁 x %d行\r\n", cols, rows);

    for (int i = 0; i < argc; i++) {
        char a[64];
        const int n = sys_arg(i, a, (int)sizeof a);
        if (n >= 0) cdk_printf("引数[%d] = %s\r\n", i, a);
    }
    return 0;
}
