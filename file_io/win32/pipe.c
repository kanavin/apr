/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "apr_arch_file_io.h"
#include "apr_file_io.h"
#include "apr_general.h"
#include "apr_strings.h"
#include "apr_escape.h"
#if APR_HAVE_ERRNO_H
#include <errno.h>
#endif
#include <string.h>
#include <stdio.h>
#if APR_HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#if APR_HAVE_PROCESS_H
#include <process.h>            /* for getpid() on Win32 */
#endif
#include "apr_arch_misc.h"

APR_DECLARE(apr_status_t) apr_file_pipe_timeout_set(apr_file_t *thepipe,
                                            apr_interval_time_t timeout)
{
    /* Always OK to unset timeouts */
    if (timeout == -1) {
        thepipe->timeout = timeout;
        return APR_SUCCESS;
    }
    if (thepipe->ftype != APR_FILETYPE_PIPE) {
        return APR_ENOTIMPL;
    }
    if (timeout && !(thepipe->pOverlapped)) {
        /* Cannot be nonzero if a pipe was opened blocking
         */
        return APR_EINVAL;
    }
    thepipe->timeout = timeout;
    return APR_SUCCESS;
}

APR_DECLARE(apr_status_t) apr_file_pipe_timeout_get(apr_file_t *thepipe,
                                           apr_interval_time_t *timeout)
{
    /* Always OK to get the timeout (even if it's unset ... -1) */
    *timeout = thepipe->timeout;
    return APR_SUCCESS;
}

