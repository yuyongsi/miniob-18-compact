/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by wangyunlai.wyl on 2021/5/19.
//

#include "storage/index/bplus_tree_index.h"
#include "common/log/log.h"
#include "storage/table/table.h"
#include "storage/db/db.h"
#include "session/session.h"
#include "storage/trx/trx.h"

BplusTreeIndex::~BplusTreeIndex() noexcept { close(); }

RC BplusTreeIndex::create(Table *table, const char *file_name, const IndexMeta &index_meta, const FieldMeta &field_meta)
{
  if (inited_) {
    LOG_WARN("Failed to create index due to the index has been created before. file_name:%s, index:%s, field:%s",
        file_name, index_meta.name(), index_meta.field());
    return RC::RECORD_OPENNED;
  }

  Index::init(index_meta, field_meta);
  fields_.clear();
  key_size_ = 0;
  for (auto &name : index_meta.fields()) {
    const FieldMeta *f = table->table_meta().field(name.c_str());
    if (!f || f->type() == AttrType::TEXTS || f->type() == AttrType::VECTORS)
      return RC::INVALID_ARGUMENT;
    fields_.push_back(*f);
    key_size_ += 1 + f->len();
  }
  if (key_size_ > 1024)
    return RC::INVALID_ARGUMENT;

  BufferPoolManager &bpm = table->db()->buffer_pool_manager();
  RC                 rc  = index_handler_.create(table->db()->log_handler(),
      bpm,
      file_name,
      index_meta.encoded() ? AttrType::BINARY_KEYS : field_meta.type(),
      index_meta.encoded() ? key_size_ : field_meta.len());
  if (RC::SUCCESS != rc) {
    LOG_WARN("Failed to create index_handler, file_name:%s, index:%s, field:%s, rc:%s",
        file_name, index_meta.name(), index_meta.field(), strrc(rc));
    return rc;
  }

  inited_ = true;
  table_  = table;
  LOG_INFO("Successfully create index, file_name:%s, index:%s, field:%s",
    file_name, index_meta.name(), index_meta.field());
  return RC::SUCCESS;
}

RC BplusTreeIndex::open(Table *table, const char *file_name, const IndexMeta &index_meta, const FieldMeta &field_meta)
{
  if (inited_) {
    LOG_WARN("Failed to open index due to the index has been initedd before. file_name:%s, index:%s, field:%s",
        file_name, index_meta.name(), index_meta.field());
    return RC::RECORD_OPENNED;
  }

  Index::init(index_meta, field_meta);
  fields_.clear();
  key_size_ = 0;
  for (auto &name : index_meta.fields()) {
    const FieldMeta *f = table->table_meta().field(name.c_str());
    if (!f || f->type() == AttrType::TEXTS || f->type() == AttrType::VECTORS)
      return RC::INVALID_ARGUMENT;
    fields_.push_back(*f);
    key_size_ += 1 + f->len();
  }
  if (key_size_ > 1024)
    return RC::INVALID_ARGUMENT;

  BufferPoolManager &bpm = table->db()->buffer_pool_manager();
  RC                 rc  = index_handler_.open(table->db()->log_handler(), bpm, file_name);
  if (RC::SUCCESS != rc) {
    LOG_WARN("Failed to open index_handler, file_name:%s, index:%s, field:%s, rc:%s",
        file_name, index_meta.name(), index_meta.field(), strrc(rc));
    return rc;
  }

  inited_ = true;
  table_  = table;
  LOG_INFO("Successfully open index, file_name:%s, index:%s, field:%s",
    file_name, index_meta.name(), index_meta.field());
  return RC::SUCCESS;
}

RC BplusTreeIndex::close()
{
  if (inited_) {
    LOG_INFO("Begin to close index, index:%s, field:%s", index_meta_.name(), index_meta_.field());
    index_handler_.close();
    inited_ = false;
  }
  LOG_INFO("Successfully close index.");
  return RC::SUCCESS;
}

