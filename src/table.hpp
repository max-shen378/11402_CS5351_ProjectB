#pragma once
//
// table.hpp -- typed in-memory tables with schema inference and hash indexes.
//
// A Table is loaded from CSV text: the first record is the header, every
// other record becomes a row. Column types are inferred by scanning all
// values (Int > Float > Bool > String precedence; empty fields are NULL and
// do not influence the inferred type).
//
// Indexing: a hash index maps a column value (canonical string form) to the
// list of row ids holding that value. It accelerates equality lookups and
// joins from O(rows) to ~O(1) per probe, at the cost of one O(rows) build
// pass and extra memory. Hash indexes cannot help range predicates (<, >) --
// that would require a sorted/tree index; see README for the trade-offs.

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "csv.hpp"
#include "value.hpp"

namespace minidb {

inline std::string toLower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline bool ciEquals(const std::string& a, const std::string& b) {
    return toLower(a) == toLower(b);
}

inline std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Strict full-string numeric parsers (no leading/trailing junk).
inline bool tryParseInt(const std::string& s, long long& out) {
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    long long v = std::strtoll(s.c_str(), &end, 10);
    if (errno != 0 || end != s.c_str() + s.size()) return false;
    out = v;
    return true;
}

inline bool tryParseFloat(const std::string& s, double& out) {
    if (s.empty()) return false;
    if (std::isspace(static_cast<unsigned char>(s.front()))) return false;
    errno = 0;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (errno != 0 || end != s.c_str() + s.size()) return false;
    out = v;
    return true;
}

struct Column {
    std::string name;
    Type type = Type::String;
};

// value (canonical string) -> ids of rows containing it
using HashIndex = std::unordered_map<std::string, std::vector<size_t>>;

class Table {
public:
    std::string name;
    std::vector<Column> columns;
    std::vector<std::vector<Value>> rows;

    // Case-insensitive column lookup; returns -1 if not found.
    int findColumn(const std::string& colName) const {
        for (size_t i = 0; i < columns.size(); ++i)
            if (ciEquals(columns[i].name, colName)) return static_cast<int>(i);
        return -1;
    }

    void buildIndex(int col) {
        HashIndex idx;
        idx.reserve(rows.size());
        for (size_t i = 0; i < rows.size(); ++i) {
            const Value& v = rows[i][col];
            if (v.isNull()) continue;  // NULL never matches an equality predicate
            idx[v.toString()].push_back(i);
        }
        indexes_[col] = std::move(idx);
    }

    const HashIndex* getIndex(int col) const {
        auto it = indexes_.find(col);
        return it == indexes_.end() ? nullptr : &it->second;
    }

    std::vector<std::string> indexedColumns() const {
        std::vector<std::string> out;
        for (const auto& [col, idx] : indexes_) out.push_back(columns[col].name);
        return out;
    }

    static Table fromCsv(std::istream& in, std::string tableName) {
        auto recs = parseCsv(in);
        if (recs.empty()) throw CsvError("file is empty (expected a header row)");

        Table t;
        t.name = std::move(tableName);

        // Header row -> column names. Strip a UTF-8 BOM from the first cell.
        std::vector<std::string>& header = recs[0];
        if (!header.empty() && header[0].rfind("\xEF\xBB\xBF", 0) == 0)
            header[0] = header[0].substr(3);
        for (size_t i = 0; i < header.size(); ++i) {
            std::string h = trim(header[i]);
            if (h.empty()) h = "col" + std::to_string(i + 1);
            t.columns.push_back({h, Type::String});
        }
        const size_t ncols = t.columns.size();

        for (size_t r = 1; r < recs.size(); ++r) {
            if (recs[r].size() != ncols)
                throw CsvError("record " + std::to_string(r + 1) + " has " +
                               std::to_string(recs[r].size()) + " fields, expected " +
                               std::to_string(ncols));
        }

        // Infer each column's type: Int beats Float beats Bool beats String.
        for (size_t c = 0; c < ncols; ++c) {
            bool allInt = true, allFloat = true, allBool = true, any = false;
            for (size_t r = 1; r < recs.size(); ++r) {
                const std::string& s = recs[r][c];
                if (s.empty()) continue;  // NULL: does not constrain the type
                any = true;
                long long iv;
                double dv;
                if (allInt && !tryParseInt(s, iv)) allInt = false;
                if (allFloat && !tryParseFloat(s, dv)) allFloat = false;
                if (allBool) {
                    std::string l = toLower(s);
                    if (l != "true" && l != "false") allBool = false;
                }
                if (!allInt && !allFloat && !allBool) break;
            }
            if (!any) t.columns[c].type = Type::String;
            else if (allInt) t.columns[c].type = Type::Int;
            else if (allFloat) t.columns[c].type = Type::Float;
            else if (allBool) t.columns[c].type = Type::Bool;
            else t.columns[c].type = Type::String;
        }

        // Convert text records into typed rows.
        t.rows.reserve(recs.size() - 1);
        for (size_t r = 1; r < recs.size(); ++r) {
            std::vector<Value> row;
            row.reserve(ncols);
            for (size_t c = 0; c < ncols; ++c) {
                const std::string& s = recs[r][c];
                if (s.empty()) {
                    row.emplace_back();  // NULL
                    continue;
                }
                switch (t.columns[c].type) {
                case Type::Int: {
                    long long iv = 0;
                    tryParseInt(s, iv);
                    row.push_back(Value::ofInt(iv));
                    break;
                }
                case Type::Float: {
                    double dv = 0;
                    tryParseFloat(s, dv);
                    row.push_back(Value::ofFloat(dv));
                    break;
                }
                case Type::Bool:
                    row.push_back(Value::ofBool(toLower(s) == "true"));
                    break;
                default:
                    row.push_back(Value::ofString(s));
                }
            }
            t.rows.push_back(std::move(row));
        }
        return t;
    }

    static Table fromCsvFile(const std::string& path, std::string tableName) {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw CsvError("cannot open file: " + path);
        return fromCsv(in, std::move(tableName));
    }

private:
    std::unordered_map<int, HashIndex> indexes_;  // keyed by column id
};

}  // namespace minidb
