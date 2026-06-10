#pragma once
//
// query.hpp -- lexer, AST and recursive-descent parser for the query language.
//
// Grammar (case-insensitive keywords):
//
//   query     := SELECT items FROM tableRef [join] [WHERE expr]
//                [GROUP BY colRef ("," colRef)*]
//                [ORDER BY orderItem ("," orderItem)*]
//                [LIMIT number] [";"]
//   items     := "*" | item ("," item)*
//   item      := aggName "(" ("*" | colRef) ")" [[AS] ident]
//              | colRef [[AS] ident]
//   aggName   := COUNT | SUM | AVG | MIN | MAX
//   tableRef  := ident [[AS] ident]
//   join      := [INNER] JOIN tableRef ON colRef "=" colRef
//   expr      := andExpr (OR andExpr)*
//   andExpr   := notExpr (AND notExpr)*
//   notExpr   := NOT notExpr | "(" expr ")" | operand cmpOp operand
//   operand   := colRef | number | string | TRUE | FALSE
//   cmpOp     := "=" | "==" | "!=" | "<>" | "<" | "<=" | ">" | ">="
//   orderItem := colRef [ASC | DESC]
//   colRef    := ident ["." ident]
//
// String literals use single quotes with '' as the escape. "--" starts a
// line comment.

#include <cctype>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "table.hpp"  // toLower / ciEquals
#include "value.hpp"

namespace minidb {

class QueryError : public std::runtime_error {
public:
    explicit QueryError(const std::string& msg) : std::runtime_error("query error: " + msg) {}
};

// ---------------------------------------------------------------- lexer ----

struct Token {
    enum class Kind { Ident, Number, String, Symbol, End };
    Kind kind = Kind::End;
    std::string text;  // identifier / symbol text
    Value value;       // literal value for Number / String
    size_t pos = 0;    // offset in the source, for error messages
};

class Lexer {
public:
    static std::vector<Token> tokenize(const std::string& src) {
        std::vector<Token> toks;
        size_t i = 0;
        const size_t n = src.size();
        auto isIdentStart = [](char c) {
            return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
        };
        auto isIdentChar = [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
        };

        while (i < n) {
            char c = src[i];
            if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }

            // line comment
            if (c == '-' && i + 1 < n && src[i + 1] == '-') {
                while (i < n && src[i] != '\n') ++i;
                continue;
            }

            Token t;
            t.pos = i;

            if (isIdentStart(c)) {
                size_t b = i;
                while (i < n && isIdentChar(src[i])) ++i;
                t.kind = Token::Kind::Ident;
                t.text = src.substr(b, i - b);
            } else if (std::isdigit(static_cast<unsigned char>(c)) ||
                       (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(src[i + 1])))) {
                size_t b = i;
                bool isFloat = false;
                while (i < n && std::isdigit(static_cast<unsigned char>(src[i]))) ++i;
                if (i < n && src[i] == '.') {
                    isFloat = true;
                    ++i;
                    while (i < n && std::isdigit(static_cast<unsigned char>(src[i]))) ++i;
                }
                if (i < n && (src[i] == 'e' || src[i] == 'E')) {
                    isFloat = true;
                    ++i;
                    if (i < n && (src[i] == '+' || src[i] == '-')) ++i;
                    while (i < n && std::isdigit(static_cast<unsigned char>(src[i]))) ++i;
                }
                std::string num = src.substr(b, i - b);
                t.kind = Token::Kind::Number;
                t.text = num;
                long long iv;
                if (!isFloat && tryParseInt(num, iv)) t.value = Value::ofInt(iv);
                else t.value = Value::ofFloat(std::strtod(num.c_str(), nullptr));
            } else if (c == '\'') {
                ++i;
                std::string s;
                bool closed = false;
                while (i < n) {
                    if (src[i] == '\'') {
                        if (i + 1 < n && src[i + 1] == '\'') { s.push_back('\''); i += 2; continue; }
                        ++i;
                        closed = true;
                        break;
                    }
                    s.push_back(src[i++]);
                }
                if (!closed) throw QueryError("unterminated string literal at offset " +
                                              std::to_string(t.pos));
                t.kind = Token::Kind::String;
                t.text = s;
                t.value = Value::ofString(s);
            } else {
                // two-character operators first
                static const char* twos[] = {"<=", ">=", "!=", "<>", "=="};
                std::string sym;
                for (const char* s2 : twos) {
                    if (src.compare(i, 2, s2) == 0) { sym = s2; break; }
                }
                if (sym.empty()) {
                    static const std::string singles = "=<>(),.*;-";
                    if (singles.find(c) == std::string::npos)
                        throw QueryError(std::string("unexpected character '") + c +
                                         "' at offset " + std::to_string(i));
                    sym = std::string(1, c);
                }
                i += sym.size();
                t.kind = Token::Kind::Symbol;
                // normalize operator spellings
                if (sym == "==") sym = "=";
                if (sym == "<>") sym = "!=";
                t.text = sym;
            }
            toks.push_back(std::move(t));
        }
        Token end;
        end.kind = Token::Kind::End;
        end.pos = n;
        toks.push_back(end);
        return toks;
    }
};