static apr_status_t file_pipe_create(apr_file_t **in,
                                     apr_file_t **out,
                                     apr_int32_t blocking,
                                     apr_pool_t *pool_in,
                                     apr_pool_t *pool_out)
{
    SECURITY_ATTRIBUTES sa;
    static unsigned long id = 0;
    DWORD dwPipeMode;
    DWORD dwOpenMode;

    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    sa.lpSecurityDescriptor = NULL;

    (*in) = (apr_file_t *)apr_pcalloc(pool_in, sizeof(apr_file_t));
    (*in)->pool = pool_in;
    (*in)->fname = NULL;
    (*in)->ftype = APR_FILETYPE_PIPE;
    (*in)->timeout = -1;
    (*in)->ungetchar = -1;
    (*in)->eof_hit = 0;
    (*in)->filePtr = 0;
    (*in)->bufpos = 0;
    (*in)->dataRead = 0;
    (*in)->direction = 0;
    (*in)->pOverlapped = NULL;
#if APR_FILES_AS_SOCKETS
    (void) apr_pollset_create(&(*in)->pollset, 1, p, 0);
#endif
    (*out) = (apr_file_t *)apr_pcalloc(pool_out, sizeof(apr_file_t));
    (*out)->pool = pool_out;
    (*out)->fname = NULL;
    (*out)->ftype = APR_FILETYPE_PIPE;
    (*out)->timeout = -1;
    (*out)->ungetchar = -1;
    (*out)->eof_hit = 0;
    (*out)->filePtr = 0;
    (*out)->bufpos = 0;
    (*out)->dataRead = 0;
    (*out)->direction = 0;
    (*out)->pOverlapped = NULL;
#if APR_FILES_AS_SOCKETS
    (void) apr_pollset_create(&(*out)->pollset, 1, p, 0);
#endif
    if (apr_os_level >= APR_WIN_NT) {
        apr_status_t rv;
        char rand[8];
        int pid = getpid();
#define FMT_PIPE_NAME "\\\\.\\pipe\\apr-pipe-%x.%lx."
        /*                                    ^   ^ ^
         *                                  pid   | |
         *                                        | |
         *                                       id |
         *                                          |
         *                        hex-escaped rand[8] (16 bytes)
         */
        char name[sizeof FMT_PIPE_NAME + 2 * sizeof(pid)
                                       + 2 * sizeof(id)
                                       + 2 * sizeof(rand)];
        apr_size_t pos;

        /* Create the read end of the pipe */
        dwOpenMode = PIPE_ACCESS_INBOUND;
#ifdef FILE_FLAG_FIRST_PIPE_INSTANCE
        dwOpenMode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
#endif
        if (blocking == APR_WRITE_BLOCK /* READ_NONBLOCK */
               || blocking == APR_FULL_NONBLOCK) {
            dwOpenMode |= FILE_FLAG_OVERLAPPED;
            (*in)->pOverlapped =
                    (OVERLAPPED*) apr_pcalloc((*in)->pool, sizeof(OVERLAPPED));
            (*in)->pOverlapped->hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
            (*in)->timeout = 0;
        }
        dwPipeMode = 0;

        rv = apr_generate_random_bytes(rand, sizeof rand);
        if (rv != APR_SUCCESS) {
            file_cleanup(*in);
            return rv;
        }

        pos = apr_snprintf(name, sizeof name, FMT_PIPE_NAME, pid, id++);
        apr_escape_hex(name + pos, rand, sizeof rand, 0, NULL);

        (*in)->filehand = CreateNamedPipe(name,
                                          dwOpenMode,
                                          dwPipeMode,
                                          1,            /* nMaxInstances,   */
                                          0,            /* nOutBufferSize,  */
                                          65536,        /* nInBufferSize,   */
                                          1,            /* nDefaultTimeOut, */
                                          &sa);
        if ((*in)->filehand == INVALID_HANDLE_VALUE) {
            rv = apr_get_os_error();
            file_cleanup(*in);
            return rv;
        }

        /* Create the write end of the pipe */
        dwOpenMode = FILE_ATTRIBUTE_NORMAL;
        if (blocking == APR_READ_BLOCK /* WRITE_NONBLOCK */
                || blocking == APR_FULL_NONBLOCK) {
            dwOpenMode |= FILE_FLAG_OVERLAPPED;
            (*out)->pOverlapped =
                    (OVERLAPPED*) apr_pcalloc((*out)->pool, sizeof(OVERLAPPED));
            (*out)->pOverlapped->hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
            (*out)->timeout = 0;
        }

        (*out)->filehand = CreateFile(name,
                                      GENERIC_WRITE,   /* access mode             */
                                      0,               /* share mode              */
                                      &sa,             /* Security attributes     */
                                      OPEN_EXISTING,   /* dwCreationDisposition   */
                                      dwOpenMode,      /* Pipe attributes         */
                                      NULL);           /* handle to template file */
        if ((*out)->filehand == INVALID_HANDLE_VALUE) {
            apr_status_t rv = apr_get_os_error();
            file_cleanup(*out);
            file_cleanup(*in);
            return rv;
        }
    }
    else {
        /* Pipes on Win9* are blocking. Live with it. */
        if (!CreatePipe(&(*in)->filehand, &(*out)->filehand, &sa, 65536)) {
            return apr_get_os_error();
        }
    }

    apr_pool_cleanup_register((*in)->pool, (void *)(*in), file_cleanup,
                        apr_pool_cleanup_null);
    apr_pool_cleanup_register((*out)->pool, (void *)(*out), file_cleanup,
                        apr_pool_cleanup_null);
    return APR_SUCCESS;
}

APR_DECLARE(apr_status_t) apr_file_pipe_create(apr_file_t **in,
                                               apr_file_t **out,
                                               apr_pool_t *pool)
{
    /* Default is full blocking pipes. */
    return file_pipe_create(in, out, APR_FULL_BLOCK, pool, pool);
}

APR_DECLARE(apr_status_t) apr_file_pipe_create_ex(apr_file_t **in,
                                                  apr_file_t **out,
                                                  apr_int32_t blocking,
                                                  apr_pool_t *pool)
{
    return file_pipe_create(in, out, blocking, pool, pool);
}

APR_DECLARE(apr_status_t) apr_file_pipe_create_pools(apr_file_t **in,
                                                     apr_file_t **out,
                                                     apr_int32_t blocking,
                                                     apr_pool_t *pool_in,
                                                     apr_pool_t *pool_out)
{
    return file_pipe_create(in, out, blocking, pool_in, pool_out);
}

APR_DECLARE(apr_status_t) apr_file_namedpipe_create(const char *filename,
                                                    apr_fileperms_t perm,
                                                    apr_pool_t *pool)
{
    /* Not yet implemented, interface not suitable.
     * Win32 requires the named pipe to be *opened* at the time it's
     * created, and to do so, blocking or non blocking must be elected.
     */
    return APR_ENOTIMPL;
}


