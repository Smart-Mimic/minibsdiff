/*
 * Implementation of multi-patch container format
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include "multipatch.h"
#include "bsdiff.h"
#include "bspatch.h"

/* Write an off_t value to a byte buffer */
static void
write_off_t(off_t value, u_char* buf)
{
    off_t y = value;
    
    buf[0] = y % 256; y /= 256;
    buf[1] = y % 256; y /= 256;
    buf[2] = y % 256; y /= 256;
    buf[3] = y % 256; y /= 256;
    buf[4] = y % 256; y /= 256;
    buf[5] = y % 256; y /= 256;
    buf[6] = y % 256; y /= 256;
    buf[7] = y % 256;
}

/* Read an off_t value from a byte buffer */
static off_t
read_off_t(u_char* buf)
{
    off_t y;

    y = buf[7] & 0x7F;
    y = y * 256 + buf[6];
    y = y * 256 + buf[5];
    y = y * 256 + buf[4];
    y = y * 256 + buf[3];
    y = y * 256 + buf[2];
    y = y * 256 + buf[1];
    y = y * 256 + buf[0];

    if (buf[7] & 0x80) y = -y;

    return y;
}

/* Read a file into memory */
static off_t
read_file(const char* filename, u_char** data)
{
    FILE* f;
    off_t size;
    
    /* Validate input parameters */
    if (filename == NULL) {
        fprintf(stderr, "Error: NULL filename\n");
        return -1;
    }
    
    if (data == NULL) {
        fprintf(stderr, "Error: NULL data pointer\n");
        return -1;
    }
    
    f = fopen(filename, "rb");
    if (f == NULL) {
        fprintf(stderr, "Error: Could not open file %s\n", filename);
        return -1;
    }
    
    if (fseek(f, 0, SEEK_END) != 0) {
        fprintf(stderr, "Error: Could not seek to end of file %s\n", filename);
        fclose(f);
        return -1;
    }
    
    size = ftell(f);
    if (size < 0) {
        fprintf(stderr, "Error: Could not get file size for %s\n", filename);
        fclose(f);
        return -1;
    }
    
    if (fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "Error: Could not seek to start of file %s\n", filename);
        fclose(f);
        return -1;
    }
    
    /* Check for reasonable size limits */
    if (size > (off_t)(1024 * 1024 * 1024)) { /* 1GB limit */
        fprintf(stderr, "Error: File %s is too large (>1GB)\n", filename);
        fclose(f);
        return -1;
    }
    
    *data = malloc((size_t)size);
    if (*data == NULL) {
        fprintf(stderr, "Error: Could not allocate %lld bytes for file %s\n", 
                (long long)size, filename);
        fclose(f);
        return -1;
    }
    
    size_t bytes_read = fread(*data, 1, (size_t)size, f);
    if (bytes_read != (size_t)size) {
        fprintf(stderr, "Error: Could not read file %s (read %zu of %lld bytes)\n", 
                filename, bytes_read, (long long)size);
        free(*data);
        fclose(f);
        return -1;
    }
    
    fclose(f);
    return size;
}

/* Write data to a file */
static int
write_file(const char* filename, u_char* data, off_t size)
{
    FILE* f;
    
    f = fopen(filename, "wb");
    if (f == NULL) {
        fprintf(stderr, "Error: Could not open file %s for writing\n", filename);
        return -1;
    }
    
    if (fwrite(data, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "Error: Could not write to file %s\n", filename);
        fclose(f);
        return -1;
    }
    
    fclose(f);
    return 0;
}

