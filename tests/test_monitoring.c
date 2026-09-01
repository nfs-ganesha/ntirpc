#include <assert.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "monitoring.h"

#define MAX_TEST_METRICS 128
#define MAX_METRIC_NAME_LEN 128
#define INPUT_BUF_SIZE 256

enum test_metric_type {
	TEST_METRIC_COUNTER,
	TEST_METRIC_GAUGE,
	TEST_METRIC_HISTOGRAM,
};

struct test_metric_entry {
	char name[MAX_METRIC_NAME_LEN];
	enum test_metric_type type;
	union {
		counter_metric_handle_t counter;
		gauge_metric_handle_t gauge;
		histogram_metric_handle_t histogram;
	} handle;
};

static struct test_metric_entry registered_metrics[MAX_TEST_METRICS];
static size_t registered_metric_count = 0;

static const char *metric_type_to_string(enum test_metric_type type)
{
	switch (type) {
	case TEST_METRIC_COUNTER:
		return "counter";
	case TEST_METRIC_GAUGE:
		return "gauge";
	case TEST_METRIC_HISTOGRAM:
		return "histogram";
	default:
		return "unknown";
	}
}

static void usage(const char *prog)
{
	printf("Usage:\n");
	printf("  %s selftest\n", prog);
	printf("  %s interactive\n", prog);
	printf("\n");
	printf("Interactive commands:\n");
	printf("  add counter <name>\n");
	printf("  add gauge <name>\n");
	printf("  add histogram <name>\n");
	printf("  show\n");
	printf("  exit\n");
	printf("\n");
	printf("Recommended for unit testing:\n");
	printf("  %s selftest\n", prog);
}

static void trim_newline(char *str)
{
	size_t len;

	if (str == NULL)
		return;

	len = strlen(str);

	if (len > 0 && str[len - 1] == '\n')
		str[len - 1] = '\0';
}

static int metric_name_exists(const char *name)
{
	size_t i;

	for (i = 0; i < registered_metric_count; i++) {
		if (strcmp(registered_metrics[i].name, name) == 0)
			return 1;
	}

	return 0;
}

static void show_registered_metrics(void)
{
	size_t i;

	if (registered_metric_count == 0) {
		printf("No metrics registered till now\n");
		return;
	}

	printf("Metrics registered till now:\n");

	for (i = 0; i < registered_metric_count; i++) {
		printf("  [%zu] name=%s type=%s\n", i,
		       registered_metrics[i].name,
		       metric_type_to_string(registered_metrics[i].type));
	}
}

static void test_counter_wrapper(void)
{
	printf("[TEST] counter wrapper\n");

	const metric_label_t labels[] = {
		METRIC_LABEL("type", "unit_test"),
	};

	counter_metric_handle_t counter = monitoring__register_counter(
		"test__counter_total",
		METRIC_METADATA("Unit test counter", METRIC_UNIT_NONE), labels,
		1);

	assert(counter.family != NULL);
	assert(counter.metric != NULL);

	uint64_t value = monitoring__counter_get(counter);
	assert(value == 0);

	monitoring__counter_inc(counter, 5);
	value = monitoring__counter_get(counter);
	assert(value == 5);

	monitoring__counter_set(counter, 10);
	value = monitoring__counter_get(counter);
	assert(value == 10);

	/*
	 * Counter values cannot go backwards.
	 * monitoring__counter_set() only increments if the new value is bigger.
	 */
	monitoring__counter_set(counter, 3);
	value = monitoring__counter_get(counter);
	assert(value == 10);

	monitoring__counter_remove(counter);

	printf("[PASS] counter wrapper\n");
}

static void test_gauge_wrapper(void)
{
	printf("[TEST] gauge wrapper\n");

	const metric_label_t labels[] = {
		METRIC_LABEL("type", "unit_test"),
	};

	gauge_metric_handle_t gauge = monitoring__register_gauge(
		"test__gauge",
		METRIC_METADATA("Unit test gauge", METRIC_UNIT_NONE), labels,
		1);

	assert(gauge.family != NULL);
	assert(gauge.metric != NULL);

	/*
	 * There is no monitoring__gauge_get() wrapper currently,
	 * so this test verifies that gauge operations do not crash.
	 */
	monitoring__gauge_set(gauge, 100);
	monitoring__gauge_inc(gauge, 25);
	monitoring__gauge_dec(gauge, 10);

	monitoring__gauge_remove(gauge);

	printf("[PASS] gauge wrapper\n");
}

