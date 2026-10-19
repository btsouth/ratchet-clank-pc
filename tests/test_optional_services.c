#define _GNU_SOURCE
#include "runtime.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
void runtime_restart(void) { abort(); }
void runtime_guest_frames(uintptr_t a,uintptr_t b) { (void)a; (void)b; abort(); }
int main(void) {
    runtime_start(1);
    void *(ABI *self)(void)=(void *(ABI *)(void))runtime_thread_resolve("aI+OeCz8xrQ#p#J");
    int32_t (ABI *get)(void*,int*,int*)=(int32_t (ABI *)(void*,int*,int*))runtime_thread_resolve("P41kTWUS3EI#p#J");
    int32_t (ABI *set)(void*,int,const int*)=(int32_t (ABI *)(void*,int,const int*))runtime_thread_resolve("oIRFTjoILbg#p#J");
    assert(self && get && set);
    void *thread=self(); int policy=-1,priority=-1;
    assert(!get(thread,&policy,&priority));
    int saved_policy=policy,saved_priority=priority,new_priority=450;
    assert(!set(thread,2,&new_priority)); assert(!get(thread,&policy,&priority));
    assert(policy==2 && priority==450);
    assert((uint32_t)get(thread,NULL,&priority)==0x80020016);
    assert((uint32_t)set(thread,0,&new_priority)==0x80020016);
    assert(!set(thread,saved_policy,&saved_priority));
    int32_t (ABI *state)(int,int,uint32_t*,uint32_t*)=(int32_t (ABI *)(int,int,uint32_t*,uint32_t*))runtime_services_resolve("LHuSmO3SLd8#l#m");
    int32_t (ABI *unlock)(int,int,int,int*)=(int32_t (ABI *)(int,int,int,int*))runtime_services_resolve("28xmRUFao68#l#m");
    assert(state && unlock);
    uint32_t flags[4]={UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX},count=99;
    assert(!state(1,1,flags,&count)); assert(!count && !(flags[0]|flags[1]|flags[2]|flags[3]));
    int platinum=99; assert(!unlock(1,1,42,&platinum)); assert(!unlock(1,1,42,&platinum));
    assert(!state(1,1,flags,&count)); assert(count==1 && flags[1]==(1u<<10) && platinum==-1);
    assert(state(1,1,NULL,&count)<0);
    int32_t (ABI *cs_init)(const void*)=(int32_t (ABI *)(const void*))runtime_services_resolve("dPj4ZtRcIWk#s#t");
    int32_t (ABI *cs_update)(int64_t*)=(int32_t (ABI *)(int64_t*))runtime_services_resolve("PD5bnBTsv6Y#s#t");
    int32_t (ABI *cs_index)(uint32_t*)=(int32_t (ABI *)(uint32_t*))runtime_services_resolve("FRT4EYtZU1Y#s#t");
    assert(cs_init && cs_update && cs_index);
    uint64_t cs_param=0x80000; int64_t update=-1; uint32_t index=UINT32_MAX;
    assert(!cs_init(&cs_param) && !cs_update(&update) && !cs_index(&index));
    assert(update==0 && index==0);
    int32_t (ABI *sd_init)(void)=(int32_t (ABI *)(void))runtime_services_resolve("s9e3+YpRnzw#d#e");
    int32_t (ABI *sd_open)(const void*)=(int32_t (ABI *)(const void*))runtime_services_resolve("4tPhsP6FpDI#d#e");
    int32_t (ABI *sd_result)(int32_t*)=(int32_t (ABI *)(int32_t*))runtime_services_resolve("yEiJ-qqr6Cg#d#e");
    int32_t (ABI *sd_ready)(void)=(int32_t (ABI *)(void))runtime_services_resolve("en7gNVnh878#d#e");
    int32_t (ABI *sd_close)(const void*)=(int32_t (ABI *)(const void*))runtime_services_resolve("fH46Lag88XY#d#e");
    int32_t (ABI *sd_progress)(int32_t,uint32_t)=(int32_t (ABI *)(int32_t,uint32_t))runtime_services_resolve("hay1CfTmLyA#d#e");
    int32_t (ABI *sd_term)(void)=(int32_t (ABI *)(void))runtime_services_resolve("YuH2FA7azqQ#d#e");
    assert(sd_init && sd_open && sd_result && sd_ready && sd_close && sd_progress && sd_term);
    int32_t sd[8]={7,-1,-1,0,0x1234,0,0,0};
    assert(!sd_init() && sd_ready()==1);
    assert((uint32_t)sd_result(sd)==0x80B80005); /* not finished before it is opened */
    assert(!sd_open(NULL) && !sd_progress(0,50) && !sd_close(NULL));
    assert(!sd_result(sd) && sd[0]==7 && sd[1]==0 && sd[2]==1 && sd[4]==0x1234);
    assert(!sd_term());
    puts("optional services: scheduler round-trip, trophy bit/count, content search and save data dialog contracts passed");
}
