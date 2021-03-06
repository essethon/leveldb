// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// WriteBatch::rep_ :=
//    sequence: fixed64
//    count: fixed32
//    data: record[count]
// record :=
//    kTypeValue varstring varstring         |
//    kTypeDeletion varstring
// varstring :=
//    len: varint32
//    data: uint8[len]

#include "leveldb/write_batch.h"

#include "db/dbformat.h"
#include "db/memtable.h"
#include "db/write_batch_internal.h"

#include "leveldb/db.h"

#include "util/coding.h"

namespace leveldb {

// WriteBatch header has an 8-byte sequence number followed by a 4-byte count.
static const size_t kHeader = 12;

WriteBatch::WriteBatch() { Clear(); }

WriteBatch::~WriteBatch() = default;

WriteBatch::Handler::~Handler() = default;

void WriteBatch::Clear() {
  rep_.clear();
  rep_.resize(kHeader);
}

size_t WriteBatch::ApproximateSize() const { return rep_.size(); }

Status WriteBatch::Iterate(Handler* handler) const {
  Slice input(rep_);
  if (input.size() < kHeader) {
    return Status::Corruption("malformed WriteBatch (too small)");
  }

  input.remove_prefix(kHeader);
  Slice key, value;
  int found = 0;
  while (!input.empty()) {
    found++;
    char tag = input[0];
    input.remove_prefix(1);
    switch (tag) {
      case kTypeValue:
        if (GetLengthPrefixedSlice(&input, &key) &&
            GetLengthPrefixedSlice(&input, &value)) {
          handler->Put(key, value);
        } else {
          return Status::Corruption("bad WriteBatch Put");
        }
        break;
      case kTypeDeletion:
        if (GetLengthPrefixedSlice(&input, &key)) {
          handler->Delete(key);
        } else {
          return Status::Corruption("bad WriteBatch Delete");
        }
        break;
      default:
        return Status::Corruption("unknown WriteBatch tag");
    }
  }
  if (found != WriteBatchInternal::Count(this)) {
    return Status::Corruption("WriteBatch has wrong count");
  } else {
    return Status::OK();
  }
}

int WriteBatchInternal::Count(const WriteBatch* b) {
  return DecodeFixed32(b->rep_.data() + 8);
}

void WriteBatchInternal::SetCount(WriteBatch* b, int n) {
  EncodeFixed32(&b->rep_[8], n);
}

SequenceNumber WriteBatchInternal::Sequence(const WriteBatch* b) {
  return SequenceNumber(DecodeFixed64(b->rep_.data()));
}

void WriteBatchInternal::SetSequence(WriteBatch* b, SequenceNumber seq) {
  EncodeFixed64(&b->rep_[0], seq);
}

void WriteBatch::Put(const Slice& key, const Slice& value) {
  WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + 1);
  rep_.push_back(static_cast<char>(kTypeValue));
  PutLengthPrefixedSlice(&rep_, key);
  PutLengthPrefixedSlice(&rep_, value);
}

void WriteBatch::Delete(const Slice& key) {
  WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + 1);
  rep_.push_back(static_cast<char>(kTypeDeletion));
  PutLengthPrefixedSlice(&rep_, key);
}

void WriteBatch::Append(const WriteBatch& source) {
  WriteBatchInternal::Append(this, &source);
}

namespace {
class MemTableInserter : public WriteBatch::Handler {
 public:
  SequenceNumber sequence_;
  MemTable* mem_;

  void Put(const Slice& key, const Slice& value) override {
    mem_->Add(sequence_, kTypeValue, key, value);
    sequence_++;
  }
  void Delete(const Slice& key) override {
    mem_->Add(sequence_, kTypeDeletion, key, Slice());
    sequence_++;
  }
};

 class MemTableMVInsertor : public WriteBatchMV::Handler {
  public:
   SequenceNumber sequence_;
   MemTable* mem_;

   void Put(const Slice& key, ValidTime vt, const Slice& value) override {
     mem_->AddMV(sequence_, kTypeValue, key, vt, value);
     sequence_++;
   }
   void Delete(const Slice& key, ValidTime vt) override {
     mem_->AddMV(sequence_, kTypeDeletion, key, vt, Slice());
     sequence_++;
   }
 };
}  // namespace

Status WriteBatchInternal::InsertInto(const WriteBatch* b, MemTable* memtable) {
  MemTableInserter inserter;
  inserter.sequence_ = WriteBatchInternal::Sequence(b);
  inserter.mem_ = memtable;
  return b->Iterate(&inserter);
}

void WriteBatchInternal::SetContents(WriteBatch* b, const Slice& contents) {
  assert(contents.size() >= kHeader);
  b->rep_.assign(contents.data(), contents.size());
}

