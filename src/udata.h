/*
** Structures to handle userdata transferred between Lua process
** See Copyright Notice in luaproc.h
*/

#ifndef _LUA_LUAPROC_UDATA_H_
#define _LUA_LUAPROC_UDATA_H_

#define BUF_SIZE 8192

//socket's structures

typedef int t_socket;
typedef t_socket *p_socket;

typedef struct t_timeout_ {
    double block;          /* maximum time for blocking calls */
    double total;          /* total number of miliseconds for operation */
    double start;          /* time of start of operation */
} t_timeout;
typedef t_timeout *p_timeout;

typedef const char *(*p_error) (
    void *ctx,          /* context needed by send */
    int err             /* error code */
);

/* interface to send function */
typedef int (*p_send) (
    void *ctx,          /* context needed by send */
    const char *data,   /* pointer to buffer with data to send */
    size_t count,       /* number of bytes to send from buffer */
    size_t *sent,       /* number of bytes sent uppon return */
    p_timeout tm        /* timeout control */
);

/* interface to recv function */
typedef int (*p_recv) (
    void *ctx,          /* context needed by recv */
    char *data,         /* pointer to buffer where data will be writen */
    size_t count,       /* number of bytes to receive into buffer */
    size_t *got,        /* number of bytes received uppon return */
    p_timeout tm        /* timeout control */
);

/* IO driver definition */
typedef struct t_io_ {
    void *ctx;          /* context needed by send/recv */
    p_send send;        /* send function pointer */
    p_recv recv;        /* receive function pointer */
    p_error error;      /* strerror function */
} t_io;
typedef t_io *p_io;

typedef struct t_buffer_ {
    double birthday;        /* throttle support info: creation time, */
    size_t sent, received;  /* bytes sent, and bytes received */
    p_io io;                /* IO driver used for this buffer */
    p_timeout tm;           /* timeout management for this buffer */
	size_t first, last;     /* index of first and last bytes of stored data */
	char data[BUF_SIZE];    /* storage space for buffer data */
} t_buffer;
typedef t_buffer *p_buffer;

typedef struct t_tcp_ {
    t_socket sock;
    t_io io;
    t_buffer buf;
    t_timeout tm;
} t_tcp;

typedef t_tcp *p_tcp;

#endif