static void test_histogram_wrapper(void)
{
	printf("[TEST] histogram wrapper\n");

	const metric_label_t labels[] = {
		METRIC_LABEL("type", "unit_test"),
	};

	histogram_buckets_t buckets = monitoring__buckets_exp2_compact();

	assert(buckets.buckets != NULL);
	assert(buckets.count > 0);

	histogram_metric_handle_t histogram = monitoring__register_histogram(
		"test__histogram",
		METRIC_METADATA("Unit test histogram", METRIC_UNIT_MILLISECOND),
		labels, 1, buckets);

	assert(histogram.family != NULL);
	assert(histogram.metric != NULL);

	monitoring__histogram_observe(histogram, 10);
	monitoring__histogram_observe(histogram, 20);
	monitoring__histogram_observe(histogram, 100);

	monitoring__histogram_remove(histogram);

	printf("[PASS] histogram wrapper\n");
}

static void run_selftest(void)
{
	test_counter_wrapper();
	test_gauge_wrapper();
	test_histogram_wrapper();

	prometheus_registry_handle_t registry =
		monitoring__get_registry_handle();
	assert(registry.registry != NULL);

	printf("\nAll monitoring wrapper tests passed.\n");
}

static void remember_counter_metric(const char *name,
				    counter_metric_handle_t counter)
{
	if (registered_metric_count >= MAX_TEST_METRICS) {
		fprintf(stderr, "Metric registry full\n");
		exit(1);
	}

	snprintf(registered_metrics[registered_metric_count].name,
		 MAX_METRIC_NAME_LEN, "%s", name);

	registered_metrics[registered_metric_count].type = TEST_METRIC_COUNTER;
	registered_metrics[registered_metric_count].handle.counter = counter;
	registered_metric_count++;
}

static void remember_gauge_metric(const char *name, gauge_metric_handle_t gauge)
{
	if (registered_metric_count >= MAX_TEST_METRICS) {
		fprintf(stderr, "Metric registry full\n");
		exit(1);
	}

	snprintf(registered_metrics[registered_metric_count].name,
		 MAX_METRIC_NAME_LEN, "%s", name);

	registered_metrics[registered_metric_count].type = TEST_METRIC_GAUGE;
	registered_metrics[registered_metric_count].handle.gauge = gauge;
	registered_metric_count++;
}

static void remember_histogram_metric(const char *name,
				      histogram_metric_handle_t histogram)
{
	if (registered_metric_count >= MAX_TEST_METRICS) {
		fprintf(stderr, "Metric registry full\n");
		exit(1);
	}

	snprintf(registered_metrics[registered_metric_count].name,
		 MAX_METRIC_NAME_LEN, "%s", name);

	registered_metrics[registered_metric_count].type =
		TEST_METRIC_HISTOGRAM;
	registered_metrics[registered_metric_count].handle.histogram =
		histogram;
	registered_metric_count++;
}

static void add_counter_interactive(const char *name)
{
	counter_metric_handle_t counter;

	if (name == NULL || strlen(name) == 0) {
		fprintf(stderr, "Counter name is required\n");
		return;
	}

	if (metric_name_exists(name)) {
		fprintf(stderr, "Metric already exists in test registry: %s\n",
			name);
		return;
	}

	counter = monitoring__register_counter(
		name,
		METRIC_METADATA("Interactive test counter", METRIC_UNIT_NONE),
		NULL, 0);

	if (counter.metric == NULL) {
		fprintf(stderr, "Failed to register counter: %s\n", name);
		return;
	}

	remember_counter_metric(name, counter);

	printf("Counter registered: %s\n", name);
}

static void add_gauge_interactive(const char *name)
{
	gauge_metric_handle_t gauge;

	if (name == NULL || strlen(name) == 0) {
		fprintf(stderr, "Gauge name is required\n");
		return;
	}

	if (metric_name_exists(name)) {
		fprintf(stderr, "Metric already exists in test registry: %s\n",
			name);
		return;
	}

	gauge = monitoring__register_gauge(
		name,
		METRIC_METADATA("Interactive test gauge", METRIC_UNIT_NONE),
		NULL, 0);

	if (gauge.metric == NULL) {
		fprintf(stderr, "Failed to register gauge: %s\n", name);
		return;
	}

	remember_gauge_metric(name, gauge);

	printf("Gauge registered: %s\n", name);
}

