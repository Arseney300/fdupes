/* FDUPES Copyright (c) 1999-2002 Adrian Lopez

   Permission is hereby granted, free of charge, to any person
   obtaining a copy of this software and associated documentation files
   (the "Software"), to deal in the Software without restriction,
   including without limitation the rights to use, copy, modify, merge,
   publish, distribute, sublicense, and/or sell copies of the Software,
   and to permit persons to whom the Software is furnished to do so,
   subject to the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS 
   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF 
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. 
   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY 
   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, 
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE 
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE. */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <stdlib.h>
#ifndef OMIT_GETOPT_LONG
#include <getopt.h>
#endif
#include <string.h>
#include <errno.h>
#include <locale.h>
#define __USE_XOPEN
#include <wchar.h>
#define _XOPEN_SOURCE_EXTENDED
#include <ncursesw/ncurses.h>

#ifndef EXTERNAL_MD5
#include "md5/md5.h"
#endif

#define ISFLAG(a,b) ((a & b) == b)
#define SETFLAG(a,b) (a |= b)

#define F_RECURSE           0x0001
#define F_HIDEPROGRESS      0x0002
#define F_DSAMELINE         0x0004
#define F_FOLLOWLINKS       0x0008
#define F_DELETEFILES       0x0010
#define F_EXCLUDEEMPTY      0x0020
#define F_CONSIDERHARDLINKS 0x0040
#define F_SHOWSIZE          0x0080
#define F_OMITFIRST         0x0100
#define F_RECURSEAFTER      0x0200
#define F_NOPROMPT          0x0400
#define F_SUMMARIZEMATCHES  0x0800

char *program_name;

unsigned long flags = 0;

#define CHUNK_SIZE 8192

#define INPUT_SIZE 256

#define PARTIAL_MD5_SIZE 4096

/* 

TODO: Partial sums (for working with very large files).

typedef struct _signature
{
  md5_state_t state;
  md5_byte_t  digest[16];
} signature_t;

typedef struct _signatures
{
  int         num_signatures;
  signature_t *signatures;
} signatures_t;

*/

typedef struct _file {
  char *d_name;
  off_t size;
  char *crcpartial;
  char *crcsignature;
  dev_t device;
  ino_t inode;
  time_t mtime;
  int hasdupes; /* true only if file is first on duplicate chain */
  struct _file *duplicates;
  struct _file *next;
} file_t;

typedef struct _filetree {
  file_t *file; 
  struct _filetree *left;
  struct _filetree *right;
} filetree_t;

void errormsg(char *message, ...)
{
  va_list ap;

  va_start(ap, message);

  fprintf(stderr, "\r%40s\r%s: ", "", program_name);
  vfprintf(stderr, message, ap);
}

void escapefilename(char *escape_list, char **filename_ptr)
{
  int x;
  int tx;
  char *tmp;
  char *filename;

  filename = *filename_ptr;

  tmp = (char*) malloc(strlen(filename) * 2 + 1);
  if (tmp == NULL) {
    errormsg("out of memory!\n");
    exit(1);
  }

  for (x = 0, tx = 0; x < strlen(filename); x++) {
    if (strchr(escape_list, filename[x]) != NULL) tmp[tx++] = '\\';
    tmp[tx++] = filename[x];
  }

  tmp[tx] = '\0';

  if (x != tx) {
    *filename_ptr = realloc(*filename_ptr, strlen(tmp) + 1);
    if (*filename_ptr == NULL) {
      errormsg("out of memory!\n");
      exit(1);
    }
    strcpy(*filename_ptr, tmp);
  }
}

off_t filesize(char *filename) {
  struct stat s;

  if (stat(filename, &s) != 0) return -1;

  return s.st_size;
}

dev_t getdevice(char *filename) {
  struct stat s;

  if (stat(filename, &s) != 0) return 0;

  return s.st_dev;
}

ino_t getinode(char *filename) {
  struct stat s;
   
  if (stat(filename, &s) != 0) return 0;

  return s.st_ino;   
}

time_t getmtime(char *filename) {
  struct stat s;

  if (stat(filename, &s) != 0) return 0;

  return s.st_mtime;
}

wchar_t *wcsrstr(wchar_t *haystack, wchar_t *needle)
{
  wchar_t *found = 0;
  wchar_t *next = 0;

  found = wcsstr(haystack, needle);
  if (found)
  {
    do {
      next = wcsstr(found + 1, needle);
      if (next != 0)
        found = next;
    } while (next);

    return found;
  }

  return 0;
}

/* compare wide and multibyte strings */
int wcsmbcscmp(wchar_t *s1, char *s2)
{
  wchar_t *s2w;
  size_t size;
  int result;

  size = mbstowcs(0, s2, 0) + 1;

  s2w = (wchar_t*) malloc(size * sizeof(wchar_t));
  if (s2w == 0)
    return -1;

  mbstowcs(s2w, s2, size);

  result = wcscmp(s1, s2w);

  free(s2w);

  return result;
}

/* wide character needle is contained in multibyte haystack */
int wcsinmbcs(char *haystack, wchar_t *needle)
{
  wchar_t *haystackw;
  size_t size;
  int result;

  size = mbstowcs(0, haystack, 0) + 1;

  haystackw = (wchar_t*) malloc(size * sizeof(wchar_t));
  if (haystackw == 0)
    return -1;

  mbstowcs(haystackw, haystack, size);

  if (wcsstr(haystackw, needle) == 0)
    result = 0;
  else
    result = 1;

  free(haystackw);

  return result;
}

/* wide character needle at beginning of multibyte haystack */
int wcsbeginmbcs(char *haystack, wchar_t *needle)
{
  wchar_t *haystackw;
  size_t size;
  int result;

  size = mbstowcs(0, haystack, 0) + 1;

  haystackw = (wchar_t*) malloc(size * sizeof(wchar_t));
  if (haystackw == 0)
    return -1;

  mbstowcs(haystackw, haystack, size);

  if (wcsncmp(haystackw, needle, wcslen(needle)) == 0)
    result = 1;
  else
    result = 0;

  free(haystackw);

  return result;
}

/* wide character needle at end of multibyte haystack */
int wcsendsmbcs(char *haystack, wchar_t *needle)
{
  wchar_t *haystackw;
  size_t size;
  int result;

  size = mbstowcs(0, haystack, 0) + 1;

  haystackw = (wchar_t*) malloc(size * sizeof(wchar_t));
  if (haystackw == 0)
    return -1;

  mbstowcs(haystackw, haystack, size);

  if (wcsrstr(haystackw, needle) != 0 && wcscmp(wcsrstr(haystackw, needle), needle) == 0)
    result = 1;
  else
    result = 0;

  free(haystackw);

  return result;
}

char **cloneargs(int argc, char **argv)
{
  int x;
  char **args;

  args = (char **) malloc(sizeof(char*) * argc);
  if (args == NULL) {
    errormsg("out of memory!\n");
    exit(1);
  }

  for (x = 0; x < argc; x++) {
    args[x] = (char*) malloc(strlen(argv[x]) + 1);
    if (args[x] == NULL) {
      free(args);
      errormsg("out of memory!\n");
      exit(1);
    }

    strcpy(args[x], argv[x]);
  }

  return args;
}

int findarg(char *arg, int start, int argc, char **argv)
{
  int x;
  
  for (x = start; x < argc; x++)
    if (strcmp(argv[x], arg) == 0) 
      return x;

  return x;
}

/* Find the first non-option argument after specified option. */
int nonoptafter(char *option, int argc, char **oldargv, 
		      char **newargv, int optind) 
{
  int x;
  int targetind;
  int testind;
  int startat = 1;

  targetind = findarg(option, 1, argc, oldargv);
    
  for (x = optind; x < argc; x++) {
    testind = findarg(newargv[x], startat, argc, oldargv);
    if (testind > targetind) return x;
    else startat = testind;
  }

  return x;
}

