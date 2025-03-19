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
    
    f = fopen(filename, "rb");
    if (f == NULL) {
        fprintf(stderr, "Error: Could not open file %s\n", filename);
        return -1;
    }
    
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    *data = malloc(size);
    if (*data == NULL) {
        fprintf(stderr, "Error: Could not allocate memory for file %s\n", filename);
        fclose(f);
        return -1;
    }
    
    if (fread(*data, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "Error: Could not read file %s\n", filename);
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
        
        /* Calculate patch size */
        patch_size = bsdiff_patchsize_max(old_size, new_size);
        
        /* Allocate memory for patch */
        patch_data = malloc(patch_size);
        if (patch_data == NULL) {
            fprintf(stderr, "Error: Could not allocate memory for patch\n");
            free(old_data);
            free(new_data);
            free(entries);
            return -1;
        }
        
        /* Create patch */
        int res = bsdiff(old_data, old_size, new_data, new_size, patch_data, patch_size);
        if (res <= 0) {
            fprintf(stderr, "Error: Could not create patch for files %s and %s\n", 
                    old_files[i], new_files[i]);
            free(old_data);
            free(new_data);
            free(patch_data);
            free(entries);
            return -1;
        }
        
        /* Update patch size */
        patch_size = res;
        
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
    for (i = 0; i < num_files; i++) {
        off_t offset = (off_t)sizeof(multipatch_header) + i * (off_t)sizeof(patch_entry);
        write_off_t(entries[i].patch_offset, container + offset);
        write_off_t(entries[i].patch_size, container + offset + 8);
        write_off_t(entries[i].input_size, container + offset + 16);
        write_off_t(entries[i].output_size, container + offset + 24);
    }
    
    /* Free memory */
    free(entries);
    
    /* Return container size */
    return current_offset;
}

int
apply_multipatch(const char* input_file, const char* output_file, 
                u_char* container, off_t container_size)
{
    multipatch_header header;
    patch_entry* entries;
    u_char* input_data = NULL;
    u_char* chunk_output = NULL;
    u_char* final_output = NULL;
    off_t input_size, chunk_size, output_pos = 0;
    int i;
    
    /* Check container size */
    if ((off_t)sizeof(multipatch_header) > container_size) {
        fprintf(stderr, "Error: Container too small for header\n");
        return -1;
    }
    
    /* Read header */
    memcpy(header.magic, container, 8);
    header.num_patches = read_off_t(container + 8);
    header.total_newsize = read_off_t(container + 16);
    
    /* Validate magic number */
    if (memcmp(header.magic, MULTIPATCH_MAGIC, 8) != 0) {
        fprintf(stderr, "Error: Invalid magic number in container\n");
        return -1;
    }
    
    /* Check container size again */
    if ((off_t)(sizeof(multipatch_header) + header.num_patches * sizeof(patch_entry)) > container_size) {
        fprintf(stderr, "Error: Container too small for patch entries\n");
        return -1;
    }
    
    /* Allocate memory for patch entries */
    entries = malloc(header.num_patches * sizeof(patch_entry));
    if (entries == NULL) {
        fprintf(stderr, "Error: Could not allocate memory for patch entries\n");
        return -1;
    }
    
    /* Read patch entries */
    for (i = 0; i < header.num_patches; i++) {
        off_t offset = (off_t)sizeof(multipatch_header) + i * (off_t)sizeof(patch_entry);
        entries[i].patch_offset = read_off_t(container + offset);
        entries[i].patch_size = read_off_t(container + offset + 8);
        entries[i].input_size = read_off_t(container + offset + 16);
        entries[i].output_size = read_off_t(container + offset + 24);
    }
    
    /* Read input file */
    input_size = read_file(input_file, &input_data);
    if (input_size < 0) {
        fprintf(stderr, "Error: Could not read input file %s\n", input_file);
        free(entries);
        return -1;
    }
    
    /* Allocate memory for final output */
    final_output = malloc(header.total_newsize);
    if (final_output == NULL) {
        fprintf(stderr, "Error: Could not allocate memory for final output\n");
        free(input_data);
        free(entries);
        return -1;
    }
    
    /* Calculate chunk size */
    chunk_size = input_size / header.num_patches;
    if (chunk_size < 1) {
        fprintf(stderr, "Error: Input file too small for %lld chunks\n", 
                (long long)header.num_patches);
        free(input_data);
        free(entries);
        free(final_output);
        return -1;
    }
    
    /* Apply patches */
    for (i = 0; i < header.num_patches; i++) {
        /* Calculate chunk boundaries */
        off_t chunk_start = i * chunk_size;
        off_t chunk_end = (i == header.num_patches - 1) ? input_size : (i + 1) * chunk_size;
        
        /* Skip if chunk exceeds input size */
        if (chunk_start >= input_size) {
            fprintf(stderr, "Warning: Chunk %d exceeds input size, skipping\n", i);
            continue;
        }
        
        /* Allocate memory for chunk output */
        chunk_output = malloc(entries[i].output_size);
        if (chunk_output == NULL) {
            fprintf(stderr, "Error: Could not allocate memory for chunk %d output\n", i);
            free(input_data);
            free(entries);
            free(final_output);
            return -1;
        }
        
        /* Apply patch to chunk */
        if (bspatch(input_data + chunk_start, chunk_end - chunk_start, 
                   chunk_output, entries[i].output_size,
                   container + entries[i].patch_offset, entries[i].patch_size) != 0) {
            fprintf(stderr, "Error: Could not apply patch %d - skipping\n", i);
            
            /* Fill with zeros on error */
            memset(chunk_output, 0, entries[i].output_size);
        }
        
        /* Copy chunk output to final output */
        memcpy(final_output + output_pos, chunk_output, entries[i].output_size);
        output_pos += entries[i].output_size;
        
        /* Free chunk output */
        free(chunk_output);
    }
    
    /* Write final output to file */
    if (write_file(output_file, final_output, header.total_newsize) != 0) {
        fprintf(stderr, "Error: Could not write output file %s\n", output_file);
        free(input_data);
        free(entries);
        free(final_output);
        return -1;
    }
    
    /* Success */
    fprintf(stderr, "Successfully applied multi-patch to create %s (%lld bytes)\n", 
            output_file, (long long)header.total_newsize);
    
    /* Free memory */
    free(input_data);
    free(entries);
    free(final_output);
    
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
