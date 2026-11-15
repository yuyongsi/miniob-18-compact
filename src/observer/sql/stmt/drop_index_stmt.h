// SPDX-License-Identifier: MulanPSL-2.0
#pragma once
#include "sql/stmt/stmt.h"
class DropIndexStmt : public Stmt
{
public:
  explicit DropIndexStmt(const DropIndexSqlNode &sql) : sql_(sql) {}
  StmtType                type() const override { return StmtType::DROP_INDEX; }
  const DropIndexSqlNode &sql() const { return sql_; }

private:
  DropIndexSqlNode sql_;
};
