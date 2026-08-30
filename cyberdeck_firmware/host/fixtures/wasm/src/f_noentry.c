/* cdk_main を出さないモジュール。「読めるが走らせられない」側の確認。 */
#include "cdk_app.h"

__attribute__((export_name("not_main"))) int not_main(void)
{
    cdk_print("x");
    return 0;
}