int grokdir(char *dir, file_t **filelistp)
{
  DIR *cd;
  file_t *newfile;
  struct dirent *dirinfo;
  int lastchar;
  int filecount = 0;
  struct stat info;
  struct stat linfo;
  static int progress = 0;
  static char indicator[] = "-\\|/";

  cd = opendir(dir);

  if (!cd) {
    errormsg("could not chdir to %s\n", dir);
    return 0;
  }

  while ((dirinfo = readdir(cd)) != NULL) {
    if (strcmp(dirinfo->d_name, ".") && strcmp(dirinfo->d_name, "..")) {
      if (!ISFLAG(flags, F_HIDEPROGRESS)) {
	fprintf(stderr, "\rBuilding file list %c ", indicator[progress]);
	progress = (progress + 1) % 4;
      }

      newfile = (file_t*) malloc(sizeof(file_t));

      if (!newfile) {
	errormsg("out of memory!\n");
	closedir(cd);
	exit(1);
      } else newfile->next = *filelistp;

      newfile->device = 0;
      newfile->inode = 0;
      newfile->crcsignature = NULL;
      newfile->crcpartial = NULL;
      newfile->duplicates = NULL;
      newfile->hasdupes = 0;

      newfile->d_name = (char*)malloc(strlen(dir)+strlen(dirinfo->d_name)+2);

      if (!newfile->d_name) {
	errormsg("out of memory!\n");
	free(newfile);
	closedir(cd);
	exit(1);
      }

      strcpy(newfile->d_name, dir);
      lastchar = strlen(dir) - 1;
      if (lastchar >= 0 && dir[lastchar] != '/')
	strcat(newfile->d_name, "/");
      strcat(newfile->d_name, dirinfo->d_name);
      
      if (filesize(newfile->d_name) == 0 && ISFLAG(flags, F_EXCLUDEEMPTY)) {
	free(newfile->d_name);
	free(newfile);
	continue;
      }

      if (stat(newfile->d_name, &info) == -1) {
	free(newfile->d_name);
	free(newfile);
	continue;
      }

      if (lstat(newfile->d_name, &linfo) == -1) {
	free(newfile->d_name);
	free(newfile);
	continue;
      }

      if (S_ISDIR(info.st_mode)) {
	if (ISFLAG(flags, F_RECURSE) && (ISFLAG(flags, F_FOLLOWLINKS) || !S_ISLNK(linfo.st_mode)))
	  filecount += grokdir(newfile->d_name, filelistp);
	free(newfile->d_name);
	free(newfile);
      } else {
	if (S_ISREG(linfo.st_mode) || (S_ISLNK(linfo.st_mode) && ISFLAG(flags, F_FOLLOWLINKS))) {
	  *filelistp = newfile;
	  filecount++;
	} else {
	  free(newfile->d_name);
	  free(newfile);
	}
      }
    }
  }

  closedir(cd);

  return filecount;
}

#ifndef EXTERNAL_MD5

/* If EXTERNAL_MD5 is not defined, use L. Peter Deutsch's MD5 library. 
 */
char *getcrcsignatureuntil(char *filename, off_t max_read)
{
  int x;
  off_t fsize;
  off_t toread;
  md5_state_t state;
  md5_byte_t digest[16];  
  static md5_byte_t chunk[CHUNK_SIZE];
  static char signature[16*2 + 1]; 
  char *sigp;
  FILE *file;
   
  md5_init(&state);

 
  fsize = filesize(filename);
  
  if (max_read != 0 && fsize > max_read)
    fsize = max_read;

  file = fopen(filename, "rb");
  if (file == NULL) {
    errormsg("error opening file %s\n", filename);
    return NULL;
  }
 
  while (fsize > 0) {
    toread = (fsize % CHUNK_SIZE) ? (fsize % CHUNK_SIZE) : CHUNK_SIZE;
    if (fread(chunk, toread, 1, file) != 1) {
      errormsg("error reading from file %s\n", filename);
      fclose(file);
      return NULL;
    }
    md5_append(&state, chunk, toread);
    fsize -= toread;
  }

  md5_finish(&state, digest);

  sigp = signature;

  for (x = 0; x < 16; x++) {
    sprintf(sigp, "%02x", digest[x]);
    sigp = strchr(sigp, '\0');
  }

  fclose(file);

  return signature;
}

char *getcrcsignature(char *filename)
{
  return getcrcsignatureuntil(filename, 0);
}

char *getcrcpartialsignature(char *filename)
{
  return getcrcsignatureuntil(filename, PARTIAL_MD5_SIZE);
}

#endif /* [#ifndef EXTERNAL_MD5] */

#ifdef EXTERNAL_MD5

/* If EXTERNAL_MD5 is defined, use md5sum program to calculate signatures.
 */
char *getcrcsignature(char *filename)
{
  static char signature[256];
  char *command;
  char *separator;
  FILE *result;

  command = (char*) malloc(strlen(filename)+strlen(EXTERNAL_MD5)+2);
  if (command == NULL) {
    errormsg("out of memory\n");
    exit(1);
  }

  sprintf(command, "%s %s", EXTERNAL_MD5, filename);

  result = popen(command, "r");
  if (result == NULL) {
    errormsg("error invoking %s\n", EXTERNAL_MD5);
    exit(1);
  }
 
  free(command);

  if (fgets(signature, 256, result) == NULL) {
    errormsg("error generating signature for %s\n", filename);
    return NULL;
  }    
  separator = strchr(signature, ' ');
  if (separator) *separator = '\0';

  pclose(result);

  return signature;
}

#endif /* [#ifdef EXTERNAL_MD5] */

void purgetree(filetree_t *checktree)
{
  if (checktree->left != NULL) purgetree(checktree->left);
    
  if (checktree->right != NULL) purgetree(checktree->right);
    
  free(checktree);
}

void getfilestats(file_t *file)
{
  file->size = filesize(file->d_name);
  file->inode = getinode(file->d_name);
  file->device = getdevice(file->d_name);
  file->mtime = getmtime(file->d_name);
}

int registerfile(filetree_t **branch, file_t *file)
{
  getfilestats(file);

  *branch = (filetree_t*) malloc(sizeof(filetree_t));
  if (*branch == NULL) {
    errormsg("out of memory!\n");
    exit(1);
  }
  
  (*branch)->file = file;
  (*branch)->left = NULL;
  (*branch)->right = NULL;

  return 1;
}

file_t **checkmatch(filetree_t **root, filetree_t *checktree, file_t *file)
{
  int cmpresult;
  char *crcsignature;
  off_t fsize;

  /* If device and inode fields are equal one of the files is a 
     hard link to the other or the files have been listed twice 
     unintentionally. We don't want to flag these files as
     duplicates unless the user specifies otherwise.
  */    

  if (!ISFLAG(flags, F_CONSIDERHARDLINKS) && (getinode(file->d_name) == 
      checktree->file->inode) && (getdevice(file->d_name) ==
      checktree->file->device)) return NULL; 

  fsize = filesize(file->d_name);
  
  if (fsize < checktree->file->size) 
    cmpresult = -1;
  else 
    if (fsize > checktree->file->size) cmpresult = 1;
  else {
    if (checktree->file->crcpartial == NULL) {
      crcsignature = getcrcpartialsignature(checktree->file->d_name);
      if (crcsignature == NULL) return NULL;

      checktree->file->crcpartial = (char*) malloc(strlen(crcsignature)+1);
      if (checktree->file->crcpartial == NULL) {
	errormsg("out of memory\n");
	exit(1);
      }
      strcpy(checktree->file->crcpartial, crcsignature);
    }

    if (file->crcpartial == NULL) {
      crcsignature = getcrcpartialsignature(file->d_name);
      if (crcsignature == NULL) return NULL;

      file->crcpartial = (char*) malloc(strlen(crcsignature)+1);
      if (file->crcpartial == NULL) {
	errormsg("out of memory\n");
	exit(1);
      }
      strcpy(file->crcpartial, crcsignature);
    }

    cmpresult = strcmp(file->crcpartial, checktree->file->crcpartial);
    /*if (cmpresult != 0) errormsg("    on %s vs %s\n", file->d_name, checktree->file->d_name);*/

    if (cmpresult == 0) {
      if (checktree->file->crcsignature == NULL) {
	crcsignature = getcrcsignature(checktree->file->d_name);
	if (crcsignature == NULL) return NULL;

	checktree->file->crcsignature = (char*) malloc(strlen(crcsignature)+1);
	if (checktree->file->crcsignature == NULL) {
	  errormsg("out of memory\n");
	  exit(1);
	}
	strcpy(checktree->file->crcsignature, crcsignature);
      }

      if (file->crcsignature == NULL) {
	crcsignature = getcrcsignature(file->d_name);
	if (crcsignature == NULL) return NULL;

	file->crcsignature = (char*) malloc(strlen(crcsignature)+1);
	if (file->crcsignature == NULL) {
	  errormsg("out of memory\n");
	  exit(1);
	}
	strcpy(file->crcsignature, crcsignature);
      }

      cmpresult = strcmp(file->crcsignature, checktree->file->crcsignature);
      /*if (cmpresult != 0) errormsg("P   on %s vs %s\n", 
          file->d_name, checktree->file->d_name);
      else errormsg("P F on %s vs %s\n", file->d_name,
          checktree->file->d_name);
      printf("%s matches %s\n", file->d_name, checktree->file->d_name);*/
    }
  }

  if (cmpresult < 0) {
    if (checktree->left != NULL) {
      return checkmatch(root, checktree->left, file);
    } else {
      registerfile(&(checktree->left), file);
      return NULL;
    }
  } else if (cmpresult > 0) {
    if (checktree->right != NULL) {
      return checkmatch(root, checktree->right, file);
    } else {
      registerfile(&(checktree->right), file);
      return NULL;
    }
  } else 
  {
    getfilestats(file);
    return &checktree->file;
  }
}

