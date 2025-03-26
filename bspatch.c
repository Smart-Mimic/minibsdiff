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

#if 0
__FBSDID("$FreeBSD: src/usr.bin/bsdiff/bspatch/bspatch.c,v 1.1 2005/08/06 01:59:06 cperciva Exp $");
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include "bspatch.h"
#include "lz4.h"

/*
  Patch file format:
  0        8       BSDIFF_CONFIG_MAGIC (see minibsdiff-config.h)
  8        8       X
  16       8       Y
  24       8       sizeof(newfile)
  32       X       control block
  32+X     Y       diff block
  32+X+Y   ???     extra block
  with control block a set of triples (x,y,z) meaning "add x bytes
  from oldfile to x bytes from the diff block; copy y bytes from the
  extra block; seek forwards in oldfile by z bytes".
*/

static off_t
offtin(u_char *buf)
{
  off_t y;

  y=buf[7]&0x7F;
  y=y*256;y+=buf[6];
  y=y*256;y+=buf[5];
  y=y*256;y+=buf[4];
  y=y*256;y+=buf[3];
  y=y*256;y+=buf[2];
  y=y*256;y+=buf[1];
  y=y*256;y+=buf[0];

  if(buf[7]&0x80) y=-y;

  return y;
}

bool
bspatch_valid_header(u_char* patch, ssize_t patchsz)
{
  ssize_t newsize, ctrllen, datalen;

  if (patchsz < 32) return false;

  /* Make sure magic and header fields are valid */
  if (memcmp(patch, "MBSDIF43", 8) != 0 && memcmp(patch, "BSDIFF40", 8) != 0) {
    return false;
  }

  ctrllen=offtin(patch+8);
  datalen=offtin(patch+16);
  newsize=offtin(patch+24);
  if((ctrllen<0) || (datalen<0) || (newsize<0))
    return false;

  return true;
}

ssize_t
bspatch_newsize(u_char* patch, ssize_t patchsz)
{
  if (!bspatch_valid_header(patch, patchsz)) return -1;
  return offtin(patch+24);
}

