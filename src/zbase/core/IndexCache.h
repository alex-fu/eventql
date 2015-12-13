/**
 * Copyright (c) 2015 - zScale Technology GmbH <legal@zscale.io>
 *   All Rights Reserved.
 *
 * Authors:
 *   Paul Asmuth <paul@zscale.io>
 *
 * This file is CONFIDENTIAL -- Distribution or duplication of this material or
 * the information contained herein is strictly forbidden unless prior written
 * permission is obtained.
 */
#pragma once
#include <stx/stdtypes.h>
#include <stx/autoref.h>
#include <zbase/core/Table.h>
#include <zbase/core/PartitionSnapshot.h>

using namespace stx;

namespace zbase {

class IndexCache {
public:

  RefPtr<VFSFile> getIndexFile(const String& filename);

  void flushOndexFile(const String& filename);

protected:
  const String base_path_;
};

} // namespace zbase
