#ifndef APP_CONTROLLER_H
#define APP_CONTROLLER_H

#include <stddef.h>
#include <stdbool.h>

#include "contest.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
  APP_KEY_NONE = -1,
  APP_KEY_RESIZE = -2,
  APP_KEY_F1 = -3,
  APP_KEY_F2 = -4,
  APP_KEY_F3 = -5,
  APP_KEY_F4 = -6,
  APP_KEY_F5 = -7,
  APP_KEY_F6 = -8,
  APP_KEY_F7 = -9,
  APP_KEY_F10 = -11,
  APP_KEY_UP = -12,
  APP_KEY_DOWN = -13,
  APP_KEY_PAGE_UP = -14,
  APP_KEY_PAGE_DOWN = -15,
  APP_KEY_BACKSPACE = -16,
  APP_KEY_ENTER = -17,
  APP_KEY_TAB = -18,
  APP_KEY_ESC = -19,
  APP_KEY_LEFT = -20,
  APP_KEY_RIGHT = -21,
  APP_KEY_ALT_W = -22,
  APP_KEY_SPACE = ' '
};

typedef enum {
  APP_CTRL_EVENT_NONE = 0,
  APP_CTRL_EVENT_REQUEST_CTY_UPDATE,
  APP_CTRL_EVENT_EXIT,
} AppControllerEvent;

typedef struct {
  const char *input;
  const char *input_call;
  const char *input_rst;
  const char *input_comments;
  int active_input_field;
  const char *input_call_r1;
  const char *input_rst_r1;
  const char *input_comments_r1;
  int active_input_field_r1;
  const char *input_call_r2;
  const char *input_rst_r2;
  const char *input_comments_r2;
  int active_input_field_r2;
  const char *status;
  const char *dxcc;
  const char *info;
  const char *contest_exchange_label;
  const char *contest_exchange_sent;
  bool contest_entry_mode;
  bool cluster_view;
  int cluster_scroll;
  const char *technique;
  int active_radio;
  int radio1_freq_khz;
  int radio2_freq_khz;
  const char *radio1_mode;
  const char *radio2_mode;
  bool radio1_run;
  bool radio2_run;
} AppRenderState;

/*
 * Initialize shared application state and start background services.
 *
 * @return 0 on success, or -1 if initialization fails.
 */
int app_controller_init(void);

/*
 * Shut down shared application state and stop background services.
 *
 * @return Nothing.
 */
void app_controller_shutdown(void);

/*
 * Copy the current render state into out for UI consumers.
 *
 * @param out Destination structure to fill. Must point to a valid
 *            AppRenderState instance.
 * @return Nothing.
 */
void app_controller_get_render_state(AppRenderState *out);

/*
 * Execute one full command line directly, without split-field key emulation.
 *
 * @param command_text Command text such as "contest contest_defs/cq_wpx_cw.conf".
 * @return The controller event the UI should react to.
 */
AppControllerEvent app_controller_submit_command_text(const char *command_text);

/*
 * Handle a translated key code and update shared controller state.
 *
 * @param key One of the APP_KEY_* values.
 * @return The controller event the UI should react to, or
 *         APP_CTRL_EVENT_NONE if no special action is required.
 */
AppControllerEvent app_controller_handle_key(int key);

/*
 * Download and reload the latest CTY database.
 *
 * @return Nothing.
 */
void app_controller_perform_cty_update(void);

/*
 * Finalize CTY update state after an external download attempt.
 *
 * @param download_ok Nonzero if wl_cty.dat was downloaded successfully.
 * @return Nothing.
 */
void app_controller_complete_cty_update(int download_ok);

/*
 * Get current operating frequency in kHz used for split-entry QSOs.
 *
 * @return Current frequency in kHz.
 */
int app_controller_get_active_frequency_khz(void);

/*
 * Read current operating technique.
 */
ContestTechnique app_controller_get_contest_technique(void);

/*
 * Update operating technique.
 */
void app_controller_set_contest_technique(ContestTechnique technique);

/*
 * Get currently active radio number (1 or 2).
 */
int app_controller_get_active_radio(void);

/*
 * Set active radio number (1 or 2).
 */
void app_controller_set_active_radio(int radio_nr);

/*
 * Swap radio 1 and radio 2 focus/state.
 */
void app_controller_swap_radios(void);

/*
 * Toggle RUN/S&P state for the selected radio.
 */
void app_controller_toggle_run_sp(int radio_nr);

/*
 * Read one radio status snapshot.
 */
int app_controller_get_radio_state(int radio_nr, int *out_freq_khz,
                                   char *out_mode, size_t out_mode_size,
                                   int *out_is_run);

#ifdef __cplusplus
}
#endif

#endif