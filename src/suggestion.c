#include "suggestion.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/*
 * Check whether a text buffer starts with a prefix.
 *
 * @param text Text to inspect.
 * @param prefix Prefix to compare against.
 * @param prefix_len Number of prefix characters to compare.
 * @return 1 if the prefix matches, otherwise 0.
 */
static int starts_with(const char *text, const char *prefix, size_t prefix_len) {
  return strncmp(text, prefix, prefix_len) == 0;
}

/*
 * Check whether a suggestion list already contains a callsign.
 *
 * @param list Candidate suggestion list.
 * @param call Callsign to search for.
 * @return 1 if the callsign exists, otherwise 0.
 */
static int contains_match(const CallSuggestionList *list, const char *call) {
  if (!list || !call)
    return 0;

  for (int i = 0; i < list->count; i++) {
    if (strcmp(list->matches[i], call) == 0)
      return 1;
  }

  return 0;
}

/*
 * Extract the first token from the input buffer and normalize it to upper case.
 *
 * @param input Raw input text.
 * @param out Destination buffer for the extracted token.
 * @param out_size Size of the destination buffer.
 * @param token_len Optional output token length.
 * @return 1 if a token was extracted, otherwise 0.
 */
static int extract_prefix_token(const char *input, char *out, size_t out_size,
                                size_t *token_len) {
  if (!input || !out || out_size < 2)
    return 0;

  out[0] = 0;
  if (token_len)
    *token_len = 0;

  size_t len = strlen(input);
  size_t start = 0;

  while (start < len && isspace((unsigned char)input[start]))
    start++;

  if (start >= len)
    return 0;

  size_t end = start;
  while (end < len && !isspace((unsigned char)input[end]) && input[end] != ';')
    end++;

  if (end < len && isspace((unsigned char)input[end]))
    return 0;

  size_t n = end - start;
  if (n < 1 || n >= out_size)
    return 0;

  for (size_t i = 0; i < n; i++)
    out[i] = (char)toupper((unsigned char)input[start + i]);

  out[n] = 0;

  if (token_len)
    *token_len = n;

  return 1;
}

/*
 * Reset a suggestion list to its empty state.
 *
 * @param list List to clear.
 * @return Nothing.
 */
void call_suggestion_list_clear(CallSuggestionList *list) {
  if (!list)
    return;

  memset(list, 0, sizeof(*list));
}

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
                             int history_count) {
  if (!list)
    return;

  call_suggestion_list_clear(list);

  if (!input || !history || history_count <= 0)
    return;

  char prefix[CALL_SUGGESTION_LEN] = {0};
  size_t prefix_len = 0;

  if (!extract_prefix_token(input, prefix, sizeof(prefix), &prefix_len))
    return;

  for (int i = history_count - 1; i >= 0; i--) {
    if (!history[i][0])
      continue;

    if (!starts_with(history[i], prefix, prefix_len))
      continue;

    if (strcmp(history[i], prefix) == 0)
      continue;

    if (contains_match(list, history[i]))
      continue;

    snprintf(list->matches[list->count], CALL_SUGGESTION_LEN, "%s", history[i]);
    list->count++;

    if (list->count >= CALL_SUGGESTION_MAX)
      break;
  }

  list->selected = 0;
}

/*
 * Return the currently selected suggestion string.
 *
 * @param list Current suggestion list.
 * @return Selected suggestion, or NULL if none is available.
 */
const char *call_suggestion_selected(const CallSuggestionList *list) {
  if (!list || list->count <= 0)
    return NULL;

  if (list->selected < 0 || list->selected >= list->count)
    return NULL;

  return list->matches[list->selected];
}

/*
 * Move the selection to the previous suggestion.
 *
 * @param list Current suggestion list.
 * @return Nothing.
 */
void call_suggestion_select_prev(CallSuggestionList *list) {
  if (!list || list->count <= 0)
    return;

  if (list->selected <= 0)
    list->selected = list->count - 1;
  else
    list->selected--;
}

/*
 * Move the selection to the next suggestion.
 *
 * @param list Current suggestion list.
 * @return Nothing.
 */
void call_suggestion_select_next(CallSuggestionList *list) {
  if (!list || list->count <= 0)
    return;

  list->selected++;
  if (list->selected >= list->count)
    list->selected = 0;
}

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
                          size_t input_capacity) {
  if (!list || !input || !len || input_capacity < 2)
    return 0;

  const char *selected = call_suggestion_selected(list);
  if (!selected)
    return 0;

  size_t in_len = strlen(input);
  size_t start = 0;

  while (start < in_len && isspace((unsigned char)input[start]))
    start++;

  size_t end = start;
  while (end < in_len && !isspace((unsigned char)input[end]) && input[end] != ';')
    end++;

  size_t selected_len = strlen(selected);
  size_t suffix_len = in_len - end;

  if (start + selected_len + suffix_len >= input_capacity)
    return 0;

  memmove(input + start + selected_len, input + end, suffix_len + 1);
  memcpy(input + start, selected, selected_len);

  *len = (int)strlen(input);

  return 1;
}
