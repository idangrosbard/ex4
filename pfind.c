#include <threads.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>

// To make sure that all threads start working at the same time
static cnd_t thread_ready_cnd;
static cnd_t opening_shot_cnd; 
static mtx_t opening_shot_mtx;
// To make sure that access to the Q is synchronized
static mtx_t q_mtx;

static cnd_t * thread_syncs; // Each sleeping thread will wait on a single thread_sync
static atomic_int num_threads = 0; // The number of thread_syncs
static atomic_int sleep_head_idx = 0; // The index of the thread_sync that the next push operation will wake up
static atomic_int sleep_tail_idx = 0; // The index of the thread_sync that the next pop operation will sleep on
static atomic_int num_sleeping; // The number of currently sleeping threads

static atomic_ullong recognized_files = 0; // Number of files found
static atomic_int experienced_error; // Whether an error has occurred
static char * search_term; // The string we're searching for
//static char * search_root; // The dir we're searchign in


///////////////////////////////
// A queue to hold the paths yet to be checked

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

void push(struct queue * q, char * path) {
    cnd_t sync;
    struct entry * e;
    // First we make sure that if there are sleeping threads, the push will send different wake up signals according to FIFO order
    mtx_lock(&q_mtx);
    sync = thread_syncs[sleep_head_idx];

    // Creating a queue entry and adding it to the queue
    e = malloc(sizeof(struct entry));
    if (e == NULL) {
        fprintf(stderr, "Error allocating memory for queue entry");
        experienced_error = 1;
        return;
    }
    e->path = path;
    e->next = NULL;
    if (q->head == NULL) {
        q->head = e;
        q->tail = e;
    } else {
        q->tail->next = e;
        q->tail = e;
    }
    
    // If there exists a thread that went to sleep, wake it up (we just added a new path to the queue)
    if (sleep_head_idx != sleep_tail_idx) {
        cnd_signal(&sync);
        sleep_head_idx = (sleep_head_idx + 1) % num_threads;
    }

    mtx_unlock(&q_mtx);
}

void cleanup() {
    atomic_int i;
    for (i = 0; i < num_threads; i++) {
        cnd_signal(&thread_syncs[i]);
    }
    mtx_unlock(&q_mtx);
    thrd_exit(0);
}


char * pop(struct queue * q) {
    char * path;
    struct entry * e;
    cnd_t sync;
    atomic_int new_sleep_tail_idx;
    // First we make sure that if there are no paths in the queue, consecutive pops will sleep on different cnd_t objects
    printf("pop\n");
    mtx_lock(&q_mtx);
    sync = thread_syncs[sleep_tail_idx];
    
    while (q->head == NULL) {
        printf("Head is null\n");
        
        num_sleeping++;
        // If all threads should be sleeping, we've finished searching files and should exit the program (during cleanup the thread will exit, so num_sleeping won't decrease)
        if (num_sleeping == num_threads) {
            cleanup();
        }

        // Dividing this operation to 2 lines, to make sure that the value in sleep_tail_idx is in [0, num_threads - 1]
        new_sleep_tail_idx = (sleep_tail_idx + 1) % num_threads;
        sleep_tail_idx = new_sleep_tail_idx;
        
        cnd_wait(&sync, &q_mtx);
        num_sleeping--;
    }

    // Popping the head of the queue
    e = q->head;
    q->head = e->next;
    if (q->head == NULL) {
        q->tail = NULL;
    }
    path = e->path;
    printf("Got path %s\n", path);
    free(e);

    mtx_unlock(&q_mtx);
    return path;
}

/////////////////////////////
// Functions for the flow of each thread

void iterate_dir(struct queue * q, char* path) {
    DIR * dir;
    char * new_path;
    struct dirent * entry;
    dir = opendir(path);
    // If we couldn't open the directory
    if (dir == NULL) {
        fprintf(stderr, "%s\n", strerror(errno));
        experienced_error = 1;
        return;
    }

    // Iterating over the directory
    while ((entry = readdir(dir)) != NULL) {
        // Ignoring the current and parent directories
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        // Creating a new path to push to the queue
        new_path = malloc(strlen(path) + strlen(entry->d_name) + 2);
        if (new_path != NULL) {
            strcpy(new_path, path);
            strcat(new_path, "/");
            strcat(new_path, entry->d_name);
            push(q, new_path);
        } else {
            fprintf(stderr, "Error: malloc failed.\n");
            experienced_error = 1;
        }
    }
    free(path);
    closedir(dir);
}

