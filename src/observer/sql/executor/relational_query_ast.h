// SPDX-License-Identifier: MulanPSL-2.0
#pragma once
#include "common/value.h"
#include <memory>
#include <stdexcept>
#include <vector>
#include <string>
#include <cctype>
#include <set>
namespace relational {
inline std::string lower(std::string s)
{
  for (char &c : s)
    c = std::tolower(static_cast<unsigned char>(c));
  return s;
}
struct Error : std::runtime_error
{
  RC rc;
  explicit Error(const std::string &s, RC code = RC::INVALID_ARGUMENT) : std::runtime_error(s), rc(code) {}
};
struct Query;
struct Expr;
using E   = std::shared_ptr<Expr>;
using Q   = std::shared_ptr<Query>;
using Row = std::vector<Value>;
struct Column
{
  std::string table, name;
  AttrType    type;
};
using Schema = std::vector<Column>;
struct Expr
{
  enum Kind
  {
    LITERAL,
    FIELD,
    STAR,
    UNARY,
    BINARY,
    FUNCTION,
    SCALAR,
    IN,
    EXISTS,
    IS_NULL
  } kind = LITERAL;
  std::string    op, qualifier, name, label;
  Value          value;
  std::vector<E> args;
  Q              query;
  int            depth = -1, index = -1;
  AttrType       type     = AttrType::UNDEFINED;
  bool           distinct = false, negative = false;
};
inline E node(Expr::Kind kind, std::string op = "", std::vector<E> args = {})
{
  auto e  = std::make_shared<Expr>();
  e->kind = kind;
  e->op   = std::move(op);
  e->args = std::move(args);
  return e;
}
struct Item
{
  E           expr;
  std::string alias;
};
struct Source
{
  std::string name, alias;
};
struct Select
{
  std::vector<Item>   items;
  std::vector<Source> sources;
  E                   where, having;
  std::vector<E>      groups;
  Schema              columns;
  bool                distinct = false, aggregate = false;
};
struct Order
{
  E    expr;
  bool descending   = false;
  int  output_index = -1;
};
struct Query
{
  Select              select;
  std::vector<Order>  order;
  Schema              output;
  bool                correlated = false, bound = false;
};
struct Mutation
{
  std::string              kind, table;
  std::vector<std::string> columns;
  std::vector<E>           values;
  std::vector<std::vector<E>> insert_rows;
  E                        where;
};
struct Token
{
  std::string text;
  enum Kind
  {
    WORD,
    NUMBER,
    STRING,
    SYMBOL,
    END
  } kind;
  size_t begin, end;
};
class Parser
{
  std::string        input_;
  std::vector<Token> t_;
  size_t             pos_     = 0;
  int                nesting_ = 0;
  bool               at(const std::string &s) const { return lower(t_[pos_].text) == s; }
  bool               take(const std::string &s)
  {
    if (at(s)) {
      pos_++;
      return true;
    }
    return false;
  }
  void need(const std::string &s)
  {
    if (!take(s))
      throw Error("expected " + s, RC::SQL_SYNTAX);
  }
  std::string identifier()
  {
    if (t_[pos_].kind != Token::WORD)
      throw Error("expected identifier", RC::SQL_SYNTAX);
    return t_[pos_++].text;
  }
  static bool reserved(const std::string &s)
  {
    static const std::set<std::string> words = {"from",
        "where",
        "inner",
        "join",
        "on",
        "left",
        "right",
        "full",
        "cross",
        "outer",
        "group",
        "having",
        "order",
        "union",
        "limit",
        "offset",
        "asc",
        "desc",
        "and",
        "or",
        "in",
        "is",
        "not",
        "as",
        "by"};
    return words.count(lower(s));
  }
  std::string alias()
  {
    if (take("as"))
      return identifier();
    if (t_[pos_].kind == Token::WORD && !reserved(t_[pos_].text))
      return identifier();
    return "";
  }
  std::string label(size_t start) const
  {
    std::string  out;
    bool         quoted = false;
    char         quote  = 0;
    const size_t end    = pos_ ? t_[pos_ - 1].end : 0;
    for (size_t i = t_[start].begin; i < end; i++) {
      char c = input_[i];
      if ((c == '\'' || c == '"') && (!quoted || c == quote)) {
        quoted = !quoted;
        quote  = c;
      }
      if (quoted || !std::isspace(static_cast<unsigned char>(c)))
        out += c;
    }
    return out;
  }
  Source source()
  {
    Source s;
    s.name = identifier();
    s.alias = alias();
    if (s.alias.empty())
      s.alias = s.name;
    return s;
  }
  Select select()
  {
    need("select");
    Select s;
    s.distinct = take("distinct");
    take("all");
    do {
      auto e = expression();
      s.items.push_back({e, alias()});
    } while (take(","));
    if (take("from")) {
      s.sources.push_back(source());
      while (true) {
        if (take(",")) {
          s.sources.push_back(source());
          continue;
        }
        bool cross = take("cross"), inner = take("inner");
        if (at("join")) {
          pos_++;
          s.sources.push_back(source());
          if (!cross) {
            need("on");
            auto e  = expression();
            s.where = s.where ? node(Expr::BINARY, "and", {s.where, e}) : e;
          }
          continue;
        }
        if (cross || inner)
          throw Error("expected JOIN", RC::SQL_SYNTAX);
        break;
      }
    }
    if (take("where")) {
      auto e  = expression();
      s.where = s.where ? node(Expr::BINARY, "and", {s.where, e}) : e;
    }
    if (take("group")) {
      need("by");
      do {
        s.groups.push_back(expression());
      } while (take(","));
    }
    if (take("having"))
      s.having = expression();
    return s;
  }
  int precedence() const
  {
    std::string s = lower(t_[pos_].text);
    if (s == "or")
      return 1;
    if (s == "and")
      return 2;
    if (s == "=" || s == "!=" || s == "<>" || s == "<" || s == "<=" || s == ">" || s == ">=" || s == "in" ||
        s == "is" || s == "like" || s == "between" || s == "not")
      return 3;
    if (s == "+" || s == "-")
      return 4;
    if (s == "*" || s == "/" || s == "%")
      return 5;
    return 0;
  }
  E primary()
  {
    if (++nesting_ > 128)
      throw Error("expression nesting too deep");
    const size_t start = pos_;
    E            e;
    if (take("not"))
      e = node(Expr::UNARY, "not", {expression(3)});
    else if (take("-") || take("+")) {
      std::string op = t_[pos_ - 1].text;
      e              = node(Expr::UNARY, op, {expression(6)});
    } else if (take("exists")) {
      need("(");
      e        = node(Expr::EXISTS);
      e->query = query();
      need(")");
    } else if (take("(")) {
      if (at("select")) {
        e        = node(Expr::SCALAR);
        e->query = query();
      } else
        e = expression();
      need(")");
    } else if (take("*"))
      e = node(Expr::STAR);
    else if (take("null")) {
      e = node(Expr::LITERAL);
      e->value.set_type(AttrType::NULLS);
    } else if (t_[pos_].kind == Token::NUMBER) {
      e      = node(Expr::LITERAL);
      auto s = t_[pos_++].text;
      try {
        if (s.find_first_of(".eE") != std::string::npos)
          e->value.set_float(std::stof(s));
        else
          e->value.set_int(std::stoi(s));
      } catch (...) {
        throw Error("invalid numeric literal");
      }
    } else if (t_[pos_].kind == Token::STRING) {
      e = node(Expr::LITERAL);
      e->value.set_string(t_[pos_++].text.c_str());
    } else if (t_[pos_].kind == Token::WORD) {
      auto name = identifier();
      if (take("(")) {
        e           = node(Expr::FUNCTION, lower(name));
        e->distinct = take("distinct");
        if (!at(")")) {
          do {
            e->args.push_back(expression());
          } while (take(","));
        }
        need(")");
      } else {
        e       = node(Expr::FIELD);
        e->name = name;
        if (take(".")) {
          e->qualifier = name;
          if (take("*"))
            e->kind = Expr::STAR;
          else
            e->name = identifier();
        }
      }
    } else
      throw Error("expected expression", RC::SQL_SYNTAX);
    e->label = label(start);
    nesting_--;
    return e;
  }
  E expression(int minimum = 1)
  {
    size_t start = pos_;
    E      left  = primary();
    while (precedence() >= minimum && precedence() != 0) {
      int         p   = precedence();
      std::string op  = lower(t_[pos_++].text);
      bool        neg = false;
      if (op == "not") {
        neg = true;
        if (take("in"))
          op = "in";
        else if (take("like"))
          op = "like";
        else if (take("between"))
          op = "between";
        else
          throw Error("unexpected NOT", RC::SQL_SYNTAX);
      }
      if (op == "is") {
        bool isnot = take("not");
        need("null");
        left           = node(Expr::IS_NULL, "", {left});
        left->negative = isnot;
      } else if (op == "in") {
        auto e      = node(Expr::IN, "", {left});
        e->negative = neg;
        need("(");
        if (at("select"))
          e->query = query();
        else {
          do {
            e->args.push_back(expression());
          } while (take(","));
        }
        need(")");
        left = e;
      } else if (op == "between") {
        E lo = expression(4);
        need("and");
        E hi = expression(4);
        left = node(Expr::BINARY, "and", {node(Expr::BINARY, ">=", {left, lo}), node(Expr::BINARY, "<=", {left, hi})});
        if (neg)
          left = node(Expr::UNARY, "not", {left});
      } else {
        left = node(Expr::BINARY, op, {left, expression(p + 1)});
        if (neg)
          left = node(Expr::UNARY, "not", {left});
      }
      left->label = label(start);
    }
    return left;
  }

public:
  explicit Parser(std::string input) : input_(std::move(input))
  {
    for (size_t i = 0; i < input_.size();) {
      unsigned char c = input_[i];
      if (std::isspace(c)) {
        i++;
        continue;
      }
      if (c == '-' && i + 1 < input_.size() && input_[i + 1] == '-') {
        while (i < input_.size() && input_[i] != '\n')
          i++;
        continue;
      }
      if (c == '/' && i + 1 < input_.size() && input_[i + 1] == '*') {
        auto end = input_.find("*/", i + 2);
        if (end == std::string::npos)
          throw Error("unterminated comment");
        i = end + 2;
        continue;
      }
      size_t begin = i;
      if (c == '\'' || c == '"' || c == '`') {
        char        quote = c;
        std::string text;
        i++;
        bool closed = false;
        while (i < input_.size()) {
          char ch = input_[i++];
          if (ch == quote) {
            if (i < input_.size() && input_[i] == quote) {
              text += ch;
              i++;
            } else {
              closed = true;
              break;
            }
          } else if (ch == '\\' && i < input_.size())
            text += input_[i++];
          else
            text += ch;
        }
        if (!closed)
          throw Error("unterminated quote", RC::SQL_SYNTAX);
        t_.push_back({text, quote == '`' ? Token::WORD : Token::STRING, begin, i});
      } else if (std::isdigit(c) ||
                 (c == '.' && i + 1 < input_.size() && std::isdigit(static_cast<unsigned char>(input_[i + 1])))) {
        i++;
        while (i < input_.size() && (std::isdigit(static_cast<unsigned char>(input_[i])) || input_[i] == '.'))
          i++;
        if (i < input_.size() && (input_[i] == 'e' || input_[i] == 'E')) {
          i++;
          if (i < input_.size() && (input_[i] == '+' || input_[i] == '-'))
            i++;
          while (i < input_.size() && std::isdigit(static_cast<unsigned char>(input_[i])))
            i++;
        }
        t_.push_back({input_.substr(begin, i - begin), Token::NUMBER, begin, i});
      } else if (std::isalpha(c) || c == '_' || c >= 128) {
        i++;
        while (i < input_.size() && (std::isalnum(static_cast<unsigned char>(input_[i])) || input_[i] == '_' ||
                                        static_cast<unsigned char>(input_[i]) >= 128))
          i++;
        t_.push_back({input_.substr(begin, i - begin), Token::WORD, begin, i});
      } else {
        std::string text(1, input_[i++]);
        if (i < input_.size() && ((text == "<" && (input_[i] == '=' || input_[i] == '>')) ||
                                     ((text == ">" || text == "!") && input_[i] == '=')))
          text += input_[i++];
        t_.push_back({text, Token::SYMBOL, begin, i});
      }
    }
    t_.push_back({"", Token::END, input_.size(), input_.size()});
  }
  bool is_select() const { return at("select"); }
  bool is_mutation() const { return at("insert") || at("update") || at("delete"); }

