#include "../svc_dropins.h"
#include <assert.h>

int main(void) {
    unsetenv("SCHEMA_SVC_DIR");
    assert(!strcmp(svc_dir_from_env("/builtin"), "/builtin"));
    setenv("SCHEMA_SVC_DIR", "relative/dir", 1);
    assert(!strcmp(svc_dir_from_env("/builtin"), "/builtin"));
    setenv("SCHEMA_SVC_DIR", "/nonexistent-schema-svc-dir", 1);
    assert(!strcmp(svc_dir_from_env("/builtin"), "/builtin"));
    setenv("SCHEMA_SVC_DIR", "/tmp", 1);
    assert(!strcmp(svc_dir_from_env("/builtin"), "/tmp"));
    puts("test_svc_dir_env: ok");
    return 0;
}