// ------------------------------------------------------------------ AST ----

struct ColumnRef {
    std::string table;   // optional qualifier (table name or alias)
    std::string column;
    std::string display() const { return table.empty() ? column : table + "." + column; }
};

struct Expr {
    enum class Kind { Literal, Column, Cmp, And, Or, Not };
    Kind kind = Kind::Literal;
    Value lit;                       // Literal
    ColumnRef col;                   // Column
    std::string op;                  // Cmp: = != < <= > >=
    std::unique_ptr<Expr> lhs, rhs;  // Cmp/And/Or (Not uses lhs only)
};

struct SelectItem {
    enum class Kind { Star, Column, Aggregate };
    Kind kind = Kind::Column;
    ColumnRef col;       // column, or aggregate argument
    std::string aggFn;   // lowercase: count/sum/avg/min/max
    bool aggStar = false;
    std::string alias;

    std::string display() const {
        if (!alias.empty()) return alias;
        if (kind == Kind::Aggregate) {
            std::string fn = aggFn;
            for (auto& ch : fn) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            return fn + "(" + (aggStar ? "*" : col.display()) + ")";
        }
        return col.column;
    }
};

struct TableRef {
    std::string table;
    std::string alias;  // empty if none
};

struct JoinClause {
    TableRef ref;
    ColumnRef left, right;  // ON left = right
};

struct OrderItem {
    ColumnRef col;
    bool desc = false;
};

struct Query {
    std::vector<SelectItem> items;
    TableRef from;
    std::optional<JoinClause> join;
    std::unique_ptr<Expr> where;
    std::vector<ColumnRef> groupBy;
    std::vector<OrderItem> orderBy;
    long long limit = -1;  // -1 = no limit
};

// --------------------------------------------------------------- parser ----

class Parser {
public:
    static Query parse(const std::string& sql) {
        Parser p(sql);
        return p.parseQuery();
    }

private:
    std::vector<Token> toks_;
    size_t pos_ = 0;

    explicit Parser(const std::string& sql) : toks_(Lexer::tokenize(sql)) {}

    const Token& peek() const { return toks_[pos_]; }
    Token next() { return toks_[pos_++]; }

    static bool isKeyword(const std::string& ident) {
        static const char* kws[] = {"select", "from",  "where", "group", "by",
                                    "order",  "limit", "join",  "inner", "on",
                                    "as",     "and",   "or",    "not",   "asc", "desc"};
        std::string l = toLower(ident);
        for (const char* k : kws)
            if (l == k) return true;
        return false;
    }

    bool isKw(const char* kw) const {
        return peek().kind == Token::Kind::Ident && ciEquals(peek().text, kw);
    }
    bool acceptKw(const char* kw) {
        if (isKw(kw)) { ++pos_; return true; }
        return false;
    }
    void expectKw(const char* kw) {
        if (!acceptKw(kw))
            throw QueryError(std::string("expected ") + kw + " but found '" +
                             describe(peek()) + "'");
    }
    bool isSym(const char* s) const {
        return peek().kind == Token::Kind::Symbol && peek().text == s;
    }
    bool acceptSym(const char* s) {
        if (isSym(s)) { ++pos_; return true; }
        return false;
    }
    void expectSym(const char* s) {
        if (!acceptSym(s))
            throw QueryError(std::string("expected '") + s + "' but found '" +
                             describe(peek()) + "'");
    }
    std::string expectIdent(const char* what) {
        if (peek().kind != Token::Kind::Ident)
            throw QueryError(std::string("expected ") + what + " but found '" +
                             describe(peek()) + "'");
        return next().text;
    }
    static std::string describe(const Token& t) {
        return t.kind == Token::Kind::End ? "end of query" : t.text;
    }

    Query parseQuery() {
        Query q;
        expectKw("select");
        if (acceptSym("*")) {
            SelectItem it;
            it.kind = SelectItem::Kind::Star;
            q.items.push_back(std::move(it));
        } else {
            q.items.push_back(parseSelectItem());
            while (acceptSym(",")) q.items.push_back(parseSelectItem());
        }

        expectKw("from");
        q.from = parseTableRef();

        if (isKw("inner") || isKw("join")) {
            acceptKw("inner");
            expectKw("join");
            JoinClause j;
            j.ref = parseTableRef();
            expectKw("on");
            j.left = parseColumnRef();
            expectSym("=");
            j.right = parseColumnRef();
            q.join = std::move(j);
        }

        if (acceptKw("where")) q.where = parseOr();

        if (acceptKw("group")) {
            expectKw("by");
            q.groupBy.push_back(parseColumnRef());
            while (acceptSym(",")) q.groupBy.push_back(parseColumnRef());
        }

        if (acceptKw("order")) {
            expectKw("by");
            q.orderBy.push_back(parseOrderItem());
            while (acceptSym(",")) q.orderBy.push_back(parseOrderItem());
        }

        if (acceptKw("limit")) {
            if (peek().kind != Token::Kind::Number || peek().value.type() != Type::Int ||
                peek().value.getInt() < 0)
                throw QueryError("LIMIT expects a non-negative integer");
            q.limit = next().value.getInt();
        }

        acceptSym(";");
        if (peek().kind != Token::Kind::End)
            throw QueryError("unexpected token '" + describe(peek()) + "' at end of query");
        return q;
    }

