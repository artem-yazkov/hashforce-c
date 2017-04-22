#ifndef __HASHFORCE_H__
#define __HASHFORCE_H__

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

typedef struct hash {
    union {
        uint8_t  digest[16];
        uint64_t group[2];
    };
} hash_t;

typedef struct args_chrange {
    uint8_t from;
    uint8_t to;
    uint8_t count;
} args_chrange_t;

typedef struct args_range {
    uint16_t        from;
    uint16_t        to;
    uint64_t        capacity;
    uint16_t        chranges_sum;
    uint16_t        chranges_cnt;
    args_chrange_t *chranges;
} args_range_t;

typedef struct args {
    args_range_t range;
    hash_t       hash;
    uint64_t     offset;
    uint32_t     blocklength;
    uint32_t     blocktime;
    uint16_t     cores;
} args_t;


typedef struct word {
    uint8_t  *data;
    uint16_t *iranges;
    uint16_t *ioffsets;
    uint16_t  len;
    uint16_t  size;
} word_t;

typedef struct worker {
    int       idx;
    pthread_t thread;
    uint64_t  cycles;
    word_t    word;
} worker_t;

typedef struct state {
    pthread_mutex_t mutex;
    pthread_cond_t  cond_begin;
    pthread_cond_t  cond_end;
    hash_t          hash;
    args_range_t   *range;
    uint64_t        blocknum;
    uint64_t        offset;
    size_t          workers_wait;
    size_t          workers_cnt;
    worker_t       *workers;
    struct state_answer {
        bool    found;
        int     worker_idx;
    } answer;
} state_t;




#endif
