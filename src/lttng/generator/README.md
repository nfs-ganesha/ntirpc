# lttng-trace-generator
===========

lttng-trace-generator is a tool to automatically generate the required data structures for LTTNG tracepoints.
For more info about LTTNG see [lttng.org/docs](https://lttng.org/docs/)

## Building
Assuming your current working directory is the directory containing this README file, run:
```
mkdir build
cd build
cmake ../ -DCMAKE_EXPORT_COMPILE_COMMANDS=1
make
```

Note that the trace generator, and any project using it must be built with
`-DCMAKE_EXPORT_COMPILE_COMMANDS=1` so that the generator has a compile_commands.json
database to get the compilation options from

## Using

In order to use the generator, include "lttng_generator.h" in your code, and use `AUTO_TRACEPOINT()`
instead of `tracepoint()` when you define your trace points.

`AUTO_TRACEPOINT()`, like `tracepoint` takes the provider and event names as the first 2 parameters,
then it takes the debug level and a format string, and finally it takes all the additional arguments to print.
For example:
```
AUTO_TRACEPOINT(provider1, event1, TRACE_INFO, "This is a test event with argument: {}, 1)
```
Will produce a trace point with provider "provider1", event name "event1" at level info and
with the given format.
For more examples, see test code, under `test/`.

Once you write your code, you can run the tool in one of two ways:
1. Use `generate_lttng_for_project` which takes a whole project dir and generates the required lttng header definitions for the entire project at once.
2. use `generate_lttng_for_files` which takes a list of files and generates the traces header definitions only for the given files.

Both tools also take a path to an output directory, where the generated headers are created.

The tool will parse your code and generate a header file for each tracepoint provider.
Once the header file is generated, include it as you would any other header of lttng traces.

Note that it is highly recommended that you run the generation process on every build, since if you
don't, and you change the input variables without re-generating the code, you might get undesired results.

For example, let's say you generated traces for:
```
char arr[10];
AUTO_TRACEPOINT(prov, event, "Print arr: {}", arr)
```

This will generate an LTTNG event for a constant size array:
```
TRACEPOINT_EVENT(
	prov,
	event,
	TP_ARGS(
		const char*, unused_format,
		const char*, arr),
	TP_FIELDS(
		...
		ctf_array_text(char, arr, arr, 10)
	)
)
```
Now, if you change the code, for example to:
```
char *arr;
AUTO_TRACEPOINT(prov, event, "Print arr: {}", arr)
```
And you don't re-generate the code, when you call the event it will read 10 bytes form arr,
which can potentially access memory it shouldn't.

### CMake integration
You can integrate the trace generator with cmake by using `generate_cmake_rules.py`.
This will generate cmake targets that will generate the traces.

Once you generate the cmake targets, each target in your project that might use
traces needs to depend on the auto-generate target.

It is recommended to run the cmake rule generation during cmake config, and include
the result in the project cmake files. See the generator cmake files as an example
