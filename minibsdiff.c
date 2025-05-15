/*-
 * Copyright 2012-2013 Austin Seipp
 * Copyright 2003-2005 Colin Percival
 * All rights reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted providing that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*-
 * This is just a simple example of using
 * the portable bsdiff API. Parts of it are derived
 * from the original bsdiff/bspatch.
 *
 * Compile with:
 *
 *   $ cc -Wall -std=c99 -O2 minibsdiff.c bsdiff.c bspatch.c
 *
 * Usage:
 *
 *   $ ./a.out gen <v1> <v2> <patch>
 *   $ ./a.out app <v1> <patch> <v2>
 */

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS 1
#endif /* _MSC_VER */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

/* Create one large compilation unit */
#include "bspatch.c"
#include "bsdiff.c"
#include "multipatch.h"

/* Add string.h for strdup */
#include <string.h>

/* ------------------------------------------------------------------------- */
/* -- Utilities ------------------------------------------------------------ */

static char* progname;

static void
barf(const char* msg)
{
  printf("%s: ERROR: %s", progname, msg);
  exit(EXIT_FAILURE);
}

static void
usage(void)
{
  printf("usage:\n\n"
         "Generate patch:\n"
         "\t$ %s gen <v1> <v2> <patch> [--mgen <num_chunks>]\n"
         "Apply patch:\n"
         "\t$ %s app <v1> <patch> <v2>\n"
         "Apply multi-patch:\n"
         "\t$ %s mapp <v1> <patch> <v2>\n", 
         progname, progname, progname);
  exit(EXIT_FAILURE);
}

static long
read_file(const char* f, u_char** buf)
{
  FILE* fp;
  long fsz;

  fsz = 0;
  if ( ((fp = fopen(f, "rb"))  == NULL)         ||
       (fseek(fp, 0, SEEK_END)  != 0)           ||
       ((fsz = ftell(fp))       == -1)          ||
       ((*buf = malloc(fsz+1))  == NULL)        ||
       (fseek(fp, 0, SEEK_SET)  != 0)           ||
       (fread(*buf, 1, fsz, fp) != (size_t)fsz) ||
       (fclose(fp)              != 0)
     ) barf("Couldn't open file for reading!\n");

  return fsz;
}

static void
write_file(const char* f, u_char* buf, long sz)
{
  FILE* fp;

  if ( ((fp = fopen(f, "w+b")) == NULL)       ||
       (fwrite(buf, 1, sz, fp) != (size_t)sz) ||
       (fclose(fp)             != 0)
     ) barf("Couldn't open file for writing!\n");

  return;
}

/* ------------------------------------------------------------------------- */
/* -- Main routines -------------------------------------------------------- */

static void
diff(const char* oldf, const char* newf, const char* patchf)
{
  u_char* old;
  u_char* new;
  u_char* patch;
  long oldsz, newsz;
  off_t patchsz;
  int res;

#ifndef NDEBUG
  printf("Generating binary patch between %s and %s\n", oldf, newf);
#endif /* NDEBUG */

  /* Read old and new files */
  oldsz = read_file(oldf, &old);
  newsz = read_file(newf, &new);

#ifndef NDEBUG
  printf("Old file = %lu bytes\nNew file = %lu bytes\n", oldsz, newsz);
#endif /* NDEBUG */

  /* Compute delta */
#ifndef NDEBUG
  printf("Computing binary delta...\n");
#endif /* NDEBUG */

  patchsz = bsdiff_patchsize_max(oldsz, newsz);
  patch = malloc(patchsz+1); /* Never malloc(0) */
  res = bsdiff(old, oldsz, new, newsz, patch, patchsz);
  if (res <= 0) barf("bsdiff() failed!");
  patchsz = res;

#ifndef NDEBUG
  printf("sizeof(delta('%s', '%s')) = %lld bytes\n", oldf, newf, patchsz);
#endif /* NDEBUG */

  /* Write patch */
  FILE *f = fopen(patchf, "wb");  // Make sure it's opened in binary mode
  if (f == NULL) {
    fprintf(stderr, "ERROR: Couldn't create patch file %s\n", patchf);
    free(patch);
    return;
  }

  // Write patch file
  size_t bytes_written = fwrite(patch, 1, patchsz, f);
  if ((size_t)bytes_written != (size_t)patchsz) {
    printf("ERROR: Could not write patch file (wrote %zu of %lld bytes)\n", 
           bytes_written, (long long)patchsz);
    fclose(f);
    free(patch);
    return;
  }
  fclose(f);

  free(old);
  free(new);
  free(patch);

#ifndef NDEBUG
  printf("Created patch file %s\n", patchf);
#endif /* NDEBUG */
  exit(EXIT_SUCCESS);
}

