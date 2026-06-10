#pragma once
//
// executor.hpp -- the Database catalog and the query execution engine.
//
// Execution pipeline:
//   1. FROM / JOIN  -> a stream of source rows (hash join when joining)
//   2. WHERE        -> filter (uses a hash index for AND-ed equality
//                      predicates on an indexed column when available)
//   3. GROUP BY / aggregates -> one output row per group
//   4. ORDER BY     -> stable sort
//   5. LIMIT / projection -> final ResultSet
//
// Plan decisions (full scan vs index lookup, join strategy) are reported in
// ResultSet::notes so the CLI can show what the engine actually did.

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "query.hpp"
#include "table.hpp"

namespace minidb {

struct ResultSet {
    std::vector<std::string> columns;
    std::vector<std::vector<Value>> rows;
    std::vector<std::string> notes;  // execution plan notes
};

struct CiLess {
    bool operator()(const std::string& a, const std::string& b) const {
        return toLower(a) < toLower(b);
    }
};

class Database {
public:
    Table& addTable(Table t) {
        std::string key = t.name;
        return tables_.insert_or_assign(key, std::move(t)).first->second;
    }

    Table& loadCsvFile(const std::string& path, std::string name) {
        return addTable(Table::fromCsvFile(path, std::move(name)));
    }

    Table* findTable(const std::string& name) {
        auto it = tables_.find(name);
        return it == tables_.end() ? nullptr : &it->second;
    }

    std::vector<std::string> tableNames() const {
        std::vector<std::string> out;
        for (const auto& [k, t] : tables_) out.push_back(t.name);
        return out;
    }

    ResultSet execute(const std::string& sql) {
        Query q = Parser::parse(sql);
        return run(q);
    }

private:
    std::map<std::string, Table, CiLess> tables_;

    // A source row is a pair of row ids into the left / right table
    // (right unused without a join). Avoids materializing joined rows.
    struct RowSrc {
        size_t l = 0, r = 0;
    };

