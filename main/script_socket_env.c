#include "script_socket_env.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_log.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include "wasm_export.h"

#define SCRIPT_SOCKET_NATIVE_MODULE "env"

#define WASI_ESUCCESS (0)
#define WASI_EACCES (2)
#define WASI_EADDRINUSE (3)
#define WASI_EADDRNOTAVAIL (4)
#define WASI_EAFNOSUPPORT (5)
#define WASI_EAGAIN (6)
#define WASI_EALREADY (7)
#define WASI_EBADF (8)
#define WASI_ECONNABORTED (13)
#define WASI_ECONNREFUSED (14)
#define WASI_ECONNRESET (15)
#define WASI_EDESTADDRREQ (17)
#define WASI_EFAULT (21)
#define WASI_EHOSTUNREACH (23)
#define WASI_EINPROGRESS (26)
#define WASI_EINTR (27)
#define WASI_EINVAL (28)
#define WASI_EIO (29)
#define WASI_EISCONN (30)
#define WASI_EMSGSIZE (35)
#define WASI_ENETDOWN (38)
#define WASI_ENETRESET (39)
#define WASI_ENETUNREACH (40)
#define WASI_ENOBUFS (42)
#define WASI_ENOENT (44)
#define WASI_ENOMEM (48)
#define WASI_ENOPROTOOPT (50)
#define WASI_ENOSYS (52)
#define WASI_ENOTCONN (53)
#define WASI_ENOTSOCK (57)
#define WASI_ENOTSUP (58)
#define WASI_EPERM (63)
#define WASI_EPIPE (64)
#define WASI_EPROTO (65)
#define WASI_EPROTONOSUPPORT (66)
#define WASI_EPROTOTYPE (67)
#define WASI_ERANGE (68)
#define WASI_ETIMEDOUT (73)
#define WASI_EWOULDBLOCK WASI_EAGAIN

typedef enum
{
    SCRIPT_SOCKET_ADDR_FAMILY_UNSPEC = 0,
    SCRIPT_SOCKET_ADDR_FAMILY_INET4 = 1,
    SCRIPT_SOCKET_ADDR_FAMILY_INET6 = 2,
} script_socket_addr_family_t;

typedef enum
{
    SCRIPT_SOCKET_TYPE_ANY = 0,
    SCRIPT_SOCKET_TYPE_DATAGRAM = 1,
    SCRIPT_SOCKET_TYPE_STREAM = 2,
} script_socket_type_t;

typedef union
{
    uint8_t v4[4];
    uint16_t v6[8];
} script_socket_ip_t;

typedef struct
{
    uint8_t kind;
    uint8_t reserved0;
    uint16_t port;
    script_socket_ip_t ip;
} script_socket_addr_t;

typedef struct
{
    script_socket_addr_t address;
    uint8_t socket_type;
    uint8_t is_internal;
} script_socket_addr_info_t;

typedef struct
{
    uint8_t family;
    uint8_t socket_type;
} script_socket_addr_info_hints_t;

_Static_assert(sizeof(script_socket_addr_t) == 20, "script_socket_addr_t must match Rust layout");
_Static_assert(sizeof(script_socket_addr_info_t) == 22, "script_socket_addr_info_t must match Rust layout");
_Static_assert(sizeof(script_socket_addr_info_hints_t) == 2, "script_socket_addr_info_hints_t must match Rust layout");

static const char *TAG = "script_socket";
static bool s_socket_natives_registered;

static uint16_t script_socket_errno_to_wasi(int error)
{
    switch (error)
    {
    case 0:
        return WASI_ESUCCESS;
    case EACCES:
        return WASI_EACCES;
    case EADDRINUSE:
        return WASI_EADDRINUSE;
    case EADDRNOTAVAIL:
        return WASI_EADDRNOTAVAIL;
    case EAFNOSUPPORT:
        return WASI_EAFNOSUPPORT;
    case EAGAIN:
        return WASI_EAGAIN;
    case EALREADY:
        return WASI_EALREADY;
    case EBADF:
        return WASI_EBADF;
    case ECONNABORTED:
        return WASI_ECONNABORTED;
    case ECONNREFUSED:
        return WASI_ECONNREFUSED;
    case ECONNRESET:
        return WASI_ECONNRESET;
    case EDESTADDRREQ:
        return WASI_EDESTADDRREQ;
    case EFAULT:
        return WASI_EFAULT;
    case EHOSTUNREACH:
        return WASI_EHOSTUNREACH;
    case EINPROGRESS:
        return WASI_EINPROGRESS;
    case EINTR:
        return WASI_EINTR;
    case EINVAL:
        return WASI_EINVAL;
    case EIO:
        return WASI_EIO;
    case EISCONN:
        return WASI_EISCONN;
    case EMSGSIZE:
        return WASI_EMSGSIZE;
    case ENETDOWN:
        return WASI_ENETDOWN;
    case ENETRESET:
        return WASI_ENETRESET;
    case ENETUNREACH:
        return WASI_ENETUNREACH;
    case ENOBUFS:
        return WASI_ENOBUFS;
    case ENOENT:
        return WASI_ENOENT;
    case ENOMEM:
        return WASI_ENOMEM;
    case ENOPROTOOPT:
        return WASI_ENOPROTOOPT;
    case ENOTCONN:
        return WASI_ENOTCONN;
    case ENOTSOCK:
        return WASI_ENOTSOCK;
    case ENOTSUP:
        return WASI_ENOTSUP;
    case EPERM:
        return WASI_EPERM;
    case EPIPE:
        return WASI_EPIPE;
    case EPROTO:
        return WASI_EPROTO;
    case EPROTONOSUPPORT:
        return WASI_EPROTONOSUPPORT;
    case EPROTOTYPE:
        return WASI_EPROTOTYPE;
    case ERANGE:
        return WASI_ERANGE;
    case ETIMEDOUT:
        return WASI_ETIMEDOUT;
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
        return WASI_EWOULDBLOCK;
#endif
    default:
        return WASI_EIO;
    }
}

