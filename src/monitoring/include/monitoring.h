/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) Google Inc., 2025
 * Author: Roy Babayov roybabayov@google.com
 *
 *   This library is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Lesser General Public License as published
 *   by the Free Software Foundation; either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See
 *   the GNU Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public License
 *   along with this library; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/**
 * @brief Monitoring infra for libntirpc.
 *
 * This file contains C wrappers for prometheus-cpp-lite to enable metrics
 * handling.
 *
 * We avoid using float/double values since updating them *atomically* also
 * affects performance.
 *
 * Usage:
 *  - Create a static metric:
 *      my_handle = monitoring__register_counter(...);
 *  - Update the metric during running time:
 *      monitoring__counter_inc(my_handle, 1);
 *
 * Naming convention:
 *   For new metrics, please use "<module>__<metric>", for example:
 *   "clients__lease_expire_count"
 *
 * See more:
 *  - https://prometheus.io/docs/concepts/data_model/
 *  - https://prometheus.io/docs/concepts/metric_types/
 */

#ifndef NTIRPC_MONITORING_H
#define NTIRPC_MONITORING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Metric help description */
typedef struct metric_metadata {
	const char *description; /* Helper message */
	const char *unit; /* Units like: second, byte */
} metric_metadata_t;

/* Label is a dimension in the metric family, for example "operation=GETATTR" */
typedef struct metric_label {
	const char *key;
	const char *value;
} metric_label_t;

/* Buckets of (a,b,c) mean boundaries of: (-INF,a) [a,b) [b,c) [c, INF) */
typedef struct histogram_buckets {
	const int64_t *buckets;
	uint16_t count;
} histogram_buckets_t;

/* C wrapper for prometheus::Counter<int64_t> pointer */
typedef struct counter_metric_handle {
	void *family;
	void *metric;
} counter_metric_handle_t;

/* C wrapper for prometheus::Gauge<int64_t> pointer */
typedef struct gauge_metric_handle {
	void *family;
	void *metric;
} gauge_metric_handle_t;

/* C wrapper for prometheus::Histogram<int64_t> pointer */
typedef struct histogram_metric_handle {
	void *family;
	void *metric;
} histogram_metric_handle_t;

/* C wrapper for prometheus::Registry pointer */
typedef struct prometheus_registry_handle {
	void *registry;
} prometheus_registry_handle_t;

/* Metric value units. */
#define METRIC_UNIT_NONE (NULL)
#define METRIC_UNIT_MINUTE ("minute")
#define METRIC_UNIT_SECOND ("sec")
#define METRIC_UNIT_MILLISECOND ("ms")
#define METRIC_UNIT_MICROSECOND ("us")
#define METRIC_UNIT_NANOSECOND ("ns")

#define METRIC_METADATA(DESCRIPTION, UNIT) \
	((metric_metadata_t){ .description = (DESCRIPTION), .unit = (UNIT) })

#define METRIC_LABEL(KEY, VALUE) \
	((metric_label_t){ .key = (KEY), .value = (VALUE) })

#ifdef USE_MONITORING

/* Registers and initializes a new static counter metric. */
counter_metric_handle_t
monitoring__register_counter(const char *name, metric_metadata_t metadata,
			     const metric_label_t *labels, uint16_t num_labels);

/* Registers and initializes a new static gauge metric. */
gauge_metric_handle_t monitoring__register_gauge(const char *name,
						 metric_metadata_t metadata,
						 const metric_label_t *labels,
						 uint16_t num_labels);

/* Registers and initializes a new static histogram metric. */
histogram_metric_handle_t monitoring__register_histogram(
	const char *name, metric_metadata_t metadata,
	const metric_label_t *labels, uint16_t num_labels,
	histogram_buckets_t buckets);

/* Increments counter metric by value. */
void monitoring__counter_inc(counter_metric_handle_t, int64_t val);

/* Get the value of counter */
uint64_t monitoring__counter_get(counter_metric_handle_t handle);

/* Update if any change in increment value */
void monitoring__counter_set(counter_metric_handle_t handle, uint64_t value);