static void
patch(const char* inf, const char* patchf, const char* outf)
{
  u_char* inp;
  u_char* patchp;
  u_char* newp;
  long insz, patchsz;
  ssize_t newsz;
  int res;

#ifndef NDEBUG
  printf("Applying binary patch %s to %s\n", patchf, inf);
#endif /* NDEBUG */

  /* Read old file and patch file */
  insz    = read_file(inf, &inp);
  FILE *f = fopen(patchf, "rb");  // Make sure it's opened in binary mode
  if (f == NULL) {
    fprintf(stderr, "ERROR: Couldn't open patch file %s\n", patchf);
    return;
  }

  // Check file size
  fseek(f, 0, SEEK_END);
  patchsz = ftell(f);
  fseek(f, 0, SEEK_SET);

  // Allocate memory for patch
  patchp = malloc(patchsz);
  if (patchp == NULL) {
    fprintf(stderr, "ERROR: Couldn't allocate memory for patch\n");
    fclose(f);
    return;
  }

  // Read patch file
  size_t bytes_read = fread(patchp, 1, patchsz, f);
  if ((size_t)bytes_read != (size_t)patchsz) {
    printf("ERROR: Couldn't read patch file (read %zu of %lld bytes)\n", 
           bytes_read, (long long)patchsz);
    free(patchp);
    fclose(f);
    return;
  }
  fclose(f);

  // Print first few bytes of patch file for debugging
  printf("Debug: First 8 bytes of patch file: ");
  for (int i = 0; i < 8; i++) {
    printf("%02x ", patchp[i]);
  }
  printf("\n");

  /* Apply delta */
  newsz = bspatch_newsize(patchp, patchsz);
  if (newsz <= 0) barf("Couldn't determine new file size; patch corrupt!");

  newp = malloc(newsz+1); /* Never malloc(0) */
  res = bspatch(inp, insz, newp, newsz, patchp, patchsz);
  if (res != 0) barf("bspatch() failed!");

  /* Write new file */
  write_file(outf, newp, newsz);

  free(inp);
  free(patchp);
  free(newp);

#ifndef NDEBUG
  printf("Successfully applied patch; new file is %s\n", outf);
#endif /* NDEBUG */
  exit(EXIT_SUCCESS);
}

