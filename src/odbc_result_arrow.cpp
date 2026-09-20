// Native Arrow support: fetching result sets into Arrow arrays and binding
// Arrow arrays to statement parameters, without going through R vectors.

#include "odbc_result.h"
#include "cctz/civil_time.h"
#include "cctz/time_zone.h"
#include "utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

#ifndef SQL_SS_TIMESTAMPOFFSET
#define SQL_SS_TIMESTAMPOFFSET (-155)
#endif
#ifndef SQL_SS_TIME2
#define SQL_SS_TIME2 (-154)
#endif
#ifndef SQL_DB2_XML
#define SQL_DB2_XML (-370)
#endif

namespace odbc {

using odbc::utils::raise_warning;

namespace {

// A chunk is cut once the data buffer of a string or binary column reaches
// this size, keeping offsets well within the 32-bit range of the `utf8` and
// `binary` Arrow types.
const int64_t arrow_max_var_bytes = 1LL << 30;

void check_arrow(
    int code, const char* context, const struct ArrowError* error = nullptr) {
  if (code == NANOARROW_OK) {
    return;
  }
  if (error != nullptr && error->message[0] != '\0') {
    Rcpp::stop("%s: %s", context, error->message);
  }
  Rcpp::stop("%s: %s", context, std::strerror(code));
}

int64_t units_per_second(enum ArrowTimeUnit unit) {
  switch (unit) {
  case NANOARROW_TIME_UNIT_SECOND:
    return 1;
  case NANOARROW_TIME_UNIT_MILLI:
    return 1000;
  case NANOARROW_TIME_UNIT_MICRO:
    return 1000000;
  case NANOARROW_TIME_UNIT_NANO:
    return 1000000000;
  }
  return 1;
}

int64_t floor_div(int64_t value, int64_t divisor) {
  int64_t quotient = value / divisor;
  if ((value % divisor != 0) && ((value < 0) != (divisor < 0))) {
    --quotient;
  }
  return quotient;
}

std::string arrow_type_name(const struct ArrowSchema* schema) {
  char buffer[256];
  ArrowSchemaToString(schema, buffer, sizeof(buffer), 0);
  return std::string(buffer);
}

bool is_variable_width(odbc_result::arrow_kind kind) {
  return kind == odbc_result::arrow_kind::string ||
         kind == odbc_result::arrow_kind::ustring ||
         kind == odbc_result::arrow_kind::binary;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// "[-]H:MM:SS[.fraction]" (hours may have more than two digits) to
// nanoseconds since midnight
bool parse_time(const std::string& text, int64_t& out) {
  const char* p = text.c_str();
  bool negative = false;
  if (*p == '-') {
    negative = true;
    ++p;
  } else if (*p == '+') {
    ++p;
  }

  int64_t parts[3] = {0, 0, 0};
  for (int k = 0; k < 3; ++k) {
    if (!is_digit(*p)) {
      return false;
    }
    while (is_digit(*p)) {
      parts[k] = parts[k] * 10 + (*p++ - '0');
    }
    if (k < 2) {
      if (*p != ':') {
        return false;
      }
      ++p;
    }
  }

  int64_t fraction = 0;
  int digits = 0;
  if (*p == '.') {
    ++p;
    while (is_digit(*p)) {
      if (digits < 9) {
        fraction = fraction * 10 + (*p - '0');
        ++digits;
      }
      ++p;
    }
  }
  while (*p == ' ') {
    ++p;
  }
  if (*p != '\0') {
    return false;
  }
  for (; digits < 9; ++digits) {
    fraction *= 10;
  }

  out = (parts[0] * 3600 + parts[1] * 60 + parts[2]) * 1000000000LL + fraction;
  if (negative) {
    out = -out;
  }
  return true;
}

// "[-]digits[.digits]" to the unscaled digits of a decimal with `scale`
// fractional digits; `truncated` reports dropped non-zero digits.
bool parse_decimal(
    const std::string& text, int scale, std::string& digits, bool& truncated) {
  const char* p = text.c_str();
  digits.clear();
  truncated = false;
  if (*p == '-') {
    digits.push_back('-');
    ++p;
  } else if (*p == '+') {
    ++p;
  }

  bool any = false;
  while (is_digit(*p)) {
    digits.push_back(*p++);
    any = true;
  }
  int fractional = 0;
  if (*p == '.') {
    ++p;
    while (is_digit(*p)) {
      if (fractional < scale) {
        digits.push_back(*p);
        ++fractional;
      } else if (*p != '0') {
        truncated = true;
      }
      ++p;
      any = true;
    }
  }
  while (*p == ' ') {
    ++p;
  }
  if (!any || *p != '\0') {
    return false;
  }
  for (; fractional < scale; ++fractional) {
    digits.push_back('0');
  }
  if (digits.empty() || digits == "-") {
    digits.push_back('0');
  }
  return true;
}

// Seconds since midnight in `per_second` units as "[-]HH:MM:SS[.fraction]",
// with up to `digits` fractional digits and without trailing zeros
std::string format_time(int64_t value, int64_t per_second, int digits) {
  const bool negative = value < 0;
  if (negative) {
    value = -value;
  }
  const long long seconds = static_cast<long long>(value / per_second);
  const long long fraction = static_cast<long long>(value % per_second);
  char buffer[64];
  int n = std::snprintf(
      buffer,
      sizeof(buffer),
      "%s%02lld:%02lld:%02lld",
      negative ? "-" : "",
      seconds / 3600,
      (seconds / 60) % 60,
      seconds % 60);
  if (fraction > 0) {
    n += std::snprintf(buffer + n, sizeof(buffer) - n, ".%0*lld", digits, fraction);
    while (buffer[n - 1] == '0') {
      buffer[--n] = '\0';
    }
  }
  return std::string(buffer, n);
}

// Strings are truncated at an embedded null, consistent with the data frame
// path (R strings can't contain nulls).
int64_t length_to_null(const std::string& x) {
  size_t n = 0;
  while (n < x.length() && x[n] != '\0') {
    ++n;
  }
  return static_cast<int64_t>(n);
}

int64_t power_of_ten(int exponent) {
  int64_t out = 1;
  for (int i = 0; i < exponent; ++i) {
    out *= 10;
  }
  return out;
}

} // namespace

// Schema -----------------------------------------------------------------

void odbc_result::reset_arrow_schema() {
  arrow_schema_.reset();
  arrow_columns_.clear();
  arrow_schema_ready_ = false;
}

// Columns are mapped to the Arrow type that keeps the values of the
// database type intact:
//
// * SQL_BIT -> bool
// * SQL_TINYINT, SQL_SMALLINT, SQL_INTEGER -> int32
// * SQL_BIGINT -> int64
// * SQL_REAL, SQL_FLOAT, SQL_DOUBLE -> double
// * SQL_DECIMAL, SQL_NUMERIC -> decimal128 (decimal256 beyond 38 digits) with
//   the precision and scale reported by the driver, or double when the driver
//   doesn't report a usable precision
// * SQL_DATE -> date32
// * SQL_TIME -> time64[us], or time64[ns] when the driver reports more than
//   six fractional digits (SQL Server time(7)), keeping fractional seconds
// * SQL_TIMESTAMP -> timestamp[us], with the `timezone_out` time zone
// * character types -> utf8
// * binary types -> binary
// * anything else -> utf8
void odbc_result::ensure_arrow_schema() {
  if (arrow_schema_ready_) {
    return;
  }

  nanoarrow::UniqueSchema schema;
  ArrowSchemaInit(schema.get());
  check_arrow(
      ArrowSchemaSetTypeStruct(schema.get(), num_columns_),
      "Can't allocate Arrow schema");

  std::vector<arrow_column> columns;
  std::vector<std::string> names;
  if (num_columns_ > 0) {
    names = column_names(*r_);
  }
  const std::string timezone = c_->timezone_out_str();

  for (short i = 0; i < num_columns_; ++i) {
    nanodbc::result& r = *r_;
    const short type = r.column_datatype(i);
    struct ArrowSchema* child = schema->children[i];
    arrow_column info;
    info.kind = arrow_kind::string;
    info.precision = 0;
    info.scale = 0;
    info.scale_warned = false;
    int code = NANOARROW_OK;

    switch (type) {
    case SQL_BIT:
      info.kind = arrow_kind::boolean;
      code = ArrowSchemaSetType(child, NANOARROW_TYPE_BOOL);
      break;
    case SQL_TINYINT:
    case SQL_SMALLINT:
    case SQL_INTEGER:
      info.kind = arrow_kind::int32;
      code = ArrowSchemaSetType(child, NANOARROW_TYPE_INT32);
      break;
    case SQL_BIGINT:
      info.kind = arrow_kind::int64;
      code = ArrowSchemaSetType(child, NANOARROW_TYPE_INT64);
      break;
    case SQL_DOUBLE:
    case SQL_FLOAT:
    case SQL_REAL:
      info.kind = arrow_kind::float64;
      code = ArrowSchemaSetType(child, NANOARROW_TYPE_DOUBLE);
      break;
    case SQL_DECIMAL:
    case SQL_NUMERIC: {
      const long precision = r.column_size(i);
      const int scale = r.column_decimal_digits(i);
      if (precision >= 1 && precision <= 76 && scale >= 0 && scale <= precision) {
        info.kind = arrow_kind::decimal;
        info.precision = static_cast<int>(precision);
        info.scale = scale;
        code = ArrowSchemaSetTypeDecimal(
            child,
            precision <= 38 ? NANOARROW_TYPE_DECIMAL128 : NANOARROW_TYPE_DECIMAL256,
            info.precision,
            info.scale);
      } else {
        info.kind = arrow_kind::float64;
        code = ArrowSchemaSetType(child, NANOARROW_TYPE_DOUBLE);
      }
      break;
    }
    case SQL_DATE:
    case SQL_TYPE_DATE:
      info.kind = arrow_kind::date;
      code = ArrowSchemaSetType(child, NANOARROW_TYPE_DATE32);
      break;
    case SQL_TIME:
    case SQL_TYPE_TIME:
    case SQL_SS_TIME2:
      info.kind = arrow_kind::time;
      // Microseconds convert to R exactly; nanoseconds are only used for
      // drivers reporting more than six fractional digits (SQL Server)
      info.scale = r.column_decimal_digits(i) > 6 ? 9 : 6;
      code = ArrowSchemaSetTypeDateTime(
          child,
          NANOARROW_TYPE_TIME64,
          info.scale == 9 ? NANOARROW_TIME_UNIT_NANO : NANOARROW_TIME_UNIT_MICRO,
          nullptr);
      break;
    case SQL_TIMESTAMP:
    case SQL_TYPE_TIMESTAMP:
    case SQL_SS_TIMESTAMPOFFSET:
      info.kind = arrow_kind::timestamp;
      code = ArrowSchemaSetTypeDateTime(
          child,
          NANOARROW_TYPE_TIMESTAMP,
          NANOARROW_TIME_UNIT_MICRO,
          timezone.c_str());
      break;
    case SQL_CHAR:
    case SQL_VARCHAR:
    case SQL_LONGVARCHAR:
      info.kind = arrow_kind::string;
      code = ArrowSchemaSetType(child, NANOARROW_TYPE_STRING);
      break;
    case SQL_WCHAR:
    case SQL_WVARCHAR:
    case SQL_WLONGVARCHAR:
      info.kind = arrow_kind::ustring;
      code = ArrowSchemaSetType(child, NANOARROW_TYPE_STRING);
      break;
    case SQL_BINARY:
    case SQL_VARBINARY:
    case SQL_LONGVARBINARY:
    case SQL_DB2_XML:
      info.kind = arrow_kind::binary;
      code = ArrowSchemaSetType(child, NANOARROW_TYPE_BINARY);
      break;
    default:
      info.kind = arrow_kind::string;
      code = ArrowSchemaSetType(child, NANOARROW_TYPE_STRING);
      signal_unknown_field_type(type, r.column_name(i));
      break;
    }
    check_arrow(code, "Can't set Arrow column type");
    check_arrow(
        ArrowSchemaSetName(child, names[i].c_str()),
        "Can't set Arrow column name");
    columns.push_back(info);
  }

  arrow_schema_ = std::move(schema);
  arrow_columns_ = columns;
  arrow_schema_ready_ = true;
}

void odbc_result::arrow_schema(struct ArrowSchema* out) {
  if (!bound_) {
    Rcpp::stop("Query needs to be bound before fetching");
  }
  ensure_arrow_schema();
  check_arrow(
      ArrowSchemaDeepCopy(arrow_schema_.get(), out), "Can't copy Arrow schema");
}

// Fetch ------------------------------------------------------------------

int64_t odbc_result::fetch_arrow(struct ArrowArray* out, int64_t n_max) {
  if (!bound_) {
    Rcpp::stop("Query needs to be bound before fetching");
  }
  ensure_arrow_schema();

  struct ArrowError error;
  ArrowErrorInit(&error);

  nanoarrow::UniqueArray array;
  check_arrow(
      ArrowArrayInitFromSchema(array.get(), arrow_schema_.get(), &error),
      "Can't allocate Arrow array",
      &error);
  check_arrow(ArrowArrayStartAppending(array.get()), "Can't allocate Arrow array");

  int64_t rows = 0;
  if (num_columns_ > 0) {
    unbind_arrow_columns();
    unbind_if_needed();
    try {
      rows = fetch_arrow_rows(*array.get(), n_max);
    } catch (...) {
      c_->set_current_result(nullptr);
      throw;
    }
  }

  check_arrow(
      ArrowArrayFinishBuildingDefault(array.get(), &error),
      "Can't finalize Arrow array",
      &error);
  array.move(out);
  return rows;
}

// Time and decimal columns are retrieved through the driver's character
// conversion (see get_arrow_string()), which requires them to be unbound.
void odbc_result::unbind_arrow_columns() {
  try {
    for (short i = 0; i < num_columns_; ++i) {
      const arrow_kind kind = arrow_columns_[i].kind;
      if ((kind == arrow_kind::time || kind == arrow_kind::decimal) &&
          r_->is_bound(i)) {
        r_->unbind(i);
      }
    }
  } catch (const nanodbc::database_error& e) {
    raise_warning("Was unable to unbind some nanodbc buffers");
  }
}

int64_t odbc_result::fetch_arrow_rows(struct ArrowArray& out, int64_t n_max) {
  nanodbc::result& r = *r_;
  const size_t ncols = arrow_columns_.size();

  std::vector<size_t> var_cols;
  for (size_t col = 0; col < ncols; ++col) {
    if (is_variable_width(arrow_columns_[col].kind)) {
      var_cols.push_back(col);
    }
  }

  int64_t row = 0;
  if (rows_fetched_ == 0 && n_max != 0) {
    complete_ = !r.next() && !nextResultSet(r);
  }

  while (!complete_ && (n_max < 0 || row < n_max)) {
    for (size_t col = 0; col < ncols; ++col) {
      append_arrow_value(
          out.children[col], arrow_columns_[col], static_cast<short>(col), r);
    }
    check_arrow(ArrowArrayFinishElement(&out), "Can't append row to Arrow array");

    complete_ = !r.next();
    ++row;
    ++rows_fetched_;
    if (rows_fetched_ % 16384 == 0) {
      Rcpp::checkUserInterrupt();
    }
    if (complete_ && nextResultSet(r)) {
      // A new result set comes with new bindings
      complete_ = false;
      unbind_arrow_columns();
      unbind_if_needed();
    }

    bool cut = false;
    for (size_t col : var_cols) {
      if (ArrowArrayBuffer(out.children[col], 2)->size_bytes >=
          arrow_max_var_bytes) {
        cut = true;
        break;
      }
    }
    if (cut) {
      break;
    }
  }

  return row;
}

// Retrieves a column of the current row in the driver's character
// representation, which keeps the fractional seconds of times and all the
// digits of decimals that the buffers bound by nanodbc can't hold.
// The column must be unbound, see unbind_arrow_columns().
bool odbc_result::get_arrow_string(short column, std::string& out) {
  SQLHSTMT handle = static_cast<SQLHSTMT>(r_->native_statement_handle());
  char buffer[256];
  out.clear();

  while (true) {
    SQLLEN indicator = 0;
    SQLRETURN rc = SQLGetData(
        handle, column + 1, SQL_C_CHAR, buffer, sizeof(buffer), &indicator);
    if (rc == SQL_NO_DATA) {
      break;
    }
    if (!SQL_SUCCEEDED(rc)) {
      throw nanodbc::database_error(handle, SQL_HANDLE_STMT, "SQLGetData");
    }
    if (indicator == SQL_NULL_DATA) {
      return false;
    }
    size_t n = 0;
    while (n < sizeof(buffer) && buffer[n] != '\0') {
      ++n;
    }
    out.append(buffer, n);
    if (rc == SQL_SUCCESS) {
      // Otherwise SQL_SUCCESS_WITH_INFO: more data is available
      break;
    }
  }
  return true;
}

// Nullity is checked both before and after retrieving the value, for the
// same reason as in the data frame path: for unbound columns the null
// indicator is only set once the data has been retrieved.
void odbc_result::append_arrow_value(
    struct ArrowArray* child,
    arrow_column& info,
    short column,
    nanodbc::result& value) {
  if (info.kind == arrow_kind::time || info.kind == arrow_kind::decimal) {
    std::string text;
    if (!get_arrow_string(column, text)) {
      check_arrow(ArrowArrayAppendNull(child, 1), "Can't append to Arrow array");
    } else if (info.kind == arrow_kind::time) {
      append_arrow_time(child, info, column, text);
    } else {
      append_arrow_decimal(child, info, column, text);
    }
    return;
  }

  if (value.is_null(column)) {
    check_arrow(ArrowArrayAppendNull(child, 1), "Can't append to Arrow array");
    return;
  }

  int code = NANOARROW_OK;
  switch (info.kind) {
  case arrow_kind::boolean: {
    int v = value.get<int>(column, 0);
    code = value.is_null(column) ? ArrowArrayAppendNull(child, 1)
                                 : ArrowArrayAppendInt(child, v != 0);
    break;
  }
  case arrow_kind::int32: {
    int v = value.get<int>(column, 0);
    code = value.is_null(column) ? ArrowArrayAppendNull(child, 1)
                                 : ArrowArrayAppendInt(child, v);
    break;
  }
  case arrow_kind::int64: {
    int64_t v = value.get<int64_t>(column, 0);
    code = value.is_null(column) ? ArrowArrayAppendNull(child, 1)
                                 : ArrowArrayAppendInt(child, v);
    break;
  }
  case arrow_kind::float64: {
    double v = value.get<double>(column, 0.0);
    code = value.is_null(column) ? ArrowArrayAppendNull(child, 1)
                                 : ArrowArrayAppendDouble(child, v);
    break;
  }
  case arrow_kind::date: {
    nanodbc::date v = value.get<nanodbc::date>(column);
    if (value.is_null(column)) {
      code = ArrowArrayAppendNull(child, 1);
    } else {
      int64_t days =
          cctz::civil_day(v.year, v.month, v.day) - cctz::civil_day(1970, 1, 1);
      code = ArrowArrayAppendInt(child, days);
    }
    break;
  }
  case arrow_kind::timestamp: {
    nanodbc::timestampoffset v = value.get<nanodbc::timestampoffset>(column);
    code = value.is_null(column) ? ArrowArrayAppendNull(child, 1)
                                 : ArrowArrayAppendInt(child, as_micros(v));
    break;
  }
  case arrow_kind::string:
  case arrow_kind::ustring: {
    std::string v = value.get<std::string>(column);
    if (value.is_null(column)) {
      code = ArrowArrayAppendNull(child, 1);
      break;
    }
    // Strings may be in the server's internal code page, so we need to
    // re-encode in UTF-8 if necessary.  Unicode strings are converted to
    // UTF-8 by nanodbc already.
    if (info.kind == arrow_kind::string) {
      v = output_encoder_->makeString(v.c_str(), v.c_str() + v.length());
    }
    struct ArrowStringView view;
    view.data = v.data();
    view.size_bytes = length_to_null(v);
    code = ArrowArrayAppendString(child, view);
    break;
  }
  case arrow_kind::binary: {
    std::vector<std::uint8_t> v = value.get<std::vector<std::uint8_t>>(column);
    if (value.is_null(column)) {
      code = ArrowArrayAppendNull(child, 1);
      break;
    }
    struct ArrowBufferView view;
    view.data.as_uint8 = v.data();
    view.size_bytes = static_cast<int64_t>(v.size());
    code = ArrowArrayAppendBytes(child, view);
    break;
  }
  default:
    code = ArrowArrayAppendNull(child, 1);
    break;
  }
  check_arrow(code, "Can't append to Arrow array");
}

// Parses "[-]H:MM:SS[.fraction]" into the unit of the column.
void odbc_result::append_arrow_time(
    struct ArrowArray* child,
    arrow_column& info,
    short column,
    const std::string& text) {
  int64_t nanoseconds = 0;
  if (!parse_time(text, nanoseconds)) {
    Rcpp::stop(
        "Can't parse time value '%s' in column %i.", text, column + 1);
  }
  const int64_t value = info.scale == 9 ? nanoseconds : nanoseconds / 1000;
  check_arrow(ArrowArrayAppendInt(child, value), "Can't append to Arrow array");
}

// Parses "[-]digits[.digits]" into a decimal with the scale of the column.
void odbc_result::append_arrow_decimal(
    struct ArrowArray* child,
    arrow_column& info,
    short column,
    const std::string& text) {
  std::string digits;
  bool truncated = false;
  if (!parse_decimal(text, info.scale, digits, truncated)) {
    Rcpp::stop(
        "Can't parse decimal value '%s' in column %i.", text, column + 1);
  }
  if (truncated && !info.scale_warned) {
    info.scale_warned = true;
    raise_warning(
        "Decimal values in column " + std::to_string(column + 1) +
        " were truncated to " + std::to_string(info.scale) +
        " fractional digits, the scale reported by the driver.");
  }

  const size_t first = digits.find_first_not_of("-0");
  const size_t significant = first == std::string::npos ? 0 : digits.size() - first;
  if (significant > static_cast<size_t>(info.precision)) {
    Rcpp::stop(
        "Decimal value '%s' in column %i has more than the %i digits reported by the driver.",
        text,
        column + 1,
        info.precision);
  }

  struct ArrowDecimal decimal;
  ArrowDecimalInit(
      &decimal, info.precision <= 38 ? 128 : 256, info.precision, info.scale);
  struct ArrowStringView view;
  view.data = digits.data();
  view.size_bytes = static_cast<int64_t>(digits.size());
  check_arrow(ArrowDecimalSetDigits(&decimal, view), "Can't convert decimal value");
  check_arrow(ArrowArrayAppendDecimal(child, &decimal), "Can't append to Arrow array");
}

// Bind -------------------------------------------------------------------

int64_t odbc_result::bind_arrow(
    struct ArrowArrayStream* stream, bool use_transaction, int64_t batch_rows) {
  complete_ = false;
  rows_fetched_ = 0;
  reset_arrow_schema();

