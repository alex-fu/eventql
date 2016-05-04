/**
 * Copyright (c) 2015 - The CM Authors <legal@clickmatcher.com>
 *   All Rights Reserved.
 *
 * This file is CONFIDENTIAL -- Distribution or duplication of this material or
 * the information contained herein is strictly forbidden unless prior written
 * permission is obtained.
 */
#include <thread>

#include "AnalyticsServlet.h"
#include "eventql/util/Language.h"
#include "eventql/util/human.h"
#include "eventql/util/wallclock.h"
#include "eventql/util/io/fileutil.h"
#include "eventql/util/util/Base64.h"
#include "eventql/util/logging.h"
#include "eventql/util/assets.h"
#include "eventql/util/protobuf/msg.h"
#include "eventql/util/http/cookies.h"
#include "eventql/util/protobuf/DynamicMessage.h"
#include "eventql/util/protobuf/MessageEncoder.h"
#include "eventql/util/csv/CSVInputStream.h"
#include "eventql/util/csv/BinaryCSVInputStream.h"
#include "eventql/PipelineInfo.h"
#include "eventql/TableDefinition.h"
#include "eventql/sql/runtime/ASCIITableFormat.h"
#include "eventql/sql/runtime/JSONResultFormat.h"
#include "eventql/sql/runtime/JSONSSEStreamFormat.h"
#include "eventql/sql/runtime/BinaryResultFormat.h"
#include "eventql/sql/runtime/ExecutionStrategy.h"
#include "eventql/core/TimeWindowPartitioner.h"
#include "eventql/core/FixedShardPartitioner.h"
#include "eventql/DrilldownQuery.h"
#include "eventql/HTTPAuth.h"
#include <eventql/infra/cstable/CSTableWriter.h>
#include <eventql/infra/cstable/RecordShredder.h>

using namespace stx;

