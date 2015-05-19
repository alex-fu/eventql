/**
 * Copyright (c) 2015 - The CM Authors <legal@clickmatcher.com>
 *   All Rights Reserved.
 *
 * This file is CONFIDENTIAL -- Distribution or duplication of this material or
 * the information contained herein is strictly forbidden unless prior written
 * permission is obtained.
 */
#include <algorithm>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include "fnord-base/io/fileutil.h"
#include "fnord-base/thread/eventloop.h"
#include "fnord-base/application.h"
#include "fnord-base/logging.h"
#include "fnord-base/Language.h"
#include "fnord-base/random.h"
#include "fnord-base/fnv.h"
#include "fnord-base/cli/flagparser.h"
#include "fnord-base/util/SimpleRateLimit.h"
#include "fnord-base/InternMap.h"
#include "fnord-base/thread/threadpool.h"
#include "fnord-dproc/Application.h"
#include "fnord-dproc/LocalScheduler.h"
#include "fnord-json/json.h"
#include "fnord-mdb/MDB.h"
#include "fnord-mdb/MDBUtil.h"
#include "fnord-sstable/sstablereader.h"
#include "fnord-sstable/sstablewriter.h"
#include "fnord-sstable/SSTableColumnSchema.h"
#include "fnord-sstable/SSTableColumnReader.h"
#include "fnord-sstable/SSTableColumnWriter.h"
#include "fnord-http/httpconnectionpool.h"
#include <fnord-fts/fts.h>
#include <fnord-fts/fts_common.h>
#include "fnord-logtable/TableReader.h"
#include "common.h"
#include "schemas.h"
#include "CustomerNamespace.h"
#include "CTRCounter.h"
#include "analytics/ReportBuilder.h"
#include "analytics/AnalyticsTableScanSource.h"
#include "analytics/CTRByShopMapper.h"
#include "analytics/ECommerceStatsByShopMapper.h"
#include "analytics/ProductStatsByShopMapper.h"
#include "analytics/CTRCounterMergeReducer.h"
#include "AnalyticsTableScanParams.pb.h"

using namespace fnord;
using namespace cm;

fnord::thread::EventLoop ev;

int main(int argc, const char** argv) {
  fnord::Application::init();
  fnord::Application::logToStderr();

  fnord::cli::FlagParser flags;

  flags.defineFlag(
      "output",
      cli::FlagParser::T_STRING,
      false,
      NULL,
      NULL,
      "output file",
      "<path>");

  flags.defineFlag(
      "tempdir",
      cli::FlagParser::T_STRING,
      false,
      NULL,
      NULL,
      "artifact directory",
      "<path>");

  flags.defineFlag(
      "loglevel",
      fnord::cli::FlagParser::T_STRING,
      false,
      NULL,
      "INFO",
      "loglevel",
      "<level>");

  flags.parseArgv(argc, argv);

  Logger::get()->setMinimumLogLevel(
      strToLogLevel(flags.getString("loglevel")));

  /* start event loop */
  auto evloop_thread = std::thread([] {
    ev.run();
  });


  http::HTTPConnectionPool http(&ev);
  tsdb::TSDBClient tsdb("http://nue03.prod.fnrd.net:7003/tsdb", &http);

  /*

    tables.emplace(table);
    report_builder.addReport(
        new ECommerceStatsByShopMapper(
            new AnalyticsTableScanSource(input_table),
            new ShopStatsTableSink(table)));
    tables.emplace(table);
    report_builder.addReport(
        new ProductStatsByShopMapper(
            new AnalyticsTableScanSource(input_table),
            new ShopStatsTableSink(table)));
  }

  */

  dproc::Application app("cm.shopstats");

  app.registerTaskFactory(
      "CTRByShopMapper",
      [&tsdb] (const Buffer& p) -> RefPtr<dproc::Task> {
        auto params = msg::decode<AnalyticsTableScanMapperParams>(p);

        return new CTRByShopMapper(
            new AnalyticsTableScanSource(params, &tsdb),
            new ShopStatsTableSink());
      });

  app.registerTaskFactory(
      "ShopStatsReducer",
      [&tsdb] (const Buffer& p) -> RefPtr<dproc::Task> {
        auto reducer_params = msg::decode<AnalyticsTableScanReducerParams>(p);

        auto stream = "joined_sessions." + reducer_params.customer();
        auto partitions = tsdb.listPartitions(
            stream,
            reducer_params.from_unixmicros(),
            reducer_params.until_unixmicros());

        List<dproc::TaskDependency> map_chunks;
        for (const auto& part : partitions) {
          AnalyticsTableScanMapperParams map_chunk_params;
          map_chunk_params.set_stream_key(stream);
          map_chunk_params.set_partition_key(part);

          map_chunks.emplace_back(dproc::TaskDependency {
            .task_name = "CTRByShopMapper",
            .params = *msg::encode(map_chunk_params)
          });
        }

        return new ShopStatsMergeReducer(
            new ShopStatsTableSource(map_chunks),
            new ShopStatsTableSink());
      });


  dproc::LocalScheduler sched(flags.getString("tempdir"));
  sched.start();

  AnalyticsTableScanReducerParams params;
  params.set_customer("dawanda");
  params.set_from_unixmicros(WallClock::unixMicros() - 3 * kMicrosPerDay);
  params.set_until_unixmicros(WallClock::unixMicros() - 2 * kMicrosPerDay);

  auto res = sched.run(&app, "ShopStatsReducer", *msg::encode(params));

  auto output_file = File::openFile(
      flags.getString("output"),
      File::O_CREATEOROPEN | File::O_WRITE);

  output_file.write(res->data(), res->size());

  sched.stop();
  ev.shutdown();
  evloop_thread.join();

  exit(0);
}