void thread_scan(struct queue * q) {
    char * path;
    struct stat st;
    while(1) {
        path = pop(q);
        if (stat(path, &st) == -1) {
            fprintf(stderr, "%s\n", strerror(errno));
        }

        // If the path is to a directory:
        if (S_ISDIR(st.st_mode)) {
            // Checking if we can access the directory
            if (access(path, R_OK | X_OK) == -1) {
                // We can't access the directory, so we print it
                if (errno == EACCES) {
                    printf("Directory %s: Permission denied.\n", path);
                } else {
                    printf("Directory '%s' error ", path);
                    fprintf(stderr, "%s\n", strerror(errno));
                    experienced_error = 1;
                }
            }

            // We can access the directory, so we scan it
            iterate_dir(q, path);
        }

        // If the path is to a file, and the file name contains the search term, we print it
        else {
            if (strstr(path, search_term) != NULL) {
                printf("%s\n", path);
                recognized_files++;
            }
        }
    }
}


void halted_thread_scan(struct queue * q) {
    mtx_lock(&opening_shot_mtx);

    // Signal the main thread it can continue to create the next thread
    cnd_signal(&thread_ready_cnd);

    // Wait for the main thread to signal that it's ready to start scanning
    cnd_wait(&opening_shot_cnd, &opening_shot_mtx);
    mtx_unlock(&opening_shot_mtx);

    thread_scan(q);
}


///////////////////////////
// Main thread

void exit_print(int num_found_files) {
    printf("Done searching, found %d files\n", num_found_files);
}

int main(int argc, char** argv) {
    int i;
    struct queue * q;
    thrd_t * threads;
    char * new_path;
    if (argc < 4) {
        fprintf(stderr, "Error: Not enough arguments.");
    }
    // search_root = argv[1];
    search_term = argv[2];
    num_threads = atoi(argv[3]);

    // Check if number of threads is valid
    if (num_threads <= 0) {
        fprintf(stderr, "Error: Invalid number of threads.");
        exit(1);
    }

    // Initialize sync variables
    mtx_init(&q_mtx, mtx_plain);
    mtx_init(&opening_shot_mtx, mtx_recursive);
    cnd_init(&opening_shot_cnd);
    cnd_init(&thread_ready_cnd);

    experienced_error = 0;
    
    thread_syncs = malloc(sizeof(cnd_t) * num_threads);
    if (thread_syncs == NULL) {
        fprintf(stderr, "Error: malloc failed.\n");
        exit(1);
    }
    
    threads = malloc(sizeof(thrd_t) * num_threads);
    for (i = 0; i < num_threads; i++) {
        cnd_init(&thread_syncs[i]);
    }

    // Initialize queue
    q = malloc(sizeof(struct queue));
    if (q == NULL) {
        fprintf(stderr, "Error: malloc failed.\n");
        exit(1);
    }

    init(q);
    new_path = malloc(strlen(argv[1]) + 1);
    if (new_path != NULL) {
        strcpy(new_path, argv[1]);
        push(q, new_path);
    } else {
        exit(1);
    }
    
    for (i = 0; i < num_threads; i++) {
        // Create a thread
        mtx_lock(&opening_shot_mtx);
        thrd_create(&threads[i], (thrd_start_t)halted_thread_scan, q);
        
        // Wait for the thread to signal it's ready
        cnd_wait(&thread_ready_cnd, &opening_shot_mtx);
        mtx_unlock(&opening_shot_mtx);
    }

    // Signal all threads to start scanning
    mtx_lock(&opening_shot_mtx);
    cnd_broadcast(&opening_shot_cnd);
    mtx_unlock(&opening_shot_mtx);

    // Destroy setup sync variables
    mtx_destroy(&opening_shot_mtx);
    cnd_destroy(&opening_shot_cnd);
    cnd_destroy(&thread_ready_cnd);

    for (i = 0; i < num_threads; i++) {
        thrd_join(threads[i], NULL);
        cnd_destroy(&thread_syncs[i]);
    }
    free(thread_syncs);
    free(threads);
    free(q);
    exit_print(recognized_files);
    
    return experienced_error;
}