/* Do a bit-for-bit comparison in case two different files produce the 
   same signature. Unlikely, but better safe than sorry. */

int confirmmatch(FILE *file1, FILE *file2)
{
  unsigned char c1 = 0;
  unsigned char c2 = 0;
  size_t r1;
  size_t r2;
  
  fseek(file1, 0, SEEK_SET);
  fseek(file2, 0, SEEK_SET);

  do {
    r1 = fread(&c1, sizeof(c1), 1, file1);
    r2 = fread(&c2, sizeof(c2), 1, file2);

    if (c1 != c2) return 0; /* file contents are different */
  } while (r1 && r2);
  
  if (r1 != r2) return 0; /* file lengths are different */

  return 1;
}

void summarizematches(file_t *files)
{
  int numsets = 0;
  double numbytes = 0.0;
  int numfiles = 0;
  file_t *tmpfile;

  while (files != NULL)
  {
    if (files->hasdupes)
    {
      numsets++;

      tmpfile = files->duplicates;
      while (tmpfile != NULL)
      {
	numfiles++;
	numbytes += files->size;
	tmpfile = tmpfile->duplicates;
      }
    }

    files = files->next;
  }

  if (numsets == 0)
    printf("No duplicates found.\n\n");
  else
  {
    if (numbytes < 1024.0)
      printf("%d duplicate files (in %d sets), occupying %.0f bytes.\n\n", numfiles, numsets, numbytes);
    else if (numbytes <= (1000.0 * 1000.0))
      printf("%d duplicate files (in %d sets), occupying %.1f kylobytes\n\n", numfiles, numsets, numbytes / 1000.0);
    else
      printf("%d duplicate files (in %d sets), occupying %.1f megabytes\n\n", numfiles, numsets, numbytes / (1000.0 * 1000.0));
 
  }
}

void printmatches(file_t *files)
{
  file_t *tmpfile;

  while (files != NULL) {
    if (files->hasdupes) {
      if (!ISFLAG(flags, F_OMITFIRST)) {
	if (ISFLAG(flags, F_SHOWSIZE)) printf("%lld byte%seach:\n", files->size,
	 (files->size != 1) ? "s " : " ");
	if (ISFLAG(flags, F_DSAMELINE)) escapefilename("\\ ", &files->d_name);
	printf("%s%c", files->d_name, ISFLAG(flags, F_DSAMELINE)?' ':'\n');
      }
      tmpfile = files->duplicates;
      while (tmpfile != NULL) {
	if (ISFLAG(flags, F_DSAMELINE)) escapefilename("\\ ", &tmpfile->d_name);
	printf("%s%c", tmpfile->d_name, ISFLAG(flags, F_DSAMELINE)?' ':'\n');
	tmpfile = tmpfile->duplicates;
      }
      printf("\n");

    }
      
    files = files->next;
  }
}

/*
#define REVISE_APPEND "_tmp"
char *revisefilename(char *path, int seq)
{
  int digits;
  char *newpath;
  char *scratch;
  char *dot;

  digits = numdigits(seq);
  newpath = malloc(strlen(path) + strlen(REVISE_APPEND) + digits + 1);
  if (!newpath) return newpath;

  scratch = malloc(strlen(path) + 1);
  if (!scratch) return newpath;

  strcpy(scratch, path);
  dot = strrchr(scratch, '.');
  if (dot) 
  {
    *dot = 0;
    sprintf(newpath, "%s%s%d.%s", scratch, REVISE_APPEND, seq, dot + 1);
  }

  else
  {
    sprintf(newpath, "%s%s%d", path, REVISE_APPEND, seq);
  }

  free(scratch);

  return newpath;
} */

int relink(char *oldfile, char *newfile)
{
  dev_t od;
  dev_t nd;
  ino_t oi;
  ino_t ni;

  od = getdevice(oldfile);
  oi = getinode(oldfile);

  if (link(oldfile, newfile) != 0)
    return 0;

  /* make sure we're working with the right file (the one we created) */
  nd = getdevice(newfile);
  ni = getinode(newfile);

  if (nd != od || oi != ni)
    return 0; /* file is not what we expected */

  return 1;
}

void deletefiles(file_t *files, int prompt, FILE *tty)
{
  int counter;
  int groups = 0;
  int curgroup = 0;
  file_t *tmpfile;
  file_t *curfile;
  file_t **dupelist;
  int *preserve;
  char *preservestr;
  char *token;
  char *tstr;
  int number;
  int sum;
  int max = 0;
  int x;
  int i;

  curfile = files;
  
  while (curfile) {
    if (curfile->hasdupes) {
      counter = 1;
      groups++;

      tmpfile = curfile->duplicates;
      while (tmpfile) {
	counter++;
	tmpfile = tmpfile->duplicates;
      }
      
      if (counter > max) max = counter;
    }
    
    curfile = curfile->next;
  }

  max++;

  dupelist = (file_t**) malloc(sizeof(file_t*) * max);
  preserve = (int*) malloc(sizeof(int) * max);
  preservestr = (char*) malloc(INPUT_SIZE);

  if (!dupelist || !preserve || !preservestr) {
    errormsg("out of memory\n");
    exit(1);
  }

  while (files) {
    if (files->hasdupes) {
      curgroup++;
      counter = 1;
      dupelist[counter] = files;

      if (prompt) printf("[%d] %s\n", counter, files->d_name);

      tmpfile = files->duplicates;

      while (tmpfile) {
	dupelist[++counter] = tmpfile;
	if (prompt) printf("[%d] %s\n", counter, tmpfile->d_name);
	tmpfile = tmpfile->duplicates;
      }

      if (prompt) printf("\n");

      if (!prompt) /* preserve only the first file */
      {
         preserve[1] = 1;
	 for (x = 2; x <= counter; x++) preserve[x] = 0;
      }

      else /* prompt for files to preserve */

      do {
	printf("Set %d of %d, preserve files [1 - %d, all]", 
          curgroup, groups, counter);
	if (ISFLAG(flags, F_SHOWSIZE)) printf(" (%lld byte%seach)", files->size,
	  (files->size != 1) ? "s " : " ");
	printf(": ");
	fflush(stdout);

	if (!fgets(preservestr, INPUT_SIZE, tty))
	  preservestr[0] = '\n'; /* treat fgets() failure as if nothing was entered */

	i = strlen(preservestr) - 1;

	while (preservestr[i]!='\n'){ /* tail of buffer must be a newline */
	  tstr = (char*)
	    realloc(preservestr, strlen(preservestr) + 1 + INPUT_SIZE);
	  if (!tstr) { /* couldn't allocate memory, treat as fatal */
	    errormsg("out of memory!\n");
	    exit(1);
	  }

	  preservestr = tstr;
	  if (!fgets(preservestr + i + 1, INPUT_SIZE, tty))
	  {
	    preservestr[0] = '\n'; /* treat fgets() failure as if nothing was entered */
	    break;
	  }
	  i = strlen(preservestr)-1;
	}

	for (x = 1; x <= counter; x++) preserve[x] = 0;
	
	token = strtok(preservestr, " ,\n");
	
	while (token != NULL) {
	  if (strcasecmp(token, "all") == 0)
	    for (x = 0; x <= counter; x++) preserve[x] = 1;
	  
	  number = 0;
	  sscanf(token, "%d", &number);
	  if (number > 0 && number <= counter) preserve[number] = 1;
	  
	  token = strtok(NULL, " ,\n");
	}
      
	for (sum = 0, x = 1; x <= counter; x++) sum += preserve[x];
      } while (sum < 1); /* make sure we've preserved at least one file */

      printf("\n");

      for (x = 1; x <= counter; x++) { 
	if (preserve[x])
	  printf("   [+] %s\n", dupelist[x]->d_name);
	else {
	  if (remove(dupelist[x]->d_name) == 0) {
	    printf("   [-] %s\n", dupelist[x]->d_name);
	  } else {
	    printf("   [!] %s ", dupelist[x]->d_name);
	    printf("-- unable to delete file!\n");
	  }
	}
      }
      printf("\n");
    }
    
    files = files->next;
  }

  free(dupelist);
  free(preserve);
  free(preservestr);
}

