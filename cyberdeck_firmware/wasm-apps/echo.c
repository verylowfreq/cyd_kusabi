/* 打った文字をそのまま返す。Ctrl-C で終わる。
 * con_read は待たないので、暇なときは sys_delay で譲る。 */
#include "cdk_app.h"

CDK_MAIN int cdk_main(int argc)
{
    (void)argc;
    cdk_print("打つとそのまま返します。Ctrl-C で終わり。\r\n");

    for (;;) {
        char      buf[64];
        const int n = con_read(buf, (int)sizeof buf);
        if (n > 0) {
            con_write(buf, n);
        } else {
            /* Ctrl-C はここで効く。戻ってこない。 */
            sys_delay(20);
        }
    }
}
