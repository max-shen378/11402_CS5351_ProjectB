-- demo.sql — walkthrough of the CSV Mini Database query engine
-- Run with:  ./minidb data/Iris.csv data/SpeciesInfo.csv -f queries/demo.sql

-- [1] FILTER + SORT — select rows matching a condition, ordered by value
SELECT Id, SepalLengthCm, Species
FROM Iris
WHERE SepalLengthCm > 7.0
ORDER BY SepalLengthCm DESC;

-- [2] AGGREGATE — whole-table statistics
SELECT COUNT(*) AS total_rows,
       AVG(SepalLengthCm) AS avg_sepal,
       MIN(SepalWidthCm)  AS min_width,
       MAX(SepalWidthCm)  AS max_width
FROM Iris;

-- [3] GROUP BY — per-species statistics
SELECT Species,
       COUNT(*)              AS n,
       AVG(PetalLengthCm)   AS avg_petal,
       MAX(PetalWidthCm)    AS max_width
FROM Iris
GROUP BY Species
ORDER BY avg_petal DESC;

-- [4] JOIN — combine two CSV tables on a shared key
SELECT i.Id, s.CommonName, i.PetalLengthCm
FROM Iris i
JOIN SpeciesInfo s ON i.Species = s.Species
WHERE i.PetalLengthCm > 6.3
ORDER BY i.PetalLengthCm DESC;

-- [5] JOIN + AGGREGATE — aggregate across joined tables
SELECT s.CommonName,
       COUNT(*)              AS n,
       AVG(i.SepalLengthCm) AS avg_sepal
FROM Iris i
JOIN SpeciesInfo s ON i.Species = s.Species
GROUP BY s.CommonName
ORDER BY avg_sepal DESC;
