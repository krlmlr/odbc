#' @include dbi-result-arrow.R
NULL

#' Native Arrow support
#'
#' @description
#' odbc implements the Arrow generics of DBI natively: the rows of a result
#' set are written from the ODBC buffers directly into Arrow arrays, and Arrow
#' arrays are bound directly to query parameters, without passing through R
#' vectors. Arrow data is exchanged as
#' [nanoarrow](https://arrow.apache.org/nanoarrow/) objects:
#'
#' * [DBI::dbSendQueryArrow()] returns an [OdbcResultArrow] object.
#' * [DBI::dbFetchArrow()], [DBI::dbGetQueryArrow()], and
#'   [DBI::dbReadTableArrow()] return a nanoarrow array stream (see
#'   [nanoarrow::as_nanoarrow_array_stream()]), [DBI::dbFetchArrowChunk()]
#'   returns a nanoarrow array (see [nanoarrow::as_nanoarrow_array()]).
#'   Convert them with [as.data.frame()],
#'   [nanoarrow::convert_array_stream()], or `arrow::as_arrow_table()`.
#' * [DBI::dbBindArrow()], [DBI::dbAppendTableArrow()],
#'   [DBI::dbWriteTableArrow()], and [DBI::dbCreateTableArrow()] accept
#'   anything that [nanoarrow::as_nanoarrow_array_stream()] can convert, for
#'   example a data frame, a nanoarrow array or array stream, or an Arrow
#'   table or record batch reader.
#'
#' @section Result columns:
#'
#' Columns of a result set are mapped to Arrow types as follows:
#'
#' * `BIT` to `bool`.
#' * `TINYINT`, `SMALLINT`, and `INTEGER` to `int32`.
#' * `BIGINT` to `int64` by default, or to `int32`, `double`, or `utf8`
#'   according to the `bigint` argument of [DBI::dbConnect()].
#' * `REAL`, `FLOAT`, `DOUBLE`, `DECIMAL`, and `NUMERIC` to `double`.
#' * `DATE` to `date32`.
#' * `TIME` to `time32` with second precision.
#' * `TIMESTAMP`, and SQL Server `DATETIMEOFFSET`, to `timestamp` with
#'   microsecond precision. As for data frames, values are interpreted in the
#'   `timezone` of the connection, unless the type itself carries an offset,
#'   and are labelled with the `timezone_out` of the connection.
#' * Character types to `utf8`, converted from the `encoding` of the connection
#'   if needed.
#' * Binary types to `binary`.
#' * Other types to `utf8`.
#'
#' @section Parameters:
#'
#' Arrow arrays are bound to query parameters as follows:
#'
#' * `bool` and integers with up to 32 bits as 32-bit integers, `int64` and
#'   `uint32` as 64-bit integers, `uint64` as unsigned 64-bit integers.
#' * Floating point types as `double`.
#' * Decimal types as strings holding their exact decimal representation.
#' * `utf8`, `large_utf8`, `utf8_view`, and dictionaries of these types as
#'   strings, converted to the `encoding` of the connection if needed.
#' * `binary`, `large_binary`, `fixed_size_binary`, and `binary_view` as binary
#'   data.
#' * `date32` and `date64` as `DATE`.
#' * `time32`, `time64`, and `duration` as `TIME`, dropping fractional seconds.
#' * `timestamp` with a time zone as `TIMESTAMP`, expressed in the `timezone`
#'   of the connection, or with its own time zone when the target is a SQL
#'   Server `DATETIMEOFFSET`. `timestamp` without a time zone is written as is.
#' * `null` as `NULL`.
#'
#' Nested types are not supported.
#'
#' @section Chunking:
#'
#' `dbFetchArrow()` fetches all remaining rows before returning, as an array
#' stream made of arrays of at most `chunk_size` rows each; an array is also
#' cut short once a string or binary column holds 1 GiB of data.
#' `dbFetchArrowChunk()` returns one such array per call, and an empty array
#' once all rows have been fetched.
#'
#' When binding, the statement is executed once for each array of the stream,
#' in batches of at most `batch_rows` rows. For queries, only the rows returned
#' by the last execution can be fetched afterwards.
#'
#' @param conn An [OdbcConnection] object, produced by [DBI::dbConnect()].
#' @param statement A character string containing SQL.
#' @param res An [OdbcResult] or [OdbcResultArrow] object.
#' @param params Query parameters, see [DBI::dbBind()] and
#'   [DBI::dbBindArrow()]. `dbBindArrow()` accepts anything that
#'   [nanoarrow::as_nanoarrow_array_stream()] can convert, with one column per
#'   parameter.
#' @param immediate If `TRUE`, `SQLExecDirect` will be used instead of
#'   `SQLPrepare`, and the `params` argument is ignored.
#' @param name The table name, see [DBI::dbQuoteIdentifier()].
#' @param value The data to write, as anything that
#'   [nanoarrow::as_nanoarrow_array_stream()] can convert. For
#'   `dbCreateTableArrow()`, a nanoarrow schema (see
#'   [nanoarrow::as_nanoarrow_schema()]) is accepted as well.
#' @param chunk_size The maximum number of rows in each Arrow array.
#' @param batch_rows The number of rows bound per execution of the statement.
#'   Defaults to `NA`, which executes the statement once per array of the
#'   stream for `dbBindArrow()`, and in batches of 1024 rows for
#'   `dbAppendTableArrow()` and `dbWriteTableArrow()`.
#' @param overwrite Allow overwriting the destination table. Cannot be
#'   `TRUE` if `append` is also `TRUE`.
#' @param append Allow appending to the destination table. Cannot be
#'   `TRUE` if `overwrite` is also `TRUE`.
#' @param temporary If `TRUE`, create a temporary table.
#' @param field.types Additional field types used to override derived types.
#' @param ... Passed on to other methods.
#'
#' @examples
#' \dontrun{
#' library(DBI)
#' con <- dbConnect(odbc::odbc(), dsn = "PostgreSQL")
#' dbWriteTableArrow(con, "mtcars", mtcars, temporary = TRUE)
#'
#' # A nanoarrow array stream, convert with as.data.frame() or
#' # arrow::as_arrow_table()
#' stream <- dbReadTableArrow(con, "mtcars")
#' as.data.frame(stream)
#'
#' # Fetch in chunks
#' rs <- dbSendQueryArrow(con, "SELECT * FROM mtcars")
#' while (!dbHasCompleted(rs)) {
#'   chunk <- dbFetchArrowChunk(rs, chunk_size = 10)
#'   print(nrow(as.data.frame(chunk)))
#' }
#' dbClearResult(rs)
#'
#' # Bind parameters from an Arrow stream
#' rs <- dbSendQueryArrow(con, "SELECT * FROM mtcars WHERE cyl = ?")
#' dbBindArrow(rs, data.frame(cyl = 4))
#' as.data.frame(dbFetchArrow(rs))
#' dbClearResult(rs)
#'
#' dbDisconnect(con)
#' }
#' @name DBI-arrow
NULL

