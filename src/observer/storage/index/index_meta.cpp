// SPDX-License-Identifier: MulanPSL-2.0
#include "storage/index/index_meta.h"
#include "storage/field/field_meta.h"
#include "storage/table/table_meta.h"
#include "json/json.h"
#include <set>
RC IndexMeta::init(const char *name, const FieldMeta &field) { return init(name, {field.name()}); }
RC IndexMeta::init(const char *name, const std::vector<std::string> &fields, bool unique, bool encoded)
{
  if (!name || !*name || fields.empty())
    return RC::INVALID_ARGUMENT;
  std::set<std::string> seen;
  for (auto &f : fields)
    if (f.empty() || !seen.insert(f).second)
      return RC::INVALID_ARGUMENT;
  name_     = name;
  fields_   = fields;
  field_    = fields.front();
  unique_   = unique;
  encoded_  = encoded;
  return RC::SUCCESS;
}
void IndexMeta::to_json(Json::Value &v) const
{
  v["name"]       = name_;
  v["field_name"] = field_;
  v["unique"]     = unique_;
  v["encoded"]    = encoded_;
  v["fields"]     = Json::Value(Json::arrayValue);
  for (auto &f : fields_)
    v["fields"].append(f);
}
RC IndexMeta::from_json(const TableMeta &table, const Json::Value &v, IndexMeta &index)
{
  if (v.get("fulltext", false).asBool())
    return RC::UNIMPLEMENTED;
  if (!v["name"].isString())
    return RC::INTERNAL;
  std::vector<std::string> fields;
  if (v["fields"].isArray()) {
    for (auto &f : v["fields"]) {
      if (!f.isString())
        return RC::INTERNAL;
      fields.push_back(f.asString());
    }
  } else if (v["field_name"].isString())
    fields.push_back(v["field_name"].asString());
  for (auto &f : fields)
    if (!table.field(f.c_str()))
      return RC::SCHEMA_FIELD_MISSING;
  return index.init(v["name"].asCString(),
      fields,
      v.get("unique", false).asBool(),
      v.get("encoded", false).asBool());
}
const char *IndexMeta::name() const { return name_.c_str(); }
const char *IndexMeta::field() const { return field_.c_str(); }
void        IndexMeta::desc(ostream &os) const
{
  os << "index name=" << name_ << ", fields=";
  for (auto &f : fields_)
    os << f << " ";
}
