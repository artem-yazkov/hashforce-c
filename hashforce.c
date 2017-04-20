#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

typedef struct worker {
    pthread_t thread;
    int       idx;
} worker_t;

typedef struct state {
    pthread_mutex_t  mutex;
    pthread_cond_t   cond_begin;
    pthread_cond_t   cond_end;
    size_t    workers_wait;
    size_t    workers_cnt;
    worker_t *workers;
} state_t;

static state_t state;

void *worker_thread(void *arg)
{
    worker_t            *worker = arg;
    struct drand48_data rdata;
    double              rtime;

    srand48_r(time(NULL) + worker->idx, &rdata);

    while (1) {
        drand48_r(&rdata, &rtime);

        fprintf(stdout, "worker %d: begin block (%f secs)\n", worker->idx, (rtime * 10));
        usleep((rtime * 10) * 1000000);
        fprintf(stdout, "worker %d: end block\n", worker->idx);

        pthread_mutex_lock(&state.mutex);
        state.workers_wait++;
        pthread_cond_signal(&state.cond_end);
        pthread_cond_wait  (&state.cond_begin, &state.mutex);
        pthread_mutex_unlock(&state.mutex);
    }

    return NULL;
}

int manage(void)
{
    fprintf(stdout, "manager: new block begin\n");

    for (int i = 0; i < state.workers_cnt; i++) {
        state.workers[i].idx = i;
        pthread_create(&state.workers[i].thread, NULL, worker_thread, &state.workers[i]);
    }

    while (1) {
        pthread_mutex_lock(&state.mutex);
        while (state.workers_wait < state.workers_cnt) {
            pthread_cond_wait(&state.cond_end, &state.mutex);
        }
        fprintf(stdout, "manager: all workers ended for block\n");
        state.workers_wait = 0;
        fprintf(stdout, "manager: new block begin\n");
        pthread_cond_broadcast(&state.cond_begin);
        pthread_mutex_unlock(&state.mutex);
    }
    return 0;
}

int main(int argc, char *argv)
{
    state.workers_cnt = 4;
    state.workers = calloc(state.workers_cnt, sizeof(state.workers[0]));

    pthread_mutex_init(&state.mutex, NULL);
    pthread_cond_init(&state.cond_begin, NULL);
    pthread_cond_init(&state.cond_end, NULL);
    srand(time(NULL));

    manage();

    return 0;
}

