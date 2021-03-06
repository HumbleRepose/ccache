/*
 * ccache -- a fast C/C++ compiler cache
 *
 * Copyright (C) 2002-2007 Andrew Tridgell
 * Copyright (C) 2009-2010 Joel Rosdahl
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "ccache.h"
#include "getopt_long.h"
#include "hashtable.h"
#include "hashtable_itr.h"
#include "hashutil.h"
#include "manifest.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char VERSION_TEXT[] =
"ccache version %s\n"
"\n"
"Copyright (C) 2002-2007 Andrew Tridgell\n"
"Copyright (C) 2009-2010 Joel Rosdahl\n"
"\n"
"This program is free software; you can redistribute it and/or modify it under\n"
"the terms of the GNU General Public License as published by the Free Software\n"
"Foundation; either version 3 of the License, or (at your option) any later\n"
"version.\n";

static const char USAGE_TEXT[] =
"Usage:\n"
"    ccache [options]\n"
"    ccache compiler [compiler options]\n"
"    compiler [compiler options]          (via symbolic link)\n"
"\n"
"Options:\n"
"    -c, --cleanup         delete old files and recalculate size counters\n"
"                          (normally not needed as this is done automatically)\n"
"    -C, --clear           clear the cache completely\n"
"    -F, --max-files=N     set maximum number of files in cache to N (use 0 for\n"
"                          no limit)\n"
"    -M, --max-size=SIZE   set maximum size of cache to SIZE (use 0 for no\n"
"                          limit; available suffixes: G, M and K; default\n"
"                          suffix: G)\n"
"    -s, --show-stats      show statistics summary\n"
"    -z, --zero-stats      zero statistics counters\n"
"\n"
"    -h, --help            print this help text\n"
"    -V, --version         print version and copyright information\n"
"\n"
"See also <http://ccache.samba.org>.\n";

/* current working directory taken from $PWD, or getcwd() if $PWD is bad */
static char *current_working_dir;

/* the base cache directory */
char *cache_dir = NULL;

/* the directory for temporary files */
static char *temp_dir;

/* the debug logfile name, if set */
char *cache_logfile = NULL;

/* base directory (from CCACHE_BASEDIR) */
static char *base_dir;

/* the original argument list */
static ARGS *orig_args;

/* the source file */
static char *input_file;

/* The output file being compiled to. */
static char *output_obj;

/* The path to the dependency file (implicit or specified with -MF). */
static char *output_dep;

/*
 * Name (represented as a struct file_hash) of the file containing the cached
 * object code.
 */
static struct file_hash *cached_obj_hash;

/*
 * Full path to the file containing the cached object code
 * (cachedir/a/b/cdef[...]-size.o).
 */
static char *cached_obj;

/*
 * Full path to the file containing the standard error output
 * (cachedir/a/b/cdef[...]-size.stderr).
 */
static char *cached_stderr;

/*
 * Full path to the file containing the dependency information
 * (cachedir/a/b/cdef[...]-size.d).
 */
static char *cached_dep;

/*
 * Full path to the file containing the manifest
 * (cachedir/a/b/cdef[...]-size.manifest).
 */
static char *manifest_path;

/*
 * Time of compilation. Used to see if include files have changed after
 * compilation.
 */
static time_t time_of_compilation;

/* Bitmask of SLOPPY_*. */
unsigned sloppiness = 0;

/*
 * Files included by the preprocessor and their hashes/sizes. Key: file path.
 * Value: struct file_hash.
 */
static struct hashtable *included_files;

/* is gcc being asked to output dependencies? */
static int generating_dependencies;

/* the extension of the file (without dot) after pre-processing */
static const char *i_extension;

/* the name of the temporary pre-processor file */
static char *i_tmpfile;

/* are we compiling a .i or .ii file directly? */
static int direct_i_file;

/* the name of the cpp stderr file */
static char *cpp_stderr;

/*
 * Full path to the statistics file in the subdirectory where the cached result
 * belongs (CCACHE_DIR/X/stats).
 */
char *stats_file = NULL;

/* can we safely use the unification hashing backend? */
static int enable_unify;

/* should we use the direct mode? */
static int enable_direct = 1;

/*
 * Whether to enable compression of files stored in the cache. (Manifest files
 * are always compressed.)
 */
static int enable_compression = 0;

/* number of levels (1 <= nlevels <= 8) */
static int nlevels = 2;

/*
 * Whether we should use the optimization of passing the already existing
 * preprocessed source code to the compiler.
 */
static int compile_preprocessed_source_code;

/*
 * Supported file extensions and corresponding languages (as in parameter to
 * the -x option).
 */
static const struct {
	const char *extension;
	const char *language;
} extensions[] = {
	{".c",   "c"},
	{".C",   "c++"},
	{".cc",  "c++"},
	{".CC",  "c++"},
	{".cpp", "c++"},
	{".CPP", "c++"},
	{".cxx", "c++"},
	{".CXX", "c++"},
	{".c++", "c++"},
	{".C++", "c++"},
	{".i",   "cpp-output"},
	{".ii",  "c++-cpp-output"},
	{".mi",  "objc-cpp-output"},
	{".mii", "objc++-cpp-output"},
	{".m",   "objective-c"},
	{".M",   "objective-c++"},
	{".mm",  "objective-c++"},
	{NULL,  NULL}};

/*
 * Supported languages and corresponding file extensions.
 */
static const struct {
	const char *language;
	const char *i_extension;
} languages[] = {
	{"c",                 ".i"},
	{"cpp-output",        ".i"},
	{"c++",               ".ii"},
	{"c++-cpp-output",    ".ii"},
	{"objective-c",       ".mi"},
	{"objc-cpp-output",   ".mi"},
	{"objective-c++",     ".mii"},
	{"objc++-cpp-output", ".mii"},
	{NULL,  NULL}};

enum fromcache_call_mode {
	FROMCACHE_DIRECT_MODE,
	FROMCACHE_CPP_MODE,
	FROMCACHE_COMPILED_MODE
};

/*
 * This is a string that identifies the current "version" of the hash sum
 * computed by ccache. If, for any reason, we want to force the hash sum to be
 * different for the same input in a new ccache version, we can just change
 * this string. A typical example would be if the format of one of the files
 * stored in the cache changes in a backwards-incompatible way.
 */
static const char HASH_PREFIX[] = "3";

/*
  something went badly wrong - just execute the real compiler
*/
static void failed(void)
{
	char *e;

	/* delete intermediate pre-processor file if needed */
	if (i_tmpfile) {
		if (!direct_i_file) {
			unlink(i_tmpfile);
		}
		free(i_tmpfile);
		i_tmpfile = NULL;
	}

	/* delete the cpp stderr file if necessary */
	if (cpp_stderr) {
		unlink(cpp_stderr);
		free(cpp_stderr);
		cpp_stderr = NULL;
	}

	/* strip any local args */
	args_strip(orig_args, "--ccache-");

	if ((e=getenv("CCACHE_PREFIX"))) {
		char *p = find_executable(e, MYNAME);
		if (!p) {
			perror(e);
			exit(1);
		}
		args_add_prefix(orig_args, p);
	}

	cc_log("Failed; falling back to running the real compiler");
	cc_log_executed_command(orig_args->argv);
	execv(orig_args->argv[0], orig_args->argv);
	cc_log("execv returned (%s)!", strerror(errno));
	perror(orig_args->argv[0]);
	exit(1);
}

/*
 * Transform a name to a full path into the cache directory, creating needed
 * sublevels if needed. Caller frees.
 */
static char *get_path_in_cache(const char *name, const char *suffix)
{
	int i;
	char *path;
	char *result;

	path = x_strdup(cache_dir);
	for (i = 0; i < nlevels; ++i) {
		char *p;
		x_asprintf(&p, "%s/%c", path, name[i]);
		free(path);
		path = p;
		if (create_dir(path) != 0) {
			cc_log("Failed to create %s", path);
			failed();
		}
	}
	x_asprintf(&result, "%s/%s%s", path, name + nlevels, suffix);
	free(path);
	return result;
}

