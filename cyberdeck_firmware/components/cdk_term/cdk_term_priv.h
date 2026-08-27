/* cdk_term の内部インターフェース。cdk_term.c と cdk_vt.c で共有する。 */
#ifndef CDK_TERM_PRIV_H
#define CDK_TERM_PRIV_H

#include "cdk_term.h"

void cdk__mark(cdk_term_t *t, int y);
void cdk__mark_range(cdk_term_t *t, int y0, int y1);

cdk_cell_t cdk__blank(const cdk_term_t *t);
void cdk__fill(cdk_term_t *t, int x0, int y0, int x1, int y1);

/* 全角ペアの片割れを潰さないように、そのセルと相方をまとめて空白にする */
void cdk__clear_cell(cdk_term_t *t, int x, int y);

void cdk__scroll_up(cdk_term_t *t, int n);
void cdk__scroll_down(cdk_term_t *t, int n);

void cdk__index(cdk_term_t *t);         /* LF 相当 */
void cdk__reverse_index(cdk_term_t *t); /* RI 相当 */

void cdk__move_to(cdk_term_t *t, int x, int y);
void cdk__put_char(cdk_term_t *t, uint32_t cp);

void cdk__insert_chars(cdk_term_t *t, int n);
void cdk__delete_chars(cdk_term_t *t, int n);
void cdk__erase_chars(cdk_term_t *t, int n);
void cdk__insert_lines(cdk_term_t *t, int n);
void cdk__delete_lines(cdk_term_t *t, int n);

void cdk__switch_screen(cdk_term_t *t, bool to_alt);
void cdk__reply(cdk_term_t *t, const char *s);

/* パーサ。cdk_vt.c */
void cdk__vt_byte(cdk_term_t *t, uint8_t b);

#endif /* CDK_TERM_PRIV_H */
