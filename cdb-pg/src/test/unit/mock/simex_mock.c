/*
 * Auto-generated Mocking Source
 * 
 * Source: utils/simex.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "lib/stringinfo.h"
#include "utils/elog.h"
#include "utils/simex.h"

void 
simex_init(void)
{



    mock();

}

void 
simex_set_sync_bitvector_container(SyncBitvectorContainer container)
{

    check_expected(container);

    mock();

}

int32 
simex_get_subclass_count(void)
{



    return (int32) mock();

}

SimExESSubClass 
simex_check(const char *file, int32 line)
{

    check_expected(file);
    check_expected(line);

    return (SimExESSubClass) mock();

}

bool            gp_simex_init;
int             gp_simex_class;
double          gp_simex_rand;
bool            gp_simex_run;
int 
gp_socket(int socket_family, int socket_type, int protocol)
{

    check_expected(socket_family);
    check_expected(socket_type);
    check_expected(protocol);

    return (int) mock();

}

int 
gp_poll(struct pollfd * fds, uint32 nfds, int timeout)
{

    check_expected(fds);
    check_expected(nfds);
    check_expected(timeout);
    optional_assignment(fds);
    return (int) mock();

}

int 
gp_send(int socket, const void *buffer, size_t length, int flags)
{

    check_expected(socket);
    check_expected(buffer);
    check_expected(length);
    check_expected(flags);

    return (int) mock();

}

int 
gp_recv(int socket, void *buffer, size_t length, int flags)
{

    check_expected(socket);
    check_expected(buffer);
    check_expected(length);
    check_expected(flags);
    optional_assignment(buffer);
    return (int) mock();

}
