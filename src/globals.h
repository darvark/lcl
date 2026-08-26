#ifndef GLOBALS_H
#define GLOBALS_H

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "suggestion.h"

extern int last_cq;
extern int last_itu;
extern int cty_update_in_progress;
extern int call_suggestion_available;
extern int call_suggestion_count;
extern int call_suggestion_selected_index;
extern char call_suggestion_matches[CALL_SUGGESTION_MAX][CALL_SUGGESTION_LEN];
extern int app_debug_enabled;

#endif