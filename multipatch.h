/*
 * Multi-patch container format for memory-constrained environments
 */
#ifndef _MINIBSDIFF_MULTIPATCH_H_
#define _MINIBSDIFF_MULTIPATCH_H_

#include <sys/types.h>
#include <stdbool.h>
#include "minibsdiff-config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Magic number for multi-patch container */
#define MULTIPATCH_MAGIC "MPATCH01"

/* Multi-patch header structure */
typedef struct {
    char magic[8];        /* MULTIPATCH_MAGIC */
    off_t num_patches;    /* Number of patches in container */
    off_t total_newsize;  /* Total size of final output */
} multipatch_header;

/* Patch entry header */
typedef struct {
    off_t patch_offset;   /* Offset to patch data in container */
    off_t patch_size;     /* Size of this patch */
    off_t input_size;     /* Size of input for this patch */
    off_t output_size;    /* Size of output for this patch */
} patch_entry;

/*
 * Create a multi-patch container from multiple input/output file pairs
 * Returns the size of the container or -1 on error
 */
off_t create_multipatch(const char** old_files, const char** new_files, int num_files, 
                       u_char* container, off_t container_size);

/*
 * Apply a multi-patch container to a sequence of files
 * Returns 0 on success, -1 on error
 */
int apply_multipatch(const char* input_file, const char* output_file, 
                    u_char* container, off_t container_size);

/*
 * Get the total output size from a multi-patch container
 * Returns the size or -1 on error
 */
off_t multipatch_total_size(u_char* container, off_t container_size);

/*
 * Validate a multi-patch container
 * Returns true if valid, false otherwise
 */
bool multipatch_valid(u_char* container, off_t container_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _MINIBSDIFF_MULTIPATCH_H_ */