int sort_pairs_by_arrival(file_t *f1, file_t *f2)
{
  if (f2->duplicates != 0)
    return 1;

  return -1;
}

int sort_pairs_by_mtime(file_t *f1, file_t *f2)
{
  if (f1->mtime < f2->mtime)
    return -1;
  else if (f1->mtime > f2->mtime)
    return 1;

  return 0;
}

void putline(WINDOW *window, const char *str, const int line, const int columns, const int compensate_indent)
{
  wchar_t *dest = 0;
  int inputlength;
  int linestart;
  int linelength;
  int linewidth;
  int first_line_columns;
  int l;

  inputlength = mbstowcs(0, str, 0);
  if (inputlength == 0)
    return;

  dest = (wchar_t *) malloc((inputlength + 1) * sizeof(wchar_t));
  if (dest == NULL)
  {
    endwin();
    errormsg("out of memory\n");
    exit(1);
  }

  mbstowcs(dest, str, inputlength);
  dest[inputlength] = L'\0';

  first_line_columns = columns - compensate_indent;

  linestart = 0;

  if (line > 0)
  {
    linewidth = wcwidth(dest[linestart]);

    while (linestart + 1 < inputlength && linewidth + wcwidth(dest[linestart + 1]) <= first_line_columns)
      linewidth += wcwidth(dest[++linestart]);

    if (++linestart == inputlength)
      return;

    for (l = 1; l < line; ++l)
    {
      linewidth = wcwidth(dest[linestart]);

      while (linestart + 1 < inputlength && linewidth + wcwidth(dest[linestart + 1]) <= columns)
        linewidth += wcwidth(dest[++linestart]);

      if (++linestart == inputlength)
        return;
    }
  }

  linewidth = wcwidth(dest[linestart]);
  linelength = 1;

  if (line == 0)
  {
    while (linestart + linelength < inputlength && linewidth + wcwidth(dest[linestart + linelength]) <= first_line_columns)
    {
      linewidth += wcwidth(dest[linestart + linelength]);
      ++linelength;
    }
  }
  else
  {
    while (linestart + linelength < inputlength && linewidth + wcwidth(dest[linestart + linelength]) <= columns)
    {
      linewidth += wcwidth(dest[linestart + linelength]);
      ++linelength;
    }    
  }

  waddnwstr(window, dest + linestart, linelength);

  free(dest);
}

struct groupfile
{
  file_t *file;
  int action;
  int selected;
};

struct filegroup
{
  struct groupfile *files;
  size_t filecount;
  int startline;
  int endline;
  int selected;
};

enum linestyle
{
  linestyle_groupheader = 0,
  linestyle_groupheaderspacing,
  linestyle_filename,
  linestyle_groupfooterspacing
};

enum linestyle getlinestyle(struct filegroup *group, int line)
{
  if (line <= group->startline)
    return linestyle_groupheader;
  else if (line == group->startline + 1)
    return linestyle_groupheaderspacing;
  else if (line >= group->endline)
    return linestyle_groupfooterspacing;
  else
    return linestyle_filename;
}

int filerowcount(file_t *file, const int columns, const int first_line_indent)
{
  int lines = 1;
  int line_remaining;
  int x = 0;
  size_t read;
  size_t filename_bytes;
  wchar_t ch;
  mbstate_t mbstate;

  memset(&mbstate, 0, sizeof(mbstate));

  filename_bytes = strlen(file->d_name);

  line_remaining = columns - first_line_indent;

  while (x < filename_bytes)
  {
    read = mbrtowc(&ch, file->d_name + x, filename_bytes - x, &mbstate);
    if (read < 0)
      return lines;

    x += read;

    if (wcwidth(ch) <= line_remaining)
    {
      line_remaining -= wcwidth(ch);
    }
    else
    {
      line_remaining = columns - wcwidth(ch);
      ++lines;
    }
  }

  return lines;
}

int getgroupindex(struct filegroup *groups, int group_count, int group_hint, int line)
{
  int group = group_hint;

  while (group > 0 && line < groups[group].startline)
    --group;

  while (group < group_count && line > groups[group].endline)
    ++group;

  return group;
}

int getgroupfileindex(int *row, struct filegroup *group, int line, int columns, int first_line_indent)
{
  int l;
  int f = 0;
  int rowcount;

  l = group->startline + 2;

  while (f < group->filecount)
  {
    rowcount = filerowcount(group->files[f].file, columns, first_line_indent);

    if (line <= l + rowcount - 1)
    {
      *row = line - l;
      return f;
    }

    l += rowcount;
    ++f;
  }

  return -1;
}

#define COMMAND_NAME_MAX 32

/* get command and arguments from user input */
int getcommand(wchar_t *command, int command_name_max, wchar_t **arguments, wchar_t *input)
{
  int x;

  for (x = 0; x < command_name_max; ++x)
  {
    if (input[x] == L' ' || input[x] == L'\0')
    {
      command[x] = L'\0';
      break;
    }
    else
    {
      command[x] = input[x];
    }
  }

  if (input[x] == L' ')
    *arguments = input + x + 1;
  else if (input[x] == L'\0')
    *arguments = input + x;
  else
    return 0;

  return 1;
}

/* select files containing string */
int cmd_select_containing(struct filegroup *groups, int groupcount, wchar_t *commandarguments)
{
  int g;
  int f;

  if (wcscmp(commandarguments, L"") != 0)
  {
    for (g = 0; g < groupcount; ++g)
    {
      for (f = 0; f < groups[g].filecount; ++f)
      {
        if (wcsinmbcs(groups[g].files[f].file->d_name, commandarguments))
        {
          groups[g].selected = 1;
          groups[g].files[f].selected = 1;
        }
      }
    }
  }

  return 1;
}

/* select files beginning with string */
int cmd_select_beginning(struct filegroup *groups, int groupcount, wchar_t *commandarguments)
{
  int g;
  int f;

  if (wcscmp(commandarguments, L"") != 0)
  {
    for (g = 0; g < groupcount; ++g)
    {
      for (f = 0; f < groups[g].filecount; ++f)
      {
        if (wcsbeginmbcs(groups[g].files[f].file->d_name, commandarguments))
        {
          groups[g].selected = 1;
          groups[g].files[f].selected = 1;
        }
      }
    }
  }

  return 1;
}

