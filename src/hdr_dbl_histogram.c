/**
 * hdr_dbl_histogram.c
 * Written by Michael Barker and released to the public domain,
 * as explained at http://creativecommons.org/publicdomain/zero/1.0/
 */

#include <stdlib.h>
#include <errno.h>
#include <hdr_dbl_histogram.h>
#include <math.h>
#include <float.h>
#include <string.h>

static const double HIGHEST_VALUE_EVA = 4.49423283715579E307;

// ##     ## ######## #### ##       #### ######## ##    ##
// ##     ##    ##     ##  ##        ##     ##     ##  ##
// ##     ##    ##     ##  ##        ##     ##      ####
// ##     ##    ##     ##  ##        ##     ##       ##
// ##     ##    ##     ##  ##        ##     ##       ##
// ##     ##    ##     ##  ##        ##     ##       ##
//  #######     ##    #### ######## ####    ##       ##

static int64_t power(int64_t base, int64_t exp)
{
    int result = 1;
    while(exp)
    {
        result *= base; exp--;
    }
    return result;
}

static int32_t number_of_sub_buckets(int32_t significant_figures)
{
    int64_t largest_value_with_single_unit_resolution = 2 * (int64_t) pow(10, significant_figures);
    int32_t sub_bucket_count_magnitude = (int32_t) ceil(log(largest_value_with_single_unit_resolution/log(2)));

    return (int32_t) pow(2, sub_bucket_count_magnitude);
}

static int find_containing_binary_order_of_magnitude(int64_t value)
{
    return 64 - __builtin_clzll(value);
}

static int64_t calculate_internal_highest_to_lowest_value_ratio(int64_t external_highest_to_lowest_value_ratio)
{
    return INT64_C(1) << (find_containing_binary_order_of_magnitude(external_highest_to_lowest_value_ratio) + 1);
}

static int64_t calculate_integer_value_range(
        int64_t external_highest_to_lowest_value_ratio,
        int32_t significant_figures)
{
    int64_t internal_highest_to_lowest_value_ratio = calculate_internal_highest_to_lowest_value_ratio(external_highest_to_lowest_value_ratio);
    int64_t lowest_tracking_integer_value = number_of_sub_buckets(significant_figures) / 2;

    return lowest_tracking_integer_value * internal_highest_to_lowest_value_ratio;
}

static int32_t find_capped_containing_binary_order_of_magnitude(struct hdr_dbl_histogram *h, double d)
{
    if (d > h->highest_to_lowest_value_ratio)
    {
        return (int32_t) (log(h->highest_to_lowest_value_ratio) / log(2));
    }
    if (d > pow(2.0, 50))
    {
        return 50;
    }

    return find_containing_binary_order_of_magnitude(d);
}

static bool shift_covered_range_right(struct hdr_dbl_histogram* h, int32_t shift)
{
    double shift_multiplier = 1.0 / (INT64_C(1) << shift);

    if (h->values.total_count > hdr_count_at_index(&h->values, 0) ||
        hdr_shift_values_left(&h->values, shift))
    {
        h->current_lowest_value *= shift_multiplier;
        h->current_highest_value *= shift_multiplier;

        return true;
    }

    return false;
}

static bool shift_covered_range_left(struct hdr_dbl_histogram* h, int32_t shift)
{
    double shift_multiplier = 1.0 * (INT64_C(1) << shift);

    if (h->values.total_count > hdr_count_at_index(&h->values, 0) ||
        hdr_shift_values_right(&h->values, shift))
    {
        h->current_lowest_value *= shift_multiplier;
        h->current_highest_value *= shift_multiplier;

        return true;
    }

    return false;
}

