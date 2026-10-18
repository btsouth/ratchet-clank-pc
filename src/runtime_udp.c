/* Optional local IPv4 datagrams for games that use sockets between their own
 * host and client even offline. BB_LOCAL_UDP=1; PSN/HTTP keep their offline model.
 * Guest descriptors share the file table so close() and descriptor reuse work. */
#define _GNU_SOURCE
#include "runtime.h"
#ifndef _WIN32
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct { uint8_t len,family; uint16_t port; uint32_t addr; uint8_t zero[8]; } Address;
typedef struct { uint32_t events,pad; uint64_t ident,data; } Event;
static int64_t failure(void) { *runtime_errno()=runtime_guest_errno(errno); return -1; }
static int host(int fd) {
    int h=runtime_file_host_fd(fd);
    if (h<0) errno=EBADF;
    return h;
}
static int address(const Address *a,uint32_t n,struct sockaddr_in *out) {
    if (!a || n<sizeof(*a) || a->family!=2) { errno=EINVAL; return -1; }
    /* This transport is intentionally local. Offline NetCtl's synthetic LAN
     * address also names this console, not another machine on the host LAN. */
    uint32_t ip=ntohl(a->addr);
    if (ip && (ip>>24)!=127 && ip!=0xc0a80002) { errno=ENETUNREACH; return -1; }
    memset(out,0,sizeof(*out)); out->sin_family=AF_INET; out->sin_port=a->port;
    out->sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    return 0;
}
static void guest_address(const struct sockaddr_in *a,Address *out,uint32_t *n) {
    Address g={.len=sizeof(g),.family=2,.port=a->sin_port,.addr=a->sin_addr.s_addr};
    uint32_t copy=*n<sizeof(g) ? *n : sizeof(g);
    memcpy(out,&g,copy); *n=sizeof(g);
}
static int flags(int f) {
    int out=MSG_NOSIGNAL;
    if (f&2) out|=MSG_PEEK;
    if (f&0x40) out|=MSG_WAITALL;
    if (f&0x80) out|=MSG_DONTWAIT;
    return out;
}
static ABI int udp_socket(int af,int type,int protocol) {
    if (af!=2 || (type!=2 && type!=6) || protocol) { errno=EPROTONOSUPPORT; return (int)failure(); }
    int h=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);
    if (h<0) return (int)failure();
    int fd=runtime_file_adopt(h);
    if (fd<0) { close(h); errno=EMFILE; return (int)failure(); }
    printf("Runtime: local UDP socket type=%d -> %d\n",type,fd);
    return fd;
}
static ABI int udp_bind(int fd,const Address *a,uint32_t n) {
    struct sockaddr_in s; int h=host(fd);
    if (h<0 || address(a,n,&s) || bind(h,(struct sockaddr*)&s,sizeof(s))) return (int)failure();
    printf("Runtime: local UDP bind fd=%d port=%u\n",fd,ntohs(s.sin_port));
    return 0;
}
static ABI int udp_connect(int fd,const Address *a,uint32_t n) {
    struct sockaddr_in s; int h=host(fd);
    if (h<0 || address(a,n,&s) || connect(h,(struct sockaddr*)&s,sizeof(s))) return (int)failure();
    return 0;
}
static ABI int64_t udp_sendto(int fd,const void *b,size_t n,int f,const Address *a,uint32_t len) {
    int h=host(fd); struct sockaddr_in s;
    if (h<0 || (a && address(a,len,&s))) return failure();
    ssize_t r=sendto(h,b,n,flags(f),a ? (struct sockaddr*)&s : NULL,a ? sizeof(s) : 0);
    return r<0 ? failure() : r;
}
static ABI int64_t udp_send(int fd,const void *b,size_t n,int f) { return udp_sendto(fd,b,n,f,NULL,0); }
static ABI int64_t udp_recvfrom(int fd,void *b,size_t n,int f,Address *a,uint32_t *len) {
    int h=host(fd); struct sockaddr_in s; socklen_t slen=sizeof(s);
    if (h<0) return failure();
    if (a && !len) { errno=EINVAL; return failure(); }
    ssize_t r=recvfrom(h,b,n,flags(f),a ? (struct sockaddr*)&s : NULL,a ? &slen : NULL);
    if (r<0) return failure();
    if (a) guest_address(&s,a,len);
    return r;
}
static ABI int64_t udp_recv(int fd,void *b,size_t n,int f) { return udp_recvfrom(fd,b,n,f,NULL,NULL); }
static ABI int udp_name(int fd,Address *a,uint32_t *len) {
    int h=host(fd); struct sockaddr_in s; socklen_t n=sizeof(s);
    if (h<0) return (int)failure();
    if (!a || !len) { errno=EINVAL; return (int)failure(); }
    if (getsockname(h,(struct sockaddr*)&s,&n)) return (int)failure();
    guest_address(&s,a,len); return 0;
}
static ABI int udp_option(int fd,int level,int opt,const void *v,uint32_t n) {
    int h=host(fd),ho=0;
    if (h<0) return (int)failure();
    if (!v || n!=sizeof(int) || level!=0xffff) { errno=EINVAL; return (int)failure(); }
    if (opt==0x1200) {
        int current=fcntl(h,F_GETFL);
        if (current<0 || fcntl(h,F_SETFL,*(const int*)v ? current|O_NONBLOCK : current&~O_NONBLOCK)<0) return (int)failure();
        return 0;
    }
    switch (opt) {
    case 4: ho=SO_REUSEADDR; break; case 0x200: ho=SO_REUSEPORT; break;
    case 0x1001: ho=SO_SNDBUF; break; case 0x1002: ho=SO_RCVBUF; break;
    case 0x20: ho=SO_BROADCAST; break;
    default: errno=ENOPROTOOPT; return (int)failure();
    }
    return setsockopt(h,SOL_SOCKET,ho,v,n)<0 ? (int)failure() : 0;
}
static ABI int udp_shutdown(int fd,int how) { int h=host(fd); return h<0 || shutdown(h,how)<0 ? (int)failure() : 0; }