off_t
create_multipatch(const char** old_files, const char** new_files, int num_files, 
                 u_char* container, off_t container_size)
{
    multipatch_header header;
    patch_entry* entries;
    u_char* old_data;
    u_char* new_data;
    u_char* patch_data;
    off_t old_size, new_size, patch_size;
    off_t current_offset;
    int i;
    
    /* Initialize header */
    memcpy(header.magic, MULTIPATCH_MAGIC, 8);
    header.num_patches = num_files;
    header.total_newsize = 0;
    
    /* Calculate required container size and total output size */
    off_t required_size = (off_t)sizeof(multipatch_header) + (off_t)num_files * (off_t)sizeof(patch_entry);
    
    for (i = 0; i < num_files; i++) {
        old_size = 0;
        new_size = 0;
        
        /* Validate file pointers */
        if (old_files[i] == NULL || new_files[i] == NULL) {
            fprintf(stderr, "Error: NULL file pointer at index %d\n", i);
            return -1;
        }
        
        /* Read input files */
        old_size = read_file(old_files[i], &old_data);
        if (old_size < 0) {
            fprintf(stderr, "Error: Could not read old file %s\n", old_files[i]);
            return -1;
        }
        
        new_size = read_file(new_files[i], &new_data);
        if (new_size < 0) {
            fprintf(stderr, "Error: Could not read new file %s\n", new_files[i]);
            free(old_data);
            return -1;
        }
        
        /* Calculate patch size */
        patch_size = bsdiff_patchsize_max(old_size, new_size);
        required_size += patch_size;
        
        /* Update total output size */
        header.total_newsize += new_size;
        
        /* Free memory */
        free(old_data);
        free(new_data);
    }
    
    /* Check if container is large enough */
    if (required_size > container_size) {
        fprintf(stderr, "Error: Container size too small (need %lld bytes, have %lld bytes)\n", 
                (long long)required_size, (long long)container_size);
        return -1;
    }
    
    /* Write header */
    memcpy(container, header.magic, 8);
    write_off_t(header.num_patches, container + 8);
    write_off_t(header.total_newsize, container + 16);
    
    /* Allocate memory for patch entries */
    entries = malloc(num_files * sizeof(patch_entry));
    if (entries == NULL) {
        fprintf(stderr, "Error: Could not allocate memory for patch entries\n");
        return -1;
    }
    
    /* Initialize current offset */
    current_offset = (off_t)sizeof(multipatch_header) + (off_t)num_files * (off_t)sizeof(patch_entry);
    
    /* Create patches */
    for (i = 0; i < num_files; i++) {
        /* Read input files */
        old_size = read_file(old_files[i], &old_data);
        if (old_size < 0) {
            fprintf(stderr, "Error: Could not read old file %s\n", old_files[i]);
            free(entries);
            return -1;
        }
        
        new_size = read_file(new_files[i], &new_data);
        if (new_size < 0) {
            fprintf(stderr, "Error: Could not read new file %s\n", new_files[i]);
            free(old_data);
            free(entries);
            return -1;
        }
        
        /* Calculate patch size with validation */
        patch_size = bsdiff_patchsize_max(old_size, new_size);
        if (patch_size <= 0) {
            fprintf(stderr, "Error: Invalid patch size calculated for files %s and %s\n", 
                    old_files[i], new_files[i]);
            free(old_data);
            free(new_data);
            free(entries);
            return -1;
        }
        
        /* Check if adding this patch would overflow the container */
        if (current_offset + patch_size > container_size) {
            fprintf(stderr, "Error: Container size too small for patch %d (need %lld more bytes)\n", 
                    i, (long long)(current_offset + patch_size - container_size));
            free(old_data);
            free(new_data);
            free(entries);
            return -1;
        }
        
        /* Allocate memory for patch */
        patch_data = malloc((size_t)patch_size);
        if (patch_data == NULL) {
            fprintf(stderr, "Error: Could not allocate %lld bytes for patch\n", 
                    (long long)patch_size);
            free(old_data);
            free(new_data);
            free(entries);
            return -1;
        }
        
        /* Create patch with error handling */
        bool print_stats = false;
        if (i == num_files - 1) print_stats = true;
        int res = bsdiff(old_data, old_size, new_data, new_size, patch_data, patch_size, print_stats);
        if (res <= 0) {
            fprintf(stderr, "Error: Could not create patch for files %s and %s (error: %d)\n", 
                    old_files[i], new_files[i], res);
            free(old_data);
            free(new_data);
            free(patch_data);
            free(entries);
            return -1;
        }
        
        /* Update patch size and validate */
        patch_size = res;
        if (patch_size <= 0 || patch_size > container_size - current_offset) {
            fprintf(stderr, "Error: Invalid patch size %lld for files %s and %s\n", 
                    (long long)patch_size, old_files[i], new_files[i]);
            free(old_data);
            free(new_data);
            free(patch_data);
            free(entries);
            return -1;
        }
        
        /* Fill in patch entry */
        entries[i].patch_offset = current_offset;
        entries[i].patch_size = patch_size;
        entries[i].input_size = old_size;
        entries[i].output_size = new_size;
        
        /* Copy patch data to container */
        memcpy(container + current_offset, patch_data, patch_size);
        
        /* Update current offset */
        current_offset += patch_size;
        
        /* Free memory */
        free(old_data);
        free(new_data);
        free(patch_data);
    }
    
    /* Write patch entries */
    off_t MaxOutputSize = 0;
    off_t MaxInputSize = 0;
    for (i = 0; i < num_files; i++) {
        off_t offset = (off_t)sizeof(multipatch_header) + i * (off_t)sizeof(patch_entry);
        write_off_t(entries[i].patch_offset, container + offset);
        write_off_t(entries[i].patch_size, container + offset + 8);
        write_off_t(entries[i].input_size, container + offset + 16);
        write_off_t(entries[i].output_size, container + offset + 24);
        if (entries[i].input_size > MaxInputSize) MaxInputSize = entries[i].input_size;
        if (entries[i].output_size > MaxOutputSize) MaxOutputSize = entries[i].output_size;
    }
    printf("MaxInputSize: %lld\n", (long long)MaxInputSize);
    printf("MaxOutputSize: %lld\n", (long long)MaxOutputSize);
    
    /* Free memory */
    free(entries);
    
    return current_offset;
}

