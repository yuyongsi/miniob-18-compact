// SPDX-License-Identifier: MulanPSL-2.0
#include "sql/stmt/create_index_stmt.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
RC CreateIndexStmt::create(Db *db, const CreateIndexSqlNode &sql, Stmt *&stmt)
{
  stmt         = nullptr;
  Table *table = db->find_table(sql.relation_name.c_str());
  if (!table)
    return RC::SCHEMA_TABLE_NOT_EXIST;
  if (table->find_index(sql.index_name.c_str()))
    return RC::SCHEMA_INDEX_NAME_REPEAT;
  auto fields = sql.attribute_names;
  if (fields.empty() && !sql.attribute_name.empty())
    fields.push_back(sql.attribute_name);
  for (auto &name : fields) {
    auto *f = table->table_meta().field(name.c_str());
    if (!f || !f->visible())
      return RC::SCHEMA_FIELD_NOT_EXIST;
    if (f->type() == AttrType::TEXTS || f->type() == AttrType::VECTORS)
      return RC::SCHEMA_FIELD_TYPE_MISMATCH;
  }
  IndexMeta meta;
  RC        rc = meta.init(sql.index_name.c_str(), fields, sql.unique);
  if (rc != RC::SUCCESS)
    return rc;
  stmt = new CreateIndexStmt(table, meta);
  return RC::SUCCESS;
}
