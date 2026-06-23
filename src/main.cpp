//
// main.cpp -- the minidb command line interface.
//
// Usage:
//   minidb [csv files...]                 load CSVs and start the REPL
//   minidb data.csv -q "SELECT ..."       run one or more queries and exit
//   minidb data.csv -f script.sql         run a script of ';'-separated queries
//   minidb --bench [N]                    index vs full-scan benchmark (default N=1000000)
//
// Each loaded CSV becomes a table named after the file (e.g. data/Iris.csv
// -> table "Iris"). Use name=path to choose the table name explicitly.

#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "executor.hpp"

using namespace minidb;

namespace {

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
        .count();
}

std::string tableNameFromPath(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    std::string stem = (slash == std::string::npos) ? path : path.substr(slash + 1);
    size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos && dot > 0) stem = stem.substr(0, dot);
    for (auto& c : stem)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';
    if (stem.empty() || std::isdigit(static_cast<unsigned char>(stem[0]))) stem = "t" + stem;
    return stem;
}

void printResult(const ResultSet& rs, std::ostream& os) {
    std::vector<size_t> widths(rs.columns.size());
    std::vector<std::vector<std::string>> cells;
    for (size_t i = 0; i < rs.columns.size(); ++i) widths[i] = rs.columns[i].size();
    cells.reserve(rs.rows.size());
    for (const auto& row : rs.rows) {
        std::vector<std::string> line;
        line.reserve(row.size());
        for (size_t i = 0; i < row.size(); ++i) {
            std::string s = row[i].isNull() ? "NULL" : row[i].toString();
            widths[i] = std::max(widths[i], s.size());
            line.push_back(std::move(s));
        }
        cells.push_back(std::move(line));
    }
    auto printRow = [&](const std::vector<std::string>& vals) {
        for (size_t i = 0; i < vals.size(); ++i) {
            os << vals[i];
            if (i + 1 < vals.size()) os << std::string(widths[i] - vals[i].size() + 2, ' ');
        }
        os << "\n";
    };
    printRow(rs.columns);
    std::vector<std::string> sep;
    for (size_t w : widths) sep.push_back(std::string(w, '-'));
    printRow(sep);
    for (const auto& line : cells) printRow(line);
    os << "(" << rs.rows.size() << (rs.rows.size() == 1 ? " row)" : " rows)") << "\n";
}

struct Options {
    bool timer = false;
};

void runStatement(Database& db, const std::string& sql, Options& opt) {
    auto t0 = std::chrono::steady_clock::now();
    try {
        ResultSet rs = db.execute(sql);
        double ms = msSince(t0);
        printResult(rs, std::cout);
        if (opt.timer) {
            for (const auto& n : rs.notes) std::cout << "-- plan: " << n << "\n";
            std::cout << "-- time: " << ms << " ms\n";
        }
    } catch (const std::exception& e) {
        std::cout << "error: " << e.what() << "\n";
    }
    std::cout << "\n";
}

// Split a script into statements on ';' (ignoring ';' inside string
// literals and "--" comments).
std::vector<std::string> splitStatements(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    bool inStr = false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (!inStr && c == '-' && i + 1 < text.size() && text[i + 1] == '-') {
            while (i < text.size() && text[i] != '\n') ++i;
            cur.push_back('\n');
            continue;
        }
        if (c == '\'') inStr = !inStr;
        if (c == ';' && !inStr) {
            out.push_back(cur);
            cur.clear();
            continue;
        }
        cur.push_back(c);
    }
    out.push_back(cur);
    // drop whitespace-only fragments
    std::vector<std::string> stmts;
    for (auto& s : out)
        if (s.find_first_not_of(" \t\r\n") != std::string::npos) stmts.push_back(s);
    return stmts;
}

