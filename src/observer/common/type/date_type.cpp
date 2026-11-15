#include "common/type/date_type.h"
#include "common/value.h"
#include <cstdio>
#include <regex>
RC DateType::parse(const string &s, Value &out)
{
  static const std::regex pattern("[0-9]{4}-[0-9]{1,2}-[0-9]{1,2}");
  if (!std::regex_match(s, pattern))
    return RC::SCHEMA_FIELD_TYPE_MISMATCH;
  int y, m, d;
  if (sscanf(s.c_str(), "%d-%d-%d", &y, &m, &d) != 3)
    return RC::SCHEMA_FIELD_TYPE_MISMATCH;
  static const int days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (y < 1 || y > 9999 || m < 1 || m > 12 || d < 1 ||
      d > days[m] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)))
    return RC::SCHEMA_FIELD_TYPE_MISMATCH;
  out.set_int(y * 10000 + m * 100 + d);
  out.set_type(AttrType::DATES);
  return RC::SUCCESS;
}
int DateType::compare(const Value &a, const Value &b) const
{
  if (b.attr_type() != AttrType::DATES)
    return INT32_MAX;
  return (a.get_int() > b.get_int()) - (a.get_int() < b.get_int());
}
RC DateType::to_string(const Value &value, string &out) const
{
  int  v = value.get_int();
  char b[16];
  snprintf(b, sizeof(b), "%04d-%02d-%02d", v / 10000, v / 100 % 100, v % 100);
  out = b;
  return RC::SUCCESS;
}
