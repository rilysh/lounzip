#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <err.h>
#include <errno.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <zip.h>

/* For compatibility with C90. */
#ifndef PATH_MAX
# define PATH_MAX          (1024)
#endif
#ifndef ECHOCTL
# define ECHOCTL           (64)
#endif
#ifndef ECHOKE
# define ECHOKE            (1)
#endif

/* Maximum amount of buffer that will be read, and then
   will be written on the disk. */
#define ZBUF_MAX           (1024)

/* Fancy constants for zip_open() and zip_get_num_entries(). */
#undef ZIP_NONE
#undef ZIP_FL_NONE
#define ZIP_NONE           (0)
#define ZIP_FL_NONE        (0)

/* Constants for take_stdin_args() function. */
#define REPLACE_YES        (1)
#define REPLACE_NO         (2)
#define REPLACE_ALL        (3)
#define REPLACE_RENAME     (4)
#define REPLACE_EXIT       (5)
#define REPLACE_INVALID    (6)
#define REPLACE_ERROR      (7)
#define REPLACE_OVERFLOW   (8)

/* Maximum size that can be for a password. */
#define MAX_PASSWD_SIZE    (82)

/* Use the compiler extension instead of language feature. */
#if defined (__GNUC__) || defined (__clang__)
# define NORETURN           __attribute__((noreturn))
#else
# define NORETURN
#endif

/* libzip error strings. */
static const char *zip_proper_error[33] = {
	"", /* 0 - No error (ignore). */
	"multidisk zip archives are not supported.", /* 1 */
	"renaming a temporary file failed.", /* 2 */
	"closing zip archive failed.", /* 3 */
	"cannot seek the archive, possibly an I/O error.", /* 4 */
	"cannot read the archive, possibly an I/O error.", /* 5 */
	"cannot write archive contents, possibly an I/O error.", /* 6 */
	"crc validation failed.", /* 7 */
	"containing zip archive was closed.", /* 8 */
	"no such file exists.", /* 9 */
	"another file already exists with that name.", /* 10 */
	"zip archive cannot be opened.", /* 11 */
	"failed to create temporary file.", /* 12 */
	"zlib initialization failed.", /* 13 */
	"memory allocation failed.", /* 14 */
	"archive entry has been altered.", /* 15 */
	"unsupported compression method.", /* 16 */
	"premature end of file.", /* 17 */
	"invalid argument was provided.", /* 18 */
	"invalid zip archive.", /* 19 */
	"an internal error has occurred.", /* 20 */
	"unexpected inconsistencies were found.", /* 21 */
	"removing a file failed.", /* 22 */
	"an unexpected error occurred.", /* 23 */
	"unsupported encryption algorithm.", /* 24 */
	"zip archive is read-only.", /* 25 */
	"", /* 26 - No password is provided (ignore). */
	"wrong password was provided.", /* 27 */
	"unsupported operation.", /* 28 */
	"resource is still in use.", /* 29 */
	"cannot tell the file.", /* 30 */
	"invalid compressed data was found.", /* 31 */
	"ongoing operation was cancelled.", /* 32 */
};

/* Get the base of a path. */
static const char *pathbase(const char *path)
{
        size_t len;

	len = strlen(path);
        do {
		if (path[len] == '/')
			/* Return the path + length and
			   extra 1 for '/'. */
			return (path + len + 1);

	} while (len--);
	return (path);
}

static int take_stdin_args(void)
{
	ssize_t ret;
	char rbuf[10];

	ret = read(STDIN_FILENO, rbuf, sizeof(rbuf));
	if (ret == (ssize_t)-1)
	        return (REPLACE_ERROR);

	if ((size_t)ret >= sizeof(rbuf))
		return (REPLACE_OVERFLOW);

	/* Only match the first character of the input. */
	switch (rbuf[0]) {
	case 'y':
	        return (REPLACE_YES);
	case 'n':
	        return (REPLACE_NO);
	case 'a':
		return (REPLACE_ALL);
	case 'r':
		return (REPLACE_RENAME);
	case 'e':
		return (REPLACE_EXIT);
	default:
	        return (REPLACE_INVALID);
	}
}