  if (s_->parameters() == 0) {
    Rcpp::stop("Query does not require parameters.");
  }

  struct ArrowError error;
  ArrowErrorInit(&error);

  nanoarrow::UniqueSchema schema;
  check_arrow(
      ArrowArrayStreamGetSchema(stream, schema.get(), &error),
      "Can't read Arrow schema",
      &error);
  if (schema->format == nullptr || std::strcmp(schema->format, "+s") != 0) {
    Rcpp::stop(
        "Parameters must be a struct array with one child per parameter, not '%s'.",
        arrow_type_name(schema.get()));
  }

  const int64_t ncols = schema->n_children;
  if (ncols != s_->parameters()) {
    Rcpp::stop(
        "Query requires '%i' params; '%i' supplied.",
        s_->parameters(),
        static_cast<int>(ncols));
  }

  std::vector<struct ArrowSchemaView> schema_views(ncols);
  for (int64_t col = 0; col < ncols; ++col) {
    check_arrow(
        ArrowSchemaViewInit(&schema_views[col], schema->children[col], &error),
        "Can't parse Arrow schema",
        &error);
  }

  nanoarrow::UniqueArrayView view;
  check_arrow(
      ArrowArrayViewInitFromSchema(view.get(), schema.get(), &error),
      "Can't parse Arrow schema",
      &error);

