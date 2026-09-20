#pragma once
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
typedef struct {
    size_t total_free_bytes, minimum_free_bytes, largest_free_block;
} multi_heap_info_t;
void heap_caps_get_info(multi_heap_info_t *info, uint32_t caps);