static char *take_stdin_password(void)
{
	int fd;
	char *passw;
        struct termios tios;
	size_t alen;

	/* Specify the fd as standard input. */
	fd = STDIN_FILENO;

	/* Get terminal information. */
	if (ioctl(fd, TCGETS, &tios) == -1) {
		close(fd);
		warn("ioctl()");
		return (NULL);
        }

	/* Set the terminal with new information about it.
	   Also disable CTRL+d (disabling the EOF and exit). */
	tios.c_lflag &= (ISIG | ICANON | ECHOE | ECHOK | IEXTEN |
			 ECHOCTL | ECHOKE);
	if (ioctl(fd, TCSETSF, &tios) == -1) {
		close(fd);
		warn("ioctl()");
		return (NULL);
        }

	passw = calloc(MAX_PASSWD_SIZE, sizeof(char));
	if (passw == NULL) {
		close(fd);
		warn("calloc()");
		return (NULL);
	}

	for (;;) {
		if (read(fd, passw, MAX_PASSWD_SIZE) == -1) {
			close(fd);
			free(passw);
			warn("read()");
			return (NULL);
	        }

		/* Only break the loop if we've enough input. */
		if (strlen(passw) > 0)
		        break;
	}

	alen = strlen(passw);
	if (alen >= MAX_PASSWD_SIZE)
		fputs("\nwarn: password was too big...",
		      stderr);

	/* Get the current terminal information. */
	if (ioctl(fd, TCGETS, &tios) == -1) {
		close(fd);
		free(passw);
		warn("ioctl()");
		return (NULL);
        }

	/* Reset the terminal as it was earlier (add ECHO back). */
	tios.c_lflag &= (
		ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN |
		ECHOCTL | ECHOKE);
	if (ioctl(fd, TCSETSF, &tios) == -1) {
		close(fd);
		free(passw);
		warn("ioctl()");
		return (NULL);
        }

	passw[alen - 1] = '\0';
	return (passw);
}

static char *take_stdin_rename(void)
{
        ssize_t ret;
	char *rbuf;

	rbuf = calloc(PATH_MAX, sizeof(char));
	if (rbuf == NULL)
		return (NULL);

	ret = read(STDIN_FILENO, rbuf, PATH_MAX);
	if (ret == (ssize_t)-1) {
		free(rbuf);
	        return (NULL);
	}

        rbuf[strlen(rbuf) - 1] = '\0';
	return (rbuf);
}

NORETURN
static void zip_basic_error_exit(zip_t *zip, int ec)
{
	if (zip) {
		ec = zip_error_code_zip(zip_get_error(zip));
		zip_close(zip);
		errx(EXIT_FAILURE, "error: %s",
		     zip_proper_error[ec]);
	} else {
		errx(EXIT_FAILURE, "error: %s",
		     zip_proper_error[ec]);
	}
}

static __inline__ int is_space(const char c)
{
	/* Convert it a to a lookup table, if possible. */
	switch (c) {
	case ' ':
	case '\t': case '\n': case '\v':
	case '\f': case '\r':
		return (1);

	default:
		return (0);
	}
}

/* Ignore input that only consists of spaces, but not ASCII
   or other characters. */
static int ignore_only_spaces(const char *s)
{
	for (; *s != '\0'; s++) {
		if (is_space(*s) == 0)
			return (1);
	}
	return (0);
}

