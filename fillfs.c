/*
 * fillfs.c
 *
 * Copyright (c) 2025 Robert Heffernan
 * 
 * Author: Robert Heffernan <robert@heffernantech.au>
 * Date: 14 January 2025
 *
 * This file is part of the fillfs utility. It is licensed under the MIT License:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this
 * software and associated documentation files (the “Software”), to deal in the Software
 * without restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
 * IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stdint.h>   // for SIZE_MAX
#include <fcntl.h>    // for O_WRONLY, O_CREAT, O_TRUNC, etc.
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>   // for close, unlink, etc.
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <getopt.h>
#include <ctype.h>
#include <signal.h>
#include <sys/statvfs.h>
#include <limits.h>   // for PATH_MAX, etc.

/* Because we want to handle large data sizes. */
#ifndef MAX_FILENAME_LENGTH
#define MAX_FILENAME_LENGTH 1024
#endif

#define FILLFS_FILE_NAME "/.fillfs"

// Global so we can clean up from signal handlers & at exit
static char g_filename[MAX_FILENAME_LENGTH] = {0};

/**
 * @brief Parse a human-readable size string (e.g., 800K, 32M, 10G, etc.) into bytes.
 *
 * @param size_str Input string representing the size (with optional suffix).
 * @return size_t The size in bytes.
 */
static size_t parse_size(const char *size_str) {
    size_t size = 0;
    char suffix = 0;
    char *endptr = NULL;

    size = strtoull(size_str, &endptr, 10);

    if (endptr && *endptr) {
        suffix = (char)tolower((unsigned char)*endptr);
    }

    switch (suffix) {
        case 'k':  // Kilobytes (1024 bytes)
            size *= 1024ULL;
            break;
        case 'm':  // Megabytes (1024 * 1024)
            size *= 1024ULL * 1024ULL;
            break;
        case 'g':  // Gigabytes (1024 * 1024 * 1024)
            size *= 1024ULL * 1024ULL * 1024ULL;
            break;
        case 't':  // Terabytes
            size *= 1024ULL * 1024ULL * 1024ULL * 1024ULL;
            break;
        case 'p':  // Petabytes
            size *= 1024ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL;
            break;
        case 'e':  // Exabytes
            size *= 1024ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL;
            break;
        case 'z':  // Zettabytes
            size *= 1024ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL;
            break;
        case 'y':  // Yottabytes
            size *= 1024ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL;
            break;
        case 0:    // No suffix provided; assume bytes
            break;
        default:
            fprintf(stderr,
                    "Error: Invalid size suffix '%c'. Supported: K, M, G, T, P, E, Z, Y.\n",
                    suffix);
            exit(EXIT_FAILURE);
    }

    return size;
}

/**
 * @brief Cleans up the hidden file (if it exists) and optionally exits.
 *
 * @param success If 0, success exit; otherwise error exit.
 */
static void clean_exit(int success) {
    if (g_filename[0] != '\0') {
        unlink(g_filename);
    }
    exit(success);
}

/**
 * @brief atexit handler so that if the program exits unexpectedly,
 *        we remove the temporary file.
 */
static void exit_handler(void) {
    if (g_filename[0] != '\0') {
        unlink(g_filename);
    }
}

/**
 * @brief Signal handler for common signals to ensure cleanup.
 *
 * @param signum The signal number caught.
 */
static void signal_handler(int signum) {
    fprintf(stderr, "\nCaught signal %d. Cleaning up...\n", signum);
    clean_exit(EXIT_FAILURE);
}

/**
 * @brief Generate full path for the fill file in the provided mount point.
 *
 * @param path The buffer to store the resulting filename.
 * @param mount_point The mount point directory where the file will be created.
 */
static void generate_file_path(char *path, const char *mount_point) {
    snprintf(path, MAX_FILENAME_LENGTH, "%s%s", mount_point, FILLFS_FILE_NAME);
}

