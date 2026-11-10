// A small harness for the VR tests: CHECK keeps going after a failure and counts it, VR_TEST_END prints the total and
// gives the exit code (0 when everything passed). No dependencies beyond the C library.
#pragma once
#include <math.h>
#include <stdio.h>

static int vr_test_checks, vr_test_failures;
static const char *vr_test_name = "";

// VR_TEST_VERBOSE=1 prints each part as it starts (to find the part a crash happens in)
#include <stdlib.h>
#define VR_TEST(name) \
    (vr_test_name = (name), getenv("VR_TEST_VERBOSE") ? (void)fprintf(stderr, "-- %s\n", vr_test_name) : (void)0)

#define CHECK(cond)                                                                                          \
    do {                                                                                                     \
        vr_test_checks++;                                                                                    \
        if (!(cond)) {                                                                                       \
            vr_test_failures++;                                                                              \
            fprintf(stderr, "%s:%d: [%s] CHECK failed: %s\n", __FILE__, __LINE__, vr_test_name, #cond);      \
        }                                                                                                    \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                                \
    do {                                                                                                     \
        double vr_a_ = (double)(a), vr_b_ = (double)(b);                                                     \
        vr_test_checks++;                                                                                    \
        if (!(fabs(vr_a_ - vr_b_) <= (eps))) {                                                               \
            vr_test_failures++;                                                                              \
            fprintf(stderr, "%s:%d: [%s] CHECK_NEAR failed: %s = %.6f, %s = %.6f (eps %g)\n", __FILE__, __LINE__, \
                    vr_test_name, #a, vr_a_, #b, vr_b_, (double)(eps));                                      \
        }                                                                                                    \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                                                   \
    do {                                                                                                     \
        long long vr_a_ = (long long)(a), vr_b_ = (long long)(b);                                            \
        vr_test_checks++;                                                                                    \
        if (vr_a_ != vr_b_) {                                                                                \
            vr_test_failures++;                                                                              \
            fprintf(stderr, "%s:%d: [%s] CHECK_EQ failed: %s = %lld, %s = %lld\n", __FILE__, __LINE__,       \
                    vr_test_name, #a, vr_a_, #b, vr_b_);                                                     \
        }                                                                                                    \
    } while (0)

#define VR_TEST_END(suite)                                                                                   \
    (printf("%s: %d checks, %d failed\n", (suite), vr_test_checks, vr_test_failures), vr_test_failures ? 1 : 0)
