#include "buddy.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

#define PAGE_SIZE 4096UL
#define MIN_RANK 1
#define MAX_RANK 16

static uintptr_t pool_base;
static size_t pool_pages;
static size_t pool_bytes;
static unsigned char *allocation_start;
static unsigned char *allocated_page_rank;
static uint64_t *free_map[MAX_RANK + 1];
static size_t free_map_words[MAX_RANK + 1];
static size_t free_block_count[MAX_RANK + 1];

static size_t pages_for_rank(int rank)
{
    return (size_t)1 << (rank - 1);
}

static size_t blocks_for_rank(size_t pages, int rank)
{
    size_t block_pages = pages_for_rank(rank);

    return (pages + block_pages - 1) / block_pages;
}

static void clear_state(void)
{
    int rank;

    free(allocation_start);
    free(allocated_page_rank);
    allocation_start = NULL;
    allocated_page_rank = NULL;

    for (rank = MIN_RANK; rank <= MAX_RANK; ++rank) {
        free(free_map[rank]);
        free_map[rank] = NULL;
        free_map_words[rank] = 0;
        free_block_count[rank] = 0;
    }

    pool_base = 0;
    pool_pages = 0;
    pool_bytes = 0;
}

static int block_is_free(int rank, size_t page_index)
{
    size_t block_index = page_index / pages_for_rank(rank);

    return (free_map[rank][block_index / 64] >> (block_index % 64)) & 1U;
}

static void add_free_block(int rank, size_t page_index)
{
    size_t block_index = page_index / pages_for_rank(rank);
    uint64_t bit = UINT64_C(1) << (block_index % 64);

    free_map[rank][block_index / 64] |= bit;
    ++free_block_count[rank];
}

static void remove_free_block(int rank, size_t page_index)
{
    size_t block_index = page_index / pages_for_rank(rank);
    uint64_t bit = UINT64_C(1) << (block_index % 64);

    free_map[rank][block_index / 64] &= ~bit;
    --free_block_count[rank];
}

static int first_free_block(int rank, size_t *page_index)
{
    size_t word_index;
    size_t block_count = blocks_for_rank(pool_pages, rank);

    for (word_index = 0; word_index < free_map_words[rank]; ++word_index) {
        uint64_t word = free_map[rank][word_index];
        size_t block_index;

        if (word == 0)
            continue;
        block_index = word_index * 64 + (size_t)__builtin_ctzll(word);
        if (block_index < block_count) {
            *page_index = block_index * pages_for_rank(rank);
            return 1;
        }
    }
    return 0;
}

static int page_index_from_address(void *p, size_t *page_index)
{
    uintptr_t address;
    size_t offset;

    if (pool_pages == 0 || p == NULL)
        return 0;

    address = (uintptr_t)p;
    if (address < pool_base)
        return 0;
    offset = (size_t)(address - pool_base);
    if (offset >= pool_bytes || offset % PAGE_SIZE != 0)
        return 0;

    *page_index = offset / PAGE_SIZE;
    return 1;
}

