/* 大量に出力する。出力リング (4KB) をアプリが追い越す状況を実機で作る。
 *
 * 捨てる実装だと途中の行が黙って消える。待つ実装なら全部出る。
 * 最後に行数を出すので、数えれば落ちていないか分かる。 */
#include "cdk_app.h"

CDK_MAIN int cdk_main(int argc)
{
    (void)argc;
    const int lines = 200;
    for (int i = 1; i <= lines; i++) {
        cdk_printf("%d ", i);
    }
    cdk_printf("\r\n--- %d 行 ---\r\n", lines);
    return 0;
}