#' @rdname DBI-arrow
#' @export
setMethod(
  "dbSendQueryArrow",
  c("OdbcConnection", "character"),
  function(conn, statement, params = NULL, ..., immediate = FALSE) {
    res <- dbSendQuery(
      conn,
      statement,
      params = params,
      ...,
      immediate = immediate
    )
    OdbcResultArrow(res)
  }
)

#' @rdname DBI-arrow
#' @export
setMethod(
  "dbGetQueryArrow",
  c("OdbcConnection", "character"),
  function(conn, statement, params = NULL, immediate = is.null(params), ...) {
    rs <- dbSendQueryArrow(
      conn,
      statement,
      params = params,
      immediate = immediate,
      ...
    )
    on.exit(dbClearResult(rs))

    dbFetchArrow(rs, ...)
  }
)

#' @rdname DBI-arrow
#' @export
setMethod(
  "dbFetchArrow",
  "OdbcResult",
  function(res, ..., chunk_size = 65536L) {
    call <- caller_env()
    chunk_size <- parse_size(chunk_size, call = call)
    result_fetch_arrow(res@ptr, chunk_size)
  }
)

#' @rdname DBI-arrow
#' @export
setMethod(
  "dbFetchArrow",
  "OdbcResultArrow",
  function(res, ..., chunk_size = 65536L) {
    dbFetchArrow(res@result, ..., chunk_size = chunk_size)
  }
)

#' @rdname DBI-arrow
#' @export
setMethod(
  "dbFetchArrowChunk",
  "OdbcResult",
  function(res, ..., chunk_size = 65536L) {
    call <- caller_env()
    chunk_size <- parse_size(chunk_size, call = call)
    result_fetch_arrow_chunk(res@ptr, chunk_size)
  }
)

#' @rdname DBI-arrow
#' @export
setMethod(
  "dbFetchArrowChunk",
  "OdbcResultArrow",
  function(res, ..., chunk_size = 65536L) {
    dbFetchArrowChunk(res@result, ..., chunk_size = chunk_size)
  }
)

#' @rdname DBI-arrow
#' @export
setMethod(
  "dbBindArrow",
  "OdbcResult",
  function(res, params, ..., batch_rows = getOption("odbc.batch_rows", NA)) {
    call <- caller_env()
    check_number_whole(batch_rows, min = 1, allow_na = TRUE, call = call)
    params <- nanoarrow::as_nanoarrow_array_stream(params)
    if (is.na(batch_rows)) {
      batch_rows <- 0
    }
    result_bind_arrow(
      res@ptr,
      params,
      batch_rows = batch_rows,
      use_transaction = FALSE
    )
    invisible(res)
  }
)