namespace zbase {

AnalyticsServlet::AnalyticsServlet(
    RefPtr<AnalyticsApp> app,
    const String& cachedir,
    AnalyticsAuth* auth,
    csql::Runtime* sql,
    zbase::TSDBService* tsdb,
    ConfigDirectory* customer_dir,
    PartitionMap* pmap) :
    app_(app),
    cachedir_(cachedir),
    auth_(auth),
    sql_(sql),
    tsdb_(tsdb),
    customer_dir_(customer_dir),
    logfile_api_(app->logfileService(), customer_dir, cachedir),
    events_api_(app->eventsService(), customer_dir, cachedir),
    mapreduce_api_(app->mapreduceService(), customer_dir, cachedir),
    pmap_(pmap) {}

void AnalyticsServlet::handleHTTPRequest(
    RefPtr<http::HTTPRequestStream> req_stream,
    RefPtr<http::HTTPResponseStream> res_stream) {
  try {
    handle(req_stream, res_stream);
  } catch (const StandardException& e) {
    logError("zbase", e, "error while handling HTTP request");
    req_stream->discardBody();

    http::HTTPResponse res;
    res.populateFromRequest(req_stream->request());
    res.setStatus(http::kStatusInternalServerError);
    res.addHeader("Content-Type", "text/html; charset=utf-8");
    res.addBody(Assets::getAsset("eventql/webui/500.html"));
    res_stream->writeResponse(res);
  }
}

void AnalyticsServlet::handle(
    RefPtr<http::HTTPRequestStream> req_stream,
    RefPtr<http::HTTPResponseStream> res_stream) {
  const auto& req = req_stream->request();
  URI uri(req.uri());

  logDebug("zbase", "HTTP Request: $0 $1", req.method(), req.uri());

  http::HTTPResponse res;
  res.populateFromRequest(req);
  res.addHeader("Access-Control-Allow-Origin", "*");
  res.addHeader("Access-Control-Allow-Methods", "GET, POST");
  res.addHeader("Access-Control-Allow-Headers", "Authorization");

  if (req.method() == http::HTTPMessage::M_OPTIONS) {
    req_stream->readBody();
    res.setStatus(http::kStatusOK);
    res_stream->writeResponse(res);
    return;
  }

  /* AUTH METHODS */
  if (uri.path() == "/api/v1/auth/login") {
    expectHTTPPost(req);
    req_stream->readBody();
    performLogin(uri, &req, &res);
    res_stream->writeResponse(res);
    return;
  }

  if (uri.path() == "/api/v1/auth/logout") {
    expectHTTPPost(req);
    req_stream->readBody();
    performLogout(uri, &req, &res);
    res_stream->writeResponse(res);
    return;
  }

  auto session_opt = HTTPAuth::authenticateRequest(
      req_stream->request(),
      auth_);

  if (uri.path() == "/api/v1/tables/insert") {
    req_stream->readBody();
    catchAndReturnErrors(&res, [this, &session_opt, &req, &res] {
      insertIntoTable(session_opt, &req, &res);
    });
    res_stream->writeResponse(res);
    return;
  }

  /* authorized below here */

  if (session_opt.isEmpty()) {
    req_stream->readBody();
    res.setStatus(http::kStatusUnauthorized);
    res.addHeader("WWW-Authenticate", "Token");
    res.addHeader("Content-Type", "text/html; charset=utf-8");
    res.addBody(Assets::getAsset("eventql/webui/401.html"));
    res_stream->writeResponse(res);
    return;
  }

  const auto& session = session_opt.get();

  if (StringUtil::beginsWith(uri.path(), "/api/v1/logfiles")) {
    logfile_api_.handle(session, req_stream, res_stream);
    return;
  }

  if (StringUtil::beginsWith(uri.path(), "/api/v1/events")) {
    events_api_.handle(session, req_stream, res_stream);
    return;
  }

  if (StringUtil::beginsWith(uri.path(), "/api/v1/mapreduce")) {
    mapreduce_api_.handle(session, req_stream, res_stream);
    return;
  }

  if (uri.path() == "/api/v1/auth/info") {
    req_stream->readBody();
    getAuthInfo(session, &req, &res);
    res_stream->writeResponse(res);
    return;
  }

  if (uri.path() == "/api/v1/auth/private_api_token") {
    req_stream->readBody();
    getPrivateAPIToken(session, &req, &res);
    res_stream->writeResponse(res);
    return;
  }


  if (uri.path() == "/api/v1/pipeline_info") {
    req_stream->readBody();
    pipelineInfo(session, &req, &res);
    res_stream->writeResponse(res);
    return;
  }


  /* SESSION TRACKING */
  if (uri.path() == "/api/v1/session_tracking/events") {
    req_stream->readBody();
    catchAndReturnErrors(&res, [this, &session, &req, &res] {
      sessionTrackingListEvents(session, &req, &res);
    });
    res_stream->writeResponse(res);
    return;
  }

  if (uri.path() == "/api/v1/session_tracking/events/add_event") {
    expectHTTPPost(req);
    req_stream->readBody();
    sessionTrackingEventAdd(session, &req, &res);
    res_stream->writeResponse(res);
    return;
  }

  if (uri.path() == "/api/v1/session_tracking/events/remove_event") {
    expectHTTPPost(req);
    req_stream->readBody();
    sessionTrackingEventRemove(session, &req, &res);
    res_stream->writeResponse(res);
    return;
  }

  if (uri.path() == "/api/v1/session_tracking/events/add_field") {
    expectHTTPPost(req);
    req_stream->readBody();
    catchAndReturnErrors(&res, [this, &session, &req, &res] {
      sessionTrackingEventAddField(session, &req, &res);
    });
    res_stream->writeResponse(res);
    return;
  }

  if (uri.path() == "/api/v1/session_tracking/events/remove_field") {
    expectHTTPPost(req);
    req_stream->readBody();
    catchAndReturnErrors(&res, [this, &session, &req, &res] {
      sessionTrackingEventRemoveField(session, &req, &res);
    });
    res_stream->writeResponse(res);
    return;
  }

  if (uri.path() == "/api/v1/session_tracking/event_info") {
    req_stream->readBody();
    catchAndReturnErrors(&res, [this, &session, &req, &res] {
      sessionTrackingEventInfo(session, &req, &res);
    });
    res_stream->writeResponse(res);
    return;
  }

  if (uri.path() == "/api/v1/session_tracking/attributes") {
    req_stream->readBody();
    catchAndReturnErrors(&res, [this, &session, &req, &res] {
      sessionTrackingListAttributes(session, &req, &res);
    });
    res_stream->writeResponse(res);
    return;
  }


  /* TABLES */
  if (uri.path() == "/api/v1/tables") {
    req_stream->readBody();
    listTables(session, &req, &res);
    res_stream->writeResponse(res);
    return;
  }

  if (uri.path() == "/api/v1/tables/create_table") {
    req_stream->readBody();
    catchAndReturnErrors(&res, [this, &session, &req, &res] {
      createTable(session, &req, &res);
    });
    res_stream->writeResponse(res);
    return;
  }

  if (uri.path() == "/api/v1/tables/upload_table") {
    expectHTTPPost(req);
    uploadTable(uri, session, req_stream.get(), &res);
    res_stream->writeResponse(res);
    return;
  }

  if (uri.path() == "/api/v1/tables/add_field") {
    req_stream->readBody();
    catchAndReturnErrors(&res, [this, &session, &req, &res] {
      addTableField(session, &req, &res);
    });
    res_stream->writeResponse(res);
    return;
  }

  if (uri.path() == "/api/v1/tables/remove_field") {
    req_stream->readBody();
    catchAndReturnErrors(&res, [this, &session, &req, &res] {
      removeTableField(session, &req, &res);
    });
    res_stream->writeResponse(res);
    return;
  }

  if (uri.path() == "/api/v1/tables/add_tag") {
    req_stream->readBody();
    catchAndReturnErrors(&res, [this, &session, &req, &res] {
      addTableTag(session, &req, &res);
    });
    res_stream->writeResponse(res);
    return;
  }

  if (uri.path() == "/api/v1/tables/remove_tag") {
    req_stream->readBody();
    catchAndReturnErrors(&res, [this, &session, &req, &res] {
      removeTableTag(session, &req, &res);
    });
    res_stream->writeResponse(res);
    return;
  }

  static const String kTablesPathPrefix = "/api/v1/tables/";
  if (StringUtil::beginsWith(uri.path(), kTablesPathPrefix)) {
    req_stream->readBody();
    fetchTableDefinition(
        session,
        uri.path().substr(kTablesPathPrefix.size()),
        &req,
        &res);
    res_stream->writeResponse(res);
    return;
  }

  /* query */
  if (uri.path() == "/api/v1/query/drilldown") {
    req_stream->readBody();
    executeDrilldownQuery(session, &req, &res, res_stream);
    return;
  }

  /* SQL */
  if (uri.path() == "/api/v1/sql" ||
      uri.path() == "/api/v1/sql_stream") {
    req_stream->readBody();
    executeSQL(session, &req, &res, res_stream);
    return;
  }

  if (uri.path() == "/api/v1/sql/aggregate_partition") {
    req_stream->readBody();
    executeSQLAggregatePartition(session, &req, &res);
    res_stream->writeResponse(res);
    return;
  }

  if (uri.path() == "/api/v1/sql/scan_partition") {
    req_stream->readBody();
    executeSQLScanPartition(session, &req, &res, res_stream);
    return;
  }


  res.setStatus(http::kStatusNotFound);
  res.addHeader("Content-Type", "text/html; charset=utf-8");
  res.addBody(Assets::getAsset("eventql/webui/404.html"));
  res_stream->writeResponse(res);
}

void AnalyticsServlet::listTables(
    const AnalyticsSession& session,
    const http::HTTPRequest* req,
    http::HTTPResponse* res) {
  URI uri(req->uri());
  const auto& params = uri.queryParams();

  /* param tag */
  String tag_filter;
  URI::getParam(params, "tag", &tag_filter);

  if (tag_filter == "all") {
    tag_filter.clear();
  }

  /* param sort_fn */
  String order_filter;
  URI::getParam(params, "order", &order_filter);

  Buffer buf;
  json::JSONOutputStream json(BufferOutputStream::fromBuffer(&buf));

  json.beginObject();
  json.addObjectEntry("tables");
  json.beginArray();

  auto table_service = app_->getTSDBNode();
  size_t ntable = 0;

  auto writeTableJSON = [&json, &ntable, &tag_filter] (const TSDBTableInfo& table) {

    Set<String> tags;
    for (const auto& tag : table.config.tags()) {
      tags.insert(tag);
    }

    if (!tag_filter.empty()) {
      if (tags.count(tag_filter) == 0) {
        return;
      }
    }

    if (++ntable > 1) {
      json.addComma();
    }

    json.beginObject();

    json.addObjectEntry("name");
    json.addString(table.table_name);
    json.addComma();

    json.addObjectEntry("tags");
    json::toJSON(tags, &json);

    json.endObject();

  };

  if (order_filter == "desc") {
    table_service->listTablesReverse(session.customer(), writeTableJSON);
  } else {
    table_service->listTables(session.customer(), writeTableJSON);
  }

  json.endArray();
  json.endObject();

  res->setStatus(http::kStatusOK);
  res->setHeader("Content-Type", "application/json; charset=utf-8");
  res->addBody(buf);
}

void AnalyticsServlet::fetchTableDefinition(
    const AnalyticsSession& session,
    const String& table_name,
    const http::HTTPRequest* req,
    http::HTTPResponse* res) {

  auto table_provider = app_->getTableProvider(session.customer());
  auto table_info_opt = table_provider->describe(table_name);
  if (table_info_opt.isEmpty()) {
    res->setStatus(http::kStatusNotFound);
    res->addBody("table not found");
    return;
  }

  const auto& table_info = table_info_opt.get();

  auto table_opt = pmap_->findTable(session.customer(), table_name);
  if (table_opt.isEmpty()) {
    res->setStatus(http::kStatusNotFound);
    res->addBody("table not found");
    return;
  }

  Buffer buf;
  json::JSONOutputStream json(BufferOutputStream::fromBuffer(&buf));

  json.beginObject();
  json.addObjectEntry("table");
  json.beginObject();

  json.addObjectEntry("name");
  json.addString(table_info.table_name);
  json.addComma();

  json.addObjectEntry("tags");
  json::toJSON(table_info.tags, &json);
  json.addComma();

  json.addObjectEntry("columns");
  json.beginArray();
  for (size_t i = 0; i < table_info.columns.size(); ++i) {
    const auto& col = table_info.columns[i];

    if (i > 0) {
      json.addComma();
    }

    json.beginObject();

    json.addObjectEntry("column_name");
    json.addString(col.column_name);
    json.addComma();

    json.addObjectEntry("type");
    json.addString(col.type);
    json.addComma();

    json.addObjectEntry("is_nullable");
    json.addBool(col.is_nullable);

    json.endObject();
  }

  json.endArray();

  json.addComma();
  json.addObjectEntry("schema");
  table_opt.get()->schema()->toJSON(&json);

  json.endObject();
  json.endObject();

  res->setStatus(http::kStatusOK);
  res->setHeader("Content-Type", "application/json; charset=utf-8");
  res->addBody(buf);
}

void AnalyticsServlet::createTable(
    const AnalyticsSession& session,
    const http::HTTPRequest* req,
    http::HTTPResponse* res) {
  auto jreq = json::parseJSON(req->body());

  auto table_name = json::objectGetString(jreq, "table_name");
  if (table_name.isEmpty()) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing field: table_name");
    return;
  }