/*
 * This function hashes an include file and stores the path and hash in the
 * global included_files variable. Takes over ownership of path.
 */
static void remember_include_file(char *path, size_t path_len)
{
	struct file_hash *h;
	struct mdfour fhash;
	struct stat st;
	int fd = -1;
	char *data = (char *)-1;
	char *source;
	int result;

	if (!included_files) {
		goto ignore;
	}

	if (path_len >= 2 && (path[0] == '<' && path[path_len - 1] == '>')) {
		/* Typically <built-in> or <command-line>. */
		goto ignore;
	}

	if (strcmp(path, input_file) == 0) {
		/* Don't remember the input file. */
		goto ignore;
	}

	if (hashtable_search(included_files, path)) {
		/* Already known include file. */
		goto ignore;
	}

	/* Let's hash the include file. */
	fd = open(path, O_RDONLY|O_BINARY);
	if (fd == -1) {
		cc_log("Failed to open include file %s", path);
		goto failure;
	}
	if (fstat(fd, &st) != 0) {
		cc_log("Failed to fstat include file %s", path);
		goto failure;
	}
	if (S_ISDIR(st.st_mode)) {
		/* Ignore directory, typically $PWD. */
		goto ignore;
	}
	if (!(sloppiness & SLOPPY_INCLUDE_FILE_MTIME)
	    && st.st_mtime >= time_of_compilation) {
		cc_log("Include file %s too new", path);
		goto failure;
	}
	if (st.st_size > 0) {
		data = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
		if (data == (char *)-1) {
			cc_log("Failed to mmap %s", path);
			goto failure;
		}
		source = data;
	} else {
		source = "";
	}
	close(fd);

	hash_start(&fhash);
	result = hash_source_code_string(&fhash, source, st.st_size, path);
	if (result & HASH_SOURCE_CODE_ERROR
	    || result & HASH_SOURCE_CODE_FOUND_TIME) {
		goto failure;
	}

	h = x_malloc(sizeof(*h));
	hash_result_as_bytes(&fhash, h->hash);
	h->size = fhash.totalN;
	hashtable_insert(included_files, path, h);
	munmap(data, st.st_size);
	return;

failure:
	cc_log("Disabling direct mode");
	enable_direct = 0;
	/* Fall through. */
ignore:
	free(path);
	if (data != (char *)-1) {
		munmap(data, st.st_size);
	}
	if (fd != -1) {
		close(fd);
	}
}

/*
 * Make a relative path from CCACHE_BASEDIR to path. Takes over ownership of
 * path. Caller frees.
 */
static char *make_relative_path(char *path)
{
	char *relpath;

	if (!base_dir || strncmp(path, base_dir, strlen(base_dir)) != 0) {
		return path;
	}

	relpath = get_relative_path(current_working_dir, path);
	free(path);
	return relpath;
}

/*
 * This function reads and hashes a file. While doing this, it also does these
 * things:
 *
 * - Makes include file paths whose prefix is CCACHE_BASEDIR relative when
 *   computing the hash sum.
 * - Stores the paths and hashes of included files in the global variable
 *   included_files.
 */
static int process_preprocessed_file(struct mdfour *hash, const char *path)
{
	int fd;
	char *data;
	char *p, *q, *end;
	off_t size;
	struct stat st;

	fd = open(path, O_RDONLY);
	if (fd == -1) {
		cc_log("Failed to open %s", path);
		return 0;
	}
	if (fstat(fd, &st) != 0) {
		cc_log("Failed to fstat %s", path);
		return 0;
	}
	size = st.st_size;
	data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (data == (void *)-1) {
		cc_log("Failed to mmap %s", path);
		return 0;
	}

	if (enable_direct) {
		included_files = create_hashtable(1000, hash_from_string,
						  strings_equal);
	}

	/* Bytes between p and q are pending to be hashed. */
	end = data + size;
	p = data;
	q = data;
	while (q < end - 7) { /* There must be at least 7 characters (# 1 "x") left
	                         to potentially find an include file path. */
		/*
		 * Check if we look at a line containing the file name of an included file.
		 * At least the following formats exist (where N is a positive integer):
		 *
		 * GCC:
		 *
		 *   # N "file"
		 *   # N "file" N
		 *
		 * HP's compiler:
		 *
		 *   #line N "file"
		 *
		 * Note that there may be other lines starting with '#' left after
		 * preprocessing as well, for instance "#    pragma".
		 */
		if (q[0] == '#'
		        /* GCC: */
		    && ((q[1] == ' ' && q[2] >= '0' && q[2] <= '9')
		        /* HP: */
		        || (q[1] == 'l' && q[2] == 'i' && q[3] == 'n' && q[4] == 'e'
		            && q[5] == ' '))
		    && (q == data || q[-1] == '\n')) {
			char *path;

			while (q < end && *q != '"') {
				q++;
			}
			q++;
			if (q >= end) {
				cc_log("Failed to parse included file path");
				munmap(data, size);
				return 0;
			}
			/* q points to the beginning of an include file path */
			hash_buffer(hash, p, q - p);
			p = q;
			while (q < end && *q != '"') {
				q++;
			}
			/* p and q span the include file path */
			path = x_strndup(p, q - p);
			path = make_relative_path(path);
			hash_string(hash, path);
			if (enable_direct) {
				remember_include_file(path, q - p);
			} else {
				free(path);
			}
			p = q;
		} else {
			q++;
		}
	}

	hash_buffer(hash, p, (end - p));
	munmap(data, size);
	return 1;
}

/*
 * Try to guess the language of a file based on its extension. Returns NULL if
 * the extension is unknown.
 */
static const char *
language_for_file(const char *fname)
{
	int i;
	const char *p;

	p = get_extension(fname);
	for (i = 0; extensions[i].extension; i++) {
		if (strcmp(p, extensions[i].extension) == 0) {
			return extensions[i].language;
		}
	}
	return NULL;
}

/*
 * Return the default file extension (including dot) for a language, or NULL if
 * unknown.
 */
static const char *
i_extension_for_language(const char *language)
{
	int i;

	if (!language) {
		return NULL;
	}
	for (i = 0; languages[i].language; i++) {
		if (strcmp(language, languages[i].language) == 0) {
			return languages[i].i_extension;
		}
	}
	return NULL;
}

static int
language_is_supported(const char *language)
{
	return i_extension_for_language(language) != NULL;
}

static int
language_is_preprocessed(const char *language)
{
	const char *i_ext = i_extension_for_language(language);
	return strcmp(language, language_for_file(i_ext)) == 0;
}

