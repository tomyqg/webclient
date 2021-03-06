/*
 * File      : webclient.c
 * COPYRIGHT (C) 2012-2013, Shanghai Real-Thread Technology Co., Ltd
 *
 * Change Logs:
 * Date           Author       Notes
 * 2013-05-05     Bernard      the first version
 * 2013-06-10     Bernard      fix the slow speed issue when download file.
 * 2015-11-14     aozima       add content_length_remainder.
 */

#include "webclient.h"

#include <string.h>

#include <lwip/netdb.h>
#include <lwip/sockets.h>

#include "webclient_internal.h"

// #define DEBUG_ENABLE
#define DEBUG_SECTION_NAME  "[WEB]"
#define DEBUG_LEVEL         DBG_LOG
#define DEBUG_COLOR

#include <rtdbg.h>

#define WEBCLIENT_SOCKET_TIMEO  6000 /* 6 second */

extern long int strtol(const char *nptr, char **endptr, int base);

char *webclient_strdup(const char *s)
{
    size_t len = strlen(s) + 1;
    char *tmp = (char *)web_malloc(len);

    if (!tmp) return NULL;

    memcpy(tmp, s, len);

    return tmp;
}

static char* webclient_header_skip_prefix(char* line, const char* prefix)
{
    char *ptr;
    size_t len = strlen(prefix);

    if (strncmp(line, prefix, len))
        return NULL;

    ptr = line + len;

    /* skip whitespace */
    while (*ptr && (*ptr == ' ' || *ptr == '\t'))
        ptr += 1;

    /* remove '\r\n' */
    line = ptr;
    ptr = strstr(line, "\r\n");
    if (ptr != RT_NULL)
    {
        *ptr = '\0';
    }

    return line;
}

/*
 * When a request has been sent, we can expect mime headers to be
 * before the data.  We need to read exactly to the end of the headers
 * and no more data.  This readline reads a single char at a time.
 */
static int webclient_read_line(int socket, char * buffer, int size)
{
    int rc;
    char *ptr = buffer;
    int count = 0;

    /* Keep reading until we fill the buffer. */
    while (count < size)
    {
        rc = recv(socket, ptr, 1, 0);
        if (rc <= 0)
            return rc;

        if (*ptr == '\n')
        {
            ptr++;
            count++;
            break;
        }

        /* increment after check for cr.  Don't want to count the cr. */
        count++;
        ptr++;
    }

    /* add terminate string */
    *ptr = '\0';

    dbg_log(DBG_LOG, "%s\n", buffer);

    return count;
}

/*
 * resolve server address
 * @param server the server sockaddress
 * @param url the input URL address, for example, http://www.rt-thread.org/index.html
 * @param host_addr the buffer pointer to save server host address
 * @param request the pointer to point the request url, for example, /index.html
 *
 * @return 0 on resolve server address OK, others failed
 */
