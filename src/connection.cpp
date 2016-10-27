#include "Rcpp.h"
#include "nanodbc.h"
#include "odbconnect_types.h"
#include <sqlext.h>

using namespace odbconnect;

// [[Rcpp::export]]
connection_ptr odbconnect_connect(std::string connection_string) {
  return connection_ptr(new odbc_connection(connection_string));
}

// [[Rcpp::export]]
Rcpp::List connection_info(connection_ptr p) {
    return Rcpp::List::create(
      Rcpp::_["dbname"] = p->connection()->dbms_name(),
      Rcpp::_["db.version"]     = p->connection()->dbms_version(),
      Rcpp::_["username"] = "",
      Rcpp::_["host"] = "",
      Rcpp::_["port"] = "",
      Rcpp::_["sourcename"]   = p->connection()->get_info<std::string>(SQL_DATA_SOURCE_NAME),
      Rcpp::_["servername"]   = p->connection()->get_info<std::string>(SQL_SERVER_NAME),
      Rcpp::_["drivername"]   = p->connection()->driver_name(),
      Rcpp::_["odbc.version"]   = p->connection()->get_info<std::string>(SQL_ODBC_VER),
      Rcpp::_["driver.version"]   = p->connection()->get_info<std::string>(SQL_DRIVER_VER),
      Rcpp::_["odbcdriver.version"]   = p->connection()->get_info<std::string>(SQL_DRIVER_ODBC_VER)
    );
}

// [[Rcpp::export]]
std::string connection_quote(connection_ptr p) {
  return p->connection()->get_info<std::string>(SQL_IDENTIFIER_QUOTE_CHAR);
}

// [[Rcpp::export]]
std::string connection_special(connection_ptr p) {
  return p->connection()->get_info<std::string>(SQL_SPECIAL_CHARACTERS);
}

// [[Rcpp::export]]
void connection_release(connection_ptr p) {
  p.release();
  return;
}

// [[Rcpp::export]]
void connection_begin(connection_ptr p) {
  p->begin();
  return;
}

// [[Rcpp::export]]
void connection_commit(connection_ptr p) {
  p->commit();
  return;
}

// [[Rcpp::export]]
void connection_rollback(connection_ptr p) {
  p->rollback();
  return;
}

// [[Rcpp::export]]
bool connection_valid(connection_ptr p) {
  return p.get() != NULL;
}

// [[Rcpp::export]]
std::vector<std::string> connection_sql_tables(connection_ptr p,
    std::string catalog_name = "",
    std::string schema_name = "",
    std::string table_name = "",
    std::string table_type = "") {
  auto c = nanodbc::catalog(*p->connection());
  auto tables = c.find_tables(table_name, table_type, schema_name, catalog_name);
  std::vector<std::string> out;
  while(tables.next()) {
    out.push_back(tables.table_name());
  }
  return out;
}

// "%" is a wildcard for all possible values
// [[Rcpp::export]]
Rcpp::RObject connection_sql_columns(connection_ptr p,
    std::string catalog_name = "%",
    std::string schema_name = "%",
    std::string table_name = "%",
    std::string table_type = "%") {
  //auto c = turbodbc::cursor(*p, turbodbc::megabytes(10), 0, false);
  //return c.sql_columns(catalog_name, schema_name, table_name, table_type);
  return Rcpp::List();
}
