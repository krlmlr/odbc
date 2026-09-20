#' @include dbi-result.R
NULL

#' Odbc Arrow result methods
#'
#' `OdbcResultArrow` objects are returned by [DBI::dbSendQueryArrow()]. They
#' wrap an [OdbcResult] and implement the methods defined in the `DBI` package
#' for [DBI::DBIResultArrow-class] objects by delegating to it. See
#' [DBI-arrow] for the native Arrow interface of odbc.
#'
#' @param res,dbObj,object An `OdbcResultArrow` object.
#' @param ... Passed on to the corresponding [OdbcResult] method.
#' @name OdbcResultArrow
#' @docType methods
#' @keywords internal
NULL

#' @rdname OdbcResultArrow
#' @export
setClass(
  "OdbcResultArrow",
  contains = "DBIResultArrow",
  slots = list(result = "OdbcResult")
)

OdbcResultArrow <- function(result) {
  new("OdbcResultArrow", result = result)
}

#' @rdname OdbcResultArrow
#' @export
setMethod("show", "OdbcResultArrow", function(object) {
  cat("<OdbcResultArrow>\n")
  if (!dbIsValid(object)) {
    cat("  EXPIRED\n")
    return(invisible())
  }
  # Some drivers can't report the affected rows once the cursor is exhausted
  tryCatch(
    {
      cat("  SQL  ", dbGetStatement(object), "\n", sep = "")
      cat(
        "  ROWS Fetched: ",
        dbGetRowCount(object),
        " [",
        if (dbHasCompleted(object)) "complete" else "incomplete",
        "]\n",
        sep = ""
      )
      cat("       Changed: ", dbGetRowsAffected(object), "\n", sep = "")
    },
    error = function(e) NULL
  )
  invisible()
})

#' @rdname OdbcResultArrow
#' @export
setMethod("dbClearResult", "OdbcResultArrow", function(res, ...) {
  dbClearResult(res@result, ...)
})

#' @rdname OdbcResultArrow
#' @export
setMethod("dbIsValid", "OdbcResultArrow", function(dbObj, ...) {
  dbIsValid(dbObj@result, ...)
})

#' @rdname OdbcResultArrow
#' @export
setMethod("dbHasCompleted", "OdbcResultArrow", function(res, ...) {
  dbHasCompleted(res@result, ...)
})

#' @rdname OdbcResultArrow
#' @export
setMethod("dbGetStatement", "OdbcResultArrow", function(res, ...) {
  dbGetStatement(res@result, ...)
})

#' @rdname OdbcResultArrow
#' @export
setMethod("dbColumnInfo", "OdbcResultArrow", function(res, ...) {
  dbColumnInfo(res@result, ...)
})

#' @rdname OdbcResultArrow
#' @export
setMethod("dbGetRowCount", "OdbcResultArrow", function(res, ...) {
  dbGetRowCount(res@result, ...)
})

#' @rdname OdbcResultArrow
#' @export
setMethod("dbGetRowsAffected", "OdbcResultArrow", function(res, ...) {
  dbGetRowsAffected(res@result, ...)
})

#' @rdname OdbcResultArrow
#' @export
setMethod("dbGetInfo", "OdbcResultArrow", function(dbObj, ...) {
  dbGetInfo(dbObj@result, ...)
})

#' @rdname OdbcResultArrow
#' @param params Query parameters, see [DBI::dbBind()].
#' @export
setMethod("dbBind", "OdbcResultArrow", function(res, params, ...) {
  dbBind(res@result, params, ...)
  invisible(res)
})