/* select files ending with string */
int cmd_select_ending(struct filegroup *groups, int groupcount, wchar_t *commandarguments)
{
  int g;
  int f;

  if (wcscmp(commandarguments, L"") != 0)
  {
    for (g = 0; g < groupcount; ++g)
    {
      for (f = 0; f < groups[g].filecount; ++f)
      {
        if (wcsendsmbcs(groups[g].files[f].file->d_name, commandarguments))
        {
          groups[g].selected = 1;
          groups[g].files[f].selected = 1;
        }
      }
    }
  }

  return 1;
}

/* select files matching string */
int cmd_select_matching(struct filegroup *groups, int groupcount, wchar_t *commandarguments)
{
  int g;
  int f;

  if (wcscmp(commandarguments, L"") != 0)
  {
    for (g = 0; g < groupcount; ++g)
    {
      for (f = 0; f < groups[g].filecount; ++f)
      {
        if (wcsmbcscmp(commandarguments, groups[g].files[f].file->d_name) == 0)
        {
          groups[f].selected = 1;
          groups[g].files[f].selected = 1;
        }
      }
    }
  }

  return 1;
}

/* clear selections containing string */
int cmd_clear_selections_containing(struct filegroup *groups, int groupcount, wchar_t *commandarguments)
{
  int g;
  int f;

  if (wcscmp(commandarguments, L"") != 0)
  {
    for (g = 0; g < groupcount; ++g)
    {
      for (f = 0; f < groups[g].filecount; ++f)
        if (wcsinmbcs(groups[g].files[f].file->d_name, commandarguments))
          groups[g].files[f].selected = 0;
    }
  }

  return 1;
}

/* clear selections beginning with string */
int cmd_clear_selections_beginning(struct filegroup *groups, int groupcount, wchar_t *commandarguments)
{
  int g;
  int f;

  if (wcscmp(commandarguments, L"") != 0)
  {
    for (g = 0; g < groupcount; ++g)
    {
      for (f = 0; f < groups[g].filecount; ++f)
        if (wcsbeginmbcs(groups[g].files[f].file->d_name, commandarguments))
          groups[g].files[f].selected = 0;
    }
  }

  return 1;
}

/* clear selections ending with string */
int cmd_clear_selections_ending(struct filegroup *groups, int groupcount, wchar_t *commandarguments)
{
  int g;
  int f;

  if (wcscmp(commandarguments, L"") != 0)
  {
    for (g = 0; g < groupcount; ++g)
    {
      for (f = 0; f < groups[g].filecount; ++f)
      {
        if (wcsendsmbcs(groups[g].files[f].file->d_name, commandarguments))
          groups[g].files[f].selected = 0;
      }
    }
  }

  return 1;
}

/* clear selections matching string */
int cmd_clear_selections_matching(struct filegroup *groups, int groupcount, wchar_t *commandarguments)
{
  int g;
  int f;

  if (wcscmp(commandarguments, L"") != 0)
  {
    for (g = 0; g < groupcount; ++g)
    {
      for (f = 0; f < groups[g].filecount; ++f)
        if (wcsmbcscmp(commandarguments, groups[g].files[f].file->d_name) == 0)
          groups[g].files[f].selected = 0;
    }
  }

  return 1;
}

/* clear all selections and selected groups */
int cmd_clear_all_selections(struct filegroup *groups, int groupcount, wchar_t *commandarguments)
{
  int g;
  int f;

  for (g = 0; g < groupcount; ++g)
  {
    for (f = 0; f < groups[g].filecount; ++f)
      groups[g].files[f].selected = 0;

    groups[g].selected = 0;
  }

  return 1;
}

/* invert selections within selected groups */
int cmd_invert_group_selections(struct filegroup *groups, int groupcount, wchar_t *commandarguments)
{
  int g;
  int f;

  for (g = 0; g < groupcount; ++g)
    if (groups[g].selected)
      for (f = 0; f < groups[g].filecount; ++f)
        groups[g].files[f].selected = !groups[g].files[f].selected;

  return 1;
}

/* mark selected files for preservation */
int cmd_keep_selected(struct filegroup *groups, int groupcount, wchar_t *commandarguments)
{
  int g;
  int f;

  for (g = 0; g < groupcount; ++g)
    for (f = 0; f < groups[g].filecount; ++f)
      if (groups[g].files[f].selected)
        groups[g].files[f].action = 1;

  return 1;
}

/* mark selected files for deletion */
int cmd_delete_selected(struct filegroup *groups, int groupcount, wchar_t *commandarguments)
{
  int g;
  int f;

  for (g = 0; g < groupcount; ++g)
    for (f = 0; f < groups[g].filecount; ++f)
      if (groups[g].files[f].selected)
        groups[g].files[f].action = -1;

  return 1;
}

/* mark selected files as unresolved */
int cmd_reset_selected(struct filegroup *groups, int groupcount, wchar_t *commandarguments)
{
  int g;
  int f;

  for (g = 0; g < groupcount; ++g)
    for (f = 0; f < groups[g].filecount; ++f)
      if (groups[g].files[f].selected)
        groups[g].files[f].action = 0;

  return 1;
}

struct command_map {
  wchar_t *command_name;
  int (*command)(struct filegroup *groups, int groupcount, wchar_t *commandarguments);
} command_list[] = {
  {L"s*", cmd_select_containing},
  {L"s[", cmd_select_beginning},
  {L"s]", cmd_select_ending},
  {L"s[]", cmd_select_matching},
  {L"cs*", cmd_clear_selections_containing},
  {L"cs[", cmd_clear_selections_beginning},
  {L"cs]", cmd_clear_selections_ending},
  {L"cs[]", cmd_clear_selections_matching},
  {L"ca", cmd_clear_all_selections},
  {L"ig", cmd_invert_group_selections},
  {L"+", cmd_keep_selected},
  {L"-", cmd_delete_selected},
  {L"x", cmd_reset_selected},
  {0, 0}
};

int dispatchcommand(wchar_t *commandname, wchar_t *commandarguments, struct filegroup *groups, int groupcount)
{
  int c = 0;

  while (command_list[c].command != 0)
  {
    if (wcscmp(commandname, command_list[c].command_name) == 0)
      return command_list[c].command(groups, groupcount, commandarguments);
    ++c;
  }

  return 0;
}

#define FILENAME_INDENT 4

#define MODE_ARROWSELECT 1
#define MODE_NUMBERSELECT 2

