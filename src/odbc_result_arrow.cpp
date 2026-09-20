// Native Arrow support: fetching result sets into Arrow arrays and binding
// Arrow arrays to statement parameters, without going through R vectors.

#include "odbc_result.h"
#include "cctz/civil_time.h"
#include "cctz/time_zone.h"
#include "utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>

#ifndef SQL_SS_TIMESTAMPOFFSET
#define SQL_SS_TIMESTAMPOFFSET (-155)
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

bool is_variable_width(r_type type) {
  return type == string_t || type == ustring_t || type == raw_t;
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
  arrow_types_.clear();
  arrow_schema_ready_ = false;
}

// The type mapping mirrors the one used for data frames:
//
// * SQL_BIT -> bool
// * SQL_TINYINT, SQL_SMALLINT, SQL_INTEGER -> int32
// * SQL_BIGINT -> int64, int32, double or utf8, following the `bigint`
//   argument of `dbConnect()`
// * SQL_REAL, SQL_FLOAT, SQL_DOUBLE, SQL_DECIMAL, SQL_NUMERIC -> double
// * SQL_DATE -> date32
// * SQL_TIME -> time32[s]
// * SQL_TIMESTAMP -> timestamp[us], with the `timezone_out` time zone
// * character types -> utf8
// * binary types -> binary
// * anything else -> utf8
void odbc_result::ensure_arrow_schema() {
  if (arrow_schema_ready_) {
    return;
  }

  std::vector<r_type> types;
  std::vector<std::string> names;
  if (num_columns_ > 0) {
    types = column_types(*r_);
    names = column_names(*r_);
  }

  nanoarrow::UniqueSchema schema;
  ArrowSchemaInit(schema.get());
  check_arrow(
      ArrowSchemaSetTypeStruct(schema.get(), num_columns_),
      "Can't allocate Arrow schema");

  for (int i = 0; i < num_columns_; ++i) {
    struct ArrowSchema* child = schema->children[i];
    int code = NANOARROW_OK;
    switch (types[i]) {
    case logical_t:
      code = ArrowSchemaSetType(child, NANOARROW_TYPE_BOOL);
      break;
    case integer_t:
      code = ArrowSchemaSetType(child, NANOARROW_TYPE_INT32);
      break;
    case integer64_t:
      code = ArrowSchemaSetType(child, NANOARROW_TYPE_INT64);
      break;
    case odbc::double_t:
      code = ArrowSchemaSetType(child, NANOARROW_TYPE_DOUBLE);
      break;
    case date_int_t:
    case date_double_t:
      code = ArrowSchemaSetType(child, NANOARROW_TYPE_DATE32);
      break;
    case odbc::time_t:
      code = ArrowSchemaSetTypeDateTime(
          child, NANOARROW_TYPE_TIME32, NANOARROW_TIME_UNIT_SECOND, nullptr);
      break;
    case datetime_int_t:
    case datetime_double_t:
      code = ArrowSchemaSetTypeDateTime(
          child,
          NANOARROW_TYPE_TIMESTAMP,
          NANOARROW_TIME_UNIT_MICRO,
          c_->timezone_out_str().c_str());
      break;
    case raw_t:
      code = ArrowSchemaSetType(child, NANOARROW_TYPE_BINARY);
      break;
    case string_t:
    case ustring_t:
    default:
      code = ArrowSchemaSetType(child, NANOARROW_TYPE_STRING);
      break;
    }
    check_arrow(code, "Can't set Arrow column type");
    check_arrow(
        ArrowSchemaSetName(child, names[i].c_str()),
        "Can't set Arrow column name");
  }

  arrow_schema_ = std::move(schema);
  arrow_types_ = types;
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

int64_t odbc_result::fetch_arrow_rows(struct ArrowArray& out, int64_t n_max) {
  nanodbc::result& r = *r_;
  const size_t ncols = arrow_types_.size();

  std::vector<size_t> var_cols;
  for (size_t col = 0; col < ncols; ++col) {
    if (is_variable_width(arrow_types_[col])) {
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
          out.children[col], arrow_types_[col], static_cast<short>(col), r);
    }
    check_arrow(ArrowArrayFinishElement(&out), "Can't append row to Arrow array");

    complete_ = !r.next();
    ++row;
    ++rows_fetched_;
    if (rows_fetched_ % 16384 == 0) {
      Rcpp::checkUserInterrupt();
    }
    complete_ = complete_ && !nextResultSet(r);

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

// Nullity is checked both before and after retrieving the value, for the
// same reason as in the data frame path: for unbound columns the null
// indicator is only set once the data has been retrieved.
void odbc_result::append_arrow_value(
    struct ArrowArray* child, r_type type, short column, nanodbc::result& value) {
  if (value.is_null(column)) {
    check_arrow(ArrowArrayAppendNull(child, 1), "Can't append to Arrow array");
    return;
  }

  int code = NANOARROW_OK;
  switch (type) {
  case logical_t: {
    int v = value.get<int>(column, 0);
    code = value.is_null(column) ? ArrowArrayAppendNull(child, 1)
                                 : ArrowArrayAppendInt(child, v != 0);
    break;
  }
  case integer_t: {
    int v = value.get<int>(column, 0);
    code = value.is_null(column) ? ArrowArrayAppendNull(child, 1)
                                 : ArrowArrayAppendInt(child, v);
    break;
  }
  case integer64_t: {
    int64_t v = value.get<int64_t>(column, 0);
    code = value.is_null(column) ? ArrowArrayAppendNull(child, 1)
                                 : ArrowArrayAppendInt(child, v);
    break;
  }
  case odbc::double_t: {
    double v = value.get<double>(column, 0.0);
    code = value.is_null(column) ? ArrowArrayAppendNull(child, 1)
                                 : ArrowArrayAppendDouble(child, v);
    break;
  }
  case date_int_t:
  case date_double_t: {
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
  case odbc::time_t: {
    nanodbc::time v = value.get<nanodbc::time>(column);
    code = value.is_null(column)
               ? ArrowArrayAppendNull(child, 1)
               : ArrowArrayAppendInt(child, v.hour * 3600 + v.min * 60 + v.sec);
    break;
  }
  case datetime_int_t:
  case datetime_double_t: {
    nanodbc::timestampoffset v = value.get<nanodbc::timestampoffset>(column);
    code = value.is_null(column) ? ArrowArrayAppendNull(child, 1)
                                 : ArrowArrayAppendInt(child, as_micros(v));
    break;
  }
  case string_t:
  case ustring_t: {
    std::string v = value.get<std::string>(column);
    if (value.is_null(column)) {
      code = ArrowArrayAppendNull(child, 1);
      break;
    }
    // Strings may be in the server's internal code page, so we need to
    // re-encode in UTF-8 if necessary.  Unicode strings are converted to
    // UTF-8 by nanodbc already.
    if (type == string_t) {
      v = output_encoder_->makeString(v.c_str(), v.c_str() + v.length());
    }
    struct ArrowStringView view;
    view.data = v.data();
    view.size_bytes = length_to_null(v);
    code = ArrowArrayAppendString(child, view);
    break;
  }
  case raw_t: {
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
    // `hms`) values are.  Fractional seconds are dropped, as for data frames.
    std::vector<nanodbc::time>& values = buffers_.times_[column];
    values.assign(size, nanodbc::time());
    const int64_t per_second = units_per_second(schema_view.time_unit);
    for (int64_t i = 0; i < size; ++i) {
      if (nulls[i]) {
        continue;
      }
      int64_t v = ArrowArrayViewGetIntUnsafe(view, first + i);
      values[i] = as_time(static_cast<double>(floor_div(v, per_second)));
    }
    s_->bind(column, values.data(), size, nulls_ptr);
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
