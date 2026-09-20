# OdbcResultArrow delegates DBI methods to the underlying result

    Code
      rs
    Output
      <OdbcResultArrow>
        SQL  SELECT 1.5 AS a
        ROWS Fetched: 0 [incomplete]
             Changed: 0

---

    Code
      rs
    Output
      <OdbcResultArrow>
        SQL  SELECT 1.5 AS a
        ROWS Fetched: 1 [complete]

---

    Code
      rs
    Output
      <OdbcResultArrow>
        EXPIRED

