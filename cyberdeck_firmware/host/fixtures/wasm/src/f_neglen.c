/* 長さに負の数を渡す。i32 なので渡せてしまうが、m3ApiCheckMem が
 * uint64 に広げて比べるので巨大な長さとしてトラップする。 */
#include "cdk_app.h"

CDK_MAIN int cdk_main(int argc)
{
    (void)argc;
    char buf[8];
    con_write(buf, -1);
    cdk_print("NOT REACHED");
    return 0;
}
