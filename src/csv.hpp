#pragma once
//
// csv.hpp -- a small, robust RFC 4180-style CSV parser.
//
// Handles:
//   * fields separated by a configurable delimiter (default ',')
//   * records terminated by LF or CRLF (lone '\r' inside a field is kept)
//   * quoted fields that may contain delimiters, quotes ("" escape) and
//     embedded newlines
//   * blank lines (skipped) and a missing trailing newline
//
// The parser is a single-pass character state machine, so it never needs to
// buffer whole lines and works for fields that span multiple lines.

#include <istream>
#include <stdexcept>
#include <string>
#include <vector>

namespace minidb {

class CsvError : public std::runtime_error {
public:
    explicit CsvError(const std::string& msg) : std::runtime_error("CSV error: " + msg) {}
};

inline std::vector<std::vector<std::string>> parseCsv(std::istream& in, char delim = ',') {
    std::vector<std::vector<std::string>> records;
    std::vector<std::string> record;
    std::string field;

    enum class State {
        RecordStart,  // nothing consumed for the current record yet
        FieldStart,   // just after a delimiter
        InField,      // inside an unquoted field
        InQuoted,     // inside a quoted field
        AfterQuoted,  // right after the closing quote of a quoted field
    };
    State st = State::RecordStart;
    size_t line = 1;

    auto endField = [&]() {
        record.push_back(field);
        field.clear();
    };
    auto endRecord = [&]() {
        endField();
        records.push_back(std::move(record));
        record.clear();
    };

    int ci;
    while ((ci = in.get()) != EOF) {
        char c = static_cast<char>(ci);
        if (c == '\n') ++line;

        // Normalize CRLF: a '\r' immediately followed by '\n' is dropped and
        // the '\n' is handled as the newline. A lone '\r' is treated as data.
        if (c == '\r' && in.peek() == '\n' && st != State::InQuoted) continue;

        switch (st) {
        case State::RecordStart:
        case State::FieldStart:
            if (c == '"') {
                st = State::InQuoted;
            } else if (c == delim) {
                endField();
                st = State::FieldStart;
            } else if (c == '\n') {
                if (st == State::RecordStart) {
                    // blank line -> skip
                } else {
                    endRecord();  // record ending in an empty field
                }
                st = State::RecordStart;
            } else {
                field.push_back(c);
                st = State::InField;
            }
            break;

        case State::InField:
            if (c == delim) {
                endField();
                st = State::FieldStart;
            } else if (c == '\n') {
                endRecord();
                st = State::RecordStart;
            } else {
                field.push_back(c);
            }
            break;

        case State::InQuoted:
            if (c == '"') {
                if (in.peek() == '"') {
                    in.get();           // "" -> literal quote
                    field.push_back('"');
                } else {
                    st = State::AfterQuoted;
                }
            } else {
                field.push_back(c);     // delimiters and newlines are data here
            }
            break;

        case State::AfterQuoted:
            if (c == delim) {
                endField();
                st = State::FieldStart;
            } else if (c == '\n') {
                endRecord();
                st = State::RecordStart;
            } else {
                throw CsvError("unexpected character '" + std::string(1, c) +
                               "' after closing quote near line " + std::to_string(line));
            }
            break;
        }
    }

    if (st == State::InQuoted)
        throw CsvError("unterminated quoted field at end of input (started before line " +
                       std::to_string(line) + ")");

    // Flush the last record if the file does not end with a newline.
    if (st != State::RecordStart) endRecord();

    return records;
}

}  // namespace minidb
