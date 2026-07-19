#ifndef TEAM_H
#define TEAM_H

#include "config.h"
#include "ipc.h"

typedef struct ChildArgs {
    Config cfg;
    AllIPC *ipc;
    int team_id;
    int member_id;
} ChildArgs;

void run_team_member(const ChildArgs *args);

#endif