static uint16_t script_socket_gai_to_wasi(int error)
{
    switch (error)
    {
    case 0:
        return WASI_ESUCCESS;
#ifdef EAI_AGAIN
    case EAI_AGAIN:
        return WASI_EAGAIN;
#endif
#ifdef EAI_FAMILY
    case EAI_FAMILY:
        return WASI_EAFNOSUPPORT;
#endif
#ifdef EAI_MEMORY
    case EAI_MEMORY:
        return WASI_ENOMEM;
#endif
#ifdef EAI_NONAME
    case EAI_NONAME:
        return WASI_ENOENT;
#endif
#ifdef EAI_SERVICE
    case EAI_SERVICE:
        return WASI_EPROTONOSUPPORT;
#endif
#ifdef EAI_SOCKTYPE
    case EAI_SOCKTYPE:
        return WASI_EPROTOTYPE;
#endif
    default:
        return WASI_EIO;
    }
}

static int script_socket_family_to_native(uint8_t family)
{
    switch (family)
    {
    case SCRIPT_SOCKET_ADDR_FAMILY_UNSPEC:
        return AF_UNSPEC;
    case SCRIPT_SOCKET_ADDR_FAMILY_INET4:
        return AF_INET;
    case SCRIPT_SOCKET_ADDR_FAMILY_INET6:
        return AF_INET6;
    default:
        return -1;
    }
}

static int script_socket_type_to_native(uint8_t socket_type)
{
    switch (socket_type)
    {
    case SCRIPT_SOCKET_TYPE_ANY:
        return 0;
    case SCRIPT_SOCKET_TYPE_DATAGRAM:
        return SOCK_DGRAM;
    case SCRIPT_SOCKET_TYPE_STREAM:
        return SOCK_STREAM;
    default:
        return -1;
    }
}

static uint8_t script_socket_type_from_native(int socket_type)
{
    switch (socket_type)
    {
    case SOCK_DGRAM:
        return SCRIPT_SOCKET_TYPE_DATAGRAM;
    case SOCK_STREAM:
        return SCRIPT_SOCKET_TYPE_STREAM;
    default:
        return SCRIPT_SOCKET_TYPE_ANY;
    }
}

uint16_t script_socket_get_guest_buffer(wasm_module_inst_t module_inst,
                                        uint32_t offset,
                                        uint32_t size,
                                        bool allow_null,
                                        void **out_ptr)
{
    if (!out_ptr)
    {
        return WASI_EINVAL;
    }

    *out_ptr = NULL;

    if (offset == 0)
    {
        return allow_null ? WASI_ESUCCESS : WASI_EFAULT;
    }

    if (size > 0 && !wasm_runtime_validate_app_addr(module_inst, offset, size))
    {
        return WASI_EFAULT;
    }

    *out_ptr = wasm_runtime_addr_app_to_native(module_inst, offset);
    return *out_ptr || size == 0 ? WASI_ESUCCESS : WASI_EFAULT;
}

uint16_t script_socket_get_guest_string(wasm_module_inst_t module_inst,
                                        uint32_t offset,
                                        bool allow_null,
                                        const char **out_ptr)
{
    if (!out_ptr)
    {
        return WASI_EINVAL;
    }

    *out_ptr = NULL;

    if (offset == 0)
    {
        return allow_null ? WASI_ESUCCESS : WASI_EFAULT;
    }

    if (!wasm_runtime_validate_app_str_addr(module_inst, offset))
    {
        return WASI_EFAULT;
    }

    *out_ptr = wasm_runtime_addr_app_to_native(module_inst, offset);
    return *out_ptr ? WASI_ESUCCESS : WASI_EFAULT;
}