  std::unique_ptr<nanodbc::transaction> t;
  if (use_transaction && c_->supports_transactions()) {
    t = std::unique_ptr<nanodbc::transaction>(
        new nanodbc::transaction(*c_->connection()));
  }

  int64_t total = 0;
  while (true) {
    nanoarrow::UniqueArray array;
    check_arrow(
        ArrowArrayStreamGetNext(stream, array.get(), &error),
        "Can't read Arrow array",
        &error);
    if (array->release == nullptr) {
      // End of stream
      break;
    }
    check_arrow(
        ArrowArrayViewSetArray(view.get(), array.get(), &error),
        "Invalid Arrow array",
        &error);
    check_arrow(
        ArrowArrayViewValidate(
            view.get(), NANOARROW_VALIDATION_LEVEL_DEFAULT, &error),
        "Invalid Arrow array",
        &error);

    const int64_t nrows = array->length;
    const int64_t step = batch_rows > 0 ? batch_rows : std::max<int64_t>(nrows, 1);
    for (int64_t start = 0; start < nrows; start += step) {
      const int64_t size = std::min(step, nrows - start);
      clear_buffers();
      for (int64_t col = 0; col < ncols; ++col) {
        bind_arrow_column(
            view->children[col],
            schema_views[col],
            static_cast<short>(col),
            view->offset + start,
            size);
      }
      r_ = std::make_shared<nanodbc::result>(nanodbc::execute(*s_, size));
      num_columns_ = r_->columns();
      total += size;

      Rcpp::checkUserInterrupt();
    }
  }