/* run the real compiler and put the result in cache */
static void to_cache(ARGS *args)
{
	char *tmp_stdout, *tmp_stderr, *tmp_obj;
	struct stat st;
	int status;
	size_t added_bytes = 0;
	unsigned added_files = 0;

	x_asprintf(&tmp_stdout, "%s.tmp.stdout.%s", cached_obj, tmp_string());
	x_asprintf(&tmp_stderr, "%s.tmp.stderr.%s", cached_obj, tmp_string());
	x_asprintf(&tmp_obj, "%s.tmp.%s", cached_obj, tmp_string());

	args_add(args, "-o");
	args_add(args, tmp_obj);

	/* Turn off DEPENDENCIES_OUTPUT when running cc1, because
	 * otherwise it will emit a line like
	 *
	 *  tmp.stdout.vexed.732.o: /home/mbp/.ccache/tmp.stdout.vexed.732.i
	 *
	 * unsetenv() is on BSD and Linux but not portable. */
	putenv("DEPENDENCIES_OUTPUT");

	if (compile_preprocessed_source_code) {
		args_add(args, i_tmpfile);
	} else {
		args_add(args, input_file);
	}

	cc_log("Running real compiler");
	status = execute(args->argv, tmp_stdout, tmp_stderr);
	args_pop(args, 3);

	if (stat(tmp_stdout, &st) != 0 || st.st_size != 0) {
		cc_log("Compiler produced stdout");
		stats_update(STATS_STDOUT);
		unlink(tmp_stdout);
		unlink(tmp_stderr);
		unlink(tmp_obj);
		failed();
	}
	unlink(tmp_stdout);

	/*
	 * Merge stderr from the preprocessor (if any) and stderr from the real
	 * compiler into tmp_stderr.
	 */
	if (cpp_stderr) {
		int fd_cpp_stderr;
		int fd_real_stderr;
		int fd_result;

		fd_cpp_stderr = open(cpp_stderr, O_RDONLY | O_BINARY);
		if (fd_cpp_stderr == -1) {
			cc_log("Failed opening %s", cpp_stderr);
			failed();
		}
		fd_real_stderr = open(tmp_stderr, O_RDONLY | O_BINARY);
		if (fd_real_stderr == -1) {
			cc_log("Failed opening %s", tmp_stderr);
			failed();
		}
		unlink(tmp_stderr);
		fd_result = open(tmp_stderr,
				 O_WRONLY | O_CREAT | O_TRUNC | O_BINARY,
				 0666);
		if (fd_result == -1) {
			cc_log("Failed opening %s", tmp_stderr);
			failed();
		}
		copy_fd(fd_cpp_stderr, fd_result);
		copy_fd(fd_real_stderr, fd_result);
		close(fd_cpp_stderr);
		close(fd_real_stderr);
		close(fd_result);
		unlink(cpp_stderr);
		free(cpp_stderr);
		cpp_stderr = NULL;
	}

	if (status != 0) {
		int fd;
		cc_log("Compiler gave exit status %d", status);
		stats_update(STATS_STATUS);

		fd = open(tmp_stderr, O_RDONLY | O_BINARY);
		if (fd != -1) {
			if (strcmp(output_obj, "/dev/null") == 0
			    || (access(tmp_obj, R_OK) == 0
			        && move_file(tmp_obj, output_obj, 0) == 0)
			    || errno == ENOENT) {
				/* we can use a quick method of
				   getting the failed output */
				copy_fd(fd, 2);
				close(fd);
				unlink(tmp_stderr);
				if (i_tmpfile && !direct_i_file) {
					unlink(i_tmpfile);
				}
				exit(status);
			}
		}

		unlink(tmp_stderr);
		unlink(tmp_obj);
		failed();
	}

	if (stat(tmp_obj, &st) != 0) {
		cc_log("Compiler didn't produce an object file");
		stats_update(STATS_NOOUTPUT);
		failed();
	}
	if (st.st_size == 0) {
		cc_log("Compiler produced an empty object file");
		stats_update(STATS_EMPTYOUTPUT);
		failed();
	}

	if (stat(tmp_stderr, &st) != 0) {
		cc_log("Failed to stat %s", tmp_stderr);
		stats_update(STATS_ERROR);
		failed();
	}
	if (st.st_size > 0) {
		if (move_uncompressed_file(tmp_stderr, cached_stderr,
		                           enable_compression) != 0) {
			cc_log("Failed to move %s to %s",
			       tmp_stderr, cached_stderr);
			stats_update(STATS_ERROR);
			failed();
		}
		cc_log("Stored in cache: %s", cached_stderr);
		if (enable_compression) {
			stat(cached_stderr, &st);
		}
		added_bytes += file_size(&st);
		added_files += 1;
	} else {
		unlink(tmp_stderr);
	}
	if (move_uncompressed_file(tmp_obj, cached_obj, enable_compression) != 0) {
		cc_log("Failed to move %s to %s", tmp_obj, cached_obj);
		stats_update(STATS_ERROR);
		failed();
	} else {
		cc_log("Stored in cache: %s", cached_obj);
		stat(cached_obj, &st);
		added_bytes += file_size(&st);
		added_files += 1;
	}

	/*
	 * Do an extra stat on the potentially compressed object file for the
	 * size statistics.
	 */
	if (stat(cached_obj, &st) != 0) {
		cc_log("Failed to stat %s", strerror(errno));
		stats_update(STATS_ERROR);
		failed();
	}

	stats_update_size(STATS_TOCACHE, added_bytes / 1024, added_files);

	free(tmp_obj);
	free(tmp_stderr);
	free(tmp_stdout);
}

/*
 * Find the object file name by running the compiler in preprocessor mode.
 * Returns the hash as a heap-allocated hex string.
 */
static struct file_hash *
get_object_name_from_cpp(ARGS *args, struct mdfour *hash)
{
	char *input_base;
	char *tmp;
	char *path_stdout, *path_stderr;
	int status;
	struct file_hash *result;

	/* ~/hello.c -> tmp.hello.123.i
	   limit the basename to 10
	   characters in order to cope with filesystem with small
	   maximum filename length limits */
	input_base = basename(input_file);
	tmp = strchr(input_base, '.');
	if (tmp != NULL) {
		*tmp = 0;
	}
	if (strlen(input_base) > 10) {
		input_base[10] = 0;
	}

	/* now the run */
	x_asprintf(&path_stdout, "%s/%s.tmp.%s.%s", temp_dir,
		   input_base, tmp_string(), i_extension);
	x_asprintf(&path_stderr, "%s/tmp.cpp_stderr.%s", temp_dir,
		   tmp_string());

	time_of_compilation = time(NULL);

	if (!direct_i_file) {
		/* run cpp on the input file to obtain the .i */
		args_add(args, "-E");
		args_add(args, input_file);
		status = execute(args->argv, path_stdout, path_stderr);
		args_pop(args, 2);
	} else {
		/* we are compiling a .i or .ii file - that means we
		   can skip the cpp stage and directly form the
		   correct i_tmpfile */
		path_stdout = input_file;
		if (create_empty_file(path_stderr) != 0) {
			stats_update(STATS_ERROR);
			cc_log("Failed to create %s", path_stderr);
			failed();
		}
		status = 0;
	}

	if (status != 0) {
		if (!direct_i_file) {
			unlink(path_stdout);
		}
		unlink(path_stderr);
		cc_log("Preprocessor gave exit status %d", status);
		stats_update(STATS_PREPROCESSOR);
		failed();
	}

	if (enable_unify) {
		/*
		 * When we are doing the unifying tricks we need to include the
		 * input file name in the hash to get the warnings right.
		 */
		hash_delimiter(hash, "unifyfilename");
		hash_string(hash, input_file);

		hash_delimiter(hash, "unifycpp");
		if (unify_hash(hash, path_stdout) != 0) {
			stats_update(STATS_ERROR);
			unlink(path_stderr);
			cc_log("Failed to unify %s", path_stdout);
			failed();
		}
	} else {
		hash_delimiter(hash, "cpp");
		if (!process_preprocessed_file(hash, path_stdout)) {
			stats_update(STATS_ERROR);
			unlink(path_stderr);
			failed();
		}
	}

	hash_delimiter(hash, "cppstderr");
	if (!hash_file(hash, path_stderr)) {
		fatal("Failed to open %s", path_stderr);
	}

	i_tmpfile = path_stdout;

	if (compile_preprocessed_source_code) {
		/*
		 * If we are using the CPP trick, we need to remember this
		 * stderr data and output it just before the main stderr from
		 * the compiler pass.
		 */
		cpp_stderr = path_stderr;
	} else {
		unlink(path_stderr);
		free(path_stderr);
	}

	result = x_malloc(sizeof(*result));
	hash_result_as_bytes(hash, result->hash);
	result->size = hash->totalN;
	return result;
}