void printHelp() {
    std::cout <<
        "Queries end with ';'. Meta commands:\n"
        "  .help                 show this help\n"
        "  .tables               list loaded tables\n"
        "  .schema <table>       show columns, types, row count, indexes\n"
        "  .index <table> <col>  build a hash index on a column\n"
        "  .load <path> [name]   load a CSV file as a table\n"
        "  .timer on|off         print execution time and query plan\n"
        "  .quit                 exit\n"
        "Example:\n"
        "  SELECT Species, COUNT(*), AVG(PetalLengthCm) FROM Iris GROUP BY Species;\n";
}

void metaCommand(Database& db, const std::string& line, Options& opt, bool& quit) {
    std::istringstream ss(line);
    std::string cmd, a1, a2;
    ss >> cmd >> a1 >> a2;
    try {
        if (cmd == ".quit" || cmd == ".exit") {
            quit = true;
        } else if (cmd == ".help") {
            printHelp();
        } else if (cmd == ".tables") {
            for (const auto& n : db.tableNames()) std::cout << "  " << n << "\n";
        } else if (cmd == ".schema") {
            Table* t = db.findTable(a1);
            if (!t) { std::cout << "error: unknown table '" << a1 << "'\n"; return; }
            std::cout << "table " << t->name << " (" << t->rows.size() << " rows)\n";
            for (const auto& c : t->columns)
                std::cout << "  " << c.name << "  " << typeName(c.type) << "\n";
            for (const auto& ic : t->indexedColumns())
                std::cout << "  index on " << ic << "\n";
        } else if (cmd == ".index") {
            Table* t = db.findTable(a1);
            if (!t) { std::cout << "error: unknown table '" << a1 << "'\n"; return; }
            int c = t->findColumn(a2);
            if (c < 0) { std::cout << "error: unknown column '" << a2 << "'\n"; return; }
            auto t0 = std::chrono::steady_clock::now();
            t->buildIndex(c);
            std::cout << "built hash index on " << t->name << "." << t->columns[c].name
                      << " in " << msSince(t0) << " ms\n";
        } else if (cmd == ".load") {
            if (a1.empty()) { std::cout << "usage: .load <path> [name]\n"; return; }
            std::string name = a2.empty() ? tableNameFromPath(a1) : a2;
            Table& t = db.loadCsvFile(a1, name);
            std::cout << "loaded " << t.rows.size() << " rows into table " << t.name << "\n";
        } else if (cmd == ".timer") {
            opt.timer = (a1 == "on");
            std::cout << "timer " << (opt.timer ? "on" : "off") << "\n";
        } else {
            std::cout << "unknown command '" << cmd << "' (try .help)\n";
        }
    } catch (const std::exception& e) {
        std::cout << "error: " << e.what() << "\n";
    }
}

void repl(Database& db) {
    Options opt;
    std::cout << "minidb -- type .help for help, queries end with ';'\n";
    std::string buffer, line;
    bool quit = false;
    while (!quit) {
        std::cout << (buffer.empty() ? "minidb> " : "   ...> ") << std::flush;
        if (!std::getline(std::cin, line)) break;
        if (buffer.empty() && !line.empty() && line[0] == '.') {
            metaCommand(db, line, opt, quit);
            continue;
        }
        buffer += line;
        buffer += "\n";
        // execute complete statements, keep the rest buffered
        if (line.find(';') != std::string::npos) {
            for (const auto& stmt : splitStatements(buffer)) runStatement(db, stmt, opt);
            buffer.clear();
        }
    }
}

