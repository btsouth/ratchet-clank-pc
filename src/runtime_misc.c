/* Smaller system APIs other games need: modules the game loads itself, the temporary data
 * mount, and accessories that are never connected (PlayStation Camera, Move, live streaming). */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))

/* sceKernelLoadStartModule: the game's own modules (LittleBigPlanet 3: gamedata_orbis/spu)
 * are linked into the image at start (link_modules.py) and already initialized. A module file
 * that exists is "loaded" again with a stable handle; others are not found. */
static pthread_mutex_t module_lock=PTHREAD_MUTEX_INITIALIZER;
static char module_paths[32][256];
static int module_count;
static ABI int32_t load_start_module(const char *path,uint64_t args,const void *argp,uint32_t flags,const void *opt,int32_t *result) {
    (void)args; (void)argp; (void)flags; (void)opt;
    if (!path) return ERR(22);
    char host[1024];
    if (runtime_file_translate(path,host,sizeof(host))) { printf("Runtime: module %s not found\n",path); return ERR(2); }
    FILE *f=fopen(host,"rb");
    if (!f) { printf("Runtime: module %s not found\n",path); return ERR(2); }
    fclose(f);
    pthread_mutex_lock(&module_lock);
    int i=0;
    while (i<module_count && strcmp(module_paths[i],path)) ++i;
    if (i==module_count && module_count<32) snprintf(module_paths[module_count++],256,"%s",path);
    pthread_mutex_unlock(&module_lock);
    if (result) *result=0;
    printf("Runtime: module %s started (linked at start), handle %d\n",path,0x100+i);
    return 0x100+i;
}
static ABI int32_t stop_unload_module(int32_t handle,uint64_t args,const void *argp,uint32_t flags,const void *opt,int32_t *result) {
    (void)handle; (void)args; (void)argp; (void)flags; (void)opt;
    if (result) *result=0;
    return 0;
}
static ABI int32_t posix_ok(void) { return 0; }

/* sceAppContentTemporaryDataMount2: /temp0, mounted by runtime_file_configure. */
static ABI int32_t temp_mount(uint32_t option,char *mount_point) {
    (void)option;
    if (!mount_point) return (int32_t)0x80D90002;
    strcpy(mount_point,"/temp0");
    return 0;
}
static ABI int32_t temp_format(char *mount_point) { return temp_mount(0,mount_point); }
static ABI int32_t temp_space(const char *mount_point,uint64_t *kb) {
    if (!mount_point || !kb) return (int32_t)0x80D90002;
    *kb=4ull*1024*1024; return 0;
}

/* PlayStation Move: the library opens handles, no controller is ever connected (shadPS4 move.cpp). */
#define MOVE_NOT_CONNECTED ((int32_t)0x80EE0003)
static ABI int32_t move_open(int32_t user,int32_t type,int32_t index) { (void)user; (void)type; (void)index; static int32_t h=0x30b0000; return h++; }
static ABI int32_t move_no_device(void) { return MOVE_NOT_CONNECTED; }
static ABI int32_t move_read(int32_t handle,int64_t timestamp,void *data,int32_t *count) {
    (void)handle; (void)timestamp; (void)data;
    if (count) *count=0;
    return MOVE_NOT_CONNECTED;
}
/* Move tracker: initializes with a small working area; nothing is tracked. */
static ABI int32_t tracker_memory(uint32_t *onion,uint32_t *garlic) {
    if (onion) *onion=64*1024;
    if (garlic) *garlic=64*1024;
    return 0;
}
/* PlayStation Camera: not attached (sceCameraIsAttached is 0), opening it fails. */
#define CAMERA_NOT_CONNECTED ((int32_t)0x802E000A)
static ABI int32_t camera_attached(int32_t index) { (void)index; return 0; }
static ABI int32_t camera_fail(void) { return CAMERA_NOT_CONNECTED; }
/* Live streaming: available, never streaming (status zeroed). */
static ABI int32_t stream_status(void *status) { if (status) memset(status,0,16); return 0; }
static ABI int32_t stream_messages(void *p1,uint32_t *count) { (void)p1; if (count) *count=0; return 0; }

/* sceErrorDialog: the system error message is logged and counts as read and closed at once
 * (status NONE 0, INITIALIZED 1, RUNNING 2, FINISHED 3). */
