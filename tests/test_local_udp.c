/* Standalone local datagram contract test; no desktop or game assets. */
#include "../src/runtime_udp.c"
#include <assert.h>
#include <time.h>
static int32_t error_value;
int32_t *runtime_errno(void) { return &error_value; }
int32_t runtime_guest_errno(int e) { return e==EAGAIN ? 35 : e==ENETUNREACH ? 51 : e; }
int runtime_file_adopt(int h) { return h; }
int runtime_file_host_fd(int fd) { return fd; }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
static int64_t micros(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000;
}
int main(void) {
    unsetenv("BB_LOCAL_UDP"); assert(!runtime_udp_resolve("socket"));
    setenv("BB_LOCAL_UDP","1",1); assert(runtime_udp_resolve("socket"));
    int receiver=udp_socket(2,6,0), sender=udp_socket(2,2,0);
    assert(receiver>=0 && sender>=0);
    Address a={.len=16,.family=2,.addr=htonl(INADDR_LOOPBACK)};
    assert(!udp_bind(receiver,&a,sizeof(a)));
    uint32_t len=sizeof(a); assert(!udp_name(receiver,&a,&len));
    assert(len==16 && a.family==2 && a.port);
    int one=1; assert(!udp_option(receiver,0xffff,0x1200,&one,sizeof(one)));
    char bytes[32]; assert(udp_recv(receiver,bytes,sizeof(bytes),0)==-1 && error_value==35);
    int ep=ep_create("test",0); assert(ep>0);
    Event event={.events=1,.data=UINT64_C(0x12345678abcdef01)}, got;
    assert(!ep_control(ep,1,receiver,&event));
    assert(udp_sendto(sender,"profile",7,0,&a,sizeof(a))==7);
    assert(ep_wait(ep,&got,1,100000)==1);
    assert(got.events==1 && got.ident==(uint64_t)receiver && got.data==event.data);
    Address peer={0}; len=sizeof(peer);
    assert(udp_recvfrom(receiver,bytes,sizeof(bytes),0,&peer,&len)==7);
    assert(!memcmp(bytes,"profile",7) && peer.family==2 && peer.port);
    assert(!ep_control(ep,3,receiver,NULL));
    int64_t start=micros(); assert(ep_wait(ep,&got,1,20000)==0);
    assert(micros()-start>=19000);
    assert(!ep_destroy(ep)); assert(ep_destroy(ep)<0);
    a.addr=htonl(0x08080808); assert(udp_sendto(sender,"x",1,0,&a,sizeof(a))==-1 && error_value==51);
    close(sender); close(receiver);
    puts("local UDP: loopback, P2P type, nonblocking errno, event ABI, timeout and offline boundary passed");
    return 0;
}
