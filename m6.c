/*
    m6.c -- Xander Pickering (3118504) -- Last updated 2024/12/08
    A program that takes in two input files containing binary-formatted matrices
    with 32-bit integer values, and optionally outputs the result as binary into another file.

    Utilizes multithreading for performance gains.
*/

#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>

/* Struct to pass arguments to worker threads. */
typedef struct
{
    int *A;
    int *B;
    int *C;
    int dimension;
    int start_row; // Start row of matrix for this thread
    int end_row;   // End row
} thread_args_t;

/* Prints usage instructions and exits. */
void usage(const char *progname)
{
    fprintf(stderr, "Usage: %s <input1.dat> <input2.dat> -t <num_threads> -o <output_file>\n", progname);
    fprintf(stderr, "  <input1.dat>   The first input file (required)\n");
    fprintf(stderr, "  <input2.dat>   The second input file (required)\n");
    fprintf(stderr, "  -t <threads>   Number of threads to use (required)\n");
    fprintf(stderr, "  -o <filename>  Output file name (optional, if not specified output is discarded)\n");
    exit(EXIT_FAILURE);
}

/* Worker thread function to multiply rows of the matrices. */
static void *multiply_rows(void *arg)
{
    thread_args_t *args = (thread_args_t *)arg;
    int *A = args->A;
    int *B = args->B;
    int *C = args->C;
    int dim = args->dimension;

    // Each thread processes a range of rows:
    for (int r = args->start_row; r < args->end_row; r++)
    {
        for (int c = 0; c < dim; c++)
        {
            long sum = 0;
            for (int k = 0; k < dim; k++)
            {
                sum += (long)A[r * dim + k] * (long)B[k * dim + c];
            }
            C[r * dim + c] = (int)sum; // Stores the result in the output matrix
        }
    }

    return NULL;
}

