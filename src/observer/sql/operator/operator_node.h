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

#include <stdint.h>
#include "common/lang/vector.h"
#include "common/lang/memory.h"
/**
 * @brief Operator type(including logical and physical)
 */
enum class OpType
{
  UNDEFINED = 0,

  // Special wildcard
  LEAF,

  // Logical Operators
  LOGICALGET,
  LOGICALCALCULATE,
  LOGICALGROUPBY,
  LOGICALPROJECTION,
  LOGICALFILTER,
  LOGICALINNERJOIN,
  LOGICALINSERT,
  LOGICALDELETE,
  LOGICALUPDATE,
  LOGICALLIMIT,
  LOGICALANALYZE,
  LOGICALEXPLAIN,
  // Separation of logical and physical operators
  LOGICALPHYSICALDELIMITER,

  // Physical Operators
  EXPLAIN,
  CALCULATE,
  SEQSCAN,
  INDEXSCAN,
  ORDERBY,
  LIMIT,
  INNERINDEXJOIN,
  INNERNLJOIN,
  INNERHASHJOIN,
  PROJECTION,
  INSERT,
  DELETE,
  UPDATE,
  AGGREGATE,
  HASHGROUPBY,
  ANALYZE,
  FILTER,
  SCALARGROUPBY
};

// TODO: OperatorNode is the abstrace class of logical/physical operator
// in cascade there is EXPR to include OperatorNode and OperatorNode children
// so here remove genral_children.
class OperatorNode
{
public:
  virtual ~OperatorNode() = default;
  /**
   * TODO: add this function
   */
  // virtual std::string get_name() const = 0;

  /**
   * TODO: unify logical and physical OpType
   */
  virtual OpType get_op_type() const { return OpType::UNDEFINED; }

  /**
   * @return Whether node contents represent a physical operator / expression
   */
  virtual bool is_physical() const = 0;

  /**
   * @return Whether node represents a logical operator / expression
   */
  virtual bool is_logical() const = 0;
};