static bool script_socket_wasm_to_native_addr(const script_socket_addr_t *source,
                                              struct sockaddr_storage *storage,
                                              socklen_t *storage_len)
{
    if (!source || !storage || !storage_len)
    {
        return false;
    }

    memset(storage, 0, sizeof(*storage));

    switch (source->kind)
    {
    case SCRIPT_SOCKET_ADDR_FAMILY_INET4:
    {
        struct sockaddr_in *addr4 = (struct sockaddr_in *)storage;

        addr4->sin_family = AF_INET;
        addr4->sin_port = htons(source->port);
        memcpy(&addr4->sin_addr, source->ip.v4, sizeof(source->ip.v4));
        *storage_len = sizeof(*addr4);
        return true;
    }
    case SCRIPT_SOCKET_ADDR_FAMILY_INET6:
    {
        struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)storage;
        uint16_t words[8];

        addr6->sin6_family = AF_INET6;
        addr6->sin6_port = htons(source->port);
        for (size_t index = 0; index < 8; index++)
        {
            words[index] = htons(source->ip.v6[index]);
        }
        memcpy(&addr6->sin6_addr, words, sizeof(words));
        *storage_len = sizeof(*addr6);
        return true;
    }
    default:
        return false;
    }
}

static bool script_socket_native_to_wasm_addr(const struct sockaddr *source,
                                              script_socket_addr_t *dest)
{
    if (!source || !dest)
    {
        return false;
    }

    memset(dest, 0, sizeof(*dest));

    switch (source->sa_family)
    {
    case AF_INET:
    {
        const struct sockaddr_in *addr4 = (const struct sockaddr_in *)source;

        dest->kind = SCRIPT_SOCKET_ADDR_FAMILY_INET4;
        dest->port = ntohs(addr4->sin_port);
        memcpy(dest->ip.v4, &addr4->sin_addr, sizeof(dest->ip.v4));
        return true;
    }
    case AF_INET6:
    {
        const struct sockaddr_in6 *addr6 = (const struct sockaddr_in6 *)source;
        const uint16_t *words = (const uint16_t *)&addr6->sin6_addr;

        dest->kind = SCRIPT_SOCKET_ADDR_FAMILY_INET6;
        dest->port = ntohs(addr6->sin6_port);
        for (size_t index = 0; index < 8; index++)
        {
            dest->ip.v6[index] = ntohs(words[index]);
        }
        return true;
    }
    default:
        return false;
    }
}

static uint16_t script_socket_set_int_opt(int fd, int level, int option_name, int value)
{
    return setsockopt(fd, level, option_name, &value, sizeof(value)) == 0
               ? WASI_ESUCCESS
               : script_socket_errno_to_wasi(errno);
}

static uint16_t script_socket_get_int_opt(int fd, int level, int option_name, int32_t *value_out)
{
    int value = 0;
    socklen_t option_len = sizeof(value);

    if (!value_out)
    {
        return WASI_EINVAL;
    }

    if (getsockopt(fd, level, option_name, &value, &option_len) != 0)
    {
        return script_socket_errno_to_wasi(errno);
    }

    *value_out = value;
    return WASI_ESUCCESS;
}

static uint16_t script_socket_set_timeout_opt(int fd, int option_name, int64_t timeout_us)
{
    struct timeval timeout = {0};

    if (timeout_us < 0)
    {
        return WASI_EINVAL;
    }

    timeout.tv_sec = (time_t)(timeout_us / 1000000);
    timeout.tv_usec = (suseconds_t)(timeout_us % 1000000);

    return setsockopt(fd, SOL_SOCKET, option_name, &timeout, sizeof(timeout)) == 0
               ? WASI_ESUCCESS
               : script_socket_errno_to_wasi(errno);
}

static uint32_t sock_open_wrapper(wasm_exec_env_t exec_env,
                                  int32_t pool_fd,
                                  int32_t family_raw,
                                  int32_t socket_type_raw,
                                  uint32_t fd_out_offset)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    uint32_t *fd_out = NULL;
    uint16_t err = script_socket_get_guest_buffer(module_inst,
                                                  fd_out_offset,
                                                  sizeof(*fd_out),
                                                  false,
                                                  (void **)&fd_out);
    int family;
    int socket_type;
    int fd;

    (void)pool_fd;

    if (err != WASI_ESUCCESS)
    {
        return err;
    }

    family = script_socket_family_to_native((uint8_t)family_raw);
    socket_type = script_socket_type_to_native((uint8_t)socket_type_raw);
    if (family < 0)
    {
        return WASI_EAFNOSUPPORT;
    }
    if (socket_type <= 0)
    {
        return WASI_EPROTOTYPE;
    }

    fd = socket(family, socket_type, 0);
    if (fd < 0)
    {
        return script_socket_errno_to_wasi(errno);
    }

    *fd_out = (uint32_t)fd;
    return WASI_ESUCCESS;
}

static uint32_t sock_close_wrapper(wasm_exec_env_t exec_env, int32_t fd)
{
    (void)exec_env;
    return close(fd) == 0 ? WASI_ESUCCESS : script_socket_errno_to_wasi(errno);
}