  auto jschema = json::objectLookup(jreq, "schema");
  if (jschema == jreq.end()) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing field: schema");
    return;
  }

  auto num_shards = json::objectGetUInt64(jreq, "num_shards");

  String table_type = "static";
  auto jtable_type = json::objectGetString(jreq, "table_type");
  if (!jtable_type.isEmpty()) {
    table_type = jtable_type.get();
  }

  auto update_param = json::objectGetBool(jreq, "update");
  auto force = !update_param.isEmpty() && update_param.get();

  try {
    msg::MessageSchema schema(nullptr);
    schema.fromJSON(jschema, jreq.end());

    TableDefinition td;
    td.set_customer(session.customer());
    td.set_table_name(table_name.get());

    auto tblcfg = td.mutable_config();
    tblcfg->set_schema(schema.encode().toString());
    tblcfg->set_num_shards(num_shards.isEmpty() ? 1 : num_shards.get());

    if (table_type == "timeseries" || table_type == "log_timeseries") {
      tblcfg->set_partitioner(zbase::TBL_PARTITION_TIMEWINDOW);
      tblcfg->set_storage(zbase::TBL_STORAGE_COLSM);

      auto partition_size = json::objectGetUInt64(jreq, "partition_size");
      auto partcfg = tblcfg->mutable_time_window_partitioner_config();
      partcfg->set_partition_size(
          partition_size.isEmpty() ? 4 * kMicrosPerHour : partition_size.get());
    }

    else if (table_type == "static" || table_type == "static_fixed") {
      tblcfg->set_partitioner(zbase::TBL_PARTITION_FIXED);
      tblcfg->set_storage(zbase::TBL_STORAGE_STATIC);
    }

    else if (table_type == "static_timeseries") {
      tblcfg->set_partitioner(zbase::TBL_PARTITION_TIMEWINDOW);
      tblcfg->set_storage(zbase::TBL_STORAGE_STATIC);

      auto partition_size = json::objectGetUInt64(jreq, "partition_size");
      auto partcfg = tblcfg->mutable_time_window_partitioner_config();
      partcfg->set_partition_size(
          partition_size.isEmpty() ? 4 * kMicrosPerHour : partition_size.get());
    }

    else {
      RAISEF(kIllegalArgumentError, "invalid table type: $0", table_type);
    }

    app_->updateTable(td, force);
  } catch (const StandardException& e) {
    stx::logError("analyticsd", e, "error");
    res->setStatus(http::kStatusInternalServerError);
    res->addBody(StringUtil::format("error: $0", e.what()));
    return;
  }

  res->setStatus(http::kStatusCreated);
}

void AnalyticsServlet::addTableField(
    const AnalyticsSession& session,
    const http::HTTPRequest* req,
    http::HTTPResponse* res) {

  URI uri(req->uri());
  const auto& params = uri.queryParams();

  String table_name;
  if (!URI::getParam(params, "table", &table_name)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing ?table=... parameter");
    return;
  }

  auto table_opt = pmap_->findTable(session.customer(), table_name);
  if (table_opt.isEmpty()) {
    res->setStatus(http::kStatusNotFound);
    res->addBody("table not found");
    return;
  }
  const auto& table = table_opt.get();

  String field_name;
  if (!URI::getParam(params, "field_name", &field_name)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing &field_name=... parameter");
    return;
  }

  String field_type_str;
  if (!URI::getParam(params, "field_type", &field_type_str)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing &field_name=... parameter");
    return;
  }

  String repeated_param;
  URI::getParam(params, "repeated", &repeated_param);
  auto repeated = Human::parseBoolean(repeated_param);

  String optional_param;
  URI::getParam(params, "optional", &optional_param);
  auto optional = Human::parseBoolean(optional_param);

  auto td = table->config();
  auto schema = stx::msg::MessageSchema::decode(td.config().schema());

  uint32_t next_field_id;
  if (td.has_next_field_id()) {
    next_field_id = td.next_field_id();
  } else {
    next_field_id = schema->maxFieldId() + 1;
  }

  auto cur_schema = schema;
  auto field = field_name;

  while (StringUtil::includes(field, ".")) {
    auto prefix_len = field.find(".");
    auto prefix = field.substr(0, prefix_len);

    field = field.substr(prefix_len + 1);
    if (!cur_schema->hasField(prefix)) {
      res->setStatus(http::kStatusNotFound);
      res->addBody(StringUtil::format("field $0 not found", prefix));
      return;
    }

    auto parent_field_id = cur_schema->fieldId(prefix);
    auto parent_field_type = cur_schema->fieldType(parent_field_id);
    if (parent_field_type != stx::msg::FieldType::OBJECT) {
      res->setStatus(http::kStatusBadRequest);
      res->addBody(StringUtil::format(
        "can't add field to a field of type $0",
        fieldTypeToString(parent_field_type)));
      return;
    }

    cur_schema = cur_schema->fieldSchema(parent_field_id);
  }

  auto field_type = stx::msg::fieldTypeFromString(field_type_str);
  if (field_type == stx::msg::FieldType::OBJECT) {
    cur_schema->addField(
          stx::msg::MessageSchemaField::mkObjectField(
              next_field_id,
              field,
              repeated.isEmpty() ? false : repeated.get(),
              optional.isEmpty() ? false : optional.get(),
              mkRef(new stx::msg::MessageSchema(nullptr))));


  } else {
    cur_schema->addField(
          stx::msg::MessageSchemaField(
              next_field_id,
              field,
              field_type,
              0,
              repeated.isEmpty() ? false : repeated.get(),
              optional.isEmpty() ? false : optional.get()));
  }


  td.set_next_field_id(next_field_id + 1);
  td.mutable_config()->set_schema(schema->encode().toString());

  app_->updateTable(td);
  res->setStatus(http::kStatusCreated);
  res->addBody("ok");
  return;
}

