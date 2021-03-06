// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/dbformat.h"

#include <cstdio>
#include <sstream>

#include "port/port.h"
#include "util/coding.h"

namespace leveldb {

static uint64_t PackSequenceAndType(uint64_t seq, ValueType t) {
  assert(seq <= kMaxSequenceNumber);
  assert(t <= kValueTypeForSeek);
  return (seq << 8) | t;
}

void AppendInternalKey(std::string* result, const ParsedInternalKey& key) {
  result->append(key.user_key.data(), key.user_key.size());
  PutFixed64(result, PackSequenceAndType(key.sequence, key.type));
}
void AppendMVInternalKey(std::string* result, const ParsedMVInternalKey& key) {
  result->append(key.user_key.data(), key.user_key.size());
  PutFixed64(result, PackSequenceAndType(key.sequence, key.type));
  PutFixed64(result, key.valid_time);
}

std::string ParsedInternalKey::DebugString() const {
  std::ostringstream ss;
  ss << '\'' << EscapeString(user_key.ToString()) << "' @ " << sequence << " : "
     << static_cast<int>(type);
  return ss.str();
}

std::string InternalKey::DebugString() const {
  ParsedInternalKey parsed;
  if (ParseInternalKey(rep_, &parsed)) {
    return parsed.DebugString();
  }
  std::ostringstream ss;
  ss << "(bad)" << EscapeString(rep_);
  return ss.str();
}

const char* InternalKeyComparator::Name() const {
  return "leveldb.InternalKeyComparator";
}
//const char* MVInternalKeyComparator::Name() const {
//  return "leveldb.MVInternalKeyComparator";
//}

int InternalKeyComparator::Compare(const Slice& akey, const Slice& bkey) const {
  // Order by:
  //    increasing user key (according to user-supplied comparator)
  //    decreasing sequence number
  //    decreasing type (though sequence# should be enough to disambiguate)

  // If multi_version == true, strip the ValidTime field (8 bytes)
  int r, s;
  if (multi_version) {
    s = 16;
    r = user_comparator_->Compare(MVExtractUserKey(akey), MVExtractUserKey(bkey));
  } else {
    s = 8;
    r = user_comparator_->Compare(ExtractUserKey(akey), ExtractUserKey(bkey));
  }
  if (r == 0) {
    const uint64_t anum = DecodeFixed64(akey.data() + akey.size() - s);
    const uint64_t bnum = DecodeFixed64(bkey.data() + bkey.size() - s);
    if (anum > bnum) {
      r = -1;
    } else if (anum < bnum) {
      if (multi_version) {
        const ValidTime at = DecodeFixed64(akey.data() + akey.size() - 8);
        const ValidTime bt = DecodeFixed64(bkey.data() + bkey.size() - 8);
        if (at > bt) {
          r = -1;
        } else if (at < bt) {
          r = +1;
        } else {
          r = +1;
        }
      } else {
        r = +1;
      }
    }
  }
  return r;
}

void InternalKeyComparator::FindShortestSeparator(std::string* start,
                                                  const Slice& limit) const {
  // Attempt to shorten the user portion of the key
  Slice user_start, user_limit;
  if (multi_version) {
    user_start = MVExtractUserKey(*start);
    user_limit = MVExtractUserKey(limit);
  } else {
    user_start = ExtractUserKey(*start);
    user_limit = ExtractUserKey(limit);
  }

  std::string tmp(user_start.data(), user_start.size());
  user_comparator_->FindShortestSeparator(&tmp, user_limit);
  if (tmp.size() < user_start.size() &&
      user_comparator_->Compare(user_start, tmp) < 0) {
    // User key has become shorter physically, but larger logically.
    // Tack on the earliest possible number to the shortened user key.
    PutFixed64(&tmp,
               PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek));
    // Set timestamp field to 0. MVLevelDB
    if (multi_version) { PutFixed64(&tmp, kMinValidTime); }
    assert(this->Compare(*start, tmp) < 0);
    assert(this->Compare(tmp, limit) < 0);
    start->swap(tmp);
  }
}