/* Remove the metric if not needed */
void monitoring__counter_remove(counter_metric_handle_t handle);
void monitoring__gauge_remove(gauge_metric_handle_t handle);
void monitoring__histogram_remove(histogram_metric_handle_t handle);

/* Increments gauge metric by value. */
void monitoring__gauge_inc(gauge_metric_handle_t, int64_t val);

/* Decrements gauge metric by value. */
void monitoring__gauge_dec(gauge_metric_handle_t, int64_t val);

/* Sets gauge metric value. */
void monitoring__gauge_set(gauge_metric_handle_t, int64_t val);

/* Observes a histogram metric value */
void monitoring__histogram_observe(histogram_metric_handle_t, int64_t val);

/* Returns default exp2 histogram buckets. */
histogram_buckets_t monitoring__buckets_exp2(void);

/* Returns compact exp2 histogram buckets (fewer compared to default). */
histogram_buckets_t monitoring__buckets_exp2_compact(void);

/* Returns handle for metrics registry */
prometheus_registry_handle_t monitoring__get_registry_handle(void);

#else /* USE_MONITORING */

#ifndef UNUSED
#define UNUSED_ATTR __attribute__((unused))
#define UNUSED(...) UNUSED_(__VA_ARGS__)
#define UNUSED_(arg) NOT_USED_##arg UNUSED_ATTR
#endif

static inline histogram_buckets_t monitoring__buckets_exp2(void)
{
	histogram_buckets_t dummy_ret = {};
	return dummy_ret;
}

static inline histogram_buckets_t monitoring__buckets_exp2_compact(void)
{
	histogram_buckets_t dummy_ret = {};
	return dummy_ret;
}

static inline counter_metric_handle_t monitoring__register_counter(
	const char *UNUSED(name), metric_metadata_t UNUSED(metadata),
	const metric_label_t *UNUSED(labels), uint16_t UNUSED(num_labels))
{
	counter_metric_handle_t dummy_ret = {};
	return dummy_ret;
}

static inline gauge_metric_handle_t monitoring__register_gauge(
	const char *UNUSED(name), metric_metadata_t UNUSED(metadata),
	const metric_label_t *UNUSED(labels), uint16_t UNUSED(num_labels))
{
	gauge_metric_handle_t dummy_ret = {};
	return dummy_ret;
}

static inline histogram_metric_handle_t monitoring__register_histogram(
	const char *UNUSED(name), metric_metadata_t UNUSED(metadata),
	const metric_label_t *UNUSED(labels), uint16_t UNUSED(num_labels),
	histogram_buckets_t UNUSED(buckets))
{
	histogram_metric_handle_t dummy_ret = {};
	return dummy_ret;
}

static inline void monitoring__counter_inc(
	counter_metric_handle_t UNUSED(handle), int64_t UNUSED(value))
{
}

static inline uint64_t
monitoring__counter_get(counter_metric_handle_t UNUSED(handle))
{
}

static inline void monitoring__counter_set(
	counter_metric_handle_t UNUSED(handle), uint64_t UNUSED(value))
{
}

static void monitoring__counter_remove(counter_metric_handle_t UNUSED(handle))
{
}

static void monitoring__gauge_remove(gauge_metric_handle_t UNUSED(handle))
{
}

static void
monitoring__histogram_remove(histogram_metric_handle_t UNUSED(handle))
{
}

static inline void monitoring__gauge_inc(gauge_metric_handle_t UNUSED(handle),
					 int64_t UNUSED(value))
{
}

static inline void monitoring__gauge_dec(gauge_metric_handle_t UNUSED(handle),
					 int64_t UNUSED(value))
{
}

static inline void monitoring__gauge_set(gauge_metric_handle_t UNUSED(handle),
					 int64_t UNUSED(value))
{
}

static inline void monitoring__histogram_observe(
	histogram_metric_handle_t UNUSED(handle), int64_t UNUSED(value))
{
}

static inline prometheus_registry_handle_t monitoring__get_registry_handle(void)
{
	prometheus_registry_handle_t dummy_ret = {};
	return dummy_ret;
}

#endif /* USE_MONITORING */

#ifdef __cplusplus
}
#endif

#endif /* NTIRPC_MONITORING_H */
