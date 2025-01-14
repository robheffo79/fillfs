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

#define _GNU_SOURCE         /* Ensure 'syscall' and others get declared on Linux */
#define _DEFAULT_SOURCE     /* Helps ensure 'usleep' is declared (or you could use _XOPEN_SOURCE) */
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>    // for SIZE_MAX
#include <fcntl.h>     // for O_WRONLY, O_CREAT, O_TRUNC, etc.
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>    // for close, unlink, etc.
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <getopt.h>
#include <ctype.h>
#include <signal.h>
#include <sys/statvfs.h>
#include <limits.h>    // for PATH_MAX
#include <pthread.h>   // for pthread_create, pthread_join, etc.

#ifdef __linux__      // For ioprio_set (Linux only)
#include <sys/syscall.h>
#include <linux/ioprio.h>
#endif

#include <sys/resource.h> // for setpriority, PRIO_PROCESS

#ifndef MAX_FILENAME_LENGTH
#define MAX_FILENAME_LENGTH 1024
#endif

#define FILLFS_FILE_NAME "/.fillfs"

/*
 * Global filename for hidden-file usage if target is a directory.
 * If the user passed an actual file, we won't use/unlink g_hidden_filename.
 */
static char g_hidden_filename[MAX_FILENAME_LENGTH] = {0};

/**
 * @brief Cleans up the hidden file (if it was used) and optionally exits.
 *
 * @param success 0 for success exit code, nonzero for error exit code.
 */
static void clean_exit(int success) {
    /*
     * We only want to unlink if we actually created a hidden file.
     * If user gave us a regular file, that means we won't have used g_hidden_filename,
     * and hence we won't unlink.
     */
    if (g_hidden_filename[0] != '\0') {
        unlink(g_hidden_filename);
    }
    exit(success);
}

/**
 * @brief atexit handler so that if the program exits unexpectedly,
 *        we remove the temporary file (only if we used one).
 */
