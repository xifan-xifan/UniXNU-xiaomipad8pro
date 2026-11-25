/* SPDX-License-Identifier: MIT */
#include "ArmSmmu4KPaged.h"
#define PAGE UINT64_C(4096)
#define PA_MASK UINT64_C(0x0000fffffffff000)

typedef ArmSmmu4KPagedSpace Space;
typedef ArmSmmu4KPagedReservation Reservation;

static int output_width(unsigned bits)
{return bits==32 || bits==36 || bits==40 || bits==42 || bits==44 || bits==48;}
static unsigned shift_of(unsigned level) {return 39u-9u*level;}
static unsigned width_of(const Space *s,unsigned level) {return level==s->start_level?s->config.input_bits-shift_of(level):9u;}
static unsigned index_of(const Space *s,unsigned level,uint64_t va)
{return (unsigned)((va>>shift_of(level))&((UINT64_C(1)<<width_of(s,level))-1));}
static unsigned entries_of(const Space *s,unsigned level) {return 1u<<width_of(s,level);}
static int bit(const uint64_t *set,unsigned i) {return (int)((set[i>>6]>>(i&63))&1u);}
static void set_bit(uint64_t *set,unsigned i) {set[i>>6]|=UINT64_C(1)<<(i&63);}
static int valid_attrs(const ArmSmmu4KPagedAttrs *a)
{
    return a && (a->permissions&ArmSmmu4KRead) && !(a->permissions&~15u) && a->writeback<=1 &&
        (a->shareability==2 || a->shareability==3);
}
uint64_t ArmSmmu4KPagedLeaf(const ArmSmmu4KPagedAttrs *a,uint64_t physical)
{
    uint64_t attrs=(1u<<11)|(1u<<10)|3u|((uint64_t)a->shareability<<8);
    if(!(a->permissions&ArmSmmu4KExecute))attrs|=UINT64_C(3)<<53;
    if(!(a->permissions&ArmSmmu4KPrivileged))attrs|=1u<<6;
    if(!(a->permissions&ArmSmmu4KWrite))attrs|=1u<<7;
    if(a->writeback)attrs|=1u<<2;
    return physical|attrs;
}
/* A table descriptor is exactly PA|3 of a page the allocator gave (the only form this kit and ArmSmmu4KBuild write). */
static volatile uint64_t *table_at(const Space *s,uint64_t entry)
{
    const uint64_t physical=entry&PA_MASK;
    if(entry!=(physical|3u))return 0;
    return s->pages.cpu(s->pages.cookie,physical);
}
static void count_take(Space *s,unsigned level)
{
    ArmSmmu4KPagedCounters *c=&s->counters;
    ++c->table_pages;++c->level_pages[level];
    if(c->table_pages>c->table_pages_peak)c->table_pages_peak=c->table_pages;
}
static void count_drop(Space *s,unsigned level) {--s->counters.table_pages;--s->counters.level_pages[level];}
static void zero_page(volatile uint64_t *page) {for(unsigned i=0;i<512;++i)page[i]=0;}
static int publish(Space *s,volatile uint64_t *page)
{
    ++s->counters.published_pages;
    if(!s->pages.publish(s->pages.cookie,page,PAGE))return 0;
    s->poisoned=1;return 1;
}
/* One page from the allocator, checked: 4 KiB aligned on both sides and inside the output size. */
static int allocate_page(Space *s,uint64_t *physical,volatile uint64_t **cpu)
{
    uint64_t p=0;volatile uint64_t *v=0;
    ++s->counters.allocations;
    if(s->pages.allocate(s->pages.cookie,&p,&v)) {++s->counters.allocation_failures;return 1;}
    if(!v || (p&(PAGE-1)) || ((uintptr_t)v&(PAGE-1)) || p>=(UINT64_C(1)<<s->config.output_bits)) {
        s->pages.release(s->pages.cookie,p,v);++s->counters.allocation_failures;return 1;
    }
    *physical=p;*cpu=v;return 0;
}
/* [iova, iova + bytes): page aligned, inside the space's range and in none of its linked shared slots. */
static ArmSmmu4KStatus check_range(const Space *s,uint64_t iova,uint64_t bytes)
{
    if(!bytes || ((iova|bytes)&(PAGE-1)))return ArmSmmu4KBadRange;
    const uint64_t start=s->config.iova_start,end=s->config.iova_end;
    if(iova<start || iova>=end || bytes>end-iova)return ArmSmmu4KBadRange;
    const unsigned shift=shift_of(s->start_level);
    for(uint64_t slot=iova>>shift;slot<=((iova+bytes-1)>>shift);++slot)
        if(bit(s->foreign,(unsigned)slot))return ArmSmmu4KOverlap;
    return ArmSmmu4KSuccess;
}
/* The tables a missing subtree below a level-`level` entry needs for [lo, hi): one per level-m table it touches, m > level. */
static size_t missing_tables(unsigned level,uint64_t lo,uint64_t hi)
{
    size_t n=0;
    for(unsigned m=level+1;m<=3;++m) {const unsigned shift=shift_of(m-1);n+=(size_t)(((hi-1)>>shift)-(lo>>shift)+1);}
    return n;
}
ArmSmmu4KStatus ArmSmmu4KPagedInit(Space *s,const ArmSmmu4KPagedConfig *config,const ArmSmmu4KPagedPages *pages)
{
    if(!s || !config || !pages || s->prepared || s->poisoned || s->root || !pages->allocate || !pages->release || !pages->cpu ||
        !pages->publish || config->input_bits<32 || config->input_bits>48 || !output_width(config->output_bits))
        return ArmSmmu4KArgument;
    const uint64_t limit=UINT64_C(1)<<config->input_bits,end=config->iova_end?config->iova_end:limit;
    if(((config->iova_start|end)&(PAGE-1)) || config->iova_start>=end || end>limit)return ArmSmmu4KBadRange;
    s->config=*config;s->config.iova_end=end;s->pages=*pages;
    s->start_level=4-(config->input_bits-12+8)/9;
    s->root_entries=entries_of(s,s->start_level);
    uint64_t physical;volatile uint64_t *root;
    if(allocate_page(s,&physical,&root))return ArmSmmu4KCapacity;
    zero_page(root);
    if(s->pages.publish(s->pages.cookie,root,PAGE)) {s->pages.release(s->pages.cookie,physical,root);return ArmSmmu4KIoError;}
    ++s->counters.published_pages;
    s->root=root;s->root_physical=physical;s->prepared=1;
    count_take(s,s->start_level);
    return ArmSmmu4KSuccess;
}
size_t ArmSmmu4KPagedWorstCase(const Space *s,uint64_t iova,uint64_t bytes)
{
    if(!s || !s->prepared || !bytes || bytes>UINT64_MAX-iova)return 0;
    return missing_tables(s->start_level,iova,iova+bytes);
}
static ArmSmmu4KStatus need_walk(const Space *s,const volatile uint64_t *table,unsigned level,uint64_t lo,uint64_t hi,
    size_t *need,uint64_t *reads)
{
    if(level==3) {
        for(unsigned i=index_of(s,3,lo),last=index_of(s,3,hi-1);i<=last;++i) {++*reads;if(table[i])return ArmSmmu4KOverlap;}
        return ArmSmmu4KSuccess;
    }
    const uint64_t span=UINT64_C(1)<<shift_of(level);
    for(uint64_t va=lo;va<hi;) {
        const uint64_t next=(va|(span-1))+1,end=next<hi?next:hi;
        const uint64_t entry=table[index_of(s,level,va)];++*reads;
        if(!entry)*need+=missing_tables(level,va,end);
        else {
            const volatile uint64_t *child=table_at(s,entry);
            if(!child)return ArmSmmu4KStateMismatch;
            const ArmSmmu4KStatus status=need_walk(s,child,level+1,va,end,need,reads);
            if(status)return status;
        }
        va=end;
    }
    return ArmSmmu4KSuccess;
}
ArmSmmu4KStatus ArmSmmu4KPagedNeed(const Space *s,uint64_t iova,uint64_t bytes,size_t *pages)
{
    if(!pages)return ArmSmmu4KArgument;
    *pages=0;
    if(!s || !s->prepared || s->poisoned)return ArmSmmu4KArgument;
    ArmSmmu4KStatus status=check_range(s,iova,bytes);if(status)return status;
    size_t need=0;uint64_t reads=0;
    status=need_walk(s,s->root,s->start_level,iova,iova+bytes,&need,&reads);
    if(!status)*pages=need;
    return status;
}
/* The reservation: first in, first out, linked through each page's first word (the pages are off the tree). */
static void reservation_push_back(Space *s,Reservation *r,uint64_t physical,volatile uint64_t *cpu)
{
    cpu[0]=0;
    if(r->pages)s->pages.cpu(s->pages.cookie,r->tail)[0]=physical;else r->head=physical;
    r->tail=physical;++r->pages;
}
static void reservation_push_front(Reservation *r,uint64_t physical,volatile uint64_t *cpu)
{
    cpu[0]=r->pages?r->head:0;
    if(!r->pages)r->tail=physical;
    r->head=physical;++r->pages;
}
static volatile uint64_t *reservation_pop(Space *s,Reservation *r,uint64_t *physical)
{
    if(!r->pages)return 0;
    const uint64_t p=r->head;
    volatile uint64_t *cpu=s->pages.cpu(s->pages.cookie,p);
    if(!cpu)return 0;
    const uint64_t next=cpu[0];
    if(--r->pages)r->head=next;else r->head=r->tail=0;
    *physical=p;return cpu;
}
ArmSmmu4KStatus ArmSmmu4KPagedReserve(Space *s,size_t pages,Reservation *r)
{
    if(!s || !r || !s->prepared || s->poisoned)return ArmSmmu4KArgument;
    const uint64_t old_tail=r->tail;
    const size_t old_pages=r->pages;
    for(size_t i=0;i<pages;++i) {
        uint64_t physical;volatile uint64_t *cpu;
        if(!allocate_page(s,&physical,&cpu)) {reservation_push_back(s,r,physical,cpu);++s->counters.reserved;continue;}
        /* Give back exactly what this call took: the pages after the old tail. */
        uint64_t at=old_pages?s->pages.cpu(s->pages.cookie,old_tail)[0]:r->head;
        for(size_t k=0;k<i;++k) {
            volatile uint64_t *page=s->pages.cpu(s->pages.cookie,at);
            const uint64_t next=page[0];
            s->pages.release(s->pages.cookie,at,page);--s->counters.reserved;at=next;
        }
        if(old_pages) {s->pages.cpu(s->pages.cookie,old_tail)[0]=0;r->tail=old_tail;}
        else r->head=r->tail=0;
        r->pages=old_pages;
        return ArmSmmu4KCapacity;
    }
    return ArmSmmu4KSuccess;
}
void ArmSmmu4KPagedCancel(Space *s,Reservation *r)
{
    if(!s || !r)return;
    uint64_t physical;volatile uint64_t *cpu;
    while(r->pages && (cpu=reservation_pop(s,r,&physical))) {s->pages.release(s->pages.cookie,physical,cpu);++s->counters.cancelled;}
}
typedef struct {
    Space *s;
    Reservation *r;
    const ArmSmmu4KPagedRuns *runs;
    uint64_t leaf_bits,offset,run_physical,run_left,remaining,output_limit;
    uint64_t fail;   /* the first leaf not written when the commit stopped */
    size_t leaves;
} Commit;
static int next_physical(Commit *x,uint64_t *physical)
{
    if(!x->run_left) {
        uint64_t p=0,b=0;
        if(x->runs->run(x->runs->cookie,x->offset,&p,&b) || !b || ((p|b)&(PAGE-1)))return 1;
        if(b>x->remaining)b=x->remaining;
        if(p>=x->output_limit || b>x->output_limit-p)return 1;
        if(x->s->pages.owns && x->s->pages.owns(x->s->pages.cookie,p,b))return 1;
        x->run_physical=p;x->run_left=b;
    }
    *physical=x->run_physical;
    x->run_physical+=PAGE;x->run_left-=PAGE;x->offset+=PAGE;x->remaining-=PAGE;
    return 0;
}
/* A new subtree that never got linked goes back to the reservation whole. */
static void give_back(Commit *x,volatile uint64_t *table,uint64_t physical,unsigned level)
{
    Space *s=x->s;
    if(level<3)for(unsigned i=0;i<512;++i)if(table[i]) {
        volatile uint64_t *child=table_at(s,table[i]);
        if(child)give_back(x,child,table[i]&PA_MASK,level+1);
    }
    count_drop(s,level);reservation_push_front(x->r,physical,table);
}
/* Bottom up: a new table is filled and published (at the end of its own walk) before the descriptor linking it is written; each
 * changed table page is published once, after its last store. */