static uint32_t sock_bind_wrapper(wasm_exec_env_t exec_env, int32_t fd, uint32_t addr_offset)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    script_socket_addr_t *addr = NULL;
    struct sockaddr_storage storage;
    socklen_t storage_len = 0;
    uint16_t err = script_socket_get_guest_buffer(module_inst,
                                                  addr_offset,
                                                  sizeof(*addr),
                                                  false,
                                                  (void **)&addr);

    if (err != WASI_ESUCCESS)
    {
        return err;
    }

    if (!script_socket_wasm_to_native_addr(addr, &storage, &storage_len))
    {
        return WASI_EAFNOSUPPORT;
    }

    return bind(fd, (const struct sockaddr *)&storage, storage_len) == 0
               ? WASI_ESUCCESS
               : script_socket_errno_to_wasi(errno);
}

static uint32_t sock_listen_wrapper(wasm_exec_env_t exec_env, int32_t fd, int32_t backlog)
{
    (void)exec_env;

    if (backlog < 0)
    {
        return WASI_EINVAL;
    }

    return listen(fd, backlog) == 0 ? WASI_ESUCCESS : script_socket_errno_to_wasi(errno);
}

static uint32_t sock_accept_wrapper(wasm_exec_env_t exec_env,
                                    int32_t fd,
                                    int32_t flags,
                                    uint32_t fd_out_offset)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    uint32_t *fd_out = NULL;
    uint16_t err = script_socket_get_guest_buffer(module_inst,
                                                  fd_out_offset,
                                                  sizeof(*fd_out),
                                                  false,
                                                  (void **)&fd_out);
    int new_fd;

    if (err != WASI_ESUCCESS)
    {
        return err;
    }

    if (flags != 0)
    {
        return WASI_EINVAL;
    }

    new_fd = accept(fd, NULL, NULL);
    if (new_fd < 0)
    {
        return script_socket_errno_to_wasi(errno);
    }

    *fd_out = (uint32_t)new_fd;
    return WASI_ESUCCESS;
}

static uint32_t sock_connect_wrapper(wasm_exec_env_t exec_env, int32_t fd, uint32_t addr_offset)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    script_socket_addr_t *addr = NULL;
    struct sockaddr_storage storage;
    socklen_t storage_len = 0;
    uint16_t err = script_socket_get_guest_buffer(module_inst,
                                                  addr_offset,
                                                  sizeof(*addr),
                                                  false,
                                                  (void **)&addr);

    if (err != WASI_ESUCCESS)
    {
        return err;
    }

    if (!script_socket_wasm_to_native_addr(addr, &storage, &storage_len))
    {
        return WASI_EAFNOSUPPORT;
    }

    return connect(fd, (const struct sockaddr *)&storage, storage_len) == 0
               ? WASI_ESUCCESS
               : script_socket_errno_to_wasi(errno);
}

static uint32_t sock_addr_local_wrapper(wasm_exec_env_t exec_env, int32_t fd, uint32_t addr_offset)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    script_socket_addr_t *addr = NULL;
    struct sockaddr_storage storage;
    socklen_t storage_len = sizeof(storage);
    uint16_t err = script_socket_get_guest_buffer(module_inst,
                                                  addr_offset,
                                                  sizeof(*addr),
                                                  false,
                                                  (void **)&addr);

    if (err != WASI_ESUCCESS)
    {
        return err;
    }

    if (getsockname(fd, (struct sockaddr *)&storage, &storage_len) != 0)
    {
        return script_socket_errno_to_wasi(errno);
    }

    return script_socket_native_to_wasm_addr((const struct sockaddr *)&storage, addr)
               ? WASI_ESUCCESS
               : WASI_EAFNOSUPPORT;
}

static uint32_t sock_addr_remote_wrapper(wasm_exec_env_t exec_env, int32_t fd, uint32_t addr_offset)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    script_socket_addr_t *addr = NULL;
    struct sockaddr_storage storage;
    socklen_t storage_len = sizeof(storage);
    uint16_t err = script_socket_get_guest_buffer(module_inst,
                                                  addr_offset,
                                                  sizeof(*addr),
                                                  false,
                                                  (void **)&addr);

    if (err != WASI_ESUCCESS)
    {
        return err;
    }

    if (getpeername(fd, (struct sockaddr *)&storage, &storage_len) != 0)
    {
        return script_socket_errno_to_wasi(errno);
    }

    return script_socket_native_to_wasm_addr((const struct sockaddr *)&storage, addr)
               ? WASI_ESUCCESS
               : WASI_EAFNOSUPPORT;
}

