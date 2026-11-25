/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Leo Zhang
 */
#include "PianoGpuQualificationLog.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line%d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
int main() {
    pianogpu::QualificationLog<3> log;
    decltype(log)::Record r;
    CHECK(!log.copy(0,r));
    char text[960];memset(text,'x',sizeof text);text[959]=0;
    const char tail[]="opened-us=123 failed-us=0 inhibit=4";
    memcpy(text+900,tail,sizeof tail-1);
    text[900+sizeof tail-1]=' '; // preserve the full959-byte test record
    CHECK(log.push(text) && log.copy(0,r) && r.length==959 && !strcmp(text,r.text));
    CHECK(strstr(r.text,"failed-us=0 inhibit=4"));
    text[959]='x';CHECK(!log.push(text) && log.bounds().rejected==1);
    CHECK(!log.push("bad\nrecord") && !log.push("bad\rrecord") && !log.push(""));
    CHECK(log.bounds().next==1 && log.bounds().rejected==4);
    const auto end=log.bounds();
    CHECK(log.push("second") && log.push("third"));
    CHECK(end.next==1 && log.copy(0,r) && r.length==959); // reader's fixed end excludes later records
    CHECK(log.push("fourth") && !log.copy(0,r)); // overwrite while a reader holds an older bounds snapshot
    CHECK(log.bounds().first==1 && log.bounds().next==4 && log.copy(3,r) && !strcmp(r.text,"fourth"));
    CHECK(log.copy(1,r) && !strcmp(r.text,"second") && !log.copy(4,r));
    std::puts("PASS: bounded C5LOG: long tails, exact size, rejected records, fixed end, wrap and overwritten-reader refusal");
}