static int webclient_resolve_address(struct webclient_session* session, struct sockaddr_in *server,
        const char *url, char** request)
{
    int rc = WEBCLIENT_OK;
    char *ptr;
    char port[6] = "80"; /* default port of 80(http) */
    int i = 0, is_domain;
    struct hostent *hptr;
    const char *host_addr = 0;
    int url_len, host_addr_len = 0;

    url_len = strlen(url);

    /* strip protocol(http) */
    ptr = strchr(url, ':');
    if (ptr != NULL)
    {
        url = ptr + 1;
    }

    /* URL must start with double forward slashes. */
    if ((url[0] != '/') || (url[1] != '/'))
    {
        rc = -1;
        goto _exit;
    }

    url += 2;
    is_domain = 0;
    i = 0;
    /* allow specification of port in URL like http://www.server.net:8080/ */
    while (*url)
    {
        if (*url == '/')
        {
            host_addr_len = url - host_addr;
            break;
        }

        if (*url == ':')
        {
            unsigned char w;

            host_addr_len = url - host_addr;

            for (w = 0; w < 5 && url[w + 1] != '/' && url[w + 1] != '\0'; w++)
                port[w] = url[w + 1];

            /* get port ok */
            port[w] = '\0';
            url += w + 1;
            break;
        }

        if ((*url < '0' || *url > '9') && *url != '.')
            is_domain = 1;

        if(host_addr == 0)
        {
            host_addr = url;
            //rt_kprintf("webclient_resolve_address host_addr start: %s\n", host_addr);
        }

        url++;
    }
    *request = (char*) url;

    if((host_addr_len < 1) || (host_addr_len > url_len) )
    {
        //rt_kprintf("webclient_resolve_address host_addr_len: %d error!\n", host_addr_len);
        rc = -1;
        goto _exit;
    }

    /* get host addr ok. */
    {
        char *host_addr_new = web_malloc(host_addr_len+1);

        if(!host_addr_new)
        {
            rc = -1;
            goto _exit;
        }

        memcpy(host_addr_new, host_addr, host_addr_len);
        host_addr_new[host_addr_len] = '\0';
        session->host = host_addr_new;
        //rt_kprintf("session->host: %s\n", session->host);
    }

    if (is_domain)
    {
        /* resolve the host name. */
        hptr = gethostbyname(session->host);
        if (hptr == 0)
        {
            rt_kprintf("WEBCLIENT: failed to resolve domain '%s'\n", session->host);
            rc = -1;
            goto _exit;
        }
        memcpy(&server->sin_addr, *hptr->h_addr_list, sizeof(server->sin_addr));
    }
    else
    {
        inet_aton(session->host, (struct in_addr* )&(server->sin_addr));
    }
    /* set the port */
    server->sin_port = htons((int) strtol(port, NULL, 10));
    server->sin_family = AF_INET;

_exit:
    if(rc != WEBCLIENT_OK)
    {
        if(session->host)
        {
            web_free(session->host);
            session->host = 0;
        }
    }

    return rc;
}

int webclient_send_header(struct webclient_session* session, int method,
        const char *header, size_t header_sz)
{
    int rc = WEBCLIENT_OK;
    unsigned char *header_buffer = RT_NULL, *header_ptr;

    if (header == RT_NULL)
    {
        header_buffer = web_malloc (WEBCLIENT_HEADER_BUFSZ);
        if (header_buffer == RT_NULL)
        {
            rc = -WEBCLIENT_NOMEM;
            goto __exit;
        }

        header_ptr = header_buffer;
        header_ptr += rt_snprintf((char*) header_ptr,
                WEBCLIENT_HEADER_BUFSZ - (header_ptr - header_buffer),
                "GET %s HTTP/1.1\r\n",
                session->request ? session->request : "/");
        header_ptr += rt_snprintf((char*) header_ptr,
                WEBCLIENT_HEADER_BUFSZ - (header_ptr - header_buffer),
                "Host: %s\r\n", session->host);
        header_ptr += rt_snprintf((char*) header_ptr,
                WEBCLIENT_HEADER_BUFSZ - (header_ptr - header_buffer),
                "User-Agent: RT-Thread HTTP Agent\r\n\r\n");

        webclient_write(session, header_buffer, header_ptr - header_buffer);
    }
    else
    {
        if (method != WEBCLIENT_USER_METHOD)
        {
            header_buffer = web_malloc (WEBCLIENT_HEADER_BUFSZ);
            if (header_buffer == RT_NULL)
            {
                rc = -WEBCLIENT_NOMEM;
                goto __exit;
            }

            header_ptr = header_buffer;

            if (strstr(header, "HTTP/1.") == RT_NULL)
            {
                if (method == WEBCLIENT_GET)
                    header_ptr += rt_snprintf((char*) header_ptr,
                            WEBCLIENT_HEADER_BUFSZ
                                    - (header_ptr - header_buffer),
                            "GET %s HTTP/1.1\r\n",
                            session->request ? session->request : "/");
                else if (method == WEBCLIENT_POST)
                    header_ptr += rt_snprintf((char*) header_ptr,
                            WEBCLIENT_HEADER_BUFSZ
                                    - (header_ptr - header_buffer),
                            "POST %s HTTP/1.1\r\n",
                            session->request ? session->request : "/");
            }

            if (strstr(header, "Host:") == RT_NULL)
            {
                header_ptr += rt_snprintf((char*) header_ptr,
                        WEBCLIENT_HEADER_BUFSZ - (header_ptr - header_buffer),
                        "Host: %s\r\n", session->host);
            }

            if (strstr(header, "User-Agent:") == RT_NULL)
            {
                header_ptr += rt_snprintf((char*) header_ptr,
                        WEBCLIENT_HEADER_BUFSZ - (header_ptr - header_buffer),
                        "User-Agent: RT-Thread HTTP Agent\r\n");
            }

            if (strstr(header, "Accept: ") == RT_NULL)
            {
                header_ptr += rt_snprintf((char*) header_ptr,
                        WEBCLIENT_HEADER_BUFSZ - (header_ptr - header_buffer),
                        "Accept: */*\r\n");
            }

            if ((WEBCLIENT_HEADER_BUFSZ - (header_ptr - header_buffer))
                    < (int) header_sz + 3)
            {
                rc = -WEBCLIENT_NOBUFFER;
                goto __exit;
            }

            /* append user's header */
            memcpy(header_ptr, header, header_sz);
            header_ptr += header_sz;
            header_ptr += rt_snprintf((char*) header_ptr,
                    WEBCLIENT_HEADER_BUFSZ - (header_ptr - header_buffer),
                    "\r\n");

            webclient_write(session, header_buffer, header_ptr - header_buffer);
        }
        else
        {
            webclient_write(session, (unsigned char*) header, header_sz);
        }
    }

__exit:
    web_free(header_buffer);
    return rc;
}