static uint32_t sock_addr_resolve_wrapper(wasm_exec_env_t exec_env,
                                          uint32_t host_offset,
                                          uint32_t service_offset,
                                          uint32_t hints_offset,
                                          uint32_t addr_info_offset,
                                          uint32_t addr_info_len,
                                          uint32_t addr_info_count_offset)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const char *host = NULL;
    const char *service = NULL;
    script_socket_addr_info_hints_t *hints = NULL;
    script_socket_addr_info_t *addr_info = NULL;
    uint32_t *count_out = NULL;
    struct addrinfo native_hints = {0};
    struct addrinfo *results = NULL;
    struct addrinfo *entry = NULL;
    uint32_t count = 0;
    uint16_t err;
    int rc;

    err = script_socket_get_guest_string(module_inst, host_offset, true, &host);
    if (err != WASI_ESUCCESS)
    {
        return err;
    }

    err = script_socket_get_guest_string(module_inst, service_offset, true, &service);
    if (err != WASI_ESUCCESS)
    {
        return err;
    }

    err = script_socket_get_guest_buffer(module_inst,
                                         hints_offset,
                                         sizeof(*hints),
                                         true,
                                         (void **)&hints);
    if (err != WASI_ESUCCESS)
    {
        return err;
    }

    err = script_socket_get_guest_buffer(module_inst,
                                         addr_info_offset,
                                         sizeof(*addr_info) * addr_info_len,
                                         false,
                                         (void **)&addr_info);
    if (err != WASI_ESUCCESS)
    {
        return err;
    }

    err = script_socket_get_guest_buffer(module_inst,
                                         addr_info_count_offset,
                                         sizeof(*count_out),
                                         false,
                                         (void **)&count_out);
    if (err != WASI_ESUCCESS)
    {
        return err;
    }

    *count_out = 0;

    if (hints)
    {
        native_hints.ai_family = script_socket_family_to_native(hints->family);
        native_hints.ai_socktype = script_socket_type_to_native(hints->socket_type);
        if (native_hints.ai_family < 0)
        {
            return WASI_EAFNOSUPPORT;
        }
        if (native_hints.ai_socktype < 0)
        {
            return WASI_EPROTOTYPE;
        }
    }

    rc = getaddrinfo(host && host[0] ? host : NULL,
                     service && service[0] ? service : NULL,
                     hints ? &native_hints : NULL,
                     &results);
    if (rc != 0)
    {
        return script_socket_gai_to_wasi(rc);
    }

    for (entry = results; entry && count < addr_info_len; entry = entry->ai_next)
    {
        if (!entry->ai_addr || !script_socket_native_to_wasm_addr(entry->ai_addr, &addr_info[count].address))
        {
            continue;
        }

        addr_info[count].socket_type = script_socket_type_from_native(entry->ai_socktype);
        addr_info[count].is_internal = 0;
        count++;
    }

    freeaddrinfo(results);
    *count_out = count;
    return WASI_ESUCCESS;
}

static uint32_t sock_recv_wrapper(wasm_exec_env_t exec_env,
                                  int32_t fd,
                                  uint32_t buffer_offset,
                                  uint32_t buffer_len,
                                  int32_t flags,
                                  uint32_t data_len_out_offset,
                                  uint32_t ro_flags_out_offset)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    uint8_t *buffer = NULL;
    uint32_t *data_len_out = NULL;
    uint16_t *ro_flags_out = NULL;
    ssize_t received;
    uint16_t err;

    err = script_socket_get_guest_buffer(module_inst,
                                         buffer_offset,
                                         buffer_len,
                                         buffer_len == 0,
                                         (void **)&buffer);
    if (err != WASI_ESUCCESS)
    {
        return err;
    }

    err = script_socket_get_guest_buffer(module_inst,
                                         data_len_out_offset,
                                         sizeof(*data_len_out),
                                         false,
                                         (void **)&data_len_out);
    if (err != WASI_ESUCCESS)
    {
        return err;
    }

    err = script_socket_get_guest_buffer(module_inst,
                                         ro_flags_out_offset,
                                         sizeof(*ro_flags_out),
                                         false,
                                         (void **)&ro_flags_out);
    if (err != WASI_ESUCCESS)
    {
        return err;
    }

    received = recv(fd, buffer, buffer_len, flags);
    if (received < 0)
    {
        return script_socket_errno_to_wasi(errno);
    }

    *data_len_out = (uint32_t)received;
    *ro_flags_out = 0;
    return WASI_ESUCCESS;
}

static uint32_t sock_send_wrapper(wasm_exec_env_t exec_env,
                                  int32_t fd,
                                  uint32_t buffer_offset,
                                  uint32_t buffer_len,
                                  int32_t flags,
                                  uint32_t data_len_out_offset)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    uint8_t *buffer = NULL;
    uint32_t *data_len_out = NULL;
    ssize_t sent;
    uint16_t err;

    err = script_socket_get_guest_buffer(module_inst,
                                         buffer_offset,
                                         buffer_len,
                                         buffer_len == 0,
                                         (void **)&buffer);
    if (err != WASI_ESUCCESS)
    {
        return err;
    }

    err = script_socket_get_guest_buffer(module_inst,
                                         data_len_out_offset,
                                         sizeof(*data_len_out),
                                         false,
                                         (void **)&data_len_out);
    if (err != WASI_ESUCCESS)
    {
        return err;
    }

    sent = send(fd, buffer, buffer_len, flags);
    if (sent < 0)
    {
        return script_socket_errno_to_wasi(errno);
    }

    *data_len_out = (uint32_t)sent;
    return WASI_ESUCCESS;
}