void AnalyticsServlet::removeTableField(
    const AnalyticsSession& session,
    const http::HTTPRequest* req,
    http::HTTPResponse* res) {

  URI uri(req->uri());
  const auto& params = uri.queryParams();

  String table_name;
  if (!URI::getParam(params, "table", &table_name)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing ?table=... parameter");
    return;
  }

  auto table_opt = pmap_->findTable(session.customer(), table_name);
  if (table_opt.isEmpty()) {
    res->setStatus(http::kStatusNotFound);
    res->addBody("table not found");
    return;
  }
  const auto& table = table_opt.get();

  String field_name;
  if (!URI::getParam(params, "field_name", &field_name)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing &field_name=... parameter");
    return;
  }

  auto td = table->config();
  auto schema = stx::msg::MessageSchema::decode(td.config().schema());
  auto cur_schema = schema;
  auto field = field_name;

  while (StringUtil::includes(field, ".")) {
    auto prefix_len = field.find(".");
    auto prefix = field.substr(0, prefix_len);

    field = field.substr(prefix_len + 1);

    if (!cur_schema->hasField(prefix)) {
      res->setStatus(http::kStatusNotFound);
      res->addBody("field not found");
      return;
    }
    cur_schema = cur_schema->fieldSchema(cur_schema->fieldId(prefix));
  }

  if (!cur_schema->hasField(field)) {
    res->setStatus(http::kStatusNotFound);
    res->addBody("field not found");
    return;
  }

  if (!td.has_next_field_id()) {
    td.set_next_field_id(schema->maxFieldId() + 1);
  }

  cur_schema->removeField(cur_schema->fieldId(field));
  td.mutable_config()->set_schema(schema->encode().toString());

  app_->updateTable(td);
  res->setStatus(http::kStatusCreated);
  res->addBody("ok");
  return;
}

void AnalyticsServlet::addTableTag(
    const AnalyticsSession& session,
    const http::HTTPRequest* req,
    http::HTTPResponse* res) {

  URI uri(req->uri());
  const auto& params = uri.queryParams();

  String table_name;
  if (!URI::getParam(params, "table", &table_name)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing ?table=... parameter");
    return;
  }

  auto table_opt = pmap_->findTable(session.customer(), table_name);
  if (table_opt.isEmpty()) {
    res->setStatus(http::kStatusNotFound);
    res->addBody("table not found");
    return;
  }
  const auto& table = table_opt.get();

  String tag;
  if (!URI::getParam(params, "tag", &tag)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing &tag=... parameter");
    return;
  }

  auto td = table->config();
  td.add_tags(tag);

  app_->updateTable(td);
  res->setStatus(http::kStatusCreated);
  res->addBody("ok");
  return;
}

void AnalyticsServlet::removeTableTag(
    const AnalyticsSession& session,
    const http::HTTPRequest* req,
    http::HTTPResponse* res) {

  URI uri(req->uri());
  const auto& params = uri.queryParams();

  String table_name;
  if (!URI::getParam(params, "table", &table_name)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing ?table=... parameter");
    return;
  }

  String tag;
  if (!URI::getParam(params, "tag", &tag)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing ?tag=... parameter");
    return;
  }

  auto table_opt = pmap_->findTable(session.customer(), table_name);
  if (table_opt.isEmpty()) {
    res->setStatus(http::kStatusNotFound);
    res->addBody("table not found");
    return;
  }

  auto table = table_opt.get();
  auto td = table->config();
  auto tags = td.mutable_tags();


  for (size_t i = tags->size() - 1; ; --i) {
    if (tags->Get(i) == tag) {
      tags->DeleteSubrange(i, 1);
    }

    if (i == 0) {
      break;
    }

  }

  app_->updateTable(td);
  res->setStatus(http::kStatusCreated);
  res->addBody("ok");
  return;
}


void AnalyticsServlet::insertIntoTable(
    const Option<AnalyticsSession>& session,
    const http::HTTPRequest* req,
    http::HTTPResponse* res) {
  auto jreq = json::parseJSON(req->body());

  auto ncols = json::arrayLength(jreq.begin(), jreq.end());
  for (size_t i = 0; i < ncols; ++i) {
    auto jrow = json::arrayLookup(jreq.begin(), jreq.end(), i); // O(N^2) but who cares...

    auto table = json::objectGetString(jrow, jreq.end(), "table");
    if (table.isEmpty()) {
      RAISE(kRuntimeError, "missing field: table");
    }

    String ns;
    if (!session.isEmpty()) {
      ns = session.get().customer();
    }

    if (req->hasHeader("X-Z1-Namespace")) {
      ns = req->getHeader("X-Z1-Namespace");
      auto tc = tsdb_->tableConfig(ns, table.get());
      if (tc.isEmpty() || !tc.get().config().allow_public_insert()) {
        res->setStatus(http::kStatusForbidden);
        return;
      }
    }

    if (ns.empty()) {
      RAISE(kRuntimeError, "missing X-Z1-Namespace or Authorization header");
    }

    auto id_opt = json::objectGetString(jrow, jreq.end(), "id");
    auto version_opt = json::objectGetUInt64(jrow, jreq.end(), "version");

    auto data = json::objectLookup(jrow, jreq.end(), "data");
    if (data == jreq.end()) {
      RAISE(kRuntimeError, "missing field: data");
    }

    auto record_id =
        id_opt.isEmpty() ?
            Random::singleton()->sha1() :
            SHA1::compute(id_opt.get());

    uint64_t record_version =
        version_opt.isEmpty() ?
            WallClock::unixMicros() :
            version_opt.get();

    if (data->type == json::JSON_STRING) {
      auto data_parsed = json::parseJSON(data->data);
      tsdb_->insertRecord(
          ns,
          table.get(),
          record_id,
          record_version,
          data_parsed.begin(),
          data_parsed.end());
    } else {
      tsdb_->insertRecord(
          ns,
          table.get(),
          record_id,
          record_version,
          data,
          data + data->size);
    }
  }

  res->setStatus(http::kStatusCreated);
}

