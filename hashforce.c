#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "hashforce.h"

static state_t state;

int word_init(word_t *word, args_range_t *range)
{
    word->size = range->to;
    word->len  = range->from;
    word->data     = malloc(word->size * sizeof(word->data[0]));
    word->iranges  = malloc(word->size * sizeof(word->iranges[0]));
    word->ioffsets = malloc(word->size * sizeof(word->ioffsets[0]));
    for (int i = 0; i < word->size; i++) {
        word->data[i] = range->chranges[0].from;
        word->iranges[i] = 0;
        word->ioffsets[i] = 0;
    }
    return 0;
}

int word_set(word_t *word, args_range_t *range, uint64_t offset)
{
    for (int widx = word->size - 1; offset > 0; widx--) {
        if (widx < 0) {
            return -1;
        }
        if (widx < word->size - word->len) {
            word->len++;
        }

        uint16_t choff = offset % range->chranges_sum;  /* char value offset */
        int      chidx = 0;                             /* char range index  */
        while ((chidx < range->chranges_cnt) &&
               (choff >= range->chranges[chidx].count)) {
            choff -= range->chranges[chidx].count;
            chidx++;
        }
        if (chidx == range->chranges_cnt) {
            return -1;
        }
        word->data[widx]     = range->chranges[chidx].from + choff;
        word->iranges[widx]  = chidx;
        word->ioffsets[widx] = choff;
        offset /= range->chranges_sum;
    }
    return 0;
}

int word_increment(word_t *word, args_range_t *range, int widx)
{
    if ((widx > word->size - 1) || (widx < 0)) {
        return -1;
    }
    if (widx < word->size - word->len) {
        word->len++;
    }

    int rcode;

    if (word->ioffsets[widx] < (range->chranges[word->iranges[widx]].count - 1)) {
        word->ioffsets[widx]++;
        rcode = 0;
    } else if (word->iranges[widx] < (range->chranges_cnt - 1)) {
        word->iranges[widx]++;
        word->ioffsets[widx] = 0;
        rcode = 0;
    } else {
        word->iranges[widx] = 0;
        word->ioffsets[widx] = 0;
        rcode = word_increment(word, range, widx - 1);
    }
    word->data[widx] = range->chranges[word->iranges[widx]].from + word->ioffsets[widx];
    return rcode;
}

int word_print(word_t *word)
{
    for (int i = 0; i < word->size; i++) {
        if (i < word->size - word->len)
            printf(" ");
        else
            printf("%c", word->data[i]);
    }
    return 0;
}

void *worker_thread(void *arg)
{
    worker_t            *worker = arg;

    while (1) {
        for (int icycle = 0; icycle < worker->cycles; icycle++) {
            usleep(2500);
            if (0) {
                pthread_mutex_lock(&state.mutex);
                state.workers_wait++;
                state.answer.found      = true;
                state.answer.worker_idx = worker->idx;
                pthread_cond_signal(&state.cond_end);
                pthread_mutex_unlock(&state.mutex);
            }
        }
        pthread_mutex_lock(&state.mutex);
        state.workers_wait++;
        pthread_cond_signal(&state.cond_end);
        pthread_cond_wait  (&state.cond_begin, &state.mutex);
        pthread_mutex_unlock(&state.mutex);
    }

    return NULL;
}

void workers_block_begin(args_t *opts)
{
    fprintf(stdout, "block № %llu: start from %llu ... ", state.blocknum, state.offset);
    fflush(stdout);

    for (int i = 0; i < state.workers_cnt; i++) {
        word_set(&state.workers[i].word, &opts->range, state.offset);
        state.workers[i].cycles = opts->blocklength;
        state.offset += opts->blocklength;
    }
}

int workers_manage(args_t *opts)
{
    state.workers_cnt = opts->cores;
    state.workers = calloc(state.workers_cnt, sizeof(state.workers[0]));
    state.offset  = opts->offset;

    pthread_mutex_init(&state.mutex, NULL);
    pthread_cond_init(&state.cond_begin, NULL);
    pthread_cond_init(&state.cond_end, NULL);

    workers_block_begin(opts);

    for (int i = 0; i < state.workers_cnt; i++) {
        state.workers[i].idx = i;
        pthread_create(&state.workers[i].thread, NULL, worker_thread, &state.workers[i]);
        word_init(&state.workers[i].word, &opts->range);
    }

    while (1) {
        pthread_mutex_lock(&state.mutex);
        while (state.workers_wait < state.workers_cnt) {
            pthread_cond_wait(&state.cond_end, &state.mutex);
        }
        fprintf(stdout, "+ %u * %u hashes checked\n", opts->cores, opts->blocklength);
        state.workers_wait = 0;
        state.blocknum++;
        workers_block_begin(opts);
        pthread_cond_broadcast(&state.cond_begin);
        pthread_mutex_unlock(&state.mutex);
    }
    return 0;
}

void args_show_help(void)
{

}

int args_chranges_asc(const void *a, const void *b)
{
    const args_chrange_t *range_a = a;
    const args_chrange_t *range_b = b;

    if (range_a->from < range_b->from)
        return -1;
    else if (range_a->from > range_b->from)
        return 1;
    else
        return 0;
}