static uint32_t sock_recv_from_wrapper(wasm_exec_env_t exec_env,
                                       int32_t fd,
                                       uint32_t buffer_offset,
                                       uint32_t buffer_len,
                                       int32_t flags,
                                       uint32_t addr_out_offset,
                                       uint32_t data_len_out_offset)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    uint8_t *buffer = NULL;
    script_socket_addr_t *addr_out = NULL;
    uint32_t *data_len_out = NULL;
    struct sockaddr_storage storage;
    socklen_t storage_len = sizeof(storage);
    ssize_t received;
    uint16_t err;

    err = script_socket_get_guest_buffer(module_inst,
                                         buffer_offset,
                                         buffer_len,
                                         buffer_len == 0,
                                         (void **)&buffer);
    if (err != WASI_ESUCCESS)
    {
        return err;
    }

    err = script_socket_get_guest_buffer(module_inst,
                                         addr_out_offset,
                                         sizeof(*addr_out),
                                         false,
                                         (void **)&addr_out);
    if (err != WASI_ESUCCESS)
    {
        return err;
    }

    err = script_socket_get_guest_buffer(module_inst,
                                         data_len_out_offset,
                                         sizeof(*data_len_out),
                                         false,
                                         (void **)&data_len_out);
    if (err != WASI_ESUCCESS)
    {
        return err;
    }

    received = recvfrom(fd, buffer, buffer_len, flags, (struct sockaddr *)&storage, &storage_len);
    if (received < 0)
    {
        return script_socket_errno_to_wasi(errno);
    }

    if (!script_socket_native_to_wasm_addr((const struct sockaddr *)&storage, addr_out))
    {
        return WASI_EAFNOSUPPORT;
    }

    *data_len_out = (uint32_t)received;
    return WASI_ESUCCESS;
}

static uint32_t sock_send_to_wrapper(wasm_exec_env_t exec_env,
                                     int32_t fd,
                                     uint32_t buffer_offset,
                                     uint32_t buffer_len,
                                     int32_t flags,
                                     uint32_t addr_offset,
                                     uint32_t data_len_out_offset)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    uint8_t *buffer = NULL;
    script_socket_addr_t *addr = NULL;
    uint32_t *data_len_out = NULL;
    struct sockaddr_storage storage;
    socklen_t storage_len = 0;
    ssize_t sent;
    uint16_t err;

    err = script_socket_get_guest_buffer(module_inst,
                                         buffer_offset,
                                         buffer_len,
                                         buffer_len == 0,
                                         (void **)&buffer);
    if (err != WASI_ESUCCESS)
    {
        return err;
    }

    err = script_socket_get_guest_buffer(module_inst,
                                         addr_offset,
                                         sizeof(*addr),
                                         false,
                                         (void **)&addr);
    if (err != WASI_ESUCCESS)
    {
        return err;
    }

    err = script_socket_get_guest_buffer(module_inst,
                                         data_len_out_offset,
                                         sizeof(*data_len_out),
                                         false,
                                         (void **)&data_len_out);
    if (err != WASI_ESUCCESS)
    {
        return err;
    }

    if (!script_socket_wasm_to_native_addr(addr, &storage, &storage_len))
    {
        return WASI_EAFNOSUPPORT;
    }

    sent = sendto(fd, buffer, buffer_len, flags, (const struct sockaddr *)&storage, storage_len);
    if (sent < 0)
    {
        return script_socket_errno_to_wasi(errno);
    }

    *data_len_out = (uint32_t)sent;
    return WASI_ESUCCESS;
}

static uint32_t sock_set_reuse_addr_wrapper(wasm_exec_env_t exec_env, int32_t fd, int32_t enabled)
{
    (void)exec_env;
    return script_socket_set_int_opt(fd, SOL_SOCKET, SO_REUSEADDR, enabled ? 1 : 0);
}

static uint32_t sock_get_reuse_addr_wrapper(wasm_exec_env_t exec_env, int32_t fd, uint32_t enabled_out_offset)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    int32_t *enabled_out = NULL;
    uint16_t err = script_socket_get_guest_buffer(module_inst,
                                                  enabled_out_offset,
                                                  sizeof(*enabled_out),
                                                  false,
                                                  (void **)&enabled_out);
    if (err != WASI_ESUCCESS)
    {
        return err;
    }

    return script_socket_get_int_opt(fd, SOL_SOCKET, SO_REUSEADDR, enabled_out);
}

