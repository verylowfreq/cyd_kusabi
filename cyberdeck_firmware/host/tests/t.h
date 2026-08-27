/* 最小のテストヘルパ。外部依存を作らないためにこれで足りる範囲に留める。 */
#ifndef CDK_TEST_H
#define CDK_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int t_failures = 0;
static int t_checks = 0;
static const char *t_current = "";

#define T_CASE(name)                                                           \
    do {                                                                       \
        t_current = (name);                                                    \
        printf("-- %s\n", t_current);                                          \
        /* 落ちたときにどこまで進んだか分かるように毎回流す */                 \
        fflush(stdout);                                                        \
    } while (0)

#define T_CHECK(cond)                                                          \
    do {                                                                       \
        t_checks++;                                                            \
        if (!(cond)) {                                                         \
            t_failures++;                                                      \
            printf("   FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, t_current,   \
                   #cond);                                                     \
        }                                                                      \
    } while (0)

#define T_EQ_INT(a, b)                                                         \
    do {                                                                       \
        t_checks++;                                                            \
        long long _a = (long long)(a);                                         \
        long long _b = (long long)(b);                                         \
        if (_a != _b) {                                                        \
            t_failures++;                                                      \
            printf("   FAIL %s:%d [%s] %s == %s : %lld != %lld\n", __FILE__,   \
                   __LINE__, t_current, #a, #b, _a, _b);                       \
        }                                                                      \
    } while (0)

#define T_EQ_U32(a, b)                                                         \
    do {                                                                       \
        t_checks++;                                                            \
        unsigned long _a = (unsigned long)(a);                                 \
        unsigned long _b = (unsigned long)(b);                                 \
        if (_a != _b) {                                                        \
            t_failures++;                                                      \
            printf("   FAIL %s:%d [%s] %s == %s : 0x%08lX != 0x%08lX\n",       \
                   __FILE__, __LINE__, t_current, #a, #b, _a, _b);             \
        }                                                                      \
    } while (0)

/* 文字列は中身を出さないと直せない。UTF-8 のまま表示する。 */
#define T_EQ_STR(a, b)                                                         \
    do {                                                                       \
        t_checks++;                                                            \
        const char *_a = (a);                                                  \
        const char *_b = (b);                                                  \
        if (_a == NULL || _b == NULL || strcmp(_a, _b) != 0) {                 \
            t_failures++;                                                      \
            printf("   FAIL %s:%d [%s] %s == %s\n        got      \"%s\"\n"    \
                   "        expected \"%s\"\n",                                \
                   __FILE__, __LINE__, t_current, #a, #b,                      \
                   (_a != NULL) ? _a : "(null)",                               \
                   (_b != NULL) ? _b : "(null)");                              \
        }                                                                      \
    } while (0)

#define T_EQ_MEM(a, b, n)                                                      \
    do {                                                                       \
        t_checks++;                                                            \
        if (memcmp((a), (b), (size_t)(n)) != 0) {                              \
            t_failures++;                                                      \
            printf("   FAIL %s:%d [%s] memcmp(%s, %s, %s)\n", __FILE__,        \
                   __LINE__, t_current, #a, #b, #n);                           \
        }                                                                      \
    } while (0)

static int t_report(const char *suite)
{
    printf("%s: %d checks, %d failures\n", suite, t_checks, t_failures);
    return t_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

#endif /* CDK_TEST_H */