    ResultSet run(const Query& q) {
        ResultSet rs;

        Table* left = findTable(q.from.table);
        if (!left) throw QueryError("unknown table '" + q.from.table + "'");
        const std::string lAlias = q.from.alias.empty() ? left->name : q.from.alias;

        Table* right = nullptr;
        std::string rAlias;
        if (q.join) {
            right = findTable(q.join->ref.table);
            if (!right) throw QueryError("unknown table '" + q.join->ref.table + "'");
            rAlias = q.join->ref.alias.empty() ? right->name : q.join->ref.alias;
        }
        const int leftN = static_cast<int>(left->columns.size());
        const int totalN = leftN + (right ? static_cast<int>(right->columns.size()) : 0);

        // ---- column resolution over the combined scope -------------------
        auto resolveOpt = [&](const ColumnRef& cr) -> int {
            if (!cr.table.empty()) {
                if (ciEquals(cr.table, lAlias) || ciEquals(cr.table, left->name))
                    return left->findColumn(cr.column);
                if (right && (ciEquals(cr.table, rAlias) || ciEquals(cr.table, right->name))) {
                    int c = right->findColumn(cr.column);
                    return c < 0 ? -1 : leftN + c;
                }
                throw QueryError("unknown table or alias '" + cr.table + "'");
            }
            int lc = left->findColumn(cr.column);
            int rc = right ? right->findColumn(cr.column) : -1;
            if (lc >= 0 && rc >= 0)
                throw QueryError("column '" + cr.column + "' is ambiguous; qualify it");
            if (lc >= 0) return lc;
            if (rc >= 0) return leftN + rc;
            return -1;
        };
        auto resolve = [&](const ColumnRef& cr) -> int {
            int c = resolveOpt(cr);
            if (c < 0) throw QueryError("unknown column '" + cr.display() + "'");
            return c;
        };
        auto cellOf = [&](const RowSrc& s, int c) -> const Value& {
            return c < leftN ? left->rows[s.l][c] : right->rows[s.r][c - leftN];
        };
        auto scopeColumnName = [&](int c) -> std::string {
            return c < leftN ? left->columns[c].name : right->columns[c - leftN].name;
        };
        auto scopeColumnType = [&](int c) -> Type {
            return c < leftN ? left->columns[c].type : right->columns[c - leftN].type;
        };

        // ---- 1. FROM / JOIN: produce candidate source rows ----------------
        std::vector<RowSrc> src;
        if (!right) {
            // Try to satisfy one AND-ed `col = literal` predicate from a
            // hash index instead of scanning every row.
            std::vector<const Expr*> eqs;
            collectEqualities(q.where.get(), eqs);
            const HashIndex* usedIdx = nullptr;
            std::string idxNote;
            for (const Expr* e : eqs) {
                const Expr* colE = e->lhs->kind == Expr::Kind::Column ? e->lhs.get() : e->rhs.get();
                const Expr* litE = e->lhs->kind == Expr::Kind::Literal ? e->lhs.get() : e->rhs.get();
                int c = resolve(colE->col);
                if (c >= leftN) continue;
                const HashIndex* idx = left->getIndex(c);
                if (!idx) continue;
                usedIdx = idx;
                auto it = idx->find(litE->lit.toString());
                if (it != idx->end())
                    for (size_t rid : it->second) src.push_back({rid, 0});
                idxNote = "index lookup on " + left->name + "." + left->columns[c].name +
                          " (" + std::to_string(src.size()) + " of " +
                          std::to_string(left->rows.size()) + " rows)";
                break;
            }
            if (usedIdx) {
                rs.notes.push_back(idxNote);
            } else {
                src.reserve(left->rows.size());
                for (size_t i = 0; i < left->rows.size(); ++i) src.push_back({i, 0});
                rs.notes.push_back("full scan of " + left->name + " (" +
                                   std::to_string(left->rows.size()) + " rows)");
            }
        } else {
            // Hash join: build (or reuse) a hash table on the right join key,
            // probe with each left row. O(L + R) instead of O(L * R).
            int lc = resolve(q.join->left);
            int rc = resolve(q.join->right);
            if (lc >= leftN && rc < leftN) std::swap(lc, rc);
            if (!(lc < leftN && rc >= leftN))
                throw QueryError("JOIN ... ON must reference one column from each table");
            rc -= leftN;

            const HashIndex* idx = right->getIndex(rc);
            HashIndex temp;
            std::string how;
            if (idx) {
                how = "reusing existing index";
            } else {
                temp.reserve(right->rows.size());
                for (size_t i = 0; i < right->rows.size(); ++i) {
                    const Value& v = right->rows[i][rc];
                    if (!v.isNull()) temp[v.toString()].push_back(i);
                }
                idx = &temp;
                how = "built temporary hash table";
            }
            for (size_t i = 0; i < left->rows.size(); ++i) {
                const Value& v = left->rows[i][lc];
                if (v.isNull()) continue;
                auto it = idx->find(v.toString());
                if (it == idx->end()) continue;
                for (size_t rid : it->second) src.push_back({i, rid});
            }
            rs.notes.push_back("hash join " + left->name + " \xE2\x8B\x88 " + right->name +
                               " on " + right->columns[rc].name + " (" + how + ", " +
                               std::to_string(src.size()) + " matches)");
        }

        // ---- 2. WHERE: evaluate the full predicate -------------------------
        // (also re-checks index candidates, so index lookups can never change
        // the result, only speed it up)
        if (q.where) {
            auto pred = compilePredicate(*q.where, resolve, cellOf);
            std::vector<RowSrc> kept;
            kept.reserve(src.size());
            for (const RowSrc& s : src)
                if (pred(s)) kept.push_back(s);
            src = std::move(kept);
        }

        // ---- 3. aggregation / projection -----------------------------------
        bool hasAgg = false;
        for (const auto& it : q.items)
            if (it.kind == SelectItem::Kind::Aggregate) hasAgg = true;

        if (hasAgg || !q.groupBy.empty()) {
            runAggregation(q, src, rs, resolve, cellOf);
        } else {
            // ORDER BY before projection so non-selected columns can be keys.
            if (!q.orderBy.empty()) {
                std::vector<std::pair<int, bool>> keys;
                for (const auto& o : q.orderBy) {
                    int c = resolveOpt(o.col);
                    if (c < 0) {
                        // maybe it names a select alias -> sort by that column
                        for (const auto& it : q.items)
                            if (it.kind == SelectItem::Kind::Column &&
                                ciEquals(it.alias, o.col.column)) {
                                c = resolve(it.col);
                                break;
                            }
                    }
                    if (c < 0) throw QueryError("unknown ORDER BY column '" + o.col.display() + "'");
                    keys.push_back({c, o.desc});
                }
                std::stable_sort(src.begin(), src.end(), [&](const RowSrc& a, const RowSrc& b) {
                    for (auto [c, desc] : keys) {
                        int cmp = orderCompare(cellOf(a, c), cellOf(b, c));
                        if (cmp != 0) return desc ? cmp > 0 : cmp < 0;
                    }
                    return false;
                });
            }
            if (q.limit >= 0 && src.size() > static_cast<size_t>(q.limit))
                src.resize(static_cast<size_t>(q.limit));

            // projection
            std::vector<int> outCols;
            if (q.items.size() == 1 && q.items[0].kind == SelectItem::Kind::Star) {
                // duplicate names across joined tables get qualified
                std::map<std::string, int, CiLess> nameCount;
                for (int c = 0; c < totalN; ++c) ++nameCount[scopeColumnName(c)];
                for (int c = 0; c < totalN; ++c) {
                    outCols.push_back(c);
                    std::string nm = scopeColumnName(c);
                    if (nameCount[nm] > 1)
                        nm = (c < leftN ? lAlias : rAlias) + "." + nm;
                    rs.columns.push_back(nm);
                }
            } else {
                for (const auto& it : q.items) {
                    outCols.push_back(resolve(it.col));
                    rs.columns.push_back(it.display());
                }
            }
            rs.rows.reserve(src.size());
            for (const RowSrc& s : src) {
                std::vector<Value> row;
                row.reserve(outCols.size());
                for (int c : outCols) row.push_back(cellOf(s, c));
                rs.rows.push_back(std::move(row));
            }
        }
        (void)scopeColumnType;
        return rs;
    }

