# Native Arrow support

odbc implements the Arrow generics of DBI natively: the rows of a result
set are written from the ODBC buffers directly into Arrow arrays, and
Arrow arrays are bound directly to query parameters, without passing
through R vectors. Arrow data is exchanged as
[nanoarrow](https://arrow.apache.org/nanoarrow/) objects:

- [`DBI::dbSendQueryArrow()`](https://dbi.r-dbi.org/reference/dbSendQueryArrow.html)
  returns an
  [OdbcResultArrow](https://odbc.r-dbi.org/dev/reference/OdbcResultArrow.md)
  object.

- [`DBI::dbFetchArrow()`](https://dbi.r-dbi.org/reference/dbFetchArrow.html),
  [`DBI::dbGetQueryArrow()`](https://dbi.r-dbi.org/reference/dbGetQueryArrow.html),
  and
  [`DBI::dbReadTableArrow()`](https://dbi.r-dbi.org/reference/dbReadTableArrow.html)
  return a nanoarrow array stream (see
  [`nanoarrow::as_nanoarrow_array_stream()`](https://arrow.apache.org/nanoarrow/latest/r/reference/as_nanoarrow_array_stream.html)),
  [`DBI::dbFetchArrowChunk()`](https://dbi.r-dbi.org/reference/dbFetchArrowChunk.html)
  returns a nanoarrow array (see
  [`nanoarrow::as_nanoarrow_array()`](https://arrow.apache.org/nanoarrow/latest/r/reference/as_nanoarrow_array.html)).
  Convert them with
  [`as.data.frame()`](https://rdrr.io/r/base/as.data.frame.html),
  [`nanoarrow::convert_array_stream()`](https://arrow.apache.org/nanoarrow/latest/r/reference/convert_array_stream.html),
  or `arrow::as_arrow_table()`.

- [`DBI::dbBindArrow()`](https://dbi.r-dbi.org/reference/dbBind.html),
  [`DBI::dbAppendTableArrow()`](https://dbi.r-dbi.org/reference/dbAppendTableArrow.html),
  [`DBI::dbWriteTableArrow()`](https://dbi.r-dbi.org/reference/dbWriteTableArrow.html),
  and
  [`DBI::dbCreateTableArrow()`](https://dbi.r-dbi.org/reference/dbCreateTableArrow.html)
  accept anything that
  [`nanoarrow::as_nanoarrow_array_stream()`](https://arrow.apache.org/nanoarrow/latest/r/reference/as_nanoarrow_array_stream.html)
  can convert, for example a data frame, a nanoarrow array or array
  stream, or an Arrow table or record batch reader.

## Usage

``` r
# S4 method for class 'OdbcConnection,character'
dbSendQueryArrow(conn, statement, params = NULL, ..., immediate = FALSE)

# S4 method for class 'OdbcConnection,character'
dbGetQueryArrow(
  conn,
  statement,
  params = NULL,
  immediate = is.null(params),
  ...
)

# S4 method for class 'OdbcResult'
dbFetchArrow(res, ..., chunk_size = 65536L)

# S4 method for class 'OdbcResultArrow'
dbFetchArrow(res, ..., chunk_size = 65536L)

# S4 method for class 'OdbcResult'
dbFetchArrowChunk(res, ..., chunk_size = 65536L)

# S4 method for class 'OdbcResultArrow'
dbFetchArrowChunk(res, ..., chunk_size = 65536L)

# S4 method for class 'OdbcResult'
dbBindArrow(res, params, ..., batch_rows = getOption("odbc.batch_rows", NA))

# S4 method for class 'OdbcResultArrow'
dbBindArrow(res, params, ..., batch_rows = getOption("odbc.batch_rows", NA))

# S4 method for class 'OdbcConnection'
dbReadTableArrow(conn, name, ...)

# S4 method for class 'OdbcConnection'
dbCreateTableArrow(
  conn,
  name,
  value,
  ...,
  field.types = NULL,
  temporary = FALSE
)

# S4 method for class 'OdbcConnection'
dbAppendTableArrow(
  conn,
  name,
  value,
  ...,
  batch_rows = getOption("odbc.batch_rows", NA)
)

# S4 method for class 'OdbcConnection'
dbWriteTableArrow(
  conn,
  name,
  value,
  append = FALSE,
  overwrite = FALSE,
  ...,
  temporary = FALSE,
  field.types = NULL,
  batch_rows = getOption("odbc.batch_rows", NA)
)
```

## Arguments

- conn:

  An
  [OdbcConnection](https://odbc.r-dbi.org/dev/reference/OdbcConnection.md)
  object, produced by
  [`DBI::dbConnect()`](https://dbi.r-dbi.org/reference/dbConnect.html).

- statement:

  A character string containing SQL.

- params:

  Query parameters, see
  [`DBI::dbBind()`](https://dbi.r-dbi.org/reference/dbBind.html) and
  [`DBI::dbBindArrow()`](https://dbi.r-dbi.org/reference/dbBind.html).
  [`dbBindArrow()`](https://dbi.r-dbi.org/reference/dbBind.html) accepts
  anything that
  [`nanoarrow::as_nanoarrow_array_stream()`](https://arrow.apache.org/nanoarrow/latest/r/reference/as_nanoarrow_array_stream.html)
  can convert, with one column per parameter.

- ...:

  Passed on to other methods.

- immediate:

  If `TRUE`, `SQLExecDirect` will be used instead of `SQLPrepare`, and
  the `params` argument is ignored.

- res:

  An [OdbcResult](https://odbc.r-dbi.org/dev/reference/OdbcResult.md) or
  [OdbcResultArrow](https://odbc.r-dbi.org/dev/reference/OdbcResultArrow.md)
  object.

- chunk_size:

  The maximum number of rows in each Arrow array.

- batch_rows:

  The number of rows bound per execution of the statement. Defaults to
  `NA`, which executes the statement once per array of the stream for
  [`dbBindArrow()`](https://dbi.r-dbi.org/reference/dbBind.html), and in
  batches of 1024 rows for
  [`dbAppendTableArrow()`](https://dbi.r-dbi.org/reference/dbAppendTableArrow.html)
  and
  [`dbWriteTableArrow()`](https://dbi.r-dbi.org/reference/dbWriteTableArrow.html).

- name:

  The table name, see
  [`DBI::dbQuoteIdentifier()`](https://dbi.r-dbi.org/reference/dbQuoteIdentifier.html).

- value:

  The data to write, as anything that
  [`nanoarrow::as_nanoarrow_array_stream()`](https://arrow.apache.org/nanoarrow/latest/r/reference/as_nanoarrow_array_stream.html)
  can convert. For
  [`dbCreateTableArrow()`](https://dbi.r-dbi.org/reference/dbCreateTableArrow.html),
  a nanoarrow schema (see
  [`nanoarrow::as_nanoarrow_schema()`](https://arrow.apache.org/nanoarrow/latest/r/reference/as_nanoarrow_schema.html))
  is accepted as well.

- field.types:

  Additional field types used to override derived types.

- temporary:

  If `TRUE`, create a temporary table.

- append:

  Allow appending to the destination table. Cannot be `TRUE` if
  `overwrite` is also `TRUE`.

- overwrite:

  Allow overwriting the destination table. Cannot be `TRUE` if `append`
  is also `TRUE`.

## Result columns

Columns of a result set are mapped to Arrow types as follows:

- `BIT` to `bool`.

- `TINYINT`, `SMALLINT`, and `INTEGER` to `int32`.

- `BIGINT` to `int64` by default, or to `int32`, `double`, or `utf8`
  according to the `bigint` argument of
  [`DBI::dbConnect()`](https://dbi.r-dbi.org/reference/dbConnect.html).

- `REAL`, `FLOAT`, `DOUBLE`, `DECIMAL`, and `NUMERIC` to `double`.

- `DATE` to `date32`.

- `TIME` to `time32` with second precision.

- `TIMESTAMP`, and SQL Server `DATETIMEOFFSET`, to `timestamp` with
  microsecond precision. As for data frames, values are interpreted in
  the `timezone` of the connection, unless the type itself carries an
  offset, and are labelled with the `timezone_out` of the connection.

- Character types to `utf8`, converted from the `encoding` of the
  connection if needed.

- Binary types to `binary`.

- Other types to `utf8`.

## Parameters

Arrow arrays are bound to query parameters as follows:

- `bool` and integers with up to 32 bits as 32-bit integers, `int64` and
  `uint32` as 64-bit integers, `uint64` as unsigned 64-bit integers.

- Floating point types as `double`.

- Decimal types as strings holding their exact decimal representation.

- `utf8`, `large_utf8`, `utf8_view`, and dictionaries of these types as
  strings, converted to the `encoding` of the connection if needed.

- `binary`, `large_binary`, `fixed_size_binary`, and `binary_view` as
  binary data.

- `date32` and `date64` as `DATE`.

- `time32`, `time64`, and `duration` as `TIME`, dropping fractional
  seconds.

- `timestamp` with a time zone as `TIMESTAMP`, expressed in the
  `timezone` of the connection, or with its own time zone when the
  target is a SQL Server `DATETIMEOFFSET`. `timestamp` without a time
  zone is written as is.

- `null` as `NULL`.

Nested types are not supported.

## Chunking

[`dbFetchArrow()`](https://dbi.r-dbi.org/reference/dbFetchArrow.html)
fetches all remaining rows before returning, as an array stream made of
arrays of at most `chunk_size` rows each; an array is also cut short
once a string or binary column holds 1 GiB of data.
[`dbFetchArrowChunk()`](https://dbi.r-dbi.org/reference/dbFetchArrowChunk.html)
returns one such array per call, and an empty array once all rows have
been fetched.

When binding, the statement is executed once for each array of the
stream, in batches of at most `batch_rows` rows. For queries, only the
rows returned by the last execution can be fetched afterwards.

## Examples

``` r
if (FALSE) { # \dontrun{
library(DBI)
con <- dbConnect(odbc::odbc(), dsn = "PostgreSQL")
dbWriteTableArrow(con, "mtcars", mtcars, temporary = TRUE)

# A nanoarrow array stream, convert with as.data.frame() or
# arrow::as_arrow_table()
stream <- dbReadTableArrow(con, "mtcars")
as.data.frame(stream)

# Fetch in chunks
rs <- dbSendQueryArrow(con, "SELECT * FROM mtcars")
while (!dbHasCompleted(rs)) {
  chunk <- dbFetchArrowChunk(rs, chunk_size = 10)
  print(nrow(as.data.frame(chunk)))
}
dbClearResult(rs)

# Bind parameters from an Arrow stream
rs <- dbSendQueryArrow(con, "SELECT * FROM mtcars WHERE cyl = ?")
dbBindArrow(rs, data.frame(cyl = 4))
as.data.frame(dbFetchArrow(rs))
dbClearResult(rs)

dbDisconnect(con)
} # }
```