/**
 * @brief Fill the file with data until `file_size` is reached or disk is full.
 *
 * @param filename         Path to the file to fill.
 * @param file_size        Desired size in bytes (or SIZE_MAX to fill to ENOSPC).
 * @param block_size       Size of each write operation in bytes.
 * @param use_random       Non-zero if random data is to be written.
 * @param use_zero         Non-zero if zero data is explicitly chosen (overrides random if both set).
 * @param show_status      Non-zero if progress, throughput, and ETA should be displayed.
 * @param known_free_space For ETA if file_size==SIZE_MAX (fill-until-full).
 */
static void fill_file(const char *filename,
                      size_t file_size,
                      size_t block_size,
                      int use_random,
                      int use_zero,
                      int show_status,
                      size_t known_free_space)
{
    int fd = -1;
    void *buffer = NULL;
    ssize_t bytes_written;
    size_t total_written = 0;

    // For throughput calculations
    struct timespec start_time, current_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    // Keep track of last stats print time
    double last_print_time = 0.0;
    // Keep a "filtered" throughput (exponential moving average)
    double filtered_throughput_mb_s = 0.0;
    const double alpha = 0.2;  // smoothing factor

    // Keep track of flush times (once per 60 seconds)
    double last_flush_time = 0.0;
    const double flush_interval = 60.0;  // flush once per 60s

    // Allocate the buffer
    buffer = malloc(block_size);
    if (!buffer) {
        perror("malloc");
        clean_exit(EXIT_FAILURE);
    }

    // Initialize buffer data
    if (use_zero) {
        memset(buffer, 0, block_size);
    } else if (use_random) {
        srand((unsigned int)time(NULL));
        for (size_t i = 0; i < block_size; ++i) {
            ((unsigned char*)buffer)[i] = (unsigned char)(rand() % 256);
        }
    } else {
        // Default to zero if neither is specified
        memset(buffer, 0, block_size);
    }

    // Open the file normally (no O_DIRECT)
    fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd == -1) {
        perror("open");
        free(buffer);
        clean_exit(EXIT_FAILURE);
    }

    // Write until specified size is reached or disk is full
    while (total_written < file_size) {
        bytes_written = write(fd, buffer, block_size);
        if (bytes_written == -1) {
            if (errno == ENOSPC) {
                // Disk full
                if (show_status) {
                    fprintf(stderr, "\nDisk is full. Stopping...\n");
                    // Print final 100% progress if known_free_space was used
                    if (file_size == SIZE_MAX && known_free_space > 0) {
                        fprintf(stdout, "\rProgress: 100.00%% (disk full)        \n");
                    } else if (file_size != SIZE_MAX) {
                        fprintf(stdout, "\rProgress: 100.00%% (disk full)        \n");
                    }
                }
                // Final flush before exit
                if (fsync(fd) == -1) {
                    perror("fsync");
                }
                free(buffer);
                clean_exit(EXIT_SUCCESS);
            }
            perror("write");
            free(buffer);
            clean_exit(EXIT_FAILURE);
        }

        total_written += (size_t)bytes_written;

        // Check if it's time to flush (once per minute)
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        double elapsed_sec_total = (current_time.tv_sec - start_time.tv_sec) +
                                   (current_time.tv_nsec - start_time.tv_nsec) / 1e9;

        double elapsed_sec_since_flush = elapsed_sec_total - last_flush_time;
        if (elapsed_sec_since_flush >= flush_interval) {
            if (fsync(fd) == -1) {
                perror("fsync");
                free(buffer);
                clean_exit(EXIT_FAILURE);
            }
            last_flush_time = elapsed_sec_total;
        }

        // Periodically show progress (once per second by default)
        if (show_status) {
            double elapsed_sec_since_start = elapsed_sec_total;
            if (elapsed_sec_since_start - last_print_time >= 1.0) {
                last_print_time = elapsed_sec_since_start;

                // Instantaneous throughput
                double instantaneous_throughput_mb_s =
                    (total_written / (1024.0 * 1024.0)) / elapsed_sec_since_start;

                // Update the smoothed throughput (EMA)
                if (filtered_throughput_mb_s < 1e-9) {
                    filtered_throughput_mb_s = instantaneous_throughput_mb_s;
                } else {
                    filtered_throughput_mb_s =
                        alpha * instantaneous_throughput_mb_s +
                        (1.0 - alpha) * filtered_throughput_mb_s;
                }

                double throughput_mb_s = filtered_throughput_mb_s;

                // If filling until disk full
                if (file_size == SIZE_MAX) {
                    double progress_percent = 0.0;
                    if (known_free_space > 0) {
                        progress_percent =
                            (double)total_written / (double)known_free_space * 100.0;
                        if (progress_percent > 100.0) {
                            progress_percent = 100.0;
                        }
                    }

                    // Calculate ETA
                    double remaining_bytes =
                        (known_free_space > total_written)
                           ? (double)known_free_space - (double)total_written
                           : 0.0;
                    double estimated_time_sec = 0.0;
                    if (throughput_mb_s > 0.0) {
                        estimated_time_sec =
                            (remaining_bytes / (1024.0 * 1024.0)) / throughput_mb_s;
                    }

                    // ROUND the total seconds
                    int total_seconds = (int)(estimated_time_sec + 0.5);
                    int eta_h = total_seconds / 3600;
                    int remainder = total_seconds % 3600;
                    int eta_m = remainder / 60;
                    int eta_s = remainder % 60;

                    fprintf(stdout,
                            "\rProgress: %.2f%% | Written: %.2f / %.2f MB | "
                            "Throughput: %.2f MB/s | ETA: %02d:%02d:%02d ",
                            progress_percent,
                            total_written / (1024.0 * 1024.0),
                            known_free_space / (1024.0 * 1024.0),
                            throughput_mb_s,
                            eta_h, eta_m, eta_s);
                } else {
                    // Definite target
                    double progress_percent =
                        (double)total_written / (double)file_size * 100.0;
                    if (progress_percent > 100.0) {
                        progress_percent = 100.0;
                    }

                    double remaining_bytes =
                        (double)file_size - (double)total_written;
                    double estimated_time_sec = 0.0;
                    if (throughput_mb_s > 0.0) {
                        estimated_time_sec =
                            (remaining_bytes / (1024.0 * 1024.0)) / throughput_mb_s;
                    }

                    // ROUND the total seconds
                    int total_seconds = (int)(estimated_time_sec + 0.5);
                    int eta_h = total_seconds / 3600;
                    int remainder = total_seconds % 3600;
                    int eta_m = remainder / 60;
                    int eta_s = remainder % 60;

                    fprintf(stdout,
                            "\rProgress: %.2f%% | Written: %.2f/%.2f MB | "
                            "Throughput: %.2f MB/s | ETA: %02d:%02d:%02d ",
                            progress_percent,
                            total_written / (1024.0 * 1024.0),
                            (double)file_size / (1024.0 * 1024.0),
                            throughput_mb_s,
                            eta_h, eta_m, eta_s);
                }
                fflush(stdout);
            }
        }

        if (total_written >= file_size) {
            break;
        }
    }

    // Final flush to disk
    if (fsync(fd) == -1) {
        perror("fsync");
        free(buffer);
        clean_exit(EXIT_FAILURE);
    }

    if (show_status) {
        // Show final 100% progress line
        fprintf(stdout, "\rProgress: 100.00%% | Written: ... (finalizing)\n");

        // Final stats
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        double total_elapsed = (current_time.tv_sec - start_time.tv_sec) +
                              (current_time.tv_nsec - start_time.tv_nsec) / 1e9;
        double total_mb = (double)total_written / (1024.0 * 1024.0);

        double final_throughput = (total_elapsed > 0.0)
                                      ? (total_mb / total_elapsed)
                                      : 0.0;

        fprintf(stdout,
                "Fill complete.\n"
                "Wrote: %.2f MB in %.2f seconds (avg throughput: %.2f MB/s)\n",
                total_mb, total_elapsed, final_throughput);
    }

    free(buffer);
    close(fd);
    clean_exit(EXIT_SUCCESS); // Graceful exit
}