  Mutation mutation()
  {
    Mutation m;
    m.kind = lower(identifier());
    if (m.kind == "insert") {
      need("into");
      m.table = identifier();
      if (take("(")) {
        do {
          m.columns.push_back(identifier());
        } while (take(","));
        need(")");
      }
      need("values");
      do {
        need("(");
        auto &row = m.insert_rows.emplace_back();
        do {
          row.push_back(expression());
        } while (take(","));
        need(")");
      } while (take(","));
    } else if (m.kind == "update") {
      m.table = identifier();
      need("set");
      do {
        m.columns.push_back(identifier());
        need("=");
        m.values.push_back(expression());
      } while (take(","));
      if (take("where"))
        m.where = expression();
    } else {
      need("from");
      m.table = identifier();
      if (take("where"))
        m.where = expression();
    }
    take(";");
    if (t_[pos_].kind != Token::END)
      throw Error("unexpected mutation token", RC::SQL_SYNTAX);
    return m;
  }
  Q query()
  {
    auto q = std::make_shared<Query>();
    q->select = select();
    if (take("order")) {
      need("by");
      do {
        Order o;
        o.expr       = expression();
        o.descending = take("desc");
        if (!o.descending)
          take("asc");
        q->order.push_back(o);
      } while (take(","));
    }
    return q;
  }
  Q parse()
  {
    auto q = query();
    take(";");
    if (t_[pos_].kind != Token::END)
      throw Error("unexpected token " + t_[pos_].text, RC::SQL_SYNTAX);
    return q;
  }
};
}  // namespace relational
