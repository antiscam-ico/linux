// SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause)
/* Copyright (c) 2022 Meta Platforms, Inc. and affiliates. */
#define _GNU_SOURCE
#include <argp.h>
#include <libgen.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <sched.h>
#include <pthread.h>
#include <dirent.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/sysinfo.h>
#include <sys/stat.h>
#include <bpf/libbpf.h>
#include <bpf/btf.h>
#include <bpf/bpf.h>
#include <libelf.h>
#include <gelf.h>
#include <float.h>
#include <math.h>
#include <limits.h>
#include <assert.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif

enum stat_id {
	VERDICT,
	DURATION,
	TOTAL_INSNS,
	TOTAL_STATES,
	PEAK_STATES,
	MAX_STATES_PER_INSN,
	MARK_READ_MAX_LEN,
	SIZE,
	JITED_SIZE,
	STACK,
	PROG_TYPE,
	ATTACH_TYPE,
	MEMORY_PEAK,

	FILE_NAME,
	PROG_NAME,

	ALL_STATS_CNT,
	NUM_STATS_CNT = FILE_NAME - VERDICT,
};

/* In comparison mode each stat can specify up to four different values:
 *   - A side value;
 *   - B side value;
 *   - absolute diff value;
 *   - relative (percentage) diff value.
 *
 * When specifying stat specs in comparison mode, user can use one of the
 * following variant suffixes to specify which exact variant should be used for
 * ordering or filtering:
 *   - `_a` for A side value;
 *   - `_b` for B side value;
 *   - `_diff` for absolute diff value;
 *   - `_pct` for relative (percentage) diff value.
 *
 * If no variant suffix is provided, then `_b` (control data) is assumed.
 *
 * As an example, let's say instructions stat has the following output:
 *
 * Insns (A)  Insns (B)  Insns   (DIFF)
 * ---------  ---------  --------------
 * 21547      20920       -627 (-2.91%)
 *
 * Then:
 *   - 21547 is A side value (insns_a);
 *   - 20920 is B side value (insns_b);
 *   - -627 is absolute diff value (insns_diff);
 *   - -2.91% is relative diff value (insns_pct).
 *
 * For verdict there is no verdict_pct variant.
 * For file and program name, _a and _b variants are equivalent and there are
 * no _diff or _pct variants.
 */
enum stat_variant {
	VARIANT_A,
	VARIANT_B,
	VARIANT_DIFF,
	VARIANT_PCT,
};

struct verif_stats {
	char *file_name;
	char *prog_name;

	long stats[NUM_STATS_CNT];
};

/* joined comparison mode stats */
struct verif_stats_join {
	char *file_name;
	char *prog_name;

	const struct verif_stats *stats_a;
	const struct verif_stats *stats_b;
};

struct stat_specs {
	int spec_cnt;
	enum stat_id ids[ALL_STATS_CNT];
	enum stat_variant variants[ALL_STATS_CNT];
	bool asc[ALL_STATS_CNT];
	bool abs[ALL_STATS_CNT];
	int lens[ALL_STATS_CNT * 3]; /* 3x for comparison mode */
};

enum resfmt {
	RESFMT_TABLE,
	RESFMT_TABLE_CALCLEN, /* fake format to pre-calculate table's column widths */
	RESFMT_CSV,
};

enum filter_kind {
	FILTER_NAME,
	FILTER_STAT,
};

enum operator_kind {
	OP_EQ,		/* == or = */
	OP_NEQ,		/* != or <> */
	OP_LT,		/* < */
	OP_LE,		/* <= */
	OP_GT,		/* > */
	OP_GE,		/* >= */
};

struct filter {
	enum filter_kind kind;
	/* FILTER_NAME */
	char *any_glob;
	char *file_glob;
	char *prog_glob;
	/* FILTER_STAT */
	enum operator_kind op;
	int stat_id;
	enum stat_variant stat_var;
	long value;
	bool abs;
};

struct rvalue {
	enum { INTEGRAL, ENUMERATOR } type;
	union {
		long long ivalue;
		char *svalue;
	};
};

struct field_access {
	enum { FIELD_NAME, ARRAY_INDEX } type;
	union {
		char *name;
		struct rvalue index;
	};
};

struct var_preset {
	struct field_access *atoms;
	int atom_count;
	char *full_name;
	struct rvalue value;
	bool applied;
};

static struct env {
	char **filenames;
	int filename_cnt;
	bool verbose;
	bool debug;
	bool quiet;
	bool force_checkpoints;
	bool force_reg_invariants;
	enum resfmt out_fmt;
	bool show_version;
	bool comparison_mode;
	bool replay_mode;
	int top_n;

	int log_level;
	int log_size;
	bool log_fixed;

	struct verif_stats *prog_stats;
	int prog_stat_cnt;

	/* baseline_stats is allocated and used only in comparison mode */
	struct verif_stats *baseline_stats;
	int baseline_stat_cnt;

	struct verif_stats_join *join_stats;
	int join_stat_cnt;

	struct stat_specs output_spec;
	struct stat_specs sort_spec;

	struct filter *allow_filters;
	struct filter *deny_filters;
	int allow_filter_cnt;
	int deny_filter_cnt;

	int files_processed;
	int files_skipped;
	int progs_processed;
	int progs_skipped;
	int top_src_lines;
	struct var_preset *presets;
	int npresets;
	char orig_cgroup[PATH_MAX];
	char stat_cgroup[PATH_MAX];
	int memory_peak_fd;
} env;

static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args)
{
	if (!env.verbose)
		return 0;
	if (level == LIBBPF_DEBUG  && !env.debug)
		return 0;
	return vfprintf(stderr, format, args);
}

#define log_errno(fmt, ...) log_errno_aux(__FILE__, __LINE__, fmt, ##__VA_ARGS__)

__attribute__((format(printf, 3, 4)))
static int log_errno_aux(const char *file, int line, const char *fmt, ...)
{
	int err = -errno;
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "%s:%d: ", file, line);
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, " failed with error '%s'.\n", strerror(errno));
	va_end(ap);
	return err;
}

#ifndef VERISTAT_VERSION
#define VERISTAT_VERSION "<kernel>"
#endif

const char *argp_program_version = "veristat v" VERISTAT_VERSION;
const char *argp_program_bug_address = "<bpf@vger.kernel.org>";
const char argp_program_doc[] =
"veristat    BPF verifier stats collection and comparison tool.\n"
"\n"
"USAGE: veristat <obj-file> [<obj-file>...]\n"
"   OR: veristat -C <baseline.csv> <comparison.csv>\n"
"   OR: veristat -R <results.csv>\n"
"   OR: veristat -vl2 <to_analyze.bpf.o>\n";

enum {
	OPT_LOG_FIXED = 1000,
	OPT_LOG_SIZE = 1001,
};

static const struct argp_option opts[] = {
	{ NULL, 'h', NULL, OPTION_HIDDEN, "Show the full help" },
	{ "version", 'V', NULL, 0, "Print version" },
	{ "verbose", 'v', NULL, 0, "Verbose mode" },
	{ "debug", 'd', NULL, 0, "Debug mode (turns on libbpf debug logging)" },
	{ "log-level", 'l', "LEVEL", 0, "Verifier log level (default 0 for normal mode, 1 for verbose mode, 2 for full verification log)" },
	{ "log-fixed", OPT_LOG_FIXED, NULL, 0, "Disable verifier log rotation" },
	{ "log-size", OPT_LOG_SIZE, "BYTES", 0, "Customize verifier log size (default to 16MB)" },
	{ "top-n", 'n', "N", 0, "Emit only up to first N results." },
	{ "quiet", 'q', NULL, 0, "Quiet mode" },
	{ "emit", 'e', "SPEC", 0, "Specify stats to be emitted" },
	{ "sort", 's', "SPEC", 0, "Specify sort order" },
	{ "output-format", 'o', "FMT", 0, "Result output format (table, csv), default is table." },
	{ "compare", 'C', NULL, 0, "Comparison mode" },
	{ "replay", 'R', NULL, 0, "Replay mode" },
	{ "filter", 'f', "FILTER", 0, "Filter expressions (or @filename for file with expressions)." },
	{ "test-states", 't', NULL, 0,
	  "Force frequent BPF verifier state checkpointing (set BPF_F_TEST_STATE_FREQ program flag)" },
	{ "test-reg-invariants", 'r', NULL, 0,
	  "Force BPF verifier failure on register invariant violation (BPF_F_TEST_REG_INVARIANTS program flag)" },
	{ "top-src-lines", 'S', "N", 0, "Emit N most frequent source code lines" },
	{ "set-global-vars", 'G', "GLOBAL", 0, "Set global variables provided in the expression, for example \"var1 = 1\"" },
	{},
};

static int parse_stats(const char *stats_str, struct stat_specs *specs);
static int append_filter(struct filter **filters, int *cnt, const char *str);
static int append_filter_file(const char *path);
static int append_var_preset(struct var_preset **presets, int *cnt, const char *expr);
static int append_var_preset_file(const char *filename);
static int append_file(const char *path);
static int append_file_from_file(const char *path);