static void update_cached_result_globals(struct file_hash *hash)
{
	char *object_name;

	object_name = format_hash_as_string(hash->hash, hash->size);
	cached_obj_hash = hash;
	cached_obj = get_path_in_cache(object_name, ".o");
	cached_stderr = get_path_in_cache(object_name, ".stderr");
	cached_dep = get_path_in_cache(object_name, ".d");
	x_asprintf(&stats_file, "%s/%c/stats", cache_dir, object_name[0]);
	free(object_name);
}

/*
 * Update a hash sum with information common for the direct and preprocessor
 * modes.
 */
static void calculate_common_hash(ARGS *args, struct mdfour *hash)
{
	struct stat st;
	const char *compilercheck;
	char *p;

	hash_string(hash, HASH_PREFIX);

	/*
	 * We have to hash the extension, as a .i file isn't treated the same
	 * by the compiler as a .ii file.
	 */
	hash_delimiter(hash, "ext");
	hash_string(hash, i_extension);

	if (stat(args->argv[0], &st) != 0) {
		cc_log("Couldn't stat the compiler (%s)", args->argv[0]);
		stats_update(STATS_COMPILER);
		failed();
	}

	/*
	 * Hash information about the compiler.
	 */
	compilercheck = getenv("CCACHE_COMPILERCHECK");
	if (!compilercheck) {
		compilercheck = "mtime";
	}
	if (strcmp(compilercheck, "none") == 0) {
		/* Do nothing. */
	} else if (strcmp(compilercheck, "content") == 0) {
		hash_delimiter(hash, "cc_content");
		hash_file(hash, args->argv[0]);
	} else { /* mtime */
		hash_delimiter(hash, "cc_mtime");
		hash_int(hash, st.st_size);
		hash_int(hash, st.st_mtime);
	}

	/*
	 * Also hash the compiler name as some compilers use hard links and
	 * behave differently depending on the real name.
	 */
	hash_delimiter(hash, "cc_name");
	hash_string(hash, basename(args->argv[0]));

	/* Possibly hash the current working directory. */
	if (getenv("CCACHE_HASHDIR")) {
		char *cwd = gnu_getcwd();
		if (cwd) {
			hash_delimiter(hash, "cwd");
			hash_string(hash, cwd);
			free(cwd);
		}
	}

	p = getenv("CCACHE_EXTRAFILES");
	if (p) {
		char *path, *q;
		p = x_strdup(p);
		q = p;
		while ((path = strtok(q, ":"))) {
			cc_log("Hashing extra file %s", path);
			hash_delimiter(hash, "extrafile");
			if (!hash_file(hash, path)) {
				stats_update(STATS_BADEXTRAFILE);
				failed();
			}
			q = NULL;
		}
		free(p);
	}
}

/*
 * Update a hash sum with information specific to the direct and preprocessor
 * modes and calculate the object hash. Returns the object hash on success,
 * otherwise NULL. Caller frees.
 */
static struct file_hash *calculate_object_hash(
	ARGS *args, struct mdfour *hash, int direct_mode)
{
	int i;
	char *manifest_name;
	struct stat st;
	int result;
	struct file_hash *object_hash = NULL;

	/* first the arguments */
	for (i=1;i<args->argc;i++) {
		/* -L doesn't affect compilation. */
		if (i < args->argc-1 && strcmp(args->argv[i], "-L") == 0) {
			i++;
			continue;
		}
		if (strncmp(args->argv[i], "-L", 2) == 0) {
			continue;
		}

		/* When using the preprocessor, some arguments don't contribute
		   to the hash. The theory is that these arguments will change
		   the output of -E if they are going to have any effect at
		   all. */
		if (!direct_mode) {
			if (i < args->argc-1) {
				if (strcmp(args->argv[i], "-D") == 0 ||
				    strcmp(args->argv[i], "-I") == 0 ||
				    strcmp(args->argv[i], "-U") == 0 ||
				    strcmp(args->argv[i], "-idirafter") == 0 ||
				    strcmp(args->argv[i], "-imacros") == 0 ||
				    strcmp(args->argv[i], "-imultilib") == 0 ||
				    strcmp(args->argv[i], "-include") == 0 ||
				    strcmp(args->argv[i], "-iprefix") == 0 ||
				    strcmp(args->argv[i], "-iquote") == 0 ||
				    strcmp(args->argv[i], "-isysroot") == 0 ||
				    strcmp(args->argv[i], "-isystem") == 0 ||
				    strcmp(args->argv[i], "-iwithprefix") == 0 ||
				    strcmp(args->argv[i], "-iwithprefixbefore") == 0 ||
				    strcmp(args->argv[i], "-nostdinc") == 0 ||
				    strcmp(args->argv[i], "-nostdinc++") == 0) {
					/* Skip from hash. */
					i++;
					continue;
				}
			}
			if (strncmp(args->argv[i], "-D", 2) == 0 ||
			    strncmp(args->argv[i], "-I", 2) == 0 ||
			    strncmp(args->argv[i], "-U", 2) == 0) {
				/* Skip from hash. */
				continue;
			}
		}

		if (strncmp(args->argv[i], "--specs=", 8) == 0 &&
		    stat(args->argv[i] + 8, &st) == 0) {
			/* If given a explicit specs file, then hash that file,
			   but don't include the path to it in the hash. */
			hash_delimiter(hash, "specs");
			if (!hash_file(hash, args->argv[i] + 8)) {
				failed();
			}
			continue;
		}

		/* All other arguments are included in the hash. */
		hash_delimiter(hash, "arg");
		hash_string(hash, args->argv[i]);
	}

	if (direct_mode) {
		if (!(sloppiness & SLOPPY_FILE_MACRO)) {
			/*
			 * The source code file or an include file may contain
			 * __FILE__, so make sure that the hash is unique for
			 * the file name.
			 */
			hash_delimiter(hash, "inputfile");
			hash_string(hash, input_file);
		}

		hash_delimiter(hash, "sourcecode");
		result = hash_source_code_file(hash, input_file);
		if (result & HASH_SOURCE_CODE_ERROR) {
			failed();
		}
		if (result & HASH_SOURCE_CODE_FOUND_TIME) {
			cc_log("Disabling direct mode");
			enable_direct = 0;
			return NULL;
		}
		manifest_name = hash_result(hash);
		manifest_path = get_path_in_cache(manifest_name, ".manifest");
		free(manifest_name);
		cc_log("Looking for object file hash in %s",
		       manifest_path);
		object_hash = manifest_get(manifest_path);
		if (object_hash) {
			cc_log("Got object file hash from manifest");
		} else {
			cc_log("Did not find object file hash in manifest");
		}
	} else {
		object_hash = get_object_name_from_cpp(args, hash);
		cc_log("Got object file hash from preprocessor");
		if (generating_dependencies) {
			cc_log("Preprocessor created %s", output_dep);
		}
	}

	return object_hash;
}

/*
   try to return the compile result from cache. If we can return from
   cache then this function exits with the correct status code,
   otherwise it returns */