/**
 * @brief Show usage message for the program.
 *
 * @param prog_name The name of the program (argv[0]).
 */
static void show_help(const char *prog_name) {
    fprintf(stderr,
        "Usage: %s [OPTIONS] <mount_point> [size]\n\n"
        "Arguments:\n"
        "  <mount_point>   The mount point where /.fillfs file will be created.\n"
        "  [size]          Optional. If omitted, fill until the disk is full.\n"
        "                  Supports suffixes: K, M, G, T, P, E, Z, Y.\n\n"
        "Options:\n"
        "  -r, --random           Write random data.\n"
        "  -z, --zero             Write zero data (overrides --random if both set).\n"
        "  -s, --status           Show progress (throughput, ETA, etc.).\n"
        "  -b, --block-size=SIZE  Set the write block size. Defaults to 32M if not specified.\n"
        "  -h, --help             Display this help message and exit.\n\n"
        "Examples:\n"
        "  %s / --status 1G                Fill up 1 GB on root filesystem, showing progress.\n"
        "  %s /mnt/data                    Fill /mnt/data until full with zeroes.\n"
        "  %s -r -s /mnt/data 1G           Fill 1 GB with random data, show status.\n"
        "  %s --block-size=32M /mnt/data 2G   Use 32M blocks, fill 2 GB.\n\n",
        prog_name, prog_name, prog_name, prog_name, prog_name
    );
}

