#pragma once
#include "common/type/data_type.h"
class DateType : public DataType
{
public:
  DateType() : DataType(AttrType::DATES) {}
  static RC parse(const string &s, Value &out);
  int       compare(const Value &a, const Value &b) const override;
  RC        to_string(const Value &value, string &out) const override;
  RC        set_value_from_str(Value &value, const string &s) const override { return parse(s, value); }
};