int
apply_multipatch(const char* input_file, const char* output_file, 
                u_char* container, off_t container_size)
{
    multipatch_header header;
    patch_entry* entries;
    u_char* input_data;
    u_char* output_data;
    u_char* patch_data;
    off_t input_size;
    off_t output_size;
    off_t patch_size;
    int i;
    
    /* Validate input parameters */
    if (input_file == NULL || output_file == NULL || container == NULL || container_size <= 0) {
        fprintf(stderr, "Error: Invalid input parameters\n");
        return -1;
    }
    
    /* Read input file */
    input_size = read_file(input_file, &input_data);
    if (input_size < 0) {
        fprintf(stderr, "Error: Could not read input file %s\n", input_file);
        return -1;
    }
    
    /* Validate container size */
    if (container_size < (off_t)sizeof(multipatch_header)) {
        fprintf(stderr, "Error: Container size too small for header\n");
        free(input_data);
        return -1;
    }
    
    /* Read header */
    memcpy(header.magic, container, 8);
    if (memcmp(header.magic, MULTIPATCH_MAGIC, 8) != 0) {
        fprintf(stderr, "Error: Invalid multipatch magic number\n");
        free(input_data);
        return -1;
    }
    
    header.num_patches = read_off_t(container + 8);
    header.total_newsize = read_off_t(container + 16);
    
    /* Validate header */
    if (header.num_patches <= 0 || header.num_patches > 1000) {
        fprintf(stderr, "Error: Invalid number of patches in header (%lld)\n", 
                (long long)header.num_patches);
        free(input_data);
        return -1;
    }
    
    if (header.total_newsize <= 0) {
        fprintf(stderr, "Error: Invalid total new size in header (%lld)\n", 
                (long long)header.total_newsize);
        free(input_data);
        return -1;
    }
    
    /* Allocate memory for patch entries */
    entries = malloc(header.num_patches * sizeof(patch_entry));
    if (entries == NULL) {
        fprintf(stderr, "Error: Could not allocate memory for patch entries\n");
        free(input_data);
        return -1;
    }
    
    /* Read patch entries */
    for (i = 0; i < header.num_patches; i++) {
        off_t offset = (off_t)sizeof(multipatch_header) + i * (off_t)sizeof(patch_entry);
        if (offset + (off_t)sizeof(patch_entry) > container_size) {
            fprintf(stderr, "Error: Container size too small for patch entries\n");
            free(input_data);
            free(entries);
            return -1;
        }
        
        entries[i].patch_offset = read_off_t(container + offset);
        entries[i].patch_size = read_off_t(container + offset + 8);
        entries[i].input_size = read_off_t(container + offset + 16);
        entries[i].output_size = read_off_t(container + offset + 24);
        
        /* Validate patch entry */
        if (entries[i].patch_offset < 0 || entries[i].patch_size <= 0 ||
            entries[i].input_size <= 0 || entries[i].output_size <= 0) {
            fprintf(stderr, "Error: Invalid patch entry %d\n", i);
            free(input_data);
            free(entries);
            return -1;
        }
        
        if (entries[i].patch_offset + entries[i].patch_size > container_size) {
            fprintf(stderr, "Error: Patch %d extends beyond container size\n", i);
            free(input_data);
            free(entries);
            return -1;
        }
    }
    
    /* Allocate memory for output */
    output_data = malloc((size_t)header.total_newsize);
    if (output_data == NULL) {
        fprintf(stderr, "Error: Could not allocate %lld bytes for output\n", 
                (long long)header.total_newsize);
        free(input_data);
        free(entries);
        return -1;
    }
    
    /* Apply patches */
    for (i = 0; i < header.num_patches; i++) {
        /* Validate patch data */
        if (entries[i].input_size != input_size) {
            fprintf(stderr, "Error: Input size mismatch for patch %d (expected %lld, got %lld)\n", 
                    i, (long long)entries[i].input_size, (long long)input_size);
            free(input_data);
            free(output_data);
            free(entries);
            return -1;
        }
        
        /* Allocate memory for patch */
        patch_data = malloc((size_t)entries[i].patch_size);
        if (patch_data == NULL) {
            fprintf(stderr, "Error: Could not allocate %lld bytes for patch %d\n", 
                    (long long)entries[i].patch_size, i);
            free(input_data);
            free(output_data);
            free(entries);
            return -1;
        }
        
        /* Copy patch data */
        memcpy(patch_data, container + entries[i].patch_offset, (size_t)entries[i].patch_size);
        
        /* Apply patch */
        int res = bspatch(input_data, input_size, output_data, entries[i].output_size, 
                         patch_data, entries[i].patch_size);
        if (res != 0) {
            fprintf(stderr, "Error: Failed to apply patch %d (error: %d)\n", i, res);
            free(input_data);
            free(output_data);
            free(patch_data);
            free(entries);
            return -1;
        }
        
        /* Update input data for next patch */
        free(input_data);
        input_data = output_data;
        input_size = entries[i].output_size;
        
        /* Allocate new output buffer for next patch */
        if (i < header.num_patches - 1) {
            output_data = malloc((size_t)header.total_newsize);
            if (output_data == NULL) {
                fprintf(stderr, "Error: Could not allocate %lld bytes for output\n", 
                        (long long)header.total_newsize);
                free(input_data);
                free(patch_data);
                free(entries);
                return -1;
            }
        }
        
        free(patch_data);
    }
    
    /* Write output file */
    if (write_file(output_file, output_data, header.total_newsize) != 0) {
        fprintf(stderr, "Error: Could not write output file %s\n", output_file);
        free(input_data);
        free(entries);
        return -1;
    }
    
    /* Cleanup */
    free(input_data);
    free(entries);
    
    return 0;
}