static void from_cache(enum fromcache_call_mode mode, int put_object_in_manifest)
{
	int fd_stderr;
	int ret;
	struct stat st;
	int produce_dep_file;

	/* the user might be disabling cache hits */
	if (mode != FROMCACHE_COMPILED_MODE && getenv("CCACHE_RECACHE")) {
		return;
	}

	/* Check if the object file is there. */
	if (stat(cached_obj, &st) != 0) {
		cc_log("Object file %s not in cache", cached_obj);
		return;
	}

	/*
	 * (If mode != FROMCACHE_DIRECT_MODE, the dependency file is created by
	 * gcc.)
	 */
	produce_dep_file = \
		generating_dependencies && mode == FROMCACHE_DIRECT_MODE;

	/* If the dependency file should be in the cache, check that it is. */
	if (produce_dep_file && stat(cached_dep, &st) != 0) {
		cc_log("Dependency file %s missing in cache", cached_dep);
		return;
	}

	if (strcmp(output_obj, "/dev/null") == 0) {
		ret = 0;
	} else {
		unlink(output_obj);
		/* only make a hardlink if the cache file is uncompressed */
		if (getenv("CCACHE_HARDLINK") &&
		    test_if_compressed(cached_obj) == 0) {
			ret = link(cached_obj, output_obj);
		} else {
			ret = copy_file(cached_obj, output_obj, 0);
		}
	}

	if (ret == -1) {
		if (errno == ENOENT) {
			/* Someone removed the file just before we began copying? */
			cc_log("Object file %s just disappeared from cache",
			       cached_obj);
			stats_update(STATS_MISSING);
		} else {
			cc_log("Failed to copy/link %s to %s (%s)",
			       cached_obj, output_obj, strerror(errno));
			stats_update(STATS_ERROR);
			failed();
		}
		unlink(output_obj);
		unlink(cached_stderr);
		unlink(cached_obj);
		unlink(cached_dep);
		return;
	} else {
		cc_log("Created %s from %s", output_obj, cached_obj);
	}

	if (produce_dep_file) {
		unlink(output_dep);
		/* only make a hardlink if the cache file is uncompressed */
		if (getenv("CCACHE_HARDLINK") &&
		    test_if_compressed(cached_dep) == 0) {
			ret = link(cached_dep, output_dep);
		} else {
			ret = copy_file(cached_dep, output_dep, 0);
		}
		if (ret == -1) {
			if (errno == ENOENT) {
				/*
				 * Someone removed the file just before we
				 * began copying?
				 */
				cc_log("Dependency file %s just disappeared"
				       " from cache", output_obj);
				stats_update(STATS_MISSING);
			} else {
				cc_log("Failed to copy/link %s to %s (%s)",
				       cached_dep, output_dep,
				       strerror(errno));
				stats_update(STATS_ERROR);
				failed();
			}
			unlink(output_obj);
			unlink(output_dep);
			unlink(cached_stderr);
			unlink(cached_obj);
			unlink(cached_dep);
			return;
		} else {
			cc_log("Created %s from %s", output_dep, cached_dep);
		}
	}

	/* Update modification timestamps to save files from LRU cleanup.
	   Also gives files a sensible mtime when hard-linking. */
	update_mtime(cached_obj);
	update_mtime(cached_stderr);
	if (produce_dep_file) {
		update_mtime(cached_dep);
	}

	if (generating_dependencies && mode != FROMCACHE_DIRECT_MODE) {
		/* Store the dependency file in the cache. */
		ret = copy_file(output_dep, cached_dep, enable_compression);
		if (ret == -1) {
			cc_log("Failed to copy %s to %s", output_dep, cached_dep);
			/* Continue despite the error. */
		} else {
			cc_log("Stored in cache: %s", cached_dep);
			stat(cached_dep, &st);
			stats_update_size(STATS_NONE, file_size(&st) / 1024, 1);
		}
	}

	/* get rid of the intermediate preprocessor file */
	if (i_tmpfile) {
		if (!direct_i_file) {
			unlink(i_tmpfile);
		}
		free(i_tmpfile);
		i_tmpfile = NULL;
	}

	/* Delete the cpp stderr file if necessary. */
	if (cpp_stderr) {
		unlink(cpp_stderr);
		free(cpp_stderr);
		cpp_stderr = NULL;
	}

	/* Send the stderr, if any. */
	fd_stderr = open(cached_stderr, O_RDONLY | O_BINARY);
	if (fd_stderr != -1) {
		copy_fd(fd_stderr, 2);
		close(fd_stderr);
	}

	/* Create or update the manifest file. */
	if (enable_direct
	    && put_object_in_manifest
	    && included_files
	    && !getenv("CCACHE_READONLY")) {
		struct stat st;
		size_t old_size = 0; /* in bytes */
		if (stat(manifest_path, &st) == 0) {
			old_size = file_size(&st);
		}
		if (manifest_put(manifest_path, cached_obj_hash, included_files)) {
			cc_log("Added object file hash to %s", manifest_path);
			update_mtime(manifest_path);
			stat(manifest_path, &st);
			stats_update_size(
				STATS_NONE,
				(file_size(&st) - old_size) / 1024,
				old_size == 0 ? 1 : 0);
		} else {
			cc_log("Failed to add object file hash to %s", manifest_path);
		}
	}

	/* log the cache hit */
	switch (mode) {
	case FROMCACHE_DIRECT_MODE:
		cc_log("Succeded getting cached result");
		stats_update(STATS_CACHEHIT_DIR);
		break;

	case FROMCACHE_CPP_MODE:
		cc_log("Succeded getting cached result");
		stats_update(STATS_CACHEHIT_CPP);
		break;

	case FROMCACHE_COMPILED_MODE:
		/* Stats already updated in to_cache(). */
		break;
	}

	/* and exit with the right status code */
	exit(0);
}

/* find the real compiler. We just search the PATH to find a executable of the
   same name that isn't a link to ourselves */
static void find_compiler(int argc, char **argv)
{
	char *base;
	char *path;
	char *compiler;

	orig_args = args_init(argc, argv);

	base = basename(argv[0]);

	/* we might be being invoked like "ccache gcc -c foo.c" */
	if (strcmp(base, MYNAME) == 0) {
		args_remove_first(orig_args);
		free(base);
		if (strchr(argv[1],'/')) {
			/* a full path was given */
			return;
		}
		base = basename(argv[1]);
	}

	/* support user override of the compiler */
	if ((path=getenv("CCACHE_CC"))) {
		base = x_strdup(path);
	}

	compiler = find_executable(base, MYNAME);

	/* can't find the compiler! */
	if (!compiler) {
		stats_update(STATS_COMPILER);
		fatal("Could not find compiler \"%s\" in PATH", base);
	}
	if (strcmp(compiler, argv[0]) == 0) {
		fatal("Recursive invocation (the name of the ccache binary"
		      " must be \"%s\")", MYNAME);
	}
	orig_args->argv[0] = compiler;
}