static ArmSmmu4KStatus commit_walk(Commit *x,volatile uint64_t *table,unsigned level,uint64_t lo,uint64_t hi)
{
    Space *s=x->s;
    ArmSmmu4KStatus status=ArmSmmu4KSuccess;
    int wrote=0;
    if(level==3) {
        const unsigned first=index_of(s,3,lo),last=index_of(s,3,hi-1);
        for(unsigned i=first;i<=last;++i) {
            uint64_t physical;
            if(next_physical(x,&physical)) {x->fail=lo+(uint64_t)(i-first)*PAGE;status=ArmSmmu4KBadRange;break;}
            table[i]=physical|x->leaf_bits;++s->counters.descriptor_writes;++x->leaves;wrote=1;
        }
    } else {
        const uint64_t span=UINT64_C(1)<<shift_of(level);
        for(uint64_t va=lo;va<hi;) {
            const uint64_t next=(va|(span-1))+1,end=next<hi?next:hi;
            const unsigned index=index_of(s,level,va);
            const uint64_t entry=table[index];++s->counters.descriptor_reads;
            if(entry) {
                volatile uint64_t *child=table_at(s,entry);
                if(!child) {x->fail=va;status=ArmSmmu4KStateMismatch;break;}
                status=commit_walk(x,child,level+1,va,end);
                if(status)break;
            } else {
                uint64_t physical;
                volatile uint64_t *child=reservation_pop(s,x->r,&physical);
                if(!child) {x->fail=va;status=ArmSmmu4KStateMismatch;break;}
                zero_page(child);count_take(s,level+1);
                status=commit_walk(x,child,level+1,va,end);
                if(status) {give_back(x,child,physical,level+1);break;}
                table[index]=physical|3u;++s->counters.descriptor_writes;wrote=1;
            }
            va=end;
        }
    }
    if(s->poisoned)return ArmSmmu4KIoError;
    if(wrote && publish(s,table))return ArmSmmu4KIoError;
    return status;
}
/* A rolled-back map: every leaf it wrote in [lo, hi) is cleared again; tables stay. */
static ArmSmmu4KStatus clear_leaves(Space *s,volatile uint64_t *table,unsigned level,uint64_t lo,uint64_t hi)
{
    int wrote=0;
    if(level==3) {
        for(unsigned i=index_of(s,3,lo),last=index_of(s,3,hi-1);i<=last;++i)
            if(table[i]) {table[i]=0;++s->counters.descriptor_writes;wrote=1;}
    } else {
        const uint64_t span=UINT64_C(1)<<shift_of(level);
        for(uint64_t va=lo;va<hi;) {
            const uint64_t next=(va|(span-1))+1,end=next<hi?next:hi;
            const uint64_t entry=table[index_of(s,level,va)];
            if(entry) {
                volatile uint64_t *child=table_at(s,entry);
                if(!child)return ArmSmmu4KStateMismatch;
                const ArmSmmu4KStatus status=clear_leaves(s,child,level+1,va,end);
                if(status)return status;
            }
            va=end;
        }
    }
    return wrote && publish(s,table)?ArmSmmu4KIoError:ArmSmmu4KSuccess;
}
ArmSmmu4KStatus ArmSmmu4KPagedMap(Space *s,Reservation *r,uint64_t iova,uint64_t bytes,
    const ArmSmmu4KPagedRuns *runs,const ArmSmmu4KPagedAttrs *attrs,ArmSmmu4KPagedPlan *plan)
{
    if(!plan)return ArmSmmu4KArgument;
    *plan=(ArmSmmu4KPagedPlan){0};
    if(!s || !s->prepared || s->poisoned || !r || !runs || !runs->run)return ArmSmmu4KArgument;
    if(!valid_attrs(attrs)) {++s->counters.refusals;return ArmSmmu4KBadRange;}
    ArmSmmu4KStatus status=check_range(s,iova,bytes);
    if(status) {++s->counters.refusals;return status;}
    /* Read-only first: every target leaf invalid, every missing table counted against the reservation. */
    size_t need=0;
    status=need_walk(s,s->root,s->start_level,iova,iova+bytes,&need,&s->counters.descriptor_reads);
    if(status) {++s->counters.refusals;return status;}
    if(need>r->pages) {++s->counters.refusals;return ArmSmmu4KCapacity;}
    const uint64_t tables_before=s->counters.table_pages;
    Commit x={s,r,runs,ArmSmmu4KPagedLeaf(attrs,0),0,0,0,bytes,UINT64_C(1)<<s->config.output_bits,iova,0};
    status=commit_walk(&x,s->root,s->start_level,iova,iova+bytes);
    ++s->generation;
    plan->tables_new=(size_t)(s->counters.table_pages-tables_before);
    if(status==ArmSmmu4KIoError)return status;
    if(status) {
        if(x.fail>iova) {
            const ArmSmmu4KStatus cleared=clear_leaves(s,s->root,s->start_level,iova,x.fail);
            if(cleared) {s->poisoned=1;return cleared;}
        }
        ++s->counters.rollbacks;++s->counters.plans_asid;
        plan->rolled_back=1;plan->kind=ArmSmmu4KPagedTlbAsid;
        return status;
    }
    ArmSmmu4KPagedCounters *c=&s->counters;
    c->leaves+=x.leaves;if(c->leaves>c->leaves_peak)c->leaves_peak=c->leaves;
    ++c->maps;
    plan->kind=ArmSmmu4KPagedTlbNone;plan->leaves=x.leaves;plan->iova=iova;plan->pages=bytes/PAGE;
    return ArmSmmu4KSuccess;
}
static ArmSmmu4KStatus check_walk(Space *s,const volatile uint64_t *table,unsigned level,uint64_t lo,uint64_t hi,
    const ArmSmmu4KPagedAttrs *expect,uint64_t bits)
{
    if(level==3) {
        for(unsigned i=index_of(s,3,lo),last=index_of(s,3,hi-1);i<=last;++i) {
            const uint64_t entry=table[i];++s->counters.descriptor_reads;
            if((entry&3)!=3 || (expect && (entry&~PA_MASK)!=bits))return ArmSmmu4KStateMismatch;
        }
        return ArmSmmu4KSuccess;
    }
    const uint64_t span=UINT64_C(1)<<shift_of(level);
    for(uint64_t va=lo;va<hi;) {
        const uint64_t next=(va|(span-1))+1,end=next<hi?next:hi;
        const uint64_t entry=table[index_of(s,level,va)];++s->counters.descriptor_reads;
        const volatile uint64_t *child=entry?table_at(s,entry):0;
        if(!child)return ArmSmmu4KStateMismatch;
        const ArmSmmu4KStatus status=check_walk(s,child,level+1,va,end,expect,bits);
        if(status)return status;
        va=end;
    }
    return ArmSmmu4KSuccess;
}
typedef struct {uint64_t *retired;size_t capacity,count,kept,leaves;} Retire;
static int table_empty(Space *s,const volatile uint64_t *table)
{
    for(unsigned i=0;i<512;++i) {++s->counters.descriptor_reads;if(table[i])return 0;}
    return 1;
}
/* An emptied child is unlinked into the array (a pinned root slot never is); without room it stays linked. */
static void retire_child(Space *s,volatile uint64_t *table,unsigned level,unsigned index,Retire *x,int *wrote)
{
    if(level==s->start_level && bit(s->pinned,index))return;
    if(x->count>=x->capacity) {++x->kept;return;}
    x->retired[x->count++]=table[index]&PA_MASK;
    table[index]=0;++s->counters.descriptor_writes;*wrote=1;
    count_drop(s,level+1);++s->counters.retired_pending;
}
static ArmSmmu4KStatus unmap_walk(Space *s,volatile uint64_t *table,unsigned level,uint64_t lo,uint64_t hi,Retire *x,int *empty)
{
    if(level==3) {
        const unsigned first=index_of(s,3,lo),last=index_of(s,3,hi-1);
        for(unsigned i=first;i<=last;++i) {table[i]=0;++s->counters.descriptor_writes;++x->leaves;}
        if(publish(s,table))return ArmSmmu4KIoError;
        *empty=(first==0 && last==511) || table_empty(s,table);
        return ArmSmmu4KSuccess;
    }
    int wrote=0;
    const uint64_t span=UINT64_C(1)<<shift_of(level);
    for(uint64_t va=lo;va<hi;) {
        const uint64_t next=(va|(span-1))+1,end=next<hi?next:hi;
        const unsigned index=index_of(s,level,va);
        volatile uint64_t *child=table_at(s,table[index]);
        if(!child)return ArmSmmu4KStateMismatch;
        int child_empty=0;
        const ArmSmmu4KStatus status=unmap_walk(s,child,level+1,va,end,x,&child_empty);
        if(status)return status;
        if(child_empty)retire_child(s,table,level,index,x,&wrote);
        va=end;
    }
    if(wrote && publish(s,table))return ArmSmmu4KIoError;
    /* It still holds the entry this walk came through unless that child was unlinked. */
    *empty=wrote && level!=s->start_level && table_empty(s,table);
    return ArmSmmu4KSuccess;
}
static void finish_plan(Space *s,ArmSmmu4KPagedPlan *plan,const Retire *x,uint64_t iova)
{
    plan->leaves=x->leaves;plan->retired=x->count;plan->kept_empty=x->kept;
    if(x->count) {plan->kind=ArmSmmu4KPagedTlbAsid;plan->walk=1;++s->counters.plans_asid;++s->counters.plans_walk;}
    else if(!x->leaves)plan->kind=ArmSmmu4KPagedTlbNone;
    else if(x->leaves<=ArmSmmu4KPagedTlbPageLimit) {
        plan->kind=ArmSmmu4KPagedTlbPages;plan->iova=iova;plan->pages=x->leaves;++s->counters.plans_pages;
    } else {plan->kind=ArmSmmu4KPagedTlbAsid;++s->counters.plans_asid;}
}
ArmSmmu4KStatus ArmSmmu4KPagedUnmap(Space *s,uint64_t iova,uint64_t bytes,const ArmSmmu4KPagedAttrs *expect,
    uint64_t *retired,size_t capacity,ArmSmmu4KPagedPlan *plan)
{
    if(!plan)return ArmSmmu4KArgument;
    *plan=(ArmSmmu4KPagedPlan){0};
    if(!s || !s->prepared || s->poisoned || (capacity && !retired) || (expect && !valid_attrs(expect)))return ArmSmmu4KArgument;
    ArmSmmu4KStatus status=check_range(s,iova,bytes);
    if(status) {++s->counters.refusals;return status;}
    status=check_walk(s,s->root,s->start_level,iova,iova+bytes,expect,expect?ArmSmmu4KPagedLeaf(expect,0):0);
    if(status) {++s->counters.refusals;return status;}
    Retire x={retired,capacity,0,0,0};
    int empty=0;
    status=unmap_walk(s,s->root,s->start_level,iova,iova+bytes,&x,&empty);
    ++s->generation;
    if(status) {s->poisoned=1;return status;}
    s->counters.leaves-=x.leaves;++s->counters.unmaps;
    finish_plan(s,plan,&x,iova);
    return ArmSmmu4KSuccess;
}
static ArmSmmu4KStatus trim_walk(Space *s,volatile uint64_t *table,unsigned level,Retire *x,int *empty)
{
    if(level==3) {*empty=table_empty(s,table);return ArmSmmu4KSuccess;}
    int wrote=0;
    for(unsigned i=0,n=entries_of(s,level);i<n;++i) {
        const uint64_t entry=table[i];++s->counters.descriptor_reads;
        if(!entry || (level==s->start_level && bit(s->foreign,i)))continue;
        volatile uint64_t *child=table_at(s,entry);
        if(!child)return ArmSmmu4KStateMismatch;
        int child_empty=0;
        const ArmSmmu4KStatus status=trim_walk(s,child,level+1,x,&child_empty);
        if(status)return status;
        if(child_empty)retire_child(s,table,level,i,x,&wrote);
    }
    if(wrote && publish(s,table))return ArmSmmu4KIoError;
    *empty=level!=s->start_level && table_empty(s,table);
    return ArmSmmu4KSuccess;
}
ArmSmmu4KStatus ArmSmmu4KPagedTrim(Space *s,uint64_t *retired,size_t capacity,ArmSmmu4KPagedPlan *plan)
{
    if(!plan)return ArmSmmu4KArgument;
    *plan=(ArmSmmu4KPagedPlan){0};
    if(!s || !s->prepared || s->poisoned || (capacity && !retired))return ArmSmmu4KArgument;
    Retire x={retired,capacity,0,0,0};
    int empty=0;
    const ArmSmmu4KStatus status=trim_walk(s,s->root,s->start_level,&x,&empty);
    ++s->generation;++s->counters.trims;
    if(status) {s->poisoned=1;return status;}
    finish_plan(s,plan,&x,0);
    return ArmSmmu4KSuccess;
}
/* Trim restricted to the tables that cover [lo, hi): each child is visited only for the part of the range below it; whether a table is
 * empty is decided over the whole table. */