// TODO: This is synchronised in the Java version, should we do the same here.
static bool adjust_range_for_value(struct hdr_dbl_histogram* h, double value)
{
    if (0.0 == value)
    {
        return true;
    }

    if (value < h->current_lowest_value)
    {
        if (value < 0.0)
        {
            return false;
        }

        do
        {
            double r_val = ceil(h->current_lowest_value / value) - 1.0;
            int32_t shift_amount = find_capped_containing_binary_order_of_magnitude(h, r_val);

            if (!shift_covered_range_right(h, shift_amount))
            {
                return false;
            }
        }
        while (value < h->current_lowest_value);
    }
    else if (value >= h->current_highest_value)
    {
        if (value > HIGHEST_VALUE_EVA)
        {
            return false;
        }

        do
        {
            double r_val = ceil(nextafter(value, DBL_MAX) / h->current_highest_value) - 1.0;
            int32_t shift_amount = find_capped_containing_binary_order_of_magnitude(h, r_val);

            if (!shift_covered_range_left(h, shift_amount))
            {
                return false;
            }
        }
        while (value >= h->current_highest_value);
    }

    return true;
}

// ##     ## ######## ##     ##  #######  ########  ##    ##
// ###   ### ##       ###   ### ##     ## ##     ##  ##  ##
// #### #### ##       #### #### ##     ## ##     ##   ####
// ## ### ## ######   ## ### ## ##     ## ########     ##
// ##     ## ##       ##     ## ##     ## ##   ##      ##
// ##     ## ##       ##     ## ##     ## ##    ##     ##
// ##     ## ######## ##     ##  #######  ##     ##    ##

int hdr_dbl_init(
    int64_t highest_to_lowest_value_ratio,
    int32_t significant_figures,
    struct hdr_dbl_histogram** result)
{
    if (highest_to_lowest_value_ratio < 2)
    {
        return EINVAL;
    }
    if (significant_figures < 1)
    {
        return EINVAL;
    }
    if ((highest_to_lowest_value_ratio * power(10, significant_figures)) >= (INT64_C(1) << INT64_C(61)))
    {
        return EINVAL;
    }

    struct hdr_dbl_histogram*dbl_histogram;
    struct hdr_histogram_bucket_config cfg;

    int64_t integer_value_range = calculate_integer_value_range(highest_to_lowest_value_ratio, significant_figures);

    int rc = hdr_calculate_bucket_config(1, integer_value_range - 1, significant_figures, &cfg);
    if (0 != rc)
    {
        return rc;
    }

    size_t histogram_size = sizeof(struct hdr_dbl_histogram) + cfg.counts_len * sizeof(int64_t);
    dbl_histogram = malloc(histogram_size);

    if (!dbl_histogram)
    {
        return ENOMEM;
    }

    // memset will ensure that all of the function pointers are null.
    memset((void*) dbl_histogram, 0, histogram_size);

    hdr_init_preallocated(&dbl_histogram->values, &cfg);
    *result = dbl_histogram;

    int64_t internal_highest_to_lowest_value_ratio =
            calculate_internal_highest_to_lowest_value_ratio(highest_to_lowest_value_ratio);

    dbl_histogram->highest_to_lowest_value_ratio = highest_to_lowest_value_ratio;
    dbl_histogram->current_lowest_value = pow(2.0, 800);
    dbl_histogram->current_highest_value = dbl_histogram->current_lowest_value * internal_highest_to_lowest_value_ratio;
    dbl_histogram->int_to_dbl_conversion_ratio = dbl_histogram->current_lowest_value / cfg.sub_bucket_half_count;
    dbl_histogram->dbl_to_int_conversion_ratio = 1.0 / dbl_histogram->int_to_dbl_conversion_ratio;
    dbl_histogram->values.conversion_ratio = dbl_histogram->int_to_dbl_conversion_ratio;

    return 0;
}

bool hdr_dbl_record_value(struct hdr_dbl_histogram* h, double value)
{
    if (value < h->current_lowest_value || h->current_highest_value <= value)
    {
        if (!adjust_range_for_value(h, value))
        {
            return false;
        }
    }

    int64_t int_value = (int64_t) (value * h->dbl_to_int_conversion_ratio);
    hdr_record_value(&h->values, int_value);

    return true;
}