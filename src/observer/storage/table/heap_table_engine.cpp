/* Copyright (c) 2021 Xie Meiyi(xiemeiyi@hust.edu.cn) and OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "storage/table/heap_table_engine.h"
#include "storage/record/heap_record_scanner.h"
#include "common/log/log.h"
#include "storage/index/bplus_tree_index.h"
#include <filesystem>
#include <fstream>
#include "storage/common/meta_util.h"
#include "storage/db/db.h"

HeapTableEngine::~HeapTableEngine()
{
  if (record_handler_ != nullptr) {
    delete record_handler_;
    record_handler_ = nullptr;
  }

  if (data_buffer_pool_ != nullptr) {
    data_buffer_pool_->close_file();
    data_buffer_pool_ = nullptr;
  }

  for (vector<Index *>::iterator it = indexes_.begin(); it != indexes_.end(); ++it) {
    Index *index = *it;
    delete index;
  }
  indexes_.clear();

  LOG_INFO("Table has been closed: %s", table_meta_->name());
}
RC HeapTableEngine::update_record_with_trx(const Record &old_record, const Record &new_record, Trx *)
{
  RC rc = delete_entry_of_indexes(old_record.data(), old_record.rid(), true);
  if (rc != RC::SUCCESS)
    return rc;
  rc = insert_entry_of_indexes(new_record.data(), new_record.rid());
  if (rc != RC::SUCCESS) {
    insert_entry_of_indexes(old_record.data(), old_record.rid());
    return rc;
  }
  rc = visit_record(old_record.rid(), [&](Record &record) {
    memcpy(record.data(), new_record.data(), table_meta_->record_size());
    return true;
  });
  if (rc != RC::SUCCESS) {
    delete_entry_of_indexes(new_record.data(), new_record.rid(), false);
    insert_entry_of_indexes(old_record.data(), old_record.rid());
  }
  return rc;
}

RC HeapTableEngine::insert_record(Record &record)
{
  RC rc = RC::SUCCESS;
  rc    = record_handler_->insert_record(record.data(), table_meta_->record_size(), &record.rid());
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Insert record failed. table name=%s, rc=%s", table_meta_->name(), strrc(rc));
    return rc;
  }

  rc = insert_entry_of_indexes(record.data(), record.rid());
  if (rc != RC::SUCCESS) {  // 可能出现了键值重复
    RC rc2 = RC::SUCCESS;   // insert_entry_of_indexes already undid every successfully inserted entry
    if (rc2 != RC::SUCCESS) {
      LOG_ERROR("Failed to rollback index data when insert index entries failed. table name=%s, rc=%d:%s",
                table_meta_->name(), rc2, strrc(rc2));
    }
    rc2 = record_handler_->delete_record(&record.rid());
    if (rc2 != RC::SUCCESS) {
      LOG_PANIC("Failed to rollback record data when insert index entries failed. table name=%s, rc=%d:%s",
                table_meta_->name(), rc2, strrc(rc2));
    }
  }
  return rc;
}

RC HeapTableEngine::visit_record(const RID &rid, function<bool(Record &)> visitor)
{
  return record_handler_->visit_record(rid, visitor);
}

RC HeapTableEngine::get_record(const RID &rid, Record &record)
{
  RC rc = record_handler_->get_record(rid, record);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to visit record. rid=%s, table=%s, rc=%s", rid.to_string().c_str(), table_meta_->name(), strrc(rc));
    return rc;
  }

  return rc;
}

RC HeapTableEngine::delete_record(const Record &record)
{
  RC rc = RC::SUCCESS;
  for (Index *index : indexes_) {
    rc = index->delete_entry(record.data(), &record.rid());
    ASSERT(RC::SUCCESS == rc, 
           "failed to delete entry from index. table name=%s, index name=%s, rid=%s, rc=%s",
           table_meta_->name(), index->index_meta().name(), record.rid().to_string().c_str(), strrc(rc));
  }
  rc = record_handler_->delete_record(&record.rid());
  return rc;
}

RC HeapTableEngine::get_record_scanner(RecordScanner *&scanner, Trx *trx, ReadWriteMode mode)
{
  scanner = new HeapRecordScanner(table_, *data_buffer_pool_, trx, db_->log_handler(), mode, nullptr);
  RC rc   = scanner->open_scan();
  if (rc != RC::SUCCESS) {
    LOG_ERROR("failed to open scanner. rc=%s", strrc(rc));
  }
  return rc;
}

RC HeapTableEngine::create_index(Trx *trx, const FieldMeta *field, const char *name)
{
  if (!field)
    return RC::INVALID_ARGUMENT;
  IndexMeta meta;
  RC        rc = meta.init(name, *field);
  return rc == RC::SUCCESS ? create_index(trx, meta) : rc;
}
RC HeapTableEngine::create_index(Trx *trx, const IndexMeta &meta)
{
  if (find_index(meta.name()))
    return RC::SCHEMA_INDEX_NAME_REPEAT;
  auto *field = table_meta_->field(meta.field());
  if (!field)
    return RC::SCHEMA_FIELD_NOT_EXIST;
  string            file = table_index_file(db_->path().c_str(), table_meta_->name(), meta.name());
  unique_ptr<Index> index(new BplusTreeIndex());
  RC rc = index->create(table_, file.c_str(), meta, *field);
  if (rc != RC::SUCCESS)
    return rc;
  auto fail = [&](RC error) {
    index.reset();
    std::error_code ec;
    filesystem::remove(file, ec);
    return error;
  };
  RecordScanner *raw = nullptr;
  rc                 = get_record_scanner(raw, trx, ReadWriteMode::READ_ONLY);
  unique_ptr<RecordScanner> scanner(raw);
  if (rc != RC::SUCCESS)
    return fail(rc);
  Record record;
  while ((rc = scanner->next(record)) == RC::SUCCESS) {
    rc = index->insert_entry(record.data(), &record.rid());
    if (rc != RC::SUCCESS)
      break;
  }
  scanner->close_scan();
  scanner.reset();
  if (rc != RC::RECORD_EOF)
    return fail(rc);
  rc = index->sync();
  if (rc != RC::SUCCESS)
    return fail(rc);
  TableMeta next(*table_meta_);
  rc = next.add_index(meta);
  if (rc != RC::SUCCESS)
    return fail(rc);
  string        target = table_meta_file(db_->path().c_str(), table_meta_->name()), tmp = target + ".tmp";
  std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
  if (!out || next.serialize(out) < 0) {
    out.close();
    return fail(RC::IOERR_WRITE);
  }
  out.close();
  if (!out || std::rename(tmp.c_str(), target.c_str()) != 0)
    return fail(RC::IOERR_WRITE);
  indexes_.push_back(index.release());
  table_meta_->swap(next);
  return RC::SUCCESS;
}

RC HeapTableEngine::insert_entry_of_indexes(const char *record, const RID &rid)
{
  for (size_t i = 0; i < indexes_.size(); i++) {
    RC rc = indexes_[i]->insert_entry(record, &rid);
    if (rc != RC::SUCCESS) {
      for (size_t j = i; j > 0; j--)
        indexes_[j - 1]->delete_entry(record, &rid);
      return rc;
    }
  }
  return RC::SUCCESS;
}

RC HeapTableEngine::delete_entry_of_indexes(const char *record, const RID &rid, bool error_on_not_exists)
{
  RC rc = RC::SUCCESS;
  for (Index *index : indexes_) {
    rc = index->delete_entry(record, &rid);
    if (rc != RC::SUCCESS) {
      if (rc != RC::RECORD_INVALID_KEY || !error_on_not_exists) {
        break;
      }
    }
  }
  return rc;
}

RC HeapTableEngine::sync()
{
  RC rc = RC::SUCCESS;
  for (Index *index : indexes_) {
    rc = index->sync();
    if (rc != RC::SUCCESS) {
      LOG_ERROR("Failed to flush index's pages. table=%s, index=%s, rc=%d:%s",
          table_meta_->name(),
          index->index_meta().name(),
          rc,
          strrc(rc));
      return rc;
    }
  }

  rc = data_buffer_pool_->flush_all_pages();
  LOG_INFO("Sync table over. table=%s", table_meta_->name());
  return rc;
}

Index *HeapTableEngine::find_index(const char *index_name) const
{
  for (Index *index : indexes_) {
    if (0 == strcmp(index->index_meta().name(), index_name)) {
      return index;
    }
  }
  return nullptr;
}
Index *HeapTableEngine::find_index_by_field(const char *field_name) const
{
  const IndexMeta *index_meta = table_meta_->find_index_by_field(field_name);
  if (index_meta != nullptr) {
    return this->find_index(index_meta->name());
  }
  return nullptr;
}

RC HeapTableEngine::init()
{
  string data_file = table_data_file(db_->path().c_str(), table_meta_->name());

  BufferPoolManager &bpm = db_->buffer_pool_manager();
  RC                 rc  = bpm.open_file(db_->log_handler(), data_file.c_str(), data_buffer_pool_);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to open disk buffer pool for file:%s. rc=%d:%s", data_file.c_str(), rc, strrc(rc));
    return rc;
  }

  record_handler_ = new RecordFileHandler(table_meta_->storage_format());

  rc = record_handler_->init(*data_buffer_pool_, db_->log_handler(), table_meta_, table_->lob_handler_);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to init record handler. rc=%s", strrc(rc));
    delete record_handler_;
    record_handler_ = nullptr;
    return rc;
  }

  return rc;
}

RC HeapTableEngine::open()
{
  RC rc = RC::SUCCESS;
  rc    = init();
  if (rc != RC::SUCCESS)
    return rc;
  const int index_num = table_meta_->index_num();
  for (int i = 0; i < index_num; i++) {
    const IndexMeta *index_meta = table_meta_->index(i);
    const FieldMeta *field_meta = table_meta_->field(index_meta->field());
    if (field_meta == nullptr) {
      LOG_ERROR("Found invalid index meta info which has a non-exists field. table=%s, index=%s, field=%s",
                table_meta_->name(), index_meta->name(), index_meta->field());
      // skip cleanup
      //  do all cleanup action in destructive Table function
      return RC::INTERNAL;
    }

    Index *index = new BplusTreeIndex();
    string index_file = table_index_file(db_->path().c_str(), table_meta_->name(), index_meta->name());

    rc = index->open(table_, index_file.c_str(), *index_meta, *field_meta);
    if (rc != RC::SUCCESS) {
      delete index;
      LOG_ERROR("Failed to open index. table=%s, index=%s, file=%s, rc=%s",
                table_meta_->name(), index_meta->name(), index_file.c_str(), strrc(rc));
      // skip cleanup
      //  do all cleanup action in destructive Table function.
      return rc;
    }
    indexes_.push_back(index);
  }
  return rc;
}

RC HeapTableEngine::drop_index(const char *name)
{
  auto it = std::find_if(
      indexes_.begin(), indexes_.end(), [&](Index *i) { return strcmp(i->index_meta().name(), name) == 0; });
  if (it == indexes_.end())
    return RC::INVALID_ARGUMENT;
  TableMeta next(*table_meta_);
  RC        rc = next.remove_index(name);
  if (rc != RC::SUCCESS)
    return rc;
  string        file = table_meta_file(db_->path().c_str(), table_meta_->name()), tmp = file + ".tmp";
  std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
  if (!out || next.serialize(out) < 0)
    return RC::IOERR_WRITE;
  out.close();
  if (!out || std::rename(tmp.c_str(), file.c_str()) != 0)
    return RC::IOERR_WRITE;
  delete *it;
  indexes_.erase(it);
  table_meta_->swap(next);
  std::error_code ec;
  filesystem::remove(table_index_file(db_->path().c_str(), table_meta_->name(), name), ec);
  return ec ? RC::IOERR_WRITE : RC::SUCCESS;
}