    // Collect AND-ed `column = literal` comparisons (candidates for an
    // index lookup). OR / NOT subtrees are skipped -- an index can only be
    // used when the predicate is a conjunction containing the equality.
    static void collectEqualities(const Expr* e, std::vector<const Expr*>& out) {
        if (!e) return;
        if (e->kind == Expr::Kind::And) {
            collectEqualities(e->lhs.get(), out);
            collectEqualities(e->rhs.get(), out);
            return;
        }
        if (e->kind == Expr::Kind::Cmp && e->op == "=") {
            bool a = e->lhs->kind == Expr::Kind::Column && e->rhs->kind == Expr::Kind::Literal;
            bool b = e->lhs->kind == Expr::Kind::Literal && e->rhs->kind == Expr::Kind::Column;
            if (a || b) out.push_back(e);
        }
    }

    // Compile the WHERE tree into a closure tree. Columns are resolved once
    // here instead of per row, which matters when scanning large tables.
    template <typename Resolve, typename CellOf>
    static std::function<bool(const RowSrc&)> compilePredicate(const Expr& e, Resolve& resolve,
                                                               CellOf& cellOf) {
        using Pred = std::function<bool(const RowSrc&)>;
        switch (e.kind) {
        case Expr::Kind::And: {
            Pred l = compilePredicate(*e.lhs, resolve, cellOf);
            Pred r = compilePredicate(*e.rhs, resolve, cellOf);
            return [l, r](const RowSrc& s) { return l(s) && r(s); };
        }
        case Expr::Kind::Or: {
            Pred l = compilePredicate(*e.lhs, resolve, cellOf);
            Pred r = compilePredicate(*e.rhs, resolve, cellOf);
            return [l, r](const RowSrc& s) { return l(s) || r(s); };
        }
        case Expr::Kind::Not: {
            Pred l = compilePredicate(*e.lhs, resolve, cellOf);
            return [l](const RowSrc& s) { return !l(s); };
        }
        case Expr::Kind::Cmp: {
            struct Operand {
                bool isCol = false;
                int col = -1;
                Value lit;
            };
            auto compileOperand = [&](const Expr& o) {
                Operand op;
                if (o.kind == Expr::Kind::Column) {
                    op.isCol = true;
                    op.col = resolve(o.col);
                } else if (o.kind == Expr::Kind::Literal) {
                    op.lit = o.lit;
                } else {
                    throw QueryError("comparison operands must be columns or literals");
                }
                return op;
            };
            Operand a = compileOperand(*e.lhs);
            Operand b = compileOperand(*e.rhs);
            std::string op = e.op;
            CellOf cell = cellOf;
            return [a, b, op, cell](const RowSrc& s) {
                const Value& x = a.isCol ? cell(s, a.col) : a.lit;
                const Value& y = b.isCol ? cell(s, b.col) : b.lit;
                if (x.isNull() || y.isNull()) return false;  // SQL: NULL never matches
                auto c = compareValues(x, y);
                if (!c)
                    throw QueryError(std::string("cannot compare ") + typeName(x.type()) +
                                     " with " + typeName(y.type()));
                if (op == "=") return *c == 0;
                if (op == "!=") return *c != 0;
                if (op == "<") return *c < 0;
                if (op == "<=") return *c <= 0;
                if (op == ">") return *c > 0;
                return *c >= 0;  // ">="
            };
        }
        default:
            throw QueryError("WHERE clause must be a boolean expression");
        }
    }