void deletefiles_ncurses(file_t *files)
{
  WINDOW *filewin;
  WINDOW *statuswin;
  file_t *curfile;
  file_t *dupefile;
  struct filegroup *groups;
  struct filegroup *reallocgroups;
  size_t groupfilecount;
  int topline = 0;
  int cursorgroup = 0;
  int cursorfile = 0;
  int groupfirstline = 0;
  int totallines = 0;
  int allocatedgroups = 0;
  int totalgroups = 0;
  size_t groupindex = 0;
  enum linestyle linestyle;
  int preservecount;
  int deletecount;
  int unresolvedcount;
  int mode = MODE_ARROWSELECT;
  int row;
  int x;
  int g;
  wint_t wch;
  int keyresult;
  int cy;
  int f;
  int to;
  wchar_t *commandbuffer;
  wchar_t *realloccommandbuffer;
  size_t commandbuffersize;
  wchar_t command[COMMAND_NAME_MAX];
  wchar_t *commandarguments;
  int docommandinput;
  int doprune;
  size_t length;

  initscr();
  noecho();
  cbreak();

  filewin = newwin(LINES - 1, COLS, 0, 0);	
  statuswin = newwin(1, COLS, LINES - 1, 0);

  scrollok(filewin, FALSE);

  wattron(statuswin, A_REVERSE);

  keypad(statuswin, 1);

  commandbuffersize = 80;
  commandbuffer = malloc(commandbuffersize * sizeof(wchar_t));
  if (commandbuffer == 0)
  {
    endwin();
    errormsg("out of memory\n");
    exit(1);
  }

  allocatedgroups = 1024;
  groups = malloc(sizeof(struct filegroup) * allocatedgroups);
  if (groups == 0)
  {
    endwin();
    errormsg("out of memory\n");
    exit(1);
  }

  curfile = files;
  while (curfile)
  {
    if (!curfile->hasdupes)
    {
      curfile = curfile->next;
      continue;
    }

    if (totalgroups + 1 > allocatedgroups)
    {
      allocatedgroups *= 2;

      reallocgroups = realloc(groups, sizeof(struct filegroup) * allocatedgroups);
      if (reallocgroups == 0)
      {
        free(groups);

        endwin();
        errormsg("out of memory\n");
        exit(1);
      }

      groups = reallocgroups;
    }

    groups[totalgroups].startline = groupfirstline;
    groups[totalgroups].endline = groupfirstline + 2;
    groups[totalgroups].selected = 0;

    groupfilecount = 0;

    dupefile = curfile;
    do
    {
      groups[totalgroups].endline += filerowcount(dupefile, COLS, FILENAME_INDENT);

      ++groupfilecount;

      dupefile = dupefile->duplicates;
    } while (dupefile);

    groups[totalgroups].files = malloc(sizeof(struct groupfile) * groupfilecount);
    if (groups[totalgroups].files == 0)
    {
      free(groups);

      endwin();
      errormsg("out of memory\n");
      exit(1);
    }

    groupfilecount = 0;

    dupefile = curfile;
    do
    {
      groups[totalgroups].files[groupfilecount].file = dupefile;
      groups[totalgroups].files[groupfilecount].action = 0;
      groups[totalgroups].files[groupfilecount].selected = 0;
      ++groupfilecount;

      dupefile = dupefile->duplicates;
    } while (dupefile);

    groups[totalgroups].filecount = groupfilecount;

    groupfirstline = groups[totalgroups].endline + 1;

    ++totalgroups;

    curfile = curfile->next;
  }

  doprune = 1;
  do
  {
    wmove(filewin, 0, 0);
    werase(filewin);

    totallines = groups[totalgroups-1].endline;

    for (x = topline; x < topline + LINES - 1; ++x)
    {
      if (x >= totallines)
      {
        wclrtoeol(filewin);
        continue;
      }

      groupindex = getgroupindex(groups, totalgroups, groupindex, x);

      linestyle = getlinestyle(groups + groupindex, x);
      
      if (linestyle == linestyle_groupheader)
      {
        wattron(filewin, A_BOLD);
        if (groups[groupindex].selected)
          wattron(filewin, A_REVERSE);
        wprintw(filewin, "Set %d of %d:\n", groupindex + 1, totalgroups);
        if (groups[groupindex].selected)
          wattroff(filewin, A_REVERSE);
        wattroff(filewin, A_BOLD);
      }
      else if (linestyle == linestyle_groupheaderspacing)
      {
        wprintw(filewin, "\n");
      }
      else if (linestyle == linestyle_filename)
      {
        f = getgroupfileindex(&row, groups + groupindex, x, COLS, FILENAME_INDENT);

        if (mode == MODE_ARROWSELECT || cursorgroup != groupindex)
        {
          if (row == 0)
          {
            if (cursorgroup == groupindex && cursorfile == f)
              wprintw(filewin, "> ");
            else
              wprintw(filewin, "  ");

            wprintw(filewin, "%c ", groups[groupindex].files[f].action > 0 ? '+' : groups[groupindex].files[f].action < 0 ? '-' : ' ');
          }

          cy = getcury(filewin);

          if (groups[groupindex].files[f].selected)
            wattron(filewin, A_REVERSE);
          putline(filewin, groups[groupindex].files[f].file->d_name, row, COLS, FILENAME_INDENT);
          if (groups[groupindex].files[f].selected)
            wattroff(filewin, A_REVERSE);

          wclrtoeol(filewin);
          wmove(filewin, cy+1, 0);
        }
        else
        {
          wprintw(filewin, "[%d] %s\n", f + 1,  groups[groupindex].files[f].file->d_name);
        }
      }
      else if (linestyle == linestyle_groupfooterspacing)
      {
        wprintw(filewin, "\n");
      }
    }

    wnoutrefresh(filewin);

    werase(statuswin);
    wprintw(statuswin, "h for help");
    wnoutrefresh(statuswin);

    doupdate();

    /* wait for user input */
    keyresult = wget_wch(statuswin, &wch);

    if (keyresult == OK && wch == ':') /* enter command input mode */
    {
      commandbuffer[0] = L'\0';

      docommandinput = 1;
      do
      {
        /* draw command buffer to status window */
        wattroff(statuswin, A_REVERSE);

        werase(statuswin);
        wprintw(statuswin, ":%ls", commandbuffer);
        wnoutrefresh(statuswin);

        doupdate();

        /* get next character */
        keyresult = wget_wch(statuswin, &wch);

        if (keyresult == OK)
        {
          if (wch != '\n') /* add character to buffer */
          {
            /* increase command buffer, if necessary */
            length = wcslen(commandbuffer);

            if (length + 1 >= commandbuffersize)
            {
              commandbuffersize *= 2;

              realloccommandbuffer = (wchar_t*) realloc(commandbuffer, commandbuffersize * sizeof(wchar_t));
              if (realloccommandbuffer == 0)
              {
                free(commandbuffer);
                free(groups);

                endwin();
                errormsg("out of memory\n");
                exit(1);
              }

              commandbuffer = realloccommandbuffer;
            }

            /* append character to command buffer */
            commandbuffer[length] = wch;
            commandbuffer[length+1] = L'\0';
          }
          else /* process command */
          {
            if (getcommand(command, COMMAND_NAME_MAX, &commandarguments, commandbuffer))
            {
              if (!dispatchcommand(command, commandarguments, groups, totalgroups))
              {
                if (wcscmp(command, L"q") == 0 || wcscmp(command, L"quit") == 0 || wcscmp(command, L"exit") == 0)
                {
                  /* exit program */
                  docommandinput = 0;
                  doprune = 0;
                  continue;
                }
              }
            }

            /* exit command mode */
            wattron(statuswin, A_REVERSE);
            docommandinput = 0;

            continue;
          }
        }
        else if (keyresult == KEY_CODE_YES)
        {
          switch (wch)
          {
          case KEY_BACKSPACE:
            /* erase last character */
            length = wcslen(commandbuffer);

            if (length == 1)
            {
              wattron(statuswin, A_REVERSE);
              commandbuffer[0] = '\0';
              docommandinput = 0;
              continue;
            }

            commandbuffer[length-1] = '\0';

            break;

          case KEY_RESIZE:
              /* resize windows */
              wresize(filewin, LINES - 1, COLS);

              wresize(statuswin, 1, COLS);
              mvwin(statuswin, LINES - 1, 0);

              /* recalculate line boundaries */
              groupfirstline = 0;

              for (g = 0; g < totalgroups; ++g)
              {
                groups[g].startline = groupfirstline;
                groups[g].endline = groupfirstline + 2;

                for (f = 0; f < groups[g].filecount; ++f)
                  groups[g].endline += filerowcount(groups[g].files[f].file, COLS, FILENAME_INDENT);

                groupfirstline = groups[g].endline + 1;
              }

              /* exit command mode */
              wattron(statuswin, A_REVERSE);
              docommandinput = 0;

              continue;

              break;
          }
        }
      } while (docommandinput);

      /* redraw file list and wait for input */
      continue;
    }

    /* handle keypress */
    switch (wch)
    {
    case KEY_DOWN:
      if (keyresult != KEY_CODE_YES)
        break;

      if (cursorfile < groups[cursorgroup].filecount - 1)
      {
        ++cursorfile;
      }
      else
      {
        if (cursorgroup < totalgroups - 1)
        {
          ++cursorgroup;
          cursorfile = 0;
        }
      }
      break;
    
    case KEY_UP:
      if (keyresult != KEY_CODE_YES)
        break;

      if (cursorfile > 0)
      {
        --cursorfile;
      }
      else
      {
        if (cursorgroup > 0)
        {
          --cursorgroup;
          cursorfile = groups[cursorgroup].filecount - 1;
        }
      }
      break;

    case ']':
      ++topline;
      break;

    case '[':
      if (topline > 0)
        --topline;
      break;

    case KEY_RIGHT:
      if (keyresult != KEY_CODE_YES)
        break;

      groups[cursorgroup].files[cursorfile].action = 1;

      if (cursorfile < groups[cursorgroup].filecount - 1)
      {
        ++cursorfile;
      }
      else
      {
        if (cursorgroup < totalgroups - 1)
        {
          ++cursorgroup;
          cursorfile = 0;
        }
      }

      break;

    case 'a':
    case 'A':
      deletecount = 0;

      for (x = 0; x < groups[cursorgroup].filecount; ++x)
      {
        if (groups[cursorgroup].files[x].action == 0)
          groups[cursorgroup].files[x].action = 1;
        else if (groups[cursorgroup].files[x].action == -1)
          ++deletecount;
      }

      if (cursorgroup < totalgroups - 1 && deletecount < groups[cursorgroup].filecount)
      {
        ++cursorgroup;
        cursorfile = 0;
      }

      break;

    case KEY_LEFT:
      if (keyresult != KEY_CODE_YES)
        break;

      deletecount = 0;

      groups[cursorgroup].files[cursorfile].action = -1;

      for (x = 0; x < groups[cursorgroup].filecount; ++x)
        if (groups[cursorgroup].files[x].action == -1)
          ++deletecount;
 
      if (deletecount < groups[cursorgroup].filecount)
      {
        if (cursorfile < groups[cursorgroup].filecount - 1)
        {
          ++cursorfile;
        }
        else
        {
          if (cursorgroup < totalgroups - 1)
          {
            ++cursorgroup;
            cursorfile = 0;
          }
        }
      }

      break;

    case 'd':
    case 'D':
      deletecount = 0;

      for (x = 0; x < groups[cursorgroup].filecount; ++x)
      {
        if (groups[cursorgroup].files[x].action == 0)
          groups[cursorgroup].files[x].action = -1;

        if (groups[cursorgroup].files[x].action == -1)
          ++deletecount;
      }

      if (cursorgroup < totalgroups - 1 && deletecount < groups[cursorgroup].filecount)
      {
        ++cursorgroup;
        cursorfile = 0;
      }

      break;

    case '\n':
      if (cursorgroup < totalgroups - 1)
      {
        ++cursorgroup;
        cursorfile = 0;
      }
      break;

    case KEY_BACKSPACE:
      if (keyresult != KEY_CODE_YES)
        break;

      if (cursorgroup > 0)
      {
        --cursorgroup;
        cursorfile = 0;
      }
      break;

    case 'x':
    case 'X':
      for (x = 0; x < groups[cursorgroup].filecount; ++x)
        groups[cursorgroup].files[x].action = 0;
      break;

    case KEY_DC:
      if (keyresult != KEY_CODE_YES)
        break;

      for (g = 0; g < totalgroups; ++g)
      {
        preservecount = 0;
        deletecount = 0;
        unresolvedcount = 0;

        for (f = 0; f < groups[g].filecount; ++f)
        {
          switch (groups[g].files[f].action)
          {
            case -1:
              ++deletecount;
              break;
            case 0:
              ++unresolvedcount;
              break;
            case 1:
              ++preservecount;
              break;
          }
        }

        /* delete files marked for deletion unless no files left undeleted */
        if (deletecount < groups[g].filecount)
        {
          for (f = 0; f < groups[g].filecount; ++f)
          {
            if (groups[g].files[f].action == -1)
            {
              if (remove(groups[g].files[f].file->d_name) == 0)
                groups[g].files[f].action = -2;
            }
          }

          deletecount = 0;
        }

        /* if no files left unresolved, mark preserved files for delisting */
        if (unresolvedcount == 0)
        {
          for (f = 0; f < groups[g].filecount; ++f) 
            if (groups[g].files[f].action == 1)
              groups[g].files[f].action = -2;

          preservecount = 0;
        }
        /* if only one file left unresolved, mark it for delesting */
        else if (unresolvedcount == 1 && preservecount + deletecount == 0)
        {
          for (f = 0; f < groups[g].filecount; ++f)
            if (groups[g].files[f].action == 0)
              groups[g].files[f].action = -2;
        }

        /* delist any files marked for delisting */
        to = 0;
        for (f = 0; f < groups[g].filecount; ++f)
          if (groups[g].files[f].action != -2)
            groups[g].files[to++] = groups[g].files[f];

        groups[g].filecount = to;

        /* reposition cursor, if necessary */
        if (cursorgroup == g && cursorfile > 0 && cursorfile >= groups[g].filecount)
          cursorfile = groups[g].filecount - 1;
      }

      /* delist empty groups */
      to = 0;
      for (g = 0; g < totalgroups; ++g)
      {
        if (groups[g].filecount > 0)
        {
          groups[to] = groups[g];

          /* reposition cursor, if necessary */
          if (g == cursorgroup)
            cursorgroup = to;

          ++to;
        } 
      }

      totalgroups = to;

      /* reposition cursor, if necessary */
      if (cursorgroup >= totalgroups)
        cursorgroup = totalgroups - 1;

      /* recalculate line boundaries */
      groupfirstline = 0;

      for (g = 0; g < totalgroups; ++g)
      {
        groups[g].startline = groupfirstline;
        groups[g].endline = groupfirstline + 2;

        for (f = 0; f < groups[g].filecount; ++f)
          groups[g].endline += filerowcount(groups[g].files[f].file, COLS, FILENAME_INDENT);

        groupfirstline = groups[g].endline + 1;
      }

      break;

    case KEY_IC:
      if (keyresult != KEY_CODE_YES)
        break;

      switch (mode)
      {
        case MODE_ARROWSELECT:
          mode = MODE_NUMBERSELECT;
          break;

        case MODE_NUMBERSELECT:
          mode = MODE_ARROWSELECT;
          break;
      }
      break;

    case KEY_RESIZE:
      if (keyresult != KEY_CODE_YES)
        break;

      /* resize windows */
      wresize(filewin, LINES - 1, COLS);

      wresize(statuswin, 1, COLS);
      mvwin(statuswin, LINES - 1, 0);

      /* recalculate line boundaries */
      groupfirstline = 0;

      for (g = 0; g < totalgroups; ++g)
      {
        groups[g].startline = groupfirstline;
        groups[g].endline = groupfirstline + 2;

        for (f = 0; f < groups[g].filecount; ++f)
          groups[g].endline += filerowcount(groups[g].files[f].file, COLS, FILENAME_INDENT);

        groupfirstline = groups[g].endline + 1;
      }

      break;
    }
  } while (doprune);

  endwin();

  for (x = 0; x < totalgroups; ++x)
    free(groups[x].files);

  free(groups);
}

