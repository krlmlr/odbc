skip_if_no_unixodbc()

test_that("dbSendQueryArrow() returns an OdbcResultArrow that fetches natively", {
  con <- test_con("SQLITE")
  tbl <- local_table(
    con,
    "arrow_fetch",
    data.frame(x = 1:3, y = c("a", "b", NA), z = c(1.5, NA, 3))
  )

  rs <- dbSendQueryArrow(con, paste0("SELECT * FROM ", tbl))
  on.exit(dbClearResult(rs))
  expect_s4_class(rs, "OdbcResultArrow")
  expect_s4_class(rs, "DBIResultArrow")
  expect_false(dbHasCompleted(rs))

  stream <- dbFetchArrow(rs)
  expect_s3_class(stream, "nanoarrow_array_stream")
  schema <- nanoarrow::infer_nanoarrow_schema(stream)
  expect_equal(
    vapply(schema$children, function(x) x$format, character(1)),
    c(x = "i", y = "u", z = "g")
  )
  expect_equal(
    as.data.frame(stream),
    data.frame(x = 1:3, y = c("a", "b", NA), z = c(1.5, NA, 3))
  )
  expect_true(dbHasCompleted(rs))
  expect_equal(dbGetRowCount(rs), 3)

  # Fetching again gives an empty stream with the same schema
  empty <- as.data.frame(dbFetchArrow(rs))
  expect_named(empty, c("x", "y", "z"))
  expect_equal(nrow(empty), 0)
})

test_that("dbFetchArrow() chunks the stream by chunk_size", {
  con <- test_con("SQLITE")
  tbl <- local_table(con, "arrow_chunks", data.frame(x = 1:10))

  rs <- dbSendQueryArrow(con, paste0("SELECT * FROM ", tbl))
  on.exit(dbClearResult(rs))
  stream <- dbFetchArrow(rs, chunk_size = 4)

  sizes <- integer()
  repeat {
    chunk <- stream$get_next()
    if (is.null(chunk)) {
      break
    }
    sizes <- c(sizes, chunk$length)
  }
  expect_equal(sizes, c(4L, 4L, 2L))
})

test_that("dbFetchArrowChunk() fetches one chunk at a time", {
  con <- test_con("SQLITE")
  tbl <- local_table(con, "arrow_chunk", data.frame(x = 1:5))

  rs <- dbSendQueryArrow(con, paste0("SELECT * FROM ", tbl))
  on.exit(dbClearResult(rs))

  chunk <- dbFetchArrowChunk(rs, chunk_size = 2)
  expect_s3_class(chunk, "nanoarrow_array")
  expect_equal(as.data.frame(chunk), data.frame(x = 1:2))
  expect_false(dbHasCompleted(rs))

  expect_equal(
    as.data.frame(dbFetchArrowChunk(rs, chunk_size = 2)),
    data.frame(x = 3:4)
  )
  expect_equal(
    as.data.frame(dbFetchArrowChunk(rs, chunk_size = 2)),
    data.frame(x = 5L)
  )
  expect_true(dbHasCompleted(rs))
  expect_equal(nrow(as.data.frame(dbFetchArrowChunk(rs))), 0)
  expect_equal(dbGetRowCount(rs), 5)
})

test_that("dbFetchArrow() and dbFetch() can be mixed on a result", {
  con <- test_con("SQLITE")
  tbl <- local_table(con, "arrow_mixed", data.frame(x = 1:4))

  rs <- dbSendQuery(con, paste0("SELECT * FROM ", tbl))
  on.exit(dbClearResult(rs))
  expect_equal(dbFetch(rs, n = 1), data.frame(x = 1L))
  expect_equal(
    as.data.frame(dbFetchArrowChunk(rs, chunk_size = 2)),
    data.frame(x = 2:3)
  )
  expect_equal(as.data.frame(dbFetchArrow(rs)), data.frame(x = 4L))
  expect_true(dbHasCompleted(rs))
})

