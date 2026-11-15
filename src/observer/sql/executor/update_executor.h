#pragma once
#include "common/sys/rc.h"
class SQLStageEvent;
class UpdateExecutor
{
public:
  RC execute(SQLStageEvent *event);
};