void registerpair(file_t **matchlist, file_t *newmatch, 
		  int (*comparef)(file_t *f1, file_t *f2))
{
  file_t *traverse;
  file_t *back;

  (*matchlist)->hasdupes = 1;

  back = 0;
  traverse = *matchlist;
  while (traverse)
  {
    if (comparef(newmatch, traverse) <= 0)
    {
      newmatch->duplicates = traverse;
      
      if (back == 0)
      {
	*matchlist = newmatch; /* update pointer to head of list */

	newmatch->hasdupes = 1;
	traverse->hasdupes = 0; /* flag is only for first file in dupe chain */
      }
      else
	back->duplicates = newmatch;

      break;
    }
    else
    {
      if (traverse->duplicates == 0)
      {
	traverse->duplicates = newmatch;
	
	if (back == 0)
	  traverse->hasdupes = 1;
	
	break;
      }
    }
    
    back = traverse;
    traverse = traverse->duplicates;
  }
}

void help_text()
{
  printf("Usage: fdupes [options] DIRECTORY...\n\n");

  printf(" -r --recurse     \tfor every directory given follow subdirectories\n");
  printf("                  \tencountered within\n");
  printf(" -R --recurse:    \tfor each directory given after this option follow\n");
  printf("                  \tsubdirectories encountered within\n");
  printf(" -s --symlinks    \tfollow symlinks\n");
  printf(" -H --hardlinks   \tnormally, when two or more files point to the same\n");
  printf("                  \tdisk area they are treated as non-duplicates; this\n"); 
  printf("                  \toption will change this behavior\n");
  printf(" -n --noempty     \texclude zero-length files from consideration\n");
  printf(" -f --omitfirst   \tomit the first file in each set of matches\n");
  printf(" -1 --sameline    \tlist each set of matches on a single line\n");
  printf(" -S --size        \tshow size of duplicate files\n");
  printf(" -m --summarize   \tsummarize dupe information\n");
  printf(" -q --quiet       \thide progress indicator\n");
  printf(" -d --delete      \tprompt user for files to preserve and delete all\n"); 
  printf("                  \tothers; important: under particular circumstances,\n");
  printf("                  \tdata may be lost when using this option together\n");
  printf("                  \twith -s or --symlinks, or when specifying a\n");
  printf("                  \tparticular directory more than once; refer to the\n");
  printf("                  \tfdupes documentation for additional information\n");
  /*printf(" -l --relink      \t(description)\n");*/
  printf(" -N --noprompt    \ttogether with --delete, preserve the first file in\n");
  printf("                  \teach set of duplicates and delete the rest without\n");
  printf("                  \tprompting the user\n");
  printf(" -v --version     \tdisplay fdupes version\n");
  printf(" -h --help        \tdisplay this help message\n\n");
#ifdef OMIT_GETOPT_LONG
  printf("Note: Long options are not supported in this fdupes build.\n\n");
#endif
}

