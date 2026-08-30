/* ゲストのパスをサンドボックスへ閉じ込める。
 *
 * ここが破れるとアプリが SD の外（NVS の設定を書いたファイル、known_hosts、
 * 秘密鍵）に手が届く。**正規化してから判定する**のが要点で、
 * 文字列の先頭を見るだけだと "/sdcard/../etc" が通ってしまう。
 */
#include "cdk_wasm.h"

#include <string.h>

/* 正規化した結果が置ける最大の要素数。"/a/a/a/..." が最悪。 */
#define MAX_PARTS (CDK_WASM_PATH_MAX / 2 + 1)

int cdk_wasm_path_resolve(const char *in, int in_len, char *out, int out_cap)
{
    if (!in || !out || out_cap < CDK_WASM_ROOT_LEN + 1) return CDK_E_INVAL;
    if (in_len <= 0) return CDK_E_INVAL;

    /* ゲストのメモリは (ptr,len) で来る。途中の NUL は受け付けない。 */
    if (memchr(in, '\0', (size_t)in_len) != NULL) return CDK_E_INVAL;

    /* 絶対パスに直す。相対は root からの相対とみなす。 */
    char work[CDK_WASM_PATH_MAX];
    int  wlen = 0;
    if (in[0] != '/') {
        if (CDK_WASM_ROOT_LEN + 1 + in_len >= (int)sizeof work) return CDK_E_INVAL;
        memcpy(work, CDK_WASM_ROOT, CDK_WASM_ROOT_LEN);
        wlen = CDK_WASM_ROOT_LEN;
        work[wlen++] = '/';
    } else if (in_len >= (int)sizeof work) {
        return CDK_E_INVAL;
    }
    memcpy(work + wlen, in, (size_t)in_len);
    wlen += in_len;
    work[wlen] = '\0';

    /* "/" 区切りで畳む。"." は捨て、".." は 1 つ戻す。 */
    const char *part[MAX_PARTS];
    int         plen[MAX_PARTS];
    int         nparts = 0;

    int i = 0;
    while (i < wlen) {
        while (i < wlen && work[i] == '/') i++;
        int start = i;
        while (i < wlen && work[i] != '/') i++;
        const int len = i - start;
        if (len == 0) continue;
        if (len == 1 && work[start] == '.') continue;
        if (len == 2 && work[start] == '.' && work[start + 1] == '.') {
            /* root より上へ出ようとしたら、そこで禁止。
             * 黙って root に留めると、アプリ側は成功したつもりで
             * 別のファイルを触ることになる。 */
            if (nparts == 0) return CDK_E_PERM;
            nparts--;
            continue;
        }
        if (nparts >= MAX_PARTS) return CDK_E_INVAL;
        part[nparts] = work + start;
        plen[nparts] = len;
        nparts++;
    }

    /* 組み直す。 */
    int n = 0;
    if (nparts == 0) {
        /* "/" そのもの。root の外なので禁止。 */
        return CDK_E_PERM;
    }
    for (int k = 0; k < nparts; k++) {
        if (n + 1 + plen[k] >= out_cap) return CDK_E_INVAL;
        out[n++] = '/';
        memcpy(out + n, part[k], (size_t)plen[k]);
        n += plen[k];
    }
    out[n] = '\0';

    /* 畳んだ結果が root の下にあるか。root そのものは許す。 */
    if (strncmp(out, CDK_WASM_ROOT, CDK_WASM_ROOT_LEN) != 0) return CDK_E_PERM;
    if (out[CDK_WASM_ROOT_LEN] != '\0' && out[CDK_WASM_ROOT_LEN] != '/') return CDK_E_PERM;

    return CDK_E_OK;
}