/*
   process the compiler options to form the correct set of options
   for obtaining the preprocessor output
*/
static void process_args(int argc, char **argv, ARGS **preprocessor_args,
			 ARGS **compiler_args)
{
	int i;
	int found_c_opt = 0;
	int found_S_opt = 0;
	int found_arch_opt = 0;
	const char *explicit_language = NULL; /* As specified with -x. */
	const char *file_language;            /* As deduced from file extension. */
	const char *actual_language;          /* Language to actually use. */
	const char *input_charset = NULL;
	struct stat st;
	/* is the dependency makefile name overridden with -MF? */
	int dependency_filename_specified = 0;
	/* is the dependency makefile target name specified with -MT or -MQ? */
	int dependency_target_specified = 0;
	ARGS *stripped_args;

	stripped_args = args_init(0, NULL);

	args_add(stripped_args, argv[0]);

	for (i=1; i<argc; i++) {
		/* some options will never work ... */
		if (strcmp(argv[i], "-E") == 0) {
			cc_log("Compiler option -E is unsupported");
			stats_update(STATS_UNSUPPORTED);
			failed();
		}

		/* these are too hard */
		if (strncmp(argv[i], "@", 1) == 0 ||
		    strcmp(argv[i], "--coverage") == 0 ||
		    strcmp(argv[i], "-M") == 0 ||
		    strcmp(argv[i], "-MM") == 0 ||
		    strcmp(argv[i], "-fbranch-probabilities") == 0 ||
		    strcmp(argv[i], "-fprofile-arcs") == 0 ||
		    strcmp(argv[i], "-fprofile-generate") == 0 ||
		    strcmp(argv[i], "-fprofile-use") == 0 ||
		    strcmp(argv[i], "-ftest-coverage") == 0 ||
		    strcmp(argv[i], "-save-temps") == 0) {
			cc_log("Compiler option %s is unsupported", argv[i]);
			stats_update(STATS_UNSUPPORTED);
			failed();
			continue;
		}

		/* These are too hard in direct mode. */
		if (enable_direct) {
			if (strcmp(argv[i], "-Xpreprocessor") == 0) {
				cc_log("Unsupported compiler option for direct"
				       " mode: %s", argv[i]);
				enable_direct = 0;
			}
		}

		/* Multiple -arch options are too hard. */
		if (strcmp(argv[i], "-arch") == 0) {
			if (found_arch_opt) {
				cc_log("More than one -arch compiler option"
				       " is unsupported");
				stats_update(STATS_UNSUPPORTED);
				failed();
			} else {
				found_arch_opt = 1;
			}
		}

		/* we must have -c */
		if (strcmp(argv[i], "-c") == 0) {
			args_add(stripped_args, argv[i]);
			found_c_opt = 1;
			continue;
		}

		/* -S changes the default extension */
		if (strcmp(argv[i], "-S") == 0) {
			args_add(stripped_args, argv[i]);
			found_S_opt = 1;
			continue;
		}

		/*
		 * Special handling for -x: remember the last specified language before the
		 * input file and strip all -x options from the arguments.
		 */
		if (strcmp(argv[i], "-x") == 0) {
			if (i == argc-1) {
				cc_log("Missing argument to %s", argv[i]);
				stats_update(STATS_ARGS);
				failed();
			}
			if (!input_file) {
				explicit_language = argv[i+1];
			}
			i++;
			continue;
		}
		if (strncmp(argv[i], "-x", 2) == 0) {
			if (!input_file) {
				explicit_language = &argv[i][2];
			}
			continue;
		}

		/* we need to work out where the output was meant to go */
		if (strcmp(argv[i], "-o") == 0) {
			if (i == argc-1) {
				cc_log("Missing argument to %s", argv[i]);
				stats_update(STATS_ARGS);
				failed();
			}
			output_obj = argv[i+1];
			i++;
			continue;
		}

		/* alternate form of -o, with no space */
		if (strncmp(argv[i], "-o", 2) == 0) {
			output_obj = &argv[i][2];
			continue;
		}

		/* debugging is handled specially, so that we know if we
		   can strip line number info
		*/
		if (strncmp(argv[i], "-g", 2) == 0) {
			args_add(stripped_args, argv[i]);
			if (enable_unify && strcmp(argv[i], "-g0") != 0) {
				cc_log("%s used; disabling unify mode",
				       argv[i]);
				enable_unify = 0;
			}
			if (strcmp(argv[i], "-g3") == 0) {
				/*
				 * Fix for bug 7190 ("commandline macros (-D)
				 * have non-zero lineno when using -g3").
				 */
				cc_log("%s used; not compiling preprocessed"
				       " code", argv[i]);
				compile_preprocessed_source_code = 0;
			}
			continue;
		}

		/* The user knows best: just swallow the next arg */
		if (strcmp(argv[i], "--ccache-skip") == 0) {
			i++;
			if (i == argc) {
				cc_log("--ccache-skip lacks an argument");
				failed();
			}
			args_add(stripped_args, argv[i]);
			continue;
		}

		/* These options require special handling, because they
		   behave differently with gcc -E, when the output
		   file is not specified. */
		if (strcmp(argv[i], "-MD") == 0
		    || strcmp(argv[i], "-MMD") == 0) {
			generating_dependencies = 1;
		}
		if (i < argc - 1) {
			if (strcmp(argv[i], "-MF") == 0) {
				dependency_filename_specified = 1;
				output_dep = make_relative_path(
					x_strdup(argv[i + 1]));
			} else if (strcmp(argv[i], "-MQ") == 0
				   || strcmp(argv[i], "-MT") == 0) {
				dependency_target_specified = 1;
			}
		}

		if (strncmp(argv[i], "-Wp,", 4) == 0) {
			if (strncmp(argv[i], "-Wp,-MD,", 8) == 0
			    && !strchr(argv[i] + 8, ',')) {
				generating_dependencies = 1;
				dependency_filename_specified = 1;
				output_dep = make_relative_path(
					x_strdup(argv[i] + 8));
			} else if (strncmp(argv[i], "-Wp,-MMD,", 9) == 0
			    && !strchr(argv[i] + 9, ',')) {
				generating_dependencies = 1;
				dependency_filename_specified = 1;
				output_dep = make_relative_path(
					x_strdup(argv[i] + 9));
			} else if (enable_direct) {
				/*
				 * -Wp, can be used to pass too hard options to
				 * the preprocessor. Hence, disable direct
				 * mode.
				 */
				cc_log("Unsupported compiler option for direct mode: %s",
				       argv[i]);
				enable_direct = 0;
			}
		}

		/* Input charset needs to be handled specially. */
		if (strncmp(argv[i], "-finput-charset=", 16) == 0) {
			input_charset = argv[i];
			continue;
		}

		/*
		 * Options taking an argument that that we may want to rewrite
		 * to relative paths to get better hit rate. A secondary effect
		 * is that paths in the standard error output produced by the
		 * compiler will be normalized.
		 */
		{
			const char *opts[] = {
				"-I", "-idirafter", "-imacros", "-include",
				"-iprefix", "-isystem", NULL
			};
			int j;
			char *relpath;
			for (j = 0; opts[j]; j++) {
				if (strcmp(argv[i], opts[j]) == 0) {
					if (i == argc-1) {
						cc_log("Missing argument to %s",
						       argv[i]);
						stats_update(STATS_ARGS);
						failed();
					}

					args_add(stripped_args, argv[i]);
					relpath = make_relative_path(x_strdup(argv[i+1]));
					args_add(stripped_args, relpath);
					free(relpath);
					i++;
					break;
				}
			}
			if (opts[j]) {
				continue;
			}
		}

		/* Same as above but options with concatenated argument. */
		{
			const char *opts[] = {"-I", NULL};
			int j;
			char *relpath;
			char *option;
			for (j = 0; opts[j]; j++) {
				if (strncmp(argv[i], opts[j], strlen(opts[j])) == 0) {
					relpath = make_relative_path(
						x_strdup(argv[i] + strlen(opts[j])));
					x_asprintf(&option, "%s%s", opts[j], relpath);
					args_add(stripped_args, option);
					free(relpath);
					free(option);
					break;
				}
			}
			if (opts[j]) {
				continue;
			}
		}

		/* options that take an argument */
		{
			const char *opts[] = {
				"--param",
				"-A",
				"-D",
				"-G",
				"-L",
				"-MF",
				"-MQ",
				"-MT",
				"-U",
				"-V",
				"-Xassembler",
				"-Xlinker",
				"-aux-info",
				"-b",
				"-iwithprefix",
				"-iwithprefixbefore",
				"-u",
				NULL
			};
			int j;
			for (j = 0; opts[j]; j++) {
				if (strcmp(argv[i], opts[j]) == 0) {
					if (i == argc-1) {
						cc_log("Missing argument to %s",
						       argv[i]);
						stats_update(STATS_ARGS);
						failed();
					}

					args_add(stripped_args, argv[i]);
					args_add(stripped_args, argv[i+1]);
					i++;
					break;
				}
			}
			if (opts[j]) continue;
		}

		/* other options */
		if (argv[i][0] == '-') {
			args_add(stripped_args, argv[i]);
			continue;
		}

		/* if an argument isn't a plain file then assume its
		   an option, not an input file. This allows us to
		   cope better with unusual compiler options */
		if (stat(argv[i], &st) != 0 || !S_ISREG(st.st_mode)) {
			cc_log("%s is not a regular file, not considering as input file",
			       argv[i]);
			args_add(stripped_args, argv[i]);
			continue;
		}

		/* If we're being called as distcc and the first argument is
		   not a source file, it's treated as the compiler by distcc,
		   so we treat it the same */
		if (i == 1 && strcmp(basename(argv[0]), "distcc") == 0 && !language_for_file(argv[i])) {
			args_add(stripped_args, argv[i]);
			continue;
		}

		if (input_file) {
			if (language_for_file(argv[i])) {
				cc_log("Multiple input files: %s and %s",
				       input_file, argv[i]);
				stats_update(STATS_MULTIPLE);
			} else if (!found_c_opt) {
				cc_log("Called for link with %s", argv[i]);
				if (strstr(argv[i], "conftest.")) {
					stats_update(STATS_CONFTEST);
				} else {
					stats_update(STATS_LINK);
				}
			} else {
				cc_log("Unsupported source extension: %s", argv[i]);
				stats_update(STATS_SOURCELANG);
			}
			failed();
		}

		/* Rewrite to relative to increase hit rate. */
		input_file = make_relative_path(x_strdup(argv[i]));
	}

	if (!input_file) {
		cc_log("No input file found");
		stats_update(STATS_NOINPUT);
		failed();
	}

	if (explicit_language && strcmp(explicit_language, "none") == 0) {
		explicit_language = NULL;
	}
	file_language = language_for_file(input_file);
	if (explicit_language) {
		if (!language_is_supported(explicit_language)) {
			cc_log("Unsupported language: %s", explicit_language);
			stats_update(STATS_SOURCELANG);
			failed();
		}
		actual_language = explicit_language;
	} else {
		actual_language = file_language;
	}
	if (!actual_language) {
		cc_log("Unsupported source extension: %s", input_file);
		stats_update(STATS_SOURCELANG);
		failed();
	}
	direct_i_file = language_is_preprocessed(actual_language);

	i_extension = getenv("CCACHE_EXTENSION");
	if (!i_extension) {
		i_extension = i_extension_for_language(actual_language) + 1;
	}

	if (!found_c_opt) {
		cc_log("No -c option found");
		/* I find that having a separate statistic for autoconf tests is useful,
		   as they are the dominant form of "called for link" in many cases */
		if (strstr(input_file, "conftest.")) {
			stats_update(STATS_CONFTEST);
		} else {
			stats_update(STATS_LINK);
		}
		failed();
	}

	/* don't try to second guess the compilers heuristics for stdout handling */
	if (output_obj && strcmp(output_obj, "-") == 0) {
		stats_update(STATS_OUTSTDOUT);
		cc_log("Output file is -");
		failed();
	}

	if (!output_obj) {
		char *p;
		output_obj = x_strdup(input_file);
		if ((p = strrchr(output_obj, '/'))) {
			output_obj = p+1;
		}
		p = strrchr(output_obj, '.');
		if (!p || !p[1]) {
			cc_log("Badly formed object filename");
			stats_update(STATS_ARGS);
			failed();
		}
		p[1] = found_S_opt ? 's' : 'o';
		p[2] = 0;
	}

	/* If dependencies are generated, configure the preprocessor */

	if (generating_dependencies) {
		if (!dependency_filename_specified) {
			char *default_depfile_name;
			char *base_name;

			base_name = remove_extension(output_obj);
			x_asprintf(&default_depfile_name, "%s.d", base_name);
			free(base_name);
			args_add(stripped_args, "-MF");
			args_add(stripped_args, default_depfile_name);
			output_dep = make_relative_path(
				x_strdup(default_depfile_name));
		}

		if (!dependency_target_specified) {
			args_add(stripped_args, "-MT");
			args_add(stripped_args, output_obj);
		}
	}

	/* cope with -o /dev/null */
	if (strcmp(output_obj,"/dev/null") != 0
	    && stat(output_obj, &st) == 0
	    && !S_ISREG(st.st_mode)) {
		cc_log("Not a regular file: %s", output_obj);
		stats_update(STATS_DEVICE);
		failed();
	}

	/*
	 * Some options shouldn't be passed to the real compiler when it compiles
	 * preprocessed code:
	 *
	 * -finput-charset=XXX (otherwise conversion happens twice)
	 * -x XXX (otherwise the wrong language is selected)
	 */
	*preprocessor_args = args_copy(stripped_args);
	if (input_charset) {
		args_add(*preprocessor_args, input_charset);
	}
	if (explicit_language) {
		args_add(*preprocessor_args, "-x");
		args_add(*preprocessor_args, explicit_language);
	}
	if (compile_preprocessed_source_code) {
		*compiler_args = args_copy(stripped_args);
		if (explicit_language) {
			char *p;
			x_asprintf(&p, ".%s", i_extension);
			args_add(*compiler_args, "-x");
			args_add(*compiler_args, language_for_file(p));
			free(p);
		}
	} else {
		*compiler_args = args_copy(*preprocessor_args);
	}
	args_free(stripped_args);
}