int webclient_handle_response(struct webclient_session* session)
{
    int rc;
    int content_length = -1;
    char *mimeBuffer, *mime_ptr;

    if (!session) return -1;

    /* set content length of session */
    session->content_length = -1;

    mimeBuffer = (char*)web_malloc(WEBCLIENT_RESPONSE_BUFSZ + 1);
    if (!mimeBuffer)
        return -1;

    /* We now need to read the header information */
    while (1)
    {
        int i;

        /* read a line from the header information. */
        rc = webclient_read_line(session->socket, mimeBuffer, WEBCLIENT_RESPONSE_BUFSZ);
        if (rc < 0)
            break;

        /* set terminal charater */
        mimeBuffer[rc] = '\0';

        /* End of headers is a blank line.  exit. */
        if (rc == 0)
            break;
        if ((rc == 2) && (mimeBuffer[0] == '\r'))
            break;

        mime_ptr = webclient_header_skip_prefix(mimeBuffer, "HTTP/1.");
        if (mime_ptr != RT_NULL)
        {
            mime_ptr += 1;
            while (*mime_ptr && (*mime_ptr == ' ' || *mime_ptr == '\t'))
                mime_ptr++;
            /* Terminate string after status code */
            for (i = 0; ((mime_ptr[i] != ' ') && (mime_ptr[i] != '\t')); i++)
                ;
            mime_ptr[i] = '\0';

            session->response = (int) strtol(mime_ptr, RT_NULL, 10);
        }
        mime_ptr = webclient_header_skip_prefix(mimeBuffer, "Last-Modified:");
        if (mime_ptr != RT_NULL)
        {
            session->last_modified = webclient_strdup(mime_ptr);
        }
        mime_ptr = webclient_header_skip_prefix(mimeBuffer,
                "Transfer-Encoding: ");
        if (mime_ptr != RT_NULL)
        {
            session->transfer_encoding = webclient_strdup(mime_ptr);
        }
        mime_ptr = webclient_header_skip_prefix(mimeBuffer, "Content-Type:");
        if (mime_ptr != RT_NULL)
        {
            session->content_type = webclient_strdup(mime_ptr);
        }
        mime_ptr = webclient_header_skip_prefix(mimeBuffer, "Content-Length:");
        if (mime_ptr != RT_NULL)
        {
            session->content_length = (int) strtol(mime_ptr, RT_NULL, 10);
        }
        mime_ptr = webclient_header_skip_prefix(mimeBuffer, "Location: ");
        if (mime_ptr != RT_NULL)
        {
            session->location = webclient_strdup(mime_ptr);
        }
        mime_ptr = webclient_header_skip_prefix(mimeBuffer, "Content-Range:");
        if (mime_ptr != RT_NULL)
        {
            char *ptr;

            mime_ptr = webclient_header_skip_prefix(mime_ptr, "bytes");
            while (*mime_ptr == ' ')
                mime_ptr++;

            session->position = atoi(mime_ptr);
            ptr = strstr(mime_ptr, "/");
            if (ptr)
            {
                ptr ++;
                content_length = atoi(ptr);
            }
        }
    }

    /* use the content length in content range */
    if (content_length != -1)
        session->content_length = content_length;

    session->content_length_remainder =
            (session->content_length) ? session->content_length : 0xFFFFFFFF;

    if (session->transfer_encoding
            && strcmp(session->transfer_encoding, "chunked") == 0)
    {
        /* chunk mode, we should get the first chunk size */
        webclient_read_line(session->socket, mimeBuffer, WEBCLIENT_RESPONSE_BUFSZ);
        session->chunk_sz = strtol(mimeBuffer, RT_NULL, 16);
        session->chunk_offset = 0;
    }

    /* release buffer */
    web_free(mimeBuffer);

    if (rc < 0)
        return rc;

    return session->response;
}