static void
split_and_diff(const char* oldf, const char* newf, const char* patchf, int num_chunks)
{
  u_char *old_data, *new_data;
  long old_size, new_size;
  off_t patchsz, res;
  u_char* patch;
  
  printf("Splitting files into %d chunks and creating multi-patch\n", num_chunks);
  
  /* Read input files */
  old_size = read_file(oldf, &old_data);
  new_size = read_file(newf, &new_data);
  
  printf("Old file = %ld bytes\nNew file = %ld bytes\n", old_size, new_size);
  
  /* Calculate chunk sizes based on the NEW file size */
  off_t new_chunk_size = new_size / num_chunks;
  
  /* Ensure chunk sizes are at least 1 byte */
  if (new_chunk_size < 1) {
    printf("ERROR: New file too small to split into %d chunks\n", num_chunks);
    free(old_data);
    free(new_data);
    exit(EXIT_FAILURE);
  }
  
  /* Create temporary files for chunks */
  char **old_chunk_files = malloc(num_chunks * sizeof(char*));
  char **new_chunk_files = malloc(num_chunks * sizeof(char*));
  
  if (!old_chunk_files || !new_chunk_files) {
    printf("ERROR: Memory allocation failed\n");
    free(old_data);
    free(new_data);
    exit(EXIT_FAILURE);
  }
  
  /* Initialize pointers to NULL */
  for (int i = 0; i < num_chunks; i++) {
    old_chunk_files[i] = NULL;
    new_chunk_files[i] = NULL;
  }
  
  /* Split files into chunks and save to temporary files */
  for (int i = 0; i < num_chunks; i++) {
    /* Calculate chunk boundaries for NEW file */
    off_t new_start = i * new_chunk_size;
    off_t new_end = (i == num_chunks - 1) ? new_size : (i + 1) * new_chunk_size;
    
    /* Calculate corresponding boundaries in OLD file */
    /* Use the same proportional position in the old file */
    off_t old_start = (off_t)(((double)new_start / new_size) * old_size);
    off_t old_end;
    
    if (i == num_chunks - 1) {
      old_end = old_size; // Last chunk takes the remainder
    } else {
      old_end = (off_t)(((double)(i + 1) * new_chunk_size / new_size) * old_size);
    }
    
    /* Ensure we don't exceed file boundaries */
    if (new_start >= new_size) {
      printf("WARNING: Chunk %d exceeds new file size, skipping\n", i);
      continue;
    }
    
    /* Create temporary filenames */
    char old_temp[256], new_temp[256];
    sprintf(old_temp, "old_chunk_%d.tmp", i);
    sprintf(new_temp, "new_chunk_%d.tmp", i);
    
    /* Allocate memory for filenames */
    old_chunk_files[i] = malloc(strlen(old_temp) + 1);
    new_chunk_files[i] = malloc(strlen(new_temp) + 1);
    
    if (!old_chunk_files[i] || !new_chunk_files[i]) {
      printf("ERROR: Memory allocation failed for chunk filenames\n");
      exit(EXIT_FAILURE);
    }
    
    /* Copy filenames */
    strcpy(old_chunk_files[i], old_temp);
    strcpy(new_chunk_files[i], new_temp);
    
    /* Write chunks to temporary files */
    FILE *f = fopen(old_temp, "wb");
    if (!f) {
      printf("ERROR: Could not create temporary file %s\n", old_temp);
      exit(EXIT_FAILURE);
    }
    fwrite(old_data + old_start, 1, old_end - old_start, f);
    fclose(f);
    
    f = fopen(new_temp, "wb");
    if (!f) {
      printf("ERROR: Could not create temporary file %s\n", new_temp);
      exit(EXIT_FAILURE);
    }
    fwrite(new_data + new_start, 1, new_end - new_start, f);
    fclose(f);
    
    printf("Created chunk %d: old=%ld bytes, new=%ld bytes\n", 
           i, (long)(old_end - old_start), (long)(new_end - new_start));
  }
  
  /* Free original file data */
  free(old_data);
  free(new_data);
  
  /* Calculate a more generous patch size estimate */
  size_t estimated_patch_size = sizeof(multipatch_header) + 
                              (size_t)num_chunks * sizeof(patch_entry);
  
  /* Add estimated patch sizes */
  size_t chunk_size = (size_t)((new_size + num_chunks - 1) / num_chunks);
  estimated_patch_size += (size_t)num_chunks * bsdiff_patchsize_max(chunk_size, chunk_size);
  
  /* Add safety margin */
  estimated_patch_size += 1024 * num_chunks;
  
  /* Check for reasonable size limits */
  if (estimated_patch_size < sizeof(multipatch_header) || 
      estimated_patch_size > (size_t)(1024 * 1024 * 1024)) { /* 1GB limit */
    printf("ERROR: Patch size estimation overflow or too large (%zu bytes)\n", estimated_patch_size);
    exit(EXIT_FAILURE);
  }
  
  printf("Allocating %zu bytes for patch container\n", estimated_patch_size);
  
  patchsz = estimated_patch_size;
  
  patch = malloc(patchsz);
  if (!patch) {
    printf("ERROR: Could not allocate memory for patch\n");
    exit(EXIT_FAILURE);
  }
  
  /* Create multi-patch from chunks */
  res = create_multipatch((const char**)old_chunk_files, (const char**)new_chunk_files, 
                         num_chunks, patch, patchsz);
  if (res <= 0) {
    printf("ERROR: Failed to create multi-patch\n");
    free(patch);
    
    /* Clean up temporary files */
    for (int i = 0; i < num_chunks; i++) {
      if (old_chunk_files[i]) {
        remove(old_chunk_files[i]);
        free(old_chunk_files[i]);
      }
      if (new_chunk_files[i]) {
        remove(new_chunk_files[i]);
        free(new_chunk_files[i]);
      }
    }
    
    free(old_chunk_files);
    free(new_chunk_files);
    
    exit(EXIT_FAILURE);
  }
  
  patchsz = res;
  
  /* Write patch to file */
  FILE* f = fopen(patchf, "wb");
  if (!f) {
    printf("ERROR: Could not create patch file %s\n", patchf);
    free(patch);
    exit(EXIT_FAILURE);
  }
  
  size_t bytes_written = fwrite(patch, 1, patchsz, f);
  if ((size_t)bytes_written != (size_t)patchsz) {
    printf("ERROR: Could not write patch file (wrote %zu of %lld bytes)\n", 
           bytes_written, (long long)patchsz);
    fclose(f);
    free(patch);
    exit(EXIT_FAILURE);
  }
  
  fclose(f);
  free(patch);
  
  /* Clean up temporary files */
  for (int i = 0; i < num_chunks; i++) {
    remove(old_chunk_files[i]);
    remove(new_chunk_files[i]);
    free(old_chunk_files[i]);
    free(new_chunk_files[i]);
  }
  
  free(old_chunk_files);
  free(new_chunk_files);
  

  int ctrllen_hex = (max_ctrllen + 127) / 128;
  int eblen_hex = (max_eblen + 127) / 128;
  
  printf("Created multi-patch file %s with %d chunks (%lld bytes)\n", 
         patchf, num_chunks, (long long)patchsz);

  printf("4C0601740304%02X%02X\n", ctrllen_hex, eblen_hex);

  exit(EXIT_SUCCESS);
}

