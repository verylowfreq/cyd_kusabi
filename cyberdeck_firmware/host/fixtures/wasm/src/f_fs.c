/* ファイル。サンドボックスの外へ出ようとする分も含む。 */
#include "cdk_app.h"

CDK_MAIN int cdk_main(int argc)
{
    (void)argc;

    int fd = fs_open(CDK_S("note.txt"), CDK_OPEN_WRITE);
    cdk_printf("open=%d", fd);
    if (fd >= 0) {
        cdk_printf(" wrote=%d", fs_write(fd, CDK_S("abcdef")));
        cdk_printf(" close=%d", fs_close(fd));
    }

    fd = fs_open(CDK_S("note.txt"), CDK_OPEN_READ);
    if (fd >= 0) {
        char buf[16];
        const int n = fs_read(fd, buf, (int)sizeof buf - 1);
        buf[n > 0 ? n : 0] = 0;
        cdk_printf(" read=%d:%s", n, buf);
        fs_close(fd);
    }

    /* 外へは出られないこと。どちらも CDK_E_PERM (-2) のはず。 */
    cdk_printf(" esc1=%d", fs_open(CDK_S("../secret"), CDK_OPEN_READ));
    cdk_printf(" esc2=%d", fs_open(CDK_S("/etc/passwd"), CDK_OPEN_READ));

    /* 4 本までしか開けないこと。5 本目は CDK_E_NOMEM (-5)。 */
    int held[5];
    for (int i = 0; i < 5; i++) held[i] = fs_open(CDK_S("note.txt"), CDK_OPEN_READ);
    cdk_printf(" fifth=%d", held[4]);
    for (int i = 0; i < 5; i++) if (held[i] >= 0) fs_close(held[i]);

    return 0;
}
