#ifndef CAT_H
#define CAT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int model;
  char model_name[96];
} CatRigInfo;

enum {
  CAT_SLOT_A = 0,
  CAT_SLOT_B = 1,
  CAT_SLOT_COUNT = 2
};

typedef enum {
  CAT_VFO_CURR = 0,
  CAT_VFO_A = 1,
  CAT_VFO_B = 2
} CatVfo;

typedef struct {
  int model;
  char device[128];
  int baud_rate;
  int data_bits;
  int stop_bits;
  char parity[16];
  char handshake[16];
} CatConnectionParams;

/*
 * Initialize CAT support and Hamlib backends.
 *
 * @return 0 on success, or -1 on failure.
 */
int cat_init(void);

/*
 * Release CAT resources and disconnect from rig if needed.
 *
 * @return Nothing.
 */
void cat_shutdown(void);

/*
 * Enumerate available rig models.
 *
 * @param out Destination array for rig entries.
 * @param max_entries Maximum number of entries that can be written.
 * @return Number of entries written.
 */
int cat_list_rigs(CatRigInfo *out, int max_entries);

/*
 * Open a CAT connection using provided parameters.
 *
 * @param params Connection parameters.
 * @return 0 on success, or -1 on failure.
 */
int cat_connect(const CatConnectionParams *params);

/*
 * Open a CAT connection in one rig slot.
 */
int cat_connect_slot(int slot, const CatConnectionParams *params);

/*
 * Close the active CAT connection.
 *
 * @return Nothing.
 */
void cat_disconnect(void);

/*
 * Close CAT connection in a specific rig slot.
 */
void cat_disconnect_slot(int slot);

/*
 * Report whether CAT is currently connected.
 *
 * @return 1 if connected, otherwise 0.
 */
int cat_is_connected(void);

/*
 * Report whether a specific CAT slot is connected.
 */
int cat_is_connected_slot(int slot);

/*
 * Read current rig frequency and convert it to kHz.
 *
 * @param out_khz Destination for frequency in kHz.
 * @return 0 on success, or -1 on failure.
 */
int cat_get_frequency_khz(int *out_khz);

/*
 * Read frequency in kHz from one rig slot.
 */
int cat_get_frequency_khz_slot(int slot, int *out_khz);

/*
 * Read frequency in kHz from one rig slot and selected VFO.
 */
int cat_get_frequency_khz_slot_vfo(int slot, CatVfo vfo, int *out_khz);

/*
 * Read the current rig mode and map it to a logger mode label.
 *
 * @param out Destination buffer for the mode label.
 * @param out_size Destination buffer size.
 * @return 0 on success, or -1 on failure.
 */
int cat_get_mode_label(char *out, size_t out_size);

/*
 * Read current mode label from one rig slot.
 */
int cat_get_mode_label_slot(int slot, char *out, size_t out_size);

/*
 * Read current mode label from one rig slot and selected VFO.
 */
int cat_get_mode_label_slot_vfo(int slot, CatVfo vfo, char *out,
                                size_t out_size);

/*
 * Set rig frequency from a value in kHz.
 *
 * @param freq_khz Frequency in kHz.
 * @return 0 on success, or -1 on failure.
 */
int cat_set_frequency_khz(int freq_khz);

/*
 * Set frequency in kHz for one rig slot.
 */
int cat_set_frequency_khz_slot(int slot, int freq_khz);

/*
 * Set frequency in kHz for one rig slot and selected VFO.
 */
int cat_set_frequency_khz_slot_vfo(int slot, CatVfo vfo, int freq_khz);

/*
 * Select active VFO in one rig slot.
 */
int cat_set_active_vfo_slot(int slot, CatVfo vfo);

/*
 * Read active VFO in one rig slot.
 */
int cat_get_active_vfo_slot(int slot, CatVfo *out_vfo);

/*
 * Send a string as CW via the default CAT slot.
 */
int cat_send_morse(const char *text);

/*
 * Send a string as CW via a specific CAT slot.
 */
int cat_send_morse_slot(int slot, const char *text);

/*
 * Stop ongoing CW transmission on the default CAT slot.
 */
int cat_stop_morse(void);

/*
 * Stop ongoing CW transmission on a specific CAT slot.
 */
int cat_stop_morse_slot(int slot);

/*
 * Open a dedicated CW keyer serial port (separate from the CAT connection).
 * line must be "DTR" or "RTS". wpm sets the sending speed.
 */
int cat_connect_cw_keyer(const char *device, const char *line, int wpm);

/*
 * Update the active CW keyer speed immediately.
 *
 * @param wpm New speed in WPM, clamped to 5..60.
 */
void cat_set_cw_wpm(int wpm);

/*
 * Close the dedicated CW keyer serial port and stop any transmission.
 */
void cat_disconnect_cw_keyer(void);

/*
 * Return 1 if the dedicated CW keyer port is open, otherwise 0.
 */
int cat_is_cw_keyer_connected(void);

/*
 * Copy the current CW keyer status message to the destination buffer.
 */
void cat_get_cw_keyer_status(char *out, size_t out_size);

/*
 * Queue text for CW transmission via the dedicated keyer port.
 * Returns 0 on success or -1 if the keyer is not connected.
 */
int cat_cw_send(const char *text);

/*
 * Abort the current CW transmission and clear the queue.
 */
void cat_cw_stop(void);

/*
 * Copy current CAT status message to the destination buffer.
 *
 * @param out Destination buffer.
 * @param out_size Destination buffer size.
 * @return Nothing.
 */
void cat_get_status(char *out, size_t out_size);

/*
 * Copy CAT status for one rig slot.
 */
void cat_get_status_slot(int slot, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif