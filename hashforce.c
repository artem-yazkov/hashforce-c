#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include <openssl/md5.h>
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
            hash_t hash;
            MD5_CTX context;
            MD5_Init(&context);
            MD5_Update(&context, worker->word.data, worker->word.len);
            MD5_Final(hash.digest, &context);

            if ((hash.group[0] == state.hash.group[0]) &&
                (hash.group[1] == state.hash.group[1]))
            {
                pthread_mutex_lock(&state.mutex);
                state.workers_wait++;
                state.answer.found      = true;
                state.answer.worker_idx = worker->idx;
                pthread_cond_signal(&state.cond_end);
                pthread_mutex_unlock(&state.mutex);
                return NULL;
            }
            word_increment(&worker->word, state.range, worker->word.size - 1);
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

    uint64_t cycles = opts->blocklength;
    uint32_t remain = 0;

    if ((opts->range.capacity - state.offset) < (opts->cores * opts->blocklength)) {
        cycles = (opts->range.capacity - state.offset) / opts->cores;
        remain = (opts->range.capacity - state.offset) % opts->cores;
    }
    for (int i = 0; i < state.workers_cnt; i++) {
        word_set(&state.workers[i].word, &opts->range, state.offset);
        state.workers[i].cycles = cycles + ((i < remain) ? 1 : 0);
        state.offset += state.workers[i].cycles;
    }
}

int workers_manage(args_t *opts)
{
    state.workers_cnt = opts->cores;
    state.workers = calloc(state.workers_cnt, sizeof(state.workers[0]));
    state.offset  = opts->offset;
    state.hash    = opts->hash;
    state.range   = &opts->range;

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
        while ((state.workers_wait < state.workers_cnt) && !state.answer.found) {
            pthread_cond_wait(&state.cond_end, &state.mutex);
        }
        if (state.answer.found) {
            /* answer found: stop all threads & exit */
            fprintf(stdout, "\n catched! source word are: ");
            word_print(&state.workers[state.answer.worker_idx].word);
            fprintf(stdout, "\n");

            for (int i = 0; i < state.workers_cnt; i++) {
                if (i != state.answer.worker_idx) {
                    pthread_cancel(state.workers[i].thread);
                }
            }
            pthread_mutex_unlock(&state.mutex);
            break;
        }

        if (state.offset == opts->range.capacity) {
            /* checkout all range - no luck */
            fprintf(stdout, "\n %llu hashes was checked; no luck :(\n", state.offset);
            for (int i = 0; i < state.workers_cnt; i++) {
                pthread_cancel(state.workers[i].thread);
            }
            pthread_mutex_unlock(&state.mutex);
            break;
        }

        fprintf(stdout, "+ %u * %u hashes was checked\n", opts->cores, opts->blocklength);
        state.blocknum++;
        workers_block_begin(opts);
        state.workers_wait = 0;
        pthread_cond_broadcast(&state.cond_begin);
        pthread_mutex_unlock(&state.mutex);
    }

    pthread_cond_destroy(&state.cond_begin);
    pthread_cond_destroy(&state.cond_end);
    pthread_mutex_destroy(&state.mutex);

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