// ---------------------------------------------------------------- bench ----
//
// Demonstrates the core indexing trade-off on a synthetic table:
//   * a high-selectivity equality (unique id): index turns O(n) into ~O(1)
//   * a low-selectivity equality (3 species): index helps far less, because
//     most of the time goes into producing the large result anyway
//   * the index itself costs one O(n) build pass + memory, so it only pays
//     off after enough queries.
void runBench(size_t n) {
    Database db;
    Table t;
    t.name = "bench";
    t.columns = {{"id", Type::Int}, {"val", Type::Float}, {"species", Type::String}};
    t.rows.reserve(n);
    const char* species[] = {"Iris-setosa", "Iris-versicolor", "Iris-virginica"};
    for (size_t i = 0; i < n; ++i) {
        std::vector<Value> row;
        row.push_back(Value::ofInt(static_cast<long long>(i)));
        row.push_back(Value::ofFloat(static_cast<double>(i % 1000) / 10.0));
        row.push_back(Value::ofString(species[i % 3]));
        t.rows.push_back(std::move(row));
    }
    Table& bt = db.addTable(std::move(t));
    std::cout << "benchmark table: " << n << " rows\n\n";

    auto timeQuery = [&](const std::string& sql, int reps) {
        double best = 1e18;
        size_t rows = 0;
        for (int i = 0; i < reps; ++i) {
            auto t0 = std::chrono::steady_clock::now();
            ResultSet rs = db.execute(sql);
            best = std::min(best, msSince(t0));
            rows = rs.rows.size();
        }
        return std::pair<double, size_t>(best, rows);
    };

    std::string q1 = "SELECT id, val FROM bench WHERE id = " + std::to_string(n / 2);
    std::string q2 = "SELECT COUNT(*) FROM bench WHERE species = 'Iris-virginica'";

    auto [scan1, rows1] = timeQuery(q1, 3);
    std::cout << "point query (id = " << n / 2 << ", " << rows1 << " match)\n";
    std::cout << "  full scan:        " << scan1 << " ms\n";

    auto t0 = std::chrono::steady_clock::now();
    bt.buildIndex(bt.findColumn("id"));
    double buildId = msSince(t0);
    auto [idx1, _r1] = timeQuery(q1, 3);
    std::cout << "  index build:      " << buildId << " ms (one-time)\n";
    std::cout << "  indexed lookup:   " << idx1 << " ms  (" << scan1 / std::max(idx1, 1e-6)
              << "x faster)\n";
    std::cout << "  break-even after ~" << static_cast<long long>(buildId / std::max(scan1 - idx1, 1e-9)) + 1
              << " queries\n\n";

    auto [scan2, _r2] = timeQuery(q2, 3);
    std::cout << "low-selectivity query (species, 1/3 of all rows match)\n";
    std::cout << "  full scan:        " << scan2 << " ms\n";
    t0 = std::chrono::steady_clock::now();
    bt.buildIndex(bt.findColumn("species"));
    double buildSp = msSince(t0);
    auto [idx2, _r3] = timeQuery(q2, 3);
    std::cout << "  index build:      " << buildSp << " ms (one-time)\n";
    std::cout << "  indexed lookup:   " << idx2 << " ms  (" << scan2 / std::max(idx2, 1e-6)
              << "x faster)\n\n";

    std::cout << "take-away: a hash index turns a selective equality predicate into a\n"
                 "near O(1) lookup, but costs an O(n) build pass and extra memory; for\n"
                 "low-selectivity predicates most time goes into handling the many\n"
                 "matching rows, so the index helps much less.\n";
}

