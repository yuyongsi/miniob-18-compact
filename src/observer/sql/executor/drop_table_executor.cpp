#include "sql/executor/drop_table_executor.h"
#include "event/sql_event.h"
#include "event/session_event.h"
#include "session/session.h"
#include "sql/stmt/drop_table_stmt.h"
#include "storage/db/db.h"
RC DropTableExecutor::execute(SQLStageEvent *event)
{
  auto *stmt = static_cast<DropTableStmt *>(event->stmt());
  return event->session_event()->session()->get_current_db()->drop_table(stmt->name().c_str());
}