/*
 This is the main HTTP client connect work.  Makes the connection
 and handles the protocol and reads the return headers.  Needs
 to leave the stream at the start of the real data.
 */
int webclient_connect(struct webclient_session* session, const char *URI)
{
    int rc;
    int socket_handle;
    int timeout = WEBCLIENT_SOCKET_TIMEO;
    struct sockaddr_in server;
    char *request;

    RT_ASSERT(session != RT_NULL);

    /* initialize the socket of session */
    session->socket = -1;

    /* Check valid IP address and URL */
    rc = webclient_resolve_address(session, &server, URI, &request);
    if (rc != WEBCLIENT_OK)
        return rc;

    /* copy host address */
    if (*request)
        session->request = webclient_strdup(request);
    else
        session->request = RT_NULL;

    socket_handle = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle < 0)
        return -WEBCLIENT_NOSOCKET;

    /* set recv timeout option */
    setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, (void*) &timeout,
            sizeof(timeout));
    setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO, (void*) &timeout,
            sizeof(timeout));

    if (connect(socket_handle, (struct sockaddr *) &server,
            sizeof(struct sockaddr)) != 0)
    {
        /* connect failed, close socket handle */
        closesocket(socket_handle);
        return -WEBCLIENT_CONNECT_FAILED;
    }

    session->socket = socket_handle;
    return WEBCLIENT_OK;
}

struct webclient_session* webclient_open(const char* URI)
{
    struct webclient_session* session;

    /* create session */
    session = (struct webclient_session*) web_malloc(sizeof(struct webclient_session));
    if (session == RT_NULL)
        return RT_NULL;
    memset(session, 0x0, sizeof(struct webclient_session));

    if (webclient_connect(session, URI) < 0)
    {
        /* connect to webclient server failed. */
        webclient_close(session);
        return RT_NULL;
    }

    if (webclient_send_header(session, WEBCLIENT_GET, RT_NULL, 0)
            != WEBCLIENT_OK)
    {
        /* connect to webclient server failed. */
        webclient_close(session);
        return RT_NULL;
    }

    /* handle the response header of webclient server */
    if (webclient_handle_response(session))
    {
        /* relocation */
        if ((session->response == 302 || session->response == 301) && session->location)
        {
            char *location = webclient_strdup(session->location);
            if (location)
            {
                webclient_close(session);
                session = webclient_open(location);

                web_free(location);
                return session;
            }
        }
    }

    /* open successfully */
    return session;
}

struct webclient_session* webclient_open_position(const char* URI, int position)
{
    struct webclient_session* session;
    char *range_header;