static int32_t error_dialog_status;
static ABI int32_t error_dialog_init(void) {
    if (error_dialog_status) return (int32_t)0x80ED0002; /* ALREADY_INITIALIZED */
    error_dialog_status=1; return 0;
}
static ABI int32_t error_dialog_open(const int32_t *param) { /* {size_t size; s32 errorCode; ...} */
    if (error_dialog_status!=1 && error_dialog_status!=3) return (int32_t)0x80ED0003; /* INVALID_STATE */
    if (!param) return (int32_t)0x80ED0005;
    printf("Runtime: error dialog for %#x (closed at once)\n",(unsigned)param[2]);
    error_dialog_status=3; return 0;
}
static ABI int32_t error_dialog_status_get(void) { return error_dialog_status; }
static ABI int32_t error_dialog_close(void) { if (error_dialog_status==2) error_dialog_status=3; return 0; }
static ABI int32_t error_dialog_term(void) {
    if (!error_dialog_status) return (int32_t)0x80ED0001; /* NOT_INITIALIZED */
    error_dialog_status=0; return 0;
}

/* POSIX sockets: no network, as a console that is offline (ENETDOWN). */
static ABI int32_t socket_offline(void) { *runtime_errno()=50; return -1; }

static const RuntimeExport exports[]={
    {"sceKernelLoadStartModule",load_start_module}, {"sceKernelStopUnloadModule",stop_unload_module},
    {"pthread_setschedparam",posix_ok}, {"sceKernelMlock",posix_ok},
    {"sceAppContentTemporaryDataMount2",temp_mount}, {"sceAppContentTemporaryDataFormat",temp_format},
    {"sceAppContentTemporaryDataGetAvailableSpaceKb",temp_space},
    {"sceMoveInit",posix_ok}, {"sceMoveTerm",posix_ok}, {"sceMoveOpen",move_open}, {"sceMoveClose",posix_ok},
    {"sceMoveGetDeviceInfo",move_no_device}, {"sceMoveReadStateRecent",move_read},
    {"sceMoveReadStateLatest",move_no_device}, {"sceMoveSetLightSphere",posix_ok},
    {"sceMoveSetVibration",posix_ok},
    {"sceMoveTrackerGetWorkingMemorySize",tracker_memory}, {"sceMoveTrackerInit",posix_ok},
    {"sceMoveTrackerTerm",posix_ok}, {"sceMoveTrackerGetState",move_no_device},
    {"sceMoveTrackerControllersUpdate",posix_ok}, {"sceMoveTrackerCameraUpdate",posix_ok},
    {"sceCameraIsAttached",camera_attached}, {"sceCameraOpen",camera_fail}, {"sceCameraClose",posix_ok},
    {"sceCameraStart",camera_fail}, {"sceCameraStop",posix_ok}, {"sceCameraGetFrameData",camera_fail},
    {"sceCameraSetConfig",camera_fail}, {"sceCameraGetConfig",camera_fail}, {"sceCameraSetAttribute",camera_fail},
    {"sceGameLiveStreamingInitialize",posix_ok}, {"sceGameLiveStreamingTerminate",posix_ok},
    {"sceGameLiveStreamingEnableLiveStreaming",posix_ok}, {"sceGameLiveStreamingGetCurrentStatus",stream_status},
    {"sceGameLiveStreamingClearSocialFeedbackMessages",posix_ok},
    {"sceGameLiveStreamingSetPresetSocialFeedbackCommands",posix_ok},
    {"sceGameLiveStreamingSetPresetSocialFeedbackCommandsDescription",posix_ok},
    {"sceGameLiveStreamingGetSocialFeedbackMessagesCount",stream_messages},
    {"sceGameLiveStreamingGetSocialFeedbackMessages",stream_messages},
    {"sceErrorDialogInitialize",error_dialog_init}, {"sceErrorDialogOpen",error_dialog_open},
    {"sceErrorDialogUpdateStatus",error_dialog_status_get}, {"sceErrorDialogGetStatus",error_dialog_status_get},
    {"sceErrorDialogClose",error_dialog_close}, {"sceErrorDialogTerminate",error_dialog_term},
    {"socket",socket_offline}, {"bind",socket_offline}, {"connect",socket_offline},
    {"send",socket_offline}, {"recv",socket_offline}, {"sendto",socket_offline},
    {"recvfrom",socket_offline}, {"setsockopt",socket_offline}, {"shutdown",socket_offline},
};
uintptr_t runtime_misc_resolve(const char *name) {
    uintptr_t udp=runtime_udp_resolve(name);
    return udp ? udp : RUNTIME_LOOKUP(exports,name);
}
#else
uintptr_t runtime_misc_resolve(const char *name) { (void)name; return 0; }
#endif