static void extract_file_from_zip(zip_t *zip, zip_stat_t zs, zip_uint64_t idx,
				  zip_uint16_t encrypted, int do_rename,
				  char *passw, char *path)
{
	zip_file_t *zfp;
	int fd;
	char zbuf[ZBUF_MAX], *renm;
	size_t bytes, alen;
	zip_int64_t reads;

	if (encrypted) {
		/* Open an encrypted zip file. */
		zfp = zip_fopen_index_encrypted(zip, idx, 0, passw);
		if (zfp == NULL) {
		        free(passw);
			free(path);
			zip_basic_error_exit(zip, 0);
		}
	} else {
		/* Open a generic zip file. */
		zfp = zip_fopen_index(zip, idx, 0);
		if (zfp == NULL) {
			free(passw);
			free(path);
			zip_basic_error_exit(zip, 0);
	        }
	}

	/* It doesn't really do renaming of an existing file,
	   rather it just changes the file path that will be
	   created for that new file to live. */
	if (do_rename) {
		for (;;) {
			fputs("new name: ", stdout);
			fflush(stdout);

			/* Take standard input from the terminal/tty. */
			renm = take_stdin_rename();
			if (renm == NULL) {
				free(passw);
				free(path);
				zip_fclose(zfp);
				errx(EXIT_FAILURE, "cannot take standard input.");
			}
			
			/* Check if string length is zero. */
			alen = strlen(renm);
			if (alen == 0) {
				fputs("path name cannot be empty.\n", stderr);
				continue;
			}

			/* Check if a same file exists with that input name. */
			if (access(renm, F_OK) == 0) {
				fprintf(stdout,
					"similar file with name '%s' exists...\n",
					renm);
				continue;
			}

		        /* Ignore inputs, that consists of a single or multiple spaces
			   and no other keyword. Don't use isascii() as we don't know
			   the locale.*/
			if (ignore_only_spaces(renm) == 0) {
				fputs("invalid path name.\n", stderr);
				continue;
			}

			/* Fill the path with zeros. */
			memset(path, '\0', strlen(path));
			/* Copy the new (path) name to the path. */
			memcpy(path, renm, alen);
			break;
		}
		/* Print the renamed name string. */
		fprintf(stdout, " inflating: %s .. ", renm);
		free(renm);
	} else {
	        /* Remove the older file to not to cause data
		   corruption by appending on the older file. */
	        if (unlink(path) == -1) {
			if (errno != ENOENT) {
				warn("unlink()");
				fputs("if unlink() failed to remove the older files, "
				      "you may notice corrupted output files.\n", stderr);
			}
		}

		/* Print the original file name. */
		fprintf(stdout, " inflating: %s .. ", zs.name);
	}
	fflush(stdout);

	/* Open a file descriptor for writing. */
	fd = open(path, O_WRONLY | O_CREAT, 0644);
	if (fd == -1) {
		zip_fclose(zfp);
		zip_close(zip);
		free(passw);
		free(path);
		err(EXIT_FAILURE, "open()");
	}

	bytes = 0;
	while (bytes != zs.size) {
		/* Read the file content and store it to zbuf. */
		reads = zip_fread(zfp, zbuf, sizeof(zbuf));
		if (reads == (zip_int64_t)-1) {
			zip_fclose(zfp);
		        free(passw);
			free(path);
			close(fd);
			zip_basic_error_exit(zip, 0);
		}

		/* Write the contents that's in zbuf. */
		if (write(fd, zbuf, (size_t)reads) == -1) {
			zip_fclose(zfp);
			zip_close(zip);
			free(passw);
			free(path);
			close(fd);
			err(EXIT_FAILURE, "write()");
		}

		bytes += (size_t)reads;
	}

	close(fd);
	zip_fclose(zfp);

	/* Append a "ok" for parity. It doesn't say anything,
	   e.g. whether the file inflating was successful or
	   not. As if anything wrong  happens, it either will
	   get ignored or will be caught in error guards. */
	fputs("[ok]\n", stdout);

	/* Note: passw and path will be free'd in the parent function. */ 
}