    SelectItem parseSelectItem() {
        SelectItem it;
        if (peek().kind == Token::Kind::Ident) {
            std::string fn = toLower(peek().text);
            bool isAgg = (fn == "count" || fn == "sum" || fn == "avg" ||
                          fn == "min" || fn == "max");
            if (isAgg && toks_[pos_ + 1].kind == Token::Kind::Symbol &&
                toks_[pos_ + 1].text == "(") {
                ++pos_;  // fn
                ++pos_;  // (
                it.kind = SelectItem::Kind::Aggregate;
                it.aggFn = fn;
                if (acceptSym("*")) {
                    if (fn != "count") throw QueryError(fn + "(*) is not supported; only COUNT(*)");
                    it.aggStar = true;
                } else {
                    it.col = parseColumnRef();
                }
                expectSym(")");
                it.alias = parseOptionalAlias();
                return it;
            }
        }
        it.kind = SelectItem::Kind::Column;
        it.col = parseColumnRef();
        it.alias = parseOptionalAlias();
        return it;
    }

    std::string parseOptionalAlias() {
        if (acceptKw("as")) return expectIdent("alias after AS");
        if (peek().kind == Token::Kind::Ident && !isKeyword(peek().text)) return next().text;
        return "";
    }

    TableRef parseTableRef() {
        TableRef r;
        r.table = expectIdent("table name");
        r.alias = parseOptionalAlias();
        return r;
    }

    ColumnRef parseColumnRef() {
        ColumnRef c;
        c.column = expectIdent("column name");
        if (acceptSym(".")) {
            c.table = c.column;
            c.column = expectIdent("column name after '.'");
        }
        return c;
    }

    OrderItem parseOrderItem() {
        OrderItem o;
        o.col = parseColumnRef();
        if (acceptKw("desc")) o.desc = true;
        else acceptKw("asc");
        return o;
    }

    // expr := andExpr (OR andExpr)*
    std::unique_ptr<Expr> parseOr() {
        auto l = parseAnd();
        while (acceptKw("or")) {
            auto e = std::make_unique<Expr>();
            e->kind = Expr::Kind::Or;
            e->lhs = std::move(l);
            e->rhs = parseAnd();
            l = std::move(e);
        }
        return l;
    }

    std::unique_ptr<Expr> parseAnd() {
        auto l = parseNot();
        while (acceptKw("and")) {
            auto e = std::make_unique<Expr>();
            e->kind = Expr::Kind::And;
            e->lhs = std::move(l);
            e->rhs = parseNot();
            l = std::move(e);
        }
        return l;
    }

    std::unique_ptr<Expr> parseNot() {
        if (acceptKw("not")) {
            auto e = std::make_unique<Expr>();
            e->kind = Expr::Kind::Not;
            e->lhs = parseNot();
            return e;
        }
        if (acceptSym("(")) {
            auto e = parseOr();
            expectSym(")");
            return e;
        }
        return parseComparison();
    }

    std::unique_ptr<Expr> parseComparison() {
        auto l = parseOperand();
        if (peek().kind != Token::Kind::Symbol ||
            (peek().text != "=" && peek().text != "!=" && peek().text != "<" &&
             peek().text != "<=" && peek().text != ">" && peek().text != ">="))
            throw QueryError("expected comparison operator but found '" + describe(peek()) + "'");
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::Cmp;
        e->op = next().text;
        e->lhs = std::move(l);
        e->rhs = parseOperand();
        return e;
    }

    std::unique_ptr<Expr> parseOperand() {
        auto e = std::make_unique<Expr>();
        if (peek().kind == Token::Kind::Number || peek().kind == Token::Kind::String) {
            e->kind = Expr::Kind::Literal;
            e->lit = next().value;
            return e;
        }
        if (acceptSym("-")) {
            if (peek().kind != Token::Kind::Number)
                throw QueryError("expected number after unary '-'");
            Value v = next().value;
            e->kind = Expr::Kind::Literal;
            e->lit = (v.type() == Type::Int) ? Value::ofInt(-v.getInt())
                                             : Value::ofFloat(-v.getFloat());
            return e;
        }
        if (peek().kind == Token::Kind::Ident) {
            std::string l = toLower(peek().text);
            if (l == "true" || l == "false") {
                ++pos_;
                e->kind = Expr::Kind::Literal;
                e->lit = Value::ofBool(l == "true");
                return e;
            }
            e->kind = Expr::Kind::Column;
            e->col = parseColumnRef();
            return e;
        }
        throw QueryError("expected a column or literal but found '" + describe(peek()) + "'");
    }
};

}  // namespace minidb