    /* create session */
    session = (struct webclient_session*) web_malloc(sizeof(struct webclient_session));
    if (session == RT_NULL)
        return RT_NULL;
    memset(session, 0x0, sizeof(struct webclient_session));

    if (webclient_connect(session, URI) < 0)
    {
        /* connect to webclient server failed. */
        webclient_close(session);
        return RT_NULL;
    }

    range_header = (char*)web_malloc (WEBCLIENT_HEADER_BUFSZ);
    rt_snprintf(range_header, WEBCLIENT_HEADER_BUFSZ - 1,
            "Range: bytes=%d-\r\n", position);
    if (!range_header)
        goto __exit;

    if (webclient_send_header(session, WEBCLIENT_GET, range_header,
            rt_strlen(range_header)) != WEBCLIENT_OK)
    {
        /* connect to webclient server failed. */
        webclient_close(session);
        return RT_NULL;
    }

    /* handle the response header of webclient server */
    webclient_handle_response(session);
    /* relocation */
    if ((session->response == 302 || session->response == 301) && session->location)
    {
        char *location = webclient_strdup(session->location);
        if (location)
        {
            webclient_close(session);
            session = webclient_open_position(location, position);

            web_free(location);
            return session;
        }
    }

    /* open successfully */
    return session;

__exit:
    if (range_header)
        web_free(range_header);
    if (session)
        webclient_close(session);

    return RT_NULL;
}

struct webclient_session* webclient_open_header(const char* URI, int method,
        const char* header, size_t header_sz)
{
    struct webclient_session* session;

    /* create session */
    session = (struct webclient_session*) web_malloc(sizeof(struct webclient_session));
    if (session == RT_NULL)
        return RT_NULL;
    memset(session, 0, sizeof(struct webclient_session));

    if (webclient_connect(session, URI) < 0)
    {
        /* connect to webclient server failed. */
        webclient_close(session);
        return RT_NULL;
    }

    /* write request header */
    if (webclient_send_header(session, method, header, header_sz)
            != WEBCLIENT_OK)
    {
        /* send request header failed. */
        webclient_close(session);
        return RT_NULL;
    }

    /* handle the response header of webclient server */
    if (method == WEBCLIENT_GET)
    {
        webclient_handle_response(session);
    }

    /* open successfully */
    return session;
}

int webclient_set_timeout(struct webclient_session* session, int millisecond)
{
    RT_ASSERT(session != RT_NULL);

    /* set recv timeout option */
    setsockopt(session->socket, SOL_SOCKET, SO_RCVTIMEO,
            (void*) &millisecond, sizeof(millisecond));
    setsockopt(session->socket, SOL_SOCKET, SO_SNDTIMEO,
            (void*) &millisecond, sizeof(millisecond));

    return 0;
}

static int webclient_next_chunk(struct webclient_session* session)
{
    char line[64];
    int length;

    length = webclient_read_line(session->socket, line, sizeof(line));
    if (length)
    {
        if (strcmp(line, "\r\n") == 0)
        {
            length = webclient_read_line(session->socket, line, sizeof(line));
            if (length <= 0)
            {
                closesocket(session->socket);
                session->socket = -1;
                return length;
            }
        }
    }
    else
    {
        closesocket(session->socket);
        session->socket = -1;

        return length;
    }

    session->chunk_sz = strtol(line, RT_NULL, 16);
    session->chunk_offset = 0;

    if (session->chunk_sz == 0)
    {
        /* end of chunks */
        closesocket(session->socket);
        session->socket = -1;
    }

    return session->chunk_sz;
}