/* XXX: Problem; we need to choose between blocking and nonblocking based
 * on how *thefile was opened, and we don't have that information :-/
 * Hack; assume a blocking socket, since the most common use for the fn
 * would be to handle stdio-style or blocking pipes.  Win32 doesn't have
 * select() blocking for pipes anyways :(
 */
APR_DECLARE(apr_status_t) apr_os_pipe_put_ex(apr_file_t **file,
                                             apr_os_file_t *thefile,
                                             int register_cleanup,
                                             apr_pool_t *pool)
{
    (*file) = apr_pcalloc(pool, sizeof(apr_file_t));
    (*file)->pool = pool;
    (*file)->ftype = APR_FILETYPE_PIPE;
    (*file)->timeout = -1;
    (*file)->ungetchar = -1;
    (*file)->filehand = *thefile;
#if APR_FILES_AS_SOCKETS
    (void) apr_pollset_create(&(*file)->pollset, 1, pool, 0);
#endif
    if (register_cleanup) {
        apr_pool_cleanup_register(pool, *file, file_cleanup,
                                  apr_pool_cleanup_null);
    }

    return APR_SUCCESS;
}


APR_DECLARE(apr_status_t) apr_os_pipe_put(apr_file_t **file,
                                          apr_os_file_t *thefile,
                                          apr_pool_t *pool)
{
    return apr_os_pipe_put_ex(file, thefile, 0, pool);
}

