// SPDX-License-Identifier: MulanPSL-2.0
#include "sql/executor/relational_query.h"
#include "sql/executor/relational_query_ast.h"
#include "event/sql_event.h"
#include "event/session_event.h"
#include "session/session.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "storage/record/record_scanner.h"
#include "storage/trx/trx.h"
#include "storage/trx/mvcc_trx.h"
#include "common/type/date_type.h"
#include <algorithm>
#include <functional>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <cstdlib>

namespace relational {
using Scopes  = std::vector<Schema>;
using Rows    = std::vector<Row>;
using Context = std::vector<const Row *>;
struct Result
{
  Rows   rows;
};
Value null_value()
{
  Value v;
  v.set_type(AttrType::NULLS);
  return v;
}
bool   is_null(const Value &v) { return v.attr_type() == AttrType::NULLS; }
bool   numeric(AttrType t) { return t == AttrType::INTS || t == AttrType::FLOATS || t == AttrType::BOOLEANS; }
double number(const Value &v)
{
  if (v.attr_type() == AttrType::INTS)
    return v.get_int();
  if (v.attr_type() == AttrType::BOOLEANS)
    return v.get_boolean() ? 1 : 0;
  if (v.attr_type() == AttrType::CHARS) {
    char  *end = nullptr;
    double n   = std::strtod(v.get_string().c_str(), &end);
    return n;
  }
  return v.get_float();
}
bool  truth(const Value &v) { return !is_null(v) && number(v) != 0; }
Value boolean(bool b) { return Value(b ? 1 : 0); }
Value number_value(double x, bool integer = false)
{
  if (x == 0)
    x = 0;  // SQL output has no negative-zero spelling.
  if (!std::isfinite(x))
    return null_value();
  if (integer && x >= INT32_MIN && x <= INT32_MAX)
    return Value(static_cast<int>(x));
  return Value(static_cast<float>(x));
}
bool aggregate_name(const std::string &name)
{
  return name == "count" || name == "sum" || name == "avg" || name == "min" || name == "max";
}
bool contains_subquery(const E &e)
{
  if (!e)
    return false;
  if (e->query)
    return true;
  for (auto &arg : e->args)
    if (contains_subquery(arg))
      return true;
  return false;
}
bool has_aggregate(const E &e)
{
  if (!e)
    return false;
  if (e->kind == Expr::FUNCTION && aggregate_name(e->op))
    return true;
  for (auto &a : e->args)
    if (has_aggregate(a))
      return true;
  return false;
}
int compare(Value a, Value b)
{
  if (a.attr_type() == AttrType::DATES && b.attr_type() == AttrType::CHARS) {
    Value converted;
    if (DateType::parse(b.get_string(), converted) != RC::SUCCESS)
      throw Error("invalid date");
    b = converted;
  }
  if (b.attr_type() == AttrType::DATES && a.attr_type() == AttrType::CHARS) {
    Value converted;
    if (DateType::parse(a.get_string(), converted) != RC::SUCCESS)
      throw Error("invalid date");
    a = converted;
  }
  if (a.attr_type() == AttrType::DATES || b.attr_type() == AttrType::DATES) {
    if (a.attr_type() != b.attr_type())
      throw Error("incompatible date comparison");
    return (a.get_int() > b.get_int()) - (a.get_int() < b.get_int());
  }
  if (a.attr_type() == AttrType::CHARS && b.attr_type() == AttrType::CHARS) {
    auto x = a.get_string(), y = b.get_string();
    return (x > y) - (x < y);
  }
  double x = number(a), y = number(b);
  return (x > y) - (x < y);
}
std::string row_key(const Row &row)
{
  std::ostringstream out;
  out << std::setprecision(17);
  for (const auto &v : row) {
    if (is_null(v))
      out << "n;";
    else if (numeric(v.attr_type()))
      out << "v" << number(v) << ';';
    else {
      auto s = v.to_string();
      out << 's' << s.size() << ':' << s << ';';
    }
  }
  return out.str();
}
bool same_expression(const E &a, const E &b)
{
  if (!a || !b || a->kind != b->kind)
    return false;
  if (a->kind == Expr::FIELD)
    return a->depth == b->depth && a->index == b->index;
  if (a->kind == Expr::LITERAL)
    return a->value.attr_type() == b->value.attr_type() && a->value.to_string() == b->value.to_string();
  if (a->query || b->query)
    return a.get() == b.get();
  if (a->op != b->op || a->args.size() != b->args.size())
    return false;
  for (size_t i = 0; i < a->args.size(); i++)
    if (!same_expression(a->args[i], b->args[i]))
      return false;
  return true;
}
void for_query_expressions(const Q &q, const std::function<void(const E &)> &fn)
{
  auto &s = q->select;
  for (auto &i : s.items)
    fn(i.expr);
  fn(s.where);
  fn(s.having);
  for (auto &g : s.groups)
    fn(g);
  for (auto &o : q->order)
    fn(o.expr);
}
bool external_expr(const E &e, int level);
bool external_query(const Q &q, int level)
{
  bool found = false;
  for_query_expressions(q, [&](const E &e) { found = found || external_expr(e, level); });
  return found;
}
bool external_expr(const E &e, int level)
{
  if (!e)
    return false;
  if (e->kind == Expr::FIELD && e->depth > level)
    return true;
  for (auto &a : e->args)
    if (external_expr(a, level))
      return true;
  return e->query && external_query(e->query, level + 1);
}
int dependency(const E &e, int level = 0)
{
  if (!e)
    return -1;
  int d = e->kind == Expr::FIELD && e->depth == level ? e->index : -1;
  for (auto &a : e->args)
    d = std::max(d, dependency(a, level));
  if (e->query)
    for_query_expressions(e->query, [&](const E &a) { d = std::max(d, dependency(a, level + 1)); });
  return d;
}
void conjunctions(const E &e, std::vector<E> &terms)
{
  if (!e)
    return;
  if (e->kind == Expr::BINARY && e->op == "and") {
    conjunctions(e->args[0], terms);
    conjunctions(e->args[1], terms);
  } else
    terms.push_back(e);
}
bool extended_expr(const E &e)
{
  if (!e)
    return false;
  if ((e->kind == Expr::LITERAL && is_null(e->value)) || e->label.find('(') != std::string::npos ||
      lower(e->label).find("between") != std::string::npos)
    return true;
  if (e->query || e->kind == Expr::FUNCTION || e->kind == Expr::IN || e->kind == Expr::IS_NULL ||
      e->kind == Expr::UNARY)
    return true;
  if (e->kind == Expr::BINARY && (e->op == "or" || e->op == "+" || e->op == "-" || e->op == "*" || e->op == "/" ||
                                     e->op == "%" || e->op == "like"))
    return true;
  for (auto &a : e->args)
    if (extended_expr(a))
      return true;
  return false;
}
bool extended_query(const Q &q)
{
  if (!q->order.empty())
    return true;
  auto &s = q->select;
  if (s.sources.size() != 1 || s.distinct || s.having || !s.groups.empty())
    return true;
  for (auto &src : s.sources)
    if (lower(src.alias) != lower(src.name))
      return true;
  for (auto &i : s.items)
    if (!i.alias.empty() || i.expr->kind == Expr::LITERAL || extended_expr(i.expr) ||
        (i.expr->kind == Expr::STAR && !i.expr->qualifier.empty()))
      return true;
  return extended_expr(s.where);
}

class Engine
{
  Db                                           *db_;
  Trx                                          *trx_ = nullptr;
  std::unordered_map<Table *, Rows>             tables_;
  std::unordered_map<Query *, Result>           subqueries_;
  int                                           recursion_ = 0;
  Table                                        *table(const std::string &name)
  {
    Table *t = db_->find_table(name.c_str());
    if (t)
      return t;
    std::vector<std::string> names;
    db_->all_tables(names);
    for (auto &n : names)
      if (lower(n) == lower(name))
        return db_->find_table(n.c_str());
    throw Error("table not found: " + name, RC::SCHEMA_TABLE_NOT_EXIST);
  }
  void bind_expr(const E &e, const Scopes &scopes, bool inside_aggregate = false)
  {
    if (!e)
      return;
    if (e->kind == Expr::LITERAL) {
      e->type = e->value.attr_type();
      return;
    }
    if (e->kind == Expr::FIELD) {
      for (size_t depth = 0; depth < scopes.size(); depth++) {
        int found = -1;
        for (size_t i = 0; i < scopes[depth].size(); i++) {
          auto &c = scopes[depth][i];
          if (lower(c.name) == lower(e->name) && (e->qualifier.empty() || lower(c.table) == lower(e->qualifier))) {
            if (found >= 0)
              throw Error("ambiguous field: " + e->name);
            found = i;
          }
        }
        if (found >= 0) {
          e->depth = depth;
          e->index = found;
          e->type  = scopes[depth][found].type;
          return;
        }
        if (!e->qualifier.empty()) {
          for (const auto &column : scopes[depth]) {
            if (lower(column.table) == lower(e->qualifier))
              throw Error("field not found in nearest table alias", RC::SCHEMA_FIELD_NOT_EXIST);
          }
        }
      }
      throw Error("field not found: " + e->qualifier + "." + e->name, RC::SCHEMA_FIELD_NOT_EXIST);
    }
    if (e->kind == Expr::STAR)
      throw Error("star is only allowed in projection or COUNT");
    bool agg = e->kind == Expr::FUNCTION && aggregate_name(e->op);
    if (agg && inside_aggregate)
      throw Error("nested aggregate");
    if (agg) {
      if (e->args.size() != 1)
        throw Error("aggregate requires one argument");
      if (e->args[0]->kind == Expr::STAR) {
        if (e->op != "count" || e->distinct || !e->args[0]->qualifier.empty())
          throw Error("invalid aggregate star");
      } else
        bind_expr(e->args[0], scopes, true);
    } else
      for (auto &a : e->args)
        bind_expr(a, scopes, inside_aggregate);
    if (e->query) {
      bind(e->query, scopes);
      if (e->kind != Expr::EXISTS && e->query->output.size() != 1)
        throw Error("subquery must return one column");
    }
    if (e->kind == Expr::SCALAR) {
      e->type = e->query->output[0].type;
      return;
    }
    if (e->kind == Expr::FUNCTION) {
      const size_t n = e->args.size();
      if (agg) {
        e->type = e->op == "count" ? AttrType::INTS : e->op == "avg" ? AttrType::FLOATS : e->args[0]->type;
        return;
      }
      if (e->op == "length") {
        if (n != 1)
          throw Error("LENGTH requires one argument");
        if (e->args[0]->type != AttrType::CHARS && e->args[0]->type != AttrType::NULLS)
          throw Error("LENGTH needs a string");
        e->type = AttrType::INTS;
      } else
        throw Error("unknown function: " + e->op);
      return;
    }
    if (e->kind == Expr::BINARY) {
      auto a = e->args[0], b = e->args[1];
      if ((a->type == AttrType::DATES && b->kind == Expr::LITERAL && b->type == AttrType::CHARS) ||
          (b->type == AttrType::DATES && a->kind == Expr::LITERAL && a->type == AttrType::CHARS)) {
        E     literal = a->type == AttrType::CHARS ? a : b;
        Value converted;
        if (DateType::parse(literal->value.get_string(), converted) != RC::SUCCESS)
          throw Error("invalid date literal");
        literal->value = converted;
        literal->type  = AttrType::DATES;
      }
      if (e->op == "+" || e->op == "-" || e->op == "*" || e->op == "/" || e->op == "%")
        e->type = e->op == "/" || a->type == AttrType::FLOATS || b->type == AttrType::FLOATS ? AttrType::FLOATS
                                                                                             : AttrType::INTS;
      else
        e->type = AttrType::BOOLEANS;
    } else
      e->type = e->kind == Expr::UNARY && e->op != "not" ? e->args[0]->type : AttrType::BOOLEANS;
  }
  E resolve_alias(E expr, const Select &s)
  {
    if (!expr)
      return expr;
    if (expr->kind == Expr::FIELD && expr->qualifier.empty())
      for (auto &i : s.items)
        if (!i.alias.empty() && lower(i.alias) == lower(expr->name))
          return i.expr;
    for (auto &a : expr->args)
      a = resolve_alias(a, s);
    return expr;
  }
  void check_grouped(const E &e, const Select &s)
  {
    if (!e || e->query)
      return;
    if (e->kind == Expr::FUNCTION && aggregate_name(e->op))
      return;
    for (auto &g : s.groups)
      if (same_expression(e, g))
        return;
    if (e->kind == Expr::FIELD && e->depth == 0)
      throw Error("selected field is not grouped");
    for (auto &a : e->args)
      check_grouped(a, s);
  }
  Rows &load(Table *t)
  {
    auto found = tables_.find(t);
    if (found != tables_.end())
      return found->second;
    Rows                           values;
    RecordScanner                 *raw = nullptr;
    RC                             rc  = t->get_record_scanner(raw, trx_, ReadWriteMode::READ_ONLY);
    std::unique_ptr<RecordScanner> scanner(raw);
    if (rc != RC::SUCCESS)
      throw Error("table scan failed", rc);
    Record      record;
    const auto &meta = t->table_meta();
    while ((rc = scanner->next(record)) == RC::SUCCESS) {
      Row row;
      for (int i = meta.sys_field_num(); i < meta.field_num(); i++) {
        auto *f = meta.field(i);
        Value v;
        RC    value_rc = t->read_value(record.data(), *f, v);
        if (value_rc != RC::SUCCESS)
          throw Error("cannot read field", value_rc);
        row.push_back(v);
      }
      values.push_back(std::move(row));
    }
    scanner->close_scan();
    if (rc != RC::RECORD_EOF)
      throw Error("table scan failed", rc);
    return tables_.emplace(t, std::move(values)).first->second;
  }
  static bool like(const std::string &text, const std::string &pattern)
  {
    std::vector<bool> dp(text.size() + 1, false);
    dp[0] = true;
    for (char c : pattern) {
      std::vector<bool> next(text.size() + 1, false);
      if (c == '%')
        next[0] = dp[0];
      for (size_t i = 1; i <= text.size(); i++)
        next[i] = c == '%' ? (dp[i] || next[i - 1]) : dp[i - 1] && (c == '_' || c == text[i - 1]);
      dp.swap(next);
    }
    return dp.back();
  }
  Value eval(const E &e, const Context &context, const Rows *group = nullptr)
  {
    if (!e)
      return boolean(true);
    if (e->kind == Expr::LITERAL)
      return e->value;
    if (e->kind == Expr::FIELD) {
      if (e->depth < 0 || static_cast<size_t>(e->depth) >= context.size() || !context[e->depth])
        return null_value();
      return context[e->depth]->at(e->index);
    }
    if (e->kind == Expr::SCALAR) {
      auto result = subquery(e->query, context);
      if (result.rows.empty())
        return null_value();
      if (result.rows.size() != 1)
        throw Error("scalar subquery returned multiple rows");
      return result.rows[0][0];
    }
    if (e->kind == Expr::EXISTS)
      return boolean(!subquery(e->query, context).rows.empty());
    if (e->kind == Expr::IN) {
      Value              left = eval(e->args[0], context, group);
      std::vector<Value> candidates;
      if (e->query) {
        auto result = subquery(e->query, context);
        for (auto &row : result.rows)
          candidates.push_back(row[0]);
      } else
        for (size_t i = 1; i < e->args.size(); i++)
          candidates.push_back(eval(e->args[i], context, group));
      bool unknown = false, found = false;
      for (auto &v : candidates) {
        if (is_null(left) || is_null(v)) {
          unknown = true;
          continue;
        }
        if (compare(left, v) == 0) {
          found = true;
          break;
        }
      }
      if (!found && unknown)
        return null_value();
      return boolean(e->negative ? !found : found);
    }
    if (e->kind == Expr::IS_NULL) {
      bool yes = is_null(eval(e->args[0], context, group));
      return boolean(e->negative ? !yes : yes);
    }
    if (e->kind == Expr::UNARY) {
      Value v = eval(e->args[0], context, group);
      if (is_null(v))
        return v;
      if (e->op == "not")
        return boolean(!truth(v));
      return number_value((e->op == "-" ? -1 : 1) * number(v), v.attr_type() == AttrType::INTS);
    }
    if (e->kind == Expr::BINARY) {
      Value a = eval(e->args[0], context, group);
      if (e->op == "and" && !is_null(a) && !truth(a))
        return boolean(false);
      if (e->op == "or" && truth(a))
        return boolean(true);
      Value b = eval(e->args[1], context, group);
      if (e->op == "and") {
        if (!is_null(b) && !truth(b))
          return boolean(false);
        return is_null(a) || is_null(b) ? null_value() : boolean(true);
      }
      if (e->op == "or") {
        if (truth(b))
          return boolean(true);
        return is_null(a) || is_null(b) ? null_value() : boolean(false);
      }
      if (is_null(a) || is_null(b))
        return null_value();
      double x = number(a), y = number(b);
      bool   integral = a.attr_type() == AttrType::INTS && b.attr_type() == AttrType::INTS;
      if (e->op == "+")
        return number_value(x + y, integral);
      if (e->op == "-")
        return number_value(x - y, integral);
      if (e->op == "*")
        return number_value(x * y, integral);
      if (e->op == "/")
        return y == 0 ? null_value() : number_value(x / y);
      if (e->op == "%")
        return y == 0 ? null_value() : number_value(std::fmod(x, y), integral);
      if (e->op == "like")
        return boolean(like(a.to_string(), b.to_string()));
      int cmp = compare(a, b);
      return boolean(e->op == "="                     ? cmp == 0
                     : e->op == "<>" || e->op == "!=" ? cmp != 0
                     : e->op == "<"                   ? cmp < 0
                     : e->op == "<="                  ? cmp <= 0
                     : e->op == ">"                   ? cmp > 0
                                                      : cmp >= 0);
    }
    if (e->kind == Expr::FUNCTION && aggregate_name(e->op)) {
      if (!group)
        throw Error("aggregate outside group");
      long long                       count = 0;
      double                          sum   = 0;
      Value                           best  = null_value();
      std::unordered_set<std::string> seen;
      for (auto &row : *group) {
        Context inner = context;
        inner[0]      = &row;
        Value v       = e->args[0]->kind == Expr::STAR ? Value(1) : eval(e->args[0], inner);
        if (is_null(v))
          continue;
        if (e->distinct && !seen.insert(row_key({v})).second)
          continue;
        count++;
        sum += number(v);
        if (is_null(best) || (e->op == "min" ? compare(v, best) < 0 : compare(v, best) > 0))
          best = v;
      }
      if (e->op == "count")
        return number_value(count, true);
      if (count == 0)
        return null_value();
      if (e->op == "sum")
        return number_value(sum, e->args[0]->type == AttrType::INTS);
      if (e->op == "avg")
        return number_value(sum / count);
      return best;
    }
    if (e->kind == Expr::FUNCTION) {
      std::vector<Value> a;
      for (auto &arg : e->args) {
        a.push_back(eval(arg, context, group));
        if (is_null(a.back()))
          return null_value();
      }
      if (e->op == "length") {
        if (a[0].attr_type() != AttrType::CHARS)
          throw Error("LENGTH needs a string");
        return number_value(a[0].get_string().size(), true);
      }
    }
    throw Error("unsupported expression");
  }
  Result subquery(const Q &q, const Context &outer)
  {
    // A correlated result belongs to one outer row and must never enter this
    // statement-wide cache. Base table scans can still share the same snapshot.
    if (!q->correlated) {
      auto found = subqueries_.find(q.get());
      if (found != subqueries_.end())
        return found->second;
    }
    Result result = run(q, outer);
    if (!q->correlated)
      subqueries_[q.get()] = result;
    return result;
  }
  struct OutputRow
  {
    Row values, keys;
  };
  std::vector<OutputRow> run_select(Select &s, const Context &outer, const std::vector<Order> &order)
  {
    std::vector<Rows> sources;
    for (auto &src : s.sources)
      sources.push_back(load(table(src.name)));
    // Each predicate is evaluated when its final local column becomes available.
    // This avoids materializing the full Cartesian product for chained joins.
    std::vector<E> terms;
    conjunctions(s.where, terms);
    std::vector<int> deps;
    for (auto &term : terms)
      deps.push_back(dependency(term));
    Rows    filtered;
    Row     current;
    Context context = outer;
    context.insert(context.begin(), &current);
    bool constants = true;
    for (size_t i = 0; i < terms.size(); i++)
      if (deps[i] < 0 && !truth(eval(terms[i], context))) {
        constants = false;
        break;
      }
    std::function<void(size_t)> join = [&](size_t depth) {
      if (depth == sources.size()) {
        filtered.push_back(current);
        return;
      }
      size_t before = current.size();
      for (auto &row : sources[depth]) {
        current.insert(current.end(), row.begin(), row.end());
        bool keep = true;
        for (size_t i = 0; i < terms.size(); i++)
          if (deps[i] >= static_cast<int>(before) && deps[i] < static_cast<int>(current.size()) &&
              !truth(eval(terms[i], context))) {
            keep = false;
            break;
          }
        if (keep)
          join(depth + 1);
        current.resize(before);
      }
    };
    if (constants)
      join(0);
    std::vector<Rows> groups;
    if (s.aggregate || !s.groups.empty()) {
      if (s.groups.empty())
        groups.push_back(std::move(filtered));
      else {
        std::unordered_map<std::string, size_t> positions;
        for (auto &row : filtered) {
          Context c = outer;
          c.insert(c.begin(), &row);
          Row values;
          for (auto &g : s.groups)
            values.push_back(eval(g, c));
          auto key   = row_key(values);
          auto found = positions.find(key);
          if (found == positions.end()) {
            positions[key] = groups.size();
            groups.push_back({});
            found = positions.find(key);
          }
          groups[found->second].push_back(std::move(row));
        }
      }
    } else
      for (auto &row : filtered)
        groups.push_back({std::move(row)});
    std::vector<OutputRow>          output;
    std::unordered_set<std::string> seen;
    for (auto &group : groups) {
      Row        empty;
      const Row *row = group.empty() ? &empty : &group[0];
      Context    c   = outer;
      c.insert(c.begin(), row);
      if (s.having && !truth(eval(s.having, c, &group)))
        continue;
      OutputRow result;
      for (auto &item : s.items)
        result.values.push_back(eval(item.expr, c, &group));
      if (s.distinct && !seen.insert(row_key(result.values)).second)
        continue;
      for (auto &o : order)
        result.keys.push_back(o.output_index >= 0 ? result.values.at(o.output_index) : eval(o.expr, c, &group));
      output.push_back(std::move(result));
    }
    return output;
  }

public:
  explicit Engine(Db *db) : db_(db) {}
  void set_transaction(Trx *trx) { trx_ = trx; }
  void bind_mutation_values(const Mutation &m, const std::vector<const FieldMeta *> &fields, const Scopes &scopes)
  {
    auto bind_values = [&](const std::vector<E> &values) {
      if (values.size() != fields.size())
        throw Error("wrong number of values");
      for (size_t i = 0; i < values.size(); i++) {
        const auto &e = values[i];
        bind_expr(e, scopes);
        if (has_aggregate(e))
          throw Error("aggregate mutation value");
        if (dependency(e) < 0 && !contains_subquery(e))
          validate_value(*fields[i], eval(e, {}));
      }
    };
    if (m.kind == "insert") {
      for (const auto &row : m.insert_rows)
        bind_values(row);
    } else {
      bind_values(m.values);
    }
  }