static void unzip_zip_archive(const char *dpath, const char *zfile, int all_ok)
{
	zip_t *zip;
	zip_int64_t entries;
	zip_uint64_t i;
        zip_stat_t zs;
        char *p, *r, *passw;
	size_t zlen, dlen, mlen, tlen;
	int ret, rename_ok, in_loop, eptr;

	/* Check whether the source path (zip) file exists or not. */
	if (access(zfile, F_OK) == -1)
		errx(EXIT_FAILURE,
		     "error: file '%s' does not exists.", zfile);

	/* Check whether the destination path exists or not. */
	if (access(dpath, F_OK) == -1)
		errx(EXIT_FAILURE,
		     "error: destination path '%s' does not exists.",
		     dpath);
    
	p = calloc(1, sizeof(char));
	if (p == NULL)
		err(EXIT_FAILURE, "calloc()");

	zip = zip_open(zfile, ZIP_NONE, &eptr);
        if (zip == NULL)
	        zip_basic_error_exit(NULL, eptr);

	entries = zip_get_num_entries(zip, 0);
	mlen = 0;
        ret = rename_ok = 0;
	in_loop = 1;
	passw = NULL;

	for (i = 0; i < (zip_uint64_t)entries; i++) {
		if (zip_stat_index(zip, i, 0, &zs) == 0) {
			zlen = strlen(zs.name);
			dlen = strlen(dpath);
			tlen = zlen + dlen + 3;

			if (mlen == 0) {
				/* Allocate memory to keep the formatted path stored. */
				tlen = (zlen + dlen + 4) * 4;
				r = realloc(p, tlen);
				if (r == NULL) {
					/* Keeping open file descriptors are very costly.
					   Ensure cleanup before exiting. */
					zip_close(zip);
					free(p);
					err(EXIT_FAILURE, "realloc()");
				}

				p = r;
				mlen = tlen;
			} else {
				/* Decrease for 1 time from the 4 times (x * 4). */
				mlen -= tlen + 1;
			}

			/* Destination place where the file will be created. */
			snprintf(p, tlen, "%s/%s", dpath, zs.name);

			/* If the file is a directory, create a directory for it. */
			if (zs.name[zlen - 1] == '/') {
				if (mkdir(p, 0777) == -1) {
					if (errno != EEXIST) {
						zip_close(zip);
						free(p);
						err(EXIT_FAILURE, "mkdir()");
					}
				}
			} else {
				/* For a file, create the file with proper permission bits,
				   and write the contents to that file descriptor. Also,
				   print the file name that's being inflated. */
				if (all_ok == 0 && access(p, F_OK) == 0) {
				        do {
						fprintf(stdout,
							"replace %s? [y]es, [n]o, [a]ll, "
							"[r]ename, [e]xit: ",
							zs.name);
						fflush(stdout);
						ret = take_stdin_args();
						switch (ret) {
						case REPLACE_ERROR:
							/* An internal error occurred in libzip. */
							zip_close(zip);
							free(p);
							errx(EXIT_FAILURE,
							     "reading input stream failed.");
							break;

						case REPLACE_INVALID:
							/* Invalid input was provided. */
							fputs("invalid input, ignoring...\n",
							      stderr);
							break;

						case REPLACE_ALL:
							/* Assume other answers are always
							   will be 'yes'. */
							all_ok = 1;
							ret = REPLACE_YES;
							in_loop = 0;
							break;

						case REPLACE_RENAME:
							/* Indicate that we need to rename
							   the file, so we don't overwrite
							   the original or already extracted
							   file. */
							rename_ok = 1;
							ret = REPLACE_YES;
							in_loop = 0;
							break;

						case REPLACE_OVERFLOW:
							/* If we read more than we need to,
							   free the buffers, and exit from
							   the program. */
						        zip_close(zip);
							free(p);
							errx(EXIT_FAILURE,
							     "invalid input, exiting...\n");
						        break;

						default:
							/* This case will be always unreachable
							   as we handle invalid inputs, this is
							   to satisfy the compiler. */ 
							in_loop = 0;
							break;
						}
				        } while (in_loop);
			        }

				switch (ret) {
				case 0: /* This the default value of ret,
					   only used if the previous access()
					   call returns -1 (fails). */
				case REPLACE_YES:
					if (zs.encryption_method) {
						fprintf(stdout, "[%s] %s password: ",
							pathbase(zfile), zs.name);
						fflush(stdout);
						passw = take_stdin_password();
						fputc('\n', stdout);
					}

					/* extract the file from the archive. */
					extract_file_from_zip(
						zip, zs, i, zs.encryption_method,
						rename_ok, passw, p);
					rename_ok = 0;
					if (passw) {
						free(passw);
						passw = NULL;
					}
				        break;

				case REPLACE_EXIT:
					zip_close(zip);
					free(p);
					exit(EXIT_SUCCESS);
					/* unreachable. */

				case REPLACE_NO:
				default:
					break;
				}
		        }
		}
	}

	free(p);
	zip_close(zip);
}

