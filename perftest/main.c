// Copyright (c) Pavel Kirienko

#include "o1heap.h"

#include "pico/stdlib.h"
#include "pico/stdio_uart.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"

#include <inttypes.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define HEAP_ARENA_SIZE_BYTES (64u * 1024u)
#define MEASURE_ITERATIONS    10000000u
#define MAX_LIVE_BLOCKS       256u

#define DEMCR_ADDR       (0xE000EDFCu)
#define DWT_CTRL_ADDR    (0xE0001000u)
#define DWT_CYCCNT_ADDR  (0xE0001004u)
#define DEMCR_TRCENA_BIT (1u << 24)
#define DWT_CTRL_CYCCNT  (1u << 0)

static alignas(O1HEAP_ALIGNMENT) uint8_t heap_arena[HEAP_ARENA_SIZE_BYTES];

static const uint16_t alloc_sizes[] = {
    16u, 32u, 64u, 128u, 256u, 512u, 1024u,  // More sizes for varied fragmentation
};

#define ALLOC_SIZES_COUNT (sizeof(alloc_sizes) / sizeof(alloc_sizes[0]))

static void* live_blocks[MAX_LIVE_BLOCKS];
static uint32_t live_block_size_idx[MAX_LIVE_BLOCKS];  // Track size index for each live block
static uint32_t free_order[MAX_LIVE_BLOCKS];           // Shuffled indices for random free order
static uint32_t rng_state = 0x9e3779b9u;

typedef struct
{
    uint32_t min;
    uint32_t max;
    uint64_t sum;
} Stats;

static inline void cycle_counter_init(void)
{
    *(volatile uint32_t*) DEMCR_ADDR |= DEMCR_TRCENA_BIT;
    *(volatile uint32_t*) DWT_CYCCNT_ADDR = 0u;
    *(volatile uint32_t*) DWT_CTRL_ADDR |= DWT_CTRL_CYCCNT;
}

static inline uint32_t cycle_counter_read(void)
{
    return *(volatile uint32_t*) DWT_CYCCNT_ADDR;
}

static inline void stats_init(Stats* s)
{
    s->min = UINT32_MAX;
    s->max = 0u;
    s->sum = 0u;
}

static inline void stats_add(Stats* s, uint32_t v)
{
    if (v < s->min)
    {
        s->min = v;
    }
    if (v > s->max)
    {
        s->max = v;
    }
    s->sum += v;
}

static inline uint32_t stats_mean(const Stats* s, uint32_t n)
{
    if (n == 0u)
    {
        return 0u;
    }
    return (uint32_t) (s->sum / n);
}

static uint32_t measure_cycle_overhead(uint32_t samples)
{
    uint32_t min = UINT32_MAX;
    if (samples == 0u)
    {
        samples = 1u;
    }
    for (uint32_t i = 0; i < samples; i++)
    {
        const uint32_t start = cycle_counter_read();
        const uint32_t end = cycle_counter_read();
        const uint32_t delta = end - start;
        if (delta < min)
        {
            min = delta;
        }
    }
    return min;
}

static void print_header(void)
{
    printf("%-5s %7s %10s %10s %10s %10s\n", "op", "bytes", "min", "mean", "max", "count");
}

static void print_row_size(const char* op, uint32_t size, const Stats* stats, uint32_t count)
{
    printf("%-5s %7" PRIu32 " %10" PRIu32 " %10" PRIu32 " %10" PRIu32 " %10" PRIu32 "\n",
           op,
           size,
           stats->min,
           stats_mean(stats, count),
           stats->max,
           count);
}

static void print_row_label(const char* op, const char* label, const Stats* stats, uint32_t count)
{
    printf("%-5s %7s %10" PRIu32 " %10" PRIu32 " %10" PRIu32 " %10" PRIu32 "\n",
           op,
           label,
           stats->min,
           stats_mean(stats, count),
           stats->max,
           count);
}

