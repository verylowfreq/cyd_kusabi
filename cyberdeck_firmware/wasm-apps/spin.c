/* Ctrl-C の検証用。native を呼びながら回り続ける。
 *
 * native を一度も呼ばない while(1); は wasm3 の構造上止められない
 * （DESIGN.md §15）。こちらは sys_delay を呼ぶので止まる。 */
#include "cdk_app.h"

CDK_MAIN int cdk_main(int argc)
{
    (void)argc;
    cdk_print("回り続けます。Ctrl-C で止めてください。\r\n");
    for (unsigned i = 0;; i++) {
        cdk_printf("\r%u", i);
        sys_delay(100);
    }
}