static unsigned parse_sloppiness(char *p)
{
	unsigned result = 0;
	char *word, *q;

	if (!p) {
		return result;
	}
	p = x_strdup(p);
	q = p;
	while ((word = strtok(q, ", "))) {
		if (strcmp(word, "file_macro") == 0) {
			cc_log("Being sloppy about __FILE__");
			result |= SLOPPY_FILE_MACRO;
		}
		if (strcmp(word, "include_file_mtime") == 0) {
			cc_log("Being sloppy about include file mtime");
			result |= SLOPPY_INCLUDE_FILE_MTIME;
		}
		if (strcmp(word, "time_macros") == 0) {
			cc_log("Being sloppy about __DATE__ and __TIME__");
			result |= SLOPPY_TIME_MACROS;
		}
		q = NULL;
	}
	free(p);
	return result;
}

/* the main ccache driver function */
static void ccache(int argc, char *argv[])
{
	int put_object_in_manifest = 0;
	struct file_hash *object_hash;
	struct file_hash *object_hash_from_manifest = NULL;
	char *env;
	struct mdfour common_hash;
	struct mdfour direct_hash;
	struct mdfour cpp_hash;

	/* Arguments (except -E) to send to the preprocessor. */
	ARGS *preprocessor_args;

	/* Arguments to send to the real compiler. */
	ARGS *compiler_args;

	cc_log("=== CCACHE STARTED =========================================");

	sloppiness = parse_sloppiness(getenv("CCACHE_SLOPPINESS"));

	cc_log("Hostname: %s", get_hostname());
	cc_log("Working directory: %s", current_working_dir);

	if (base_dir) {
		cc_log("Base directory: %s", base_dir);
	}

	find_compiler(argc, argv);

	if (getenv("CCACHE_DISABLE")) {
		cc_log("ccache is disabled");
		failed();
	}

	if (getenv("CCACHE_UNIFY")) {
		cc_log("Unify mode disabled");
		enable_unify = 1;
	}

	if (getenv("CCACHE_NODIRECT") || enable_unify) {
		cc_log("Direct mode disabled");
		enable_direct = 0;
	}

	if (getenv("CCACHE_COMPRESS")) {
		cc_log("Compression enabled");
		enable_compression = 1;
	}

	if ((env = getenv("CCACHE_NLEVELS"))) {
		nlevels = atoi(env);
		if (nlevels < 1) nlevels = 1;
		if (nlevels > 8) nlevels = 8;
	}

	/*
	 * Process argument list, returning a new set of arguments to pass to
	 * the preprocessor and the real compiler.
	 */
	process_args(orig_args->argc, orig_args->argv, &preprocessor_args,
		     &compiler_args);

	cc_log("Source file: %s", input_file);
	if (generating_dependencies) {
		cc_log("Dependency file: %s", output_dep);
	}
	cc_log("Object file: %s", output_obj);

	hash_start(&common_hash);
	calculate_common_hash(preprocessor_args, &common_hash);

	/* try to find the hash using the manifest */
	direct_hash = common_hash;
	if (enable_direct) {
		cc_log("Trying direct lookup");
		object_hash = calculate_object_hash(
			preprocessor_args, &direct_hash, 1);
		if (object_hash) {
			update_cached_result_globals(object_hash);

			/*
			 * If we can return from cache at this point then do
			 * so.
			 */
			from_cache(FROMCACHE_DIRECT_MODE, 0);

			/*
			 * Wasn't able to return from cache at this point.
			 * However, the object was already found in manifest,
			 * so don't readd it later.
			 */
			put_object_in_manifest = 0;

			object_hash_from_manifest = object_hash;
		} else {
			/* Add object to manifest later. */
			put_object_in_manifest = 1;
		}
	}

	/*
	 * Find the hash using the preprocessed output. Also updates
	 * included_files.
	 */
	cpp_hash = common_hash;
	cc_log("Running preprocessor");
	object_hash = calculate_object_hash(preprocessor_args, &cpp_hash, 0);
	if (!object_hash) {
		fatal("internal error: object hash from cpp returned NULL");
	}
	update_cached_result_globals(object_hash);

	if (object_hash_from_manifest
	    && !file_hashes_equal(object_hash_from_manifest, object_hash)) {
		/*
		 * The hash from manifest differs from the hash of the
		 * preprocessor output. This could be because:
		 *
		 * - The preprocessor produces different output for the same
		 *   input (not likely).
		 * - There's a bug in ccache (maybe incorrect handling of
		 *   compiler arguments).
		 * - The user has used a different CCACHE_BASEDIR (most
		 *   likely).
		 *
		 * The best thing here would probably be to remove the hash
		 * entry from the manifest. For now, we use a simpler method:
		 * just remove the manifest file.
		 */
		cc_log("Hash from manifest doesn't match preprocessor output");
		cc_log("Likely reason: different CCACHE_BASEDIRs used");
		cc_log("Removing manifest as a safety measure");
		unlink(manifest_path);

		put_object_in_manifest = 1;
	}

	/* if we can return from cache at this point then do */
	from_cache(FROMCACHE_CPP_MODE, put_object_in_manifest);

	if (getenv("CCACHE_READONLY")) {
		cc_log("Read-only mode; running real compiler");
		failed();
	}

	env = getenv("CCACHE_PREFIX");
	if (env) {
		char *p = find_executable(env, MYNAME);
		if (!p) {
			perror(env);
			exit(1);
		}
		cc_log("Using command-line prefix %s", env);
		args_add_prefix(compiler_args, p);
	}

	/* run real compiler, sending output to cache */
	to_cache(compiler_args);

	/* return from cache */
	from_cache(FROMCACHE_COMPILED_MODE, put_object_in_manifest);

	/* oh oh! */
	cc_log("Secondary from_cache failed");
	stats_update(STATS_ERROR);
	failed();
}

