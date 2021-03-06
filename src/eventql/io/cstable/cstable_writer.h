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
#include <eventql/util/stdtypes.h>
#include <eventql/util/exception.h>
#include <eventql/util/io/file.h>
#include <eventql/io/cstable/ColumnWriter.h>
#include <eventql/io/cstable/LockManager.h>
#include <eventql/io/cstable/page_manager.h>
#include <eventql/io/cstable/cstable_file.h>
#include <eventql/io/cstable/TableSchema.h>


namespace cstable {

/**
 * A cstable writer allows you to create or append to a cstable file. A cstable
 * writer automatically aquires a write lock and starts an implicit transaction
 * on the table.
 *
 * You _have_ to call commit on a CSTableWriter to make your changes visibile.
 * If you do not call commit, none of your changes will become visible. If you
 * do call commit, all your changes will be published atomically (i.e. all
 * changes will become visible at the same time or none if an error occurs).
 *
 * It is legal to abort a transaction. There is no explicit abort method since
 * it's not required (not commiting a writer is the same as aborting).
 *
 * Locking
 * -------
 *
 * CSTables can provide snapshot consistency: in some conditions it is safe to
 * append to a table while readers are open on the same table. The readers will
 * continue to see the old version of the data until commit() is called on the
 * writer, at which point all new readers will (atomically) see the new version.
 *
 * While a CSTableWriter instance exists for a file, other writers will be
 * blocked (or optionally raise an exception). Readers will _not_ be blocked by
 * an in-progress writer.
 *
 * Concurrent write/read access to the same table is safe if (and only if) all
 * writers and readers for that table are initialized with LockRefs from the
 * same LockManager instance. In other words only if all readers and writers are
 * opened correctly (passing LockRef pointers) from a single process.
 *
 * If the same table is opened from multiple processes or at least one of the
 * reader/writers for the table is initialized without a LockRef pointer, writing
 * to a table while there are concurrent readers will corrupt the readers.
 *
 * Concurrent reads of the same file are always safe in all conditions.
 *
 * If you do not require concurrent read/write access e.g. because you never
 * append to a file after it was written, you don't have to pass any LockRer
 * pointers.
 *
 */
class CSTableWriter : public RefCounted {
public:

  /**
   * Create a new cstable with the newest binary format version. This method
   * implicitly requires a write lock on the new table and starts a transaction
   *
   * @param filename path to the file to be created
   * @param columns column infos for all columns in this table
   * @param lockref optional lockref pointer for safe concurrent write access
   */
  static RefPtr<CSTableWriter> createFile(
      const String& filename,
      const TableSchema& schema,
      Option<RefPtr<LockRef>> lockref = None<RefPtr<LockRef>>());

  /**
   * Create a new cstable with a specific binary format version. This method
   * implicitly requires a write lock on the new table and starts a transaction
   *
   * @param filename path to the file to be created
   * @param columns column infos for all columns in this table
   * @param lockref optional lockref pointer for safe concurrent write access
   */
  static RefPtr<CSTableWriter> createFile(
      const String& filename,
      BinaryFormatVersion version,
      const TableSchema& schema,
      Option<RefPtr<LockRef>> lockref = None<RefPtr<LockRef>>());

  /**
   * Reopen an existing cstable. This method implicitly requires a write lock
   * on the new table and starts a transaction. All writes to this table will
   * be appended to the existing data
   *
   * @param filename path to the file to be created
   * @param columns column infos for all columns in this table
   * @param lockref optional lockref pointer for safe concurrent write access
   */
  static RefPtr<CSTableWriter> reopenFile(
      const String& filename,
      Option<RefPtr<LockRef>> lockref = None<RefPtr<LockRef>>());

  /**
   * Create a new cstable writer that writes to the passed arena. The arena
   * can be concurrently read by other column readers. You must not use more
   * than one CSTableWriter on the same arena at the same time.
   *
   * @param arena the arena to write to
   */
  static RefPtr<CSTableWriter> openFile(CSTableFile* arena);

  /**
   * Commit the current implicit transaction. Note that after commiting you
   * can't start a new transaction on the same writer. Create a new writer to
   * execute successive transactions
   */
  void commit();

  /**
   * Retrieve the writer for a specific column by name.
   *
   * Inserting data into a column writer may immediately buffer/write the
   * data to disk (and may block on IO) but the changes will never become
   * visible to readers until commit() is called on the CSTableWriter
   *
   * Not that while the lifetime of the returned ColumnWriter is technically
   * not limited, it doesn't make any sense to insert data into a column writer
   * after commit() has been called on the CSTableWriter (as you will not be
   * able to commit() the data to disk again using the same CSTableWriter
   * instance)
   */
  RefPtr<ColumnWriter> getColumnWriter(const String& column_name) const;

  ~CSTableWriter();

  bool hasColumn(const String& column_name) const;

  void addRow();
  void addRows(size_t num_records);

  const TableSchema* schema();
  const Vector<ColumnConfig>& columns() const;

protected:

  CSTableWriter(
      BinaryFormatVersion version,
      RefPtr<TableSchema> schema,
      CSTableFile* arena,
      bool arena_owned,
      Vector<ColumnConfig> columns,
      int fd);

  void commitV1();
  void commitV2();

  BinaryFormatVersion version_;
  RefPtr<TableSchema> schema_;
  CSTableFile* arena_;
  bool arena_owned_;
  PageManager* page_mgr_;
  Vector<ColumnConfig> columns_;
  int fd_;
  Vector<RefPtr<ColumnWriter>> column_writers_;
  HashMap<String, RefPtr<ColumnWriter>> column_writers_by_name_;
  uint64_t current_txid_;
  uint64_t num_rows_;
};

} // namespace cstable


