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

#include <unistd.h>

#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <algorithm>
#include <shared_mutex>

#include "monitoring.h"

#ifdef USE_MONITORING

#include "prometheus/counter.h"
#include "prometheus/gauge.h"
#include "prometheus/histogram.h"

namespace ntirpc_monitoring
{

using CounterInt = prometheus::Counter<int64_t>;
using GaugeInt = prometheus::Gauge<int64_t>;
using HistogramInt = prometheus::Histogram<int64_t>;
using HistogramDouble = prometheus::Histogram<double>;
using LabelsMap = std::map<const std::string, const std::string>;

using CounterFamily = prometheus::CustomFamily<CounterInt>;
using GaugeFamily = prometheus::CustomFamily<GaugeInt>;
using HistogramFamily = prometheus::CustomFamily<HistogramInt>;
using Histogramdoublefamily = prometheus::CustomFamily<HistogramDouble>;
static prometheus::Registry registry;

/**
 * @brief Formats full description from metadata into output buffer.
 *
 * @note description character array needs to be provided by the caller,
 * the function populates a string into the character array.
 */
static const std::string get_description(const metric_metadata_t &metadata)
{
	std::ostringstream description;
	description << metadata.description;
	if (metadata.unit != METRIC_UNIT_NONE) {
		description << " [" << metadata.unit << "]";
	}

	return description.str();
}

static const LabelsMap get_labels(const metric_label_t *labels,
				  uint16_t num_labels)
{
	LabelsMap labels_map;
	for (uint16_t i = 0; i < num_labels; i++) {
		labels_map.emplace(labels[i].key, labels[i].value);
	}
	return labels_map;
}

template <typename X, typename Y, typename Z>
static X convert_to_handle(Y *family, Z *metric)
{
	void *const family_ptr = static_cast<void *>(family);
	void *const metric_ptr = static_cast<void *>(metric);
	return { family_ptr, metric_ptr };
}

template <typename X, typename Y> static X *convert_metric_from_handle(Y handle)
{
	void *const ptr = handle.metric;
	return static_cast<X *>(ptr);
}

template <typename X, typename Y> static X *convert_family_from_handle(Y handle)
{
	void *const ptr = handle.family;
	return static_cast<X *>(ptr);
}

extern "C" {

histogram_buckets_t monitoring__buckets_exp2(void)
{
	static const int64_t buckets[] = {
		1,	   2,	     4,	       8,	  16,	     32,
		64,	   128,	     256,      512,	  1024,	     2048,
		4096,	   8192,     16384,    32768,	  65536,     131072,
		262144,	   524288,   1048576,  2097152,	  4194304,   8388608,
		16777216,  33554432, 67108864, 134217728, 268435456, 536870912,
		1073741824
	};
	return { buckets, sizeof(buckets) / sizeof(*buckets) };
}

histogram_buckets_t monitoring__buckets_exp2_compact(void)
{
	static const int64_t buckets[] = { 10,	  20,	 40,	 80,
					   160,	  320,	 640,	 1280,
					   2560,  5120,	 10240,	 20480,
					   40960, 81920, 163840, 327680 };
	return { buckets, sizeof(buckets) / sizeof(*buckets) };
}

counter_metric_handle_t
monitoring__register_counter(const char *name, metric_metadata_t metadata,
			     const metric_label_t *labels, uint16_t num_labels)
{
	auto &family = prometheus::Builder<CounterInt>()
			       .Name(name)
			       .Help(get_description(metadata))
			       .Register(registry);
	auto &counter = family.Add(get_labels(labels, num_labels));

	return convert_to_handle<counter_metric_handle_t>(&family, &counter);
}

gauge_metric_handle_t monitoring__register_gauge(const char *name,
						 metric_metadata_t metadata,
						 const metric_label_t *labels,
						 uint16_t num_labels)
{
	auto &family = prometheus::Builder<GaugeInt>()
			       .Name(name)
			       .Help(get_description(metadata))
			       .Register(registry);
	auto &gauge = family.Add(get_labels(labels, num_labels));

	return convert_to_handle<gauge_metric_handle_t>(&family, &gauge);
}

histogram_metric_handle_t
monitoring__register_histogram(const char *name, metric_metadata_t metadata,
			       const metric_label_t *labels,
			       uint16_t num_labels, histogram_buckets_t buckets)
{
	const auto &buckets_vector =
		HistogramInt::BucketBoundaries(buckets.buckets,
					       buckets.buckets + buckets.count);
	auto &family = prometheus::Builder<HistogramInt>()
			       .Name(name)
			       .Help(get_description(metadata))
			       .Register(registry);
	auto &histogram =
		family.Add(get_labels(labels, num_labels), buckets_vector);

	return convert_to_handle<histogram_metric_handle_t>(&family,
							    &histogram);
}

void monitoring__counter_inc(counter_metric_handle_t handle, int64_t value)
{
	convert_metric_from_handle<CounterInt>(handle)->Increment(value);
}

uint64_t monitoring__counter_get(counter_metric_handle_t handle)
{
	return convert_metric_from_handle<CounterInt>(handle)->Get();
}

void monitoring__counter_set(counter_metric_handle_t handle, uint64_t value)
{
	uint64_t old_value = monitoring__counter_get(handle);
	uint64_t new_value_to_inc = (value > old_value) ? (value - old_value)
							: 0;
	convert_metric_from_handle<CounterInt>(handle)->Increment(
		new_value_to_inc);
}

void monitoring__counter_remove(counter_metric_handle_t handle)
{
	auto *family = convert_family_from_handle<CounterFamily>(handle);
	auto *counter = convert_metric_from_handle<CounterInt>(handle);

	family->Remove(counter);
}

void monitoring__gauge_remove(gauge_metric_handle_t handle)
{
	auto *family = convert_family_from_handle<GaugeFamily>(handle);
	auto *gauge = convert_metric_from_handle<GaugeInt>(handle);

	family->Remove(gauge);
}

void monitoring__histogram_remove(histogram_metric_handle_t handle)
{
	auto *family = convert_family_from_handle<HistogramFamily>(handle);
	auto *histogram = convert_metric_from_handle<HistogramInt>(handle);

	family->Remove(histogram);
}

void monitoring__gauge_inc(gauge_metric_handle_t handle, int64_t value)
{
	convert_metric_from_handle<GaugeInt>(handle)->Increment(value);
}

void monitoring__gauge_dec(gauge_metric_handle_t handle, int64_t value)
{
	convert_metric_from_handle<GaugeInt>(handle)->Decrement(value);
}

void monitoring__gauge_set(gauge_metric_handle_t handle, int64_t value)
{
	convert_metric_from_handle<GaugeInt>(handle)->Set(value);
}

void monitoring__histogram_observe(histogram_metric_handle_t handle,
				   int64_t value)
{
	convert_metric_from_handle<HistogramInt>(handle)->Observe(value);
}

prometheus_registry_handle_t monitoring__get_registry_handle(void)
{
	void *const registry_ptr = static_cast<void *>(&registry);
	return { registry_ptr };
}

} /* extern "C" */

} /* namespace ntirpc_monitoring */

#endif /* USE_MONITORING */