int webclient_read(struct webclient_session* session, unsigned char *buffer,
        size_t length)
{
    int bytesRead = 0;
    int totalRead = 0;
    int left;

    if (!session || session->socket < 0) return -WEBCLIENT_DISCONNECT;
    if (length == 0) return 0;

    /* which is transfered as chunk mode */
    if (session->chunk_sz)
    {
        if (length > (session->chunk_sz - session->chunk_offset))
            length = session->chunk_sz - session->chunk_offset;

        bytesRead = recv(session->socket, buffer, length, 0);
        if (bytesRead <= 0)
        {
            if (errno == EWOULDBLOCK || errno == EAGAIN)
            {
                /* recv timeout */
                return -WEBCLIENT_TIMEOUT;
            }
            else
            {
                closesocket(session->socket);
                session->socket = -1;
                return 0;
            }
        }

        session->chunk_offset += bytesRead;
        if (session->chunk_offset >= session->chunk_sz)
        {
            webclient_next_chunk(session);
        }

        return bytesRead;
    }

    if(session->content_length > 0)
    {
        if(length > session->content_length_remainder)
        {
            length = session->content_length_remainder;
        }

        if(length == 0)
        {
            return 0;
        }
    }

    /*
     * Read until: there is an error, we've read "size" bytes or the remote
     * side has closed the connection.
     */
    left = length;
    do
    {
        bytesRead = recv(session->socket, buffer + totalRead, left, 0);
        if (bytesRead <= 0)
        {
            rt_kprintf("errno=%d\n", bytesRead);

            if (totalRead)
            {
                rt_kprintf("totalRead=%d\n", totalRead);
                break;
            }
            else
            {
                rt_kprintf("EWOULDBLOCK=%d, EAGAIN=%d\n", EWOULDBLOCK, EAGAIN);

                if (errno == EWOULDBLOCK || errno == EAGAIN)
                {
                    /* recv timeout */
                    return -WEBCLIENT_TIMEOUT;
                }
                else
                {
                    closesocket(session->socket);
                    session->socket = -1;
                    return 0;
                }
            }
        }

        left -= bytesRead;
        totalRead += bytesRead;
    } while (left);

    if(session->content_length > 0)
    {
        session->content_length_remainder -= totalRead;
    }

    return totalRead;
}

int webclient_write(struct webclient_session* session,
        const unsigned char *buffer, size_t length)
{
    int bytesWrite = 0;
    int totalWrite = 0;
    int left = length;

    RT_ASSERT(session != RT_NULL);
    if (session->socket < 0)
        return -WEBCLIENT_DISCONNECT;

    /*
     * Send all of data on the buffer.
     */
    do
    {
        bytesWrite = send(session->socket, buffer + totalWrite, left, 0);
        if (bytesWrite <= 0)
        {
            if (errno == EWOULDBLOCK || errno == EAGAIN)
            {
                /* send timeout */
                if (totalWrite) return totalWrite;

                continue;
                /* TODO: whether return the TIMEOUT
                 * return -WEBCLIENT_TIMEOUT; */
            }
            else
            {
                closesocket(session->socket);
                session->socket = -1;

                if (totalWrite == 0) return -WEBCLIENT_DISCONNECT;

                break;
            }
        }

        left -= bytesWrite;
        totalWrite += bytesWrite;
    } while (left);

    return totalWrite;
}

/*
 * close a webclient client session.
 */
int webclient_close(struct webclient_session* session)
{
    RT_ASSERT(session != RT_NULL);

    if (session->socket >= 0)
        closesocket(session->socket);
    web_free(session->transfer_encoding);
    web_free(session->content_type);
    web_free(session->last_modified);
    web_free(session->host);
    web_free(session->request);
    web_free(session);

    return 0;
}

int webclient_response(struct webclient_session* session, void **response)
{
    unsigned char* buf_ptr;
    unsigned char* response_buf = 0;
    int length, total_read = 0;

    if (!session || !response) return -1;
    *response = NULL; /* initialize response */

    if (session->content_length < 0) /* not content length field kind */
    {
        size_t result_sz;

        total_read = 0;
        while (1)
        {
            unsigned char* new_resp;

            result_sz = total_read + WEBCLIENT_RESPONSE_BUFSZ;
            new_resp = web_realloc(response_buf, result_sz + 1);
            if(!new_resp)
            {
                rt_kprintf("no memory for realloc new_resp\n");
                break;
            }

            response_buf = new_resp;
            buf_ptr = (unsigned char*) response_buf + total_read;

            /* read result */
            length = webclient_read(session, buf_ptr, result_sz - total_read);
            if (length <= 0)
                break;

            total_read += length;
        }
    }
    else
    {
        int result_sz;

        result_sz = session->content_length;
        response_buf = web_malloc(result_sz + 1);
        if(!response_buf) return 0;

        buf_ptr = (unsigned char*) response_buf;
        for (total_read = 0; total_read < result_sz;)
        {
            length = webclient_read(session, buf_ptr, result_sz - total_read);

            if (length <= 0)
                break;

            buf_ptr += length;
            total_read += length;
        }
    }

    if ((total_read == 0) && (response_buf != 0))
    {
        web_free(response_buf);
        response_buf = NULL;
    }

    if (response_buf)
    {
        *response = response_buf;
        *(response_buf + total_read) = '\0';
    }

    return total_read;
}