/**
 * @brief Main function for fillfs.
 */
int main(int argc, char *argv[]) {
    // Install cleanup on exit
    atexit(exit_handler);

    // Register signals to ensure cleanup on interruption
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
#ifdef SIGHUP
    signal(SIGHUP,  signal_handler);
#endif

    // Default settings
    int   use_random       = 0;
    int   use_zero         = 0;
    int   show_status      = 0;
    size_t file_size       = SIZE_MAX;  // If not specified, fill until full
    size_t block_size      = 0;         // We'll default to 32M if not specified
    size_t known_free_space = 0;        // For ETA if user doesn't specify a size

    static struct option long_opts[] = {
        {"random",      no_argument,       0, 'r'},
        {"zero",        no_argument,       0, 'z'},
        {"status",      no_argument,       0, 's'},
        {"help",        no_argument,       0, 'h'},
        {"block-size",  required_argument, 0, 'b'},
        {0, 0, 0, 0}
    };

    while (1) {
        int opt_index = 0;
        int c = getopt_long(argc, argv, "rzshb:", long_opts, &opt_index);
        if (c == -1) {
            break;
        }
        switch (c) {
            case 'r':
                use_random = 1;
                break;
            case 'z':
                use_zero = 1;
                break;
            case 's':
                show_status = 1;
                break;
            case 'h':
                show_help(argv[0]);
                return 0;
            case 'b':
                block_size = parse_size(optarg);
                if (block_size == 0) {
                    fprintf(stderr, "Error: Invalid block size.\n");
                    return 1;
                }
                break;
            default:
                show_help(argv[0]);
                return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "Error: Missing mount_point argument.\n");
        show_help(argv[0]);
        return 1;
    }

    const char *mount_point = argv[optind++];

    // If there's a trailing arg, treat it as the size
    if (optind < argc) {
        file_size = parse_size(argv[optind]);
    }

    // Default block_size if not specified: 32 MB
    if (block_size == 0) {
        block_size = parse_size("32M");
    }

    // If size was not specified, attempt to get free space from mount point
    if (file_size == SIZE_MAX) {
        struct statvfs fs_info;
        if (statvfs(mount_point, &fs_info) == 0) {
            known_free_space = (size_t)fs_info.f_bavail * fs_info.f_bsize;
        }
    }

    // Generate the path for the hidden file
    generate_file_path(g_filename, mount_point);

    fill_file(
        g_filename,
        file_size,
        block_size,
        use_random,
        use_zero,
        show_status,
        known_free_space
    );

    // If we get here for some reason (should not in normal flow), exit gracefully
    clean_exit(EXIT_SUCCESS);
    return 0;
}
