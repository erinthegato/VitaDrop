#ifndef VITADROP_SERVER_H
#define VITADROP_SERVER_H

#include <stdint.h>

/* A snapshot of what the server is doing, for showing on screen. */
typedef struct {
    int running;               /* 1 once the server is listening */
    int files_received;        /* how many files were saved */
    uint64_t bytes_received;   /* total bytes saved */
    int busy;                  /* 1 while a file is being received */
    char current_name[128];    /* file being received right now */
    uint64_t current_done;     /* bytes of the current file received so far */
    uint64_t current_total;    /* size of the current file */
    char message[160];         /* last thing that happened */
} ServerStatus;

/* Starts the web server in the background. Files are saved into save_dir. */
int server_start(const char *save_dir, int port);

/* Asks the server to stop. */
void server_stop(void);

/* Copies the current status into *out. */
void server_get_status(ServerStatus *out);

#endif