  RC insert_rows(Table *table, const Mutation &m, const std::vector<const FieldMeta *> &fields)
  {
    const auto &meta = table->table_meta();
    // LSM does not implement per-record undo.
    if (m.insert_rows.size() > 1 && meta.storage_engine() != StorageEngine::HEAP)
      return RC::UNIMPLEMENTED;

    std::vector<Record> records;
    records.reserve(m.insert_rows.size());
    for (const auto &row : m.insert_rows) {
      Row values(meta.field_num() - meta.sys_field_num(), null_value());
      for (size_t i = 0; i < fields.size(); i++)
        values[fields[i]->field_id()] = eval(row[i], {});
      RC rc = table->make_record(values.size(), values.data(), records.emplace_back());
      if (rc != RC::SUCCESS)
        return rc;
    }
    for (size_t i = 0; i < records.size(); i++) {
      RC rc = trx_->insert_record(table, records[i]);
      if (rc == RC::SUCCESS)
        continue;
      // The mutation operator already rolls MVCC statements back to a savepoint.
      if (trx_->type() == TrxKit::VACUOUS) {
        while (i > 0) {
          RC undo = trx_->delete_record(table, records[--i]);
          if (undo != RC::SUCCESS)
            return undo;
        }
      }
      return rc;
    }
    return RC::SUCCESS;
  }

