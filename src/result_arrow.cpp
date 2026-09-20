// R entry points for the native Arrow interface of odbc_result.
//
// Arrow structures are exchanged with R as `nanoarrow_schema`,
// `nanoarrow_array` and `nanoarrow_array_stream` external pointers, using
// the conventions of the nanoarrow package (see nanoarrow/r.h).

#include "odbc_result.h"
#include "odbc_types.h"

// Must be included after nanoarrow.h (via odbc_result.h) so that the Arrow
// C data interface structs are defined only once.
#include "nanoarrow/r.h"

#include <cstring>
#include <vector>

using namespace Rcpp;

namespace {

void check_arrow(int code, const char* context) {
  if (code != NANOARROW_OK) {
    Rcpp::stop("%s: %s", context, std::strerror(code));
  }
}

// Validate an input array stream with an R error rather than Rf_error(),
// which would jump over the C++ frames of the calling function.
struct ArrowArrayStream* input_array_stream(SEXP stream) {
  if (!Rf_inherits(stream, "nanoarrow_array_stream")) {
    Rcpp::stop("`params` must be a nanoarrow_array_stream.");
  }
  struct ArrowArrayStream* out =
      static_cast<struct ArrowArrayStream*>(R_ExternalPtrAddr(stream));
  if (out == nullptr || out->release == nullptr) {
    Rcpp::stop("The nanoarrow_array_stream has already been released.");
  }
  return out;
}

} // namespace

// Arrow schema of the result set, as a nanoarrow_schema.
// [[Rcpp::export]]
SEXP result_arrow_schema(result_ptr const& r) {
  Rcpp::RObject schema_xptr(nanoarrow_schema_owning_xptr());
  r->arrow_schema(nanoarrow_output_schema_from_xptr(schema_xptr));
  return schema_xptr;
}

// Fetch all remaining rows into a nanoarrow_array_stream made of arrays of
// at most `chunk_size` rows each.
// [[Rcpp::export]]
SEXP result_fetch_arrow(result_ptr const& r, double chunk_size) {
  nanoarrow::UniqueSchema schema;
  r->arrow_schema(schema.get());

  std::vector<nanoarrow::UniqueArray> arrays;
  while (!r->complete()) {
    nanoarrow::UniqueArray array;
    int64_t rows = r->fetch_arrow(array.get(), static_cast<int64_t>(chunk_size));
    if (rows == 0) {
      break;
    }
    arrays.push_back(std::move(array));
  }

  Rcpp::RObject stream_xptr(nanoarrow_array_stream_owning_xptr());
  struct ArrowArrayStream* stream =
      nanoarrow_output_array_stream_from_xptr(stream_xptr);
  check_arrow(
      ArrowBasicArrayStreamInit(
          stream, schema.get(), static_cast<int64_t>(arrays.size())),
      "Can't allocate Arrow array stream");
  for (size_t i = 0; i < arrays.size(); ++i) {
    ArrowBasicArrayStreamSetArray(stream, static_cast<int64_t>(i), arrays[i].get());
  }
  return stream_xptr;
}

// Fetch the next chunk of at most `chunk_size` rows as a nanoarrow_array.
// [[Rcpp::export]]
SEXP result_fetch_arrow_chunk(result_ptr const& r, double chunk_size) {
  Rcpp::RObject array_xptr(nanoarrow_array_owning_xptr());
  r->fetch_arrow(
      nanoarrow_output_array_from_xptr(array_xptr),
      static_cast<int64_t>(chunk_size));

  // The schema of a nanoarrow_array lives in the tag of the external pointer.
  Rcpp::RObject schema_xptr(nanoarrow_schema_owning_xptr());
  r->arrow_schema(nanoarrow_output_schema_from_xptr(schema_xptr));
  R_SetExternalPtrTag(array_xptr, schema_xptr);
  return array_xptr;
}

// Bind the arrays of a nanoarrow_array_stream to the parameters of the
// prepared statement, and execute it.  The stream is consumed and released.
// Returns the number of rows bound.
// [[Rcpp::export]]
double result_bind_arrow(
    result_ptr const& r, SEXP params, double batch_rows, bool use_transaction) {
  struct ArrowArrayStream* stream = input_array_stream(params);
  int64_t rows =
      r->bind_arrow(stream, use_transaction, static_cast<int64_t>(batch_rows));
  ArrowArrayStreamRelease(stream);
  return static_cast<double>(rows);
}
