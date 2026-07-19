#ifndef PROJECT_H
#define PROJECT_H

#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define TEAM_COUNT 2
#define MAX_MEMBERS 64
#define MAX_TEXT 160
#define MAX_PATH 256
#define INVALID_FD (-1)

#define FURNITURE_MSG_MAGIC 0x4655524Eu /* FURN */
#define EVENT_MSG_MAGIC     0x45564E54u /* EVNT */

#endif