test_that("chunk_size is validated", {
  con <- test_con("SQLITE")
  rs <- dbSendQueryArrow(con, "SELECT 1 AS a")
  on.exit(dbClearResult(rs))

  expect_snapshot(error = TRUE, dbFetchArrow(rs, chunk_size = 0))
  expect_snapshot(error = TRUE, dbFetchArrowChunk(rs, chunk_size = NA))
})

test_that("fetching from a cleared result errors", {
  con <- test_con("SQLITE")
  rs <- dbSendQueryArrow(con, "SELECT 1 AS a")
  dbClearResult(rs)

  expect_false(dbIsValid(rs))
  expect_error(dbFetchArrow(rs))
  expect_error(dbFetchArrowChunk(rs))
  expect_error(dbBindArrow(rs, data.frame(a = 1)))
})

test_that("dbGetQueryArrow() supports params and immediate", {
  con <- test_con("SQLITE")

  expect_equal(
    as.data.frame(dbGetQueryArrow(con, "SELECT 1.5 AS a", immediate = TRUE)),
    data.frame(a = 1.5)
  )
  expect_equal(
    as.data.frame(dbGetQueryArrow(
      con,
      "SELECT ? + 1.0 AS a",
      params = list(0.5)
    )),
    data.frame(a = 1.5)
  )
  expect_error(dbGetQueryArrow(con, "SELLECT"))
})

test_that("results without columns give an empty stream", {
  con <- test_con("SQLITE")
  tbl <- local_table(con, "arrow_statement", data.frame(x = 1:2))

  rs <- dbSendStatement(con, paste0("DELETE FROM ", tbl, " WHERE x = 1"))
  on.exit(dbClearResult(rs))
  expect_equal(dbGetRowsAffected(rs), 1)

  out <- as.data.frame(dbFetchArrow(rs))
  expect_equal(dim(out), c(0L, 0L))
})

test_that("dbBindArrow() binds Arrow arrays to parameters", {
  con <- test_con("SQLITE")

  rs <- dbSendQueryArrow(con, "SELECT ? + 1.0 AS a, ? AS b")
  on.exit(dbClearResult(rs))
  expect_false(dbHasCompleted(rs))
  expect_equal(dbGetRowCount(rs), 0)
  expect_error(dbFetchArrow(rs), "bound")

  expect_invisible(dbBindArrow(rs, data.frame(x = 0.5, y = "a")))
  expect_equal(as.data.frame(dbFetchArrow(rs)), data.frame(a = 1.5, b = "a"))
  expect_true(dbHasCompleted(rs))

  # Binding again resets the result; nanoarrow streams and arrays work too
  stream <- nanoarrow::as_nanoarrow_array_stream(data.frame(
    x = 1.5,
    y = NA_character_
  ))
  dbBindArrow(rs, stream)
  expect_equal(
    as.data.frame(dbFetchArrow(rs)),
    data.frame(a = 2.5, b = NA_character_)
  )

  dbBindArrow(rs, nanoarrow::as_nanoarrow_array(data.frame(x = 2.5, y = "c")))
  expect_equal(as.data.frame(dbFetchArrow(rs)), data.frame(a = 3.5, b = "c"))
})

test_that("dbBindArrow() checks the number of parameters", {
  con <- test_con("SQLITE")

  rs <- dbSendQueryArrow(con, "SELECT ? + 1.0 AS a")
  expect_snapshot(error = TRUE, dbBindArrow(rs, data.frame(x = 1, y = 2)))
  expect_snapshot(error = TRUE, dbBindArrow(rs, data.frame()))
  expect_snapshot(
    error = TRUE,
    dbBindArrow(rs, data.frame(x = 1), batch_rows = 0)
  )
  dbClearResult(rs)

  rs <- dbSendQueryArrow(con, "SELECT 1.5 AS a")
  on.exit(dbClearResult(rs))
  expect_snapshot(error = TRUE, dbBindArrow(rs, data.frame(x = 1)))
})