static uint32_t sock_set_broadcast_wrapper(wasm_exec_env_t exec_env, int32_t fd, int32_t enabled)
{
    (void)exec_env;
    return script_socket_set_int_opt(fd, SOL_SOCKET, SO_BROADCAST, enabled ? 1 : 0);
}

static uint32_t sock_get_broadcast_wrapper(wasm_exec_env_t exec_env, int32_t fd, uint32_t enabled_out_offset)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    int32_t *enabled_out = NULL;
    uint16_t err = script_socket_get_guest_buffer(module_inst,
                                                  enabled_out_offset,
                                                  sizeof(*enabled_out),
                                                  false,
                                                  (void **)&enabled_out);
    if (err != WASI_ESUCCESS)
    {
        return err;
    }

    return script_socket_get_int_opt(fd, SOL_SOCKET, SO_BROADCAST, enabled_out);
}

static uint32_t sock_set_recv_timeout_wrapper(wasm_exec_env_t exec_env, int32_t fd, int64_t timeout_us)
{
    (void)exec_env;
    return script_socket_set_timeout_opt(fd, SO_RCVTIMEO, timeout_us);
}

static uint32_t sock_set_send_timeout_wrapper(wasm_exec_env_t exec_env, int32_t fd, int64_t timeout_us)
{
    (void)exec_env;
    return script_socket_set_timeout_opt(fd, SO_SNDTIMEO, timeout_us);
}

static uint32_t sock_set_recv_buf_size_wrapper(wasm_exec_env_t exec_env, int32_t fd, int32_t size)
{
    (void)exec_env;

    if (size < 0)
    {
        return WASI_EINVAL;
    }

    return script_socket_set_int_opt(fd, SOL_SOCKET, SO_RCVBUF, size);
}

static uint32_t sock_get_recv_buf_size_wrapper(wasm_exec_env_t exec_env, int32_t fd, uint32_t size_out_offset)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    uint32_t *size_out = NULL;
    int32_t value = 0;
    uint16_t err = script_socket_get_guest_buffer(module_inst,
                                                  size_out_offset,
                                                  sizeof(*size_out),
                                                  false,
                                                  (void **)&size_out);
    if (err != WASI_ESUCCESS)
    {
        return err;
    }

    err = script_socket_get_int_opt(fd, SOL_SOCKET, SO_RCVBUF, &value);
    if (err != WASI_ESUCCESS)
    {
        return err;
    }

    *size_out = value < 0 ? 0 : (uint32_t)value;
    return WASI_ESUCCESS;
}

static uint32_t sock_set_send_buf_size_wrapper(wasm_exec_env_t exec_env, int32_t fd, int32_t size)
{
    (void)exec_env;

    if (size < 0)
    {
        return WASI_EINVAL;
    }

    return script_socket_set_int_opt(fd, SOL_SOCKET, SO_SNDBUF, size);
}

static uint32_t sock_get_send_buf_size_wrapper(wasm_exec_env_t exec_env, int32_t fd, uint32_t size_out_offset)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    uint32_t *size_out = NULL;
    int32_t value = 0;
    uint16_t err = script_socket_get_guest_buffer(module_inst,
                                                  size_out_offset,
                                                  sizeof(*size_out),
                                                  false,
                                                  (void **)&size_out);
    if (err != WASI_ESUCCESS)
    {
        return err;
    }

    err = script_socket_get_int_opt(fd, SOL_SOCKET, SO_SNDBUF, &value);
    if (err != WASI_ESUCCESS)
    {
        return err;
    }

    *size_out = value < 0 ? 0 : (uint32_t)value;
    return WASI_ESUCCESS;
}

static uint32_t sock_set_keep_alive_wrapper(wasm_exec_env_t exec_env, int32_t fd, int32_t enabled)
{
    (void)exec_env;
    return script_socket_set_int_opt(fd, SOL_SOCKET, SO_KEEPALIVE, enabled ? 1 : 0);
}

static uint32_t sock_get_keep_alive_wrapper(wasm_exec_env_t exec_env, int32_t fd, uint32_t enabled_out_offset)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    int32_t *enabled_out = NULL;
    uint16_t err = script_socket_get_guest_buffer(module_inst,
                                                  enabled_out_offset,
                                                  sizeof(*enabled_out),
                                                  false,
                                                  (void **)&enabled_out);
    if (err != WASI_ESUCCESS)
    {
        return err;
    }

    return script_socket_get_int_opt(fd, SOL_SOCKET, SO_KEEPALIVE, enabled_out);
}

static uint32_t sock_set_tcp_keep_idle_wrapper(wasm_exec_env_t exec_env, int32_t fd, int32_t seconds)
{
    (void)exec_env;

    if (seconds < 0)
    {
        return WASI_EINVAL;
    }

#ifdef TCP_KEEPIDLE
    return script_socket_set_int_opt(fd, IPPROTO_TCP, TCP_KEEPIDLE, seconds);
#else
    return WASI_ENOTSUP;
#endif
}