static void
multipatch(const char* inf, const char* patchf, const char* outf)
{
  u_char* patchp;
  off_t patchsz;
  
  printf("Applying multi-patch %s to %s\n", patchf, inf);
  
  /* Read patch file */
  FILE* f = fopen(patchf, "rb");
  if (f == NULL) {
    printf("ERROR: Couldn't open patch file %s\n", patchf);
    exit(EXIT_FAILURE);
  }
  
  fseek(f, 0, SEEK_END);
  patchsz = ftell(f);
  fseek(f, 0, SEEK_SET);
  
  patchp = malloc(patchsz);
  if (patchp == NULL) {
    printf("ERROR: Couldn't allocate memory for patch\n");
    fclose(f);
    exit(EXIT_FAILURE);
  }
  
  size_t bytes_read = fread(patchp, 1, patchsz, f);
  if ((size_t)bytes_read != (size_t)patchsz) {
    printf("ERROR: Couldn't read patch file (read %zu of %lld bytes)\n", 
           bytes_read, (long long)patchsz);
    free(patchp);
    fclose(f);
    exit(EXIT_FAILURE);
  }
  
  fclose(f);
  
  /* Validate multi-patch */
  if (!multipatch_valid(patchp, patchsz)) {
    printf("ERROR: Invalid multi-patch file\n");
    free(patchp);
    exit(EXIT_FAILURE);
  }
  
  /* Apply multi-patch */
  int res = apply_multipatch(inf, outf, patchp, patchsz);
  if (res != 0) {
    printf("ERROR: Failed to apply multi-patch\n");
    free(patchp);
    exit(EXIT_FAILURE);
  }
  
  free(patchp);
  
  printf("Successfully applied multi-patch; new file is %s\n", outf);
  exit(EXIT_SUCCESS);
}

/* ------------------------------------------------------------------------- */
/* -- Driver --------------------------------------------------------------- */

int
main(int ac, char* av[])
{
  /* WIN32 FIXME: av[0] becomes the full path to minibsdiff */
  progname = av[0];
  
  if (ac < 3) usage();

  if (memcmp(av[1], "gen", 3) == 0) {
    if (ac == 5) {
      // Standard patch generation
      diff(av[2], av[3], av[4]);
    } else if (ac == 7 && strcmp(av[5], "--mgen") == 0) {
      // Split files into chunks and create multi-patch
      int num_chunks = atoi(av[6]);
      if (num_chunks <= 0) usage();
      split_and_diff(av[2], av[3], av[4], num_chunks);
    } else {
      usage();
    }
  }
  
  if (memcmp(av[1], "app", 3) == 0) {
    if (ac != 5) usage();
    patch(av[2], av[3], av[4]);
  }
  
  if (memcmp(av[1], "mapp", 4) == 0) {
    if (ac != 5) usage();
    multipatch(av[2], av[3], av[4]);
  }

  usage(); /* patch()/diff() don't return */
  return 0;
}
