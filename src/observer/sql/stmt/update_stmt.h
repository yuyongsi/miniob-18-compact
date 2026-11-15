/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once
#include "sql/stmt/stmt.h"
class UpdateStmt : public Stmt
{
public:
  explicit UpdateStmt(const UpdateSqlNode &sql) : sql_(sql) {}
  StmtType             type() const override { return StmtType::UPDATE; }
  const UpdateSqlNode &sql() const { return sql_; }
  static RC            create(Db *, const UpdateSqlNode &sql, Stmt *&stmt)
  {
    stmt = new UpdateStmt(sql);
    return RC::SUCCESS;
  }

private:
  UpdateSqlNode sql_;
};
