#!/usr/bin/python
import datetime
import sys

import bt2

NS_PER_SEC = 1e9
CPU_ID = "cpu_id"
FORMAT = "format"


def print_help_and_exist():
  print("{} [traces_path]".format(sys.argv[0]))
  exit(1)


def parse_args():
  if len(sys.argv) != 2:
    print_help_and_exist()

  return sys.argv[1]


def is_array_len(arg_name: str):
  # We have code in the generator to verify that no arg name except for array
  # length can start with underscore
  return arg_name[0] == "_"


if __name__ == "__main__":
  path = parse_args()
  for msg in bt2.TraceCollectionMessageIterator(path):
    if not isinstance(msg, bt2._EventMessageConst):
      continue

    event = msg.event

    time = datetime.datetime.utcfromtimestamp(
        msg.default_clock_snapshot.ns_from_origin / NS_PER_SEC
    )
    payload = event.payload_field

    arg_values = []
    format_string = None
    cpu_id = None
    try:
      cpu_id = event[CPU_ID]
      format_string = payload[FORMAT].labels[0]
      for arg_name in payload:
        if arg_name == FORMAT:
          continue

        if is_array_len(arg_name):
          continue

        value_struct = payload[arg_name]
        if isinstance(
            value_struct,
            (
                bt2._UnsignedEnumerationFieldConst,
                bt2._SignedEnumerationFieldConst,
            ),
        ):
          value = value_struct.labels[0]
        else:
          value = str(value_struct)

        arg_values.append(value)

    except KeyError:
      # This event doesn't have the expected keys, this makes sense, as the
      # infra adds some events during the startup of the process.
      # Skip this event...
      continue

    print(
        ("{} | cpu_id={} | " + format_string).format(time, cpu_id, *arg_values)
    )
