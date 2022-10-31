#include <jni.h>
#include <sys/types.h>
#include <riru.h>
#include <unistd.h>
#include <malloc.h>
#include <string>
#include <sys/wait.h>
#include <xhook.h>
#include <sched.h>

#include "logging.h"
#include "nativehelper/scoped_utf_chars.h"
#include "android_filesystem_config.h"

#define HOOK(NAME, REPLACE) \
RegisterHook(#NAME, reinterpret_cast<void*>(REPLACE), reinterpret_cast<void**>(&orig_##NAME))

#define UNHOOK(NAME) \
RegisterHook(#NAME, reinterpret_cast<void*>(orig_##NAME), nullptr)

pid_t (*orig_fork)() = nullptr;

bool RegisterHook(const char* name, void* replace, void** backup) {
    int ret = xhook_register(".*\\libandroid_runtime.so$", name, replace, backup);
    if (ret != 0) {
        LOGE("Failed to hook %s", name);
        return false;
    }
    return true;
}

static void do_unhook(){
    xhook_enable_debug(1);
    xhook_enable_sigsegv_protection(0);
    bool unhook_fork = UNHOOK(fork);
    if (!unhook_fork || xhook_refresh(0)) {
        LOGE("Failed to clear hooks!");
        return;
    }
    xhook_clear();
}

static int shouldSkipUid(int uid) {
    int appid = uid % AID_USER_OFFSET;
    if (appid >= AID_APP_START && appid <= AID_APP_END) return false;
    if (appid >= AID_ISOLATED_START && appid <= AID_ISOLATED_END) return false;
    return true;
}

int fork_dont_care() {
    if (int pid = fork()) {
        waitpid(pid, nullptr, 0);
        return pid;
    } else if (fork()) {
        exit(0);
    }
    return 0;
}

static void trigger_magiskhide(int xpid){
    char buf[1024];

    char intStr[15];
    sprintf(intStr, "%d", xpid);
    int fork_pid = fork_dont_care();
    if (fork_pid == 0) {
        char *cmd[]= { "magisk", "magiskhide", "--check", intStr, nullptr };
        execvp(*cmd,cmd);
        _exit(0);
    }
}

static void turn_off_monitor(){
    int fork_pid = fork_dont_care();
    if (fork_pid == 0) {
        char *cmd[]= { "magisk", "magiskhide", "--monitor", "disable", nullptr };
        execvp(*cmd,cmd);
        _exit(0);
    }
}


static void doUnshare(JNIEnv *env, jint *uid, jint *mountExternal, jstring *niceName) {
    if (shouldSkipUid(*uid)) return;
    if (*mountExternal == 0) {
        *mountExternal = 1;
        ScopedUtfChars name(env, *niceName);
        LOGI("unshare uid=%d name=%s", *uid, name.c_str());
    }
}

static void forkAndSpecializePre(
        JNIEnv *env, jclass clazz, jint *uid, jint *gid, jintArray *gids, jint *runtimeFlags,
        jobjectArray *rlimits, jint *mountExternal, jstring *seInfo, jstring *niceName,
        jintArray *fdsToClose, jintArray *fdsToIgnore, jboolean *is_child_zygote,
        jstring *instructionSet, jstring *appDataDir, jboolean *isTopApp, jobjectArray *pkgDataInfoList,
        jobjectArray *whitelistedDataInfoList, jboolean *bindMountAppDataDirs, jboolean *bindMountAppStorageDirs) {
    doUnshare(env, uid, mountExternal, niceName);
}

static void forkAndSpecializePost(JNIEnv *env, jclass clazz, jint res) {
    if (res == 0) {
    	do_unhook();
        riru_set_unload_allowed(true);
    }
}

static void specializeAppProcessPre(
        JNIEnv *env, jclass clazz, jint *uid, jint *gid, jintArray *gids, jint *runtimeFlags,
        jobjectArray *rlimits, jint *mountExternal, jstring *seInfo, jstring *niceName,
        jboolean *startChildZygote, jstring *instructionSet, jstring *appDataDir,
        jboolean *isTopApp, jobjectArray *pkgDataInfoList, jobjectArray *whitelistedDataInfoList,
        jboolean *bindMountAppDataDirs, jboolean *bindMountAppStorageDirs) {
    doUnshare(env, uid, mountExternal, niceName);
}

static void specializeAppProcessPost(JNIEnv *env, jclass clazz) {
	do_unhook();
    riru_set_unload_allowed(true);
}

pid_t new_fork() {
    pid_t pid = orig_fork();
    if (pid > 0) {
        LOGD("Zygote fork PID=[%d], UID=[%d]\n", pid, getuid());
        // report event to MagiskHide
        trigger_magiskhide(pid);
    }
    return pid;
}

static void onModuleLoaded() {
    // Called when this library is loaded and "hidden" by Riru (see Riru's hide.cpp)

    // If you want to use threads, start them here rather than the constructors
    // __attribute__((constructor)) or constructors of static variables,
    // or the "hide" will cause SIGSEGV
    xhook_enable_debug(1);
    xhook_enable_sigsegv_protection(0);
    bool hook_fork = HOOK(fork, new_fork);
    if (!hook_fork || xhook_refresh(0)) {
        LOGE("Failed to register hooks!");
        return;
    }
    LOGI("Replace fork()");
    xhook_clear();
    // tell magiskhide to disable proc_monitor
    turn_off_monitor();
}


extern "C" {

int riru_api_version;
const char *riru_magisk_module_path = nullptr;
int *riru_allow_unload = nullptr;

static auto module = RiruVersionedModuleInfo{
        .moduleApiVersion = RIRU_MODULE_API_VERSION,
        .moduleInfo= RiruModuleInfo{
                .supportHide = true,
                .version = RIRU_MODULE_VERSION,
                .versionName = RIRU_MODULE_VERSION_NAME,
                .onModuleLoaded = onModuleLoaded,
                .forkAndSpecializePre = forkAndSpecializePre,
                .forkAndSpecializePost = forkAndSpecializePost,
                .forkSystemServerPre = nullptr,
                .forkSystemServerPost = nullptr,
                .specializeAppProcessPre = specializeAppProcessPre,
                .specializeAppProcessPost = specializeAppProcessPost
        }
};

#ifndef RIRU_MODULE_LEGACY_INIT
RiruVersionedModuleInfo *init(Riru *riru) {
    auto core_max_api_version = riru->riruApiVersion;
    riru_api_version = core_max_api_version <= RIRU_MODULE_API_VERSION ? core_max_api_version : RIRU_MODULE_API_VERSION;
    module.moduleApiVersion = riru_api_version;

    riru_magisk_module_path = strdup(riru->magiskModulePath);
    if (riru_api_version >= 25) {
        riru_allow_unload = riru->allowUnload;
    }
    return &module;
}
#else
RiruVersionedModuleInfo *init(Riru *riru) {
    static int step = 0;
    step += 1;

    switch (step) {
        case 1: {
            auto core_max_api_version = riru->riruApiVersion;
            riru_api_version = core_max_api_version <= RIRU_MODULE_API_VERSION ? core_max_api_version : RIRU_MODULE_API_VERSION;
            if (riru_api_version < 25) {
                module.moduleInfo.unused = (void *) shouldSkipUid;
            } else {
                riru_allow_unload = riru->allowUnload;
            }
            if (riru_api_version >= 24) {
                module.moduleApiVersion = riru_api_version;
                riru_magisk_module_path = strdup(riru->magiskModulePath);
                return &module;
            } else {
                return (RiruVersionedModuleInfo *) &riru_api_version;
            }
        }
        case 2: {
            return (RiruVersionedModuleInfo *) &module.moduleInfo;
        }
        case 3:
        default: {
            return nullptr;
        }
    }
}
#endif
}