int main(int argc, char *argv[])
{
    // Init variables for I/O handling and arguments
    int opt;
    int threads = -1;         // Number of threads to use
    char *output_file = NULL; // Optional output file name
    char *input_file1 = NULL; // Input matrix file 1
    char *input_file2 = NULL; // Input matrix file 2

    /* ARGUMENT PARSING */
    if (argc < 4)
    {
        usage(argv[0]);
    }

    input_file1 = argv[1];
    input_file2 = argv[2];

    argc -= 2;
    argv += 2;

    // Parses command-line arguments for thread count and output file
    while ((opt = getopt(argc, argv, "t:o:")) != -1)
    {
        switch (opt)
        {
        case 't':
            threads = atoi(optarg); // Converts the thread count to an integer
            break;
        case 'o':
            output_file = optarg;
            break;
        default:
            usage(argv[0]);
        }
    }

    if (!input_file1 || !input_file2 || threads <= 0)
    {
        usage(argv[-(optind - 2)]);
    }

    /* FILE HANDLING */
    int fd1 = open(input_file1, O_RDONLY);
    if (fd1 < 0)
    {
        fprintf(stderr, "error: could not open file %s: %s\n", input_file1, strerror(errno));
        exit(EXIT_FAILURE);
    }

    int fd2 = open(input_file2, O_RDONLY);
    if (fd2 < 0)
    {
        fprintf(stderr, "error: could not open file %s: %s\n", input_file2, strerror(errno));
        close(fd1);
        exit(EXIT_FAILURE);
    }

    /* FILE SIZE VALIDATION */
    // Ensures files represent square matrices of 32-bit integers
    struct stat st1, st2;
    if (fstat(fd1, &st1) < 0)
    {
        fprintf(stderr, "error: could not stat file %s: %s\n", input_file1, strerror(errno));
        close(fd1);
        close(fd2);
        exit(EXIT_FAILURE);
    }
    if (fstat(fd2, &st2) < 0)
    {
        fprintf(stderr, "error: could not stat file %s: %s\n", input_file2, strerror(errno));
        close(fd1);
        close(fd2);
        exit(EXIT_FAILURE);
    }

    size_t size1 = st1.st_size;
    size_t size2 = st2.st_size;

    if (size1 % sizeof(int) != 0 || size2 % sizeof(int) != 0)
    {
        fprintf(stderr, "error: file sizes are not multiples of 4 bytes\n");
        close(fd1);
        close(fd2);
        exit(EXIT_FAILURE);
    }

    size_t elements1 = size1 / sizeof(int);
    size_t elements2 = size2 / sizeof(int);

    int dimension1 = (int)floor(sqrt((double)elements1));
    if ((size_t)(dimension1 * dimension1) != elements1)
    {
        fprintf(stderr, "error: first matrix is not a perfect square in size\n");
        close(fd1);
        close(fd2);
        exit(EXIT_FAILURE);
    }

    int dimension2 = (int)floor(sqrt((double)elements2));
    if ((size_t)(dimension2 * dimension2) != elements2)
    {
        fprintf(stderr, "error: second matrix is not a perfect square in size\n");
        close(fd1);
        close(fd2);
        exit(EXIT_FAILURE);
    }

    if (dimension1 != dimension2)
    {
        fprintf(stderr, "error: the arrays are not the same size\n");
        close(fd1);
        close(fd2);
        exit(EXIT_FAILURE);
    }

    int dimension = dimension1;

    /* MEMORY MAPPING */
    // Maps input files into memory for efficient access
    int *A = mmap(NULL, size1, PROT_READ, MAP_SHARED, fd1, 0);
    if (A == MAP_FAILED)
    {
        fprintf(stderr, "error: mmap failed for %s: %s\n", input_file1, strerror(errno));
        close(fd1);
        close(fd2);
        exit(EXIT_FAILURE);
    }

    int *B = mmap(NULL, size2, PROT_READ, MAP_SHARED, fd2, 0);
    if (B == MAP_FAILED)
    {
        fprintf(stderr, "error: mmap failed for %s: %s\n", input_file2, strerror(errno));
        munmap(A, size1);
        close(fd1);
        close(fd2);
        exit(EXIT_FAILURE);
    }

    // Allocates or maps memory for the output matrix
    int *C = NULL;
    size_t output_size = dimension * dimension * sizeof(int);
    int fd_out = -1;

    if (output_file)
    {
        // Creates or truncates the output file
        fd_out = open(output_file, O_RDWR | O_CREAT | O_TRUNC, 0666);
        if (fd_out < 0)
        {
            fprintf(stderr, "error: could not open/create output file %s: %s\n", output_file, strerror(errno));
            munmap(A, size1);
            munmap(B, size2);
            close(fd1);
            close(fd2);
            exit(EXIT_FAILURE);
        }

        // Resizes the output file
        if (ftruncate(fd_out, (off_t)output_size) < 0)
        {
            fprintf(stderr, "error: ftruncate failed for %s: %s\n", output_file, strerror(errno));
            munmap(A, size1);
            munmap(B, size2);
            close(fd1);
            close(fd2);
            close(fd_out);
            exit(EXIT_FAILURE);
        }

        // mmaps the output file
        C = mmap(NULL, output_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_out, 0);
        if (C == MAP_FAILED)
        {
            fprintf(stderr, "error: mmap failed for %s: %s\n", output_file, strerror(errno));
            munmap(A, size1);
            munmap(B, size2);
            close(fd1);
            close(fd2);
            close(fd_out);
            exit(EXIT_FAILURE);
        }
    }
    else
    {
        // No output file specified, discards output
        // We'll just allocate C in memory and not write to any file
        C = malloc(output_size);
        if (!C)
        {
            fprintf(stderr, "error: could not allocate memory for result\n");
            munmap(A, size1);
            munmap(B, size2);
            close(fd1);
            close(fd2);
            exit(EXIT_FAILURE);
        }
    }

    // Sets up threading
    pthread_t *thread_ids = malloc(sizeof(pthread_t) * threads);
    if (!thread_ids)
    {
        fprintf(stderr, "error: could not allocate memory for threads\n");
        if (output_file)
        {
            munmap(C, output_size);
            close(fd_out);
        }
        else
        {
            free(C);
        }
        munmap(A, size1);
        munmap(B, size2);
        close(fd1);
        close(fd2);
        exit(EXIT_FAILURE);
    }

    // Distributes work among threads
    thread_args_t *targs = malloc(sizeof(thread_args_t) * threads);
    if (!targs)
    {
        fprintf(stderr, "error: could not allocate memory for thread arguments\n");
        free(thread_ids);
        if (output_file)
        {
            munmap(C, output_size);
            close(fd_out);
        }
        else
        {
            free(C);
        }
        munmap(A, size1);
        munmap(B, size2);
        close(fd1);
        close(fd2);
        exit(EXIT_FAILURE);
    }

    int rows_per_thread = dimension / threads;
    int remainder = dimension % threads;
    int start_row = 0;

    printf("Multiplying arrays of dimension %d\n", dimension);

    for (int i = 0; i < threads; i++)
    {
        int end_row = start_row + rows_per_thread;
        if (i < remainder)
        {
            end_row += 1;
        }

        targs[i].A = A;
        targs[i].B = B;
        targs[i].C = C;
        targs[i].dimension = dimension;
        targs[i].start_row = start_row;
        targs[i].end_row = end_row;
        start_row = end_row;

        if (pthread_create(&thread_ids[i], NULL, multiply_rows, &targs[i]) != 0)
        {
            fprintf(stderr, "error: could not create thread %d: %s\n", i, strerror(errno));
            // Cleanup
            for (int j = 0; j < i; j++)
            {
                pthread_join(thread_ids[j], NULL);
            }
            free(targs);
            free(thread_ids);
            if (output_file)
            {
                munmap(C, output_size);
                close(fd_out);
            }
            else
            {
                free(C);
            }
            munmap(A, size1);
            munmap(B, size2);
            close(fd1);
            close(fd2);
            exit(EXIT_FAILURE);
        }
    }

    // Joins threads
    for (int i = 0; i < threads; i++)
    {
        pthread_join(thread_ids[i], NULL);
    }

    // Cleanup
    free(targs);
    free(thread_ids);

    // If no output file was given, result is discarded (we do nothing with C)
    if (output_file)
    {
        // msync to ensure data is written to file
        if (msync(C, output_size, MS_SYNC) < 0)
        {
            fprintf(stderr, "warning: msync failed on output file: %s\n", strerror(errno));
        }
        munmap(C, output_size);
        close(fd_out);
    }
    else
    {
        free(C);
    }

    munmap(A, size1);
    munmap(B, size2);
    close(fd1);
    close(fd2);

    return EXIT_SUCCESS;
}