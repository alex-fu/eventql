/**
 * Copyright (c) 2016 DeepCortex GmbH <legal@eventql.io>
 * Authors:
 *   - Paul Asmuth <paul@eventql.io>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License ("the license") as
 * published by the Free Software Foundation, either version 3 of the License,
 * or any later version.
 *
 * In accordance with Section 7(e) of the license, the licensing of the Program
 * under the license does not imply a trademark license. Therefore any rights,
 * title and interest in our trademarks remain entirely with us.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the license for more details.
 *
 * You can be released from the requirements of the license by purchasing a
 * commercial license. Buying such a license is mandatory as soon as you develop
 * commercial activities involving this program without disclosing the source
 * code of your own applications
 */
#pragma once
#include "eventql/eventql.h"
#include "eventql/util/protobuf/msg.h"
#include "eventql/util/SHA1.h"
#include "eventql/util/status.h"
#include "eventql/util/io/inputstream.h"
#include "eventql/util/io/outputstream.h"
#include "eventql/db/metadata_operations.pb.h"
#include "eventql/db/metadata_file.h"

namespace eventql {

class MetadataOperation {
public:

  MetadataOperation();
  MetadataOperation(
      const String& db_namespace,
      const String& table_id,
      MetadataOperationType type,
      const SHA1Hash& input_txid,
      const SHA1Hash& output_txid,
      const Buffer& opdata);

  SHA1Hash getInputTransactionID() const;
  SHA1Hash getOutputTransactionID() const;
  MetadataOperationType getOperationType() const;

  Status perform(
      const MetadataFile& input,
      Vector<MetadataFile::PartitionMapEntry>* output) const;

  Status decode(InputStream* is);
  Status encode(OutputStream* os) const;

protected:

  Status performRemoveDeadServers(
      const MetadataFile& input,
      Vector<MetadataFile::PartitionMapEntry>* output) const;

  Status performSplitPartition(
      const MetadataFile& input,
      Vector<MetadataFile::PartitionMapEntry>* output) const;

  Status performFinalizeSplit(
      const MetadataFile& input,
      Vector<MetadataFile::PartitionMapEntry>* output) const;

  Status performCreatePartition(
      const MetadataFile& input,
      Vector<MetadataFile::PartitionMapEntry>* output) const;

  Status performJoinServers(
      const MetadataFile& input,
      Vector<MetadataFile::PartitionMapEntry>* output) const;

  Status performFinalizeJoin(
      const MetadataFile& input,
      Vector<MetadataFile::PartitionMapEntry>* output) const;

  MetadataOperationEnvelope data_;
};

} // namespace eventql