// ---------------------------------------------------------------- demo ----
//
// Self-contained walkthrough of all four pipeline stages using the Iris
// dataset.  Run with:  ./minidb data/Iris.csv data/SpeciesInfo.csv --demo
void runDemo(Database& db) {
    auto step = [](int n, const char* title) {
        std::cout << "\n[" << n << "/5] " << title << "\n"
                  << std::string(60, '-') << "\n";
    };
    auto run = [&](const std::string& sql) {
        std::cout << "SQL: " << sql << "\n";
        Options opt; opt.timer = true;
        runStatement(db, sql, opt);
    };

    std::cout << "=== minidb: CSV Mini Database & Query Engine ===\n";

    // 1. LOAD
    step(1, "LOAD — CSV files parsed into typed in-memory tables");
    for (const auto& name : db.tableNames()) {
        Table* t = db.findTable(name);
        std::cout << "  table \"" << name << "\"  (" << t->rows.size() << " rows, "
                  << t->columns.size() << " columns)\n";
        for (const auto& c : t->columns)
            std::cout << "    " << c.name << "  " << typeName(c.type) << "\n";
    }

    // 2. PARSE + FILTER + SORT
    step(2, "PARSE + EXECUTE — filter and sort");
    run("SELECT Id, SepalLengthCm, Species FROM Iris "
        "WHERE SepalLengthCm > 7.0 ORDER BY SepalLengthCm DESC;");

    // 3. AGGREGATE
    step(3, "AGGREGATE — GROUP BY with COUNT and AVG");
    run("SELECT Species, COUNT(*) AS n, AVG(PetalLengthCm) AS avg_petal, "
        "MAX(PetalWidthCm) AS max_width FROM Iris GROUP BY Species ORDER BY avg_petal DESC;");

    // 4. JOIN
    step(4, "JOIN — two CSV tables joined on a shared key");
    run("SELECT i.Id, s.CommonName, i.PetalLengthCm FROM Iris i "
        "JOIN SpeciesInfo s ON i.Species = s.Species "
        "WHERE i.PetalLengthCm > 6.3 ORDER BY i.PetalLengthCm DESC;");

    // 5. INDEX
    step(5, "INDEX — hash index vs. full scan on the same query");
    std::string q = "SELECT Id, Species FROM Iris WHERE Species = 'Iris-setosa' LIMIT 5;";
    std::cout << "SQL: " << q << "\n";
    Options opt; opt.timer = true;
    runStatement(db, q, opt);

    Table* iris = db.findTable("Iris");
    if (iris) {
        auto t0 = std::chrono::steady_clock::now();
        iris->buildIndex(iris->findColumn("Species"));
        std::cout << "  (hash index built on Iris.Species in " << msSince(t0) << " ms)\n\n";
    }
    std::cout << "same query, now with index:\n";
    runStatement(db, q, opt);

    std::cout << std::string(60, '-') << "\n"
              << "Done. Run './minidb data/Iris.csv data/SpeciesInfo.csv' for the REPL.\n";
}

}  // namespace

int main(int argc, char** argv) {
    Database db;
    std::vector<std::string> queries;
    std::string scriptPath;
    bool wantBench = false;
    bool wantDemo = false;
    size_t benchN = 1000000;

    std::vector<std::string> args(argv + 1, argv + argc);
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        try {
            if (a == "-q" && i + 1 < args.size()) {
                queries.push_back(args[++i]);
            } else if (a == "-f" && i + 1 < args.size()) {
                scriptPath = args[++i];
            } else if (a == "--demo") {
                wantDemo = true;
            } else if (a == "--bench") {
                wantBench = true;
                if (i + 1 < args.size() && args[i + 1].find_first_not_of("0123456789") ==
                                               std::string::npos && !args[i + 1].empty())
                    benchN = std::stoull(args[++i]);
            } else if (a == "-h" || a == "--help") {
                std::cout << "usage: minidb [name=]file.csv ... [-q SQL] [-f script.sql] [--bench [N]]\n";
                return 0;
            } else {
                // CSV to load; "name=path" picks the table name
                size_t eq = a.find('=');
                std::string name, path;
                if (eq != std::string::npos && eq > 0) {
                    name = a.substr(0, eq);
                    path = a.substr(eq + 1);
                } else {
                    path = a;
                    name = tableNameFromPath(path);
                }
                Table& t = db.loadCsvFile(path, name);
                std::cerr << "loaded " << t.rows.size() << " rows into table " << t.name
                          << "\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << "\n";
            return 1;
        }
    }

    if (wantDemo) {
        runDemo(db);
        return 0;
    }
    if (wantBench) {
        runBench(benchN);
        return 0;
    }

    Options opt;
    if (!scriptPath.empty()) {
        std::ifstream in(scriptPath);
        if (!in) {
            std::cerr << "error: cannot open script " << scriptPath << "\n";
            return 1;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        opt.timer = true;
        for (const auto& stmt : splitStatements(ss.str())) {
            std::cout << ">" << stmt << "\n";
            runStatement(db, stmt, opt);
        }
        return 0;
    }
    if (!queries.empty()) {
        for (const auto& sql : queries)
            for (const auto& stmt : splitStatements(sql)) runStatement(db, stmt, opt);
        return 0;
    }

    repl(db);
    return 0;
}