void InternalKeyComparator::FindShortSuccessor(std::string* key) const {
  Slice user_key = ExtractUserKey(*key);
  std::string tmp(user_key.data(), user_key.size());
  user_comparator_->FindShortSuccessor(&tmp);
  if (tmp.size() < user_key.size() &&
      user_comparator_->Compare(user_key, tmp) < 0) {
    // User key has become shorter physically, but larger logically.
    // Tack on the earliest possible number to the shortened user key.
    PutFixed64(&tmp,
               PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek));
    // Set timestamp field to 0. MVLevelDB
    if (multi_version) { PutFixed64(&tmp, kMinValidTime); }
    assert(this->Compare(*key, tmp) < 0);
    key->swap(tmp);
  }
}

// Implementations of multi-version (MV) internal key comparator
//int MVInternalKeyComparator::Compare(const Slice& akey, const Slice& bkey) const {
//  int r = user_comparator_->Compare(MVExtractUserKey(akey),
//                                    MVExtractUserKey(bkey));
//  if (r == 0) {
//    // Order by:
//    //    decreasing sequence number ()
//    const uint64_t anum = DecodeFixed64(akey.data() + akey.size() - 16);
//    const uint64_t bnum = DecodeFixed64(bkey.data() + bkey.size() - 16);
//    if (anum > bnum) {
//      r = -1;
//    } else if (anum < bnum) {
//      r = +1;
//    }
//  }
//  return r;
//}
//
//void MVInternalKeyComparator::FindShortestSeparator(std::string* start,
//                                                  const Slice& limit) const {
//  // MVLevelDB: We do nothing here.
//}
//
//void MVInternalKeyComparator::FindShortSuccessor(std::string* key) const {
//  // MVLevelDB: We do nothing here.
//}

const char* InternalFilterPolicy::Name() const { return user_policy_->Name(); }

void InternalFilterPolicy::CreateFilter(const Slice* keys, int n,
                                        std::string* dst) const {
  // We rely on the fact that the code in table.cc does not mind us
  // adjusting keys[].
  Slice* mkey = const_cast<Slice*>(keys);
  for (int i = 0; i < n; i++) {
    mkey[i] = ExtractUserKey(keys[i]);
    // TODO(sanjay): Suppress dups?
  }
  user_policy_->CreateFilter(keys, n, dst);
}

bool InternalFilterPolicy::KeyMayMatch(const Slice& key, const Slice& f) const {
  return user_policy_->KeyMayMatch(ExtractUserKey(key), f);
}

LookupKey::LookupKey(const Slice& user_key, SequenceNumber s) {
  size_t usize = user_key.size();
  size_t needed = usize + 13;  // A conservative estimate
  char* dst;
  if (needed <= sizeof(space_)) {
    dst = space_;
  } else {
    dst = new char[needed];
  }
  start_ = dst;
  dst = EncodeVarint32(dst, usize + 8);
  kstart_ = dst;
  std::memcpy(dst, user_key.data(), usize);
  dst += usize;
  EncodeFixed64(dst, PackSequenceAndType(s, kValueTypeForSeek));
  dst += 8;
  end_ = dst;
}

MVLookupKey::MVLookupKey(const Slice& user_key, SequenceNumber s,
                         ValidTime t) {
  size_t usize = user_key.size();
  size_t needed = usize + 21;  // varint:5, seq/tag:8, ValidTime:8
  char* dst;
  if (needed <= sizeof(space_)) {
    dst = space_;
  } else {
    dst = new char[needed];
  }
  start_ = dst;
  dst = EncodeVarint32(dst, usize + 16);
  kstart_ = dst;
  std::memcpy(dst, user_key.data(), usize);
  dst += usize;
  EncodeFixed64(dst, PackSequenceAndType(s, kValueTypeForSeek));
  dst += 8;
  EncodeFixed64(dst, t);
  dst += 8;
  end_ = dst;
}

}  // namespace leveldb