void AnalyticsServlet::uploadTable(
    const URI& uri,
    const AnalyticsSession& session,
    http::HTTPRequestStream* req_stream,
    http::HTTPResponse* res) {
  const auto& params = uri.queryParams();

  String table_name;
  if (!URI::getParam(params, "table", &table_name)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("error: missing ?table=... parameter");
    return;
  }

  String shard_str;
  if (!URI::getParam(params, "shard", &shard_str)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("error: missing ?shard=... parameter");
    return;
  }

  auto shard = std::stoull(shard_str);
  auto schema = tsdb_->tableSchema(session.customer(), table_name);
  if (schema.isEmpty()) {
    res->setStatus(http::kStatusNotFound);
    res->addBody("error: table not found");
    return;
  }

  auto tmpfile_path = FileUtil::joinPaths(
      cachedir_,
      StringUtil::format("upload_$0.tmp", Random::singleton()->hex128()));

  auto partition_key = zbase::FixedShardPartitioner::partitionKeyFor(
      table_name,
      shard);

  try {
    auto cstable = cstable::CSTableWriter::createFile(
        tmpfile_path + ".cst",
        cstable::BinaryFormatVersion::v0_1_0,
        cstable::TableSchema::fromProtobuf(*schema.get()));

    cstable::RecordShredder shredder(cstable.get());

    {
      auto tmpfile = File::openFile(
          tmpfile_path,
          File::O_CREATE | File::O_READ | File::O_WRITE | File::O_AUTODELETE);

      size_t body_size = 0;
      req_stream->readBody([&tmpfile, &body_size] (const void* data, size_t size) {
        tmpfile.write(data, size);
        body_size += size;
      });

      tmpfile.seekTo(0);

      stx::logDebug(
          "analyticsd",
          "Uploading static table; customer=$0, table=$1, shard=$2, size=$3MB",
          session.customer(),
          table_name,
          shard,
          body_size / 1000000.0);

      BinaryCSVInputStream csv(FileInputStream::fromFile(std::move(tmpfile)));
      shredder.addRecordsFromCSV(&csv);
      cstable->commit();
    }

    tsdb_->updatePartitionCSTable(
        session.customer(),
        table_name,
        partition_key,
        tmpfile_path + ".cst",
        WallClock::unixMicros());
  } catch (const StandardException& e) {
    stx::logError("analyticsd", e, "error");
    res->setStatus(http::kStatusInternalServerError);
    res->addBody(StringUtil::format("error: $0", e.what()));
    return;
  }

  res->setStatus(http::kStatusCreated);
}

void AnalyticsServlet::getAuthInfo(
    const AnalyticsSession& session,
    const http::HTTPRequest* req,
    http::HTTPResponse* res) {
  Buffer buf;

  json::JSONOutputStream json(BufferOutputStream::fromBuffer(&buf));
  json.beginObject();
  json.addObjectEntry("valid");
  json.addTrue();
  json.addComma();
  json.addObjectEntry("customer");
  json.addString(session.customer());
  json.addComma();
  json.addObjectEntry("userid");
  json.addString(session.userid());
  json.endObject();

  res->addBody(buf);
  res->setStatus(http::kStatusOK);
}

void AnalyticsServlet::getPrivateAPIToken(
    const AnalyticsSession& session,
    const http::HTTPRequest* req,
    http::HTTPResponse* res) {
  URI uri(req->uri());
  const auto& params = uri.queryParams();
  String devnull;

  AnalyticsPrivileges privs;

  if (URI::getParam(params, "allow_read", &devnull)) {
    privs.set_allow_private_api_read_access(true);
  }

  if (URI::getParam(params, "allow_write", &devnull)) {
    privs.set_allow_private_api_write_access(true);
  }

  auto token = auth_->getPrivateAPIToken(session.customer(), privs);

  Buffer buf;
  json::JSONOutputStream json(BufferOutputStream::fromBuffer(&buf));
  json.beginObject();
  json.addObjectEntry("api_token");
  json.addString(URI::urlEncode(token));
  json.endObject();

  res->addBody(buf);
  res->setStatus(http::kStatusOK);
}

void AnalyticsServlet::executeSQLAggregatePartition(
    const AnalyticsSession& session,
    const http::HTTPRequest* req,
    http::HTTPResponse* res) {
  auto query = req->body().toString();

  auto txn = sql_->newTransaction();

  Buffer result;
  auto os = BufferOutputStream::fromBuffer(&result);

  try {
    sql_->executeAggregate(
        txn.get(),
        msg::decode<csql::RemoteAggregateParams>(query),
        app_->getExecutionStrategy(session.customer()),
        os.get());

    res->setStatus(http::kStatusOK);
    res->addHeader("Content-Type", "application/octet-stream");
    res->addBody(result);
  } catch (const StandardException& e) {
    res->setStatus(http::kStatusInternalServerError);
    res->addHeader("Content-Type", "application/octet-stream");
    res->addBody(e.what());
  }
}

void AnalyticsServlet::executeSQLScanPartition(
    const AnalyticsSession& session,
    const http::HTTPRequest* req,
    http::HTTPResponse* res,
    RefPtr<http::HTTPResponseStream> res_stream) {
  // FIXME error handling
  auto query = msg::decode<RemoteTSDBScanParams>(req->body().toString());

  res->setStatus(http::kStatusOK);
  res->addHeader("Content-Type", "application/octet-stream");
  res->addHeader("Connection", "close");
  res_stream->startResponse(*res);

  {
    auto result_format = new csql::BinaryResultFormat(
        [res_stream] (const void* data, size_t size) {
      res_stream->writeBodyChunk(data, size);
    });

    try {
      auto txn = sql_->newTransaction();

      Vector<RefPtr<csql::SelectListNode>> select_list;
      for (const auto& e : query.select_list()) {
        csql::Parser parser;
        parser.parseValueExpression(
            e.expression().data(),
            e.expression().size());

        auto stmts = parser.getStatements();
        if (stmts.size() != 1) {
          RAISE(kIllegalArgumentError);
        }

        auto slnode = mkRef(
            new csql::SelectListNode(
                sql_->queryPlanBuilder()->buildValueExpression(
                    txn.get(),
                    stmts[0])));

        if (e.has_alias()) {
          slnode->setAlias(e.alias());
        }

        select_list.emplace_back(slnode);
      }

      Option<RefPtr<csql::ValueExpressionNode>> where_expr;
      if (query.has_where_expression()) {
        csql::Parser parser;
        parser.parseValueExpression(
            query.where_expression().data(),
            query.where_expression().size());

        auto stmts = parser.getStatements();
        if (stmts.size() != 1) {
          RAISE(kIllegalArgumentError);
        }

        where_expr = Some(
            sql_->queryPlanBuilder()->buildValueExpression(
                txn.get(),
                stmts[0]));
      }

      auto execution_strategy = app_->getExecutionStrategy(session.customer());
      auto table_info = execution_strategy
          ->tableProvider()
          ->describe(query.table_name());

      if (table_info.isEmpty()) {
        RAISEF(kNotFoundError, "table not found: '$0'", query.table_name());
      }

      auto qtree = mkRef(
          new csql::SequentialScanNode(
              table_info.get(),
              select_list,
              where_expr,
              (csql::AggregationStrategy) query.aggregation_strategy()));

      auto qplan = sql_->buildQueryPlan(
          txn.get(),
          Vector<RefPtr<csql::QueryTreeNode>>{ qtree.get() },
          execution_strategy);

      sql_->executeQuery(txn.get(), qplan, result_format);
    } catch (const StandardException& e) {
      result_format->sendError(e.what());
    }
  }

  res_stream->finishResponse();
}