int args_process_range(char *srange, args_range_t *range)
{
    uint16_t from, to;
    int   roffset;
    char *chrtok;

    if (sscanf(srange, " %hu %hu %n", &from, &to, &roffset) != 2) {
        fprintf(stderr, "unsupported characters range format\n");
        return -1;
    }
    range->from = (from < to) ? from : to;
    range->to   = (from < to) ? to : from;

    chrtok = strtok(&srange[roffset], ":");
    while (chrtok) {
        range->chranges_cnt++;
        range->chranges = realloc(range->chranges, range->chranges_cnt * sizeof(range->chranges[0]));
        if (sscanf(chrtok, "%hu-%hu", &from, &to) != 2) {
            fprintf(stderr, "unsupported characters range format\n");
            return -1;
        }
        range->chranges[range->chranges_cnt - 1].from = (from < to) ? from : to;
        range->chranges[range->chranges_cnt - 1].to   = (from < to) ? to : from;
        if (range->chranges[range->chranges_cnt - 1].to >= 128) {
            fprintf(stderr, "unsupported characters range\n");
            return -1;
        }
        chrtok = strtok(NULL, ":");
    }

    /* sort & merge */
    qsort(range->chranges, range->chranges_cnt, sizeof(range->chranges[0]), args_chranges_asc);

    int mrange = 0;
    for (int irange = 1;
             irange < range->chranges_cnt; irange++) {
        if (range->chranges[mrange].to >= range->chranges[irange].from) {
            if (range->chranges[mrange].to < range->chranges[irange].to) {
                range->chranges[mrange].to = range->chranges[irange].to;
            }
        } else {
            mrange++;
        }
    }
    range->chranges_cnt = mrange + 1;
    range->chranges = realloc(range->chranges, range->chranges_cnt * sizeof(range->chranges[0]));
    for (int irange = 0; irange < range->chranges_cnt; irange++) {
        range->chranges[irange].count = range->chranges[irange].to - range->chranges[irange].from + 1;
        range->chranges_sum += range->chranges[irange].count;
    }

    /* calculate range capacity */
    range->capacity = 0;
    uint64_t rank  = 1;
    for (int i = 1; i <= range->to; i++) {
        if ((rank * range->chranges_sum) < rank) {
            fprintf(stderr, "range capacity overflow (> UINT64_MAX)\n");
            return -1;
        }
        rank *= range->chranges_sum;
        if (i < range->from) {
             continue;
        }
        if ((range->capacity + rank) < range->capacity) {
            fprintf(stderr, "range capacity overflow (> UINT64_MAX)\n");
            return -1;
        }
        range->capacity += rank;
    }
    return 0;
}

int args_process_hash(const char *shash, hash_t *hash)
{
    if (strlen(shash) != 32) {
        return -1;
    }
    for (int ichar = 0; ichar < 16; ichar++) {
        sscanf(&shash[ichar << 1], "%02x", &hash->digest[ichar]);
    }
    return 0;
}

int args_process(int argc, char *argv[], args_t *opts)
{
    const struct option longopts[] = {
        {"range",        required_argument, NULL, 'r'},
        {"hash",         required_argument, NULL, 'H'},
        {"cores",        required_argument, NULL, 'c'},
        {"block-length", required_argument, NULL, 'l'},
        {"block-time",   required_argument, NULL, 't'},
        {"offset",       required_argument, NULL, 'o'},
        {"help",         no_argument,       NULL, 'h'},
        {NULL,           0,                 NULL,  0 }
    };

    int rc_range = -1;
    int rc_hash  = -1;
    int opt;
    int longind = -1;

    while ((opt = getopt_long(argc, argv, "", longopts, &longind)) != -1) {
        switch(opt) {
        case 'r':
            rc_range = args_process_range(optarg, &opts->range);
            break;
        case 'H':
            rc_hash = args_process_hash(optarg, &opts->hash);
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
    if ((rc_range != 0) || !opts->range.chranges_cnt) {
        fprintf(stdout, "'--range' must be set in right order\n");
        return -1;
    }
    if (rc_hash != 0) {
        fprintf(stdout, "'--hash' must be set in right order\n");
        return -1;
    }
    if (opts->offset && (opts->offset > opts->range.capacity)) {
        fprintf(stdout, "'--offset' can't exceed range capacity\n");
        return -1;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    static args_t opts;

    if (args_process(argc, argv, &opts) != 0) {
        args_show_help();
        return -1;
    }
    fprintf(stdout, "cores: %u\n", opts.cores);
    workers_manage(&opts);
    fprintf(stdout, "bye!\n");
    return 0;

#if 0

    for (int irange = 0; irange < opts.range.chranges_cnt; irange++) {
        args_chrange_t *range = &opts.range.chranges[irange];
        fprintf(stdout, "%u-%u, count: %u\n", range->from, range->to, range->count);
    }
    fprintf(stdout, "from: %u; to: %u, sum: %u\n", opts.range.from, opts.range.to, opts.range.chranges_sum);
#endif


}

// ./hashforce --block-length 400 --cores 4 --range "4 4 48-57:65-70" --hash "b7e9a194e827ce783db9cfcdd72cdf91"
//  gcc ./hashforce.c -lpthread -lcrypto -o ./hashforce