static void check_cache_dir(void)
{
	if (!cache_dir) {
		fatal("Unable to determine cache directory");
	}
}

/* the main program when not doing a compile */
static int ccache_main(int argc, char *argv[])
{
	int c;
	size_t v;

	static const struct option long_options[] = {
		{"show-stats", no_argument,       0, 's'},
		{"zero-stats", no_argument,       0, 'z'},
		{"cleanup",    no_argument,       0, 'c'},
		{"clear",      no_argument,       0, 'C'},
		{"max-files",  required_argument, 0, 'F'},
		{"max-size",   required_argument, 0, 'M'},
		{"help",       no_argument,       0, 'h'},
		{"version",    no_argument,       0, 'V'},
		{0, 0, 0, 0}
	};
	int option_index = 0;

	while ((c = getopt_long(argc, argv, "hszcCF:M:V", long_options, &option_index)) != -1) {
		switch (c) {
		case 'V':
			fprintf(stdout, VERSION_TEXT, CCACHE_VERSION);
			exit(0);

		case 'h':
			fputs(USAGE_TEXT, stdout);
			exit(0);

		case 's':
			check_cache_dir();
			stats_summary();
			break;

		case 'c':
			check_cache_dir();
			cleanup_all(cache_dir);
			printf("Cleaned cache\n");
			break;

		case 'C':
			check_cache_dir();
			wipe_all(cache_dir);
			printf("Cleared cache\n");
			break;

		case 'z':
			check_cache_dir();
			stats_zero();
			printf("Statistics cleared\n");
			break;

		case 'F':
			check_cache_dir();
			v = atoi(optarg);
			if (stats_set_limits(v, -1) == 0) {
				if (v == 0) {
					printf("Unset cache file limit\n");
				} else {
					printf("Set cache file limit to %u\n", (unsigned)v);
				}
			} else {
				printf("Could not set cache file limit.\n");
				exit(1);
			}
			break;

		case 'M':
			check_cache_dir();
			v = value_units(optarg);
			if (stats_set_limits(-1, v) == 0) {
				if (v == 0) {
					printf("Unset cache size limit\n");
				} else {
					char *s = format_size(v);
					printf("Set cache size limit to %s\n",
					       s);
					free(s);
				}
			} else {
				printf("Could not set cache size limit.\n");
				exit(1);
			}
			break;

		default:
			fputs(USAGE_TEXT, stderr);
			exit(1);
		}
	}

	return 0;
}


/* Make a copy of stderr that will not be cached, so things like
   distcc can send networking errors to it. */
static void setup_uncached_err(void)
{
	char *buf;
	int uncached_fd;

	uncached_fd = dup(2);
	if (uncached_fd == -1) {
		cc_log("dup(2) failed");
		failed();
	}

	/* leak a pointer to the environment */
	x_asprintf(&buf, "UNCACHED_ERR_FD=%d", uncached_fd);

	if (putenv(buf) == -1) {
		cc_log("putenv failed");
		failed();
	}
}


int main(int argc, char *argv[])
{
	char *p;
	char *program_name;

	/* the user might have set CCACHE_UMASK */
	p = getenv("CCACHE_UMASK");
	if (p) {
		mode_t mask;
		errno = 0;
		mask = strtol(p, NULL, 8);
		if (errno == 0) {
			umask(mask);
		}
	}

	current_working_dir = get_cwd();
	cache_dir = getenv("CCACHE_DIR");
	if (!cache_dir) {
		const char *home_directory = get_home_directory();
		if (home_directory) {
			x_asprintf(&cache_dir, "%s/.ccache", home_directory);
		}
	}

	/* check if we are being invoked as "ccache" */
	program_name = basename(argv[0]);
	if (strcmp(program_name, MYNAME) == 0) {
		if (argc < 2) {
			fputs(USAGE_TEXT, stderr);
			exit(1);
		}
		/* if the first argument isn't an option, then assume we are
		   being passed a compiler name and options */
		if (argv[1][0] == '-') {
			return ccache_main(argc, argv);
		}
	}
	free(program_name);

	check_cache_dir();

	temp_dir = getenv("CCACHE_TEMPDIR");
	if (!temp_dir) {
		x_asprintf(&temp_dir, "%s/tmp", cache_dir);
	}

	cache_logfile = getenv("CCACHE_LOGFILE");

	base_dir = getenv("CCACHE_BASEDIR");
	if (base_dir && base_dir[0] != '/') {
		cc_log("Ignoring non-absolute base directory %s", base_dir);
		base_dir = NULL;
	}

	compile_preprocessed_source_code = !getenv("CCACHE_CPP2");

	setup_uncached_err();

	/* make sure the cache dir exists */
	if (create_dir(cache_dir) != 0) {
		fprintf(stderr,"ccache: failed to create %s (%s)\n",
			cache_dir, strerror(errno));
		exit(1);
	}

	/* make sure the temp dir exists */
	if (create_dir(temp_dir) != 0) {
		fprintf(stderr,"ccache: failed to create %s (%s)\n",
			temp_dir, strerror(errno));
		exit(1);
	}

	if (!getenv("CCACHE_READONLY")) {
		if (create_cachedirtag(cache_dir) != 0) {
			fprintf(stderr,"ccache: failed to create %s/CACHEDIR.TAG (%s)\n",
				cache_dir, strerror(errno));
			exit(1);
		}
	}

	ccache(argc, argv);
	return 1;
}
