// SPDX-License-Identifier: MulanPSL-2.0
#pragma once
#include "sql/stmt/stmt.h"
#include "storage/index/index_meta.h"
class Table;
struct CreateIndexSqlNode;
class CreateIndexStmt : public Stmt
{
public:
  CreateIndexStmt(Table *table, IndexMeta meta) : table_(table), meta_(std::move(meta)) {}
  StmtType         type() const override { return StmtType::CREATE_INDEX; }
  Table           *table() const { return table_; }
  const IndexMeta &meta() const { return meta_; }
  static RC        create(Db *db, const CreateIndexSqlNode &sql, Stmt *&stmt);

private:
  Table    *table_;
  IndexMeta meta_;
};