void AnalyticsServlet::executeSQL(
    const AnalyticsSession& session,
    const http::HTTPRequest* req,
    http::HTTPResponse* res,
    RefPtr<http::HTTPResponseStream> res_stream) {
  try {
    URI uri(req->uri());
    URI::ParamList params = uri.queryParams();
    URI::parseQueryString(req->body().toString(), &params);

    String format;
    URI::getParam(params, "format", &format);
    if (format.empty()) {
      format = "json";
    }

    if (format == "ascii") {
      executeSQL_ASCII(params, session, req, res, res_stream);
    } else if (format == "binary") {
      executeSQL_BINARY(params, session, req, res, res_stream);
    } else if (format == "json") {
      executeSQL_JSON(params, session, req, res, res_stream);
    } else if (format == "json_sse") {
      executeSQL_JSONSSE(params, session, req, res, res_stream);
    } else {
      res->setStatus(http::kStatusBadRequest);
      res->addBody("invalid format: " + format);
      res_stream->writeResponse(*res);
    }
  } catch (const StandardException& e) {
    logError("z1.sql", e, "Uncaught SQL error");
    res->setStatus(http::kStatusBadRequest);
    res->addBody("invalid request");
    res_stream->writeResponse(*res);
  }
}

void AnalyticsServlet::executeSQL_ASCII(
    const URI::ParamList& params,
    const AnalyticsSession& session,
    const http::HTTPRequest* req,
    http::HTTPResponse* res,
    RefPtr<http::HTTPResponseStream> res_stream) {
  String query;
  if (!URI::getParam(params, "query", &query)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing ?query=... parameter");
    res_stream->writeResponse(*res);
    return;
  }

  try {
    auto ctx = sql_->newTransaction();

    Buffer result;
    sql_->executeQuery(
        ctx.get(),
        query,
        app_->getExecutionStrategy(session.customer()),
        new csql::ASCIITableFormat(BufferOutputStream::fromBuffer(&result)));

    res->setStatus(http::kStatusOK);
    res->addHeader("Content-Type", "text/plain; charset=utf-8");
    res->addBody(result);
    res_stream->writeResponse(*res);
  } catch (const StandardException& e) {
    res->setStatus(http::kStatusInternalServerError);
    res->addHeader("Content-Type", "text/plain; charset=utf-8");
    res->addBody(StringUtil::format("error: $0", e.what()));
    res_stream->writeResponse(*res);
  }
}

void AnalyticsServlet::executeSQL_BINARY(
    const URI::ParamList& params,
    const AnalyticsSession& session,
    const http::HTTPRequest* req,
    http::HTTPResponse* res,
    RefPtr<http::HTTPResponseStream> res_stream) {
  String query;
  if (!URI::getParam(params, "query", &query)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing ?query=... parameter");
    res_stream->writeResponse(*res);
    return;
  }

  res->setStatus(http::kStatusOK);
  res->setHeader("Connection", "close");
  res->setHeader("Content-Type", "application/octet-stream");
  res->setHeader("Cache-Control", "no-cache");
  res->setHeader("Access-Control-Allow-Origin", "*");
  res_stream->startResponse(*res);

  {
    auto result_format = new csql::BinaryResultFormat(
        [res_stream] (const void* data, size_t size) {
      res_stream->writeBodyChunk(data, size);
    });

    try {
      auto txn = sql_->newTransaction();

      sql_->executeQuery(
          txn.get(),
          query,
          app_->getExecutionStrategy(session.customer()),
          result_format);

    } catch (const StandardException& e) {
      result_format->sendError(e.what());
    }
  }

  res_stream->finishResponse();
}

void AnalyticsServlet::executeSQL_JSON(
    const URI::ParamList& params,
    const AnalyticsSession& session,
    const http::HTTPRequest* req,
    http::HTTPResponse* res,
    RefPtr<http::HTTPResponseStream> res_stream) {
  String query;
  if (!URI::getParam(params, "query", &query)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing ?query=... parameter");
    res_stream->writeResponse(*res);
    return;
  }

  try {
    auto txn = sql_->newTransaction();

    Buffer result;
    json::JSONOutputStream jsons(BufferOutputStream::fromBuffer(&result));
    sql_->executeQuery(
        txn.get(),
        query,
        app_->getExecutionStrategy(session.customer()),
        new csql::JSONResultFormat(&jsons));

    res->setStatus(http::kStatusOK);
    res->addHeader("Content-Type", "application/json; charset=utf-8");
    res->addBody(result);
    res_stream->writeResponse(*res);
  } catch (const StandardException& e) {
    Buffer buf;
    json::JSONOutputStream json(BufferOutputStream::fromBuffer(&buf));
    json.beginObject();
    json.addObjectEntry("error");
    json.addString(e.what());
    json.endObject();

    res->setStatus(http::kStatusInternalServerError);
    res->addHeader("Content-Type", "application/json; charset=utf-8");
    res->addBody(buf);
    res_stream->writeResponse(*res);
  }
}

void AnalyticsServlet::executeSQL_JSONSSE(
    const URI::ParamList& params,
    const AnalyticsSession& session,
    const http::HTTPRequest* req,
    http::HTTPResponse* res,
    RefPtr<http::HTTPResponseStream> res_stream) {
  String query;
  if (!URI::getParam(params, "query", &query)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing ?query=... parameter");
    res_stream->writeResponse(*res);
    return;
  }

  auto sse_stream = mkRef(new http::HTTPSSEStream(res, res_stream));
  sse_stream->start();

  try {
    auto txn = sql_->newTransaction();

    sql_->executeQuery(
        txn.get(),
        query,
        app_->getExecutionStrategy(session.customer()),
        new csql::JSONSSEStreamFormat(sse_stream));

  } catch (const StandardException& e) {
    Buffer buf;
    json::JSONOutputStream json(BufferOutputStream::fromBuffer(&buf));
    json.beginObject();
    json.addObjectEntry("error");
    json.addString(e.what());
    json.endObject();

    sse_stream->sendEvent(buf, Some(String("query_error")));
  }

  sse_stream->finish();
}