int main(int argc, char **argv) {
  int x;
  int opt;
  FILE *file1;
  FILE *file2;
  file_t *files = NULL;
  file_t *curfile;
  file_t **match = NULL;
  filetree_t *checktree = NULL;
  int filecount = 0;
  int progress = 0;
  char **oldargv;
  int firstrecurse;
  
#ifndef OMIT_GETOPT_LONG
  static struct option long_options[] = 
  {
    { "omitfirst", 0, 0, 'f' },
    { "recurse", 0, 0, 'r' },
    { "recursive", 0, 0, 'r' },
    { "recurse:", 0, 0, 'R' },
    { "recursive:", 0, 0, 'R' },
    { "quiet", 0, 0, 'q' },
    { "sameline", 0, 0, '1' },
    { "size", 0, 0, 'S' },
    { "symlinks", 0, 0, 's' },
    { "hardlinks", 0, 0, 'H' },
    { "relink", 0, 0, 'l' },
    { "noempty", 0, 0, 'n' },
    { "delete", 0, 0, 'd' },
    { "version", 0, 0, 'v' },
    { "help", 0, 0, 'h' },
    { "noprompt", 0, 0, 'N' },
    { "summarize", 0, 0, 'm'},
    { "summary", 0, 0, 'm' },
    { 0, 0, 0, 0 }
  };
#define GETOPT getopt_long
#else
#define GETOPT getopt
#endif

  program_name = argv[0];

  setlocale(LC_CTYPE, "");

  oldargv = cloneargs(argc, argv);

  while ((opt = GETOPT(argc, argv, "frRq1Ss::HlndvhNm"
#ifndef OMIT_GETOPT_LONG
          , long_options, NULL
#endif
          )) != EOF) {
    switch (opt) {
    case 'f':
      SETFLAG(flags, F_OMITFIRST);
      break;
    case 'r':
      SETFLAG(flags, F_RECURSE);
      break;
    case 'R':
      SETFLAG(flags, F_RECURSEAFTER);
      break;
    case 'q':
      SETFLAG(flags, F_HIDEPROGRESS);
      break;
    case '1':
      SETFLAG(flags, F_DSAMELINE);
      break;
    case 'S':
      SETFLAG(flags, F_SHOWSIZE);
      break;
    case 's':
      SETFLAG(flags, F_FOLLOWLINKS);
      break;
    case 'H':
      SETFLAG(flags, F_CONSIDERHARDLINKS);
      break;
    case 'n':
      SETFLAG(flags, F_EXCLUDEEMPTY);
      break;
    case 'd':
      SETFLAG(flags, F_DELETEFILES);
      break;
    case 'v':
      printf("fdupes %s\n", VERSION);
      exit(0);
    case 'h':
      help_text();
      exit(1);
    case 'N':
      SETFLAG(flags, F_NOPROMPT);
      break;
    case 'm':
      SETFLAG(flags, F_SUMMARIZEMATCHES);
      break;

    default:
      fprintf(stderr, "Try `fdupes --help' for more information.\n");
      exit(1);
    }
  }

  if (optind >= argc) {
    errormsg("no directories specified\n");
    exit(1);
  }

  if (ISFLAG(flags, F_RECURSE) && ISFLAG(flags, F_RECURSEAFTER)) {
    errormsg("options --recurse and --recurse: are not compatible\n");
    exit(1);
  }

  if (ISFLAG(flags, F_SUMMARIZEMATCHES) && ISFLAG(flags, F_DELETEFILES)) {
    errormsg("options --summarize and --delete are not compatible\n");
    exit(1);
  }

  if (ISFLAG(flags, F_RECURSEAFTER)) {
    firstrecurse = nonoptafter("--recurse:", argc, oldargv, argv, optind);
    
    if (firstrecurse == argc)
      firstrecurse = nonoptafter("-R", argc, oldargv, argv, optind);

    if (firstrecurse == argc) {
      errormsg("-R option must be isolated from other options\n");
      exit(1);
    }

    /* F_RECURSE is not set for directories before --recurse: */
    for (x = optind; x < firstrecurse; x++)
      filecount += grokdir(argv[x], &files);

    /* Set F_RECURSE for directories after --recurse: */
    SETFLAG(flags, F_RECURSE);

    for (x = firstrecurse; x < argc; x++)
      filecount += grokdir(argv[x], &files);
  } else {
    for (x = optind; x < argc; x++)
      filecount += grokdir(argv[x], &files);
  }

  if (!files) {
    if (!ISFLAG(flags, F_HIDEPROGRESS)) fprintf(stderr, "\r%40s\r", " ");
    exit(0);
  }
  
  curfile = files;

  while (curfile) {
    if (!checktree) 
      registerfile(&checktree, curfile);
    else 
      match = checkmatch(&checktree, checktree, curfile);

    if (match != NULL) {
      file1 = fopen(curfile->d_name, "rb");
      if (!file1) {
	curfile = curfile->next;
	continue;
      }
      
      file2 = fopen((*match)->d_name, "rb");
      if (!file2) {
	fclose(file1);
	curfile = curfile->next;
	continue;
      }

      if (confirmmatch(file1, file2)) {
	registerpair(match, curfile, sort_pairs_by_mtime);
	
	/*match->hasdupes = 1;
        curfile->duplicates = match->duplicates;
        match->duplicates = curfile;*/
      }
      
      fclose(file1);
      fclose(file2);
    }

    curfile = curfile->next;

    if (!ISFLAG(flags, F_HIDEPROGRESS)) {
      fprintf(stderr, "\rProgress [%d/%d] %d%% ", progress, filecount,
       (int)((float) progress / (float) filecount * 100.0));
      progress++;
    }
  }

  if (!ISFLAG(flags, F_HIDEPROGRESS)) fprintf(stderr, "\r%40s\r", " ");

  if (ISFLAG(flags, F_DELETEFILES))
  {
    if (ISFLAG(flags, F_NOPROMPT))
    {
      deletefiles(files, 0, 0);
    }
    else
    {
	deletefiles_ncurses(files);
	/*
      stdin = freopen("/dev/tty", "r", stdin);
      deletefiles(files, 1, stdin);
	*/
    }
  }

  else 

    if (ISFLAG(flags, F_SUMMARIZEMATCHES))
      summarizematches(files);
      
    else

      printmatches(files);

  while (files) {
    curfile = files->next;
    free(files->d_name);
    free(files->crcsignature);
    free(files->crcpartial);
    free(files);
    files = curfile;
  }

  for (x = 0; x < argc; x++)
    free(oldargv[x]);

  free(oldargv);

  purgetree(checktree);

  return 0;
}