int init_page(void *p, int pgcount)
{
    uintptr_t base;
    size_t pages;
    size_t bytes;
    unsigned char *new_allocation_start;
    unsigned char *new_allocated_page_rank;
    uint64_t *new_free_map[MAX_RANK + 1] = { NULL };
    size_t new_free_map_words[MAX_RANK + 1] = { 0 };
    size_t new_free_block_count[MAX_RANK + 1] = { 0 };
    size_t page_index;
    int rank;

    if (p == NULL || pgcount <= 0)
        return -EINVAL;

    base = (uintptr_t)p;
    pages = (size_t)pgcount;
    if (pages > SIZE_MAX / PAGE_SIZE)
        return -EINVAL;
    bytes = pages * PAGE_SIZE;
    if (bytes > UINTPTR_MAX - base)
        return -EINVAL;

    new_allocation_start = calloc(pages, sizeof(*new_allocation_start));
    new_allocated_page_rank = calloc(pages, sizeof(*new_allocated_page_rank));
    if (new_allocation_start == NULL || new_allocated_page_rank == NULL) {
        free(new_allocation_start);
        free(new_allocated_page_rank);
        return -ENOSPC;
    }

    for (rank = MIN_RANK; rank <= MAX_RANK; ++rank) {
        size_t block_count = blocks_for_rank(pages, rank);
        size_t words = (block_count + 63) / 64;

        new_free_map[rank] = calloc(words, sizeof(*new_free_map[rank]));
        if (new_free_map[rank] == NULL) {
            int cleanup_rank;

            for (cleanup_rank = MIN_RANK; cleanup_rank <= MAX_RANK; ++cleanup_rank)
                free(new_free_map[cleanup_rank]);
            free(new_allocation_start);
            free(new_allocated_page_rank);
            return -ENOSPC;
        }
        new_free_map_words[rank] = words;
    }

    /* Build a maximal aligned decomposition of a possibly non-power-of-two pool. */
    page_index = 0;
    while (page_index < pages) {
        for (rank = MAX_RANK; rank >= MIN_RANK; --rank) {
            size_t block_pages = pages_for_rank(rank);

            if (block_pages <= pages - page_index && page_index % block_pages == 0) {
                size_t block_index = page_index / block_pages;

                new_free_map[rank][block_index / 64] |= UINT64_C(1) << (block_index % 64);
                ++new_free_block_count[rank];
                page_index += block_pages;
                break;
            }
        }
    }

    clear_state();
    pool_base = base;
    pool_pages = pages;
    pool_bytes = bytes;
    allocation_start = new_allocation_start;
    allocated_page_rank = new_allocated_page_rank;
    for (rank = MIN_RANK; rank <= MAX_RANK; ++rank) {
        free_map[rank] = new_free_map[rank];
        free_map_words[rank] = new_free_map_words[rank];
        free_block_count[rank] = new_free_block_count[rank];
    }

    return OK;
}

void *alloc_pages(int rank)
{
    size_t page_index = 0;
    size_t best_page_index = 0;
    int source_rank = 0;
    int candidate_rank;
    size_t block_pages;

    if (rank < MIN_RANK || rank > MAX_RANK)
        return ERR_PTR(-EINVAL);
    if (pool_pages == 0)
        return ERR_PTR(-ENOSPC);

    /* Select the lowest-address block among all usable ranks. */
    for (candidate_rank = rank; candidate_rank <= MAX_RANK; ++candidate_rank) {
        if (first_free_block(candidate_rank, &page_index) &&
            (source_rank == 0 || page_index < best_page_index)) {
            source_rank = candidate_rank;
            best_page_index = page_index;
        }
    }
    if (source_rank == 0)
        return ERR_PTR(-ENOSPC);

    page_index = best_page_index;
    remove_free_block(source_rank, page_index);
    while (source_rank > rank) {
        --source_rank;
        block_pages = pages_for_rank(source_rank);
        add_free_block(source_rank, page_index + block_pages);
    }

    block_pages = pages_for_rank(rank);
    allocation_start[page_index] = (unsigned char)rank;
    for (size_t i = 0; i < block_pages; ++i)
        allocated_page_rank[page_index + i] = (unsigned char)rank;

    return (void *)(pool_base + page_index * PAGE_SIZE);
}

int return_pages(void *p)
{
    size_t page_index;
    size_t block_pages;
    int rank;

    if (!page_index_from_address(p, &page_index) || allocation_start[page_index] == 0)
        return -EINVAL;

    rank = allocation_start[page_index];
    block_pages = pages_for_rank(rank);
    allocation_start[page_index] = 0;
    for (size_t i = 0; i < block_pages; ++i)
        allocated_page_rank[page_index + i] = 0;

    while (rank < MAX_RANK) {
        size_t buddy_index = page_index ^ block_pages;

        if (buddy_index + block_pages > pool_pages || !block_is_free(rank, buddy_index))
            break;
        remove_free_block(rank, buddy_index);
        if (buddy_index < page_index)
            page_index = buddy_index;
        ++rank;
        block_pages *= 2;
    }
    add_free_block(rank, page_index);

    return OK;
}

int query_ranks(void *p)
{
    size_t page_index;
    int rank;

    if (!page_index_from_address(p, &page_index))
        return -EINVAL;
    if (allocated_page_rank[page_index] != 0)
        return allocated_page_rank[page_index];

    for (rank = MAX_RANK; rank >= MIN_RANK; --rank) {
        size_t block_pages = pages_for_rank(rank);
        size_t block_start = (page_index / block_pages) * block_pages;

        if (block_start + block_pages <= pool_pages && block_is_free(rank, block_start))
            return rank;
    }
    return -EINVAL;
}

int query_page_counts(int rank)
{
    if (rank < MIN_RANK || rank > MAX_RANK)
        return -EINVAL;
    return (int)free_block_count[rank];
}