test_that("dbBindArrow() converts the supported Arrow types", {
  con <- test_con("SQLITE")

  # The driver types the result column from the first execution, so each
  # value gets its own statement
  bind_one <- function(value, sql = "SELECT ? AS a") {
    rs <- dbSendQuery(con, sql)
    on.exit(dbClearResult(rs))
    dbBindArrow(
      rs,
      structure(list(value), names = "x", class = "data.frame", row.names = 1L)
    )
    dbFetch(rs)$a
  }
  is_null <- "SELECT (? IS NULL) AS a"
  as_text <- "SELECT CAST(? AS TEXT) AS a"

  expect_equal(bind_one(TRUE), 1L)
  expect_equal(bind_one(NA, is_null), 1L)
  expect_equal(bind_one(1L), 1L)
  expect_equal(bind_one(bit64::as.integer64(2^40), as_text), "1099511627776")
  expect_equal(bind_one(1.5), 1.5)
  expect_equal(bind_one("x"), "x")
  expect_equal(bind_one(factor("y")), "y")
  expect_equal(bind_one(NA_character_, is_null), 1L)
  expect_equal(bind_one(blob::blob(as.raw(1:3))), blob::blob(as.raw(1:3)))
  expect_equal(bind_one(blob::blob(NULL), is_null), 1L)
  expect_equal(bind_one(as.Date("2020-01-02"), as_text), "2020-01-02")
})

test_that("dbBindArrow() rejects nested Arrow types", {
  con <- test_con("SQLITE")

  rs <- dbSendQuery(con, "SELECT ? AS a")
  on.exit(dbClearResult(rs))
  array <- nanoarrow::as_nanoarrow_array(
    data.frame(x = I(list(1:2))),
    schema = nanoarrow::na_struct(list(
      x = nanoarrow::na_list(nanoarrow::na_int32())
    ))
  )
  expect_snapshot(error = TRUE, dbBindArrow(rs, array))
})

test_that("dbWriteTableArrow() round trips a stream", {
  con <- test_con("SQLITE")
  df <- data.frame(a = c(1L, NA), b = c("x", NA), c = c(1.5, NA))
  withr::defer(dbRemoveTable(con, "arrow_write"))

  expect_invisible(dbWriteTableArrow(con, "arrow_write", df))
  expect_equal(as.data.frame(dbReadTableArrow(con, "arrow_write")), df)
  expect_equal(dbReadTable(con, "arrow_write"), df)

  expect_snapshot(error = TRUE, dbWriteTableArrow(con, "arrow_write", df))
  expect_snapshot(
    error = TRUE,
    dbWriteTableArrow(con, "arrow_write", df, append = TRUE, overwrite = TRUE)
  )
  expect_snapshot(
    error = TRUE,
    dbWriteTableArrow(
      con,
      "arrow_write",
      df,
      append = TRUE,
      field.types = c(a = "TEXT")
    )
  )

  dbWriteTableArrow(con, "arrow_write", df[2:1, ], overwrite = TRUE)
  expect_equal(
    dbReadTable(con, "arrow_write"),
    df[2:1, ],
    ignore_attr = "row.names"
  )

  dbWriteTableArrow(con, "arrow_write", df, append = TRUE, batch_rows = 1)
  expect_equal(nrow(dbReadTable(con, "arrow_write")), 4)
})

test_that("dbAppendTableArrow() returns the number of rows written", {
  con <- test_con("SQLITE")
  tbl <- local_table(con, "arrow_append", data.frame(a = 1:2, b = c("x", "y")))

  expect_equal(dbAppendTableArrow(con, tbl, data.frame(a = 3L, b = "z")), 1)
  expect_equal(dbAppendTableArrow(con, tbl, data.frame(b = c("u", "v"))), 2)
  expect_equal(dbAppendTableArrow(con, tbl, data.frame(a = integer())), 0)
  expect_equal(
    dbReadTable(con, tbl),
    data.frame(a = c(1:3, NA, NA), b = c("x", "y", "z", "u", "v"))
  )

  expect_snapshot(error = TRUE, dbAppendTableArrow(con, tbl, data.frame()))
  expect_error(dbAppendTableArrow(con, "arrow_missing", data.frame(a = 1)))
})

