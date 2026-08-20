#ifndef SUGGESTION_H
#define SUGGESTION_H

#include <stddef.h>

#define CALL_SUGGESTION_MAX 12
#define CALL_SUGGESTION_LEN 32

typedef struct {
  char matches[CALL_SUGGESTION_MAX][CALL_SUGGESTION_LEN];
  int count;
  int selected;
} CallSuggestionList;

/*
 * Reset a suggestion list to its empty state.
 *
 * @param list List to clear.
 * @return Nothing.
 */
void call_suggestion_list_clear(CallSuggestionList *list);

/*
 * Rebuild suggestion matches from history for the current input token.
 *
 * @param list List to populate with matches.
 * @param input Current input buffer.
 * @param history History buffer to search.
 * @param history_count Number of history entries.
 * @return Nothing.
 */
void call_suggestion_refresh(CallSuggestionList *list, const char *input,
                             char history[][CALL_SUGGESTION_LEN],
                             int history_count);

/*
 * Apply the selected suggestion into an input buffer.
 *
 * @param list Current suggestion list.
 * @param input Input buffer to modify.
 * @param len Current input length, updated on success.
 * @param input_capacity Capacity of the input buffer.
 * @return 1 if a suggestion was applied, otherwise 0.
 */
int call_suggestion_apply(const CallSuggestionList *list, char *input, int *len,
                          size_t input_capacity);

/*
 * Return the currently selected suggestion string.
 *
 * @param list Current suggestion list.
 * @return Selected suggestion, or NULL if none is available.
 */
const char *call_suggestion_selected(const CallSuggestionList *list);

/*
 * Move the selection to the previous suggestion.
 *
 * @param list Current suggestion list.
 * @return Nothing.
 */
void call_suggestion_select_prev(CallSuggestionList *list);

/*
 * Move the selection to the next suggestion.
 *
 * @param list Current suggestion list.
 * @return Nothing.
 */
void call_suggestion_select_next(CallSuggestionList *list);

#endif
