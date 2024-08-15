#!/usr/bin/python3
from pathlib import Path
import tempfile
import re
import subprocess

DIR_PATH = Path(__file__).parent
GENERATOR_PATH = DIR_PATH / ".." / "generate_lttng_for_files"

# Dictionary of tests and partial expected error
TESTS = {
    "test_code_failure_too_many_arguments.c": (
        "doesn't allow more than 10 arguments"
    ),
    "test_code_failure_too_many_arguments_with_arrays.c": (
        "doesn't allow more than 10 arguments"
    ),
    "test_code_failure_too_many_arguments_for_format.c": (
        "Wrong number of arguments"
    ),
    "test_code_failure_not_enough_arguments_for_format.c": (
        "Wrong number of arguments"
    ),
    "test_code_failure_invalid_format1.c": (
        "We currently only allow specifying parameters with {}"
    ),
    "test_code_failure_invalid_format2.c": (
        "We currently only allow specifying parameters with {}"
    ),
    "test_code_failure_invalid_format3.c": "Wrong number of arguments",
    "test_code_failure_invalid_format4.c": (
        "We currently only allow specifying parameters with {}"
    ),
    "test_code_failure_unsupported_struct.c": "Argument type .* not supported",
    "test_code_failure_unsupported_typedef_struct.c": (
        "Argument type .* not supported"
    ),
    "test_code_failure_unsupported_union.c": "Argument type .* not supported",
    "test_code_failure_missing_log_level.c": "Invalid log level argument",
    "test_code_failure_missing_log_format.c": (
        "argument must be a string literal format"
    ),
    "test_code_failure_bad_fnc_format.c": (
        "We currently only allow specifying parameters with {}"
    ),
    "test_code_failure_has_side_effects_func_call.c": (
        "has side effects"
    ),
    "test_code_failure_has_side_effects_func_expression.c": (
        "has side effects"
    ),
    "test_code_failure_has_side_effects_inc.c": (
        "has side effects"
    ),
    "test_code_failure_has_side_effects_volatile.c": (
        "has side effects"
    ),
}


def tests():
  output_path = tempfile.mkdtemp()

  for test, expected_error in TESTS.items():
    command = [
        str(GENERATOR_PATH),
        "--output_dir",
        output_path,
        str(DIR_PATH / test),
    ]
    res = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stdin=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True,
    )
    if res.returncode == 0:
      raise Exception(
          "FAILURE!!! we expected {} to fail, but it didn't.\n"
          "command: {}\nstdout: {}\nstderr: {}".format(
              test, " ".join(command), res.stdout, res.stderr
          )
      )

    if not re.search(expected_error, res.stderr):
      raise Exception(
          "Generator command for test {} failed, but with an unexpected"
          " error.\ncommand: {}\nstdout: {}\nstderr:{}".format(
              test, " ".join(command), res.stdout, res.stderr
          )
      )


def assert_no_missing_test():
  for obj in DIR_PATH.iterdir():
    obj_name = obj.name
    if (
        obj_name.startswith("test_code_failure_")
        and obj_name.endswith(".c")
        and obj_name not in TESTS
    ):
      raise Exception("Test file {} not in TESTS dictionary".format(obj_name))


if __name__ == "__main__":
  try:
    assert_no_missing_test()
    print("Running failure tests...")
    tests()
    print("Done!")
  except Exception:
    print("Test failed!")
    raise