test_that("dbCreateTableArrow() creates a table from a schema without consuming a stream", {
  con <- test_con("SQLITE")
  withr::defer(dbRemoveTable(con, "arrow_create"))

  stream <- nanoarrow::as_nanoarrow_array_stream(
    data.frame(a = 1L, b = "x", c = bit64::as.integer64(1))
  )
  expect_invisible(dbCreateTableArrow(con, "arrow_create", stream))
  expect_equal(dbListFields(con, "arrow_create"), c("a", "b", "c"))
  expect_equal(nrow(dbReadTable(con, "arrow_create")), 0)
  expect_equal(nrow(as.data.frame(stream)), 1)

  expect_error(dbCreateTableArrow(
    con,
    "arrow_create",
    nanoarrow::infer_nanoarrow_schema(data.frame(a = 1))
  ))
  expect_snapshot(
    error = TRUE,
    dbCreateTableArrow(con, "arrow_create2", data.frame(a = 1), temporary = NA)
  )
})

test_that("dbReadTableArrow() validates the name", {
  con <- test_con("SQLITE")
  tbl <- local_table(con, "arrow_read", data.frame(a = 1.5))

  expect_equal(as.data.frame(dbReadTableArrow(con, tbl)), data.frame(a = 1.5))
  expect_equal(
    as.data.frame(dbReadTableArrow(con, dbQuoteIdentifier(con, tbl))),
    data.frame(a = 1.5)
  )
  expect_snapshot(error = TRUE, dbReadTableArrow(con, c(tbl, tbl)))
  expect_error(dbReadTableArrow(con, NA))
})

test_that("strings are converted to and from the database encoding", {
  con <- test_con("SQLITE", encoding = "latin1")
  df <- data.frame(a = c("Müller", "café"))
  withr::defer(dbRemoveTable(con, "arrow_encoding"))

  dbWriteTableArrow(con, "arrow_encoding", df)
  expect_equal(as.data.frame(dbReadTableArrow(con, "arrow_encoding")), df)
  expect_equal(dbReadTable(con, "arrow_encoding"), df)

  rs <- dbSendQuery(con, "SELECT ? AS a")
  on.exit(dbClearResult(rs))
  dbBindArrow(rs, df[2, , drop = FALSE])
  expect_equal(dbFetch(rs), df[2, , drop = FALSE], ignore_attr = "row.names")
})

# PostgreSQL ------------------------------------------------------------------

test_that("Arrow results and parameters use typed columns", {
  con <- test_con("POSTGRES")
  df <- data.frame(
    d = as.Date("2020-01-02") + 0:1,
    ts = as.POSIXct("2020-01-02 03:04:05.25", tz = "UTC") + 0:1,
    tm = hms::hms(c(3661.5, 3662.25)),
    b = blob::blob(as.raw(1:3), NULL),
    big = bit64::as.integer64(2^40 + 0:1),
    dec = c(12345678.9125, NA),
    lg = c(TRUE, NA),
    i = 1:2,
    x = c(1.5, NA),
    s = c("\u00e4", NA)
  )
  withr::defer(dbRemoveTable(con, "arrow_typed"))
  dbWriteTableArrow(
    con,
    "arrow_typed",
    df,
    field.types = c(big = "BIGINT", dec = "NUMERIC(12, 4)")
  )

  stream <- dbReadTableArrow(con, "arrow_typed")
  schema <- nanoarrow::infer_nanoarrow_schema(stream)
  expect_equal(
    vapply(schema$children, function(x) x$format, character(1)),
    c(
      d = "tdD",
      ts = "tsu:UTC",
      tm = "ttu",
      b = "z",
      big = "l",
      dec = "d:12,4",
      lg = "b",
      i = "i",
      x = "g",
      s = "u"
    )
  )
  out <- as.data.frame(stream)
  expect_equal(out$big, as.numeric(df$big))
  out$big <- bit64::as.integer64(out$big)
  expect_equal(out, df)

  # The data frame path drops fractional seconds of times
  expected <- df
  expected$tm <- hms::hms(c(3661, 3662))
  expect_equal(dbReadTable(con, "arrow_typed"), expected)

  for (col in c("d", "big", "i")) {
    rs <- dbSendQueryArrow(
      con,
      paste0("SELECT i FROM arrow_typed WHERE ", col, " = ?")
    )
    dbBindArrow(rs, df[2, col, drop = FALSE])
    expect_equal(as.data.frame(dbFetchArrow(rs)), data.frame(i = 2L))
    dbClearResult(rs)
  }
})