int
bspatch(u_char* oldp, off_t oldsize,
        u_char* newp, off_t newsize,
        u_char* patch, off_t patchsize)
{
  u_char header[32], *ctrl_buf, *diff_buf, *extra_buf;
  off_t ctrl_len, diff_len, new_size;
  off_t oldpos, newpos;
  off_t ctrl[3];
  off_t i;
  int ret = 0;
  
  // Add declarations for decompression result variables
  int ctrl_decompressed_size, diff_decompressed_size, extra_decompressed_size;
  
  /* Sanity checks */
  if (oldp == NULL || newp == NULL || patch == NULL) {
    fprintf(stderr, "Error: NULL input pointer\n");
    return -1;
  }
  if (oldsize < 0 || newsize < 0 || patchsize < 0) {
    fprintf(stderr, "Error: Negative size parameter\n");
    return -1;
  }

  /* Read header */
  if (patchsize < 32) {
    fprintf(stderr, "Error: Patch too small (< 32 bytes)\n");
    return -1;
  }
  memcpy(header, patch, 32);
  
  // Print header bytes in hex
  fprintf(stderr, "Debug: Header bytes: ");
  for (i = 0; i < 8; i++) {
    fprintf(stderr, "%02x ", header[i]);
  }
  fprintf(stderr, "\n");

  /* Check for appropriate magic */
  // Try both the original bzip2 format and our new LZ4 format
  if (memcmp(header, "MBSDIF43", 8) != 0 && memcmp(header, "BSDIFF40", 8) != 0) {
    fprintf(stderr, "Error: Invalid patch magic\n");
    return -1;
  }

  /* Read lengths from header */
  ctrl_len = offtin(header + 8);
  diff_len = offtin(header + 16);
  new_size = offtin(header + 24);
  
  fprintf(stderr, "Debug: ctrl_len=%ld, diff_len=%ld, new_size=%ld\n", 
          (long )ctrl_len, (long )diff_len, (long )new_size);
  
  if (ctrl_len < 0 || diff_len < 0 || new_size < 0) {
    fprintf(stderr, "Error: Negative length in header\n");
    return -1;
  }
  
  /* Sanity check */
  if ((ctrl_len > patchsize - 32) ||
      (diff_len > patchsize - 32 - ctrl_len) ||
      (new_size != newsize)) {
    fprintf(stderr, "Error: Invalid patch sizes (ctrl_len=%ld, diff_len=%ld, patchsize=%ld, new_size=%ld, newsize=%ld)\n",
            (long)ctrl_len, (long)diff_len, (long)patchsize, 
            (long)new_size, (long)newsize);
    return -1;
  }
  
  /* Get pointers to the compressed data blocks */
  ctrl_buf = malloc(newsize);
  if (ctrl_buf == NULL) {
    fprintf(stderr, "Error: Failed to allocate memory for control buffer\n");
    return -1;
  }
  
  /* Allocate memory for decompressed diff data */
  diff_buf = malloc(newsize);
  if (diff_buf == NULL) {
    fprintf(stderr, "Error: Failed to allocate memory for diff buffer\n");
    free(ctrl_buf);
    return -1;
  }
  
  /* Allocate memory for decompressed extra data */
  extra_buf = malloc(newsize);
  if (extra_buf == NULL) {
    fprintf(stderr, "Error: Failed to allocate memory for extra buffer\n");
    free(diff_buf);
    free(ctrl_buf);
    return -1;
  }
  
  /* Decompress control data */
  ctrl_decompressed_size = LZ4_decompress_safe(
      (const char*)patch + 32, 
      (char*)ctrl_buf, 
      ctrl_len, 
      newsize * 3 * 8);
  
  if (ctrl_decompressed_size < 0) {
    fprintf(stderr, "Error: LZ4 decompression failed for control data: %d\n", ctrl_decompressed_size);
    free(extra_buf);
    free(diff_buf);
    free(ctrl_buf);
    return -1;
  }
  
  fprintf(stderr, "Debug: Control data decompressed size: %d\n", ctrl_decompressed_size);
  
  /* Decompress diff data */
  diff_decompressed_size = LZ4_decompress_safe(
      (const char*)patch + 32 + ctrl_len, 
      (char*)diff_buf, 
      diff_len, 
      newsize);
  
  if (diff_decompressed_size < 0) {
    fprintf(stderr, "Error: LZ4 decompression failed for diff data: %d\n", diff_decompressed_size);
    free(extra_buf);
    free(diff_buf);
    free(ctrl_buf);
    return -1;
  }
  
  fprintf(stderr, "Debug: Diff data decompressed size: %d\n", diff_decompressed_size);
  
  /* Decompress extra data */
  extra_decompressed_size = LZ4_decompress_safe(
      (const char*)patch + 32 + ctrl_len + diff_len, 
      (char*)extra_buf, 
      patchsize - 32 - ctrl_len - diff_len, 
      newsize);
  
  if (extra_decompressed_size < 0) {
    fprintf(stderr, "Error: LZ4 decompression failed for extra data: %d\n", extra_decompressed_size);
    free(extra_buf);
    free(diff_buf);
    free(ctrl_buf);
    return -1;
  }
  
  fprintf(stderr, "Debug: Extra data decompressed size: %d\n", extra_decompressed_size);
  
  /* Now apply the patch using the decompressed data */
  oldpos = 0;
  newpos = 0;
  
  u_char *ctrl_ptr = ctrl_buf;
  u_char *diff_ptr = diff_buf;
  u_char *extra_ptr = extra_buf;
  
  while (newpos < newsize) {
    /* Read control data */
    for (i = 0; i <= 2; i++) {
      ctrl[i] = offtin(ctrl_ptr);
      ctrl_ptr += 8;
    }
    
    fprintf(stderr, "Debug: Control triple: (%ld, %ld, %ld)\n", 
            (long)ctrl[0], (long)ctrl[1], (long)ctrl[2]);
    
    /* Sanity check */
    if (newpos + ctrl[0] > newsize ||
        oldpos + ctrl[0] > oldsize ||
        newpos + ctrl[1] > newsize) {
      fprintf(stderr, "Error: Invalid control data (newpos=%ld, ctrl[0]=%ld, oldpos=%ld, ctrl[1]=%ld, newsize=%ld, oldsize=%ld)\n",
              (long)newpos, (long)ctrl[0], (long)oldpos, 
              (long)ctrl[1], (long)newsize, (long)oldsize);
      ret = -1;
      goto out;
    }
    
    /* Add old data to diff string */
    for (i = 0; i < ctrl[0]; i++) {
      if ((oldpos + i >= 0) && (oldpos + i < oldsize)) {
        newp[newpos + i] = oldp[oldpos + i] + diff_ptr[i];
      } else {
        newp[newpos + i] = diff_ptr[i];
      }
    }
    
    /* Adjust pointers */
    diff_ptr += ctrl[0];
    newpos += ctrl[0];
    oldpos += ctrl[0];
    
    /* Sanity check */
    if (newpos + ctrl[1] > newsize) {
      fprintf(stderr, "Error: Invalid control data for extra block (newpos=%ld, ctrl[1]=%ld, newsize=%ld)\n",
              (long)newpos, (long)ctrl[1], (long)newsize);
      ret = -1;
      goto out;
    }
    
    /* Copy extra string */
    memcpy(newp + newpos, extra_ptr, ctrl[1]);
    
    /* Adjust pointers */
    extra_ptr += ctrl[1];
    newpos += ctrl[1];
    oldpos += ctrl[2];
  }
  
out:
  free(extra_buf);
  free(diff_buf);
  free(ctrl_buf);
  return ret;
}
