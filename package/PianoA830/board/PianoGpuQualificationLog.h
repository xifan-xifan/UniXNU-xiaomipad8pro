/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Leo Zhang
 * Bounded qualification evidence, independent of console and unified-log delivery.
 * Caller serializes push/bounds/copy. Never hold that lock over a user copy.
 */
#ifndef PIANO_GPU_QUALIFICATION_LOG_H
#define PIANO_GPU_QUALIFICATION_LOG_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
namespace pianogpu {
template<unsigned Capacity=8192,unsigned Bytes=960> struct QualificationLog {
    static constexpr unsigned capacity=Capacity, bytes=Bytes;
    struct Bounds {uint64_t first,next,rejected;};
    struct Record {uint16_t length;char text[Bytes];};
    Record records[Capacity]={};
    uint64_t next=0,rejected=0;
    Bounds bounds() const {return {next>Capacity?next-Capacity:0,next,rejected};}
    bool push(const char *text) {
        const size_t length=strnlen(text,Bytes);
        if(!length || length>=Bytes || memchr(text,'\n',length) || memchr(text,'\r',length)) {++rejected;return false;}
        Record &r=records[next%Capacity];r.length=uint16_t(length);memcpy(r.text,text,length+1);++next;return true;
    }
    bool copy(uint64_t sequence,Record &out) const {
        const Bounds b=bounds();
        if(sequence<b.first || sequence>=b.next)return false;
        out=records[sequence%Capacity];return true;
    }
};
}
#endif
