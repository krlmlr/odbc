# Odbc Arrow result methods

`OdbcResultArrow` objects are returned by
[`DBI::dbSendQueryArrow()`](https://dbi.r-dbi.org/reference/dbSendQueryArrow.html).
They wrap an
[OdbcResult](https://odbc.r-dbi.org/dev/reference/OdbcResult.md) and
implement the methods defined in the `DBI` package for
[DBI::DBIResultArrow](https://dbi.r-dbi.org/reference/DBIResultArrow-class.html)
objects by delegating to it. See
[DBI-arrow](https://odbc.r-dbi.org/dev/reference/DBI-arrow.md) for the
native Arrow interface of odbc.

## Usage

``` r
# S4 method for class 'OdbcResultArrow'
show(object)

# S4 method for class 'OdbcResultArrow'
dbClearResult(res, ...)

# S4 method for class 'OdbcResultArrow'
dbIsValid(dbObj, ...)

# S4 method for class 'OdbcResultArrow'
dbHasCompleted(res, ...)

# S4 method for class 'OdbcResultArrow'
dbGetStatement(res, ...)

# S4 method for class 'OdbcResultArrow'
dbColumnInfo(res, ...)

# S4 method for class 'OdbcResultArrow'
dbGetRowCount(res, ...)

# S4 method for class 'OdbcResultArrow'
dbGetRowsAffected(res, ...)

# S4 method for class 'OdbcResultArrow'
dbGetInfo(dbObj, ...)

# S4 method for class 'OdbcResultArrow'
dbBind(res, params, ...)
```

## Arguments

- res, dbObj, object:

  An `OdbcResultArrow` object.

- ...:

  Passed on to the corresponding
  [OdbcResult](https://odbc.r-dbi.org/dev/reference/OdbcResult.md)
  method.

- params:

  Query parameters, see
  [`DBI::dbBind()`](https://dbi.r-dbi.org/reference/dbBind.html).