static void zip_list_all_files(const char *zfile)
{
	zip_t *zip;
	zip_int64_t entries;
	zip_uint64_t i;
	zip_stat_t zs;
	char dfmt[11], tfmt[6];
	struct tm *t;
	int eptr;

	zip = zip_open(zfile, ZIP_NONE, &eptr);
	if (zip == NULL)
		zip_basic_error_exit(NULL, eptr);

	entries = zip_get_num_entries(zip, ZIP_FL_NONE);

	/* Iterate over the entries. */
	for (i = 0; i < (zip_uint64_t)entries; i++) {
	        if (zip_stat_index(zip, i, 0, &zs) == -1)
			zip_basic_error_exit(zip, 0);

		t = localtime(&zs.mtime);
		strftime(dfmt, sizeof(dfmt), "%Y-%m-%d", t);
		strftime(tfmt, sizeof(tfmt), "%H:%M", t);
		if (zs.name[strlen(zs.name) - 1] == '/')
			fprintf(stdout, "%s %s %s (directory)\n",
				dfmt, tfmt, zs.name);
		else
			fprintf(stdout, "%s %s %s (%zu bytes)\n",
				dfmt, tfmt, zs.name, zs.size);
	}

	zip_close(zip);
}

static void zip_archive_file_rename(const char *zfile, const char *old_name,
				    const char *new_name)
{
	zip_t *zip;
	zip_int64_t entries;
	zip_uint64_t i;
	zip_stat_t zs;
	int one_ok, eptr;

        /* No need to check for NULL, as we are sure that arguments
	   will be passed, but we are not sure whether those arguments
	   actually will have anything or not. */
	if (strlen(old_name) == 0)
	        errx(EXIT_FAILURE,
		     "error: old file path cannot be an empty string.");
	if (strlen(new_name) == 0)
		errx(EXIT_FAILURE,
		     "error: new file path cannot be an empty string.");

	zip = zip_open(zfile, ZIP_NONE, &eptr);
	if (zip == NULL)
		zip_basic_error_exit(NULL, eptr);

	/* Get the number of entries. */
	entries = zip_get_num_entries(zip, ZIP_FL_NONE);
	one_ok = 0;
	for (i = 0; i < (zip_uint64_t)entries; i++) {
		if (zip_stat_index(zip, i, 0, &zs) == -1)
			zip_basic_error_exit(zip, 0);

		/* If the old name matches with the filename
		   inside the zip archive, we can rename it. */
		if (strncmp(zs.name, old_name, zs.size) == 0) {
			one_ok = 1;
			if (zip_file_rename(zip, i, new_name,
					    ZIP_FL_ENC_GUESS) == -1)
				zip_basic_error_exit(zip, 0);
	        }
	}
	
	zip_close(zip);
	if (one_ok == 0)
		errx(EXIT_FAILURE,
		     "error: no archived file was found with name '%s'.",
		     old_name);
	else
	        fprintf(stdout, "changed from '%s' to '%s'.\n",
			old_name, new_name);
}

static void zip_archive_file_delete(const char *zfile, const char *file_name)
{
	zip_t *zip;
	zip_int64_t entries;
	zip_uint64_t i;
	zip_stat_t zs;
	int one_ok, eptr;

	if (strlen(file_name) == 0)
		errx(EXIT_FAILURE,
		     "file path cannot be an empty string.");

	zip = zip_open(zfile, ZIP_NONE, &eptr);
	if (zip == NULL)
		zip_basic_error_exit(NULL, eptr);

	one_ok = 0;
	entries = zip_get_num_entries(zip, ZIP_FL_NONE);

	/* Iterate over the file entries. */
	for (i = 0; i < (zip_uint64_t)entries; i++) {
		if (zip_stat_index(zip, i, 0, &zs) == -1)
			zip_basic_error_exit(zip, 0);

		/* If file name matches with the archive file name(s),
		   delete that file from the archive. */
	        if (strncmp(zs.name, file_name, zs.size) == 0) {
			one_ok = 1;
			if (zip_delete(zip, i) == -1)
			        zip_basic_error_exit(zip, 0);
        	}
	}

	zip_close(zip);
	if (one_ok == 0)
		errx(EXIT_FAILURE,
		     "error: no archived file was found with name '%s'.",
		     file_name);
        else
		fprintf(stdout, "file '%s' was deleted from the archive.\n",
			file_name);
}

