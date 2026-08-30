/* sys_exit で抜ける。戻り値が拾えること。 */
#include "cdk_app.h"

CDK_MAIN int cdk_main(int argc)
{
    (void)argc;
    cdk_print("bye");
    sys_exit(42);
    cdk_print("NOT REACHED");
    return 0;
}
