// SPDX-License-Identifier: MulanPSL-2.0
#pragma once
#include "common/sys/rc.h"
class SQLStageEvent;
// Prepare a row-query operator for extended SELECT syntax. Other statements
// and simple SELECTs retain the existing parser/optimizer path.
RC prepare_relational_query(SQLStageEvent *event, bool &handled);