static ArmSmmu4KStatus trim_range_walk(Space *s,volatile uint64_t *table,unsigned level,uint64_t lo,uint64_t hi,Retire *x,int *empty)
{
    if(level==3) {*empty=table_empty(s,table);return ArmSmmu4KSuccess;}
    int wrote=0;
    const uint64_t span=UINT64_C(1)<<shift_of(level);
    for(uint64_t va=lo;va<hi;) {
        const uint64_t next=(va|(span-1))+1,end=next<hi?next:hi;
        const unsigned index=index_of(s,level,va);
        const uint64_t entry=table[index];++s->counters.descriptor_reads;
        if(entry && !(level==s->start_level && bit(s->foreign,index))) {
            volatile uint64_t *child=table_at(s,entry);
            if(!child)return ArmSmmu4KStateMismatch;
            int child_empty=0;
            const ArmSmmu4KStatus status=trim_range_walk(s,child,level+1,va,end,x,&child_empty);
            if(status)return status;
            if(child_empty)retire_child(s,table,level,index,x,&wrote);
        }
        if(!next)break;   /* the last slot of a 64-bit input */
        va=end;
    }
    if(wrote && publish(s,table))return ArmSmmu4KIoError;
    *empty=level!=s->start_level && table_empty(s,table);
    return ArmSmmu4KSuccess;
}
ArmSmmu4KStatus ArmSmmu4KPagedTrimRange(Space *s,uint64_t iova,uint64_t bytes,uint64_t *retired,size_t capacity,ArmSmmu4KPagedPlan *plan)
{
    if(!plan)return ArmSmmu4KArgument;
    *plan=(ArmSmmu4KPagedPlan){0};
    if(!s || !s->prepared || s->poisoned || (capacity && !retired))return ArmSmmu4KArgument;
    if(!bytes || ((iova|bytes)&(PAGE-1)) || iova>=s->config.iova_end || bytes>s->config.iova_end-iova)return ArmSmmu4KBadRange;
    Retire x={retired,capacity,0,0,0};
    int empty=0;
    const ArmSmmu4KStatus status=trim_range_walk(s,s->root,s->start_level,iova,iova+bytes,&x,&empty);
    ++s->generation;++s->counters.trims;
    if(status) {s->poisoned=1;return status;}
    finish_plan(s,plan,&x,0);
    return ArmSmmu4KSuccess;
}
void ArmSmmu4KPagedRelease(Space *s,const uint64_t *retired,size_t count)
{
    if(!s || !retired)return;
    for(size_t i=0;i<count;++i) {
        s->pages.release(s->pages.cookie,retired[i],s->pages.cpu(s->pages.cookie,retired[i]));
        --s->counters.retired_pending;++s->counters.released;
    }
}
ArmSmmu4KStatus ArmSmmu4KPagedShare(Space *owner,unsigned index,uint64_t *table_physical)
{
    if(!table_physical)return ArmSmmu4KArgument;
    *table_physical=0;
    if(!owner || !owner->prepared || owner->poisoned || index>=owner->root_entries || bit(owner->foreign,index))
        return ArmSmmu4KArgument;
    const uint64_t entry=owner->root[index];
    if(entry) {
        if(!table_at(owner,entry))return ArmSmmu4KStateMismatch;
        set_bit(owner->pinned,index);*table_physical=entry&PA_MASK;return ArmSmmu4KSuccess;
    }
    uint64_t physical;volatile uint64_t *table;
    if(allocate_page(owner,&physical,&table))return ArmSmmu4KCapacity;
    zero_page(table);
    if(owner->pages.publish(owner->pages.cookie,table,PAGE)) {owner->pages.release(owner->pages.cookie,physical,table);return ArmSmmu4KIoError;}
    ++owner->counters.published_pages;
    owner->root[index]=physical|3u;++owner->counters.descriptor_writes;count_take(owner,owner->start_level+1);
    ++owner->generation;
    if(publish(owner,owner->root))return ArmSmmu4KIoError;
    set_bit(owner->pinned,index);*table_physical=physical;
    return ArmSmmu4KSuccess;
}
ArmSmmu4KStatus ArmSmmu4KPagedLinkShared(Space *s,unsigned index,uint64_t table_physical)
{
    if(!s || !s->prepared || s->poisoned || index>=s->root_entries || bit(s->pinned,index) || bit(s->foreign,index) ||
        (table_physical&(PAGE-1)))return ArmSmmu4KArgument;
    if(s->root[index])return ArmSmmu4KOverlap;
    if(!s->pages.cpu(s->pages.cookie,table_physical))return ArmSmmu4KBadRange;
    s->root[index]=table_physical|3u;++s->counters.descriptor_writes;++s->generation;
    if(publish(s,s->root))return ArmSmmu4KIoError;
    set_bit(s->foreign,index);
    return ArmSmmu4KSuccess;
}
ArmSmmu4KStatus ArmSmmu4KPagedLookup(const Space *s,uint64_t iova,uint64_t *descriptor)
{
    if(!descriptor)return ArmSmmu4KArgument;
    *descriptor=0;
    if(!s || !s->prepared)return ArmSmmu4KArgument;
    if(iova>>s->config.input_bits)return ArmSmmu4KBadRange;
    const volatile uint64_t *table=s->root;
    for(unsigned level=s->start_level;level<3;++level) {
        const uint64_t entry=table[index_of(s,level,iova)];
        if(!entry)return ArmSmmu4KSuccess;
        if(!(table=table_at(s,entry)))return ArmSmmu4KStateMismatch;
    }
    *descriptor=table[index_of(s,3,iova)];
    return ArmSmmu4KSuccess;
}
typedef struct {
    const Space *s;
    const ArmSmmu4KPagedRecord *records;
    size_t count,at;
    uint64_t done,run_physical,run_left;   /* pages of records[at] seen; its current run */
    size_t tables;
    ArmSmmu4KPagedAuditResult *r;
} Audit;
static int audit_bad(Audit *a,ArmSmmu4KPagedAuditWhy why,uint64_t iova) {a->r->why=why;a->r->iova=iova;return 1;}
static int audit_leaf(Audit *a,uint64_t va,uint64_t entry)
{
    while(a->at<a->count && a->done*PAGE==a->records[a->at].bytes) {++a->at;a->done=0;a->run_left=0;}
    if(a->at>=a->count || va<a->records[a->at].iova)return audit_bad(a,ArmSmmu4KPagedAuditStrayLeaf,va);
    const ArmSmmu4KPagedRecord *record=&a->records[a->at];
    const uint64_t want_va=record->iova+a->done*PAGE;
    if(va!=want_va)return audit_bad(a,ArmSmmu4KPagedAuditMissingLeaf,want_va);
    if(!a->run_left) {
        uint64_t p=0,b=0;
        if(record->runs->run(record->runs->cookie,a->done*PAGE,&p,&b) || !b || ((p|b)&(PAGE-1)))
            return audit_bad(a,ArmSmmu4KPagedAuditRuns,va);
        a->run_physical=p;a->run_left=b;
    }
    const uint64_t want=ArmSmmu4KPagedLeaf(&record->attrs,a->run_physical);
    a->run_physical+=PAGE;a->run_left-=PAGE;++a->done;
    return entry!=want?audit_bad(a,ArmSmmu4KPagedAuditWrongLeaf,va):0;
}
static int audit_walk(Audit *a,const volatile uint64_t *table,unsigned level,uint64_t base)
{
    const Space *s=a->s;
    const unsigned shift=shift_of(level);
    size_t used=0;
    for(unsigned i=0,n=entries_of(s,level);i<n;++i) {
        const uint64_t entry=table[i],va=base+((uint64_t)i<<shift);
        ++a->r->descriptor_reads;
        if(!entry)continue;
        ++used;
        if(level==3) {++a->r->leaves;if(audit_leaf(a,va,entry))return 1;continue;}
        if(level==s->start_level && bit(s->foreign,i)) {
            if(entry!=((entry&PA_MASK)|3u))return audit_bad(a,ArmSmmu4KPagedAuditBadTable,va);
            continue;
        }
        const volatile uint64_t *child=table_at(s,entry);
        if(!child || (s->pages.owns && !s->pages.owns(s->pages.cookie,entry&PA_MASK,PAGE)))
            return audit_bad(a,ArmSmmu4KPagedAuditBadTable,va);
        ++a->tables;
        if(audit_walk(a,child,level+1,va))return 1;
    }
    if(!used && level!=s->start_level)++a->r->empty_tables;
    return 0;
}
ArmSmmu4KStatus ArmSmmu4KPagedAudit(const Space *s,const ArmSmmu4KPagedRecord *records,size_t count,ArmSmmu4KPagedAuditResult *r)
{
    if(!r)return ArmSmmu4KArgument;
    *r=(ArmSmmu4KPagedAuditResult){0};
    if(!s || !s->prepared || (count && !records))return ArmSmmu4KArgument;
    for(size_t i=0;i<count;++i) {
        const ArmSmmu4KPagedRecord *record=&records[i];
        if(!record->runs || !record->runs->run || !valid_attrs(&record->attrs) || check_range(s,record->iova,record->bytes) ||
            (i && record->iova<records[i-1].iova+records[i-1].bytes))return ArmSmmu4KArgument;
    }
    Audit a={s,records,count,0,0,0,0,1,r};
    if(audit_walk(&a,s->root,s->start_level,0))return ArmSmmu4KStateMismatch;
    for(;a.at<count;++a.at,a.done=0)
        if(a.done*PAGE!=records[a.at].bytes) {audit_bad(&a,ArmSmmu4KPagedAuditMissingLeaf,records[a.at].iova+a.done*PAGE);return ArmSmmu4KStateMismatch;}
    r->tables=a.tables;
    if(a.tables!=s->counters.table_pages) {audit_bad(&a,ArmSmmu4KPagedAuditTableCount,0);return ArmSmmu4KStateMismatch;}
    if(r->leaves!=s->counters.leaves) {audit_bad(&a,ArmSmmu4KPagedAuditLeafCount,0);return ArmSmmu4KStateMismatch;}
    return ArmSmmu4KSuccess;
}
static void free_walk(Space *s,volatile uint64_t *table,unsigned level,uint64_t physical)
{
    /* Each entry is cleared before its table goes back: no page is released while a table still names it. */
    if(level<3)for(unsigned i=0,n=entries_of(s,level);i<n;++i) {
        const uint64_t entry=table[i];
        if(!entry)continue;
        table[i]=0;
        if(level==s->start_level && bit(s->foreign,i))continue;
        volatile uint64_t *child=table_at(s,entry);
        if(child)free_walk(s,child,level+1,entry&PA_MASK);
    }
    count_drop(s,level);s->pages.release(s->pages.cookie,physical,table);
}
ArmSmmu4KStatus ArmSmmu4KPagedTeardown(Space *s)
{
    if(!s || !s->prepared)return ArmSmmu4KArgument;
    free_walk(s,s->root,s->start_level,s->root_physical);
    s->root=0;s->root_physical=0;s->prepared=0;s->counters.leaves=0;
    for(unsigned i=0;i<8;++i)s->pinned[i]=s->foreign[i]=0;
    return ArmSmmu4KSuccess;
}
