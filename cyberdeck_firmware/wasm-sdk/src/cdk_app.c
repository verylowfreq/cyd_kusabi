/* 自由形式 wasm には libc が無いので、要るものだけここに置く。
 *
 * memcpy / memset は「使わなければ要らない」ものではない。**clang が
 * 構造体のコピーや配列の初期化に対して勝手に呼ぶ**ので、無いとリンクが落ちる。
 *
 * ここは -nostdlib のゲスト側でだけ組む。実機のファームには入らない。
 */
#include "cdk_app.h"

#include <stdarg.h>

/* ---- 最小の string.h ------------------------------------------------- */

void *memcpy(void *d, const void *s, cdk_size_t n)
{
    unsigned char       *p = (unsigned char *)d;
    const unsigned char *q = (const unsigned char *)s;
    while (n--) *p++ = *q++;
    return d;
}

void *memset(void *d, int c, cdk_size_t n)
{
    unsigned char *p = (unsigned char *)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

void *memmove(void *d, const void *s, cdk_size_t n)
{
    unsigned char       *p = (unsigned char *)d;
    const unsigned char *q = (const unsigned char *)s;
    if (p == q || n == 0) return d;
    if (p < q) {
        while (n--) *p++ = *q++;
    } else {
        p += n; q += n;
        while (n--) *--p = *--q;
    }
    return d;
}

int memcmp(const void *a, const void *b, cdk_size_t n)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (n--) {
        if (*x != *y) return (int)*x - (int)*y;
        x++; y++;
    }
    return 0;
}

cdk_size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (cdk_size_t)(p - s);
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* ---- 出力 ------------------------------------------------------------ */

int cdk_print(const char *s)
{
    if (!s) return CDK_E_INVAL;
    return con_write(s, (int)strlen(s));
}

/* 出力は 1 回の con_write にまとめる。細切れに呼ぶとリングの取り合いで
 * 行が混ざって見える。 */
#define PRINTF_BUF 256

static void put_u(char *buf, int cap, int *n, unsigned long v, unsigned base, int upper)
{
    char        tmp[24];
    int         t = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) tmp[t++] = '0';
    while (v) { tmp[t++] = digits[v % base]; v /= base; }
    while (t > 0 && *n < cap) buf[(*n)++] = tmp[--t];
}

int cdk_printf(const char *fmt, ...)
{
    if (!fmt) return CDK_E_INVAL;

    char    buf[PRINTF_BUF];
    int     n = 0;
    va_list ap;
    va_start(ap, fmt);

    for (const char *p = fmt; *p && n < (int)sizeof buf; p++) {
        if (*p != '%') { buf[n++] = *p; continue; }
        p++;
        switch (*p) {
        case 'd': {
            long v = va_arg(ap, int);
            if (v < 0) {
                if (n < (int)sizeof buf) buf[n++] = '-';
                put_u(buf, (int)sizeof buf, &n, (unsigned long)(-v), 10, 0);
            } else {
                put_u(buf, (int)sizeof buf, &n, (unsigned long)v, 10, 0);
            }
            break;
        }
        case 'u': put_u(buf, (int)sizeof buf, &n, va_arg(ap, unsigned), 10, 0); break;
        case 'x': put_u(buf, (int)sizeof buf, &n, va_arg(ap, unsigned), 16, 0); break;
        case 'X': put_u(buf, (int)sizeof buf, &n, va_arg(ap, unsigned), 16, 1); break;
        case 'c': buf[n++] = (char)va_arg(ap, int); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s && n < (int)sizeof buf) buf[n++] = *s++;
            break;
        }
        case '%': buf[n++] = '%'; break;
        case '\0': p--; break;   /* 末尾の % は捨てる */
        default:
            /* 知らない指定はそのまま出す。黙って消すより気づける。 */
            if (n < (int)sizeof buf) buf[n++] = '%';
            if (n < (int)sizeof buf) buf[n++] = *p;
            break;
        }
    }
    va_end(ap);
    return con_write(buf, n);
}

/* ---- 確保 ------------------------------------------------------------
 *
 * 解放しないバンプ確保。線形メモリの、リンカが使い終わった先から取る。
 * アプリは短命なので、これで足りるうちは足りる。足りなくなったら NULL。
 */
extern unsigned char __heap_base;

static cdk_size_t s_bump = 0;

void *cdk_alloc(cdk_size_t n)
{
    if (n == 0) return 0;
    n = (n + 7u) & ~(cdk_size_t)7u; /* 8 バイト境界 */

    unsigned char *base = &__heap_base;
    /* wasm の線形メモリの終わりまで。__builtin_wasm_memory_size はページ数。
     * wasm32 ではアドレスがそのままオフセットなので、整数に落として比べる。 */
    const cdk_size_t total = (cdk_size_t)__builtin_wasm_memory_size(0) * 65536u;
    const cdk_size_t here  = (cdk_size_t)(__UINTPTR_TYPE__)base + s_bump;
    if (here > total || n > total - here) return 0;

    void *p = base + s_bump;
    s_bump += n;
    return p;
}
