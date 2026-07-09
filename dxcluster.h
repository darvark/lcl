#ifndef DXCLUSTER_H
#define DXCLUSTER_H

#include "globals.h"

#include <pthread.h>

#define MAX_SPOTS 500

typedef struct {
  char time[16];
  char freq[16];
  char call[32];
  char comment[128];

} Spot;

extern Spot spots[MAX_SPOTS];
extern int spot_count;
extern int spot_start;
extern char dxcluster_status[128];
extern pthread_mutex_t dxcluster_mutex;

/*
 * Start the background DXCluster worker thread.
 *
 * @return 0 on success, or -1 if the worker thread cannot be created or its
 *         shutdown pipe cannot be initialized.
 */
int dxcluster_start(void);

/*
 * Stop the background DXCluster worker thread and release its resources.
 *
 * @return Nothing.
 */
void dxcluster_stop(void);

/*
 * Update the shared DXCluster status string.
 *
 * @param text Status message to store before the configured host and port.
 * @return Nothing.
 */
void dxcluster_set_status(const char *text);

#endif