  void bind_mutation(const Mutation &m)
  {
    Table *t = table(m.table);
    Schema columns;
    for (int i = t->table_meta().sys_field_num(); i < t->table_meta().field_num(); i++) {
      auto *f = t->table_meta().field(i);
      columns.push_back({m.table, f->name(), f->type() == AttrType::TEXTS ? AttrType::CHARS : f->type()});
    }
    std::vector<std::string> targets = m.columns;
    if (m.kind == "insert" && targets.empty())
      for (auto &c : columns)
        targets.push_back(c.name);
    std::vector<const FieldMeta *> fields;
    std::set<std::string> seen;
    for (auto &name : targets) {
      auto *f = t->table_meta().field(name.c_str());
      if (!f || !f->visible())
        throw Error("mutation field not found", RC::SCHEMA_FIELD_NOT_EXIST);
      if (!seen.insert(lower(name)).second)
        throw Error("duplicate assignment");
      fields.push_back(f);
    }
    Scopes scopes = m.kind == "insert" ? Scopes{} : Scopes{columns};
    bind_expr(m.where, scopes);
    if (has_aggregate(m.where))
      throw Error("aggregate in DML condition");
    bind_mutation_values(m, fields, scopes);
  }
  void validate_value(const FieldMeta &f, const Value &value)
  {
    if (is_null(value)) {
      if (!f.nullable())
        throw Error("NULL violates NOT NULL");
      return;
    }
    if (f.type() == AttrType::TEXTS) {
      if (value.attr_type() != AttrType::CHARS)
        throw Error("invalid TEXT");
    } else {
      Value converted;
      if (Value::cast_to(value, f.type(), converted) != RC::SUCCESS)
        throw Error("incompatible value type");
    }
  }
  RC mutate(const Mutation &m)
  {
    Table      *t    = table(m.table);
    const auto &meta = t->table_meta();
    if (m.kind == "insert") {
      std::vector<const FieldMeta *> fields;
      if (m.columns.empty()) {
        for (int i = meta.sys_field_num(); i < meta.field_num(); i++)
          fields.push_back(meta.field(i));
      } else {
        for (const auto &column : m.columns)
          fields.push_back(meta.field(column.c_str()));
      }
      return insert_rows(t, m, fields);
    }
    std::vector<Record> all;
    RecordScanner *raw = nullptr;
    RC rc = t->get_record_scanner(raw, trx_, ReadWriteMode::READ_ONLY);
    std::unique_ptr<RecordScanner> scanner(raw);
    if (rc != RC::SUCCESS)
      return rc;
    Record record;
    while ((rc = scanner->next(record)) == RC::SUCCESS) {
      Record copy;
      copy.copy_data(record.data(), meta.record_size());
      copy.rid() = record.rid();
      all.push_back(std::move(copy));
    }
    scanner->close_scan();
    if (rc != RC::RECORD_EOF)
      return rc;
    std::vector<Record> old, replacements;
    for (auto &record : all) {
      Row row;
      for (int i = meta.sys_field_num(); i < meta.field_num(); i++) {
        Value v;
        rc = t->read_value(record.data(), *meta.field(i), v);
        if (rc != RC::SUCCESS)
          return rc;
        row.push_back(v);
      }
      Context context{&row};
      if (m.where && !truth(eval(m.where, context)))
        continue;
      if (m.kind == "update") {
        Record next;
        next.copy_data(record.data(), meta.record_size());
        next.rid() = record.rid();
        for (size_t i = 0; i < m.columns.size(); i++) {
          auto *f = meta.field(m.columns[i].c_str());
          Value v = eval(m.values[i], context);
          validate_value(*f, v);
          rc = t->write_value(next.data(), *f, v);
          if (rc != RC::SUCCESS)
            return rc;
        }
        replacements.push_back(std::move(next));
      }
      old.push_back(std::move(record));
    }
    for (size_t i = 0; i < old.size(); i++) {
      rc = m.kind == "delete" ? trx_->delete_record(t, old[i]) : trx_->update_record(t, old[i], replacements[i]);
      if (rc != RC::SUCCESS) {
        if (m.kind == "update" && trx_->type() != TrxKit::MVCC)
          for (size_t j = i; j > 0; j--) {
            RC undo = trx_->update_record(t, replacements[j - 1], old[j - 1]);
            if (undo != RC::SUCCESS)
              return undo;
          }
        return rc;
      }
    }
    return RC::SUCCESS;
  }
  void bind(const Q &q, const Scopes &outer = {})
  {
    // Resolve names and validate subquery shapes even when an input is empty.
    if (q->bound)
      return;
    auto &s = q->select;
    std::unordered_set<std::string> aliases;
    for (auto &src : s.sources) {
      if (!aliases.insert(lower(src.alias)).second)
        throw Error("duplicate table alias");
      const auto &meta = table(src.name)->table_meta();
      for (int i = meta.sys_field_num(); i < meta.field_num(); i++) {
        const auto *field = meta.field(i);
        s.columns.push_back({src.alias, field->name(),
            field->type() == AttrType::TEXTS ? AttrType::CHARS : field->type()});
      }
    }
    std::vector<Item> expanded;
    for (auto &item : s.items) {
      if (item.expr->kind != Expr::STAR) {
        expanded.push_back(item);
        continue;
      }
      if (!item.alias.empty())
        throw Error("wildcard projection cannot have a column alias", RC::SQL_SYNTAX);
      bool any = false;
      for (auto &c : s.columns)
        if (item.expr->qualifier.empty() || lower(item.expr->qualifier) == lower(c.table)) {
          auto e       = node(Expr::FIELD);
          e->name      = c.name;
          e->qualifier = c.table;
          e->label     = c.name;
          expanded.push_back({e, ""});
          any = true;
        }
      if (!any)
        throw Error("unknown star qualifier");
    }
    s.items       = std::move(expanded);
    Scopes scopes = outer;
    scopes.insert(scopes.begin(), s.columns);
    for (auto &i : s.items)
      bind_expr(i.expr, scopes);
    for (auto &g : s.groups) {
      g = resolve_alias(g, s);
      bind_expr(g, scopes);
      if (has_aggregate(g))
        throw Error("aggregate in GROUP BY");
    }
    bind_expr(s.where, scopes);
    if (has_aggregate(s.where))
      throw Error("aggregate in WHERE");
    s.having = resolve_alias(s.having, s);
    bind_expr(s.having, scopes);
    for (auto &i : s.items)
      s.aggregate = s.aggregate || has_aggregate(i.expr);
    s.aggregate = s.aggregate || has_aggregate(s.having);
    if (s.aggregate || !s.groups.empty()) {
      for (auto &i : s.items)
        check_grouped(i.expr, s);
      check_grouped(s.having, s);
    }
    for (auto &i : s.items) {
      std::string name = i.alias;
      if (name.empty()) {
        if (i.expr->kind == Expr::FIELD) {
          auto &column = scopes[i.expr->depth][i.expr->index];
          name         = s.sources.size() > 1 ? column.table + "." + column.name : column.name;
        } else
          name = i.expr->label;
      }
      q->output.push_back({"", name, i.expr->type});
    }
    for (auto &o : q->order) {
      if (o.expr->kind == Expr::LITERAL && o.expr->value.attr_type() == AttrType::INTS) {
        int index = o.expr->value.get_int() - 1;
        if (index < 0 || index >= static_cast<int>(q->output.size()))
          throw Error("invalid ORDER BY position");
        o.output_index = index;
        continue;
      }
      if (o.expr->kind == Expr::FIELD && o.expr->qualifier.empty()) {
        for (size_t i = 0; i < q->output.size(); i++)
          if (lower(q->output[i].name) == lower(o.expr->name)) {
            if (o.output_index >= 0)
              throw Error("ambiguous ORDER BY alias");
            o.output_index = i;
          }
        if (o.output_index >= 0)
          continue;
      }
      o.expr = resolve_alias(o.expr, s);
      bind_expr(o.expr, scopes);
      s.aggregate = s.aggregate || has_aggregate(o.expr);
      if (s.aggregate || !s.groups.empty())
        check_grouped(o.expr, s);
    }
    if (s.aggregate)
      for (const auto &item : s.items)
        check_grouped(item.expr, s);
    q->correlated = external_query(q, 0);
    q->bound = true;
  }
  Result run(const Q &q, const Context &outer = {})
  {
    if (++recursion_ > 64) {
      recursion_--;
      throw Error("query nesting too deep");
    }
    struct Depth
    {
      int &v;
      ~Depth() { v--; }
    } guard{recursion_};
    auto rows = run_select(q->select, outer, q->order);
    if (!q->order.empty())
      std::stable_sort(rows.begin(), rows.end(), [&](const OutputRow &a, const OutputRow &b) {
        for (size_t i = 0; i < q->order.size(); i++) {
          auto &x = a.keys[i], &y = b.keys[i];
          int   c = is_null(x) ? (is_null(y) ? 0 : -1) : is_null(y) ? 1 : compare(x, y);
          if (c)
            return q->order[i].descending ? c > 0 : c < 0;
        }
        return false;
      });
    Result result;
    result.rows.reserve(rows.size());
    for (auto &row : rows)
      result.rows.push_back(std::move(row.values));
    return result;
  }
};

class QueryOperator : public PhysicalOperator
{
  std::unique_ptr<Engine>       engine_;
  Q                             query_;
  Result                        result_;
  size_t                        cursor_ = 0;
  ValueListTuple                tuple_;

public:
  QueryOperator(std::unique_ptr<Engine> engine, Q query) : engine_(std::move(engine)), query_(std::move(query)) {}
  PhysicalOperatorType type() const override { return PhysicalOperatorType::PROJECT; }
  OpType               get_op_type() const override { return OpType::PROJECTION; }
  RC                   tuple_schema(TupleSchema &schema) const override
  {
    for (auto &c : query_->output)
      schema.append_cell(c.name.c_str());
    return RC::SUCCESS;
  }
  RC open(Trx *trx) override
  {
    try {
      engine_->set_transaction(trx);
      result_ = engine_->run(query_);
      cursor_ = 0;
      return RC::SUCCESS;
    } catch (const Error &e) {
      LOG_WARN("query execution failed: %s",e.what());
      return e.rc;
    } catch (const std::exception &e) {
      LOG_WARN("query execution failed: %s",e.what());
      return RC::INTERNAL;
    }
  }
  RC next() override
  {
    if (cursor_ >= result_.rows.size())
      return RC::RECORD_EOF;
    tuple_.set_cells(result_.rows[cursor_++]);
    std::vector<TupleCellSpec> specs;
    for (auto &c : query_->output)
      specs.emplace_back(c.name);
    tuple_.set_names(specs);
    return RC::SUCCESS;
  }
  RC close() override
  {
    result_.rows.clear();
    return RC::SUCCESS;
  }
  Tuple *current_tuple() override { return &tuple_; }
};
class MutationOperator : public PhysicalOperator
{
  std::unique_ptr<Engine> engine_;
  Mutation                mutation_;
  RC                      status_ = RC::SUCCESS;

public:
  MutationOperator(std::unique_ptr<Engine> engine, Mutation mutation)
      : engine_(std::move(engine)), mutation_(std::move(mutation))
  {}
  PhysicalOperatorType type() const override { return PhysicalOperatorType::PROJECT; }
  OpType               get_op_type() const override { return OpType::PROJECTION; }
  RC                   tuple_schema(TupleSchema &) const override { return RC::SUCCESS; }
  RC                   open(Trx *trx) override
  {
    auto  *mvcc  = dynamic_cast<MvccTrx *>(trx);
    size_t point = mvcc ? mvcc->savepoint() : 0;
    try {
      engine_->set_transaction(trx);
      status_ = engine_->mutate(mutation_);
    } catch (const Error &e) {
      status_ = e.rc;
    } catch (const std::exception &) {
      status_ = RC::INTERNAL;
    }
    if (status_ != RC::SUCCESS && mvcc) {
      RC undo = mvcc->rollback_to(point);
      if (undo != RC::SUCCESS)
        status_ = undo;
    }
    return status_;
  }
  RC     next() override { return RC::RECORD_EOF; }
  RC     close() override { return status_; }
  Tuple *current_tuple() override { return nullptr; }
};
}  // namespace relational

