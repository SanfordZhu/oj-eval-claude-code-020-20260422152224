#include "buddy.h"
#include <stdlib.h>
#include <string.h>

#define NULL ((void *)0)
#define PAGE_SIZE 4096
#define MAXR 16

static void *base = NULL;
static int total_pages = 0;
static int max_rank = 0; // largest rank available for the managed region

static unsigned char *alloc_rank_head = NULL; // alloc rank at head index, 0 if not allocated
static unsigned char *free_bitmap[MAXR + 1] = {0}; // free heads per rank
static int free_count[MAXR + 1] = {0};

static inline int size_in_pages(int rank) { return 1 << (rank - 1); }

static inline int in_range(void *p) {
    if (!base || total_pages <= 0) return 0;
    long off = (long)((char *)p - (char *)base);
    if (off < 0) return 0;
    if (off % PAGE_SIZE != 0) return 0;
    if (off >= (long)total_pages * PAGE_SIZE) return 0;
    return 1;
}

static inline int page_index(void *p) {
    long off = (long)((char *)p - (char *)base);
    return (int)(off / PAGE_SIZE);
}

static int find_free_head(int rank) {
    int step = size_in_pages(rank);
    for (int i = 0; i < total_pages; i += step) {
        if (free_bitmap[rank][i]) return i;
    }
    return -1;
}

int init_page(void *p, int pgcount){
    // free previous state if re-initialized
    if (alloc_rank_head) { free(alloc_rank_head); alloc_rank_head = NULL; }
    for (int r = 1; r <= MAXR; ++r) {
        if (free_bitmap[r]) { free(free_bitmap[r]); free_bitmap[r] = NULL; }
        free_count[r] = 0;
    }

    base = p;
    total_pages = pgcount;
    if (!base || total_pages <= 0) return -EINVAL;

    alloc_rank_head = (unsigned char *)malloc((size_t)total_pages);
    if (!alloc_rank_head) return -ENOSPC;
    memset(alloc_rank_head, 0, (size_t)total_pages);

    for (int r = 1; r <= MAXR; ++r) {
        free_bitmap[r] = (unsigned char *)malloc((size_t)total_pages);
        if (!free_bitmap[r]) return -ENOSPC;
        memset(free_bitmap[r], 0, (size_t)total_pages);
        free_count[r] = 0;
    }

    // compute max_rank such that a single block fits the region best
    max_rank = 1;
    for (int r = 1; r <= MAXR; ++r) {
        int s = size_in_pages(r);
        if (s <= total_pages && (total_pages % s) == 0) max_rank = r;
    }

    // start with one big free block at head 0 of max_rank
    free_bitmap[max_rank][0] = 1;
    free_count[max_rank] = total_pages / size_in_pages(max_rank);

    return OK;
}

void *alloc_pages(int rank){
    if (rank < 1 || rank > MAXR) return ERR_PTR(-EINVAL);
    if (!base || total_pages <= 0) return ERR_PTR(-ENOSPC);

    int curr = rank;
    while (curr <= max_rank && free_count[curr] == 0) curr++;
    if (curr > max_rank) return ERR_PTR(-ENOSPC);

    // take a block at rank curr
    int head = find_free_head(curr);
    if (head < 0) return ERR_PTR(-ENOSPC);
    free_bitmap[curr][head] = 0;
    free_count[curr]--;

    // split down to desired rank, always keeping left child for allocation
    while (curr > rank) {
        int child_size = size_in_pages(curr - 1);
        int right_head = head + child_size;
        free_bitmap[curr - 1][right_head] = 1;
        free_count[curr - 1]++;
        curr--;
    }

    alloc_rank_head[head] = (unsigned char)rank;
    return (void *)((char *)base + (long)head * PAGE_SIZE);
}

int return_pages(void *p){
    if (!p) return -EINVAL;
    if (!in_range(p)) return -EINVAL;
    int idx = page_index(p);
    if (idx < 0 || idx >= total_pages) return -EINVAL;

    int rank = alloc_rank_head[idx];
    if (rank < 1 || rank > MAXR) return -EINVAL; // not an allocated head

    alloc_rank_head[idx] = 0;

    // free this block and attempt to merge upwards
    free_bitmap[rank][idx] = 1;
    free_count[rank]++;

    while (rank < max_rank) {
        int s = size_in_pages(rank);
        int buddy = idx ^ s;
        if (buddy < 0 || buddy >= total_pages) break;
        if (!free_bitmap[rank][buddy]) break;
        // remove both children
        free_bitmap[rank][idx] = 0;
        free_bitmap[rank][buddy] = 0;
        free_count[rank] -= 2;
        // new parent head
        idx = (idx & ~s);
        rank++;
        free_bitmap[rank][idx] = 1;
        free_count[rank]++;
    }

    return OK;
}

int query_ranks(void *p){
    if (!p) return -EINVAL;
    if (!in_range(p)) return -EINVAL;
    int idx = page_index(p);
    if (idx < 0 || idx >= total_pages) return -EINVAL;

    int ar = alloc_rank_head[idx];
    if (ar >= 1 && ar <= MAXR) return ar;

    // find the largest free block that covers this index
    for (int r = max_rank; r >= 1; --r) {
        int s = size_in_pages(r);
        int head = (idx / s) * s;
        if (head >= 0 && head < total_pages) {
            if (free_bitmap[r][head]) return r;
        }
    }

    return -EINVAL;
}

int query_page_counts(int rank){
    if (rank < 1 || rank > MAXR) return -EINVAL;
    return free_count[rank];
}
