skip_if_no_unixodbc()

test_that("OdbcResultArrow delegates DBI methods to the underlying result", {
  con <- test_con("SQLITE")

  rs <- dbSendQueryArrow(con, "SELECT 1.5 AS a")
  expect_snapshot(rs)
  expect_true(dbIsValid(rs))
  expect_equal(dbGetStatement(rs), "SELECT 1.5 AS a")
  expect_equal(dbColumnInfo(rs)$name, "a")
  expect_equal(dbGetRowCount(rs), 0)
  expect_equal(dbGetRowsAffected(rs), 0)
  expect_type(dbGetInfo(rs), "list")

  expect_equal(as.data.frame(dbFetchArrow(rs)), data.frame(a = 1.5))
  expect_snapshot(rs)

  expect_true(dbClearResult(rs))
  expect_false(dbIsValid(rs))
  expect_snapshot(rs)
  expect_warning(dbClearResult(rs), "already cleared")
})

test_that("dbBind() works on an OdbcResultArrow", {
  con <- test_con("SQLITE")

  rs <- dbSendQueryArrow(con, "SELECT ? + 1.0 AS a")
  on.exit(dbClearResult(rs))
  expect_identical(dbBind(rs, list(0.5)), rs)
  expect_equal(as.data.frame(dbFetchArrow(rs)), data.frame(a = 1.5))
  expect_equal(dbFetch(rs), data.frame(a = numeric()))
})

test_that("dbColumnInfo() works before parameters are bound", {
  con <- test_con("SQLITE")

  rs <- dbSendQueryArrow(con, "SELECT ? AS a")
  on.exit(dbClearResult(rs))
  expect_equal(nrow(dbColumnInfo(rs)), 0)
  expect_false(dbHasCompleted(rs))
})

test_that("a second query invalidates an open Arrow result", {
  con <- test_con("SQLITE")

  rs1 <- dbSendQueryArrow(con, "SELECT 1 AS a")
  expect_warning(rs2 <- dbSendQueryArrow(con, "SELECT 2 AS a"), "Cancelling")
  on.exit(dbClearResult(rs2))
  expect_false(dbIsValid(rs1))
  expect_true(dbIsValid(rs2))
})
