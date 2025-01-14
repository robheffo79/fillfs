
# fillfs Utility

`fillfs` is a utility designed to either fill a filesystem to a specified size or overwrite an existing file with either zeroed or random data. It is a versatile tool for testing disk behavior under high usage or verifying data integrity by overwriting existing files.

## Features

- **Directory Mode**: Creates a hidden file (`/.fillfs`) in the specified directory and fills the filesystem until:
  - The specified size is reached, or
  - The disk is full.
- **File Mode**: Overwrites an existing file with zero or random data without removing it.
- Supports writing zeroed or random data.
- Optional progress updates, including throughput and ETA.
- Customizable block size for writing operations.
- Automatic cleanup for hidden files on most termination signals.

## Usage

### Synopsis

```bash
fillfs [OPTIONS] <mount_point_or_file> [size]
```

### Arguments

- `<mount_point_or_file>`: Required. The target path can be either:
  - A directory, where a hidden file (`/.fillfs`) will be created and filled.
  - An existing file, which will be overwritten in-place.
- `[size]`: Optional. Specifies the target size for the operation. Supports human-readable formats such as `1G`, `800M`, `32K`. If omitted:
  - For directories: The disk is filled until no space remains.
  - For files: The entire file is overwritten.

### Options

- `-r, --random`: Write random data instead of zeroed data.
- `-z, --zero`: Explicitly write zeroed data (overrides `--random` if both are set).
- `-s, --status`: Show progress updates, including throughput and estimated time remaining (ETA).
- `-b, --block-size=SIZE`: Use a custom block size for writes. Defaults to `32M` if not specified.
- `-h, --help`: Display help information.

## Examples

### Fill a Filesystem

Fill 1 GB on the root filesystem, showing status updates:

```bash
fillfs / --status 1G
```

Fill until the disk is completely full at `/mnt/data` with zeroed data:

```bash
fillfs /mnt/data
```

### Overwrite an Existing File

Overwrite an existing file up to 500 MB (without removing it):

```bash
fillfs /tmp/existing_file 500M
```

Use random data for 10 GB with a custom 4M block size:

```bash
fillfs -r -s -b 4M /mnt/data 10G
```

## Exit Codes

- `0`: Success.
- Non-zero: An error occurred or the program was interrupted.

## Notes

- **Directory Mode**: If forcibly terminated with `kill -9` (`SIGKILL`), cleanup of the hidden file may not occur.
- **File Mode**: No cleanup is attempted; the file remains in its current state after termination.

## License

This utility is licensed under the MIT License:

> Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
> 
> The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
> 
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## Author

Robert Heffernan <robert@heffernantech.au>
