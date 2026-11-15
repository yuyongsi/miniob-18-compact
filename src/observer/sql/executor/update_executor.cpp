#include "sql/executor/update_executor.h"
#include "sql/stmt/update_stmt.h"
#include "event/sql_event.h"
#include "event/session_event.h"
#include "session/session.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "storage/record/record_scanner.h"
#include "storage/trx/trx.h"
RC UpdateExecutor::execute(SQLStageEvent *event)
{
  const auto &sql     = static_cast<UpdateStmt *>(event->stmt())->sql();
  Session    *session = event->session_event()->session();
  Table      *table   = session->get_current_db()->find_table(sql.relation_name.c_str());
  if (!table)
    return RC::SCHEMA_TABLE_NOT_EXIST;
  const FieldMeta *field = table->table_meta().field(sql.attribute_name.c_str());
  if (!field || !field->visible())
    return RC::SCHEMA_FIELD_NOT_EXIST;
  Value value = sql.value;
  if (value.attr_type() != AttrType::NULLS) {
    if (field->type() == AttrType::TEXTS) {
      if (value.attr_type() != AttrType::CHARS || value.length() > 65535)
        return RC::SCHEMA_FIELD_TYPE_MISMATCH;
    } else if (value.attr_type() != field->type()) {
      Value converted;
      RC    cast_rc = Value::cast_to(value, field->type(), converted);
      if (cast_rc != RC::SUCCESS)
        return cast_rc;
      value = converted;
    }
  }
  CompositeConditionFilter filter;
  RC                       rc = filter.init(*table, sql.conditions.data(), sql.conditions.size());
  if (rc != RC::SUCCESS)
    return rc;
  Trx           *trx = session->current_trx();
  RecordScanner *raw = nullptr;
  rc                 = table->get_record_scanner(raw, trx, ReadWriteMode::READ_WRITE);
  unique_ptr<RecordScanner> scanner(raw);
  if (rc != RC::SUCCESS)
    return rc;
  vector<Record> rows;
  Record         row;
  while ((rc = scanner->next(row)) == RC::SUCCESS) {
    if (filter.filter(row)) {
      Record copy;
      copy.copy_data(row.data(), table->table_meta().record_size());
      copy.rid() = row.rid();
      rows.emplace_back(std::move(copy));
    }
  }
  scanner->close_scan();
  if (rc != RC::RECORD_EOF)
    return rc;
  vector<Record> replacements;
  for (Record &old : rows) {
    Record next;
    next.copy_data(old.data(), table->table_meta().record_size());
    next.rid() = old.rid();
    rc         = table->write_value(next.data(), *field, value);
    if (rc != RC::SUCCESS)
      return rc;
    replacements.push_back(std::move(next));
  }
  for (size_t i = 0; i < rows.size(); i++) {
    rc = trx->update_record(table, rows[i], replacements[i]);
    if (rc != RC::SUCCESS) {
      for (size_t j = i; j > 0; j--) {
        RC undo = trx->update_record(table, replacements[j - 1], rows[j - 1]);
        if (undo != RC::SUCCESS)
          return undo;
      }
      return rc;
    }
  }
  return RC::SUCCESS;
}
