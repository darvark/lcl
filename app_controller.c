#include "app_controller.h"

#include "cat.h"
#include "contest.h"
#include "config.h"
#include "db.h"
#include "cty.h"
#include "dxcluster.h"
#include "export.h"
#include "globals.h"
#include "qso.h"
#include "qtc.h"
#include "suggestion.h"
#include "stats.h"

#include <errno.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>

static char input_buffer[256];
static int input_len = 0;

enum {
  ENTRY_FIELD_CALL = 0,
  ENTRY_FIELD_RST = 1,
  ENTRY_FIELD_COMMENTS = 2,
  ENTRY_FIELD_COUNT = 3
};

static char entry_call_by_radio[2][32];
static char entry_rst_by_radio[2][8];
static char entry_comments_by_radio[2][128];
static int active_entry_field_by_radio[2] = {ENTRY_FIELD_CALL, ENTRY_FIELD_CALL};

static char status_text[256] = "Ready";
static char dxcc_text[128] = "";
static char info_text[128] = "";
static char display_info[128] = "";

int app_debug_enabled = 0;

int last_cq = 0;
int last_itu = 0;
int cty_update_in_progress = 0;
int call_suggestion_available = 0;
int call_suggestion_count = 0;
int call_suggestion_selected_index = 0;
char call_suggestion_matches[CALL_SUGGESTION_MAX][CALL_SUGGESTION_LEN] = {{0}};

#define MAX_CALL_HISTORY 20000

static char call_history[MAX_CALL_HISTORY][32];
static int call_history_count = 0;
static CallSuggestionList call_suggestions;

static bool cluster_view = true;
static int cluster_scroll = 0;
static bool export_prompt_mode = false;
static int manual_entry_freq_khz[2] = {14074, 7020};
static int active_radio_nr = 1;
static int radio_run_state[2] = {1, 0};
static ContestDefinition active_contest_def;
static int contest_definition_loaded = 0;
static char contest_exchange_label_text[64] = "EXCH";

#define NAMED_LOG_LIST_MAX 12

static void sync_callsign_suggestion_state(void);

static void update_composed_input_line(void);
static void refresh_callsign_suggestion(const char *input);
static void update_dxcc_from_input(const char *input);
static int is_digits_only(const char *s);
static int process_command(const char *cmd);

#include "app_controller_core_logic.inc"
#include "app_controller_radio_and_commands.inc"
#include "app_controller_runtime.inc"
#include "app_controller_qtc.inc"
