#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <x86intrin.h>

#include "benchmark.h"

static int32_t linear_search(const uint16_t *array, size_t len, uint16_t key) {
    for (size_t i = 0; i < len; ++i) {
        if (array[i] == key) {
            return i;
        }
    }
    return -1;
}

#define SHORT_PER_M256 (256 / 16)

static int32_t linear_search_avx(const uint16_t *array, size_t len,
                                 uint16_t key) {
    const __m256i constant = _mm256_set1_epi16((int16_t)key);
    const size_t n_simd_t = len * sizeof(uint16_t) / sizeof(__m256i);
    for (size_t i = 0; i < n_simd_t; ++i) {
        __m256i A1 = _mm256_lddqu_si256((__m256i *)array + i);
        __m256i A0 = _mm256_cmpeq_epi16(A1, constant);
        int32_t bits = _mm256_movemask_epi8(A0);
        int32_t bit_pos = (__builtin_ffs(bits) - 1) / 2;

        if (bits) {
            return (i * SHORT_PER_M256) + bit_pos;
        }
    }

    /* This could be done in one pass of cmpeq_epi16 if we're allowed to read
     * pass the array bounds. Valgrind will definitively complain. */
    size_t lookups_remaining = len % (SHORT_PER_M256);
    if (lookups_remaining) {
        for (size_t i = len - lookups_remaining; i < len; ++i) {
            if (array[i] == key) return i;
        }
    }

    return -1;
}

static int32_t binary_search(const uint16_t *source, size_t n,
                             uint16_t target) {
    uint16_t *base = source;
    if (n == 0) return -1;
    while (n > 1) {
        int32_t half = n / 2;
        base = (base[half] < target) ? &base[half] : base;
        n -= half;
    }
    base += *base < target;
    return *base == target ? base - source : -1;
}

static int32_t binary_search_leaf_prefetch(const uint16_t *source, size_t n,
                                           uint16_t target) {
    uint16_t *base = source;
    if (n == 0) return -1;
    while (n > 1) {
        int32_t half = n / 2;
        __builtin_prefetch(base + (half / 2), 0, 0);
        __builtin_prefetch(base + half + (half / 2), 0, 0);
        base = (base[half] < target) ? &base[half] : base;
        n -= half;
    }
    base += *base < target;
    return *base == target ? base - source : -1;
}

#define CUTOFF 128

static int32_t combined_search(const uint16_t *source, size_t n,
                               uint16_t target) {
    return (n <= CUTOFF) ? linear_search_avx(source, n, target)
                         : binary_search(source, n, target);
}

size_t run_test(__typeof__(linear_search) search, const uint16_t *array,
                size_t n_elems, const uint16_t *searches, size_t n_searches) {
    size_t found = 0;

    for (size_t i = 0; i < n_searches; ++i) {
        if (search(array, n_elems, searches[i]) != -1) {
            found++;
        }
    }

    return found;
}

#define val(idx) (2 * idx)

#define assert_eq(a, b)                                                \
    do {                                                               \
        if (a != b) {                                                  \
            fprintf(stderr, "expected %d got %d from %s\n", a, b, #b); \
        }                                                              \
    } while (0)

#define CACHELINE_PER_ARRAY(size) (size * 2 / 64)

void cache_populate(uint16_t *array, size_t n_elems) {
    for (size_t i = 0; i < CACHELINE_PER_ARRAY(n_elems); ++i)
        __builtin_prefetch(&array[i]);
}

void cache_flush(uint16_t *array, size_t n_elems) {
    for (size_t i = 0; i < n_elems; ++i) __builtin_ia32_clflush(&array[i]);
}

void permute(uint16_t *array, size_t len) {
    for (size_t i = 0; i < len - 2; ++i) {
        const size_t idx = rand() % len;
        // lazy swap
        uint16_t tmp = array[i];
        array[i] = array[idx];
        array[idx] = tmp;
    }
}

int main(int argc, char *argv[]) {
    size_t n_elems = strtol(argv[1], NULL, 10);

    size_t repeat = 100;
    // WARN: update searches init loop if changing this expression.
    size_t n_searches = n_elems * 2;
    size_t expected_finds = 0;

    uint16_t array[n_elems];
    for (size_t i = 0; i < n_elems; ++i) {
        array[i] = val(i);

        expected_finds += val(i) < n_searches;
    }

    uint16_t searches[n_searches];
    for (size_t i = 0; i < n_searches; ++i) {
        searches[i] = i;
    }

    /* shuffling searches order is used to compensate the advantage in cache
     * locality induced by the benchmark would benefit */
    srand(29032015);
    permute(searches, n_searches);

    /* validate implementations */
    for (size_t i = 0; i < n_searches; ++i) {
        const int32_t expected = linear_search(array, n_elems, searches[i]);
        assert_eq(expected, linear_search_avx(array, n_elems, searches[i]));
        assert_eq(expected, combined_search(array, n_elems, searches[i]));
        assert_eq(expected, binary_search(array, n_elems, searches[i]));
        assert_eq(expected,
                  binary_search_leaf_prefetch(array, n_elems, searches[i]));
    }

    // clang-format off
    BEST_TIME_PRE(run_test(linear_search,
                       array, n_elems,
                       searches, n_searches),
                  cache_populate(array, n_elems),
                  expected_finds, repeat, n_searches);

    BEST_TIME_PRE(run_test(linear_search_avx,
                       array, n_elems,
                       searches, n_searches),
                  cache_populate(array, n_elems),
                  expected_finds, repeat, n_searches);


    BEST_TIME_PRE(run_test(binary_search,
                       array, n_elems,
                       searches, n_searches),
                  cache_populate(array, n_elems),
                  expected_finds, repeat, n_searches);

    BEST_TIME_PRE(run_test(binary_search_leaf_prefetch,
                       array, n_elems,
                       searches, n_searches),
                  cache_populate(array, n_elems),
                  expected_finds, repeat, n_searches);

    BEST_TIME_PRE(run_test(combined_search,
                       array, n_elems,
                       searches, n_searches),
                  cache_populate(array, n_elems),
                  expected_finds, repeat, n_searches);

    /*
    BEST_TIME_PRE(run_test(linear_search,
                       array, n_elems,
                       searches, n_searches),
                  cache_flush(array, n_elems),
                  expected_finds, repeat, n_searches);

    BEST_TIME_PRE(run_test(linear_search_avx,
                       array, n_elems,
                       searches, n_searches),
                  cache_flush(array, n_elems),
                  expected_finds, repeat, n_searches);

    BEST_TIME_PRE(run_test(binary_search,
                       array, n_elems,
                       searches, n_searches),
                  cache_flush(array, n_elems),
                  expected_finds, repeat, n_searches);

    BEST_TIME_PRE(run_test(combined_search,
                       array, n_elems,
                       searches, n_searches),
                  cache_flush(array, n_elems),
                  expected_finds, repeat, n_searches);
    */

    // clang-format on

    return EXIT_SUCCESS;
}