static error_t parse_arg(int key, char *arg, struct argp_state *state)
{
	int err;

	switch (key) {
	case 'h':
		argp_state_help(state, stderr, ARGP_HELP_STD_HELP);
		break;
	case 'V':
		env.show_version = true;
		break;
	case 'v':
		env.verbose = true;
		break;
	case 'd':
		env.debug = true;
		env.verbose = true;
		break;
	case 'q':
		env.quiet = true;
		break;
	case 'e':
		err = parse_stats(arg, &env.output_spec);
		if (err)
			return err;
		break;
	case 's':
		err = parse_stats(arg, &env.sort_spec);
		if (err)
			return err;
		break;
	case 'o':
		if (strcmp(arg, "table") == 0) {
			env.out_fmt = RESFMT_TABLE;
		} else if (strcmp(arg, "csv") == 0) {
			env.out_fmt = RESFMT_CSV;
		} else {
			fprintf(stderr, "Unrecognized output format '%s'\n", arg);
			return -EINVAL;
		}
		break;
	case 'l':
		errno = 0;
		env.log_level = strtol(arg, NULL, 10);
		if (errno) {
			fprintf(stderr, "invalid log level: %s\n", arg);
			argp_usage(state);
		}
		break;
	case OPT_LOG_FIXED:
		env.log_fixed = true;
		break;
	case OPT_LOG_SIZE:
		errno = 0;
		env.log_size = strtol(arg, NULL, 10);
		if (errno) {
			fprintf(stderr, "invalid log size: %s\n", arg);
			argp_usage(state);
		}
		break;
	case 't':
		env.force_checkpoints = true;
		break;
	case 'r':
		env.force_reg_invariants = true;
		break;
	case 'n':
		errno = 0;
		env.top_n = strtol(arg, NULL, 10);
		if (errno) {
			fprintf(stderr, "invalid top N specifier: %s\n", arg);
			argp_usage(state);
		}
		break;
	case 'C':
		env.comparison_mode = true;
		break;
	case 'R':
		env.replay_mode = true;
		break;
	case 'f':
		if (arg[0] == '@')
			err = append_filter_file(arg + 1);
		else if (arg[0] == '!')
			err = append_filter(&env.deny_filters, &env.deny_filter_cnt, arg + 1);
		else
			err = append_filter(&env.allow_filters, &env.allow_filter_cnt, arg);
		if (err) {
			fprintf(stderr, "Failed to collect program filter expressions: %d\n", err);
			return err;
		}
		break;
	case 'S':
		errno = 0;
		env.top_src_lines = strtol(arg, NULL, 10);
		if (errno) {
			fprintf(stderr, "invalid top lines N specifier: %s\n", arg);
			argp_usage(state);
		}
		break;
	case 'G': {
		if (arg[0] == '@')
			err = append_var_preset_file(arg + 1);
		else
			err = append_var_preset(&env.presets, &env.npresets, arg);
		if (err) {
			fprintf(stderr, "Failed to parse global variable presets: %s\n", arg);
			return err;
		}
		break;
	}
	case ARGP_KEY_ARG:
		if (arg[0] == '@')
			err = append_file_from_file(arg + 1);
		else
			err = append_file(arg);
		if (err) {
			fprintf(stderr, "Failed to collect BPF object files: %d\n", err);
			return err;
		}
		break;
	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static const struct argp argp = {
	.options = opts,
	.parser = parse_arg,
	.doc = argp_program_doc,
};


/* Adapted from perf/util/string.c */
static bool glob_matches(const char *str, const char *pat)
{
	while (*str && *pat && *pat != '*') {
		if (*str != *pat)
			return false;
		str++;
		pat++;
	}
	/* Check wild card */
	if (*pat == '*') {
		while (*pat == '*')
			pat++;
		if (!*pat) /* Tail wild card matches all */
			return true;
		while (*str)
			if (glob_matches(str++, pat))
				return true;
	}
	return !*str && !*pat;
}

static bool is_bpf_obj_file(const char *path) {
	Elf64_Ehdr *ehdr;
	int fd, err = -EINVAL;
	Elf *elf = NULL;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return true; /* we'll fail later and propagate error */

	/* ensure libelf is initialized */
	(void)elf_version(EV_CURRENT);

	elf = elf_begin(fd, ELF_C_READ, NULL);
	if (!elf)
		goto cleanup;

	if (elf_kind(elf) != ELF_K_ELF || gelf_getclass(elf) != ELFCLASS64)
		goto cleanup;

	ehdr = elf64_getehdr(elf);
	/* Old LLVM set e_machine to EM_NONE */
	if (!ehdr || ehdr->e_type != ET_REL || (ehdr->e_machine && ehdr->e_machine != EM_BPF))
		goto cleanup;

	err = 0;
cleanup:
	if (elf)
		elf_end(elf);
	close(fd);
	return err == 0;
}

static bool should_process_file_prog(const char *filename, const char *prog_name)
{
	struct filter *f;
	int i, allow_cnt = 0;

	for (i = 0; i < env.deny_filter_cnt; i++) {
		f = &env.deny_filters[i];
		if (f->kind != FILTER_NAME)
			continue;

		if (f->any_glob && glob_matches(filename, f->any_glob))
			return false;
		if (f->any_glob && prog_name && glob_matches(prog_name, f->any_glob))
			return false;
		if (f->file_glob && glob_matches(filename, f->file_glob))
			return false;
		if (f->prog_glob && prog_name && glob_matches(prog_name, f->prog_glob))
			return false;
	}

	for (i = 0; i < env.allow_filter_cnt; i++) {
		f = &env.allow_filters[i];
		if (f->kind != FILTER_NAME)
			continue;

		allow_cnt++;
		if (f->any_glob) {
			if (glob_matches(filename, f->any_glob))
				return true;
			/* If we don't know program name yet, any_glob filter
			 * has to assume that current BPF object file might be
			 * relevant; we'll check again later on after opening
			 * BPF object file, at which point program name will
			 * be known finally.
			 */
			if (!prog_name || glob_matches(prog_name, f->any_glob))
				return true;
		} else {
			if (f->file_glob && !glob_matches(filename, f->file_glob))
				continue;
			if (f->prog_glob && prog_name && !glob_matches(prog_name, f->prog_glob))
				continue;
			return true;
		}
	}

	/* if there are no file/prog name allow filters, allow all progs,
	 * unless they are denied earlier explicitly
	 */
	return allow_cnt == 0;
}

static struct {
	enum operator_kind op_kind;
	const char *op_str;
} operators[] = {
	/* Order of these definitions matter to avoid situations like '<'
	 * matching part of what is actually a '<>' operator. That is,
	 * substrings should go last.
	 */
	{ OP_EQ, "==" },
	{ OP_NEQ, "!=" },
	{ OP_NEQ, "<>" },
	{ OP_LE, "<=" },
	{ OP_LT, "<" },
	{ OP_GE, ">=" },
	{ OP_GT, ">" },
	{ OP_EQ, "=" },
};

static bool parse_stat_id_var(const char *name, size_t len, int *id,
			      enum stat_variant *var, bool *is_abs);

static int append_filter(struct filter **filters, int *cnt, const char *str)
{
	struct filter *f;
	void *tmp;
	const char *p;
	int i;

	tmp = realloc(*filters, (*cnt + 1) * sizeof(**filters));
	if (!tmp)
		return -ENOMEM;
	*filters = tmp;

	f = &(*filters)[*cnt];
	memset(f, 0, sizeof(*f));

	/* First, let's check if it's a stats filter of the following form:
	 * <stat><op><value, where:
	 *   - <stat> is one of supported numerical stats (verdict is also
	 *     considered numerical, failure == 0, success == 1);
	 *   - <op> is comparison operator (see `operators` definitions);
	 *   - <value> is an integer (or failure/success, or false/true as
	 *     special aliases for 0 and 1, respectively).
	 * If the form doesn't match what user provided, we assume file/prog
	 * glob filter.
	 */
	for (i = 0; i < ARRAY_SIZE(operators); i++) {
		enum stat_variant var;
		int id;
		long val;
		const char *end = str;
		const char *op_str;
		bool is_abs;

		op_str = operators[i].op_str;
		p = strstr(str, op_str);
		if (!p)
			continue;

		if (!parse_stat_id_var(str, p - str, &id, &var, &is_abs)) {
			fprintf(stderr, "Unrecognized stat name in '%s'!\n", str);
			return -EINVAL;
		}
		if (id >= FILE_NAME) {
			fprintf(stderr, "Non-integer stat is specified in '%s'!\n", str);
			return -EINVAL;
		}

		p += strlen(op_str);

		if (strcasecmp(p, "true") == 0 ||
		    strcasecmp(p, "t") == 0 ||
		    strcasecmp(p, "success") == 0 ||
		    strcasecmp(p, "succ") == 0 ||
		    strcasecmp(p, "s") == 0 ||
		    strcasecmp(p, "match") == 0 ||
		    strcasecmp(p, "m") == 0) {
			val = 1;
		} else if (strcasecmp(p, "false") == 0 ||
			   strcasecmp(p, "f") == 0 ||
			   strcasecmp(p, "failure") == 0 ||
			   strcasecmp(p, "fail") == 0 ||
			   strcasecmp(p, "mismatch") == 0 ||
			   strcasecmp(p, "mis") == 0) {
			val = 0;
		} else {
			errno = 0;
			val = strtol(p, (char **)&end, 10);
			if (errno || end == p || *end != '\0' ) {
				fprintf(stderr, "Invalid integer value in '%s'!\n", str);
				return -EINVAL;
			}
		}

		f->kind = FILTER_STAT;
		f->stat_id = id;
		f->stat_var = var;
		f->op = operators[i].op_kind;
		f->abs = true;
		f->value = val;

		*cnt += 1;
		return 0;
	}

	/* File/prog filter can be specified either as '<glob>' or
	 * '<file-glob>/<prog-glob>'. In the former case <glob> is applied to
	 * both file and program names. This seems to be way more useful in
	 * practice. If user needs full control, they can use '/<prog-glob>'
	 * form to glob just program name, or '<file-glob>/' to glob only file
	 * name. But usually common <glob> seems to be the most useful and
	 * ergonomic way.
	 */
	f->kind = FILTER_NAME;
	p = strchr(str, '/');
	if (!p) {
		f->any_glob = strdup(str);
		if (!f->any_glob)
			return -ENOMEM;
	} else {
		if (str != p) {
			/* non-empty file glob */
			f->file_glob = strndup(str, p - str);
			if (!f->file_glob)
				return -ENOMEM;
		}
		if (strlen(p + 1) > 0) {
			/* non-empty prog glob */
			f->prog_glob = strdup(p + 1);
			if (!f->prog_glob) {
				free(f->file_glob);
				f->file_glob = NULL;
				return -ENOMEM;
			}
		}
	}

	*cnt += 1;
	return 0;
}

static int append_filter_file(const char *path)
{
	char buf[1024];
	FILE *f;
	int err = 0;

	f = fopen(path, "r");
	if (!f) {
		err = -errno;
		fprintf(stderr, "Failed to open filters in '%s': %s\n", path, strerror(-err));
		return err;
	}

	while (fscanf(f, " %1023[^\n]\n", buf) == 1) {
		/* lines starting with # are comments, skip them */
		if (buf[0] == '\0' || buf[0] == '#')
			continue;
		/* lines starting with ! are negative match filters */
		if (buf[0] == '!')
			err = append_filter(&env.deny_filters, &env.deny_filter_cnt, buf + 1);
		else
			err = append_filter(&env.allow_filters, &env.allow_filter_cnt, buf);
		if (err)
			goto cleanup;
	}

cleanup:
	fclose(f);
	return err;
}

static const struct stat_specs default_output_spec = {
	.spec_cnt = 8,
	.ids = {
		FILE_NAME, PROG_NAME, VERDICT, DURATION,
		TOTAL_INSNS, TOTAL_STATES, SIZE, JITED_SIZE
	},
};

static int append_file(const char *path)
{
	void *tmp;

	tmp = realloc(env.filenames, (env.filename_cnt + 1) * sizeof(*env.filenames));
	if (!tmp)
		return -ENOMEM;
	env.filenames = tmp;
	env.filenames[env.filename_cnt] = strdup(path);
	if (!env.filenames[env.filename_cnt])
		return -ENOMEM;
	env.filename_cnt++;
	return 0;
}

static int append_file_from_file(const char *path)
{
	char buf[1024];
	int err = 0;
	FILE *f;

	f = fopen(path, "r");
	if (!f) {
		err = -errno;
		fprintf(stderr, "Failed to open object files list in '%s': %s\n",
			path, strerror(errno));
		return err;
	}

	while (fscanf(f, " %1023[^\n]\n", buf) == 1) {
		/* lines starting with # are comments, skip them */
		if (buf[0] == '\0' || buf[0] == '#')
			continue;
		err = append_file(buf);
		if (err)
			goto cleanup;
	}

cleanup:
	fclose(f);
	return err;
}

static const struct stat_specs default_csv_output_spec = {
	.spec_cnt = 15,
	.ids = {
		FILE_NAME, PROG_NAME, VERDICT, DURATION,
		TOTAL_INSNS, TOTAL_STATES, PEAK_STATES,
		MAX_STATES_PER_INSN, MARK_READ_MAX_LEN,
		SIZE, JITED_SIZE, PROG_TYPE, ATTACH_TYPE,
		STACK, MEMORY_PEAK,
	},
};

static const struct stat_specs default_sort_spec = {
	.spec_cnt = 2,
	.ids = {
		FILE_NAME, PROG_NAME,
	},
	.asc = { true, true, },
};

/* sorting for comparison mode to join two data sets */
static const struct stat_specs join_sort_spec = {
	.spec_cnt = 2,
	.ids = {
		FILE_NAME, PROG_NAME,
	},
	.asc = { true, true, },
};

static struct stat_def {
	const char *header;
	const char *names[4];
	bool asc_by_default;
	bool left_aligned;
} stat_defs[] = {
	[FILE_NAME] = { "File", {"file_name", "filename", "file"}, true /* asc */, true /* left */ },
	[PROG_NAME] = { "Program", {"prog_name", "progname", "prog"}, true /* asc */, true /* left */ },
	[VERDICT] = { "Verdict", {"verdict"}, true /* asc: failure, success */, true /* left */ },
	[DURATION] = { "Duration (us)", {"duration", "dur"}, },
	[TOTAL_INSNS] = { "Insns", {"total_insns", "insns"}, },
	[TOTAL_STATES] = { "States", {"total_states", "states"}, },
	[PEAK_STATES] = { "Peak states", {"peak_states"}, },
	[MAX_STATES_PER_INSN] = { "Max states per insn", {"max_states_per_insn"}, },
	[MARK_READ_MAX_LEN] = { "Max mark read length", {"max_mark_read_len", "mark_read"}, },
	[SIZE] = { "Program size", {"prog_size"}, },
	[JITED_SIZE] = { "Jited size", {"prog_size_jited"}, },
	[STACK] = {"Stack depth", {"stack_depth", "stack"}, },
	[PROG_TYPE] = { "Program type", {"prog_type"}, },
	[ATTACH_TYPE] = { "Attach type", {"attach_type", }, },
	[MEMORY_PEAK] = { "Peak memory (MiB)", {"mem_peak", }, },
};

static bool parse_stat_id_var(const char *name, size_t len, int *id,
			      enum stat_variant *var, bool *is_abs)
{
	static const char *var_sfxs[] = {
		[VARIANT_A] = "_a",
		[VARIANT_B] = "_b",
		[VARIANT_DIFF] = "_diff",
		[VARIANT_PCT] = "_pct",
	};
	int i, j, k;

	/* |<stat>| means we take absolute value of given stat */
	*is_abs = false;
	if (len > 2 && name[0] == '|' && name[len - 1] == '|') {
		*is_abs = true;
		name += 1;
		len -= 2;
	}

	for (i = 0; i < ARRAY_SIZE(stat_defs); i++) {
		struct stat_def *def = &stat_defs[i];
		size_t alias_len, sfx_len;
		const char *alias;

		for (j = 0; j < ARRAY_SIZE(stat_defs[i].names); j++) {
			alias = def->names[j];
			if (!alias)
				continue;

			alias_len = strlen(alias);
			if (strncmp(name, alias, alias_len) != 0)
				continue;

			if (alias_len == len) {
				/* If no variant suffix is specified, we
				 * assume control group (just in case we are
				 * in comparison mode. Variant is ignored in
				 * non-comparison mode.
				 */
				*var = VARIANT_B;
				*id = i;
				return true;
			}

			for (k = 0; k < ARRAY_SIZE(var_sfxs); k++) {
				sfx_len = strlen(var_sfxs[k]);
				if (alias_len + sfx_len != len)
					continue;

				if (strncmp(name + alias_len, var_sfxs[k], sfx_len) == 0) {
					*var = (enum stat_variant)k;
					*id = i;
					return true;
				}
			}
		}
	}

	return false;
}

static bool is_asc_sym(char c)
{
	return c == '^';
}

static bool is_desc_sym(char c)
{
	return c == 'v' || c == 'V' || c == '.' || c == '!' || c == '_';
}

static char *rtrim(char *str)
{
	int i;

	for (i = strlen(str) - 1; i > 0; --i) {
		if (!isspace(str[i]))
			break;
		str[i] = '\0';
	}
	return str;
}

static int parse_stat(const char *stat_name, struct stat_specs *specs)
{
	int id;
	bool has_order = false, is_asc = false, is_abs = false;
	size_t len = strlen(stat_name);
	enum stat_variant var;

	if (specs->spec_cnt >= ARRAY_SIZE(specs->ids)) {
		fprintf(stderr, "Can't specify more than %zd stats\n", ARRAY_SIZE(specs->ids));
		return -E2BIG;
	}

	if (len > 1 && (is_asc_sym(stat_name[len - 1]) || is_desc_sym(stat_name[len - 1]))) {
		has_order = true;
		is_asc = is_asc_sym(stat_name[len - 1]);
		len -= 1;
	}

	if (!parse_stat_id_var(stat_name, len, &id, &var, &is_abs)) {
		fprintf(stderr, "Unrecognized stat name '%s'\n", stat_name);
		return -ESRCH;
	}

	specs->ids[specs->spec_cnt] = id;
	specs->variants[specs->spec_cnt] = var;
	specs->asc[specs->spec_cnt] = has_order ? is_asc : stat_defs[id].asc_by_default;
	specs->abs[specs->spec_cnt] = is_abs;
	specs->spec_cnt++;

	return 0;
}

static int parse_stats(const char *stats_str, struct stat_specs *specs)
{
	char *input, *state = NULL, *next;
	int err, cnt = 0;

	input = strdup(stats_str);
	if (!input)
		return -ENOMEM;

	while ((next = strtok_r(cnt++ ? NULL : input, ",", &state))) {
		err = parse_stat(next, specs);
		if (err) {
			free(input);
			return err;
		}
	}

	free(input);
	return 0;
}

static void free_verif_stats(struct verif_stats *stats, size_t stat_cnt)
{
	int i;

	if (!stats)
		return;

	for (i = 0; i < stat_cnt; i++) {
		free(stats[i].file_name);
		free(stats[i].prog_name);
	}
	free(stats);
}

static char verif_log_buf[64 * 1024];

#define MAX_PARSED_LOG_LINES 100

static int parse_verif_log(char * const buf, size_t buf_sz, struct verif_stats *s)
{
	const char *cur;
	int pos, lines, sub_stack, cnt = 0;
	char *state = NULL, *token, stack[512];

	buf[buf_sz - 1] = '\0';

	for (pos = strlen(buf) - 1, lines = 0; pos >= 0 && lines < MAX_PARSED_LOG_LINES; lines++) {
		/* find previous endline or otherwise take the start of log buf */
		for (cur = &buf[pos]; cur > buf && cur[0] != '\n'; cur--, pos--) {
		}
		/* next time start from end of previous line (or pos goes to <0) */
		pos--;
		/* if we found endline, point right after endline symbol;
		 * otherwise, stay at the beginning of log buf
		 */
		if (cur[0] == '\n')
			cur++;

		if (1 == sscanf(cur, "verification time %ld usec\n", &s->stats[DURATION]))
			continue;
		if (5 == sscanf(cur, "processed %ld insns (limit %*d) max_states_per_insn %ld total_states %ld peak_states %ld mark_read %ld",
				&s->stats[TOTAL_INSNS],
				&s->stats[MAX_STATES_PER_INSN],
				&s->stats[TOTAL_STATES],
				&s->stats[PEAK_STATES],
				&s->stats[MARK_READ_MAX_LEN]))
			continue;

		if (1 == sscanf(cur, "stack depth %511s", stack))
			continue;
	}
	while ((token = strtok_r(cnt++ ? NULL : stack, "+", &state))) {
		if (sscanf(token, "%d", &sub_stack) == 0)
			break;
		s->stats[STACK] += sub_stack;
	}
	return 0;
}

struct line_cnt {
	char *line;
	int cnt;
};

static int str_cmp(const void *a, const void *b)
{
	const char **str1 = (const char **)a;
	const char **str2 = (const char **)b;

	return strcmp(*str1, *str2);
}

static int line_cnt_cmp(const void *a, const void *b)
{
	const struct line_cnt *a_cnt = (const struct line_cnt *)a;
	const struct line_cnt *b_cnt = (const struct line_cnt *)b;

	if (a_cnt->cnt != b_cnt->cnt)
		return a_cnt->cnt > b_cnt->cnt ? -1 : 1;
	return strcmp(a_cnt->line, b_cnt->line);
}

static int print_top_src_lines(char * const buf, size_t buf_sz, const char *prog_name)
{
	int lines_cap = 0;
	int lines_size = 0;
	char **lines = NULL;
	char *line = NULL;
	char *state;
	struct line_cnt *freq = NULL;
	struct line_cnt *cur;
	int unique_lines;
	int err = 0;
	int i;

	while ((line = strtok_r(line ? NULL : buf, "\n", &state))) {
		if (strncmp(line, "; ", 2) != 0)
			continue;
		line += 2;

		if (lines_size == lines_cap) {
			char **tmp;

			lines_cap = max(16, lines_cap * 2);
			tmp = realloc(lines, lines_cap * sizeof(*tmp));
			if (!tmp) {
				err = -ENOMEM;
				goto cleanup;
			}
			lines = tmp;
		}
		lines[lines_size] = line;
		lines_size++;
	}

	if (lines_size == 0)
		goto cleanup;

	qsort(lines, lines_size, sizeof(*lines), str_cmp);

	freq = calloc(lines_size, sizeof(*freq));
	if (!freq) {
		err = -ENOMEM;
		goto cleanup;
	}

	cur = freq;
	cur->line = lines[0];
	cur->cnt = 1;
	for (i = 1; i < lines_size; ++i) {
		if (strcmp(lines[i], cur->line) != 0) {
			cur++;
			cur->line = lines[i];
			cur->cnt = 0;
		}
		cur->cnt++;
	}
	unique_lines = cur - freq + 1;

	qsort(freq, unique_lines, sizeof(struct line_cnt), line_cnt_cmp);

	printf("Top source lines (%s):\n", prog_name);
	for (i = 0; i < min(unique_lines, env.top_src_lines); ++i) {
		const char *src_code = freq[i].line;
		const char *src_line = NULL;
		char *split = strrchr(freq[i].line, '@');

		if (split) {
			src_line = split + 1;

			while (*src_line && isspace(*src_line))
				src_line++;

			while (split > src_code && isspace(*split))
				split--;
			*split = '\0';
		}

		if (src_line)
			printf("%5d: (%s)\t%s\n", freq[i].cnt, src_line, src_code);
		else
			printf("%5d: %s\n", freq[i].cnt, src_code);
	}
	printf("\n");

cleanup:
	free(freq);
	free(lines);
	return err;
}

static int guess_prog_type_by_ctx_name(const char *ctx_name,
				       enum bpf_prog_type *prog_type,
				       enum bpf_attach_type *attach_type)
{
	/* We need to guess program type based on its declared context type.
	 * This guess can't be perfect as many different program types might
	 * share the same context type.  So we can only hope to reasonably
	 * well guess this and get lucky.
	 *
	 * Just in case, we support both UAPI-side type names and
	 * kernel-internal names.
	 */
	static struct {
		const char *uapi_name;
		const char *kern_name;
		enum bpf_prog_type prog_type;
		enum bpf_attach_type attach_type;
	} ctx_map[] = {
		/* __sk_buff is most ambiguous, we assume TC program */
		{ "__sk_buff", "sk_buff", BPF_PROG_TYPE_SCHED_CLS },
		{ "bpf_sock", "sock", BPF_PROG_TYPE_CGROUP_SOCK, BPF_CGROUP_INET4_POST_BIND },
		{ "bpf_sock_addr", "bpf_sock_addr_kern",  BPF_PROG_TYPE_CGROUP_SOCK_ADDR, BPF_CGROUP_INET4_BIND },
		{ "bpf_sock_ops", "bpf_sock_ops_kern", BPF_PROG_TYPE_SOCK_OPS, BPF_CGROUP_SOCK_OPS },
		{ "sk_msg_md", "sk_msg", BPF_PROG_TYPE_SK_MSG, BPF_SK_MSG_VERDICT },
		{ "bpf_cgroup_dev_ctx", "bpf_cgroup_dev_ctx", BPF_PROG_TYPE_CGROUP_DEVICE, BPF_CGROUP_DEVICE },
		{ "bpf_sysctl", "bpf_sysctl_kern", BPF_PROG_TYPE_CGROUP_SYSCTL, BPF_CGROUP_SYSCTL },
		{ "bpf_sockopt", "bpf_sockopt_kern", BPF_PROG_TYPE_CGROUP_SOCKOPT, BPF_CGROUP_SETSOCKOPT },
		{ "sk_reuseport_md", "sk_reuseport_kern", BPF_PROG_TYPE_SK_REUSEPORT, BPF_SK_REUSEPORT_SELECT_OR_MIGRATE },
		{ "bpf_sk_lookup", "bpf_sk_lookup_kern", BPF_PROG_TYPE_SK_LOOKUP, BPF_SK_LOOKUP },
		{ "xdp_md", "xdp_buff", BPF_PROG_TYPE_XDP, BPF_XDP },
		/* tracing types with no expected attach type */
		{ "bpf_user_pt_regs_t", "pt_regs", BPF_PROG_TYPE_KPROBE },
		{ "bpf_perf_event_data", "bpf_perf_event_data_kern", BPF_PROG_TYPE_PERF_EVENT },
		/* raw_tp programs use u64[] from kernel side, we don't want
		 * to match on that, probably; so NULL for kern-side type
		 */
		{ "bpf_raw_tracepoint_args", NULL, BPF_PROG_TYPE_RAW_TRACEPOINT },
	};
	int i;

	if (!ctx_name)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(ctx_map); i++) {
		if (strcmp(ctx_map[i].uapi_name, ctx_name) == 0 ||
		    (ctx_map[i].kern_name && strcmp(ctx_map[i].kern_name, ctx_name) == 0)) {
			*prog_type = ctx_map[i].prog_type;
			*attach_type = ctx_map[i].attach_type;
			return 0;
		}
	}

	return -ESRCH;
}

/* Make sure only target program is referenced from struct_ops map,
 * otherwise libbpf would automatically set autocreate for all
 * referenced programs.
 * See libbpf.c:bpf_object_adjust_struct_ops_autoload.
 */
static void mask_unrelated_struct_ops_progs(struct bpf_object *obj,
					    struct bpf_map *map,
					    struct bpf_program *prog)
{
	struct btf *btf = bpf_object__btf(obj);
	const struct btf_type *t, *mt;
	struct btf_member *m;
	int i, moff;
	size_t data_sz, ptr_sz = sizeof(void *);
	void *data;

	t = btf__type_by_id(btf, bpf_map__btf_value_type_id(map));
	if (!btf_is_struct(t))
		return;

	data = bpf_map__initial_value(map, &data_sz);
	for (i = 0; i < btf_vlen(t); i++) {
		m = &btf_members(t)[i];
		mt = btf__type_by_id(btf, m->type);
		if (!btf_is_ptr(mt))
			continue;
		moff = m->offset / 8;
		if (moff + ptr_sz > data_sz)
			continue;
		if (memcmp(data + moff, &prog, ptr_sz) == 0)
			continue;
		memset(data + moff, 0, ptr_sz);
	}
}

static void fixup_obj(struct bpf_object *obj, struct bpf_program *prog, const char *filename)
{
	struct bpf_map *map;

	bpf_object__for_each_map(map, obj) {
		/* disable pinning */
		bpf_map__set_pin_path(map, NULL);

		/* fix up map size, if necessary */
		switch (bpf_map__type(map)) {
		case BPF_MAP_TYPE_SK_STORAGE:
		case BPF_MAP_TYPE_TASK_STORAGE:
		case BPF_MAP_TYPE_INODE_STORAGE:
		case BPF_MAP_TYPE_CGROUP_STORAGE:
		case BPF_MAP_TYPE_CGRP_STORAGE:
			break;
		case BPF_MAP_TYPE_STRUCT_OPS:
			mask_unrelated_struct_ops_progs(obj, map, prog);
			break;
		default:
			if (bpf_map__max_entries(map) == 0)
				bpf_map__set_max_entries(map, 1);
		}
	}

	/* SEC(freplace) programs can't be loaded with veristat as is,
	 * but we can try guessing their target program's expected type by
	 * looking at the type of program's first argument and substituting
	 * corresponding program type
	 */
	if (bpf_program__type(prog) == BPF_PROG_TYPE_EXT) {
		const struct btf *btf = bpf_object__btf(obj);
		const char *prog_name = bpf_program__name(prog);
		enum bpf_prog_type prog_type;
		enum bpf_attach_type attach_type;
		const struct btf_type *t;
		const char *ctx_name;
		int id;

		if (!btf)
			goto skip_freplace_fixup;

		id = btf__find_by_name_kind(btf, prog_name, BTF_KIND_FUNC);
		t = btf__type_by_id(btf, id);
		t = btf__type_by_id(btf, t->type);
		if (!btf_is_func_proto(t) || btf_vlen(t) != 1)
			goto skip_freplace_fixup;

		/* context argument is a pointer to a struct/typedef */
		t = btf__type_by_id(btf, btf_params(t)[0].type);
		while (t && btf_is_mod(t))
			t = btf__type_by_id(btf, t->type);
		if (!t || !btf_is_ptr(t))
			goto skip_freplace_fixup;
		t = btf__type_by_id(btf, t->type);
		while (t && btf_is_mod(t))
			t = btf__type_by_id(btf, t->type);
		if (!t)
			goto skip_freplace_fixup;

		ctx_name = btf__name_by_offset(btf, t->name_off);

		if (guess_prog_type_by_ctx_name(ctx_name, &prog_type, &attach_type) == 0) {
			bpf_program__set_type(prog, prog_type);
			bpf_program__set_expected_attach_type(prog, attach_type);

			if (!env.quiet) {
				fprintf(stderr, "Using guessed program type '%s' for %s/%s...\n",
					libbpf_bpf_prog_type_str(prog_type),
					filename, prog_name);
			}
		} else {
			if (!env.quiet) {
				fprintf(stderr, "Failed to guess program type for freplace program with context type name '%s' for %s/%s. Consider using canonical type names to help veristat...\n",
					ctx_name, filename, prog_name);
			}
		}
	}
skip_freplace_fixup:
	return;
}

static int max_verifier_log_size(void)
{
	const int SMALL_LOG_SIZE = UINT_MAX >> 8;
	const int BIG_LOG_SIZE = UINT_MAX >> 2;
	struct bpf_insn insns[] = {
		{ .code = BPF_ALU | BPF_MOV | BPF_X, .dst_reg = BPF_REG_0, },
		{ .code  = BPF_JMP | BPF_EXIT, },
	};
	LIBBPF_OPTS(bpf_prog_load_opts, opts,
		    .log_size = BIG_LOG_SIZE,
		    .log_buf = (void *)-1,
		    .log_level = 4
	);
	int ret, insn_cnt = ARRAY_SIZE(insns);
	static int log_size;

	if (log_size != 0)
		return log_size;

	ret = bpf_prog_load(BPF_PROG_TYPE_TRACEPOINT, NULL, "GPL", insns, insn_cnt, &opts);

	if (ret == -EFAULT)
		log_size = BIG_LOG_SIZE;
	else /* ret == -EINVAL, big log size is not supported by the verifier */
		log_size = SMALL_LOG_SIZE;

	return log_size;
}

static bool output_stat_enabled(int id)
{
	int i;

	for (i = 0; i < env.output_spec.spec_cnt; i++)
		if (env.output_spec.ids[i] == id)
			return true;
	return false;
}

__attribute__((format(printf, 2, 3)))
static int write_one_line(const char *file, const char *fmt, ...)
{
	int err, saved_errno;
	va_list ap;
	FILE *f;

	f = fopen(file, "w");
	if (!f)
		return -1;

	va_start(ap, fmt);
	errno = 0;
	err = vfprintf(f, fmt, ap);
	saved_errno = errno;
	va_end(ap);
	fclose(f);
	errno = saved_errno;
	return err < 0 ? -1 : 0;
}

__attribute__((format(scanf, 3, 4)))
static int scanf_one_line(const char *file, int fields_expected, const char *fmt, ...)
{
	int res = 0, saved_errno = 0;
	char *line = NULL;
	size_t line_len;
	va_list ap;
	FILE *f;

	f = fopen(file, "r");
	if (!f)
		return -1;

	va_start(ap, fmt);
	while (getline(&line, &line_len, f) > 0) {
		res = vsscanf(line, fmt, ap);
		if (res == fields_expected)
			goto out;
	}
	if (ferror(f)) {
		saved_errno = errno;
		res = -1;
	}

out:
	va_end(ap);
	free(line);
	fclose(f);
	errno = saved_errno;
	return res;
}

static void destroy_stat_cgroup(void)
{
	char buf[PATH_MAX];
	int err;

	close(env.memory_peak_fd);

	if (env.orig_cgroup[0]) {
		snprintf(buf, sizeof(buf), "%s/cgroup.procs", env.orig_cgroup);
		err = write_one_line(buf, "%d\n", getpid());
		if (err < 0)
			log_errno("moving self to original cgroup %s\n", env.orig_cgroup);
	}

	if (env.stat_cgroup[0]) {
		err = rmdir(env.stat_cgroup);
		if (err < 0)
			log_errno("deletion of cgroup %s", env.stat_cgroup);
	}

	env.memory_peak_fd = -1;
	env.orig_cgroup[0] = 0;
	env.stat_cgroup[0] = 0;
}

/*
 * Creates a cgroup at /sys/fs/cgroup/veristat-accounting-<pid>,
 * moves current process to this cgroup.
 */
static void create_stat_cgroup(void)
{
	char cgroup_fs_mount[4096];
	char buf[4096];
	int err;

	env.memory_peak_fd = -1;

	if (!output_stat_enabled(MEMORY_PEAK))
		return;

	err = scanf_one_line("/proc/self/mounts", 2, "%*s %4095s cgroup2 %s",
			     cgroup_fs_mount, buf);
	if (err != 2) {
		if (err < 0)
			log_errno("reading /proc/self/mounts");
		else if (!env.quiet)
			fprintf(stderr, "Can't find cgroupfs v2 mount point.\n");
		goto err_out;
	}

	/* cgroup-v2.rst promises the line "0::<group>" for cgroups v2 */
	err = scanf_one_line("/proc/self/cgroup", 1, "0::%4095s", buf);
	if (err != 1) {
		if (err < 0)
			log_errno("reading /proc/self/cgroup");
		else if (!env.quiet)
			fprintf(stderr, "Can't infer veristat process cgroup.");
		goto err_out;
	}

	snprintf(env.orig_cgroup, sizeof(env.orig_cgroup), "%s/%s", cgroup_fs_mount, buf);

	snprintf(buf, sizeof(buf), "%s/veristat-accounting-%d", cgroup_fs_mount, getpid());
	err = mkdir(buf, 0777);
	if (err < 0) {
		log_errno("creation of cgroup %s", buf);
		goto err_out;
	}
	strcpy(env.stat_cgroup, buf);

	snprintf(buf, sizeof(buf), "%s/cgroup.procs", env.stat_cgroup);
	err = write_one_line(buf, "%d\n", getpid());
	if (err < 0) {
		log_errno("entering cgroup %s", buf);
		goto err_out;
	}

	snprintf(buf, sizeof(buf), "%s/memory.peak", env.stat_cgroup);
	env.memory_peak_fd = open(buf, O_RDWR | O_APPEND);
	if (env.memory_peak_fd < 0) {
		log_errno("opening %s", buf);
		goto err_out;
	}

	return;

err_out:
	if (!env.quiet)
		fprintf(stderr, "Memory usage metric unavailable.\n");
	destroy_stat_cgroup();
}

/* Current value of /sys/fs/cgroup/veristat-accounting-<pid>/memory.peak */
static long cgroup_memory_peak(void)
{
	long err, memory_peak;
	char buf[32];

	if (env.memory_peak_fd < 0)
		return -1;

	err = pread(env.memory_peak_fd, buf, sizeof(buf) - 1, 0);
	if (err <= 0) {
		log_errno("pread(%s/memory.peak)", env.stat_cgroup);
		return -1;
	}

	buf[err] = 0;
	errno = 0;
	memory_peak = strtoll(buf, NULL, 10);
	if (errno) {
		log_errno("%s/memory.peak:strtoll(%s)", env.stat_cgroup, buf);
		return -1;
	}

	return memory_peak;
}

static int reset_stat_cgroup(void)
{
	char buf[] = "r\n";
	int err;

	if (env.memory_peak_fd < 0)
		return -1;

	err = pwrite(env.memory_peak_fd, buf, sizeof(buf), 0);
	if (err <= 0) {
		log_errno("pwrite(%s/memory.peak)", env.stat_cgroup);
		return -1;
	}
	return 0;
}

static int parse_rvalue(const char *val, struct rvalue *rvalue)
{
	long long value;
	char *val_end;

	if (val[0] == '-' || isdigit(val[0])) {
		/* must be a number */
		errno = 0;
		value = strtoll(val, &val_end, 0);
		if (errno == ERANGE) {
			errno = 0;
			value = strtoull(val, &val_end, 0);
		}
		if (errno || *val_end != '\0') {
			fprintf(stderr, "Failed to parse value '%s'\n", val);
			return -EINVAL;
		}
		rvalue->ivalue = value;
		rvalue->type = INTEGRAL;
	} else {
		/* if not a number, consider it enum value */
		rvalue->svalue = strdup(val);
		if (!rvalue->svalue)
			return -ENOMEM;
		rvalue->type = ENUMERATOR;
	}
	return 0;
}

static int process_prog(const char *filename, struct bpf_object *obj, struct bpf_program *prog)
{
	const char *base_filename = basename(strdupa(filename));
	const char *prog_name = bpf_program__name(prog);
	long mem_peak_a, mem_peak_b, mem_peak = -1;
	char *buf;
	int buf_sz, log_level;
	struct verif_stats *stats;
	struct bpf_prog_info info;
	__u32 info_len = sizeof(info);
	int err = 0, cgroup_err;
	void *tmp;
	int fd;

	if (!should_process_file_prog(base_filename, bpf_program__name(prog))) {
		env.progs_skipped++;
		return 0;
	}

	tmp = realloc(env.prog_stats, (env.prog_stat_cnt + 1) * sizeof(*env.prog_stats));
	if (!tmp)
		return -ENOMEM;
	env.prog_stats = tmp;
	stats = &env.prog_stats[env.prog_stat_cnt++];
	memset(stats, 0, sizeof(*stats));

	if (env.verbose || env.top_src_lines > 0) {
		buf_sz = env.log_size ? env.log_size : max_verifier_log_size();
		buf = malloc(buf_sz);
		if (!buf)
			return -ENOMEM;
		/* ensure we always request stats */
		log_level = env.log_level | 4 | (env.log_fixed ? 8 : 0);
		/* --top-src-lines needs verifier log */
		if (env.top_src_lines > 0 && env.log_level == 0)
			log_level |= 2;
	} else {
		buf = verif_log_buf;
		buf_sz = sizeof(verif_log_buf);
		/* request only verifier stats */
		log_level = 4 | (env.log_fixed ? 8 : 0);
	}
	verif_log_buf[0] = '\0';

	bpf_program__set_log_buf(prog, buf, buf_sz);
	bpf_program__set_log_level(prog, log_level);

	/* increase chances of successful BPF object loading */
	fixup_obj(obj, prog, base_filename);

	if (env.force_checkpoints)
		bpf_program__set_flags(prog, bpf_program__flags(prog) | BPF_F_TEST_STATE_FREQ);
	if (env.force_reg_invariants)
		bpf_program__set_flags(prog, bpf_program__flags(prog) | BPF_F_TEST_REG_INVARIANTS);

	err = bpf_object__prepare(obj);
	if (!err) {
		cgroup_err = reset_stat_cgroup();
		mem_peak_a = cgroup_memory_peak();
		err = bpf_object__load(obj);
		mem_peak_b = cgroup_memory_peak();
		if (!cgroup_err && mem_peak_a >= 0 && mem_peak_b >= 0)
			mem_peak = mem_peak_b - mem_peak_a;
	}
	env.progs_processed++;

	stats->file_name = strdup(base_filename);
	stats->prog_name = strdup(bpf_program__name(prog));
	stats->stats[VERDICT] = err == 0; /* 1 - success, 0 - failure */
	stats->stats[SIZE] = bpf_program__insn_cnt(prog);
	stats->stats[PROG_TYPE] = bpf_program__type(prog);
	stats->stats[ATTACH_TYPE] = bpf_program__expected_attach_type(prog);
	stats->stats[MEMORY_PEAK] = mem_peak < 0 ? -1 : mem_peak / (1024 * 1024);

	memset(&info, 0, info_len);
	fd = bpf_program__fd(prog);
	if (fd > 0 && bpf_prog_get_info_by_fd(fd, &info, &info_len) == 0)
		stats->stats[JITED_SIZE] = info.jited_prog_len;

	parse_verif_log(buf, buf_sz, stats);

	if (env.verbose) {
		printf("PROCESSING %s/%s, DURATION US: %ld, VERDICT: %s, VERIFIER LOG:\n%s\n",
		       filename, prog_name, stats->stats[DURATION],
		       err ? "failure" : "success", buf);
	}
	if (env.top_src_lines > 0)
		print_top_src_lines(buf, buf_sz, stats->prog_name);

	if (verif_log_buf != buf)
		free(buf);

	return 0;
}

static int append_preset_atom(struct var_preset *preset, char *value, bool is_index)
{
	struct field_access *tmp;
	int i = preset->atom_count;
	int err;

	tmp = reallocarray(preset->atoms, i + 1, sizeof(*preset->atoms));
	if (!tmp)
		return -ENOMEM;

	preset->atoms = tmp;
	preset->atom_count++;

	if (is_index) {
		preset->atoms[i].type = ARRAY_INDEX;
		err = parse_rvalue(value, &preset->atoms[i].index);
		if (err)
			return err;
	} else {
		preset->atoms[i].type = FIELD_NAME;
		preset->atoms[i].name = strdup(value);
		if (!preset->atoms[i].name)
			return -ENOMEM;
	}
	return 0;
}

static int parse_var_atoms(const char *full_var, struct var_preset *preset)
{
	char expr[256], var[256], *name, *saveptr;
	int n, len, off, err;

	snprintf(expr, sizeof(expr), "%s", full_var);
	preset->atom_count = 0;
	while ((name = strtok_r(preset->atom_count ? NULL : expr, ".", &saveptr))) {
		len = strlen(name);
		/* parse variable name */
		if (sscanf(name, "%[a-zA-Z0-9_] %n", var, &off) != 1) {
			fprintf(stderr, "Can't parse %s\n", name);
			return -EINVAL;
		}
		err = append_preset_atom(preset, var, false);
		if (err)
			return err;

		/* parse optional array indexes */
		while (off < len) {
			if (sscanf(name + off, " [ %[a-zA-Z0-9_] ] %n", var, &n) != 1) {
				fprintf(stderr, "Can't parse %s as index\n", name + off);
				return -EINVAL;
			}
			err = append_preset_atom(preset, var, true);
			if (err)
				return err;
			off += n;
		}
	}
	return 0;
}

static int append_var_preset(struct var_preset **presets, int *cnt, const char *expr)
{
	void *tmp;
	struct var_preset *cur;
	char var[256], val[256];
	int n, err;

	tmp = realloc(*presets, (*cnt + 1) * sizeof(**presets));
	if (!tmp)
		return -ENOMEM;
	*presets = tmp;
	cur = &(*presets)[*cnt];
	memset(cur, 0, sizeof(*cur));
	(*cnt)++;

	if (sscanf(expr, " %[][a-zA-Z0-9_. ] = %s %n", var, val, &n) != 2 || n != strlen(expr)) {
		fprintf(stderr, "Failed to parse expression '%s'\n", expr);
		return -EINVAL;
	}
	/* Remove trailing spaces from var, as scanf may add those */
	rtrim(var);

	err = parse_rvalue(val, &cur->value);
	if (err)
		return err;

	cur->full_name = strdup(var);
	if (!cur->full_name)
		return -ENOMEM;

	err = parse_var_atoms(var, cur);
	if (err)
		return err;

	return 0;
}

static int append_var_preset_file(const char *filename)
{
	char buf[1024];
	FILE *f;
	int err = 0;

	f = fopen(filename, "rt");
	if (!f) {
		err = -errno;
		fprintf(stderr, "Failed to open presets in '%s': %s\n", filename, strerror(-err));
		return -EINVAL;
	}

	while (fscanf(f, " %1023[^\n]\n", buf) == 1) {
		if (buf[0] == '\0' || buf[0] == '#')
			continue;

		err = append_var_preset(&env.presets, &env.npresets, buf);
		if (err)
			goto cleanup;
	}

cleanup:
	fclose(f);
	return err;
}

static bool is_signed_type(const struct btf_type *t)
{
	if (btf_is_int(t))
		return btf_int_encoding(t) & BTF_INT_SIGNED;
	if (btf_is_any_enum(t))
		return btf_kflag(t);
	return true;
}

static int enum_value_from_name(const struct btf *btf, const struct btf_type *t,
				const char *evalue, long long *retval)
{
	if (btf_is_enum(t)) {
		struct btf_enum *e = btf_enum(t);
		int i, n = btf_vlen(t);

		for (i = 0; i < n; ++i, ++e) {
			const char *cur_name = btf__name_by_offset(btf, e->name_off);

			if (strcmp(cur_name, evalue) == 0) {
				*retval = e->val;
				return 0;
			}
		}
	} else if (btf_is_enum64(t)) {
		struct btf_enum64 *e = btf_enum64(t);
		int i, n = btf_vlen(t);

		for (i = 0; i < n; ++i, ++e) {
			const char *cur_name = btf__name_by_offset(btf, e->name_off);
			__u64 value =  btf_enum64_value(e);

			if (strcmp(cur_name, evalue) == 0) {
				*retval = value;
				return 0;
			}
		}
	}
	return -EINVAL;
}

static bool is_preset_supported(const struct btf_type *t)
{
	return btf_is_int(t) || btf_is_enum(t) || btf_is_enum64(t);
}

static int find_enum_value(const struct btf *btf, const char *name, long long *value)
{
	const struct btf_type *t;
	int cnt, i;
	long long lvalue;

	cnt = btf__type_cnt(btf);
	for (i = 1; i != cnt; ++i) {
		t = btf__type_by_id(btf, i);

		if (!btf_is_any_enum(t))
			continue;

		if (enum_value_from_name(btf, t, name, &lvalue) == 0) {
			*value = lvalue;
			return 0;
		}
	}
	return -ESRCH;
}

static int resolve_rvalue(struct btf *btf, const struct rvalue *rvalue, long long *result)
{
	int err = 0;

	switch (rvalue->type) {
	case INTEGRAL:
		*result = rvalue->ivalue;
		return 0;
	case ENUMERATOR:
		err = find_enum_value(btf, rvalue->svalue, result);
		if (err) {
			fprintf(stderr, "Can't resolve enum value %s\n", rvalue->svalue);
			return err;
		}
		return 0;
	default:
		fprintf(stderr, "Unknown rvalue type\n");
		return -EOPNOTSUPP;
	}
	return 0;
}

static int adjust_var_secinfo_array(struct btf *btf, int tid, struct field_access *atom,
				    const char *array_name, struct btf_var_secinfo *sinfo)
{
	const struct btf_type *t;
	struct btf_array *barr;
	long long idx;
	int err;

	tid = btf__resolve_type(btf, tid);
	t = btf__type_by_id(btf, tid);
	if (!btf_is_array(t)) {
		fprintf(stderr, "Array index is not expected for %s\n",
			array_name);
		return -EINVAL;
	}
	barr = btf_array(t);
	err = resolve_rvalue(btf, &atom->index, &idx);
	if (err)
		return err;
	if (idx < 0 || idx >= barr->nelems) {
		fprintf(stderr, "Array index %lld is out of bounds [0, %u): %s\n",
			idx, barr->nelems, array_name);
		return -EINVAL;
	}
	sinfo->size = btf__resolve_size(btf, barr->type);
	sinfo->offset += sinfo->size * idx;
	sinfo->type = btf__resolve_type(btf, barr->type);
	return 0;
}

static int adjust_var_secinfo_member(const struct btf *btf,
				     const struct btf_type *parent_type,
				     __u32 parent_offset,
				     const char *member_name,
				     struct btf_var_secinfo *sinfo)
{
	int i;

	if (!btf_is_composite(parent_type)) {
		fprintf(stderr, "Can't resolve field %s for non-composite type\n", member_name);
		return -EINVAL;
	}

	for (i = 0; i < btf_vlen(parent_type); ++i) {
		const struct btf_member *member;
		const struct btf_type *member_type;
		int tid, off;

		member = btf_members(parent_type) + i;
		tid =  btf__resolve_type(btf, member->type);
		if (tid < 0)
			return -EINVAL;

		member_type = btf__type_by_id(btf, tid);
		off = parent_offset + member->offset;
		if (member->name_off) {
			const char *name = btf__name_by_offset(btf, member->name_off);

			if (strcmp(member_name, name) == 0) {
				if (btf_member_bitfield_size(parent_type, i) != 0) {
					fprintf(stderr, "Bitfield presets are not supported %s\n",
						name);
					return -EINVAL;
				}
				sinfo->offset += off / 8;
				sinfo->type = tid;
				sinfo->size = member_type->size;
				return 0;
			}
		} else if (btf_is_composite(member_type)) {
			int err;

			err = adjust_var_secinfo_member(btf, member_type, off,
							member_name, sinfo);
			if (!err)
				return 0;
		}
	}

	return -ESRCH;
}

static int adjust_var_secinfo(struct btf *btf, const struct btf_type *t,
			      struct btf_var_secinfo *sinfo, struct var_preset *preset)
{
	const struct btf_type *base_type;
	const char *prev_name;
	int err, i;
	int tid;

	assert(preset->atom_count > 0);
	assert(preset->atoms[0].type == FIELD_NAME);

	tid = btf__resolve_type(btf, t->type);
	base_type = btf__type_by_id(btf, tid);
	prev_name = preset->atoms[0].name;

	for (i = 1; i < preset->atom_count; ++i) {
		struct field_access *atom = preset->atoms + i;

		switch (atom->type) {
		case ARRAY_INDEX:
			err = adjust_var_secinfo_array(btf, tid, atom, prev_name, sinfo);
			break;
		case FIELD_NAME:
			err = adjust_var_secinfo_member(btf, base_type, 0, atom->name, sinfo);
			if (err == -ESRCH)
				fprintf(stderr, "Can't find '%s'\n", atom->name);
			prev_name = atom->name;
			break;
		default:
			fprintf(stderr, "Unknown field_access type\n");
			return -EOPNOTSUPP;
		}
		if (err)
			return err;
		base_type = btf__type_by_id(btf, sinfo->type);
		tid = sinfo->type;
	}

	return 0;
}

static int set_global_var(struct bpf_object *obj, struct btf *btf,
			  struct bpf_map *map, struct btf_var_secinfo *sinfo,
			  struct var_preset *preset)
{
	const struct btf_type *base_type;
	void *ptr;
	long long value = preset->value.ivalue;
	size_t size;

	base_type = btf__type_by_id(btf, btf__resolve_type(btf, sinfo->type));
	if (!base_type) {
		fprintf(stderr, "Failed to resolve type %d\n", sinfo->type);
		return -EINVAL;
	}
	if (!is_preset_supported(base_type)) {
		fprintf(stderr, "Can't set %s. Only ints and enums are supported\n",
			preset->full_name);
		return -EINVAL;
	}

	if (preset->value.type == ENUMERATOR) {
		if (btf_is_any_enum(base_type)) {
			if (enum_value_from_name(btf, base_type, preset->value.svalue, &value)) {
				fprintf(stderr,
					"Failed to find integer value for enum element %s\n",
					preset->value.svalue);
				return -EINVAL;
			}
		} else {
			fprintf(stderr, "Value %s is not supported for type %s\n",
				preset->value.svalue,
				btf__name_by_offset(btf, base_type->name_off));
			return -EINVAL;
		}
	}

	/* Check if value fits into the target variable size */
	if  (sinfo->size < sizeof(value)) {
		bool is_signed = is_signed_type(base_type);
		__u32 unsigned_bits = sinfo->size * 8 - (is_signed ? 1 : 0);
		long long max_val = 1ll << unsigned_bits;

		if (value >= max_val || value < -max_val) {
			fprintf(stderr,
				"Variable %s value %lld is out of range [%lld; %lld]\n",
				btf__name_by_offset(btf, base_type->name_off), value,
				is_signed ? -max_val : 0, max_val - 1);
			return -EINVAL;
		}
	}

	ptr = bpf_map__initial_value(map, &size);
	if (!ptr || sinfo->offset + sinfo->size > size)
		return -EINVAL;

	if (__BYTE_ORDER == __LITTLE_ENDIAN) {
		memcpy(ptr + sinfo->offset, &value, sinfo->size);
	} else { /* __BYTE_ORDER == __BIG_ENDIAN */
		__u8 src_offset = sizeof(value) - sinfo->size;

		memcpy(ptr + sinfo->offset, (void *)&value + src_offset, sinfo->size);
	}
	return 0;
}

static int set_global_vars(struct bpf_object *obj, struct var_preset *presets, int npresets)
{
	struct btf_var_secinfo *sinfo;
	const char *sec_name;
	const struct btf_type *t;
	struct bpf_map *map;
	struct btf *btf;
	int i, j, k, n, cnt, err = 0;

	if (npresets == 0)
		return 0;

	btf = bpf_object__btf(obj);
	if (!btf)
		return -EINVAL;

	cnt = btf__type_cnt(btf);
	for (i = 1; i != cnt; ++i) {
		t = btf__type_by_id(btf, i);

		if (!btf_is_datasec(t))
			continue;

		sinfo = btf_var_secinfos(t);
		sec_name = btf__name_by_offset(btf, t->name_off);
		map = bpf_object__find_map_by_name(obj, sec_name);
		if (!map)
			continue;

		n = btf_vlen(t);
		for (j = 0; j < n; ++j, ++sinfo) {
			const struct btf_type *var_type = btf__type_by_id(btf, sinfo->type);
			const char *var_name;

			if (!btf_is_var(var_type))
				continue;

			var_name = btf__name_by_offset(btf, var_type->name_off);

			for (k = 0; k < npresets; ++k) {
				struct btf_var_secinfo tmp_sinfo;

				if (strcmp(var_name, presets[k].atoms[0].name) != 0)
					continue;

				if (presets[k].applied) {
					fprintf(stderr, "Variable %s is set more than once",
						var_name);
					return -EINVAL;
				}
				tmp_sinfo = *sinfo;
				err = adjust_var_secinfo(btf, var_type,
							 &tmp_sinfo, presets + k);
				if (err)
					return err;

				err = set_global_var(obj, btf, map, &tmp_sinfo, presets + k);
				if (err)
					return err;

				presets[k].applied = true;
			}
		}
	}
	for (i = 0; i < npresets; ++i) {
		if (!presets[i].applied) {
			fprintf(stderr, "Global variable preset %s has not been applied\n",
				presets[i].full_name);
			err = -EINVAL;
		}
		presets[i].applied = false;
	}
	return err;
}

static int process_obj(const char *filename)
{
	const char *base_filename = basename(strdupa(filename));
	struct bpf_object *obj = NULL, *tobj;
	struct bpf_program *prog, *tprog, *lprog;
	libbpf_print_fn_t old_libbpf_print_fn;
	LIBBPF_OPTS(bpf_object_open_opts, opts);
	int err = 0, prog_cnt = 0;

	if (!should_process_file_prog(base_filename, NULL)) {
		if (env.verbose)
			printf("Skipping '%s' due to filters...\n", filename);
		env.files_skipped++;
		return 0;
	}
	if (!is_bpf_obj_file(filename)) {
		if (env.verbose)
			printf("Skipping '%s' as it's not a BPF object file...\n", filename);
		env.files_skipped++;
		return 0;
	}

	if (!env.quiet && env.out_fmt == RESFMT_TABLE)
		printf("Processing '%s'...\n", base_filename);

	old_libbpf_print_fn = libbpf_set_print(libbpf_print_fn);
	obj = bpf_object__open_file(filename, &opts);
	if (!obj) {
		/* if libbpf can't open BPF object file, it could be because
		 * that BPF object file is incomplete and has to be statically
		 * linked into a final BPF object file; instead of bailing
		 * out, report it into stderr, mark it as skipped, and
		 * proceed
		 */
		fprintf(stderr, "Failed to open '%s': %d\n", filename, -errno);
		env.files_skipped++;
		err = 0;
		goto cleanup;
	}

	env.files_processed++;

	bpf_object__for_each_program(prog, obj) {
		prog_cnt++;
	}

	if (prog_cnt == 1) {
		prog = bpf_object__next_program(obj, NULL);
		bpf_program__set_autoload(prog, true);
		err = set_global_vars(obj, env.presets, env.npresets);
		if (err) {
			fprintf(stderr, "Failed to set global variables %d\n", err);
			goto cleanup;
		}
		process_prog(filename, obj, prog);
		goto cleanup;
	}

	bpf_object__for_each_program(prog, obj) {
		const char *prog_name = bpf_program__name(prog);

		tobj = bpf_object__open_file(filename, &opts);
		if (!tobj) {
			err = -errno;
			fprintf(stderr, "Failed to open '%s': %d\n", filename, err);
			goto cleanup;
		}

		err = set_global_vars(tobj, env.presets, env.npresets);
		if (err) {
			fprintf(stderr, "Failed to set global variables %d\n", err);
			goto cleanup;
		}

		lprog = NULL;
		bpf_object__for_each_program(tprog, tobj) {
			const char *tprog_name = bpf_program__name(tprog);

			if (strcmp(prog_name, tprog_name) == 0) {
				bpf_program__set_autoload(tprog, true);
				lprog = tprog;
			} else {
				bpf_program__set_autoload(tprog, false);
			}
		}

		process_prog(filename, tobj, lprog);
		bpf_object__close(tobj);
	}

cleanup:
	bpf_object__close(obj);
	libbpf_set_print(old_libbpf_print_fn);
	return err;
}

static int cmp_stat(const struct verif_stats *s1, const struct verif_stats *s2,
		    enum stat_id id, bool asc, bool abs)
{
	int cmp = 0;

	switch (id) {
	case FILE_NAME:
		cmp = strcmp(s1->file_name, s2->file_name);
		break;
	case PROG_NAME:
		cmp = strcmp(s1->prog_name, s2->prog_name);
		break;
	case ATTACH_TYPE:
	case PROG_TYPE:
	case SIZE:
	case JITED_SIZE:
	case STACK:
	case VERDICT:
	case DURATION:
	case TOTAL_INSNS:
	case TOTAL_STATES:
	case PEAK_STATES:
	case MAX_STATES_PER_INSN:
	case MEMORY_PEAK:
	case MARK_READ_MAX_LEN: {
		long v1 = s1->stats[id];
		long v2 = s2->stats[id];

		if (abs) {
			v1 = v1 < 0 ? -v1 : v1;
			v2 = v2 < 0 ? -v2 : v2;
		}

		if (v1 != v2)
			cmp = v1 < v2 ? -1 : 1;
		break;
	}
	default:
		fprintf(stderr, "Unrecognized stat #%d\n", id);
		exit(1);
	}

	return asc ? cmp : -cmp;
}

static int cmp_prog_stats(const void *v1, const void *v2)
{
	const struct verif_stats *s1 = v1, *s2 = v2;
	int i, cmp;

	for (i = 0; i < env.sort_spec.spec_cnt; i++) {
		cmp = cmp_stat(s1, s2, env.sort_spec.ids[i],
			       env.sort_spec.asc[i], env.sort_spec.abs[i]);
		if (cmp != 0)
			return cmp;
	}

	/* always disambiguate with file+prog, which are unique */
	cmp = strcmp(s1->file_name, s2->file_name);
	if (cmp != 0)
		return cmp;
	return strcmp(s1->prog_name, s2->prog_name);
}

static void fetch_join_stat_value(const struct verif_stats_join *s,
				  enum stat_id id, enum stat_variant var,
				  const char **str_val,
				  double *num_val)
{
	long v1, v2;

	if (id == FILE_NAME) {
		*str_val = s->file_name;
		return;
	}
	if (id == PROG_NAME) {
		*str_val = s->prog_name;
		return;
	}

	v1 = s->stats_a ? s->stats_a->stats[id] : 0;
	v2 = s->stats_b ? s->stats_b->stats[id] : 0;

	switch (var) {
	case VARIANT_A:
		if (!s->stats_a)
			*num_val = -DBL_MAX;
		else
			*num_val = s->stats_a->stats[id];
		return;
	case VARIANT_B:
		if (!s->stats_b)
			*num_val = -DBL_MAX;
		else
			*num_val = s->stats_b->stats[id];
		return;
	case VARIANT_DIFF:
		if (!s->stats_a || !s->stats_b)
			*num_val = -DBL_MAX;
		else if (id == VERDICT)
			*num_val = v1 == v2 ? 1.0 /* MATCH */ : 0.0 /* MISMATCH */;
		else
			*num_val = (double)(v2 - v1);
		return;
	case VARIANT_PCT:
		if (!s->stats_a || !s->stats_b) {
			*num_val = -DBL_MAX;
		} else if (v1 == 0) {
			if (v1 == v2)
				*num_val = 0.0;
			else
				*num_val = v2 < v1 ? -100.0 : 100.0;
		} else {
			 *num_val = (v2 - v1) * 100.0 / v1;
		}
		return;
	}
}

static int cmp_join_stat(const struct verif_stats_join *s1,
			 const struct verif_stats_join *s2,
			 enum stat_id id, enum stat_variant var,
			 bool asc, bool abs)
{
	const char *str1 = NULL, *str2 = NULL;
	double v1 = 0.0, v2 = 0.0;
	int cmp = 0;

	fetch_join_stat_value(s1, id, var, &str1, &v1);
	fetch_join_stat_value(s2, id, var, &str2, &v2);

	if (abs) {
		v1 = fabs(v1);
		v2 = fabs(v2);
	}

	if (str1)
		cmp = strcmp(str1, str2);
	else if (v1 != v2)
		cmp = v1 < v2 ? -1 : 1;

	return asc ? cmp : -cmp;
}

static int cmp_join_stats(const void *v1, const void *v2)
{
	const struct verif_stats_join *s1 = v1, *s2 = v2;
	int i, cmp;

	for (i = 0; i < env.sort_spec.spec_cnt; i++) {
		cmp = cmp_join_stat(s1, s2,
				    env.sort_spec.ids[i],
				    env.sort_spec.variants[i],
				    env.sort_spec.asc[i],
				    env.sort_spec.abs[i]);
		if (cmp != 0)
			return cmp;
	}

	/* always disambiguate with file+prog, which are unique */
	cmp = strcmp(s1->file_name, s2->file_name);
	if (cmp != 0)
		return cmp;
	return strcmp(s1->prog_name, s2->prog_name);
}

#define HEADER_CHAR '-'
#define COLUMN_SEP "  "

static void output_header_underlines(void)
{
	int i, j, len;

	for (i = 0; i < env.output_spec.spec_cnt; i++) {
		len = env.output_spec.lens[i];

		printf("%s", i == 0 ? "" : COLUMN_SEP);
		for (j = 0; j < len; j++)
			printf("%c", HEADER_CHAR);
	}
	printf("\n");
}

static void output_headers(enum resfmt fmt)
{
	const char *fmt_str;
	int i, len;

	for (i = 0; i < env.output_spec.spec_cnt; i++) {
		int id = env.output_spec.ids[i];
		int *max_len = &env.output_spec.lens[i];

		switch (fmt) {
		case RESFMT_TABLE_CALCLEN:
			len = snprintf(NULL, 0, "%s", stat_defs[id].header);
			if (len > *max_len)
				*max_len = len;
			break;
		case RESFMT_TABLE:
			fmt_str = stat_defs[id].left_aligned ? "%s%-*s" : "%s%*s";
			printf(fmt_str, i == 0 ? "" : COLUMN_SEP,  *max_len, stat_defs[id].header);
			if (i == env.output_spec.spec_cnt - 1)
				printf("\n");
			break;
		case RESFMT_CSV:
			printf("%s%s", i == 0 ? "" : ",", stat_defs[id].names[0]);
			if (i == env.output_spec.spec_cnt - 1)
				printf("\n");
			break;
		}
	}

	if (fmt == RESFMT_TABLE)
		output_header_underlines();
}

static void prepare_value(const struct verif_stats *s, enum stat_id id,
			  const char **str, long *val)
{
	switch (id) {
	case FILE_NAME:
		*str = s ? s->file_name : "N/A";
		break;
	case PROG_NAME:
		*str = s ? s->prog_name : "N/A";
		break;
	case VERDICT:
		if (!s)
			*str = "N/A";
		else
			*str = s->stats[VERDICT] ? "success" : "failure";
		break;
	case ATTACH_TYPE:
		if (!s)
			*str = "N/A";
		else
			*str = libbpf_bpf_attach_type_str(s->stats[ATTACH_TYPE]) ?: "N/A";
		break;
	case PROG_TYPE:
		if (!s)
			*str = "N/A";
		else
			*str = libbpf_bpf_prog_type_str(s->stats[PROG_TYPE]) ?: "N/A";
		break;
	case DURATION:
	case TOTAL_INSNS:
	case TOTAL_STATES:
	case PEAK_STATES:
	case MAX_STATES_PER_INSN:
	case MARK_READ_MAX_LEN:
	case STACK:
	case SIZE:
	case JITED_SIZE:
	case MEMORY_PEAK:
		*val = s ? s->stats[id] : 0;
		break;
	default:
		fprintf(stderr, "Unrecognized stat #%d\n", id);
		exit(1);
	}
}

static void output_stats(const struct verif_stats *s, enum resfmt fmt, bool last)
{
	int i;

	for (i = 0; i < env.output_spec.spec_cnt; i++) {
		int id = env.output_spec.ids[i];
		int *max_len = &env.output_spec.lens[i], len;
		const char *str = NULL;
		long val = 0;

		prepare_value(s, id, &str, &val);

		switch (fmt) {
		case RESFMT_TABLE_CALCLEN:
			if (str)
				len = snprintf(NULL, 0, "%s", str);
			else
				len = snprintf(NULL, 0, "%ld", val);
			if (len > *max_len)
				*max_len = len;
			break;
		case RESFMT_TABLE:
			if (str)
				printf("%s%-*s", i == 0 ? "" : COLUMN_SEP, *max_len, str);
			else
				printf("%s%*ld", i == 0 ? "" : COLUMN_SEP,  *max_len, val);
			if (i == env.output_spec.spec_cnt - 1)
				printf("\n");
			break;
		case RESFMT_CSV:
			if (str)
				printf("%s%s", i == 0 ? "" : ",", str);
			else
				printf("%s%ld", i == 0 ? "" : ",", val);
			if (i == env.output_spec.spec_cnt - 1)
				printf("\n");
			break;
		}
	}

	if (last && fmt == RESFMT_TABLE) {
		output_header_underlines();
		printf("Done. Processed %d files, %d programs. Skipped %d files, %d programs.\n",
		       env.files_processed, env.files_skipped, env.progs_processed, env.progs_skipped);
	}
}

static int parse_stat_value(const char *str, enum stat_id id, struct verif_stats *st)
{
	switch (id) {
	case FILE_NAME:
		st->file_name = strdup(str);
		if (!st->file_name)
			return -ENOMEM;
		break;
	case PROG_NAME:
		st->prog_name = strdup(str);
		if (!st->prog_name)
			return -ENOMEM;
		break;
	case VERDICT:
		if (strcmp(str, "success") == 0) {
			st->stats[VERDICT] = true;
		} else if (strcmp(str, "failure") == 0) {
			st->stats[VERDICT] = false;
		} else {
			fprintf(stderr, "Unrecognized verification verdict '%s'\n", str);
			return -EINVAL;
		}
		break;
	case DURATION:
	case TOTAL_INSNS:
	case TOTAL_STATES:
	case PEAK_STATES:
	case MAX_STATES_PER_INSN:
	case MARK_READ_MAX_LEN:
	case SIZE:
	case JITED_SIZE:
	case MEMORY_PEAK:
	case STACK: {
		long val;
		int err, n;

		if (sscanf(str, "%ld %n", &val, &n) != 1 || n != strlen(str)) {
			err = -errno;
			fprintf(stderr, "Failed to parse '%s' as integer\n", str);
			return err;
		}

		st->stats[id] = val;
		break;
	}
	case PROG_TYPE: {
		enum bpf_prog_type prog_type = 0;
		const char *type;

		while ((type = libbpf_bpf_prog_type_str(prog_type)))  {
			if (strcmp(type, str) == 0) {
				st->stats[id] = prog_type;
				break;
			}
			prog_type++;
		}

		if (!type) {
			fprintf(stderr, "Unrecognized prog type %s\n", str);
			return -EINVAL;
		}
		break;
	}
	case ATTACH_TYPE: {
		enum bpf_attach_type attach_type = 0;
		const char *type;

		while ((type = libbpf_bpf_attach_type_str(attach_type)))  {
			if (strcmp(type, str) == 0) {
				st->stats[id] = attach_type;
				break;
			}
			attach_type++;
		}

		if (!type) {
			fprintf(stderr, "Unrecognized attach type %s\n", str);
			return -EINVAL;
		}
		break;
	}
	default:
		fprintf(stderr, "Unrecognized stat #%d\n", id);
		return -EINVAL;
	}
	return 0;
}

static int parse_stats_csv(const char *filename, struct stat_specs *specs,
			   struct verif_stats **statsp, int *stat_cntp)
{
	char line[4096];
	FILE *f;
	int err = 0;
	bool header = true;

	f = fopen(filename, "r");
	if (!f) {
		err = -errno;
		fprintf(stderr, "Failed to open '%s': %d\n", filename, err);
		return err;
	}

	*stat_cntp = 0;

	while (fgets(line, sizeof(line), f)) {
		char *input = line, *state = NULL, *next;
		struct verif_stats *st = NULL;
		int col = 0, cnt = 0;

		if (!header) {
			void *tmp;

			tmp = realloc(*statsp, (*stat_cntp + 1) * sizeof(**statsp));
			if (!tmp) {
				err = -ENOMEM;
				goto cleanup;
			}
			*statsp = tmp;

			st = &(*statsp)[*stat_cntp];
			memset(st, 0, sizeof(*st));

			*stat_cntp += 1;
		}

		while ((next = strtok_r(cnt++ ? NULL : input, ",\n", &state))) {
			if (header) {
				/* for the first line, set up spec stats */
				err = parse_stat(next, specs);
				if (err)
					goto cleanup;
				continue;
			}

			/* for all other lines, parse values based on spec */
			if (col >= specs->spec_cnt) {
				fprintf(stderr, "Found extraneous column #%d in row #%d of '%s'\n",
					col, *stat_cntp, filename);
				err = -EINVAL;
				goto cleanup;
			}
			err = parse_stat_value(next, specs->ids[col], st);
			if (err)
				goto cleanup;
			col++;
		}

		if (header) {
			header = false;
			continue;
		}

		if (col < specs->spec_cnt) {
			fprintf(stderr, "Not enough columns in row #%d in '%s'\n",
				*stat_cntp, filename);
			err = -EINVAL;
			goto cleanup;
		}

		if (!st->file_name || !st->prog_name) {
			fprintf(stderr, "Row #%d in '%s' is missing file and/or program name\n",
				*stat_cntp, filename);
			err = -EINVAL;
			goto cleanup;
		}

		/* in comparison mode we can only check filters after we
		 * parsed entire line; if row should be ignored we pretend we
		 * never parsed it
		 */
		if (!should_process_file_prog(st->file_name, st->prog_name)) {
			free(st->file_name);
			free(st->prog_name);
			*stat_cntp -= 1;
		}
	}

	if (!feof(f)) {
		err = -errno;
		fprintf(stderr, "Failed I/O for '%s': %d\n", filename, err);
	}

cleanup:
	fclose(f);
	return err;
}

/* empty/zero stats for mismatched rows */
static const struct verif_stats fallback_stats = { .file_name = "", .prog_name = "" };

static bool is_key_stat(enum stat_id id)
{
	return id == FILE_NAME || id == PROG_NAME;
}

static void output_comp_header_underlines(void)
{
	int i, j, k;

	for (i = 0; i < env.output_spec.spec_cnt; i++) {
		int id = env.output_spec.ids[i];
		int max_j = is_key_stat(id) ? 1 : 3;

		for (j = 0; j < max_j; j++) {
			int len = env.output_spec.lens[3 * i + j];

			printf("%s", i + j == 0 ? "" : COLUMN_SEP);

			for (k = 0; k < len; k++)
				printf("%c", HEADER_CHAR);
		}
	}
	printf("\n");
}

static void output_comp_headers(enum resfmt fmt)
{
	static const char *table_sfxs[3] = {" (A)", " (B)", " (DIFF)"};
	static const char *name_sfxs[3] = {"_base", "_comp", "_diff"};
	int i, j, len;

	for (i = 0; i < env.output_spec.spec_cnt; i++) {
		int id = env.output_spec.ids[i];
		/* key stats don't have A/B/DIFF columns, they are common for both data sets */
		int max_j = is_key_stat(id) ? 1 : 3;

		for (j = 0; j < max_j; j++) {
			int *max_len = &env.output_spec.lens[3 * i + j];
			bool last = (i == env.output_spec.spec_cnt - 1) && (j == max_j - 1);
			const char *sfx;

			switch (fmt) {
			case RESFMT_TABLE_CALCLEN:
				sfx = is_key_stat(id) ? "" : table_sfxs[j];
				len = snprintf(NULL, 0, "%s%s", stat_defs[id].header, sfx);
				if (len > *max_len)
					*max_len = len;
				break;
			case RESFMT_TABLE:
				sfx = is_key_stat(id) ? "" : table_sfxs[j];
				printf("%s%-*s%s", i + j == 0 ? "" : COLUMN_SEP,
				       *max_len - (int)strlen(sfx), stat_defs[id].header, sfx);
				if (last)
					printf("\n");
				break;
			case RESFMT_CSV:
				sfx = is_key_stat(id) ? "" : name_sfxs[j];
				printf("%s%s%s", i + j == 0 ? "" : ",", stat_defs[id].names[0], sfx);
				if (last)
					printf("\n");
				break;
			}
		}
	}

	if (fmt == RESFMT_TABLE)
		output_comp_header_underlines();
}

static void output_comp_stats(const struct verif_stats_join *join_stats,
			      enum resfmt fmt, bool last)
{
	const struct verif_stats *base = join_stats->stats_a;
	const struct verif_stats *comp = join_stats->stats_b;
	char base_buf[1024] = {}, comp_buf[1024] = {}, diff_buf[1024] = {};
	int i;

	for (i = 0; i < env.output_spec.spec_cnt; i++) {
		int id = env.output_spec.ids[i], len;
		int *max_len_base = &env.output_spec.lens[3 * i + 0];
		int *max_len_comp = &env.output_spec.lens[3 * i + 1];
		int *max_len_diff = &env.output_spec.lens[3 * i + 2];
		const char *base_str = NULL, *comp_str = NULL;
		long base_val = 0, comp_val = 0, diff_val = 0;

		prepare_value(base, id, &base_str, &base_val);
		prepare_value(comp, id, &comp_str, &comp_val);

		/* normalize all the outputs to be in string buffers for simplicity */
		if (is_key_stat(id)) {
			/* key stats (file and program name) are always strings */
			if (base)
				snprintf(base_buf, sizeof(base_buf), "%s", base_str);
			else
				snprintf(base_buf, sizeof(base_buf), "%s", comp_str);
		} else if (base_str) {
			snprintf(base_buf, sizeof(base_buf), "%s", base_str);
			snprintf(comp_buf, sizeof(comp_buf), "%s", comp_str);
			if (!base || !comp)
				snprintf(diff_buf, sizeof(diff_buf), "%s", "N/A");
			else if (strcmp(base_str, comp_str) == 0)
				snprintf(diff_buf, sizeof(diff_buf), "%s", "MATCH");
			else
				snprintf(diff_buf, sizeof(diff_buf), "%s", "MISMATCH");
		} else {
			double p = 0.0;

			if (base)
				snprintf(base_buf, sizeof(base_buf), "%ld", base_val);
			else
				snprintf(base_buf, sizeof(base_buf), "%s", "N/A");
			if (comp)
				snprintf(comp_buf, sizeof(comp_buf), "%ld", comp_val);
			else
				snprintf(comp_buf, sizeof(comp_buf), "%s", "N/A");

			diff_val = comp_val - base_val;
			if (!base || !comp) {
				snprintf(diff_buf, sizeof(diff_buf), "%s", "N/A");
			} else {
				if (base_val == 0) {
					if (comp_val == base_val)
						p = 0.0; /* avoid +0 (+100%) case */
					else
						p = comp_val < base_val ? -100.0 : 100.0;
				} else {
					 p = diff_val * 100.0 / base_val;
				}
				snprintf(diff_buf, sizeof(diff_buf), "%+ld (%+.2lf%%)", diff_val, p);
			}
		}

		switch (fmt) {
		case RESFMT_TABLE_CALCLEN:
			len = strlen(base_buf);
			if (len > *max_len_base)
				*max_len_base = len;
			if (!is_key_stat(id)) {
				len = strlen(comp_buf);
				if (len > *max_len_comp)
					*max_len_comp = len;
				len = strlen(diff_buf);
				if (len > *max_len_diff)
					*max_len_diff = len;
			}
			break;
		case RESFMT_TABLE: {
			/* string outputs are left-aligned, number outputs are right-aligned */
			const char *fmt = base_str ? "%s%-*s" : "%s%*s";

			printf(fmt, i == 0 ? "" : COLUMN_SEP, *max_len_base, base_buf);
			if (!is_key_stat(id)) {
				printf(fmt, COLUMN_SEP, *max_len_comp, comp_buf);
				printf(fmt, COLUMN_SEP, *max_len_diff, diff_buf);
			}
			if (i == env.output_spec.spec_cnt - 1)
				printf("\n");
			break;
		}
		case RESFMT_CSV:
			printf("%s%s", i == 0 ? "" : ",", base_buf);
			if (!is_key_stat(id)) {
				printf("%s%s", i == 0 ? "" : ",", comp_buf);
				printf("%s%s", i == 0 ? "" : ",", diff_buf);
			}
			if (i == env.output_spec.spec_cnt - 1)
				printf("\n");
			break;
		}
	}

	if (last && fmt == RESFMT_TABLE)
		output_comp_header_underlines();
}

static int cmp_stats_key(const struct verif_stats *base, const struct verif_stats *comp)
{
	int r;

	r = strcmp(base->file_name, comp->file_name);
	if (r != 0)
		return r;
	return strcmp(base->prog_name, comp->prog_name);
}

static bool is_join_stat_filter_matched(struct filter *f, const struct verif_stats_join *stats)
{
	static const double eps = 1e-9;
	const char *str = NULL;
	double value = 0.0;

	fetch_join_stat_value(stats, f->stat_id, f->stat_var, &str, &value);

	if (f->abs)
		value = fabs(value);

	switch (f->op) {
	case OP_EQ: return value > f->value - eps && value < f->value + eps;
	case OP_NEQ: return value < f->value - eps || value > f->value + eps;
	case OP_LT: return value < f->value - eps;
	case OP_LE: return value <= f->value + eps;
	case OP_GT: return value > f->value + eps;
	case OP_GE: return value >= f->value - eps;
	}

	fprintf(stderr, "BUG: unknown filter op %d!\n", f->op);
	return false;
}

static bool should_output_join_stats(const struct verif_stats_join *stats)
{
	struct filter *f;
	int i, allow_cnt = 0;

	for (i = 0; i < env.deny_filter_cnt; i++) {
		f = &env.deny_filters[i];
		if (f->kind != FILTER_STAT)
			continue;

		if (is_join_stat_filter_matched(f, stats))
			return false;
	}

	for (i = 0; i < env.allow_filter_cnt; i++) {
		f = &env.allow_filters[i];
		if (f->kind != FILTER_STAT)
			continue;
		allow_cnt++;

		if (is_join_stat_filter_matched(f, stats))
			return true;
	}

	/* if there are no stat allowed filters, pass everything through */
	return allow_cnt == 0;
}

static int handle_comparison_mode(void)
{
	struct stat_specs base_specs = {}, comp_specs = {};
	struct stat_specs tmp_sort_spec;
	enum resfmt cur_fmt;
	int err, i, j, last_idx, cnt;

	if (env.filename_cnt != 2) {
		fprintf(stderr, "Comparison mode expects exactly two input CSV files!\n\n");
		argp_help(&argp, stderr, ARGP_HELP_USAGE, "veristat");
		return -EINVAL;
	}

	err = parse_stats_csv(env.filenames[0], &base_specs,
			      &env.baseline_stats, &env.baseline_stat_cnt);
	if (err) {
		fprintf(stderr, "Failed to parse stats from '%s': %d\n", env.filenames[0], err);
		return err;
	}
	err = parse_stats_csv(env.filenames[1], &comp_specs,
			      &env.prog_stats, &env.prog_stat_cnt);
	if (err) {
		fprintf(stderr, "Failed to parse stats from '%s': %d\n", env.filenames[1], err);
		return err;
	}

	/* To keep it simple we validate that the set and order of stats in
	 * both CSVs are exactly the same. This can be lifted with a bit more
	 * pre-processing later.
	 */
	if (base_specs.spec_cnt != comp_specs.spec_cnt) {
		fprintf(stderr, "Number of stats in '%s' and '%s' differs (%d != %d)!\n",
			env.filenames[0], env.filenames[1],
			base_specs.spec_cnt, comp_specs.spec_cnt);
		return -EINVAL;
	}
	for (i = 0; i < base_specs.spec_cnt; i++) {
		if (base_specs.ids[i] != comp_specs.ids[i]) {
			fprintf(stderr, "Stats composition differs between '%s' and '%s' (%s != %s)!\n",
				env.filenames[0], env.filenames[1],
				stat_defs[base_specs.ids[i]].names[0],
				stat_defs[comp_specs.ids[i]].names[0]);
			return -EINVAL;
		}
	}

	/* Replace user-specified sorting spec with file+prog sorting rule to
	 * be able to join two datasets correctly. Once we are done, we will
	 * restore the original sort spec.
	 */
	tmp_sort_spec = env.sort_spec;
	env.sort_spec = join_sort_spec;
	qsort(env.prog_stats, env.prog_stat_cnt, sizeof(*env.prog_stats), cmp_prog_stats);
	qsort(env.baseline_stats, env.baseline_stat_cnt, sizeof(*env.baseline_stats), cmp_prog_stats);
	env.sort_spec = tmp_sort_spec;

	/* Join two datasets together. If baseline and comparison datasets
	 * have different subset of rows (we match by 'object + prog' as
	 * a unique key) then assume empty/missing/zero value for rows that
	 * are missing in the opposite data set.
	 */
	i = j = 0;
	while (i < env.baseline_stat_cnt || j < env.prog_stat_cnt) {
		const struct verif_stats *base, *comp;
		struct verif_stats_join *join;
		void *tmp;
		int r;

		base = i < env.baseline_stat_cnt ? &env.baseline_stats[i] : &fallback_stats;
		comp = j < env.prog_stat_cnt ? &env.prog_stats[j] : &fallback_stats;

		if (!base->file_name || !base->prog_name) {
			fprintf(stderr, "Entry #%d in '%s' doesn't have file and/or program name specified!\n",
				i, env.filenames[0]);
			return -EINVAL;
		}
		if (!comp->file_name || !comp->prog_name) {
			fprintf(stderr, "Entry #%d in '%s' doesn't have file and/or program name specified!\n",
				j, env.filenames[1]);
			return -EINVAL;
		}

		tmp = realloc(env.join_stats, (env.join_stat_cnt + 1) * sizeof(*env.join_stats));
		if (!tmp)
			return -ENOMEM;
		env.join_stats = tmp;

		join = &env.join_stats[env.join_stat_cnt];
		memset(join, 0, sizeof(*join));

		r = cmp_stats_key(base, comp);
		if (r == 0) {
			join->file_name = base->file_name;
			join->prog_name = base->prog_name;
			join->stats_a = base;
			join->stats_b = comp;
			i++;
			j++;
		} else if (base != &fallback_stats && (comp == &fallback_stats || r < 0)) {
			join->file_name = base->file_name;
			join->prog_name = base->prog_name;
			join->stats_a = base;
			join->stats_b = NULL;
			i++;
		} else if (comp != &fallback_stats && (base == &fallback_stats || r > 0)) {
			join->file_name = comp->file_name;
			join->prog_name = comp->prog_name;
			join->stats_a = NULL;
			join->stats_b = comp;
			j++;
		} else {
			fprintf(stderr, "%s:%d: should never reach here i=%i, j=%i",
				__FILE__, __LINE__, i, j);
			return -EINVAL;
		}
		env.join_stat_cnt += 1;
	}

	/* now sort joined results according to sort spec */
	qsort(env.join_stats, env.join_stat_cnt, sizeof(*env.join_stats), cmp_join_stats);

	/* for human-readable table output we need to do extra pass to
	 * calculate column widths, so we substitute current output format
	 * with RESFMT_TABLE_CALCLEN and later revert it back to RESFMT_TABLE
	 * and do everything again.
	 */
	if (env.out_fmt == RESFMT_TABLE)
		cur_fmt = RESFMT_TABLE_CALCLEN;
	else
		cur_fmt = env.out_fmt;

one_more_time:
	output_comp_headers(cur_fmt);

	last_idx = -1;
	cnt = 0;
	for (i = 0; i < env.join_stat_cnt; i++) {
		const struct verif_stats_join *join = &env.join_stats[i];

		if (!should_output_join_stats(join))
			continue;

		if (env.top_n && cnt >= env.top_n)
			break;

		if (cur_fmt == RESFMT_TABLE_CALCLEN)
			last_idx = i;

		output_comp_stats(join, cur_fmt, i == last_idx);

		cnt++;
	}

	if (cur_fmt == RESFMT_TABLE_CALCLEN) {
		cur_fmt = RESFMT_TABLE;
		goto one_more_time; /* ... this time with feeling */
	}

	return 0;
}

static bool is_stat_filter_matched(struct filter *f, const struct verif_stats *stats)
{
	long value = stats->stats[f->stat_id];

	if (f->abs)
		value = value < 0 ? -value : value;

	switch (f->op) {
	case OP_EQ: return value == f->value;
	case OP_NEQ: return value != f->value;
	case OP_LT: return value < f->value;
	case OP_LE: return value <= f->value;
	case OP_GT: return value > f->value;
	case OP_GE: return value >= f->value;
	}

	fprintf(stderr, "BUG: unknown filter op %d!\n", f->op);
	return false;
}

static bool should_output_stats(const struct verif_stats *stats)
{
	struct filter *f;
	int i, allow_cnt = 0;

	for (i = 0; i < env.deny_filter_cnt; i++) {
		f = &env.deny_filters[i];
		if (f->kind != FILTER_STAT)
			continue;

		if (is_stat_filter_matched(f, stats))
			return false;
	}

	for (i = 0; i < env.allow_filter_cnt; i++) {
		f = &env.allow_filters[i];
		if (f->kind != FILTER_STAT)
			continue;
		allow_cnt++;

		if (is_stat_filter_matched(f, stats))
			return true;
	}

	/* if there are no stat allowed filters, pass everything through */
	return allow_cnt == 0;
}

static void output_prog_stats(void)
{
	const struct verif_stats *stats;
	int i, last_stat_idx = 0, cnt = 0;

	if (env.out_fmt == RESFMT_TABLE) {
		/* calculate column widths */
		output_headers(RESFMT_TABLE_CALCLEN);
		for (i = 0; i < env.prog_stat_cnt; i++) {
			stats = &env.prog_stats[i];
			if (!should_output_stats(stats))
				continue;
			output_stats(stats, RESFMT_TABLE_CALCLEN, false);
			last_stat_idx = i;
		}
	}

	/* actually output the table */
	output_headers(env.out_fmt);
	for (i = 0; i < env.prog_stat_cnt; i++) {
		stats = &env.prog_stats[i];
		if (!should_output_stats(stats))
			continue;
		if (env.top_n && cnt >= env.top_n)
			break;
		output_stats(stats, env.out_fmt, i == last_stat_idx);
		cnt++;
	}
}

static int handle_verif_mode(void)
{
	int i, err = 0;

	if (env.filename_cnt == 0) {
		fprintf(stderr, "Please provide path to BPF object file!\n\n");
		argp_help(&argp, stderr, ARGP_HELP_USAGE, "veristat");
		return -EINVAL;
	}

	create_stat_cgroup();
	for (i = 0; i < env.filename_cnt; i++) {
		err = process_obj(env.filenames[i]);
		if (err) {
			fprintf(stderr, "Failed to process '%s': %d\n", env.filenames[i], err);
			goto out;
		}
	}

	qsort(env.prog_stats, env.prog_stat_cnt, sizeof(*env.prog_stats), cmp_prog_stats);

	output_prog_stats();

out:
	destroy_stat_cgroup();
	return err;
}

static int handle_replay_mode(void)
{
	struct stat_specs specs = {};
	int err;

	if (env.filename_cnt != 1) {
		fprintf(stderr, "Replay mode expects exactly one input CSV file!\n\n");
		argp_help(&argp, stderr, ARGP_HELP_USAGE, "veristat");
		return -EINVAL;
	}

	err = parse_stats_csv(env.filenames[0], &specs,
			      &env.prog_stats, &env.prog_stat_cnt);
	if (err) {
		fprintf(stderr, "Failed to parse stats from '%s': %d\n", env.filenames[0], err);
		return err;
	}

	qsort(env.prog_stats, env.prog_stat_cnt, sizeof(*env.prog_stats), cmp_prog_stats);

	output_prog_stats();

	return 0;
}

int main(int argc, char **argv)
{
	int err = 0, i, j;

	if (argp_parse(&argp, argc, argv, 0, NULL, NULL))
		return 1;

	if (env.show_version) {
		printf("%s\n", argp_program_version);
		return 0;
	}

	if (env.verbose && env.quiet) {
		fprintf(stderr, "Verbose and quiet modes are incompatible, please specify just one or neither!\n\n");
		argp_help(&argp, stderr, ARGP_HELP_USAGE, "veristat");
		return 1;
	}
	if (env.verbose && env.log_level == 0)
		env.log_level = 1;

	if (env.output_spec.spec_cnt == 0) {
		if (env.out_fmt == RESFMT_CSV)
			env.output_spec = default_csv_output_spec;
		else
			env.output_spec = default_output_spec;
	}
	if (env.sort_spec.spec_cnt == 0)
		env.sort_spec = default_sort_spec;

	if (env.comparison_mode && env.replay_mode) {
		fprintf(stderr, "Can't specify replay and comparison mode at the same time!\n\n");
		argp_help(&argp, stderr, ARGP_HELP_USAGE, "veristat");
		return 1;
	}

	if (env.comparison_mode)
		err = handle_comparison_mode();
	else if (env.replay_mode)
		err = handle_replay_mode();
	else
		err = handle_verif_mode();

	free_verif_stats(env.prog_stats, env.prog_stat_cnt);
	free_verif_stats(env.baseline_stats, env.baseline_stat_cnt);
	free(env.join_stats);
	for (i = 0; i < env.filename_cnt; i++)
		free(env.filenames[i]);
	free(env.filenames);
	for (i = 0; i < env.allow_filter_cnt; i++) {
		free(env.allow_filters[i].any_glob);
		free(env.allow_filters[i].file_glob);
		free(env.allow_filters[i].prog_glob);
	}
	free(env.allow_filters);
	for (i = 0; i < env.deny_filter_cnt; i++) {
		free(env.deny_filters[i].any_glob);
		free(env.deny_filters[i].file_glob);
		free(env.deny_filters[i].prog_glob);
	}
	free(env.deny_filters);
	for (i = 0; i < env.npresets; ++i) {
		free(env.presets[i].full_name);
		for (j = 0; j < env.presets[i].atom_count; ++j) {
			switch (env.presets[i].atoms[j].type) {
			case FIELD_NAME:
				free(env.presets[i].atoms[j].name);
				break;
			case ARRAY_INDEX:
				if (env.presets[i].atoms[j].index.type == ENUMERATOR)
					free(env.presets[i].atoms[j].index.svalue);
				break;
			}
		}
		free(env.presets[i].atoms);
	}
	free(env.presets);
	return -err;
}
