CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
# On Windows/MinGW, build with `make CXXFLAGS="-std=c++17 -O2 -Wall -Wextra -static"`
# so the executables do not depend on matching runtime DLLs being in PATH.
HDRS      = src/csv.hpp src/value.hpp src/table.hpp src/query.hpp src/executor.hpp

all: minidb

minidb: src/main.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ src/main.cpp

run_tests: tests/test_main.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ tests/test_main.cpp

test: run_tests
	./run_tests

bench: minidb
	./minidb --bench

clean:
	rm -f minidb minidb.exe run_tests run_tests.exe

.PHONY: all test bench clean
