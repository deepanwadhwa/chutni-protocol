/* Minimal JSON document model.
 *
 * A document model rather than a value extractor because §9.1 requires that
 * unknown manifest fields survive a rewrite: we parse the whole tree, edit the
 * fields we understand, and serialize everything back, including keys this
 * implementation has never heard of. Object key order is preserved.
 */
#ifndef CHUTNI_CJ_H
#define CHUTNI_CJ_H

#include <stddef.h>

typedef enum { CJ_NULL, CJ_BOOL, CJ_NUM, CJ_STR, CJ_ARR, CJ_OBJ } cj_type;

typedef struct cj cj;
struct cj {
    cj_type type;
    int     bval;      /* CJ_BOOL */
    double  num;       /* CJ_NUM */
    char   *raw;       /* CJ_NUM: original lexeme, so 1.0 does not become 1 */
    char   *str;       /* CJ_STR */
    cj    **items;     /* CJ_ARR elements, or CJ_OBJ values */
    char  **keys;      /* CJ_OBJ keys, parallel to items */
    size_t  n, cap;
};

cj  *cj_parse(const char *text, const char **err);
void cj_free(cj *v);

cj  *cj_null(void);
cj  *cj_bool(int b);
cj  *cj_num(double d);
cj  *cj_str(const char *s);
cj  *cj_arr(void);
cj  *cj_obj(void);

/* Borrowed reference into the tree, or NULL. */
cj  *cj_get(const cj *obj, const char *key);
const char *cj_get_str(const cj *obj, const char *key);

/* Takes ownership of val. Replaces an existing key in place, keeping its
 * position; otherwise appends. Returns 0 on allocation failure. */
int  cj_set(cj *obj, const char *key, cj *val);
int  cj_push(cj *arr, cj *val);

/* Serialized form; caller frees. indent < 0 emits compact output. */
char *cj_dump(const cj *v, int indent);

#endif /* CHUTNI_CJ_H */