RC prepare_relational_query(SQLStageEvent *event, bool &handled)
{
  handled = false;
  try {
    relational::Parser parser(event->sql());
    if (parser.is_mutation()) {
      handled       = true;
      auto mutation = parser.mutation();
      auto engine   = std::make_unique<relational::Engine>(event->session_event()->session()->get_current_db());
      engine->bind_mutation(mutation);
      event->session_event()->sql_result()->set_operator(
          std::make_unique<relational::MutationOperator>(std::move(engine), std::move(mutation)));
      return RC::SUCCESS;
    }
    if (!parser.is_select())
      return RC::SUCCESS;
    auto query  = parser.parse();
    auto engine = std::make_unique<relational::Engine>(event->session_event()->session()->get_current_db());
    engine->bind(query);
    if (!relational::extended_query(query))
      return RC::SUCCESS;
    handled = true;
    event->session_event()->sql_result()->set_operator(
        std::make_unique<relational::QueryOperator>(std::move(engine), query));
    return RC::SUCCESS;
  } catch (const relational::Error &e) {
    handled = true;
    LOG_WARN("query prepare failed: %s",e.what());
    return e.rc;
  } catch (const std::exception &e) {
    handled = true;
    LOG_WARN("query prepare failed: %s",e.what());
    return RC::SQL_SYNTAX;
  }
}
