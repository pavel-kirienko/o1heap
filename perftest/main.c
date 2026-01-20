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
#define WARMUP_ITERATIONS     128u
#define MEASURE_ITERATIONS    10000u
#define MAX_LIVE_BLOCKS       256u
#define PREFILL_FREE_STRIDE   3u

#define DEMCR_ADDR       (0xE000EDFCu)
#define DWT_CTRL_ADDR    (0xE0001000u)
#define DWT_CYCCNT_ADDR  (0xE0001004u)
#define DEMCR_TRCENA_BIT (1u << 24)
#define DWT_CTRL_CYCCNT  (1u << 0)

static alignas(O1HEAP_ALIGNMENT) uint8_t heap_arena[HEAP_ARENA_SIZE_BYTES];

static const uint16_t alloc_sizes[] = {
    8u, 16u, 24u, 32u, 48u, 64u, 96u, 128u, 192u, 256u, 384u, 512u, 768u, 1024u,
};

#define ALLOC_SIZES_COUNT (sizeof(alloc_sizes) / sizeof(alloc_sizes[0]))

static void* live_blocks[MAX_LIVE_BLOCKS];
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
    printf("%-5s %7s %10s %10s %10s\n", "op", "bytes", "min", "mean", "max");
}

static void print_row_size(const char* op, uint32_t size, const Stats* stats, uint32_t count)
{
    printf("%-5s %7" PRIu32 " %10" PRIu32 " %10" PRIu32 " %10" PRIu32 "\n",
           op,
           size,
           stats->min,
           stats_mean(stats, count),
           stats->max);
}

static void print_row_label(const char* op, const char* label, const Stats* stats, uint32_t count)
{
    printf("%-5s %7s %10" PRIu32 " %10" PRIu32 " %10" PRIu32 "\n",
           op,
           label,
           stats->min,
           stats_mean(stats, count),
           stats->max);
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

static void prefill_heap(O1HeapInstance* const heap)
{
    size_t count = 0;
    for (; count < MAX_LIVE_BLOCKS; count++)
    {
        const size_t size_index = random_size_index();
        void* const p = o1heapAllocate(heap, alloc_sizes[size_index]);
        if (p == NULL)
        {
            break;
        }
        live_blocks[count] = p;
    }

    for (size_t i = 0; i < count; i++)
    {
        if ((rand_u32() % PREFILL_FREE_STRIDE) == 0u)
        {
            o1heapFree(heap, live_blocks[i]);
            live_blocks[i] = NULL;
        }
    }
}

static bool cycle_counter_works(void)
{
    const uint32_t start = cycle_counter_read();
    for (volatile uint32_t i = 0; i < 1000u; i++)
    {
        __asm volatile("nop");
    }
    const uint32_t end = cycle_counter_read();
    return start != end;
}

static bool run_perftest(void)
{
    O1HeapInstance* const heap = o1heapInit(heap_arena, sizeof(heap_arena));
    if (heap == NULL)
    {
        printf("o1heapInit failed (arena size=%u, min=%u)\n",
               (unsigned) sizeof(heap_arena),
               (unsigned) o1heapMinArenaSize);
        return false;
    }

    prefill_heap(heap);

    for (uint32_t i = 0; i < WARMUP_ITERATIONS; i++)
    {
        const uint32_t size_index = (uint32_t) random_size_index();
        void* const p = o1heapAllocate(heap, alloc_sizes[size_index]);
        if (p == NULL)
        {
            printf("warmup alloc failed at %" PRIu32 "\n", i);
            return false;
        }
        o1heapFree(heap, p);
    }

    Stats alloc_stats[ALLOC_SIZES_COUNT];
    Stats free_stats[ALLOC_SIZES_COUNT];
    uint32_t counts[ALLOC_SIZES_COUNT] = {0u};
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
    for (uint32_t i = 0; i < MEASURE_ITERATIONS; i++)
    {
        const uint32_t size_index =
            (i < (uint32_t) ALLOC_SIZES_COUNT) ? i : (uint32_t) random_size_index();
        const size_t alloc_size = alloc_sizes[size_index];
        const uint32_t alloc_start = cycle_counter_read();
        void* const p = o1heapAllocate(heap, alloc_size);
        const uint32_t alloc_end = cycle_counter_read();
        if (p == NULL)
        {
            restore_interrupts(irq_state);
            printf("alloc failed at %" PRIu32 "\n", i);
            return false;
        }
        const uint32_t alloc_cycles = alloc_end - alloc_start;
        const uint32_t alloc_adj = (alloc_cycles > overhead) ? (alloc_cycles - overhead) : 0u;
        stats_add(&alloc_stats[size_index], alloc_adj);
        stats_add(&alloc_total, alloc_adj);

        const uint32_t free_start = cycle_counter_read();
        o1heapFree(heap, p);
        const uint32_t free_end = cycle_counter_read();
        const uint32_t free_cycles = free_end - free_start;
        const uint32_t free_adj = (free_cycles > overhead) ? (free_cycles - overhead) : 0u;
        stats_add(&free_stats[size_index], free_adj);
        stats_add(&free_total, free_adj);
        counts[size_index]++;
    }
    restore_interrupts(irq_state);

    const O1HeapDiagnostics diag_after = o1heapGetDiagnostics(heap);

    print_heap_header();
    print_heap_row("pre", &diag_before);
    print_heap_row("post", &diag_after);

    printf("overhead cycles: %" PRIu32 "\n", overhead);
    print_header();
    print_row_label("alloc", "total", &alloc_total, MEASURE_ITERATIONS);
    print_row_label("free", "total", &free_total, MEASURE_ITERATIONS);
    for (size_t i = 0; i < ALLOC_SIZES_COUNT; i++)
    {
        print_row_size("alloc", alloc_sizes[i], &alloc_stats[i], counts[i]);
        print_row_size("free", alloc_sizes[i], &free_stats[i], counts[i]);
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
    rng_state ^= (uint32_t) time_us_64();
    if (!cycle_counter_works())
    {
        while (true)
        {
            printf("DWT cycle counter is not running; check core configuration.\n");
            sleep_ms(1000);
        }
    }

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
        sleep_ms(10000);
    }
}