off_t
multipatch_total_size(u_char* container, off_t container_size)
{
    /* Check container size */
    if ((off_t)sizeof(multipatch_header) > container_size) {
        return -1;
    }
    
    /* Validate magic number */
    if (memcmp(container, MULTIPATCH_MAGIC, 8) != 0) {
        return -1;
    }
    
    /* Return total output size */
    return read_off_t(container + 16);
}

bool
multipatch_valid(u_char* container, off_t container_size)
{
    multipatch_header header;
    off_t i;
    
    /* Check container size */
    if ((off_t)sizeof(multipatch_header) > container_size) {
        return false;
    }
    
    /* Read header */
    memcpy(header.magic, container, 8);
    header.num_patches = read_off_t(container + 8);
    header.total_newsize = read_off_t(container + 16);
    
    /* Validate magic number */
    if (memcmp(header.magic, MULTIPATCH_MAGIC, 8) != 0) {
        return false;
    }
    
    /* Check for valid number of patches */
    if (header.num_patches <= 0) {
        return false;
    }
    
    /* Check container size again */
    if ((off_t)(sizeof(multipatch_header) + header.num_patches * sizeof(patch_entry)) > container_size) {
        return false;
    }
    
    /* Validate patch entries */
    for (i = 0; i < header.num_patches; i++) {
        off_t offset = (off_t)sizeof(multipatch_header) + i * (off_t)sizeof(patch_entry);
        off_t patch_offset = read_off_t(container + offset);
        off_t patch_size = read_off_t(container + offset + 8);
        off_t input_size = read_off_t(container + offset + 16);
        off_t output_size = read_off_t(container + offset + 24);
        
        /* Check for valid values */
        if (patch_offset < 0 || 
            patch_size < 0 || 
            input_size < 0 || 
            output_size < 0 ||
            patch_offset + patch_size > container_size) {
            return false;
        }
    }
    
    return true;
}
