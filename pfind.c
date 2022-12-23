#include <stdio.h>
#include <thread.h>


/////////////////////////////
// Thread-safe queue for FIFO thread sync

// Mutex for queue (to make sure only one threads listens to the same condition signal at a time)
mtx_t mtx;

struct thread_sync {
    mtx_t * mtx;
    cnd_t * not_empty;
};

struct thread_sync_entry {
    struct thread_sync * sync;
    struct thread_sync_entry * next;
};

struct thread_sync_queue {
    struct thread_sync_entry * head;
    struct thread_sync_entry * tail;
    int num_sleeping_threads;
};

void init(struct thread_sync_queue * q) {
    q->head = NULL;
    q->tail = NULL;
    q->num_sleeping_threads = 0;
}

void push(struct thread_sync_queue * q, struct thread_sync * sync) {
    mtx_lock(&mtx);
    struct thread_sync_entry * e = malloc(sizeof(struct thread_sync_entry));
    e->sync = sync;
    e->next = NULL;
    if (q->head == NULL) {
        q->head = e;
        q->tail = e;
    } else {
        q->tail->next = e;
        q->tail = e; 
    }
    mtx_unlock(&mtx);
}

struct thread_sync * pop(struct thread_sync_queue * q) {
    struct thread_sync_entry * e;
    struct thread_sync * sync;
    mtx_lock(&mtx);
    if (q->head == NULL) {
        return NULL;
    }
    e = q->head;
    q->head = e->next;
    if (q->head == NULL) {
        q->tail = NULL;
    }
    sync = e->sync;
    free(e);
    mtx_unlock(&mtx);
    return sync;
}

struct thread_sync_queue * available_syncs;
struct thread_sync_queue * waiting_syncs;

/////////////////////////////
// Thread-safe queue for holding unvisited search paths

struct entry {
    char * path;
    struct entry * next;
};

struct queue {
    struct entry * head;
    struct entry * tail;
};

void init(struct queue * q) {
    q->head = NULL;
    q->tail = NULL;
}

void exit_print(int num_found_files) {
    printf("Done searching, found %d files\n", num_found_files);
}

void push(struct queue * q, char * path, struct thread_sync * sync) {
    mtx_lock(sync->mtx);
    struct entry * e = malloc(sizeof(struct entry));
    e->path = path;
    e->next = NULL;
    if (q->head == NULL) {
        q->head = e;
        q->tail = e;
    } else {
        q->tail->next = e;
        q->tail = e;
    }
    cnd_signal(sync->not_empty);
    mtx_unlock(&sync->mtx);
}

char * pop(struct queue * q, struct thread_sync * sync) {
    mtx_lock(sync->mtx);
    while (q->head == NULL) {
        return NULL;
        cnd_wait(sync->not_empty, sync->mtx);
    }
    struct entry * e = q->head;
    q->head = e->next;
    if (q->head == NULL) {
        q->tail = NULL;
    }
    char * path = e->path;
    free(e);
    mtx_unlock(&mtx);
    return path;
}

void thread_scan(struct queue * q) {
    char * path;

    path = pop(q);

}

int main(int argc, char** argv) {
    char * search_root_dir, * search_term;
    int num_threads, cnd_init_return;
    struct queue * q;
    if (argc < 4) {
        fprintf(stderr, "Error: Not enough arguments.");
    }
    search_root_dir = argv[1];
    search_term = argv[2];
    num_threads = atoi(argv[3]);

    // Check if number of threads is valid
    if (num_threads <= 0) {
        fprintf(stderr, "Error: Invalid number of threads.");
        exit_print(0);
        exit(1);
    }

    // Initialize mutex
    if (mtx_init(&mtx, mtx_plain) != thrd_success) {
        fprintf(stderr, "Error: Mutex initialization failed.");
        exit_print(0);
        exit(1);
    }

    // Initialize condition variable
    cnd_init_return = cnd_init(&not_empty);
    if (cnd_init_return != thrd_success) {
        if (cnd_init_return == thrd_nomem) {
            fprintf(stderr, "Error: Condition variable initialization failed. Out of memory.");
        } else if (cnd_init_return == thrd_error) {
            fprintf(stderr, "Error: Condition variable initialization failed. Unknown error.");
        }
        exit_print(0);
        exit(1);
    }

    // Initialize queue
    q = malloc(sizeof(struct queue));
    init(&q);
    push(&q, search_root_dir);


    return 0;
}