#' @rdname DBI-arrow
#' @export
setMethod(
  "dbBindArrow",
  "OdbcResultArrow",
  function(res, params, ..., batch_rows = getOption("odbc.batch_rows", NA)) {
    dbBindArrow(res@result, params, ..., batch_rows = batch_rows)
    invisible(res)
  }
)

#' @rdname DBI-arrow
#' @export
setMethod("dbReadTableArrow", "OdbcConnection", function(conn, name, ...) {
  call <- caller_env()
  name <- dbQuoteIdentifier(conn, name)
  if (length(name) != 1) {
    cli::cli_abort("{.arg name} must identify a single table.", call = call)
  }
  dbGetQueryArrow(conn, paste0("SELECT * FROM ", name), ...)
})

#' @rdname DBI-arrow
#' @export
setMethod(
  "dbCreateTableArrow",
  "OdbcConnection",
  function(conn, name, value, ..., field.types = NULL, temporary = FALSE) {
    call <- caller_env()
    check_bool(temporary, call = call)
    check_field.types(field.types, call = call)

    if (inherits(value, "nanoarrow_schema")) {
      schema <- value
    } else {
      schema <- nanoarrow::infer_nanoarrow_schema(value)
    }

    dbCreateTable(
      conn,
      name,
      fields = arrow_ptype(schema),
      ...,
      field.types = field.types,
      temporary = temporary
    )
  }
)

# A zero-row data frame with the R types corresponding to an Arrow schema,
# from which `dbDataType()` derives the column types of a new table.
arrow_ptype <- function(schema) {
  ptype <- nanoarrow::infer_nanoarrow_ptype(schema)
  for (i in seq_along(schema$children)) {
    if (schema$children[[i]]$format %in% c("l", "L")) {
      ptype[[i]] <- bit64::integer64()
    }
  }
  ptype
}

#' @rdname DBI-arrow
#' @export
setMethod(
  "dbAppendTableArrow",
  "OdbcConnection",
  function(
    conn,
    name,
    value,
    ...,
    batch_rows = getOption("odbc.batch_rows", NA)
  ) {
    call <- caller_env()
    check_number_whole(batch_rows, min = 1, allow_na = TRUE, call = call)
    value <- nanoarrow::as_nanoarrow_array_stream(value)

    schema <- nanoarrow::infer_nanoarrow_schema(value)
    fields <- vapply(schema$children, function(x) x$name, character(1))
    if (length(fields) == 0) {
      cli::cli_abort("{.arg value} must have at least one column.", call = call)
    }

    rs <- OdbcResult(conn, insert_statement(conn, name, fields))
    on.exit(dbClearResult(rs))
    describe_insert_parameters(conn, name, fields, rs)

    if (is.na(batch_rows)) {
      batch_rows <- 1024
    }
    rows <- result_bind_arrow(
      rs@ptr,
      value,
      batch_rows = batch_rows,
      use_transaction = TRUE
    )
    invisible(rows)
  }
)

#' @rdname DBI-arrow
#' @export
setMethod(
  "dbWriteTableArrow",
  "OdbcConnection",
  function(
    conn,
    name,
    value,
    append = FALSE,
    overwrite = FALSE,
    ...,
    temporary = FALSE,
    field.types = NULL,
    batch_rows = getOption("odbc.batch_rows", NA)
  ) {
    call <- caller_env()
    check_bool(overwrite, call = call)
    check_bool(append, call = call)
    check_bool(temporary, call = call)
    check_field.types(field.types, call = call)
    if (append && !is.null(field.types)) {
      cli::cli_abort(
        "Cannot specify {.arg field.types} with {.code append = TRUE}.",
        call = call
      )
    }
    if (overwrite && append) {
      cli::cli_abort(
        "{.arg overwrite} and {.arg append} cannot both be {.val TRUE}.",
        call = call
      )
    }

    value <- nanoarrow::as_nanoarrow_array_stream(value)

    found <- dbExistsTableForWrite(conn, name)
    if (found && !overwrite && !append) {
      cli::cli_abort(
        "Table {toString(name)} exists in database, and both overwrite and \\
         append are {.code FALSE}.",
        call = call
      )
    }
    if (found && overwrite) {
      dbRemoveTable(conn, name)
    }

    if (!found || overwrite) {
      dbCreateTableArrow(
        conn,
        name,
        value,
        field.types = field.types,
        temporary = temporary
      )
    }
    dbAppendTableArrow(conn, name, value, batch_rows = batch_rows)
    invisible(TRUE)
  }
)
