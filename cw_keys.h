#ifndef CW_KEYS_H
#define CW_KEYS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CW_KEY_COUNT 10

typedef struct {
  char run[CW_KEY_COUNT][128];
  char sp[CW_KEY_COUNT][128];
} CwKeyMap;

/* Load RUN and S&P sections from an INI file into map. Returns 0 or -1. */
int cw_keys_load(CwKeyMap *map, const char *filename);

/* Expand {MYCALL}, {RST}, {EXCH} placeholders in tpl into out. */
void cw_keys_expand(const char *tpl, const char *mycall, const char *rst,
                    const char *exch, char *out, size_t out_size);

/* Return the raw template for fn_nr (1-based) in RUN (is_run=1) or S&P. */
const char *cw_keys_get(const CwKeyMap *map, int fn_nr, int is_run);

#ifdef __cplusplus
}
#endif

#endif
