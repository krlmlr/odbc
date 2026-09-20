# chunk_size is validated

    Code
      dbFetchArrow(rs, chunk_size = 0)
    Condition
      Error in `dbFetchArrow()`:
      ! `chunk_size` must be a whole number larger than or equal to 1, not the number 0.

---

    Code
      dbFetchArrowChunk(rs, chunk_size = NA)
    Condition
      Error in `dbFetchArrowChunk()`:
      ! `chunk_size` must be a whole number, not `NA`.

# dbBindArrow() checks the number of parameters

    Code
      dbBindArrow(rs, data.frame(x = 1, y = 2))
    Condition
      Error:
      ! Query requires '1' params; '2' supplied.

---

    Code
      dbBindArrow(rs, data.frame())
    Condition
      Error:
      ! Query requires '1' params; '0' supplied.

---

    Code
      dbBindArrow(rs, data.frame(x = 1), batch_rows = 0)
    Condition
      Error in `dbBindArrow()`:
      ! `batch_rows` must be a whole number larger than or equal to 1 or `NA`, not the number 0.

---

    Code
      dbBindArrow(rs, data.frame(x = 1))
    Condition
      Error:
      ! Query does not require parameters.

# dbBindArrow() rejects nested Arrow types

    Code
      dbBindArrow(rs, array)
    Condition
      Error:
      ! Can't bind Arrow type 'list' (parameter 1).

# dbWriteTableArrow() round trips a stream

    Code
      dbWriteTableArrow(con, "arrow_write", df)
    Condition
      Error in `dbWriteTableArrow()`:
      ! Table arrow_write exists in database, and both overwrite and append are `FALSE`.

---

    Code
      dbWriteTableArrow(con, "arrow_write", df, append = TRUE, overwrite = TRUE)
    Condition
      Error in `dbWriteTableArrow()`:
      ! `overwrite` and `append` cannot both be "TRUE".

---

    Code
      dbWriteTableArrow(con, "arrow_write", df, append = TRUE, field.types = c(a = "TEXT"))
    Condition
      Error in `dbWriteTableArrow()`:
      ! Cannot specify `field.types` with `append = TRUE`.

# dbAppendTableArrow() returns the number of rows written

    Code
      dbAppendTableArrow(con, tbl, data.frame())
    Condition
      Error in `dbAppendTableArrow()`:
      ! `value` must have at least one column.

# dbCreateTableArrow() creates a table from a schema without consuming a stream

    Code
      dbCreateTableArrow(con, "arrow_create2", data.frame(a = 1), temporary = NA)
    Condition
      Error in `dbCreateTableArrow()`:
      ! `temporary` must be `TRUE` or `FALSE`, not `NA`.

# dbReadTableArrow() validates the name

    Code
      dbReadTableArrow(con, c(tbl, tbl))
    Condition
      Error:
      ! `name` must identify a single table.

