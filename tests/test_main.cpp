//
// test_main.cpp -- unit tests for the CSV parser, type inference and the
// query engine. Run from the repository root (it loads data/Iris.csv), or
// pass the data directory as argv[1].
//
//   g++ -std=c++17 -O2 -o run_tests tests/test_main.cpp && ./run_tests

#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>

#include "../src/executor.hpp"

using namespace minidb;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failures;                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                 \
    } while (0)

#define CHECK_THROWS(expr)                                                 \
    do {                                                                   \
        ++g_checks;                                                        \
        bool threw = false;                                                \
        try {                                                              \
            (void)(expr);                                                  \
        } catch (const std::exception&) {                                  \
            threw = true;                                                  \
        }                                                                  \
        if (!threw) {                                                      \
            ++g_failures;                                                  \
            std::printf("FAIL %s:%d: expected %s to throw\n", __FILE__,    \
                        __LINE__, #expr);                                  \
        }                                                                  \
    } while (0)

static std::string g_dataDir = "data";

// ----------------------------------------------------------- CSV parser ----

static void testCsvParser() {
    {
        std::istringstream in("a,b,c\n1,2,3\n");
        auto r = parseCsv(in);
        CHECK(r.size() == 2);
        CHECK(r[0].size() == 3);
        CHECK(r[1][2] == "3");
    }
    {
        // quoted fields: embedded delimiter, "" escape, embedded newline
        std::istringstream in("name,quote\n\"Doe, John\",\"he said \"\"hi\"\"\"\n\"two\nlines\",x\n");
        auto r = parseCsv(in);
        CHECK(r.size() == 3);
        CHECK(r[1][0] == "Doe, John");
        CHECK(r[1][1] == "he said \"hi\"");
        CHECK(r[2][0] == "two\nlines");
    }
    {
        // CRLF line endings + no trailing newline + blank lines skipped
        std::istringstream in("a,b\r\n1,2\r\n\r\n3,4");
        auto r = parseCsv(in);
        CHECK(r.size() == 3);
        CHECK(r[1][1] == "2");
        CHECK(r[2][0] == "3");
    }
    {
        // empty fields
        std::istringstream in("a,b,c\n,,\nx,,z\n");
        auto r = parseCsv(in);
        CHECK(r.size() == 3);
        CHECK(r[1][0] == "");
        CHECK(r[2][1] == "");
    }
    {
        std::istringstream in("a\n\"unterminated\n");
        CHECK_THROWS(parseCsv(in));
    }
    {
        std::istringstream in("a\n\"x\"y\n");  // junk after closing quote
        CHECK_THROWS(parseCsv(in));
    }
}

// ------------------------------------------------- table / type inference ----

static void testTypeInference() {
    std::istringstream in(
        "id,score,flag,label,missing\n"
        "1,1.5,true,abc,\n"
        "2,2,false,5x,\n"
        "3,.5,TRUE,hello,\n");
    Table t = Table::fromCsv(in, "T");
    CHECK(t.columns[0].type == Type::Int);
    CHECK(t.columns[1].type == Type::Float);
    CHECK(t.columns[2].type == Type::Bool);
    CHECK(t.columns[3].type == Type::String);
    CHECK(t.columns[4].type == Type::String);  // all NULL -> default string
    CHECK(t.rows.size() == 3);
    CHECK(t.rows[0][0].getInt() == 1);
    CHECK(t.rows[1][1].getFloat() == 2.0);
    CHECK(t.rows[2][2].getBool() == true);
    CHECK(t.rows[0][4].isNull());
}

static void testMalformed() {
    std::istringstream in("a,b\n1,2,3\n");  // too many fields
    CHECK_THROWS(Table::fromCsv(in, "T"));
    std::istringstream in2("");
    CHECK_THROWS(Table::fromCsv(in2, "T"));
}

// ---------------------------------------------------------- query engine ----

static Database makeIrisDb() {
    Database db;
    db.loadCsvFile(g_dataDir + "/Iris.csv", "Iris");
    db.loadCsvFile(g_dataDir + "/SpeciesInfo.csv", "SpeciesInfo");
    return db;
}

static Value scalar(Database& db, const std::string& sql) {
    ResultSet rs = db.execute(sql);
    CHECK(rs.rows.size() == 1);
    CHECK(!rs.rows.empty() && !rs.rows[0].empty());
    return rs.rows.empty() || rs.rows[0].empty() ? Value() : rs.rows[0][0];
}

static void testIrisSchema() {
    Database db = makeIrisDb();
    Table* t = db.findTable("Iris");
    CHECK(t != nullptr);
    CHECK(t->rows.size() == 150);
    CHECK(t->columns.size() == 6);
    CHECK(t->columns[0].type == Type::Int);     // Id
    CHECK(t->columns[1].type == Type::Float);   // SepalLengthCm
    CHECK(t->columns[5].type == Type::String);  // Species
}

static void testBasicQueries() {
    Database db = makeIrisDb();

    CHECK(scalar(db, "SELECT COUNT(*) FROM Iris").getInt() == 150);

    double avg = scalar(db, "SELECT AVG(SepalLengthCm) FROM Iris").getFloat();
    CHECK(std::fabs(avg - 5.8433) < 1e-3);

    ResultSet rs = db.execute("SELECT * FROM Iris WHERE Species = 'Iris-setosa'");
    CHECK(rs.rows.size() == 50);
    CHECK(rs.columns.size() == 6);

    // complementary predicates partition the table (no NULLs in iris)
    long long gt = scalar(db, "SELECT COUNT(*) FROM Iris WHERE PetalLengthCm > 4.0").getInt();
    long long le = scalar(db, "SELECT COUNT(*) FROM Iris WHERE PetalLengthCm <= 4.0").getInt();
    CHECK(gt + le == 150);

    // AND / OR / NOT consistency: |A| = |A AND B| + |A AND NOT B|
    long long a = scalar(db,
        "SELECT COUNT(*) FROM Iris WHERE Species = 'Iris-setosa'").getInt();
    long long ab = scalar(db,
        "SELECT COUNT(*) FROM Iris WHERE Species = 'Iris-setosa' AND SepalWidthCm > 3.5").getInt();
    long long anb = scalar(db,
        "SELECT COUNT(*) FROM Iris WHERE Species = 'Iris-setosa' AND NOT SepalWidthCm > 3.5").getInt();
    CHECK(a == ab + anb);
    long long orCount = scalar(db,
        "SELECT COUNT(*) FROM Iris WHERE Species = 'Iris-setosa' OR Species = 'Iris-virginica'").getInt();
    CHECK(orCount == 100);

    // != on the whole table
    long long ne = scalar(db, "SELECT COUNT(*) FROM Iris WHERE Species != 'Iris-setosa'").getInt();
    CHECK(ne == 100);
}

static void testProjectionOrderLimit() {
    Database db = makeIrisDb();

    ResultSet rs = db.execute(
        "SELECT Id, SepalLengthCm AS sl FROM Iris ORDER BY sl DESC, Id ASC LIMIT 3");
    CHECK(rs.columns.size() == 2);
    CHECK(rs.columns[1] == "sl");
    CHECK(rs.rows.size() == 3);
    CHECK(std::fabs(rs.rows[0][1].getFloat() - 7.9) < 1e-9);  // max sepal length in iris
    CHECK(rs.rows[0][1].getFloat() >= rs.rows[1][1].getFloat());
    CHECK(rs.rows[1][1].getFloat() >= rs.rows[2][1].getFloat());

    // ORDER BY a column that is not selected
    ResultSet rs2 = db.execute("SELECT Species FROM Iris ORDER BY PetalWidthCm DESC LIMIT 1");
    CHECK(rs2.rows[0][0].getString() == "Iris-virginica");  // widest petals

    // min / max
    CHECK(std::fabs(scalar(db, "SELECT MIN(SepalWidthCm) FROM Iris").getFloat() - 2.0) < 1e-9);
    CHECK(std::fabs(scalar(db, "SELECT MAX(SepalWidthCm) FROM Iris").getFloat() - 4.4) < 1e-9);

    // SUM of an int column stays integral
    Value idSum = scalar(db, "SELECT SUM(Id) FROM Iris");
    CHECK(idSum.type() == Type::Int);
    CHECK(idSum.getInt() == 150 * 151 / 2);
}

static void testGroupBy() {
    Database db = makeIrisDb();
    ResultSet rs = db.execute(
        "SELECT Species, COUNT(*) AS n, AVG(PetalLengthCm) AS apl "
        "FROM Iris GROUP BY Species ORDER BY apl");
    CHECK(rs.rows.size() == 3);
    for (const auto& row : rs.rows) CHECK(row[1].getInt() == 50);
    // setosa has by far the shortest petals -> first after ORDER BY apl
    CHECK(rs.rows[0][0].getString() == "Iris-setosa");
    CHECK(std::fabs(rs.rows[0][2].getFloat() - 1.464) < 1e-3);
    CHECK(rs.rows[2][0].getString() == "Iris-virginica");

    // grouping must reject non-grouped bare columns
    CHECK_THROWS(db.execute("SELECT Id, COUNT(*) FROM Iris GROUP BY Species"));
    // SELECT * with GROUP BY is rejected
    CHECK_THROWS(db.execute("SELECT * FROM Iris GROUP BY Species"));
}

static void testJoin() {
    Database db = makeIrisDb();
    ResultSet rs = db.execute(
        "SELECT i.Id, i.Species, s.CommonName FROM Iris i "
        "JOIN SpeciesInfo s ON i.Species = s.Species");
    CHECK(rs.rows.size() == 150);  // every iris row matches exactly one info row
    CHECK(rs.columns.size() == 3);

    ResultSet one = db.execute(
        "SELECT s.CommonName FROM Iris i JOIN SpeciesInfo s ON i.Species = s.Species "
        "WHERE i.Id = 1");
    CHECK(one.rows.size() == 1);
    CHECK(one.rows[0][0].getString() == "Bristle-pointed iris");

    // aggregate over a join
    ResultSet g = db.execute(
        "SELECT s.CommonName, COUNT(*) AS n FROM Iris i "
        "JOIN SpeciesInfo s ON i.Species = s.Species GROUP BY s.CommonName");
    CHECK(g.rows.size() == 3);
    for (const auto& row : g.rows) CHECK(row[1].getInt() == 50);

    // SELECT * over a join qualifies the duplicated join column
    ResultSet star = db.execute(
        "SELECT * FROM Iris i JOIN SpeciesInfo s ON i.Species = s.Species LIMIT 1");
    CHECK(star.columns.size() == 9);
    bool sawQualified = false;
    for (const auto& c : star.columns)
        if (c == "i.Species" || c == "s.Species") sawQualified = true;
    CHECK(sawQualified);
}

static void testIndex() {
    Database db = makeIrisDb();
    const std::string q = "SELECT Id FROM Iris WHERE Species = 'Iris-versicolor' ORDER BY Id";

    ResultSet noIdx = db.execute(q);
    Table* t = db.findTable("Iris");
    t->buildIndex(t->findColumn("Species"));
    ResultSet withIdx = db.execute(q);

    // identical results, different plan
    CHECK(noIdx.rows.size() == 50);
    CHECK(withIdx.rows.size() == noIdx.rows.size());
    for (size_t i = 0; i < noIdx.rows.size(); ++i)
        CHECK(withIdx.rows[i][0].getInt() == noIdx.rows[i][0].getInt());
    CHECK(!noIdx.notes.empty() && noIdx.notes[0].find("full scan") != std::string::npos);
    CHECK(!withIdx.notes.empty() && withIdx.notes[0].find("index lookup") != std::string::npos);

    // the index only pre-filters; the rest of the predicate still applies
    ResultSet rs = db.execute(
        "SELECT COUNT(*) FROM Iris WHERE Species = 'Iris-versicolor' AND SepalLengthCm > 6.0");
    CHECK(!rs.notes.empty() && rs.notes[0].find("index lookup") != std::string::npos);
    long long viaIndex = rs.rows[0][0].getInt();
    Database db2 = makeIrisDb();  // fresh db without the index
    long long viaScan = scalar(db2,
        "SELECT COUNT(*) FROM Iris WHERE Species = 'Iris-versicolor' AND SepalLengthCm > 6.0").getInt();
    CHECK(viaIndex == viaScan);

    // index on an int column
    t->buildIndex(t->findColumn("Id"));
    ResultSet byId = db.execute("SELECT Species FROM Iris WHERE Id = 77");
    CHECK(byId.rows.size() == 1);
    CHECK(!byId.notes.empty() && byId.notes[0].find("index lookup") != std::string::npos);
}

static void testErrors() {
    Database db = makeIrisDb();
    CHECK_THROWS(db.execute("SELECT nope FROM Iris"));
    CHECK_THROWS(db.execute("SELECT * FROM NoSuchTable"));
    CHECK_THROWS(db.execute("SELEC * FROM Iris"));
    CHECK_THROWS(db.execute("SELECT * FROM Iris WHERE"));
    CHECK_THROWS(db.execute("SELECT SUM(Species) FROM Iris"));          // non-numeric SUM
    CHECK_THROWS(db.execute("SELECT * FROM Iris WHERE Species > 5"));   // string vs number
    CHECK_THROWS(db.execute("SELECT Species FROM Iris JOIN SpeciesInfo s ON Species = s.Species"));  // ambiguous
    CHECK_THROWS(db.execute("SELECT * FROM Iris LIMIT -1"));
    CHECK_THROWS(db.execute("SELECT * FROM Iris WHERE Species = 'unterminated"));
}

int main(int argc, char** argv) {
    if (argc > 1) g_dataDir = argv[1];

    testCsvParser();
    testTypeInference();
    testMalformed();
    testIrisSchema();
    testBasicQueries();
    testProjectionOrderLimit();
    testGroupBy();
    testJoin();
    testIndex();
    testErrors();

    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
