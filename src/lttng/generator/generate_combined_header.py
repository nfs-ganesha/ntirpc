#!/usr/bin/python3
import argparse


def main():
  parser = argparse.ArgumentParser(
      prog='generate_combined_header_file',
      description='This script generates a combined header file of providers',
  )
  parser.add_argument('--output', help='Output file path', required=True)
  parser.add_argument(
      'input', nargs='+', help='List of files to include in the combined header'
  )
  args = parser.parse_args()

  with open(args.output, 'w') as f:
    for arg in args.input:
      f.write('#if __has_include("{}")\n'.format(arg))
      f.write('#include "{}"\n'.format(arg))
      f.write('#endif\n\n'.format(arg))


if __name__ == '__main__':
  main()
