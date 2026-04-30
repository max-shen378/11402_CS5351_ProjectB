# Project B- CSV Mini Database and Query Engine

## Project Overview
A compact CSV database engine that ingests CSV data, converts it into typed in-memory tables, and executes SQL-like queries with filtering, projection, aggregation, and join support. Designed to demonstrate core database concepts in a lightweight, extensible form.

## Objectives
- Implement a lightweight CSV-based table storage system
- Parse CSV files into structured in-memory tables
- Support basic query operations for filtering, projection, aggregation, and join
- Demonstrate query parsing and execution in a compact engine
- Provide a simple CLI or API for running queries against CSV data

## Key Features

- CSV file ingestion and schema detection
- Table representation with rows and typed columns
- Query parser for a minimal SQL-like language
- Filter conditions using comparison operators
- Projection of selected columns
- Aggregation functions such as `COUNT`, `SUM`, `AVG`, `MIN`, `MAX`
- Simple join support between tables
- Error handling for malformed input and invalid queries

## Technical Requirements

- Language: likely Python, Java, or C# (choose one based on course/project constraints)
- Parse CSV files reliably, including quoted fields and delimiters
- Represent tables in memory using arrays, lists, or equivalent structures
- Build a query engine capable of parsing and executing simple SQL-like statements
- Support basic data types: string, integer, float, boolean
- Provide tests for CSV parsing, query execution, and aggregation results
- Ensure the implementation is modular and extensible

## SQL Basics

- `SELECT` specifies which columns to return
- `FROM` identifies the input table
- `WHERE` filters rows by conditions
- `JOIN` combines rows from two tables based on a matching condition
- `GROUP BY` groups rows for aggregation
- `ORDER BY` sorts the result set
- Example query patterns:
    - `SELECT col1, col2 FROM table WHERE col3 > 100`
    - `SELECT COUNT(*), AVG(col4) FROM table`
    - `SELECT t1.colA, t2.colB FROM table1 t1 JOIN table2 t2 ON t1.id = t2.ref_id`
    - `SELECT col1 FROM table WHERE col2 = 'value' ORDER BY col1`
- Aggregation functions:
    - `COUNT(column)` or `COUNT(*)`
    - `SUM(column)`
    - `AVG(column)`
    - `MIN(column)`
    - `MAX(column)`
- Use `AND` / `OR` to combine predicates in `WHERE` clauses
- Basic comparison operators: `=`, `!=`, `<`, `<=`, `>`, `>=`
- Support literal values for numbers and strings in query conditions
- Select expressions may include column aliases, e.g. `SELECT col1 AS alias1`
- A minimal engine may omit advanced features like subqueries, window functions, and complex joins for clarity
Project B is an implementation of a CSV-based mini database and query engine. It focuses on parsing CSV files into structured tables, storing rows efficiently, and executing a compact query language for filtering, projection, aggregation, and simple joins. The implementation demonstrates core database engine concepts in a lightweight, extensible form.
