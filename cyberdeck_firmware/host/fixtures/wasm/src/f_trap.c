/* 線形メモリの外を指して con_write を呼ぶ。境界検査が効けばトラップする。
 * 効かなければ 2 行目の "NOT REACHED" が出るので、抜けたことが分かる。 */
#include "cdk_app.h"

CDK_MAIN int cdk_main(int argc)
{
    (void)argc;
    con_write((const void *)(unsigned)0x7fffff00u, 64);
    cdk_print("NOT REACHED");
    return 0;
}