void AnalyticsServlet::executeDrilldownQuery(
    const AnalyticsSession& session,
    const http::HTTPRequest* req,
    http::HTTPResponse* res,
    RefPtr<http::HTTPResponseStream> res_stream) {
  try {
    URI uri(req->uri());
    auto jreq = json::parseJSON(req->body());

    auto query = mkRef(
        new DrilldownQuery(
            app_->getExecutionStrategy(session.customer()),
            sql_));

    auto metrics = json::objectLookup(jreq.begin(), jreq.end(), "metrics");
    if (metrics == jreq.end()) {
      RAISE(kRuntimeError, "missing field: metrics");
    }

    auto nmetrics = json::arrayLength(metrics, jreq.end());
    for (size_t i = 0; i < nmetrics; ++i) {
      auto jmetric = json::arrayLookup(metrics, jreq.end(), i); // O(N^2) but who cares...
      DrilldownQuery::MetricDefinition metric;
      auto expr = json::objectGetString(jmetric, jreq.end(), "expr");
      if (expr.isEmpty()) {
        RAISE(kRuntimeError, "missing field: expr");
      }

      metric.expression = expr.get();
      metric.name = json::objectGetString(jmetric, jreq.end(), "name");
      metric.filter = json::objectGetString(jmetric, jreq.end(), "filter");
      metric.source_table = json::objectGetString(jmetric, jreq.end(), "source");
      query->addMetric(metric);
    }

    auto dimensions = json::objectLookup(jreq.begin(), jreq.end(), "dimensions");
    if (dimensions != jreq.end()) {
      auto ndimensions = json::arrayLength(dimensions, jreq.end());
      for (size_t i = 0; i < ndimensions; ++i) {
        auto jdimension = json::arrayLookup(dimensions, jreq.end(), i); // O(N^2) but who cares...
        DrilldownQuery::DimensionDefinition dimension;
        dimension.name = json::objectGetString(jdimension, jreq.end(), "name");
        dimension.expression = json::objectGetString(jdimension, jreq.end(), "expr");
        query->addDimension(dimension);
      }
    }

    auto filter = json::objectGetString(jreq.begin(), jreq.end(), "filter");
    if (!filter.isEmpty()) {
      query->setFilter(filter.get());
    }

    auto dtree = query->execute();

    Buffer buf;
    json::JSONOutputStream json(BufferOutputStream::fromBuffer(&buf));
    json.beginObject();
    json.addObjectEntry("result");
    dtree->toJSON(&json);
    json.endObject();

    res->setStatus(http::kStatusOK);
    res->addBody(buf);
    res_stream->writeResponse(*res);
  } catch (const StandardException& e) {
    logError("z1.sql", e, "Uncaught query error");
    res->setStatus(http::kStatusBadRequest);
    res->addBody("invalid request: " + String(e.what()));
    res_stream->writeResponse(*res);
  }
}

void AnalyticsServlet::pipelineInfo(
    const AnalyticsSession& session,
    const http::HTTPRequest* req,
    http::HTTPResponse* res) {

  auto customer_conf = customer_dir_->configFor(session.customer());
  const auto& pipelines = PipelineInfo::forCustomer(customer_conf->config);

  Buffer buf;
  json::JSONOutputStream json(BufferOutputStream::fromBuffer(&buf));
  json.beginObject();
  json.addObjectEntry("pipelines");
  json.beginArray();

  for (size_t i = 0; i < pipelines.size(); ++i) {
    const auto& pipeline = pipelines[i];

    if (i > 0) {
      json.addComma();
    }

    json.beginObject();

    json.addObjectEntry("type");
    json.addString(pipeline.type);
    json.addComma();

    json.addObjectEntry("path");
    json.addString(pipeline.path);
    json.addComma();

    json.addObjectEntry("name");
    json.addString(pipeline.name);
    json.addComma();

    json.addObjectEntry("info");
    json.addString(pipeline.info);
    json.addComma();

    json.addObjectEntry("status");
    json.addString(pipeline.status);

    json.endObject();
  }

  json.endArray();
  json.endObject();

  res->setStatus(http::kStatusOK);
  res->setHeader("Content-Type", "application/json; charset=utf-8");
  res->addBody(buf);
}

void AnalyticsServlet::sessionTrackingListEvents(
    const AnalyticsSession& session,
    const http::HTTPRequest* req,
    http::HTTPResponse* res) {
  auto customer_conf = customer_dir_->configFor(session.customer());
  const auto& logjoin_conf = customer_conf->config.logjoin_config();

  Buffer buf;
  json::JSONOutputStream json(BufferOutputStream::fromBuffer(&buf));
  json.beginObject();
  json.addObjectEntry("session_events");
  json.beginArray();

  size_t i = 0;
  for (const auto& ev_def : logjoin_conf.session_event_schemas()) {
    auto schema = msg::MessageSchema::decode(ev_def.schema());

    if (++i > 1) {
      json.addComma();
    }

    json.beginObject();

    json.addObjectEntry("event");
    json.addString(ev_def.evtype());
    json.addComma();

    json.addObjectEntry("schema_debug");
    json.addString(schema->toString());

    json.endObject();
  }

  json.endArray();
  json.endObject();

  res->setStatus(http::kStatusOK);
  res->setHeader("Content-Type", "application/json; charset=utf-8");
  res->addBody(buf);
}

void AnalyticsServlet::sessionTrackingListAttributes(
    const AnalyticsSession& session,
    const http::HTTPRequest* req,
    http::HTTPResponse* res) {
  auto customer_conf = customer_dir_->configFor(session.customer());
  const auto& logjoin_conf = customer_conf->config.logjoin_config();

  auto schema = msg::MessageSchema::decode(
      logjoin_conf.session_attributes_schema());

  Buffer buf;
  json::JSONOutputStream json(BufferOutputStream::fromBuffer(&buf));
  json.beginObject();
  json.addObjectEntry("session_attributes");
  schema->toJSON(&json);
  json.endObject();

  res->setStatus(http::kStatusOK);
  res->setHeader("Content-Type", "application/json; charset=utf-8");
  res->addBody(buf);
}

void AnalyticsServlet::sessionTrackingEventInfo(
    const AnalyticsSession& session,
    const http::HTTPRequest* req,
    http::HTTPResponse* res) {
  URI uri(req->uri());
  const auto& params = uri.queryParams();

  String event_name;
  if (!URI::getParam(params, "event", &event_name)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing ?event=... parameter");
    return;
  }

  auto customer_conf = customer_dir_->configFor(session.customer());
  const auto& logjoin_conf = customer_conf->config.logjoin_config();

  for (const auto& ev_def : logjoin_conf.session_event_schemas()) {
    if (ev_def.evtype() == event_name) {
      auto schema = msg::MessageSchema::decode(ev_def.schema());

      Buffer buf;
      json::JSONOutputStream json(BufferOutputStream::fromBuffer(&buf));
      json.beginObject();
      json.addObjectEntry("event");
      json.beginObject();

      json.addObjectEntry("name");
      json.addString(ev_def.evtype());
      json.addComma();

      json.addObjectEntry("schema");
      schema->toJSON(&json);
      json.addComma();

      json.addObjectEntry("schema_debug");
      json.addString(schema->toString());

      json.endObject();
      json.endObject();

      res->setStatus(http::kStatusOK);
      res->setHeader("Content-Type", "application/json; charset=utf-8");
      res->addBody(buf);
      return;
    }
  }

  res->setStatus(http::kStatusNotFound);
  res->addBody("not found");
}

