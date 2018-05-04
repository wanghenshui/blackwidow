//  Copyright (c) 2017-present The blackwidow Authors.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.

#include "src/redis_lists.h"

#include <memory>

#include "src/util.h"
#include "src/lists_filter.h"
#include "src/scope_record_lock.h"
#include "src/scope_snapshot.h"

namespace blackwidow {

const rocksdb::Comparator* ListsDataKeyComparator() {
  static ListsDataKeyComparatorImpl ldkc;
  return &ldkc;
}

RedisLists::~RedisLists() {
  for (auto handle : handles_) {
    delete handle;
  }
}

Status RedisLists::Open(const rocksdb::Options& options,
                        const std::string& db_path) {
  rocksdb::Options ops(options);
  Status s = rocksdb::DB::Open(ops, db_path, &db_);
  if (s.ok()) {
    // Create column family
    rocksdb::ColumnFamilyHandle* cf;
    rocksdb::ColumnFamilyOptions cfo;
    cfo.comparator = ListsDataKeyComparator();
    s = db_->CreateColumnFamily(cfo, "data_cf", &cf);
    if (!s.ok()) {
      return s;
    }
    // Close DB
    delete cf;
    delete db_;
  }

  // Open
  rocksdb::DBOptions db_ops(options);
  rocksdb::ColumnFamilyOptions meta_cf_ops(options);
  rocksdb::ColumnFamilyOptions data_cf_ops(options);
  meta_cf_ops.compaction_filter_factory =
    std::make_shared<ListsMetaFilterFactory>();
  data_cf_ops.compaction_filter_factory =
    std::make_shared<ListsDataFilterFactory>(&db_, &handles_);
  data_cf_ops.comparator = ListsDataKeyComparator();
  std::vector<rocksdb::ColumnFamilyDescriptor> column_families;
  // Meta CF
  column_families.push_back(rocksdb::ColumnFamilyDescriptor(
      rocksdb::kDefaultColumnFamilyName, meta_cf_ops));
  // Data CF
  column_families.push_back(rocksdb::ColumnFamilyDescriptor(
      "data_cf", data_cf_ops));
  return rocksdb::DB::Open(db_ops, db_path, column_families, &handles_, &db_);
}

Status RedisLists::LPush(const Slice& key,
                         const std::vector<std::string>& values,
                         uint64_t* ret) {
  rocksdb::WriteBatch batch;
  int32_t version = 0;
  uint64_t index = 0;
  *ret = 0;
  std::string meta_value;
  ScopeRecordLock l(lock_mgr_, key);
  Status s = db_->Get(default_read_options_, handles_[0], key, &meta_value);
  if (s.ok()) {
    ParsedListsMetaValue parsed_lists_meta_value(&meta_value);
    if (parsed_lists_meta_value.IsStale()) {
      version = parsed_lists_meta_value.InitialMetaValue();
    } else {
      version = parsed_lists_meta_value.version();
    }
    for (const auto& value : values) {
      index = parsed_lists_meta_value.left_index();
      parsed_lists_meta_value.ModifyLeftIndex(1);
      parsed_lists_meta_value.ModifyCount(1);
      ListsDataKey lists_data_key(key, version, index);
      batch.Put(handles_[1], lists_data_key.Encode(), value);
    }
    batch.Put(handles_[0], key, meta_value);
    *ret = parsed_lists_meta_value.count();
  } else if (s.IsNotFound()) {
    char str[8];
    EncodeFixed64(str, values.size());
    ListsMetaValue lists_meta_value(std::string(str, sizeof(uint64_t)));
    version = lists_meta_value.UpdateVersion();
    for (const auto& value : values) {
      index = lists_meta_value.left_index();
      lists_meta_value.ModifyLeftIndex(1);
      ListsDataKey lists_data_key(key, version, index);
      batch.Put(handles_[1], lists_data_key.Encode(), value);
    }
    batch.Put(handles_[0], key, lists_meta_value.Encode());
    *ret = lists_meta_value.right_index() - lists_meta_value.left_index() - 1;
  } else {
    return s;
  }
  return db_->Write(default_write_options_, &batch);
}

Status RedisLists::RPush(const Slice& key,
                         const std::vector<std::string>& values,
                         uint64_t* ret) {
  rocksdb::WriteBatch batch;
  int32_t version = 0;
  uint64_t index = 0;
  *ret = 0;
  std::string meta_value;
  ScopeRecordLock l(lock_mgr_, key);
  Status s = db_->Get(default_read_options_, handles_[0], key, &meta_value);
  if (s.ok()) {
    ParsedListsMetaValue parsed_lists_meta_value(&meta_value);
    if (parsed_lists_meta_value.IsStale()) {
      version = parsed_lists_meta_value.InitialMetaValue();
    } else {
      version = parsed_lists_meta_value.version();
    }
    for (const auto& value : values) {
      index = parsed_lists_meta_value.right_index();
      parsed_lists_meta_value.ModifyRightIndex(1);
      parsed_lists_meta_value.ModifyCount(1);
      ListsDataKey lists_data_key(key, version, index);
      batch.Put(handles_[1], lists_data_key.Encode(), value);
    }
    batch.Put(handles_[0], key, meta_value);
    *ret = parsed_lists_meta_value.count();
  } else if (s.IsNotFound()) {
    char str[8];
    EncodeFixed64(str, values.size());
    ListsMetaValue lists_meta_value(std::string(str, sizeof(uint64_t)));
    version = lists_meta_value.UpdateVersion();
    lists_meta_value.set_timestamp(0);
    for (auto value : values) {
      index = lists_meta_value.right_index();
      lists_meta_value.ModifyRightIndex(1);
      ListsDataKey lists_data_key(key, version, index);
      batch.Put(handles_[1], lists_data_key.Encode(), value);
    }
    batch.Put(handles_[0], key, lists_meta_value.Encode());
    *ret = lists_meta_value.right_index() - lists_meta_value.left_index() - 1;
  } else {
    return s;
  }
  return db_->Write(default_write_options_, &batch);
}

Status RedisLists::LRange(const Slice& key, int64_t start, int64_t stop,
                          std::vector<std::string>* ret) {
  ScopeRecordLock l(lock_mgr_, key);
  std::string meta_value;

  Status s = db_->Get(default_read_options_, handles_[0], key, &meta_value);
  if (s.ok()) {
    ParsedListsMetaValue parsed_lists_meta_value(&meta_value);
    if (parsed_lists_meta_value.IsStale()) {
      return Status::NotFound("Stale");
    } else if (parsed_lists_meta_value.count() == 0) {
      return Status::NotFound();
    } else {
      int32_t version = parsed_lists_meta_value.version();
      uint64_t start_index = start >= 0 ?
                             parsed_lists_meta_value.left_index() + start + 1 :
                             parsed_lists_meta_value.right_index() + start;
      uint64_t stop_index = stop >= 0 ?
                            parsed_lists_meta_value.left_index() + stop + 1 :
                            parsed_lists_meta_value.right_index() + stop;
      if (start_index > stop_index) {
        return s;
      }
      if (start_index <= parsed_lists_meta_value.left_index()) {
        start_index = parsed_lists_meta_value.left_index() + 1;
      }
      if (stop_index >= parsed_lists_meta_value.right_index()) {
        stop_index = parsed_lists_meta_value.right_index() - 1;
      }
      rocksdb::Iterator* iter = db_->NewIterator(default_read_options_,
              handles_[1]);
      ListsDataKey start_data_key(key, version, start_index);
      for (iter->Seek(start_data_key.Encode());
           iter->Valid() && start_index <= stop_index;
           iter->Next(), start_index++) {
        ret->push_back(iter->value().ToString());
      }
      delete iter;
      return s;
    }
  } else {
    return s;
  }
}

Status RedisLists::LTrim(const Slice& key, int64_t start, int64_t stop) {
  std::vector<std::string> values;
  uint64_t count;
  {
  ScopeRecordLock l(lock_mgr_, key);
  std::string meta_value;

  Status s = db_->Get(default_read_options_, handles_[0], key, &meta_value);
  if (s.ok()) {
    ParsedListsMetaValue parsed_meta_value(&meta_value);
    if (parsed_meta_value.IsStale()) {
      return s;
    } else {
      rocksdb::Iterator* iter = db_->NewIterator(default_read_options_,
                                                 handles_[1]);
      int32_t version = parsed_meta_value.version();
      uint64_t start_index = start >= 0 ?
                             parsed_meta_value.left_index() + start + 1 :
                             parsed_meta_value.right_index() + start;
      uint64_t stop_index = stop >= 0 ?
                            parsed_meta_value.left_index() + stop + 1 :
                            parsed_meta_value.right_index() + stop;
      if (start_index > stop_index) {
        return s;
      }
      if (stop_index >= parsed_meta_value.right_index()) {
        stop_index = parsed_meta_value.right_index() - 1;
      }
      // Update the meta of list
      parsed_meta_value.InitialMetaValue();
      s = db_->Put(default_write_options_, handles_[0], key, meta_value);
      if (!s.ok()) {
        return s;
      }
      ListsDataKey start_data_key(key, version, start_index);
      for (iter->Seek(start_data_key.Encode());
           iter->Valid() && start_index <= stop_index;
           iter->Next(), start_index++) {
        values.push_back(iter->value().ToString());
      }
      delete iter;
    }
  }
  }  // ScopeRecordLock
  return RPush(key, values, &count);
}

Status RedisLists::LLen(const Slice& key, uint64_t* len) {
  std::string meta_value;
  Status s = db_->Get(default_read_options_, handles_[0], key, &meta_value);
  if (s.ok()) {
    ParsedListsMetaValue parsed_lists_meta_value(&meta_value);
    if (parsed_lists_meta_value.IsStale()) {
      *len = 0;
      return Status::NotFound("Stale");
    } else if (parsed_lists_meta_value.count() == 0) {
      *len = 0;
      return Status::NotFound();
    } else {
      *len = parsed_lists_meta_value.count();
      return s;
    }
  }
  return s;
}

Status RedisLists::LPop(const Slice& key, std::string* element) {
  rocksdb::WriteBatch batch;
  ScopeRecordLock l(lock_mgr_, key);
  std::string meta_value;
  Status s = db_->Get(default_read_options_, handles_[0], key, &meta_value);
  if (s.ok()) {
    ParsedListsMetaValue parsed_lists_meta_value(&meta_value);
    if (parsed_lists_meta_value.IsStale()) {
      return Status::NotFound("Stale");
    } else if (parsed_lists_meta_value.count() == 0) {
      return Status::NotFound();
    } else {
      int32_t version = parsed_lists_meta_value.version();
      uint64_t first_node_index = parsed_lists_meta_value.left_index() + 1;
      ListsDataKey lists_data_key(key, version, first_node_index);
      s = db_->Get(default_read_options_, handles_[1], lists_data_key.Encode(), element);
      if (s.ok()) {
        batch.Delete(handles_[1], lists_data_key.Encode());
        parsed_lists_meta_value.ModifyCount(-1);
        parsed_lists_meta_value.ModifyLeftIndex(-1);
        batch.Put(handles_[0], key, meta_value);
        return db_->Write(default_write_options_, &batch);
      } else {
        return s;
      }
    }
  }
  return s;
}

Status RedisLists::RPop(const Slice& key, std::string* element) {
  rocksdb::WriteBatch batch;
  ScopeRecordLock l(lock_mgr_, key);
  std::string meta_value;
  Status s = db_->Get(default_read_options_, handles_[0], key, &meta_value);
  if (s.ok()) {
    ParsedListsMetaValue parsed_lists_meta_value(&meta_value);
    if (parsed_lists_meta_value.IsStale()) {
      return Status::NotFound("Stale");
    } else if (parsed_lists_meta_value.count() == 0) {
      return Status::NotFound();
    } else {
      int32_t version = parsed_lists_meta_value.version();
      uint64_t last_node_index = parsed_lists_meta_value.right_index() - 1;
      ListsDataKey lists_data_key(key, version, last_node_index);
      s = db_->Get(default_read_options_, handles_[1], lists_data_key.Encode(), element);
      if (s.ok()) {
        batch.Delete(handles_[1], lists_data_key.Encode());
        parsed_lists_meta_value.ModifyCount(-1);
        parsed_lists_meta_value.ModifyRightIndex(-1);
        batch.Put(handles_[0], key, meta_value);
        return db_->Write(default_write_options_, &batch);
      } else {
        return s;
      }
    }
  }
  return s;
}

Status RedisLists::LIndex(const Slice& key, int64_t index, std::string* element) {
  rocksdb::ReadOptions read_options;
  const rocksdb::Snapshot* snapshot;

  ScopeSnapshot ss(db_, &snapshot);
  read_options.snapshot = snapshot;
  std::string meta_value;
  Status s = db_->Get(read_options, handles_[0], key, &meta_value);
  if (s.ok()) {
    ParsedListsMetaValue parsed_lists_meta_value(&meta_value);
    int32_t version = parsed_lists_meta_value.version();
    if (parsed_lists_meta_value.IsStale()) {
      return Status::NotFound("Stale");
    } else if (parsed_lists_meta_value.count() == 0) {
      return Status::NotFound();
    } else {
      std::string tmp_element;
      uint64_t positive_direction_index = index >= 0 ?
            parsed_lists_meta_value.left_index() + index + 1 :
            parsed_lists_meta_value.right_index() + index;
      ListsDataKey lists_data_key(key, version, positive_direction_index);
      s = db_->Get(read_options, handles_[1], lists_data_key.Encode(), &tmp_element);
      if (s.ok()) {
        *element = tmp_element;
      }
    }
  }
  return s;
}

Status RedisLists::LInsert(const Slice& key,
                           const BlackWidow::BeforeOrAfter& before_or_after,
                           const std::string& pivot,
                           const std::string& value,
                           int64_t* ret) {
  rocksdb::WriteBatch batch;
  ScopeRecordLock l(lock_mgr_, key);
  std::string meta_value;
  Status s = db_->Get(default_read_options_, handles_[0], key, &meta_value);
  if (s.ok()) {
    ParsedListsMetaValue parsed_lists_meta_value(&meta_value);
    if (parsed_lists_meta_value.IsStale()) {
      *ret = 0;
      return Status::NotFound("Stale");
    } else if (parsed_lists_meta_value.count() == 0) {
      *ret = 0;
      return Status::NotFound();
    } else {
      bool find_pivot = false;
      uint64_t pivot_index = 0;
      uint32_t version = parsed_lists_meta_value.version();
      uint64_t current_index = parsed_lists_meta_value.left_index() + 1;
      rocksdb::Iterator* iter = db_->NewIterator(default_read_options_, handles_[1]);
      ListsDataKey start_data_key(key, version, current_index);
      for (iter->Seek(start_data_key.Encode());
           iter->Valid() && current_index < parsed_lists_meta_value.right_index();
           iter->Next(), current_index++) {
          if (strcmp(iter->value().ToString().data(), pivot.data()) == 0) {
            find_pivot = true;
            pivot_index = current_index;
            break;
          }
      }
      delete iter;
      if (!find_pivot) {
        *ret = -1;
        return Status::NotFound();
      } else {
        uint64_t target_index;
        std::vector<std::string> list_nodes;
        uint64_t mid_index = parsed_lists_meta_value.left_index()
            + (parsed_lists_meta_value.right_index() - parsed_lists_meta_value.left_index()) / 2;
        if (pivot_index <= mid_index) {
          target_index = (before_or_after == BlackWidow::Before) ? pivot_index - 1 : pivot_index;
          current_index = parsed_lists_meta_value.left_index() + 1;
          rocksdb::Iterator* first_half_iter = db_->NewIterator(default_read_options_, handles_[1]);
          ListsDataKey start_data_key(key, version, current_index);
          for (first_half_iter->Seek(start_data_key.Encode());
               first_half_iter->Valid() && current_index <= pivot_index;
               first_half_iter->Next(), current_index++) {
              if (current_index == pivot_index) {
                if (before_or_after == BlackWidow::After) {
                  list_nodes.push_back(first_half_iter->value().ToString());
                }
                break;
              }
              list_nodes.push_back(first_half_iter->value().ToString());
          }
          delete first_half_iter;

          current_index = parsed_lists_meta_value.left_index();
          for (const auto& node : list_nodes) {
            ListsDataKey lists_data_key(key, version, current_index++);
            batch.Put(handles_[1], lists_data_key.Encode(), node);
          }
          parsed_lists_meta_value.ModifyLeftIndex(1);
        } else {
          target_index = (before_or_after == BlackWidow::Before) ? pivot_index : pivot_index + 1;
          current_index = pivot_index;
          rocksdb::Iterator* after_half_iter = db_->NewIterator(default_read_options_, handles_[1]);
          ListsDataKey start_data_key(key, version, current_index);
          for (after_half_iter->Seek(start_data_key.Encode());
               after_half_iter->Valid() && current_index < parsed_lists_meta_value.right_index();
               after_half_iter->Next(), current_index++) {
              if (current_index == pivot_index
                && before_or_after == BlackWidow::BeforeOrAfter::After) {
                continue;
              }
              list_nodes.push_back(after_half_iter->value().ToString());
          }
          delete after_half_iter;

          current_index = target_index + 1;
          for (const auto& node : list_nodes) {
            ListsDataKey lists_data_key(key, version, current_index++);
            batch.Put(handles_[1], lists_data_key.Encode(), node);
          }
          parsed_lists_meta_value.ModifyRightIndex(1);
        }
        parsed_lists_meta_value.ModifyCount(1);
        batch.Put(handles_[0], key, meta_value);
        ListsDataKey lists_target_key(key, version, target_index);
        batch.Put(handles_[1], lists_target_key.Encode(), value);
        *ret = parsed_lists_meta_value.count();
        return db_->Write(default_write_options_, &batch);
      }
    }
  } else if (s.IsNotFound()){
    *ret = 0;
  }
  return s;
}

Status RedisLists::LPushx(const Slice& key, const Slice& value, uint64_t* len) {
  rocksdb::WriteBatch batch;
  ScopeRecordLock l(lock_mgr_, key);
  std::string meta_value;
  Status s = db_->Get(default_read_options_, handles_[0], key, &meta_value);
  if (s.ok()) {
    ParsedListsMetaValue parsed_lists_meta_value(&meta_value);
    if (parsed_lists_meta_value.IsStale()) {
      return Status::NotFound("Stale");
    } else if (parsed_lists_meta_value.count() == 0) {
      return Status::NotFound();
    } else {
      uint32_t version = parsed_lists_meta_value.version();
      uint64_t index = parsed_lists_meta_value.left_index();
      parsed_lists_meta_value.ModifyCount(1);
      parsed_lists_meta_value.ModifyLeftIndex(1);
      ListsDataKey lists_data_key(key, version, index);
      batch.Put(handles_[0], key, meta_value);
      batch.Put(handles_[1], lists_data_key.Encode(), value);
      *len = parsed_lists_meta_value.count();
      return db_->Write(default_write_options_, &batch);
    }
  }
  return s;
}

Status RedisLists::RPushx(const Slice& key, const Slice& value, uint64_t* len) {
  rocksdb::WriteBatch batch;
  ScopeRecordLock l(lock_mgr_, key);
  std::string meta_value;
  Status s = db_->Get(default_read_options_, handles_[0], key, &meta_value);
  if (s.ok()) {
    ParsedListsMetaValue parsed_lists_meta_value(&meta_value);
    if (parsed_lists_meta_value.IsStale()) {
      return Status::NotFound("Stale");
    } else if (parsed_lists_meta_value.count() == 0) {
      return Status::NotFound();
    } else {
      uint32_t version = parsed_lists_meta_value.version();
      uint64_t index = parsed_lists_meta_value.right_index();
      parsed_lists_meta_value.ModifyCount(1);
      parsed_lists_meta_value.ModifyRightIndex(1);
      ListsDataKey lists_data_key(key, version, index);
      batch.Put(handles_[0], key, meta_value);
      batch.Put(handles_[1], lists_data_key.Encode(), value);
      *len = parsed_lists_meta_value.count();
      return db_->Write(default_write_options_, &batch);
    }
  }
  return s;
}

Status RedisLists::LRem(const Slice& key, int64_t count,
                        const Slice& value, uint64_t* ret) {
  rocksdb::WriteBatch batch;
  ScopeRecordLock l(lock_mgr_, key);
  std::string meta_value;
  Status s = db_->Get(default_read_options_, handles_[0], key, &meta_value);
  if (s.ok()) {
    ParsedListsMetaValue parsed_lists_meta_value(&meta_value);
    if (parsed_lists_meta_value.IsStale()) {
      *ret = 0;
      return Status::NotFound("Stale");
    } else if (parsed_lists_meta_value.count() == 0) {
      *ret = 0;
      return Status::NotFound();
    } else {
      uint64_t current_index;
      std::vector<uint64_t> del_index;
      uint64_t rest = (count < 0) ? -count : count;
      uint32_t version = parsed_lists_meta_value.version();
      uint64_t start_index = parsed_lists_meta_value.left_index() + 1;
      uint64_t stop_index = parsed_lists_meta_value.right_index() - 1;
      ListsDataKey start_data_key(key, version, start_index);
      ListsDataKey stop_data_key(key, version, stop_index);
      rocksdb::Iterator* iter = db_->NewIterator(default_read_options_, handles_[1]);
      if (count >= 0) {
        current_index = start_index;
        for (iter->Seek(start_data_key.Encode());
             iter->Valid() && current_index <= stop_index && (!count || rest != 0);
             iter->Next(), current_index++) {
          if (strcmp(iter->value().ToString().data(), value.data()) == 0) {
            del_index.push_back(current_index);
            if (count != 0) {
              rest--;
            }
          }
        }
      } else {
        current_index = stop_index;
        for (iter->Seek(stop_data_key.Encode());
             iter->Valid() && current_index >= start_index && (!count || rest != 0);
             iter->Prev(), current_index--) {
          if (strcmp(iter->value().ToString().data(), value.data()) == 0) {
            del_index.push_back(current_index);
            if (count != 0) {
              rest--;
            }
          }
        }
      }
      if (del_index.empty()) {
        *ret = 0;
        return Status::NotFound();
      } else {
        rest = del_index.size();
        uint64_t sublist_left_index  = (count >= 0) ? del_index[0] : del_index[del_index.size() - 1];
        uint64_t sublist_right_index = (count >= 0) ? del_index[del_index.size() - 1] : del_index[0];
        uint64_t left_part_len = sublist_right_index - start_index;
        uint64_t right_part_len = stop_index - sublist_left_index;
        if (left_part_len <= right_part_len) {
          uint64_t left = sublist_right_index;
          current_index  = sublist_right_index;
          ListsDataKey sublist_right_key(key, version, sublist_right_index);
          for (iter->Seek(sublist_right_key.Encode());
               iter->Valid() && current_index >= start_index;
               iter->Prev(), current_index--) {
            if (!strcmp(iter->value().ToString().data(), value.data()) && rest > 0) {
              rest--;
            } else {
              ListsDataKey lists_data_key(key, version, left--);
              batch.Put(handles_[1], lists_data_key.Encode(), iter->value());
            }
          }
          parsed_lists_meta_value.ModifyLeftIndex(-del_index.size());
        } else {
          uint64_t right = sublist_left_index;
          current_index = sublist_left_index;
          ListsDataKey sublist_left_key(key, version, sublist_left_index);
          for (iter->Seek(sublist_left_key.Encode());
               iter->Valid() && current_index <= stop_index;
               iter->Next(), current_index++) {
            if (!strcmp(iter->value().ToString().data(), value.data()) && rest > 0) {
              rest--;
            } else {
              ListsDataKey lists_data_key(key, version, right++);
              batch.Put(handles_[1], lists_data_key.Encode(), iter->value());
            }
          }
          parsed_lists_meta_value.ModifyRightIndex(-del_index.size());
        }
        delete iter;
        parsed_lists_meta_value.ModifyCount(-del_index.size());
        batch.Put(handles_[0], key, meta_value);
        *ret = del_index.size();
        return db_->Write(default_write_options_, &batch);
      }
    }
  } else if (s.IsNotFound()) {
    *ret = 0;
  }
  return s;
}

Status RedisLists::LSet(const Slice& key, int64_t index, const Slice& value) {
  ScopeRecordLock l(lock_mgr_, key);
  std::string meta_value;
  Status s = db_->Get(default_read_options_, handles_[0], key, &meta_value);
  if (s.ok()) {
    ParsedListsMetaValue parsed_lists_meta_value(&meta_value);
    if (parsed_lists_meta_value.IsStale()) {
      return Status::NotFound("Stale");
    } else if (parsed_lists_meta_value.count() == 0) {
      return Status::NotFound();
    } else {
      uint32_t version = parsed_lists_meta_value.version();
      uint64_t target_index = index >= 0 ?
        parsed_lists_meta_value.left_index() + index + 1
        : parsed_lists_meta_value.right_index() + index;
      if (target_index <= parsed_lists_meta_value.left_index()
        || target_index >= parsed_lists_meta_value.right_index()) {
        return Status::NotFound();
      }
      ListsDataKey lists_data_key(key, version, target_index);
      return db_->Put(default_write_options_, handles_[1],
                      lists_data_key.Encode(), value);
    }
  }
  return s;
}

Status RedisLists::RPoplpush(const Slice& source,
                             const Slice& destination,
                             std::string* element) {
  element->clear();
  Status s;
  rocksdb::WriteBatch batch;
  MultiScopeRecordLock l(lock_mgr_, {source.ToString(), destination.ToString()});
  if (!source.compare(destination)) {
    std::string meta_value;
    s = db_->Get(default_read_options_, handles_[0], source, &meta_value);
    if (s.ok()) {
      ParsedListsMetaValue parsed_lists_meta_value(&meta_value);
      if (parsed_lists_meta_value.IsStale()) {
        return Status::NotFound("Stale");
      } else if (parsed_lists_meta_value.count() == 0) {
        return Status::NotFound();
      } else {
        std::string target;
        int32_t version = parsed_lists_meta_value.version();
        uint64_t last_node_index = parsed_lists_meta_value.right_index() - 1;
        ListsDataKey lists_data_key(source, version, last_node_index);
        s = db_->Get(default_read_options_, handles_[1], lists_data_key.Encode(), &target);
        if (s.ok()) {
          *element = target;
          if (parsed_lists_meta_value.count() == 1) {
            return Status::OK();
          } else {
            uint64_t target_index = parsed_lists_meta_value.left_index();
            ListsDataKey lists_target_key(source, version, target_index);
            batch.Delete(handles_[1], lists_data_key.Encode());
            batch.Put(handles_[1], lists_target_key.Encode(), target);
            parsed_lists_meta_value.ModifyRightIndex(-1);
            parsed_lists_meta_value.ModifyLeftIndex(1);
            batch.Put(handles_[0], source, meta_value);
            return db_->Write(default_write_options_, &batch);
          }
        } else {
          return s;
        }
      }
    } else {
      return s;
    }
  }

  int32_t version;
  std::string target;
  std::string source_meta_value;
  s = db_->Get(default_read_options_, handles_[0], source, &source_meta_value);
  if (s.ok()) {
    ParsedListsMetaValue parsed_lists_meta_value(&source_meta_value);
    if (parsed_lists_meta_value.IsStale()) {
      return Status::NotFound("Stale");
    } else if(parsed_lists_meta_value.count() == 0) {
      return Status::NotFound();
    } else {
      version = parsed_lists_meta_value.version();
      uint64_t last_node_index = parsed_lists_meta_value.right_index() - 1;
      ListsDataKey lists_data_key(source, version, last_node_index);
      s = db_->Get(default_read_options_, handles_[1], lists_data_key.Encode(), &target);
      if (s.ok()) {
        batch.Delete(handles_[1], lists_data_key.Encode());
        parsed_lists_meta_value.ModifyCount(-1);
        parsed_lists_meta_value.ModifyRightIndex(-1);
        batch.Put(handles_[0], source, source_meta_value);
      } else {
        return s;
      }
    }
  } else {
    return s;
  }

  std::string destination_meta_value;
  s = db_->Get(default_read_options_, handles_[0], destination, &destination_meta_value);
  if (s.ok()) {
    ParsedListsMetaValue parsed_lists_meta_value(&destination_meta_value);
    if (parsed_lists_meta_value.IsStale()) {
      version = parsed_lists_meta_value.InitialMetaValue();
    } else {
      version = parsed_lists_meta_value.version();
    }
    uint64_t target_index = parsed_lists_meta_value.left_index();
    ListsDataKey lists_data_key(destination, version, target_index);
    batch.Put(handles_[1], lists_data_key.Encode(), target);
    parsed_lists_meta_value.ModifyCount(1);
    parsed_lists_meta_value.ModifyLeftIndex(1);
    batch.Put(handles_[0], destination, destination_meta_value);
  } else if (s.IsNotFound()) {
    char str[8];
    EncodeFixed64(str, 1);
    ListsMetaValue lists_meta_value(std::string(str, sizeof(uint64_t)));
    version = lists_meta_value.UpdateVersion();
    uint64_t target_index = lists_meta_value.left_index();
    ListsDataKey lists_data_key(destination, version, target_index);
    batch.Put(handles_[1], lists_data_key.Encode(), target);
    lists_meta_value.ModifyLeftIndex(1);
    batch.Put(handles_[0], destination, lists_meta_value.Encode());
  } else {
    return s;
  }

  s = db_->Write(default_write_options_, &batch);
  if (s.ok()) {
    *element = target;
  }
  return s;
}

Status RedisLists::CompactRange(const rocksdb::Slice* begin,
                                 const rocksdb::Slice* end) {
  Status s = db_->CompactRange(default_compact_range_options_,
      handles_[0], begin, end);
  if (!s.ok()) {
    return s;
  }
  return db_->CompactRange(default_compact_range_options_,
      handles_[1], begin, end);
}

Status RedisLists::Expire(const Slice& key, int32_t ttl) {
  std::string meta_value;
  ScopeRecordLock l(lock_mgr_, key);
  Status s = db_->Get(default_read_options_, handles_[0], key, &meta_value);
  if (s.ok()) {
    ParsedListsMetaValue parsed_lists_meta_value(&meta_value);
    if (parsed_lists_meta_value.IsStale()) {
      return Status::NotFound("Stale");
    }
    if (ttl > 0) {
      parsed_lists_meta_value.SetRelativeTimestamp(ttl);
      s = db_->Put(default_write_options_, handles_[0], key, meta_value);
    } else {
      parsed_lists_meta_value.InitialMetaValue();
      s = db_->Put(default_write_options_, handles_[0], key, meta_value);
    }
  }
  return s;
}

Status RedisLists::Del(const Slice& key) {
  std::string meta_value;
  ScopeRecordLock l(lock_mgr_, key);
  Status s = db_->Get(default_read_options_, handles_[0], key, &meta_value);
  if (s.ok()) {
    ParsedListsMetaValue parsed_lists_meta_value(&meta_value);
    if (parsed_lists_meta_value.IsStale()) {
      return Status::NotFound("Stale");
    } else {
      parsed_lists_meta_value.InitialMetaValue();
      s = db_->Put(default_write_options_, handles_[0], key, meta_value);
    }
  }
  return s;
}

bool RedisLists::Scan(const std::string& start_key,
                      const std::string& pattern,
                      std::vector<std::string>* keys,
                      int64_t* count,
                      std::string* next_key) {
  return true;
}

Status RedisLists::Expireat(const Slice& key, int32_t timestamp) {
  Status s;
  return s;
}

Status RedisLists::Persist(const Slice& key) {
  Status s;
  return s;
}

Status RedisLists::TTL(const Slice& key, int64_t* timestamp) {
  Status s;
  return s;
}

}   //  namespace blackwidow
