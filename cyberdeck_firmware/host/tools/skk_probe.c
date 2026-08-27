/* skk_probe — 辞書バイナリを実機と同じコードで引く CLI
 *
 *   skk_probe <dict.skkd> < keys.txt
 *
 * 見出し語は**標準入力から 1 行 1 語**で受ける。argv にしないのは、
 * Windows の main() が argv を ANSI コードページで渡してくるため。
 * 日本語の見出し語がその時点で壊れ、辞書ではなく引数の問題で
 * 「見つからない」が出る。標準入力ならバイト列がそのまま届く。
 *
 * 行が "-a" なら以降は送りありの索引、"-n" で送りなしに戻る。
 *
 * 用途は 2 つ。
 *
 *  1. 実物の SKK-JISYO.L（6.5MB / 17万件）で二分探索が正しいかを
 *     確かめる。fixture の 10 件では並べ直しの誤りが表に出ない。
 *  2. 実機で「引けない」ときに、辞書が悪いのか実機側が悪いのかを
 *     PC 側で切り分ける。
 *
 * 出力は「見出し語 <TAB> 候補列」の 1 行。見つからなければ候補列は空。
 * 別実装（Python で元の辞書を素直に読んだもの）と diff で比べられる形。
 */
#include "cdk_skk_dict.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    FILE *f;
} filectx_t;

static int file_read(void *ctx, uint32_t off, void *buf, uint32_t len)
{
    filectx_t *c = (filectx_t *)ctx;
    if (fseek(c->f, (long)off, SEEK_SET) != 0) {
        return -1;
    }
    return (fread(buf, 1, len, c->f) == len) ? 0 : -1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: skk_probe <dict.skkd> < keys.txt\n");
        return 2;
    }

    filectx_t ctx = {fopen(argv[1], "rb")};
    if (ctx.f == NULL) {
        fprintf(stderr, "skk_probe: %s を開けません\n", argv[1]);
        return 1;
    }

    cdk_skk_dict_t d;
    if (!cdk_skk_dict_open(&d, file_read, &ctx)) {
        fprintf(stderr, "skk_probe: %s は SKK 辞書として読めません\n", argv[1]);
        fclose(ctx.f);
        return 1;
    }
    fprintf(stderr, "skk_probe: 送りあり %u 件 / 送りなし %u 件 / 注釈 %s\n",
            (unsigned)d.ari_count, (unsigned)d.nasi_count,
            d.annotations ? "あり" : "なし");

    bool        ari = false;
    static char line[CDK_SKK_KEY_MAX + 8];
    static char out[CDK_SKK_CAND_MAX];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        if (strcmp(line, "-a") == 0) {
            ari = true;
            continue;
        }
        if (strcmp(line, "-n") == 0) {
            ari = false;
            continue;
        }
        if (!cdk_skk_dict_lookup(&d, line, ari, out, sizeof(out))) {
            out[0] = '\0';
        }
        printf("%s\t%s\n", line, out);
    }

    fclose(ctx.f);
    return 0;
}