static apr_status_t create_socket_pipe(SOCKET *rd, SOCKET *wr)
{
    FD_SET rs;
    SOCKET ls;
    struct timeval socktm;
    struct sockaddr_in pa;
    struct sockaddr_in la;
    struct sockaddr_in ca;
    int nrd;
    apr_status_t rv;
    int ll = sizeof(la);
    int lc = sizeof(ca);
    unsigned long bm = 1;
    char uid[8];
    char iid[8];

    *rd = INVALID_SOCKET;
    *wr = INVALID_SOCKET;

    /* Create the unique socket identifier
     * so that we know the connection originated
     * from us.
     */
    rv = apr_generate_random_bytes(uid, sizeof(uid));
    if (rv != APR_SUCCESS) {
        return rv;
    }

    if ((ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == INVALID_SOCKET) {
        return apr_get_netos_error();
    }

    pa.sin_family = AF_INET;
    pa.sin_port   = 0;
    pa.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (bind(ls, (SOCKADDR *)&pa, sizeof(pa)) == SOCKET_ERROR) {
        rv =  apr_get_netos_error();
        goto cleanup;
    }
    if (getsockname(ls, (SOCKADDR *)&la, &ll) == SOCKET_ERROR) {
        rv =  apr_get_netos_error();
        goto cleanup;
    }
    if (listen(ls, 1) == SOCKET_ERROR) {
        rv =  apr_get_netos_error();
        goto cleanup;
    }
    if ((*wr = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == INVALID_SOCKET) {
        rv = apr_get_netos_error();
        goto cleanup;
    }
    if (connect(*wr, (SOCKADDR *)&la, sizeof(la)) == SOCKET_ERROR) {
        rv =  apr_get_netos_error();
        goto cleanup;
    }
    if (send(*wr, uid, sizeof(uid), 0) != sizeof(uid)) {
        if ((rv =  apr_get_netos_error()) == 0) {
            rv = APR_EINVAL;
        }
        goto cleanup;
    }
    if (ioctlsocket(ls, FIONBIO, &bm) == SOCKET_ERROR) {
        rv = apr_get_netos_error();
        goto cleanup;
    }
    for (;;) {
        int ns;
        int nc = 0;
        /* Listening socket is nonblocking by now.
         * The accept should create the socket
         * immediatelly because we are connected already.
         * However on buys systems this can take a while
         * until winsock gets a chance to handle the events.
         */
        FD_ZERO(&rs);
        FD_SET(ls, &rs);

        socktm.tv_sec  = 1;
        socktm.tv_usec = 0;
        if ((ns = select(0, &rs, NULL, NULL, &socktm)) == SOCKET_ERROR) {
            /* Accept still not signaled */
            Sleep(100);
            continue;
        }
        if (ns == 0) {
            /* No connections in the last second */
            continue;
        }
        if ((*rd = accept(ls, (SOCKADDR *)&ca, &lc)) == INVALID_SOCKET) {
            rv =  apr_get_netos_error();
            goto cleanup;
        }
        /* Verify the connection by reading/waiting for the identification */
        bm = 0;
        if (ioctlsocket(*rd, FIONBIO, &bm) == SOCKET_ERROR) {
            rv = apr_get_netos_error();
            goto cleanup;
        }
        nrd = recv(*rd, iid, sizeof(iid), 0);
        if (nrd == SOCKET_ERROR) {
            rv = apr_get_netos_error();
            goto cleanup;
        }
        if (nrd == (int)sizeof(uid) && memcmp(iid, uid, sizeof(uid)) == 0) {
            /* Got the right identifier, put the poll()able read side of
             * the pipe in nonblocking mode and return.
             */
            bm = 1;
            if (ioctlsocket(*rd, FIONBIO, &bm) == SOCKET_ERROR) {
                rv = apr_get_netos_error();
                goto cleanup;
            }
            break;
        }
        closesocket(*rd);
    }
    /* We don't need the listening socket any more */
    closesocket(ls);
    return 0;

cleanup:
    /* Don't leak resources */
    closesocket(ls);
    if (*rd != INVALID_SOCKET)
        closesocket(*rd);
    if (*wr != INVALID_SOCKET)
        closesocket(*wr);

    *rd = INVALID_SOCKET;
    *wr = INVALID_SOCKET;
    return rv;
}

static apr_status_t socket_pipe_cleanup(void *thefile)
{
    apr_file_t *file = thefile;
    if (file->filehand != INVALID_HANDLE_VALUE) {
        shutdown((SOCKET)file->filehand, SD_BOTH);
        closesocket((SOCKET)file->filehand);
        file->filehand = INVALID_HANDLE_VALUE;
    }
    return APR_SUCCESS;
}

apr_status_t apr_file_socket_pipe_create(apr_file_t **in,
                                         apr_file_t **out,
                                         apr_pool_t *p)
{
    apr_status_t rv;
    SOCKET rd;
    SOCKET wr;

    if ((rv = create_socket_pipe(&rd, &wr)) != APR_SUCCESS) {
        return rv;
    }
    (*in) = (apr_file_t *)apr_pcalloc(p, sizeof(apr_file_t));
    (*in)->pool = p;
    (*in)->fname = NULL;
    (*in)->ftype = APR_FILETYPE_SOCKET;
    (*in)->timeout = 0; /* read end of the pipe is non-blocking */
    (*in)->ungetchar = -1;
    (*in)->eof_hit = 0;
    (*in)->filePtr = 0;
    (*in)->bufpos = 0;
    (*in)->dataRead = 0;
    (*in)->direction = 0;
    (*in)->pOverlapped = NULL;
    (*in)->filehand = (HANDLE)rd;

    (*out) = (apr_file_t *)apr_pcalloc(p, sizeof(apr_file_t));
    (*out)->pool = p;
    (*out)->fname = NULL;
    (*out)->ftype = APR_FILETYPE_SOCKET;
    (*out)->timeout = -1;
    (*out)->ungetchar = -1;
    (*out)->eof_hit = 0;
    (*out)->filePtr = 0;
    (*out)->bufpos = 0;
    (*out)->dataRead = 0;
    (*out)->direction = 0;
    (*out)->pOverlapped = NULL;
    (*out)->filehand = (HANDLE)wr;

    apr_pool_cleanup_register(p, (void *)(*in), socket_pipe_cleanup,
                              apr_pool_cleanup_null);
    apr_pool_cleanup_register(p, (void *)(*out), socket_pipe_cleanup,
                              apr_pool_cleanup_null);

    return rv;
}

apr_status_t apr_file_socket_pipe_close(apr_file_t *file)
{
    apr_status_t stat;
    if (file->ftype != APR_FILETYPE_SOCKET)
        return apr_file_close(file);
    if ((stat = socket_pipe_cleanup(file)) == APR_SUCCESS) {
        apr_pool_cleanup_kill(file->pool, file, socket_pipe_cleanup);

        if (file->mutex) {
            apr_thread_mutex_destroy(file->mutex);
        }

        return APR_SUCCESS;
    }
    return stat;
}
