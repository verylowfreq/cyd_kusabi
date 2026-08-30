/* 線形メモリを 2MB 宣言するモジュール。ファームは 1MB (16 ページ) で
 * 断るので、ロードに失敗することを確かめるための fixture。
 *
 * リンク指定で初期メモリを大きくするので、中身は何でもよい。 */
#include "cdk_app.h"

CDK_MAIN int cdk_main(int argc)
{
    (void)argc;
    cdk_print("NOT REACHED");
    return 0;
}