test_that("Arrow timestamps respect timezone and timezone_out", {
  con <- test_con(
    "POSTGRES",
    timezone = "America/New_York",
    timezone_out = "Europe/Berlin"
  )
  ts <- as.POSIXct("2020-07-02 03:04:05", tz = "UTC")
  withr::defer(dbRemoveTable(con, "arrow_tz"))
  dbWriteTableArrow(con, "arrow_tz", data.frame(ts = ts))

  # Written as civil time in the connection time zone
  expect_equal(
    dbGetQuery(
      con,
      "SELECT to_char(ts, 'YYYY-MM-DD HH24:MI:SS') AS a FROM arrow_tz"
    )$a,
    "2020-07-01 23:04:05"
  )

  stream <- dbReadTableArrow(con, "arrow_tz")
  expect_equal(
    nanoarrow::infer_nanoarrow_schema(stream)$children$ts$format,
    "tsu:Europe/Berlin"
  )
  out <- as.data.frame(stream)$ts
  expect_equal(attr(out, "tzone"), "Europe/Berlin")
  expect_equal(as.numeric(out), as.numeric(ts))
  expect_equal(out, dbReadTable(con, "arrow_tz")$ts)
})

test_that("BIGINT columns are int64 regardless of the bigint argument", {
  for (bigint in c("integer64", "integer", "numeric", "character")) {
    con <- test_con("POSTGRES", bigint = bigint)
    stream <- dbGetQueryArrow(con, "SELECT CAST(1 AS BIGINT) AS a")
    expect_equal(
      nanoarrow::infer_nanoarrow_schema(stream)$children$a$format,
      "l",
      info = bigint
    )
    dbDisconnect(con)
  }
})

test_that("DECIMAL columns keep their digits", {
  con <- test_con("POSTGRES")

  stream <- dbGetQueryArrow(
    con,
    "SELECT CAST('-1234567890123456789.123456789' AS NUMERIC(30, 9)) AS a,
            CAST('0.5' AS NUMERIC(45, 10)) AS b"
  )
  schema <- nanoarrow::infer_nanoarrow_schema(stream)
  expect_equal(schema$children$a$format, "d:30,9")
  expect_equal(schema$children$b$format, "d:45,10,256")
  arr <- stream$get_next()
  expect_equal(
    as.data.frame(arr),
    data.frame(a = -1234567890123456789.123456789, b = 0.5)
  )
})

test_that("multi-batch streams are bound batch by batch", {
  con <- test_con("POSTGRES")
  tbl <- local_table(
    con,
    "arrow_batches",
    data.frame(a = integer(), b = character())
  )
  stream <- nanoarrow::basic_array_stream(list(
    nanoarrow::as_nanoarrow_array(data.frame(a = 1:2, b = c("x", "y"))),
    nanoarrow::as_nanoarrow_array(data.frame(a = 3L, b = "z"))
  ))

  rs <- dbSendStatement(
    con,
    paste0("INSERT INTO ", tbl, " (a, b) VALUES (?, ?)")
  )
  dbBindArrow(rs, stream)
  dbClearResult(rs)
  expect_equal(dbReadTable(con, tbl), data.frame(a = 1:3, b = c("x", "y", "z")))

  stream <- nanoarrow::basic_array_stream(list(
    nanoarrow::as_nanoarrow_array(data.frame(a = 4:5, b = c("u", "v"))),
    nanoarrow::as_nanoarrow_array(data.frame(a = 6L, b = "w"))
  ))
  expect_equal(dbAppendTableArrow(con, tbl, stream, batch_rows = 1), 3)
  expect_equal(nrow(dbReadTable(con, tbl)), 6)
})
