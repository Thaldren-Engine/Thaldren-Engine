#pragma once
// The sort the original program uses on its unit definition records (GameData_LoadAll
// 0x42D2E0 with 0x432D40 / 0x432FB0): the 1998 template library's introsort-free quicksort.
// It is not stable, so records that compare equal end in the order this exact algorithm
// leaves them; a stable sort gives the same result only when no two records are equal.
//
//  - Ranges of 16 or fewer: plain insertion sort.
//  - Otherwise the quicksort loop: while a range holds more than 16, its pivot is a copy of
//    the median of the first, middle (first + n / 2) and last records; an unguarded
//    partition (advance from the left while below the pivot, retreat from the right while
//    above it, swap, until they meet); the smaller side is sorted by recursion, the larger
//    one by the loop. Then insertion sort of the first 16 and an unguarded linear insertion
//    of every later record.

#include <cstddef>
#include <utility>
#include <vector>

namespace ta {

template <class T, class Less>
void sort_like_original(std::vector<T>& v, std::size_t first, std::size_t last, Less less) {
    if (last <= first) return;
    // Unguarded linear insertion of v[i] (something below it is not greater).
    auto insert_unguarded = [&](std::size_t i) {
        T val = std::move(v[i]);
        std::size_t j = i;
        while (less(val, v[j - 1])) {
            v[j] = std::move(v[j - 1]);
            --j;
        }
        v[j] = std::move(val);
    };
    auto insertion_sort = [&](std::size_t f, std::size_t l) {
        if (f == l) return;
        for (std::size_t i = f + 1; i != l; ++i) {
            if (less(v[i], v[f])) {
                T val = std::move(v[i]);
                for (std::size_t j = i; j > f; --j) v[j] = std::move(v[j - 1]);
                v[f] = std::move(val);
            } else {
                insert_unguarded(i);
            }
        }
    };
    auto median = [&](const T& a, const T& b, const T& c) -> const T& {
        if (less(a, b)) {
            if (less(b, c)) return b;
            if (less(a, c)) return c;
            return a;
        }
        if (less(a, c)) return a;
        if (less(b, c)) return c;
        return b;
    };
    struct Loop {
        std::vector<T>& v;
        Less& less;
        decltype(median)& med;
        void run(std::size_t f, std::size_t l) {
            while (l - f > 16) {
                const T pivot = med(v[f], v[f + (l - f) / 2], v[l - 1]);
                std::size_t i = f, j = l;
                for (;;) {
                    while (less(v[i], pivot)) ++i;
                    --j;
                    while (less(pivot, v[j])) --j;
                    if (!(i < j)) break;
                    std::swap(v[i], v[j]);
                    ++i;
                }
                const std::size_t cut = i;
                if (cut - f < l - cut) {
                    run(f, cut);
                    f = cut;
                } else {
                    run(cut, l);
                    l = cut;
                }
            }
        }
    };
    if (last - first <= 16) {
        insertion_sort(first, last);
        return;
    }
    Loop{v, less, median}.run(first, last);
    insertion_sort(first, first + 16);
    for (std::size_t i = first + 16; i != last; ++i) insert_unguarded(i);
}

}  // namespace ta