/* libkernel's native select() builds temporary sceNetEpoll sets. Keep the
 * guest event ABI (24 bytes, OUT=2), not Linux's epoll_event ABI. */
#define SETS 32
#define WATCHES 64
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static struct { int used,count; struct { int fd; Event event; } watch[WATCHES]; } sets[SETS];
static int net_error(int e) { *runtime_errno()=runtime_guest_errno(e); return (int)(0x80410100u+(unsigned)*runtime_errno()); }
static ABI int ep_create(const char *name,int options) {
    (void)name; (void)options; int id=-1;
    pthread_mutex_lock(&lock);
    for (int i=0;i<SETS;++i) if (!sets[i].used) { memset(&sets[i],0,sizeof(sets[i])); sets[i].used=1; id=i+1; break; }
    pthread_mutex_unlock(&lock); return id<0 ? net_error(EMFILE) : id;
}
static ABI int ep_destroy(int id) {
    if (id<1 || id>SETS) return net_error(EBADF);
    pthread_mutex_lock(&lock); int used=sets[id-1].used; sets[id-1].used=0; pthread_mutex_unlock(&lock);
    return used ? 0 : net_error(EBADF);
}
static ABI int ep_control(int id,int op,int fd,const Event *event) {
    if (id<1 || id>SETS || op<1 || op>3 || (op!=3 && !event)) return net_error(EINVAL);
    pthread_mutex_lock(&lock);
    int s=id-1, at=0, e=0;
    if (!sets[s].used) e=EBADF;
    else {
        while (at<sets[s].count && sets[s].watch[at].fd!=fd) ++at;
        if (op==1 && at<sets[s].count) e=EEXIST;
        else if (op!=1 && at==sets[s].count) e=ENOENT;
        else if (op==1 && at==WATCHES) e=ENOSPC;
        else if (op==3) sets[s].watch[at]=sets[s].watch[--sets[s].count];
        else {
            if (op==1) ++sets[s].count;
            sets[s].watch[at].fd=fd; sets[s].watch[at].event=*event;
        }
    }
    pthread_mutex_unlock(&lock); return e ? net_error(e) : 0;
}
static ABI int ep_wait(int id,Event *out,int capacity,int timeout_us) {
    if (id<1 || id>SETS || !out || capacity<1) return net_error(EINVAL);
    struct pollfd p[WATCHES]; Event events[WATCHES]; int count;
    pthread_mutex_lock(&lock);
    if (!sets[id-1].used) { pthread_mutex_unlock(&lock); return net_error(EBADF); }
    count=sets[id-1].count;
    for (int i=0;i<count;++i) {
        int fd=sets[id-1].watch[i].fd; events[i]=sets[id-1].watch[i].event;
        events[i].ident=(uint64_t)fd;
        p[i]=(struct pollfd){.fd=host(fd),.events=(events[i].events&1 ? POLLIN : 0)|(events[i].events&2 ? POLLOUT : 0)};
        if (p[i].fd<0) { pthread_mutex_unlock(&lock); return net_error(EBADF); }
    }
    pthread_mutex_unlock(&lock);
    int ms=timeout_us<0 ? -1 : (int)(((int64_t)timeout_us+999)/1000);
    if (poll(p,count,ms)<0) return net_error(errno);
    int n=0;
    for (int i=0;i<count && n<capacity;++i) if (p[i].revents) {
        events[i].events=(p[i].revents&POLLIN ? 1u : 0u)|(p[i].revents&POLLOUT ? 2u : 0u)|
                         (p[i].revents&POLLERR ? 8u : 0u)|(p[i].revents&(POLLHUP|POLLNVAL) ? 16u : 0u);
        out[n++]=events[i];
    }
    return n;
}
static ABI int32_t *udp_errno(void) { return runtime_errno(); }
static const RuntimeExport exports[]={
    {"socket",udp_socket},{"bind",udp_bind},{"connect",udp_connect},
    {"sendto",udp_sendto},{"send",udp_send},{"recvfrom",udp_recvfrom},{"recv",udp_recv},
    {"setsockopt",udp_option},{"getsockname",udp_name},{"shutdown",udp_shutdown},
    {"sceNetErrnoLoc",udp_errno}, {"sceNetEpollCreate",ep_create},{"sceNetEpollDestroy",ep_destroy},
    {"sceNetEpollControl",ep_control},{"sceNetEpollWait",ep_wait},
};
uintptr_t runtime_udp_resolve(const char *name) {
    const char *v=getenv("BB_LOCAL_UDP");
    return v && !strcmp(v,"1") ? RUNTIME_LOOKUP(exports,name) : 0;
}
#else
uintptr_t runtime_udp_resolve(const char *name) { (void)name; return 0; }
#endif