static void print_heap_header(void)
{
    printf("%-5s %12s %12s %12s %12s %12s\n",
           "heap", "capacity", "allocated", "peak", "peak_req", "oom");
}

static void print_heap_row(const char* label, const O1HeapDiagnostics* diag)
{
    printf("%-5s %12llu %12llu %12llu %12llu %12llu\n",
           label,
           (unsigned long long) diag->capacity,
           (unsigned long long) diag->allocated,
           (unsigned long long) diag->peak_allocated,
           (unsigned long long) diag->peak_request_size,
           (unsigned long long) diag->oom_count);
}

static uint32_t rand_u32(void)
{
    rng_state = (rng_state * 1664525u) + 1013904223u;
    return rng_state;
}

static size_t random_size_index(void)
{
    return (size_t) (rand_u32() % ALLOC_SIZES_COUNT);
}

/// Fisher-Yates shuffle for randomizing free order.
static void shuffle_indices(uint32_t* arr, uint32_t n)
{
    for (uint32_t i = n - 1; i > 0; i--)
    {
        const uint32_t j = rand_u32() % (i + 1);
        const uint32_t tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }
}

static bool run_perftest(void)
{
    rng_state = 0x9e3779b9u;  // Reset RNG for reproducible results each run
    O1HeapInstance* const heap = o1heapInit(heap_arena, sizeof(heap_arena));
    if (heap == NULL)
    {
        printf("o1heapInit failed (arena size=%u, min=%u)\n",
               (unsigned) sizeof(heap_arena),
               (unsigned) o1heapMinArenaSize);
        return false;
    }

    Stats alloc_stats[ALLOC_SIZES_COUNT];
    Stats free_stats[ALLOC_SIZES_COUNT];
    uint32_t alloc_counts[ALLOC_SIZES_COUNT] = {0u};
    uint32_t free_counts[ALLOC_SIZES_COUNT] = {0u};
    Stats alloc_total;
    Stats free_total;
    stats_init(&alloc_total);
    stats_init(&free_total);
    for (size_t i = 0; i < ALLOC_SIZES_COUNT; i++)
    {
        stats_init(&alloc_stats[i]);
        stats_init(&free_stats[i]);
    }

    const O1HeapDiagnostics diag_before = o1heapGetDiagnostics(heap);

    const uint32_t irq_state = save_and_disable_interrupts();
    const uint32_t overhead = measure_cycle_overhead(64u);

    // Batch allocations and frees with randomized patterns to explore heap states.
    // - Random batch sizes (32 to MAX_LIVE_BLOCKS)
    // - Random free order (shuffled)
    // - Partial frees (keep 0-25% of blocks sometimes)
    uint32_t total_allocs = 0;
    uint32_t total_frees = 0;
    uint32_t num_live = 0;  // Currently allocated blocks

    while (total_allocs < MEASURE_ITERATIONS || total_frees < MEASURE_ITERATIONS)
    {
        // Decide batch size: random between 32 and remaining capacity
        const uint32_t capacity = MAX_LIVE_BLOCKS - num_live;
        if (capacity == 0)
        {
            goto free_phase;  // Must free some blocks first
        }
        const uint32_t min_batch = (capacity < 32u) ? capacity : 32u;
        const uint32_t batch_size = min_batch + (rand_u32() % (capacity - min_batch + 1u));

        // Allocation phase
        for (uint32_t i = 0; i < batch_size; i++)
        {
            const uint32_t size_index = (uint32_t) random_size_index();
            const size_t alloc_size = alloc_sizes[size_index];

            const uint32_t alloc_start = cycle_counter_read();
            void* const p = o1heapAllocate(heap, alloc_size);
            const uint32_t alloc_end = cycle_counter_read();

            if (p == NULL)
            {
                break;  // Heap full
            }

            live_blocks[num_live] = p;
            live_block_size_idx[num_live] = size_index;
            num_live++;

            if (total_allocs < MEASURE_ITERATIONS)
            {
                const uint32_t alloc_cycles = alloc_end - alloc_start;
                const uint32_t alloc_adj = (alloc_cycles > overhead) ? (alloc_cycles - overhead) : 0u;
                stats_add(&alloc_stats[size_index], alloc_adj);
                stats_add(&alloc_total, alloc_adj);
                alloc_counts[size_index]++;
                total_allocs++;
            }
        }

    free_phase:;
        // Decide how many to free: sometimes keep 0-25% allocated
        const uint32_t keep_count = (rand_u32() % 4u == 0u) ? (rand_u32() % (num_live / 4u + 1u)) : 0u;
        const uint32_t num_to_free = num_live - keep_count;
        if (num_to_free == 0)
        {
            continue;
        }

        // Build shuffled free order
        for (uint32_t i = 0; i < num_live; i++)
        {
            free_order[i] = i;
        }
        shuffle_indices(free_order, num_live);

        // Free phase: free in random order
        uint32_t freed = 0;
        for (uint32_t i = 0; i < num_live && freed < num_to_free; i++)
        {
            const uint32_t idx = free_order[i];
            if (live_blocks[idx] == NULL)
            {
                continue;  // Already freed
            }

            const uint32_t size_index = live_block_size_idx[idx];

            const uint32_t free_start = cycle_counter_read();
            o1heapFree(heap, live_blocks[idx]);
            const uint32_t free_end = cycle_counter_read();

            live_blocks[idx] = NULL;
            freed++;

            if (total_frees < MEASURE_ITERATIONS)
            {
                const uint32_t free_cycles = free_end - free_start;
                const uint32_t free_adj = (free_cycles > overhead) ? (free_cycles - overhead) : 0u;
                stats_add(&free_stats[size_index], free_adj);
                stats_add(&free_total, free_adj);
                free_counts[size_index]++;
                total_frees++;
            }
        }

        // Compact live_blocks array (remove NULLs)
        uint32_t write_idx = 0;
        for (uint32_t read_idx = 0; read_idx < num_live; read_idx++)
        {
            if (live_blocks[read_idx] != NULL)
            {
                if (write_idx != read_idx)
                {
                    live_blocks[write_idx] = live_blocks[read_idx];
                    live_block_size_idx[write_idx] = live_block_size_idx[read_idx];
                }
                write_idx++;
            }
        }
        num_live = write_idx;
    }
    restore_interrupts(irq_state);

    const O1HeapDiagnostics diag_after = o1heapGetDiagnostics(heap);

    print_heap_header();
    print_heap_row("pre", &diag_before);
    print_heap_row("post", &diag_after);

    printf("overhead cycles: %" PRIu32 "\n", overhead);
    print_header();
    print_row_label("alloc", "total", &alloc_total, total_allocs);
    print_row_label("free", "total", &free_total, total_frees);
    for (size_t i = 0; i < ALLOC_SIZES_COUNT; i++)
    {
        print_row_size("alloc", alloc_sizes[i], &alloc_stats[i], alloc_counts[i]);
        print_row_size("free", alloc_sizes[i], &free_stats[i], free_counts[i]);
    }
    return true;
}

int main(void)
{
    stdio_uart_init_full(uart0, 115200, 0, 1);

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    sleep_ms(500);
    printf("\n\n\nO1Heap perftest on RP2350\n");

    cycle_counter_init();

    printf("sysclk=%" PRIu32 " Hz, heap=%u bytes, iterations=%u\n",
           (uint32_t) clock_get_hz(clk_sys),
           (unsigned) sizeof(heap_arena),
           (unsigned) MEASURE_ITERATIONS);

    bool led_on = false;
    while (true)
    {
        printf("\n\n\n=== BEGIN ===\n");
        led_on = !led_on;
        gpio_put(PICO_DEFAULT_LED_PIN, led_on);
        if (!run_perftest())
        {
            printf("perftest failed\n");
        }
        printf("\n===  END  ===\n");
        sleep_ms(500);
    }
}
