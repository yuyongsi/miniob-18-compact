#pragma once
#include "common/lang/string.h"
#include "sql/stmt/stmt.h"
class DropTableStmt : public Stmt
{
public:
  explicit DropTableStmt(const string &name) : name_(name) {}
  StmtType      type() const override { return StmtType::DROP_TABLE; }
  const string &name() const { return name_; }

private:
  string name_;
};