/*
 * High level APIs for webclient client
 */
struct webclient_session* webclient_open_custom(const char* URI, int method,
        const char* header, size_t header_sz, const char* data, size_t data_sz)
{
    int rc = 0;
    size_t length;
    struct webclient_session* session = RT_NULL;

    /* create session */
    session = (struct webclient_session*) web_malloc(sizeof(struct webclient_session));
    if (!session)
    {
        rc = -WEBCLIENT_NOMEM;
        goto _err_exit;
    }
    memset(session, 0x0, sizeof(struct webclient_session));

    rc = webclient_connect(session, URI);
    if (rc < 0)
        goto _err_exit;

    /* send header */
    rc = webclient_send_header(session, method, header, header_sz);
    if (rc < 0)
        goto _err_exit;

    /* POST data */
    if (data)
    {
        length = webclient_write(session, (unsigned char*) data, data_sz);
        if (length != data_sz)
        {
            rt_kprintf("POST data %d:%d\n", length, data_sz);
            goto _err_exit;
        }
    }

    /* handle the response header of webclient server */
    webclient_handle_response(session);

    goto _success;

_err_exit:
    if(session)
    {
        webclient_close(session);
        session = 0;
    }

_success:
    return session;
}

int webclient_transfer(const char* URI, const char* header, size_t header_sz,
        const char* data, size_t data_sz, char *result, size_t result_sz)
{
    int rc = 0;
    int length, total_read = 0;
    unsigned char* buf_ptr;
    struct webclient_session* session = RT_NULL;

    /* create session */
    session = (struct webclient_session*) web_malloc(sizeof(struct webclient_session));
    if (!session)
    {
        rc = -WEBCLIENT_NOMEM;
        goto __exit;
    }
    memset(session, 0x0, sizeof(struct webclient_session));

    rc = webclient_connect(session, URI);
    if (rc < 0)
        goto __exit;

    /* send header */
    rc = webclient_send_header(session, WEBCLIENT_POST, header, header_sz);
    if (rc < 0)
        goto __exit;

    /* POST data */
    length = webclient_write(session, (unsigned char*) data, data_sz);
    if (length != data_sz)
    {
        rt_kprintf("POST data %d:%d\n", length, data_sz);
        goto __exit;
    }

    /* handle the response header of webclient server */
    webclient_handle_response(session);
    if (session->response != 200)
    {
        rt_kprintf("HTTP response: %d\n", session->response);
        goto __exit;
    }

    /* read response data */
    if (result == RT_NULL)
        goto __exit;

    if (session->content_length == 0)
    {
        total_read = 0;
        buf_ptr = (unsigned char*) result;
        while (1)
        {
            /* read result */
            length = webclient_read(session, buf_ptr + total_read,
                    result_sz - total_read);
            if (length <= 0)
                break;

            buf_ptr += length;
            total_read += length;
        }
    }
    else
    {
        buf_ptr = (unsigned char*) result;
        for (total_read = 0; total_read < result_sz;)
        {
            length = webclient_read(session, buf_ptr, result_sz - total_read);

            if (length <= 0)
                break;

            buf_ptr += length;
            total_read += length;
        }
    }

__exit:
    if (session != RT_NULL)
        webclient_close(session);
    if (rc < 0)
        return rc;

    return total_read;
}