  if (t) {
    t->commit();
  }
  bound_ = true;
  return total;
}

void odbc_result::bind_arrow_column(
    const struct ArrowArrayView* view,
    const struct ArrowSchemaView& schema_view,
    short column,
    int64_t first,
    int64_t size) {
  std::vector<uint8_t>& nulls = buffers_.nulls_[column];
  nulls.assign(size, 0);
  for (int64_t i = 0; i < size; ++i) {
    nulls[i] = ArrowArrayViewIsNull(view, first + i);
  }
  bool* nulls_ptr = reinterpret_cast<bool*>(nulls.data());

  switch (schema_view.type) {
  case NANOARROW_TYPE_NA:
    s_->bind_null(column, size);
    return;

  case NANOARROW_TYPE_BOOL:
  case NANOARROW_TYPE_INT8:
  case NANOARROW_TYPE_UINT8:
  case NANOARROW_TYPE_INT16:
  case NANOARROW_TYPE_UINT16:
  case NANOARROW_TYPE_INT32: {
    std::vector<int>& values = buffers_.ints_[column];
    values.assign(size, 0);
    for (int64_t i = 0; i < size; ++i) {
      if (!nulls[i]) {
        values[i] = static_cast<int>(ArrowArrayViewGetIntUnsafe(view, first + i));
      }
    }
    s_->bind(column, values.data(), size, nulls_ptr);
    return;
  }

  case NANOARROW_TYPE_UINT32:
  case NANOARROW_TYPE_INT64: {
    std::vector<int64_t>& values = buffers_.int64s_[column];
    values.assign(size, 0);
    for (int64_t i = 0; i < size; ++i) {
      if (!nulls[i]) {
        values[i] = ArrowArrayViewGetIntUnsafe(view, first + i);
      }
    }
    s_->bind(column, values.data(), size, nulls_ptr);
    return;
  }

  case NANOARROW_TYPE_UINT64: {
    std::vector<uint64_t>& values = buffers_.uint64s_[column];
    values.assign(size, 0);
    for (int64_t i = 0; i < size; ++i) {
      if (!nulls[i]) {
        values[i] = ArrowArrayViewGetUIntUnsafe(view, first + i);
      }
    }
    s_->bind(column, values.data(), size, nulls_ptr);
    return;
  }

  case NANOARROW_TYPE_HALF_FLOAT:
  case NANOARROW_TYPE_FLOAT:
  case NANOARROW_TYPE_DOUBLE: {
    std::vector<double>& values = buffers_.doubles_[column];
    values.assign(size, 0.0);
    for (int64_t i = 0; i < size; ++i) {
      if (!nulls[i]) {
        values[i] = ArrowArrayViewGetDoubleUnsafe(view, first + i);
      }
    }
    s_->bind(column, values.data(), size, nulls_ptr);
    return;
  }

  case NANOARROW_TYPE_DECIMAL32:
  case NANOARROW_TYPE_DECIMAL64:
  case NANOARROW_TYPE_DECIMAL128:
  case NANOARROW_TYPE_DECIMAL256: {
    // Decimals are bound as their exact decimal representation.
    std::vector<std::string>& values = buffers_.strings_[column];
    values.assign(size, "");
    struct ArrowDecimal decimal;
    ArrowDecimalInit(
        &decimal,
        schema_view.decimal_bitwidth,
        schema_view.decimal_precision,
        schema_view.decimal_scale);
    nanoarrow::UniqueBuffer buffer;
    for (int64_t i = 0; i < size; ++i) {
      if (nulls[i]) {
        continue;
      }
      ArrowArrayViewGetDecimalUnsafe(view, first + i, &decimal);
      buffer->size_bytes = 0;
      check_arrow(
          ArrowDecimalAppendStringToBuffer(&decimal, buffer.get()),
          "Can't format Arrow decimal");
      values[i].assign(
          reinterpret_cast<const char*>(buffer->data), buffer->size_bytes);
    }
    s_->bind_strings(column, values, nulls_ptr);
    return;
  }

  case NANOARROW_TYPE_STRING:
  case NANOARROW_TYPE_LARGE_STRING:
  case NANOARROW_TYPE_STRING_VIEW:
  case NANOARROW_TYPE_DICTIONARY:
    bind_arrow_strings(view, schema_view, column, first, size);
    return;

  case NANOARROW_TYPE_BINARY:
  case NANOARROW_TYPE_LARGE_BINARY:
  case NANOARROW_TYPE_FIXED_SIZE_BINARY:
  case NANOARROW_TYPE_BINARY_VIEW: {
    std::vector<std::vector<uint8_t>>& values = buffers_.raws_[column];
    values.assign(size, std::vector<uint8_t>());
    for (int64_t i = 0; i < size; ++i) {
      if (nulls[i]) {
        continue;
      }
      struct ArrowBufferView bytes = ArrowArrayViewGetBytesUnsafe(view, first + i);
      values[i].assign(bytes.data.as_uint8, bytes.data.as_uint8 + bytes.size_bytes);
    }
    s_->bind(column, values, nulls_ptr);
    return;
  }

  case NANOARROW_TYPE_DATE32:
  case NANOARROW_TYPE_DATE64: {
    std::vector<nanodbc::date>& values = buffers_.dates_[column];
    values.assign(size, nanodbc::date());
    for (int64_t i = 0; i < size; ++i) {
      if (nulls[i]) {
        continue;
      }
      int64_t v = ArrowArrayViewGetIntUnsafe(view, first + i);
      int64_t days = schema_view.type == NANOARROW_TYPE_DATE32
                         ? v
                         : floor_div(v, 24LL * 60 * 60 * 1000);
      cctz::civil_day day = cctz::civil_day(1970, 1, 1) + days;
      values[i].year = day.year();
      values[i].month = day.month();
      values[i].day = day.day();
    }
    s_->bind(column, values.data(), size, nulls_ptr);
    return;
  }

  case NANOARROW_TYPE_TIME32:
  case NANOARROW_TYPE_TIME64:
  case NANOARROW_TYPE_DURATION: {
    // Durations are bound like times of day, as R's `difftime` (and
    // `hms`) values are.
    const int64_t per_second = units_per_second(schema_view.time_unit);
    if (per_second == 1) {
      std::vector<nanodbc::time>& values = buffers_.times_[column];
      values.assign(size, nanodbc::time());
      for (int64_t i = 0; i < size; ++i) {
        if (!nulls[i]) {
          values[i] = as_time(
              static_cast<double>(ArrowArrayViewGetIntUnsafe(view, first + i)));
        }
      }
      s_->bind(column, values.data(), size, nulls_ptr);
      return;
    }
    // Sub-second units are bound as text, which keeps fractional seconds
    const int digits = per_second == 1000 ? 3 : per_second == 1000000 ? 6 : 9;
    std::vector<std::string>& values = buffers_.strings_[column];
    values.assign(size, "");
    for (int64_t i = 0; i < size; ++i) {
      if (!nulls[i]) {
        values[i] = format_time(
            ArrowArrayViewGetIntUnsafe(view, first + i), per_second, digits);
      }
    }
    s_->bind_strings(column, values, nulls_ptr);
    return;
  }

  case NANOARROW_TYPE_TIMESTAMP:
    bind_arrow_timestamp(view, schema_view, column, first, size);
    return;

  default:
    break;
  }

  Rcpp::stop(
      "Can't bind Arrow type '%s' (parameter %i).",
      arrow_type_name(schema_view.schema),
      column + 1);
}

// Strings are converted from UTF-8 to the database encoding if needed.
// Dictionary-encoded strings (e.g. factors) are bound as their values.
void odbc_result::bind_arrow_strings(
    const struct ArrowArrayView* view,
    const struct ArrowSchemaView& schema_view,
    short column,
    int64_t first,
    int64_t size) {
  std::vector<uint8_t>& nulls = buffers_.nulls_[column];
  std::vector<std::string>& values = buffers_.strings_[column];
  values.assign(size, "");

  const struct ArrowArrayView* strings = view;
  if (schema_view.type == NANOARROW_TYPE_DICTIONARY) {
    struct ArrowError error;
    ArrowErrorInit(&error);
    struct ArrowSchemaView dictionary_view;
    check_arrow(
        ArrowSchemaViewInit(
            &dictionary_view, schema_view.schema->dictionary, &error),
        "Can't parse Arrow schema",
        &error);
    switch (dictionary_view.type) {
    case NANOARROW_TYPE_STRING:
    case NANOARROW_TYPE_LARGE_STRING:
    case NANOARROW_TYPE_STRING_VIEW:
      break;
    default:
      Rcpp::stop(
          "Can't bind Arrow type '%s' (parameter %i).",
          arrow_type_name(schema_view.schema),
          column + 1);
    }
    strings = view->dictionary;
  }

  for (int64_t i = 0; i < size; ++i) {
    if (nulls[i]) {
      continue;
    }
    int64_t index = first + i;
    if (schema_view.type == NANOARROW_TYPE_DICTIONARY) {
      index = ArrowArrayViewGetIntUnsafe(view, index);
      if (ArrowArrayViewIsNull(strings, index)) {
        nulls[i] = 1;
        continue;
      }
    }
    struct ArrowStringView value = ArrowArrayViewGetStringUnsafe(strings, index);
    values[i] =
        input_encoder_->makeString(value.data, value.data + value.size_bytes);
  }

  s_->bind_strings(column, values, reinterpret_cast<bool*>(nulls.data()));
}

// Timestamps with a time zone are instants: they are expressed as civil
// time in the connection's `timezone` (or in their own time zone when the
// target is a SQL Server DATETIMEOFFSET).  Timestamps without a time zone
// are taken as civil (wall clock) time as is.
void odbc_result::bind_arrow_timestamp(
    const struct ArrowArrayView* view,
    const struct ArrowSchemaView& schema_view,
    short column,
    int64_t first,
    int64_t size) {
  std::vector<uint8_t>& nulls = buffers_.nulls_[column];
  bool* nulls_ptr = reinterpret_cast<bool*>(nulls.data());

  const int64_t per_second = units_per_second(schema_view.time_unit);
  const std::string timezone =
      schema_view.timezone == nullptr ? "" : schema_view.timezone;

  cctz::time_zone tz = cctz::utc_time_zone();
  bool bind_tso = false;
  if (!timezone.empty()) {
    tz = c_->timezone();
    bool tso_target = false;
    try {
      tso_target = s_->parameter_type(column) == SQL_SS_TIMESTAMPOFFSET;
    } catch (const nanodbc::database_error& e) {
      tso_target = false;
    }
    if (tso_target) {
      cctz::time_zone value_tz;
      if (cctz::load_time_zone(timezone, &value_tz)) {
        tz = value_tz;
        bind_tso = true;
      } else {
        raise_warning("Failed to load time zone");
      }
    }
  }

  short precision = 3;
  try {
    precision = s_->parameter_scale(column);
  } catch (const nanodbc::database_error& e) {
    raise_warning("Unable to discern datetime precision. Using default (3).");
  }
  precision = std::max<short>(0, std::min<short>(precision, 7));
  // The fraction field is expressed in billionths of a second.
  const int64_t pad = power_of_ten(9 - precision);

  std::vector<nanodbc::timestamp>& stamps = buffers_.timestamps_[column];
  std::vector<nanodbc::timestampoffset>& offsets =
      buffers_.timestampoffsets_[column];
  if (bind_tso) {
    offsets.assign(size, nanodbc::timestampoffset());
  } else {
    stamps.assign(size, nanodbc::timestamp());
  }

  for (int64_t i = 0; i < size; ++i) {
    if (nulls[i]) {
      continue;
    }
    const int64_t value = ArrowArrayViewGetIntUnsafe(view, first + i);
    const int64_t seconds = floor_div(value, per_second);
    const int64_t fraction_ns =
        (value - seconds * per_second) * (1000000000LL / per_second);

    const auto lookup = tz.lookup(
        std::chrono::time_point<std::chrono::system_clock, std::chrono::seconds>(
            std::chrono::seconds(seconds)));
    const cctz::civil_second& civil = lookup.cs;

    nanodbc::timestampoffset tso = nanodbc::timestampoffset();
    nanodbc::timestamp& ts = tso.stamp;
    ts.year = civil.year();
    ts.month = civil.month();
    ts.day = civil.day();
    ts.hour = civil.hour();
    ts.min = civil.minute();
    ts.sec = civil.second();
    ts.fract = static_cast<std::int32_t>((fraction_ns / pad) * pad);

    if (bind_tso) {
      tso.offset_hour = std::floor(lookup.offset / 3600.);
      tso.offset_minute =
          std::floor((lookup.offset - tso.offset_hour * 3600) / 60.);
      offsets[i] = tso;
    } else {
      stamps[i] = ts;
    }
  }

  if (bind_tso) {
    s_->bind(column, offsets.data(), size, nulls_ptr);
  } else {
    s_->bind(column, stamps.data(), size, nulls_ptr);
  }
}

} // namespace odbc