static void add_histogram_interactive(const char *name)
{
	histogram_metric_handle_t histogram;
	histogram_buckets_t buckets;

	if (name == NULL || strlen(name) == 0) {
		fprintf(stderr, "Histogram name is required\n");
		return;
	}

	if (metric_name_exists(name)) {
		fprintf(stderr, "Metric already exists in test registry: %s\n",
			name);
		return;
	}

	buckets = monitoring__buckets_exp2_compact();

	histogram = monitoring__register_histogram(
		name,
		METRIC_METADATA("Interactive test histogram",
				METRIC_UNIT_MILLISECOND),
		NULL, 0, buckets);

	if (histogram.metric == NULL) {
		fprintf(stderr, "Failed to register histogram: %s\n", name);
		return;
	}

	remember_histogram_metric(name, histogram);

	printf("Histogram registered: %s\n", name);
}

static void remove_all_registered_metrics(void)
{
	size_t i;

	if (registered_metric_count == 0) {
		printf("No registered metrics to remove\n");
		return;
	}

	printf("Removing all registered metrics...\n");

	/*
	 * Remove in reverse order.
	 * This is generally safer when cleaning up resources.
	 */
	for (i = registered_metric_count; i > 0; i--) {
		struct test_metric_entry *entry = &registered_metrics[i - 1];

		switch (entry->type) {
		case TEST_METRIC_COUNTER:
			monitoring__counter_remove(entry->handle.counter);
			printf("Removed counter: %s\n", entry->name);
			break;

		case TEST_METRIC_GAUGE:
			monitoring__gauge_remove(entry->handle.gauge);
			printf("Removed gauge: %s\n", entry->name);
			break;

		case TEST_METRIC_HISTOGRAM:
			monitoring__histogram_remove(entry->handle.histogram);
			printf("Removed histogram: %s\n", entry->name);
			break;

		default:
			printf("Unknown metric type for: %s\n", entry->name);
			break;
		}
	}

	registered_metric_count = 0;
}

static void interactive_loop(void)
{
	char input[INPUT_BUF_SIZE];
	char command[64];
	char type[64];
	char name[MAX_METRIC_NAME_LEN];

	printf("Monitoring wrapper interactive test mode\n");
	printf("Commands:\n");
	printf("  add counter <name>\n");
	printf("  add gauge <name>\n");
	printf("  add histogram <name>\n");
	printf("  show\n");
	printf("  exit\n");

	while (1) {
		printf("\nmonitoring-test> ");

		if (fgets(input, sizeof(input), stdin) == NULL) {
			printf("\nEOF received. Exiting...\n");
			remove_all_registered_metrics();
			break;
		}

		trim_newline(input);

		command[0] = '\0';
		type[0] = '\0';
		name[0] = '\0';

		if (sscanf(input, "%63s %63s %127s", command, type, name) < 1)
			continue;

		if (strcmp(command, "exit") == 0) {
			remove_all_registered_metrics();
			printf("Exiting monitoring wrapper interactive test\n");
			break;
		}
		if (strcmp(command, "show") == 0) {
			show_registered_metrics();
			continue;
		}

		if (strcmp(command, "add") == 0) {
			if (strlen(type) == 0 || strlen(name) == 0) {
				printf("Usage: add <counter|gauge|histogram> <name>\n");
				continue;
			}

			if (strcmp(type, "counter") == 0) {
				add_counter_interactive(name);
			} else if (strcmp(type, "gauge") == 0) {
				add_gauge_interactive(name);
			} else if (strcmp(type, "histogram") == 0) {
				add_histogram_interactive(name);
			} else {
				printf("Unknown metric type: %s\n", type);
				printf("Supported types: counter, gauge, histogram\n");
			}

			continue;
		}

		printf("Unknown command: %s\n", command);
		printf("Supported commands: add, show, exit\n");
	}
}

int main(int argc, char *argv[])
{
	const char *op;

	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}

	op = argv[1];

	if (strcmp(op, "selftest") == 0) {
		run_selftest();
		return 0;
	}

	if (strcmp(op, "interactive") == 0) {
		interactive_loop();
		return 0;
	}

	if (strcmp(op, "help") == 0 || strcmp(op, "--help") == 0 ||
	    strcmp(op, "-h") == 0) {
		usage(argv[0]);
		return 0;
	}

	fprintf(stderr, "Unknown operation: %s\n", op);
	usage(argv[0]);

	return 1;
}