    template <typename Resolve, typename CellOf>
    void runAggregation(const Query& q, const std::vector<RowSrc>& src, ResultSet& rs,
                        Resolve& resolve, CellOf& cellOf) {
        // Resolve GROUP BY keys.
        std::vector<int> keyCols;
        for (const auto& g : q.groupBy) keyCols.push_back(resolve(g));

        // Validate select items and resolve their columns.
        struct Item {
            const SelectItem* sel;
            int col = -1;  // source column (group key or aggregate argument)
        };
        std::vector<Item> items;
        for (const auto& it : q.items) {
            if (it.kind == SelectItem::Kind::Star)
                throw QueryError("SELECT * cannot be combined with aggregation / GROUP BY");
            Item item{&it, -1};
            if (it.kind == SelectItem::Kind::Column) {
                item.col = resolve(it.col);
                bool inKeys = false;
                for (int k : keyCols)
                    if (k == item.col) inKeys = true;
                if (!inKeys)
                    throw QueryError("column '" + it.col.display() +
                                     "' must appear in GROUP BY or inside an aggregate");
            } else if (!it.aggStar) {
                item.col = resolve(it.col);
            }
            items.push_back(item);
            rs.columns.push_back(it.display());
        }

        // Group rows. Key = concatenated canonical strings of the key values.
        struct Acc {
            long long count = 0;
            double sumD = 0;
            long long sumI = 0;
            bool intSum = true;
            Value best;  // for MIN / MAX
        };
        struct Group {
            RowSrc repr;
            std::vector<Acc> accs;
        };
        std::map<std::string, size_t> groupIdx;  // ordered for deterministic output
        std::vector<Group> groups;

        for (const RowSrc& s : src) {
            std::string key;
            for (int k : keyCols) {
                key += cellOf(s, k).toString();
                key += '\x1f';
            }
            auto [it, inserted] = groupIdx.try_emplace(key, groups.size());
            if (inserted) {
                Group g;
                g.repr = s;
                g.accs.resize(items.size());
                groups.push_back(std::move(g));
            }
            Group& g = groups[it->second];

            for (size_t i = 0; i < items.size(); ++i) {
                const SelectItem& sel = *items[i].sel;
                if (sel.kind != SelectItem::Kind::Aggregate) continue;
                Acc& a = g.accs[i];
                if (sel.aggStar) {  // COUNT(*)
                    ++a.count;
                    continue;
                }
                const Value& v = cellOf(s, items[i].col);
                if (v.isNull()) continue;  // SQL aggregates ignore NULLs
                if (sel.aggFn == "count") {
                    ++a.count;
                } else if (sel.aggFn == "sum" || sel.aggFn == "avg") {
                    if (!v.isNumeric())
                        throw QueryError(sel.aggFn + "() requires a numeric column, but '" +
                                         sel.col.display() + "' is " + typeName(v.type()));
                    ++a.count;
                    a.sumD += v.asDouble();
                    if (v.type() == Type::Int && a.intSum) a.sumI += v.getInt();
                    else a.intSum = false;
                } else {  // min / max
                    if (a.count == 0) a.best = v;
                    else {
                        int c = orderCompare(v, a.best);
                        if ((sel.aggFn == "min" && c < 0) || (sel.aggFn == "max" && c > 0))
                            a.best = v;
                    }
                    ++a.count;
                }
            }
        }

        // Build one output row per group.
        for (const Group& g : groups) {
            std::vector<Value> row;
            row.reserve(items.size());
            for (size_t i = 0; i < items.size(); ++i) {
                const SelectItem& sel = *items[i].sel;
                const Acc& a = g.accs[i];
                if (sel.kind == SelectItem::Kind::Column) {
                    row.push_back(cellOf(g.repr, items[i].col));
                } else if (sel.aggFn == "count") {
                    row.push_back(Value::ofInt(a.count));
                } else if (sel.aggFn == "sum") {
                    if (a.count == 0) row.emplace_back();  // SUM of nothing -> NULL
                    else row.push_back(a.intSum ? Value::ofInt(a.sumI) : Value::ofFloat(a.sumD));
                } else if (sel.aggFn == "avg") {
                    if (a.count == 0) row.emplace_back();
                    else row.push_back(Value::ofFloat(a.sumD / static_cast<double>(a.count)));
                } else {  // min / max
                    if (a.count == 0) row.emplace_back();
                    else row.push_back(a.best);
                }
            }
            rs.rows.push_back(std::move(row));
        }

        // ORDER BY against the output columns (so aliases and aggregate
        // results can be sort keys).
        if (!q.orderBy.empty()) {
            std::vector<std::pair<size_t, bool>> keys;
            for (const auto& o : q.orderBy) {
                bool found = false;
                for (size_t i = 0; i < rs.columns.size(); ++i) {
                    if (ciEquals(rs.columns[i], o.col.display()) ||
                        ciEquals(rs.columns[i], o.col.column)) {
                        keys.push_back({i, o.desc});
                        found = true;
                        break;
                    }
                }
                if (!found)
                    throw QueryError("ORDER BY column '" + o.col.display() +
                                     "' must appear in the select list when aggregating");
            }
            std::stable_sort(rs.rows.begin(), rs.rows.end(),
                             [&](const std::vector<Value>& a, const std::vector<Value>& b) {
                                 for (auto [i, desc] : keys) {
                                     int cmp = orderCompare(a[i], b[i]);
                                     if (cmp != 0) return desc ? cmp > 0 : cmp < 0;
                                 }
                                 return false;
                             });
        }

        if (q.limit >= 0 && rs.rows.size() > static_cast<size_t>(q.limit))
            rs.rows.resize(static_cast<size_t>(q.limit));

        rs.notes.push_back("aggregated into " + std::to_string(groups.size()) + " group(s)");
    }
};

}  // namespace minidb