static void exit_handler(void) {
    if (g_hidden_filename[0] != '\0') {
        unlink(g_hidden_filename);
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
 * @brief Generate full path for the fill file in the provided directory.
 *
 * @param path         The buffer to store the resulting filename.
 * @param mount_point  The directory path where the file will be created.
 */
static void generate_file_path(char *path, const char *mount_point) {
    snprintf(path, MAX_FILENAME_LENGTH, "%s%s", mount_point, FILLFS_FILE_NAME);
}

#ifdef __linux__
/**
 * @brief Attempt to set the I/O priority of the current thread to "idle" class.
 *        This is Linux-specific and requires the ioprio_set syscall.
 */
static void set_io_priority_idle(void) {
    // ioprio_set(ioprio_which = 1 for PRIO_PROCESS, who = 0 for current,
    // ioprio = IOPRIO_PRIO_VALUE(IOPRIO_CLASS_IDLE, 7))
    if (syscall(SYS_ioprio_set, 1, 0, IOPRIO_PRIO_VALUE(IOPRIO_CLASS_IDLE, 7)) == -1) {
        perror("ioprio_set");
        // If this fails, we silently ignore or fallback to CPU nice.
    }
}
#endif

/**
 * @brief Struct for passing arguments & tracking progress between threads.
 */
typedef struct {
    const char *filename;       ///< Path to file to fill/overwrite
    size_t      file_size;      ///< Desired size in bytes (or min with file if existing)
    size_t      block_size;     ///< Write in these chunks
    int         use_random;     ///< 1 if random, 0 if not
    int         use_zero;       ///< 1 if zero, overrides random
    size_t      known_free_space; ///< For better progress calc if file_size == SIZE_MAX
    int         existing_file;  ///< 1 if user gave us an existing file, 0 if hidden-file

    volatile size_t total_written; ///< Shared progress: how many bytes have been written
    volatile int    done;          ///< 1 when writer thread finishes
    volatile int    error;         ///< Non-zero if error
} fill_thread_args_t;

/**
 * @brief Thread function that fills (or overwrites) the file until file_size is reached or ENOSPC.
 *
 * @param arg Pointer to fill_thread_args_t containing all parameters and shared progress.
 * @return void* Not used. Thread sets arg->done and arg->error internally.
 */
static void* fill_file_thread(void *arg) {
    fill_thread_args_t *params = (fill_thread_args_t*)arg;
    int fd = -1;
    void *buffer = NULL;
    ssize_t bytes_written;
    size_t total_written_local = 0;

    // Lower CPU priority:
    setpriority(PRIO_PROCESS, 0, 19); // NICENESS=19 => lowest CPU scheduling priority

#ifdef __linux__
    // Also try to set I/O priority to idle class on Linux:
    set_io_priority_idle();
#endif

    // Allocate buffer
    buffer = malloc(params->block_size);
    if (!buffer) {
        perror("malloc");
        params->error = 1;
        params->done  = 1;
        pthread_exit(NULL);
    }

    // Fill buffer with either zeros or random data
    if (params->use_zero) {
        memset(buffer, 0, params->block_size);
    }
    else if (params->use_random) {
        srand((unsigned int)time(NULL));
        for (size_t i = 0; i < params->block_size; ++i) {
            ((unsigned char*)buffer)[i] = (unsigned char)(rand() % 256);
        }
    }
    else {
        // Default to zero if neither random nor zero is specified
        memset(buffer, 0, params->block_size);
    }

    /*
     * If it's an existing file, open for writing but do NOT truncate,
     * because we only want to overwrite. If it's a hidden file in a directory,
     * we can create/truncate as usual.
     */
    int open_flags = 0;
    if (params->existing_file) {
        // Overwrite existing file. No O_TRUNC => we won't shrink it on open.
        open_flags = O_WRONLY;
    } else {
        // If it's a newly created hidden file, we do O_CREAT | O_TRUNC
        open_flags = O_WRONLY | O_CREAT | O_TRUNC;
    }

    fd = open(params->filename, open_flags, 0666);
    if (fd == -1) {
        perror("open");
        free(buffer);
        params->error = 1;
        params->done  = 1;
        pthread_exit(NULL);
    }

    // Perform writes in a loop
    while (total_written_local < params->file_size) {
        // Compute how much we can write in this iteration (avoid overshooting)
        size_t bytes_to_write = params->block_size;
        size_t remaining      = params->file_size - total_written_local;
        if (remaining < params->block_size) {
            bytes_to_write = remaining;
        }

        bytes_written = write(fd, buffer, bytes_to_write);
        if (bytes_written == -1) {
            if (errno == ENOSPC) {
                // Disk is full
                break;  // done writing
            }
            perror("write");
            params->error = 1;
            break;
        }

        total_written_local   += (size_t)bytes_written;
        params->total_written = total_written_local; // update shared progress

        // If we reached the target file size
        if (total_written_local >= params->file_size) {
            break;
        }
    }

    // Flush
    if (fsync(fd) == -1) {
        perror("fsync");
        params->error = 1;
    }

    close(fd);
    free(buffer);

    // Mark done
    params->done = 1;
    pthread_exit(NULL);
}

/**
 * @brief Show usage message for the program.
 *
 * @param prog_name The name of the program (argv[0]).
 */
static void show_help(const char *prog_name) {
    /*
     * We have 7 '%s' placeholders in the format string,
     * so we must pass 7 times 'prog_name' at the end.
     */
    fprintf(stderr,
        "Usage: %s [OPTIONS] <mount_point_or_file> [size]\n\n"
        "Arguments:\n"
        "  <mount_point_or_file>   Either:\n"
        "     - a directory: create /.fillfs in that directory.\n"
        "     - an existing file: overwrite up to [size] or to its own size.\n\n"
        "  [size]          Optional. If omitted, fill until the disk is full (dir case),\n"
        "                  or overwrite the entire existing file (file case).\n"
        "                  Supports suffixes: K, M, G, T, P, E, Z, Y.\n\n"
        "Options:\n"
        "  -r, --random           Write random data.\n"
        "  -z, --zero             Write zero data (overrides --random if both set).\n"
        "  -s, --status           Show progress (throughput, ETA, etc.).\n"
        "  -b, --block-size=SIZE  Set the write block size. Defaults to 32M if not specified.\n"
        "  -h, --help             Display this help message and exit.\n\n"
        "Examples:\n"
        "  %s / --status 1G\n"
        "  %s /mnt/data\n"
        "  %s -r -s /mnt/data 1G\n"
        "  %s --block-size=32M /mnt/data 2G\n"
        "  %s /tmp/existing_file\n"
        "  %s /tmp/existing_file 500M\n\n",
        prog_name, prog_name, prog_name, prog_name, prog_name, prog_name, prog_name
    );
}

int main(int argc, char *argv[]) {
    // Install cleanup for hidden-file scenario
    atexit(exit_handler);

    // Register signals
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
#ifdef SIGHUP
    signal(SIGHUP,  signal_handler);
#endif

    // Default settings
    int    use_random       = 0;
    int    use_zero         = 0;
    int    show_status      = 0;
    size_t file_size        = SIZE_MAX;  // fill until full by default (dir scenario)
    size_t block_size       = 0;         // will default to 32M if not specified
    size_t known_free_space = 0;         // helps with ETA if user doesn't specify size

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
        fprintf(stderr, "Error: Missing <mount_point_or_file> argument.\n");
        show_help(argv[0]);
        return 1;
    }

    const char *path_arg = argv[optind++]; // This might be a directory or an existing file

    // If there's another arg, treat it as the size
    if (optind < argc) {
        file_size = parse_size(argv[optind]);
    }

    // Default block_size: 32 MB
    if (block_size == 0) {
        block_size = parse_size("32M");
    }

    /*
     * Detect if 'path_arg' is a directory or a file.
     * We'll use stat. If S_ISDIR -> directory, if S_ISREG -> file, etc.
     */
    struct stat st;
    memset(&st, 0, sizeof(st));
    if (stat(path_arg, &st) == -1) {
        perror("stat");
        return 1;
    }

    int is_directory = S_ISDIR(st.st_mode);
    int is_reg_file  = S_ISREG(st.st_mode);

    /*
     * We'll prepare our fill_thread_args_t accordingly:
     *   - If it's a directory, we generate the hidden file path, and we fill until file_size or disk full.
     *   - If it's an existing file, we do NOT remove it on exit, and we only write up to the user-specified size
     *     or the file's own size if user didn't specify or specified something bigger than the file.
     */
    fill_thread_args_t args;
    memset(&args, 0, sizeof(args));

    args.use_random       = use_random;
    args.use_zero         = use_zero;
    args.block_size       = block_size;
    args.total_written    = 0;
    args.done             = 0;
    args.error            = 0;

    if (is_directory) {
        // For directory scenario:
        generate_file_path(g_hidden_filename, path_arg);

        // If file_size == SIZE_MAX, try to get free space from the directory
        if (file_size == SIZE_MAX) {
            struct statvfs fs_info;
            if (statvfs(path_arg, &fs_info) == 0) {
                known_free_space = (size_t)fs_info.f_bavail * fs_info.f_bsize;
            }
        }

        args.filename         = g_hidden_filename;
        args.file_size        = file_size;  // Could be SIZE_MAX
        args.known_free_space = known_free_space;
        args.existing_file    = 0; // We'll remove it on exit
    }
    else if (is_reg_file) {
        /*
         * We have an existing file. We do not remove it upon exit.
         * If user gave a size == SIZE_MAX => overwrite the entire file.
         * Otherwise, if user gave a smaller size, only overwrite that portion.
         * If user gave a bigger size than the file, we only overwrite the file's size
         * (since we won't expand the file in this scenario).
         */
        size_t file_actual_size = (size_t)st.st_size;
        size_t final_size = 0;

        if (file_size == SIZE_MAX) {
            // Overwrite the entire existing file
            final_size = file_actual_size;
        } else {
            // Overwrite up to min(file_size, file_actual_size)
            final_size = (file_size < file_actual_size) ? file_size : file_actual_size;
        }

        args.filename         = path_arg;
        args.file_size        = final_size;
        args.known_free_space = 0; // Not used for file scenario
        args.existing_file    = 1; // We won't remove it on exit
    }
    else {
        fprintf(stderr, "Error: '%s' is neither a directory nor a regular file.\n", path_arg);
        return 1;
    }

    // Create background writer thread
    pthread_t writer_thread;
    if (pthread_create(&writer_thread, NULL, fill_file_thread, &args) != 0) {
        perror("pthread_create");
        // If we fail to create the thread, clean up if we created a hidden file:
        if (is_directory) {
            clean_exit(EXIT_FAILURE);
        } else {
            return 1;
        }
    }

    // If showing status, do it in the foreground
    struct timespec start_time, current_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    double filtered_throughput_mb_s = 0.0;
    const double alpha = 0.2;
    double last_print_time = 0.0;

    while (!args.done) {
        if (show_status) {
            // Print status ~ once per second
            clock_gettime(CLOCK_MONOTONIC, &current_time);
            double elapsed_sec = (current_time.tv_sec - start_time.tv_sec) +
                                 (current_time.tv_nsec - start_time.tv_nsec) / 1e9;

            if (elapsed_sec - last_print_time >= 1.0) {
                last_print_time = elapsed_sec;

                size_t tw = args.total_written;
                double written_mb = tw / (1024.0 * 1024.0);

                double instantaneous_throughput = (written_mb / elapsed_sec);
                if (filtered_throughput_mb_s < 1e-9) {
                    filtered_throughput_mb_s = instantaneous_throughput;
                } else {
                    filtered_throughput_mb_s =
                        alpha * instantaneous_throughput +
                        (1.0 - alpha) * filtered_throughput_mb_s;
                }

                double tput = filtered_throughput_mb_s;

                if (is_directory && args.file_size == SIZE_MAX) {
                    // Possibly indefinite or limited by known_free_space
                    double progress_percent = 0.0;
                    if (args.known_free_space > 0) {
                        progress_percent =
                            (100.0 * (double)tw) / (double)args.known_free_space;
                        if (progress_percent > 100.0) {
                            progress_percent = 100.0;
                        }
                    }
                    double remaining_bytes = 0.0;
                    if (args.known_free_space > tw) {
                        remaining_bytes = (double)args.known_free_space - (double)tw;
                    }
                    double est_time_sec = (tput > 0.0)
                        ? (remaining_bytes / (1024.0 * 1024.0)) / tput
                        : 0.0;

                    int total_seconds = (int)(est_time_sec + 0.5);
                    int eta_h = total_seconds / 3600;
                    int remainder = total_seconds % 3600;
                    int eta_m = remainder / 60;
                    int eta_s = remainder % 60;

                    fprintf(stdout,
                            "\rProgress: %.2f%% | Written: %.2f / %.2f MB | "
                            "Throughput: %.2f MB/s | ETA: %02d:%02d:%02d ",
                            progress_percent,
                            written_mb,
                            args.known_free_space / (1024.0 * 1024.0),
                            tput,
                            eta_h, eta_m, eta_s);
                }
                else {
                    // We have a definite size to fill
                    double progress_percent = 0.0;
                    if (args.file_size > 0) {
                        progress_percent =
                            (100.0 * (double)tw) / (double)args.file_size;
                        if (progress_percent > 100.0) {
                            progress_percent = 100.0;
                        }
                    }
                    double remaining_bytes =
                        (double)args.file_size - (double)tw;
                    double est_time_sec = (tput > 0.0)
                        ? (remaining_bytes / (1024.0 * 1024.0)) / tput
                        : 0.0;

                    int total_seconds = (int)(est_time_sec + 0.5);
                    int eta_h = total_seconds / 3600;
                    int remainder = total_seconds % 3600;
                    int eta_m = remainder / 60;
                    int eta_s = remainder % 60;

                    fprintf(stdout,
                            "\rProgress: %.2f%% | Written: %.2f / %.2f MB | "
                            "Throughput: %.2f MB/s | ETA: %02d:%02d:%02d ",
                            progress_percent,
                            written_mb,
                            (double)args.file_size / (1024.0 * 1024.0),
                            tput,
                            eta_h, eta_m, eta_s);
                }
                fflush(stdout);
            }
        }

        // Sleep a bit to avoid busy waiting
        usleep(200000); // 200 ms
    }

    // Wait for the writer thread to join
    pthread_join(writer_thread, NULL);

    // If we were showing status, print final summary
    if (show_status) {
        fprintf(stdout, "\rProgress: 100.00%% (finalizing)\n");

        clock_gettime(CLOCK_MONOTONIC, &current_time);
        double total_elapsed = (current_time.tv_sec - start_time.tv_sec) +
                              (current_time.tv_nsec - start_time.tv_nsec) / 1e9;
        double total_mb = (double)args.total_written / (1024.0 * 1024.0);

        double final_throughput = (total_elapsed > 0.0)
                                  ? (total_mb / total_elapsed)
                                  : 0.0;

        fprintf(stdout,
                "Fill/Overwrite complete.\n"
                "Wrote: %.2f MB in %.2f seconds (avg throughput: %.2f MB/s)\n",
                total_mb, total_elapsed, final_throughput);
    }

    // If the writer thread reported an error, exit with failure
    if (args.error) {
        clean_exit(EXIT_FAILURE);
    } else {
        // Otherwise success. If it's a directory, hidden file is unlinked in clean_exit().
        // If it's a file, we skip the unlink.
        clean_exit(EXIT_SUCCESS);
    }

    return 0; // Not reached
}
