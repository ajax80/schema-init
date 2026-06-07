#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/stat.h>

static int is_sd(const char *p){
    if(!p) return 0;
    return strcmp(p,"/run/systemd/system")==0 || strcmp(p,"/run/systemd/system/")==0
        || strcmp(p,"/run/user/1000/systemd")==0 || strcmp(p,"/run/user/1000/systemd/")==0;
}
int access(const char *path,int mode){
    if(is_sd(path)) return 0;
    static int(*o)(const char*,int)=0; if(!o)o=dlsym(RTLD_NEXT,"access");
    return o(path,mode);
}
int faccessat(int fd,const char *path,int mode,int flag){
    if(is_sd(path)) return 0;
    static int(*o)(int,const char*,int,int)=0; if(!o)o=dlsym(RTLD_NEXT,"faccessat");
    return o(fd,path,mode,flag);
}
int __xstat(int v,const char *path,struct stat *b){
    static int(*o)(int,const char*,struct stat*)=0; if(!o)o=dlsym(RTLD_NEXT,"__xstat");
    int r=o(v,path,b); if(r!=0&&is_sd(path)){b->st_mode=S_IFDIR|0755;return 0;} return r;
}
int stat(const char *path,struct stat *b){
    static int(*o)(const char*,struct stat*)=0; if(!o)o=dlsym(RTLD_NEXT,"stat");
    int r=o(path,b); if(r!=0&&is_sd(path)){b->st_mode=S_IFDIR|0755;return 0;} return r;
}
int statx(int fd,const char *path,int flag,unsigned mask,struct statx *b){
    static int(*o)(int,const char*,int,unsigned,struct statx*)=0; if(!o)o=dlsym(RTLD_NEXT,"statx");
    int r=o(fd,path,flag,mask,b); if(r!=0&&is_sd(path)){b->stx_mode=S_IFDIR|0755;return 0;} return r;
}