int args_process_range(char *range, args_range_t *opts)
{
    uint16_t from, to;
    int   roffset;
    char *chrtok;

    if (sscanf(range, " %hu %hu %n", &from, &to, &roffset) != 2) {
        fprintf(stderr, "unsupported characters range format\n");
        return -1;
    }
    opts->from = (from < to) ? from : to;
    opts->to   = (from < to) ? to : from;

    chrtok = strtok(&range[roffset], ":");
    while (chrtok) {
        opts->chranges_cnt++;
        opts->chranges = realloc(opts->chranges, opts->chranges_cnt * sizeof(opts->chranges[0]));
        if (sscanf(chrtok, "%hu-%hu", &from, &to) != 2) {
            fprintf(stderr, "unsupported characters range format\n");
            return -1;
        }
        opts->chranges[opts->chranges_cnt - 1].from = (from < to) ? from : to;
        opts->chranges[opts->chranges_cnt - 1].to   = (from < to) ? to : from;
        if (opts->chranges[opts->chranges_cnt - 1].to >= 128) {
            fprintf(stderr, "unsupported characters range\n");
            return -1;
        }
        chrtok = strtok(NULL, ":");
    }

    /* sort & merge */
    qsort(opts->chranges, opts->chranges_cnt, sizeof(opts->chranges[0]), args_chranges_asc);

    int mrange = 0;
    for (int irange = 1;
             irange < opts->chranges_cnt; irange++) {
        if (opts->chranges[mrange].to >= opts->chranges[irange].from) {
            if (opts->chranges[mrange].to < opts->chranges[irange].to) {
                opts->chranges[mrange].to = opts->chranges[irange].to;
            }
        } else {
            mrange++;
        }
    }
    opts->chranges_cnt = mrange + 1;
    opts->chranges = realloc(opts->chranges, opts->chranges_cnt * sizeof(opts->chranges[0]));
    for (int irange = 0; irange < opts->chranges_cnt; irange++) {
        opts->chranges[irange].count = opts->chranges[irange].to - opts->chranges[irange].from + 1;
        opts->chranges_sum += opts->chranges[irange].count;
    }

    /* calculate range capacity */
    opts->capacity = 1;
    for (int i = 0; i < (opts->to - opts->from) + 1; i++) {
        uint64_t capacity = opts->capacity;
        opts->capacity *= opts->chranges_sum;
        if (opts->capacity < capacity) {
            fprintf(stderr, "range capacity overflow (> UINT64_MAX)\n");
            return -1;
        }
    }
    return 0;
}

int args_process(int argc, char *argv[], args_t *opts)
{
    const struct option longopts[] = {
        {"range",        required_argument, NULL, 'r'},
        {"cores",        required_argument, NULL, 'c'},
        {"block-length", required_argument, NULL, 'l'},
        {"block-time",   required_argument, NULL, 't'},
        {"offset",       required_argument, NULL, 'o'},
        {"help",         no_argument,       NULL, 'h'},
        {NULL,           0,                 NULL,  0 }
    };

    int retcode = 0;
    int opt;
    int longind = -1;

    while ((opt = getopt_long(argc, argv, "", longopts, &longind)) != -1) {
        switch(opt) {
        case 'r':
            retcode = args_process_range(optarg, &opts->range);
            break;
        case 'c':
            opts->cores = atoi(optarg);
            break;
        case 'l':
            opts->blocklength = atoi(optarg);
            break;
        case 't':
            opts->blocktime = atoi(optarg);
            break;
        case 'o':
            opts->offset = strtoull(optarg, NULL, 10);
            break;
        case 'h':
            args_show_help();
            return 1;
        }
    }

    if (!opts->blocklength && !opts->blocktime) {
        fprintf(stdout, "'--block-length' or '--block-time' must be set as positive number\n");
        return -1;
    }
    if (opts->blocklength && opts->blocktime) {
        fprintf(stdout, "'--block-length' and '--block-time' can't be set simultaneously\n");
        return -1;
    }
    if (!opts->cores) {
        fprintf(stdout, "'--cores' must be set as positive number\n");
        return -1;
    }
    if (!opts->range.chranges_cnt) {
        fprintf(stdout, "'--range' must be set\n");
        return -1;
    }
    if (opts->offset && (opts->offset > opts->range.capacity)) {
        fprintf(stdout, "'--offset' can't exceed range capacity\n");
        return -1;
    }

    return retcode;
}

int main(int argc, char *argv[])
{
    static args_t opts;

    if (args_process(argc, argv, &opts) != 0) {
        args_show_help();
        return -1;
    }
    workers_manage(&opts);

#if 0

    for (int irange = 0; irange < opts.range.chranges_cnt; irange++) {
        args_chrange_t *range = &opts.range.chranges[irange];
        fprintf(stdout, "%u-%u, count: %u\n", range->from, range->to, range->count);
    }
    fprintf(stdout, "from: %u; to: %u, sum: %u\n", opts.range.from, opts.range.to, opts.range.chranges_sum);
#endif

    return 0;
}

// ./hashforce --block-length 400 --cores 4 --range "3 10 48-57:65-70"
// gcc ./hashforce.c -lpthread -o ./hashforce
