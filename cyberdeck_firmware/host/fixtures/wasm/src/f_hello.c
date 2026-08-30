/* いちばん基本の経路: con_write と cdk_main の戻り値。 */
#include "cdk_app.h"

CDK_MAIN int cdk_main(int argc)
{
    cdk_print("hello");
    return argc + 7;
}