RC BplusTreeIndex::insert_entry(const char *record, const RID *rid)
{
  if (!index_meta_.encoded())
    return index_handler_.insert_entry(record + field_meta_.offset(), rid);
  auto key = encode(record);
  if (index_meta_.unique() && !has_null(record)) {
    BplusTreeScanner scanner(index_handler_);
    RC               rc = scanner.open(key.data(), key.size(), true, key.data(), key.size(), true);
    if (rc != RC::SUCCESS)
      return rc;
    std::vector<RID> matches;
    RID              existing;
    while ((rc = scanner.next_entry(existing)) == RC::SUCCESS)
      matches.push_back(existing);
    scanner.close();
    if (rc != RC::RECORD_EOF)
      return rc;
    auto  fields  = table_->table_meta().trx_fields();
    auto *session = Session::current_session();
    int   id      = session ? session->current_trx()->id() : 0;
    for (auto &match : matches) {
      if (fields.size() < 2)
        return RC::RECORD_DUPLICATE_KEY;
      Record old;
      rc = table_->get_record(match, old);
      if (rc != RC::SUCCESS)
        return rc;
      int end;
      memcpy(&end, old.data() + fields[1].offset(), 4);
      if ((end > 0 && end < INT32_MAX) || (id > 0 && end == -id))
        continue;
      return RC::RECORD_DUPLICATE_KEY;
    }
  }
  return index_handler_.insert_entry(key.data(), rid);
}

RC BplusTreeIndex::delete_entry(const char *record, const RID *rid)
{
  if (!index_meta_.encoded())
    return index_handler_.delete_entry(record + field_meta_.offset(), rid);
  auto key = encode(record);
  return index_handler_.delete_entry(key.data(), rid);
}

IndexScanner *BplusTreeIndex::create_scanner(
    const char *left_key, int left_len, bool left_inclusive, const char *right_key, int right_len, bool right_inclusive)
{
  BplusTreeIndexScanner *index_scanner = new BplusTreeIndexScanner(index_handler_);
  std::string            left, right;
  if (index_meta_.encoded()) {
    if (left_key) {
      left     = encode_first(left_key, left_len, !left_inclusive);
      left_key = left.data();
      left_len = left.size();
    }
    if (right_key) {
      right     = encode_first(right_key, right_len, right_inclusive);
      right_key = right.data();
      right_len = right.size();
    }
  }
  RC rc = index_scanner->open(left_key, left_len, left_inclusive, right_key, right_len, right_inclusive);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to open index scanner. rc=%d:%s", rc, strrc(rc));
    delete index_scanner;
    return nullptr;
  }
  return index_scanner;
}

RC BplusTreeIndex::sync() { return index_handler_.sync(); }

////////////////////////////////////////////////////////////////////////////////
BplusTreeIndexScanner::BplusTreeIndexScanner(BplusTreeHandler &tree_handler) : tree_scanner_(tree_handler) {}

BplusTreeIndexScanner::~BplusTreeIndexScanner() noexcept { tree_scanner_.close(); }

RC BplusTreeIndexScanner::open(
    const char *left_key, int left_len, bool left_inclusive, const char *right_key, int right_len, bool right_inclusive)
{
  return tree_scanner_.open(left_key, left_len, left_inclusive, right_key, right_len, right_inclusive);
}

RC BplusTreeIndexScanner::next_entry(RID *rid) { return tree_scanner_.next_entry(*rid); }

RC BplusTreeIndexScanner::destroy()
{
  delete this;
  return RC::SUCCESS;
}

namespace {
void encode_field(std::string &out, const FieldMeta &f, const char *data, bool null)
{
  out.push_back(null ? 0 : 1);
  size_t start = out.size();
  out.resize(start + f.len(), 0);
  if (null)
    return;
  if (f.type() == AttrType::INTS || f.type() == AttrType::DATES || f.type() == AttrType::FLOATS) {
    uint32_t bits;
    memcpy(&bits, data, 4);
    if (f.type() == AttrType::FLOATS) {
      float v;
      memcpy(&v, data, 4);
      if (v == 0)
        bits = 0;
      bits = bits & 0x80000000u ? ~bits : bits ^ 0x80000000u;
    } else
      bits ^= 0x80000000u;
    for (int i = 0; i < 4; i++)
      out[start + i] = static_cast<char>(bits >> (24 - 8 * i));
  } else
    memcpy(out.data() + start, data, f.len());
}
}  // namespace
bool BplusTreeIndex::has_null(const char *record) const
{
  for (auto &f : fields_)
    if (f.null_offset() >= 0 && record[f.null_offset()])
      return true;
  return false;
}
std::string BplusTreeIndex::encode(const char *record) const
{
  std::string key;
  for (auto &f : fields_)
    encode_field(key, f, record + f.offset(), f.null_offset() >= 0 && record[f.null_offset()]);
  return key;
}
std::string BplusTreeIndex::encode_first(const char *data, int len, bool high) const
{
  const auto &f = fields_.front();
  std::string raw(f.len(), 0);
  memcpy(raw.data(), data, std::min(len, f.len()));
  std::string key;
  encode_field(key, f, raw.data(), false);
  key.resize(key_size_, high ? static_cast<char>(255) : 0);
  return key;
}