static uint32_t sock_set_tcp_keep_intvl_wrapper(wasm_exec_env_t exec_env, int32_t fd, int32_t seconds)
{
    (void)exec_env;

    if (seconds < 0)
    {
        return WASI_EINVAL;
    }

#ifdef TCP_KEEPINTVL
    return script_socket_set_int_opt(fd, IPPROTO_TCP, TCP_KEEPINTVL, seconds);
#else
    return WASI_ENOTSUP;
#endif
}

static uint32_t sock_set_tcp_no_delay_wrapper(wasm_exec_env_t exec_env, int32_t fd, int32_t enabled)
{
    (void)exec_env;
    return script_socket_set_int_opt(fd, IPPROTO_TCP, TCP_NODELAY, enabled ? 1 : 0);
}

static uint32_t sock_get_tcp_no_delay_wrapper(wasm_exec_env_t exec_env, int32_t fd, uint32_t enabled_out_offset)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    int32_t *enabled_out = NULL;
    uint16_t err = script_socket_get_guest_buffer(module_inst,
                                                  enabled_out_offset,
                                                  sizeof(*enabled_out),
                                                  false,
                                                  (void **)&enabled_out);
    if (err != WASI_ESUCCESS)
    {
        return err;
    }

    return script_socket_get_int_opt(fd, IPPROTO_TCP, TCP_NODELAY, enabled_out);
}

static NativeSymbol s_socket_symbols[] = {
    {"sock_open", sock_open_wrapper, "(iiii)i", NULL},
    {"sock_close", sock_close_wrapper, "(i)i", NULL},
    {"sock_bind", sock_bind_wrapper, "(ii)i", NULL},
    {"sock_listen", sock_listen_wrapper, "(ii)i", NULL},
    {"sock_accept", sock_accept_wrapper, "(iii)i", NULL},
    {"sock_connect", sock_connect_wrapper, "(ii)i", NULL},
    {"sock_addr_local", sock_addr_local_wrapper, "(ii)i", NULL},
    {"sock_addr_remote", sock_addr_remote_wrapper, "(ii)i", NULL},
    {"sock_addr_resolve", sock_addr_resolve_wrapper, "(iiiiii)i", NULL},
    {"sock_recv", sock_recv_wrapper, "(iiiiii)i", NULL},
    {"sock_send", sock_send_wrapper, "(iiiii)i", NULL},
    {"sock_recv_from", sock_recv_from_wrapper, "(iiiiii)i", NULL},
    {"sock_send_to", sock_send_to_wrapper, "(iiiiii)i", NULL},
    {"sock_set_reuse_addr", sock_set_reuse_addr_wrapper, "(ii)i", NULL},
    {"sock_get_reuse_addr", sock_get_reuse_addr_wrapper, "(ii)i", NULL},
    {"sock_set_broadcast", sock_set_broadcast_wrapper, "(ii)i", NULL},
    {"sock_get_broadcast", sock_get_broadcast_wrapper, "(ii)i", NULL},
    {"sock_set_recv_timeout", sock_set_recv_timeout_wrapper, "(iI)i", NULL},
    {"sock_set_send_timeout", sock_set_send_timeout_wrapper, "(iI)i", NULL},
    {"sock_set_recv_buf_size", sock_set_recv_buf_size_wrapper, "(ii)i", NULL},
    {"sock_get_recv_buf_size", sock_get_recv_buf_size_wrapper, "(ii)i", NULL},
    {"sock_set_send_buf_size", sock_set_send_buf_size_wrapper, "(ii)i", NULL},
    {"sock_get_send_buf_size", sock_get_send_buf_size_wrapper, "(ii)i", NULL},
    {"sock_set_keep_alive", sock_set_keep_alive_wrapper, "(ii)i", NULL},
    {"sock_get_keep_alive", sock_get_keep_alive_wrapper, "(ii)i", NULL},
    {"sock_set_tcp_keep_idle", sock_set_tcp_keep_idle_wrapper, "(ii)i", NULL},
    {"sock_set_tcp_keep_intvl", sock_set_tcp_keep_intvl_wrapper, "(ii)i", NULL},
    {"sock_set_tcp_no_delay", sock_set_tcp_no_delay_wrapper, "(ii)i", NULL},
    {"sock_get_tcp_no_delay", sock_get_tcp_no_delay_wrapper, "(ii)i", NULL},
};

bool script_socket_env_register(void)
{
    if (s_socket_natives_registered)
    {
        return true;
    }

    if (!wasm_runtime_register_natives(SCRIPT_SOCKET_NATIVE_MODULE,
                                       s_socket_symbols,
                                       sizeof(s_socket_symbols) / sizeof(s_socket_symbols[0])))
    {
        ESP_LOGE(TAG, "failed to register socket natives in %s", SCRIPT_SOCKET_NATIVE_MODULE);
        return false;
    }

    s_socket_natives_registered = true;
    return true;
}