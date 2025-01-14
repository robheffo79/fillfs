
# fillfs

`fillfs` is a utility designed to fill a filesystem with data up to a specified size or until the disk is full. It creates a hidden file (`/.fillfs`) on the target filesystem and writes data to it. This tool is useful for testing storage behavior, monitoring disk performance, or ensuring specific storage conditions are met.

## Features

- Write data (zeroed or random) until a specific size or disk full.
- Configurable block sizes for write operations (default: 32MB).
- Status updates with progress, throughput, and estimated time remaining (ETA).
- Removes the temporary file automatically upon completion or interruption.
- Supports man pages for command-line documentation.

## Installation

### Building from Source

You can build `fillfs` using the provided `Makefile`:

1. Clone this repository:
   ```bash
   git clone https://github.com/robheffo79/fillfs.git
   cd fillfs
   ```

2. Build the binary:
   ```bash
   make
   ```

3. Install the binary and man page (requires root or sudo):
   ```bash
   sudo make install
   ```

### Using as a Snap Package

If you prefer, you can use the provided Snap configuration to build and install `fillfs` as a Snap package:

1. Build the snap:
   ```bash
   snapcraft
   ```

2. Install the snap locally (requires root or sudo):
   ```bash
   sudo snap install fillfs_1.0_amd64.snap --dangerous
   ```

3. Run `fillfs`:
   ```bash
   fillfs --help
   ```

## Usage

### Command-line Options

- `-r`, `--random`: Write random data instead of zeroed data.
- `-z`, `--zero`: Explicitly write zeroed data (overrides `--random` if both are used).
- `-s`, `--status`: Show periodic status updates, including throughput and ETA.
- `-b`, `--block-size=SIZE`: Use a custom block size for writes. Defaults to 32M. Must be a valid size (e.g., `1K`, `32M`, `1G`).
- `-h`, `--help`: Display help and exit.

### Examples

1. Fill 1 GB on the root filesystem, showing progress:
   ```bash
   fillfs / --status 1G
   ```

2. Fill until the disk is completely full at `/mnt/data` with zero data:
   ```bash
   fillfs /mnt/data
   ```

3. Use random data for 10 GB with a custom 4M block size:
   ```bash
   fillfs -r -s -b 4M /mnt/data 10G
   ```

## Uninstallation

To remove the binary and man page:

1. If installed via `make`:
   ```bash
   sudo make uninstall
   ```

2. If installed as a Snap package:
   ```bash
   sudo snap remove fillfs
   ```

## Contributing

Contributions are welcome! If you encounter any issues or have feature requests, feel free to open an issue or submit a pull request.

## License

This project is licensed under the MIT License. See the `LICENSE` file for details.

## Author

**Robert Heffernan**  
Email: [robert@heffernantech.au](mailto:robert@heffernantech.au)
