// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include "ConcurrentDirect.h"

// All record-list operations hold the admission gate. A record belongs to its
// direction's pool until I/O and its callback have both finished.
template<class Buffer, class Completion> struct DirectAsync {
  static constexpr unsigned kRecordsPerDirection = 64;

  struct Record : DirectAdmission::Request {
    Record() : Request(0, 1, false) {}
    Record *link = nullptr;
    Buffer *buffer = nullptr;
    Completion completion = {};
    uint64_t offset = 0, length = 0, actual = 0;
    uint64_t accepted = 0, started = 0, finished = 0;
    int status = 0;
  };

  void initialize() {
    for (unsigned i = 0; i < kRecordsPerDirection; ++i) {
      reclaim(records[0][i]);
      records[1][i].write = true;
      reclaim(records[1][i]);
    }
  }

  Record *reserve(bool write) {
    auto *record = free[write];
    if (record) free[write] = record->link;
    return record;
  }
  void reclaim(Record &record) {
    record.link = free[record.write];
    free[record.write] = &record;
  }
  void complete(Record &record) {
    record.link = nullptr;
    if (tail) tail->link = &record; else head = &record;
    tail = &record;
  }
  Record *nextCompletion() {
    auto *record = head;
    if (record) {
      head = record->link;
      if (!head) tail = nullptr;
    }
    return record;
  }

private:
  Record records[2][kRecordsPerDirection];
  Record *free[2] = {};
  Record *head = nullptr, *tail = nullptr;
};
