-- demo.sql -- sample queries against the Iris dataset.
-- Run with:  ./minidb data/Iris.csv data/SpeciesInfo.csv -f queries/demo.sql

-- projection + filter + sort
SELECT Id, SepalLengthCm, Species
FROM Iris
WHERE SepalLengthCm > 7.0
ORDER BY SepalLengthCm DESC;

-- aggregation over the whole table
SELECT COUNT(*), AVG(SepalLengthCm), MIN(SepalWidthCm), MAX(SepalWidthCm)
FROM Iris;

-- per-species statistics
SELECT Species, COUNT(*) AS n, AVG(PetalLengthCm) AS avg_petal, MAX(PetalWidthCm) AS max_width
FROM Iris
GROUP BY Species
ORDER BY avg_petal DESC;

-- compound predicates
SELECT COUNT(*) AS narrow_setosa
FROM Iris
WHERE Species = 'Iris-setosa' AND (SepalWidthCm < 3.0 OR PetalWidthCm > 0.4);

-- join against the species lookup table
SELECT i.Id, s.CommonName, i.PetalLengthCm
FROM Iris i
JOIN SpeciesInfo s ON i.Species = s.Species
WHERE i.PetalLengthCm > 6.3
ORDER BY i.PetalLengthCm DESC;

-- aggregate over a join
SELECT s.CommonName, COUNT(*) AS n, AVG(i.SepalLengthCm) AS avg_sepal
FROM Iris i
JOIN SpeciesInfo s ON i.Species = s.Species
GROUP BY s.CommonName
ORDER BY avg_sepal DESC;