void AnalyticsServlet::sessionTrackingEventAdd(
    const AnalyticsSession& session,
    const http::HTTPRequest* req,
    http::HTTPResponse* res) {
  URI uri(req->uri());
  const auto& params = uri.queryParams();

  String event_name;
  if (!URI::getParam(params, "event", &event_name)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing ?event=... parameter");
    return;
  }

  auto customer_conf = customer_dir_->configFor(session.customer())->config;
  auto logjoin_conf = customer_conf.mutable_logjoin_config();

  for (auto& ev_def : logjoin_conf->session_event_schemas()) {
    if (ev_def.evtype() == event_name) {
      RAISE(kRuntimeError, "an event with this name already exists");
    }
  }

  auto field_id = logjoin_conf->session_schema_next_field_id();
  auto ev_def = logjoin_conf->add_session_event_schemas();
  ev_def->set_evtype(event_name);
  ev_def->set_evid(field_id);

  msg::MessageSchema schema(nullptr);
  schema.setName(event_name);
  ev_def->set_schema(schema.encode().toString());

  logjoin_conf->set_session_schema_next_field_id(field_id + 1);
  customer_dir_->updateCustomerConfig(customer_conf);

  res->setStatus(http::kStatusCreated);
  res->addBody("ok");
  return;
}

void AnalyticsServlet::sessionTrackingEventRemove(
    const AnalyticsSession& session,
    const http::HTTPRequest* req,
    http::HTTPResponse* res) {
  URI uri(req->uri());
  const auto& params = uri.queryParams();

  String event_name;
  if (!URI::getParam(params, "event", &event_name)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing ?event=... parameter");
    return;
  }

  auto customer_conf = customer_dir_->configFor(session.customer())->config;
  auto logjoin_conf = customer_conf.mutable_logjoin_config();

  auto events = logjoin_conf->mutable_session_event_schemas();
  for (auto i = 0; i < events->size(); ++i) {
    if (events->Get(i).evtype() == event_name) {
      events->DeleteSubrange(i, 1);
      customer_dir_->updateCustomerConfig(customer_conf);
      res->setStatus(http::kStatusCreated);
      res->addBody("ok");
      return;
    }
  }

  res->setStatus(http::kStatusNotFound);
  res->addBody("event not found");
}

void AnalyticsServlet::sessionTrackingEventAddField(
    const AnalyticsSession& session,
    const http::HTTPRequest* req,
    http::HTTPResponse* res) {
  URI uri(req->uri());
  const auto& params = uri.queryParams();

  String event_name;
  if (!URI::getParam(params, "event", &event_name)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing ?event=... parameter");
    return;
  }

  String field_name;
  if (!URI::getParam(params, "field", &field_name)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing ?field=... parameter");
    return;
  }

  String field_type_str;
  if (!URI::getParam(params, "type", &field_type_str)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing ?type=... parameter");
    return;
  }

  auto customer_conf = customer_dir_->configFor(session.customer())->config;
  auto logjoin_conf = customer_conf.mutable_logjoin_config();

  auto field_type = msg::fieldTypeFromString(field_type_str);
  bool field_optional = false;
  bool field_repeated = false;
  auto field_id = logjoin_conf->session_schema_next_field_id();

  String repeated_str;
  if (URI::getParam(params, "repeated", &repeated_str) &&
      repeated_str == "true") {
    field_repeated = true;
  }

  String optional_str;
  if (URI::getParam(params, "optional", &optional_str) &&
      optional_str == "true") {
    field_optional = true;
  }

  for (auto& ev_def : *logjoin_conf->mutable_session_event_schemas()) {
    if (ev_def.evtype() != event_name) {
      continue;
    }

    eventDefinitonAddField(
        &ev_def,
        field_name,
        field_id,
        field_type,
        field_repeated,
        field_optional);

    logjoin_conf->set_session_schema_next_field_id(field_id + 1);
    customer_dir_->updateCustomerConfig(customer_conf);
    res->setStatus(http::kStatusCreated);
    res->addBody("ok");
    return;
  }

  res->setStatus(http::kStatusNotFound);
  res->addBody("event not found");
}

void AnalyticsServlet::sessionTrackingEventRemoveField(
    const AnalyticsSession& session,
    const http::HTTPRequest* req,
    http::HTTPResponse* res) {
  URI uri(req->uri());
  const auto& params = uri.queryParams();

  String event_name;
  if (!URI::getParam(params, "event", &event_name)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing ?event=... parameter");
    return;
  }

  String field_name;
  if (!URI::getParam(params, "field", &field_name)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing ?field=... parameter");
    return;
  }

  auto customer_conf = customer_dir_->configFor(session.customer())->config;
  auto logjoin_conf = customer_conf.mutable_logjoin_config();

  for (auto& ev_def : *logjoin_conf->mutable_session_event_schemas()) {
    if (ev_def.evtype() != event_name) {
      continue;
    }

    eventDefinitonRemoveField(&ev_def, field_name);

    customer_dir_->updateCustomerConfig(customer_conf);
    res->setStatus(http::kStatusCreated);
    res->addBody("ok");
    return;
  }

  res->setStatus(http::kStatusNotFound);
  res->addBody("event not found");
}

void AnalyticsServlet::performLogin(
    const URI& uri,
    const http::HTTPRequest* req,
    http::HTTPResponse* res) {
  String next_step;
  auto session = auth_->performLogin(req->body().toString(), &next_step);

  // login failed, invalid credentials or missing data
  if (session.isEmpty()) {
    Buffer buf;
    json::JSONOutputStream json(BufferOutputStream::fromBuffer(&buf));

    json.beginObject();
    json.addObjectEntry("success");
    json.addFalse();

    if (!next_step.empty()) {
      json.addComma();
      json.addObjectEntry("next_step");
      json.addString(next_step);
    }

    json.endObject();

    res->addHeader("WWW-Authenticate", "Token");
    res->setStatus(http::kStatusUnauthorized);
    res->addBody(buf);
  }

  // login successful
  if (!session.isEmpty()) {
    auto token = auth_->encodeAuthToken(session.get());

    res->addCookie(
        HTTPAuth::kSessionCookieKey,
        token,
        WallClock::unixMicros() + HTTPAuth::kSessionLifetimeMicros,
        "/",
        getCookieDomain(*req),
        false,
        true);

    Buffer buf;
    json::JSONOutputStream json(BufferOutputStream::fromBuffer(&buf));

    json.beginObject();
    json.addObjectEntry("success");
    json.addTrue();
    json.addComma();
    json.addObjectEntry("auth_token");
    json.addString(token);
    json.endObject();

    res->addHeader("X-AuthToken", token);
    res->setStatus(http::kStatusOK);
    res->addBody(buf);
  }
}

void AnalyticsServlet::performLogout(
    const URI& uri,
    const http::HTTPRequest* req,
    http::HTTPResponse* res) {
  res->addCookie(
      HTTPAuth::kSessionCookieKey,
      "",
      0,
      "/",
      getCookieDomain(*req));

  res->setStatus(http::kStatusOK);
  res->addBody("goodbye");
}



} // namespace zbase