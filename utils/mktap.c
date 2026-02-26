#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

void write_record(FILE *out_file, const unsigned char *data, int length) {
    uint32_t record_len = length;
    if (record_len % 2 != 0) {
        record_len++;
    }

    fwrite(&record_len, sizeof(uint32_t), 1, out_file);
    fwrite(data, 1, length, out_file);
    if (length % 2 != 0) {
        fputc(0, out_file);
    }
    fwrite(&record_len, sizeof(uint32_t), 1, out_file);
}

void write_tap_mark(FILE *out_file) {
    uint32_t mark = 0;
    fwrite(&mark, sizeof(uint32_t), 1, out_file);
}

void write_eom_mark(FILE *out_file) {
    uint32_t mark = 0xFFFFFFFF;
    fwrite(&mark, sizeof(uint32_t), 1, out_file);
}

void handle_create(int argc, char *argv[]) {
    int blocksize = 512;
    char *output_file = NULL;
    int opt;

    if (argc < 4) { // mktap create output.tap input1
        fprintf(stderr, "Usage: %s create <output_file> [-b <blocksize>] <input_file1> ...\n", argv[0]);
        fprintf(stderr, "Note: Options like -b must come before input files.\n");
        exit(EXIT_FAILURE);
    }

    output_file = argv[2];

    // Start parsing options after 'create <output_file>'
    optind = 3;
    while ((opt = getopt(argc, argv, ":b:")) != -1) {
        switch (opt) {
            case 'b':
                blocksize = atoi(optarg);
                break;
            case '?':
                fprintf(stderr, "Unknown option: -%c\n", optopt);
                exit(EXIT_FAILURE);
            case ':':
                fprintf(stderr, "Option -%c requires an argument.\n", optopt);
                exit(EXIT_FAILURE);
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "No input files specified.\n");
        exit(EXIT_FAILURE);
    }

    FILE *out_file = fopen(output_file, "wb");
    if (!out_file) {
        perror("Error opening output file");
        exit(EXIT_FAILURE);
    }

    unsigned char *buffer = malloc(blocksize);
    if (!buffer) {
        fprintf(stderr, "Failed to allocate memory for buffer\n");
        fclose(out_file);
        exit(EXIT_FAILURE);
    }

    for (int i = optind; i < argc; i++) {
        FILE *in_file = fopen(argv[i], "rb");
        if (!in_file) {
            perror(argv[i]);
            continue;
        }

        size_t bytes_read;
        while ((bytes_read = fread(buffer, 1, blocksize, in_file)) > 0) {
            write_record(out_file, buffer, bytes_read);
        }

        fclose(in_file);
        write_tap_mark(out_file);
        printf("Added %s to %s\n", argv[i], output_file);
    }

    write_eom_mark(out_file);
    free(buffer);
    fclose(out_file);

    printf("Finished writing %s\n", output_file);
}

void handle_list(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s list <tap_file>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    char *tap_file = argv[2];
    FILE *in_file = fopen(tap_file, "rb");
    if (!in_file) {
        perror("Error opening tap file");
        exit(EXIT_FAILURE);
    }

    uint32_t record_len;
    int record_num = 0;

    while (fread(&record_len, sizeof(uint32_t), 1, in_file) == 1) {
        if (record_len == 0) {
            printf("Record %d: Tape Mark\n", record_num++);
        } else if (record_len == 0xFFFFFFFF) {
            printf("Record %d: End of Medium\n", record_num++);
            break; // End of tape
        } else {
            printf("Record %d: Data Record, Length: %u bytes\n", record_num++, record_len);
            fseek(in_file, record_len, SEEK_CUR);
            uint32_t trailing_len;
            if (fread(&trailing_len, sizeof(uint32_t), 1, in_file) != 1 || trailing_len != record_len) {
                fprintf(stderr, "Error: Mismatched record length at record %d\n", record_num - 1);
                break;
            }
        }
    }

    fclose(in_file);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <command> [options]\n", argv[0]);
        fprintf(stderr, "Commands:\n");
        fprintf(stderr, "  create <output_file> [-b <blocksize>] <input_file1> ...\n");
        fprintf(stderr, "  list   <tap_file>\n");
        exit(EXIT_FAILURE);
    }

    if (strcmp(argv[1], "create") == 0) {
        handle_create(argc, argv);
    } else if (strcmp(argv[1], "list") == 0) {
        handle_list(argc, argv);
    } else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        fprintf(stderr, "Usage: %s <command> [options]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    return 0;
}