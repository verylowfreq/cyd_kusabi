/* 引数の複製が効いているか。シェルのスタックを指したままだと化ける。 */
#include "cdk_app.h"

CDK_MAIN int cdk_main(int argc)
{
    cdk_printf("argc=%d", argc);
    for (int i = 0; i < argc; i++) {
        char a[32];
        const int n = sys_arg(i, a, (int)sizeof a);
        cdk_printf(" [%d]=%s(%d)", i, a, n);
    }
    return 0;
}