NORETURN static void print_usage(int status)
{
	FILE *out;

	out = status == EXIT_SUCCESS ? stdout : stderr;
	fprintf(out,
		"lounzip - a unzipping program\n\n"
		"Commands:\n"
		" (e|x) - extract an zip archive\n"
		" (l)   - list all files in that zip archive\n"
		" (r)   - rename a file in that zip archive\n"
		" (d)   - delete a file from that zip archive\n"
		" (h)   - print this help menu\n\n"
		"Switches:\n"
		" (-y)  - assume 'yes' on archive extraction\n"
		" (-o)  - output directory for the unarchived contents\n");
	exit(status);
}

int main(int argc, char **argv)
{
	int i, j, all_ok, one_ok;
	char *path;

	if (argc < 2)
		errx(EXIT_FAILURE, "no args");

	one_ok = all_ok = i = j = 0;
	path = "."; /* Default path. */

	/* TODO: Rename l to j and comments. */
	switch (argv[1][0]) {
	case 'e':
	case 'x':
		/* Option for extraction. */
		for (i = 0; i < argc; i++) {
			if (strstr(argv[i], "-y"))
				all_ok = 1;
			if (strstr(argv[i], "-o")) {
				path = argv[i + 1];
				if (path == NULL)
					errx(EXIT_FAILURE,
					     "output path is not provided.");
			}

			if (strstr(argv[i], ".zip")) {
				one_ok = 1;
				for (j = 0; j < argc; j++) {
					if (strstr(argv[j], "-y"))
						all_ok = 1;
					if (strstr(argv[j], "-o")) {
						path = argv[j + 1];
						if (path == NULL)
							errx(EXIT_FAILURE,
							     "output path is not provided.");
					}
				}
				unzip_zip_archive(path, argv[i], all_ok);
			}
		}

		if (one_ok == 0)
			errx(EXIT_FAILURE,
			     "no zip file archive was provided.");
		goto exit_ok;

	case 'l':
		/* Option for listing files. */
		for (i = 0; i < argc; i++) {
			if (strstr(argv[i], ".zip")) {
				one_ok = 1;
				zip_list_all_files(argv[i]);
			}
		}

		if (one_ok == 0)
			errx(EXIT_FAILURE,
			     "no zip file archive was provided.");
		goto exit_ok;

	case 'r':
		/* Option for renaming a file. */
		for (i = 0; i < argc; i++) {
			if (strstr(argv[i], ".zip")) {
				one_ok = 1;
				if (argv[i + 1] == NULL)
					errx(EXIT_FAILURE,
					     "old file name is required.");
			        if (argv[i + 2] == NULL)
					errx(EXIT_FAILURE,
					     "new file name is required.");

				zip_archive_file_rename(argv[i], argv[i + 1],
							argv[i + 2]);
			}

			if (one_ok)
				goto exit_ok;
		}

		if (one_ok == 0)
			errx(EXIT_FAILURE,
			     "no zip file archive was provided.");
		goto exit_ok;

	case 'd':
		/* Option for deleting a file. */
		for (i = 0; i < argc; i++) {
			if (strstr(argv[i], ".zip")) {
				if (argv[i + 1] == NULL)
					errx(EXIT_FAILURE,
					     "file name is required.");
				for (j = 3; j < argc; j++) {
					one_ok = 1;
					zip_archive_file_delete(argv[i], argv[j]);
				}
				if (one_ok)
					goto exit_ok;
			}
		}

		if (one_ok == 0)
			errx(EXIT_FAILURE,
			     "no zip file archive was provided.");
		goto exit_ok;

	case 'h':
		/* Option for display the usage. */
	        print_usage(EXIT_SUCCESS);
		/* Unreachable. */

	default:
		/* No option is matched. */
		errx(EXIT_FAILURE,
		     "an unknown argument was provided.");
	}

exit_ok:
	exit(EXIT_SUCCESS);
}