void WriteBatchInternal::Append(WriteBatch* dst, const WriteBatch* src) {
  SetCount(dst, Count(dst) + Count(src));
  assert(src->rep_.size() >= kHeader);
  dst->rep_.append(src->rep_.data() + kHeader, src->rep_.size() - kHeader);
}

// MVLevelDB implementations of internal functions to setup WriteBatch
Status WriteBatchMVInternal::InsertInto(const WriteBatchMV* b, MemTable* memtable) {
  MemTableMVInsertor inserter;
  inserter.sequence_ = WriteBatchMVInternal::Sequence(b);
  inserter.mem_ = memtable;
  return b->Iterate(&inserter);
}

void WriteBatchMVInternal::SetContents(WriteBatchMV* b, const Slice& contents) {
  assert(contents.size() >= kHeader);
  b->rep_.assign(contents.data(), contents.size());
}

void WriteBatchMVInternal::Append(WriteBatchMV* dst, const WriteBatchMV* src) {
  SetCount(dst, Count(dst) + Count(src));
  assert(src->rep_.size() >= kHeader);
  dst->rep_.append(src->rep_.data() + kHeader, src->rep_.size() - kHeader);
}


// MVLevelDB
// WriteBatchMV::rep_ :=
//    sequence: fixed64
//    count: fixed32
//    data: record[count]
// record :=
//    kTypeValue varstring ValidTime varstring         |
//    kTypeDeletion varstring ValidTime
// varstring :=
//    len: varint32
//    data: uint8[len]
WriteBatchMV::WriteBatchMV() { Clear(); }
WriteBatchMV::~WriteBatchMV() = default;
WriteBatchMV::Handler::~Handler() = default;

void WriteBatchMV::Clear() {
  rep_.clear();
  rep_.resize(kHeader);
}

size_t WriteBatchMV::ApproximateSize() const { return rep_.size(); }

Status WriteBatchMV::Iterate(Handler* handler) const {
  Slice input(rep_);
  if (input.size() < kHeader) {
    return Status::Corruption("malformed WriteBatchMV (too small)");
  }

  input.remove_prefix(kHeader);
  Slice key, value;
  ValidTime vt;
  int found = 0;
  while (!input.empty()) {
    found++;
    char tag = input[0];
    input.remove_prefix(1);
    switch (tag) {
      case kTypeValue:
        if (GetLengthPrefixedSlice(&input, &key) &&
            GetFixed64(&input, &vt) &&
            GetLengthPrefixedSlice(&input, &value)) {
          handler->Put(key, vt, value);
        } else {
          return Status::Corruption("bad WriteBatchMV Put");
        }
        break;
      case kTypeDeletion:
        if (GetLengthPrefixedSlice(&input, &key) &&
            GetFixed64(&input, &vt)) {
          handler->Delete(key, vt);
        } else {
          return Status::Corruption("bad WriteBatchMV Delete");
        }
        break;
      default:
        return Status::Corruption("unknown WriteBatchMV tag");
    }
  }
  if (found != WriteBatchMVInternal::Count(this)) {
    return Status::Corruption("WriteBatchMV has wrong count");
  } else {
    return Status::OK();
  }
}

int WriteBatchMVInternal::Count(const WriteBatchMV* b) {
  return DecodeFixed32(b->rep_.data() + 8);
}

void WriteBatchMVInternal::SetCount(WriteBatchMV* b, int n) {
  EncodeFixed32(&b->rep_[8], n);
}

SequenceNumber WriteBatchMVInternal::Sequence(const WriteBatchMV* b) {
  return SequenceNumber(DecodeFixed64(b->rep_.data()));
}

void WriteBatchMVInternal::SetSequence(WriteBatchMV* b, SequenceNumber seq) {
  EncodeFixed64(&b->rep_[0], seq);
}

void WriteBatchMV::Put(const Slice& key, const ValidTime vt, const Slice& value) {
  WriteBatchMVInternal::SetCount(this,
                                 WriteBatchMVInternal::Count(this) + 1);
  rep_.push_back(static_cast<char>(kTypeValue));
  PutLengthPrefixedSlice(&rep_, key);
  PutFixed64(&rep_, vt);
  PutLengthPrefixedSlice(&rep_, value);
}

void WriteBatchMV::Delete(const Slice& key, const ValidTime vt) {
  WriteBatchMVInternal::SetCount(this,
                                 WriteBatchMVInternal::Count(this) + 1);
  rep_.push_back(static_cast<char>(kTypeDeletion));
  PutLengthPrefixedSlice(&rep_, key);
  PutFixed64(&rep_, vt);
}

void WriteBatchMV::Append(const WriteBatchMV& source) {
  WriteBatchMVInternal::Append(this, &source);
}

}  // namespace leveldb
