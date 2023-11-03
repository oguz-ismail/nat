/* Copyright 2022, 2023 Oğuz İsmail Uysal <oguzismailuysal@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#if defined(__sun) || defined(__USLC__)
#include <stropts.h>
#include <termios.h>
#endif

#define _XOPEN_SOURCE 600

#if defined(__QNX__)
#include "wcwidth.c"
#define wcwidth mk_wcwidth
#endif

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#define TERM_WIDTH  80
#define BUF_ALLOC   512
#define LIST_ALLOC  32
#define ROWS_ALLOC  8
#define RIGHT_ALLOC 16

#define MIN(x, y) ((x) < (y) ? (x) : (y))

struct item {
	const wchar_t *str;
	size_t len;
	size_t width;
};

struct col {
	size_t width;
	int align_right;
};

struct row {
	size_t last;
};

struct seq {
	size_t first;
	size_t step;
	int backward;
};

static wchar_t *buf;
static size_t buf_len;
static size_t buf_alloc;

static struct item *list;
static size_t list_len;
static size_t list_alloc;

static wchar_t delim;
static int words_only;
static int have_colors;
static size_t term_width;
static size_t orig_term_width;
static int num_cols_given;
static size_t padding;
static int across;
static int table;
static int info;

static struct seq right[RIGHT_ALLOC];
static size_t right_len;

static int have_nuls;

static size_t num_rows;
static size_t num_cols;
static struct col *cols;
static size_t unused_space;
static size_t *wider_next;
static struct row *rows;
static size_t rows_alloc;

static int status;

static void die(const char *);
static void *xmalloc(size_t);
static void *xrealloc(void *, size_t);

static int
parse_size(const char *src, char **endptr, size_t *dest) {
	intmax_t i;
	uintmax_t u;
	char *end;

	errno = 0;
	i = strtoimax(src, &end, 10);

	if (errno == 0) {
		if (i < 0 || i > SIZE_MAX) {
			errno = ERANGE;
			return 0;
		}

		u = i;
	}
#if SIZE_MAX > INTMAX_MAX
	else if (errno == ERANGE && i == INTMAX_MAX) {
		errno = 0;
		u = strtoumax(src, &end, 10);

		if (errno != 0) {
			return 0;
		}
		else if (u > SIZE_MAX) {
			errno = ERANGE;
			return 0;
		}
	}
#endif
	else {
		return 0;
	}

	if (end == src) {
		errno = EINVAL;
		return 0;
	}

	while (isspace(*end))
		end++;

	*dest = u;
	*endptr = end;

	return 1;
}

static int
to_size(const char *src, size_t *dest) {
	char *end;

	if (!parse_size(src, &end, dest)) {
		return 0;
	}
	else if (*end != '\0') {
		errno = EINVAL;
		return 0;
	}

	return 1;
}

static void
set_defaults(void) {
	struct winsize ws;
	const char *env;

	delim = L'\n';
	padding = 2;

	if (ioctl(2, TIOCGWINSZ, &ws) == 0) {
		term_width = ws.ws_col;
	}
	else {
		env = getenv("COLUMNS");
		if (!env || !to_size(env, &term_width))
			term_width = TERM_WIDTH;
	}

	orig_term_width = term_width;
}

static int
parse_term_width(const char *src) {
	size_t sub;

	if (src[0] == '-') {
		if (!to_size(&src[1], &sub)) {
			return 0;
		}
		else if (sub > orig_term_width) {
			errno = EINVAL;
			return 0;
		}

		term_width = orig_term_width - sub;
	}
	else if (!to_size(src, &term_width)) {
		return 0;
	}

	return 1;
}

static void
usage_error(void) {
	fputs("Usage:\
\tnat [-d delimiter|-s] [-R] [-w width|-c columns] [-p padding] [-a]\n\
\t    [-r column[,column]...] [-I]\n\
\tnat -t [-d delimiter|-s] [-R] [-p padding] [-r column[,column]...]\n\
\t    [-I]\n", stderr);
	exit(2);
}

static int
parse_seq(const char *src, char **endptr, struct seq *dest) {
	const char *begin;
	char *end;
	size_t first, step;
	int backward;

	step = 0;
	backward = 0;
	begin = src;

	if (*begin == '-') {
		backward = 1;
		begin++;
	}

	if (!parse_size(begin, &end, &first)) {
		return 0;
	}
	else if (first == 0) {
		errno = EINVAL;
		return 0;
	}

	if (*end == '~') {
		begin = end + 1;
		if (!parse_size(begin, &end, &step)) {
			return 0;
		}
	}

	dest->first = first;
	dest->step = step;
	dest->backward = backward;
	*endptr = end;

	return 1;
}

static int
parse_right_aligned(const char *src) {
	const char *begin;
	char *end;

	for (begin = src; ; begin = end + 1) {
		if (right_len >= RIGHT_ALLOC) {
			errno = ENOMEM;
			return 0;
		}

		if (!parse_seq(begin, &end, &right[right_len]))
			return 0;

		right_len++;

		if (*end == '\0') {
			break;
		}
		else if (*end != ',') {
			errno = EINVAL;
			return 0;
		}
	}

	return 1;
}

static void
parse_args(int argc, char *argv[]) {
	int opt;

	while ((opt = getopt(argc, argv, ":d:sRw:c:p:axn:r:tI")) != -1)
		switch (opt) {
		case 'd':
			if (mbtowc(&delim, optarg, strlen(optarg) + 1) == -1) {
				die("-d");
			}
			else if (table && delim == L'\n') {
				errno = EINVAL;
				die("-d");
			}

			words_only = 0;
			break;
		case 's':
			words_only = 1;
			break;
		case 'R':
			have_colors = 1;
			break;
		case 'w':
			if (table)
				usage_error();

			if (!parse_term_width(optarg))
				die(optarg);

			num_cols_given = 0;
			break;
		case 'c':
			if (table)
				usage_error();

			if (!to_size(optarg, &num_cols)) {
				die(optarg);
			}
			else if (num_cols == 0) {
				errno = EINVAL;
				die("-c");
			}

			term_width = SIZE_MAX;
			num_cols_given = 1;
			break;
		case 'p':
			if (!to_size(optarg, &padding))
				die(optarg);

			break;
		case 'x':
		case 'a':
			if (table)
				usage_error();

			across = 1;
			break;
		case 'n':
		case 'r':
			if (!parse_right_aligned(optarg))
				die(optarg);

			break;
		case 't':
			if (!words_only && delim == L'\n')
				delim = L'\t';

			term_width = SIZE_MAX;
			num_cols_given = 0;
			across = 0;
			table = 1;
			num_cols = 0;
			break;
		case 'I':
			info = 1;
			break;
		default:
			usage_error();
		}

	if (optind != argc)
		usage_error();
}

static void
slurp_input(void) {
	wint_t c;

	buf_alloc = BUF_ALLOC;
	buf = xmalloc(buf_alloc * sizeof buf[0]);

	while ((c = getwchar()) != WEOF) {
		if (c == L'\0')
			have_nuls = 1;

		if (buf_len + 1 >= buf_alloc) {
			buf_alloc *= 2;
			buf = xrealloc(buf, buf_alloc * sizeof buf[0]);
		}

		buf[buf_len] = c;
		buf_len++;
	}

	if (ferror(stdin))
		die("stdin");

	if (buf_len == 0)
		exit(0);
}

static void
fix_eof(void) {
	wchar_t eof, correct_eof;

	eof = buf[buf_len - 1];

	if (!table && !words_only && delim != L'\n' && eof == L'\n') {
		buf[buf_len - 1] = delim;
		return;
	}

	if (table)
		correct_eof = L'\n';
	else
		correct_eof = delim;

	if (eof != correct_eof) {
		buf[buf_len] = correct_eof;
		buf_len++;
	}
}

static void
init_parse(void) {
	if (!words_only && delim == L'\0')
		have_nuls = 0;

	fix_eof();

	list_alloc = LIST_ALLOC;
	list = xmalloc(list_alloc * sizeof list[0]);

	if (table) {
		rows_alloc = ROWS_ALLOC;
		rows = xmalloc(rows_alloc * sizeof rows[0]);
	}
}

static size_t
skip_whitespace(size_t begin) {
	size_t i;

	for (i = begin; i < buf_len; i++)
		if (!iswspace(buf[i]) || (table && buf[i] == L'\n'))
			break;

	return i;
}

static size_t
skip_color(size_t begin) {
	size_t i;

	i = begin;
	if (buf_len - i < 3 || buf[i] != L'\33' || buf[i + 1] != L'[')
		return begin;

	i += 2;
	for (; i < buf_len; i++)
		if ((buf[i] < L'0' || buf[i] > L'9') && buf[i] != L';')
			break;

	if (i == buf_len || buf[i] != L'm')
		return begin;

	return i;
}

static size_t
parse_item(size_t begin, struct item *dest) {
	int truncated;
	size_t len, width;
	size_t i, j;
	int w;

	truncated = 0;
	len = 0;
	width = 0;

	for (i = begin; i < buf_len; i++) {
		if (have_colors && (j = skip_color(i)) != i) {
			if (!truncated)
				len += (j - i) + 1;

			i = j;
			continue;
		}

		if (words_only) {
			if (iswspace(buf[i]))
				break;
		}
		else if (buf[i] == delim) {
			break;
		}

		if (table && buf[i] == L'\n')
			break;

		if (truncated)
			continue;

		w = wcwidth(buf[i]);
		if (w == -1)
			w = 0;

		if (width + w > term_width) {
			truncated = 1;
			continue;
		}

		len++;
		width += w;
	}

	if (truncated)
		status = 1;

	dest->str = &buf[begin];
	dest->len = len;
	dest->width = width;

	return i;
}

static void
end_of_row(size_t fields) {
	if (num_rows >= rows_alloc) {
		rows_alloc *= 2;
		rows = xrealloc(rows, rows_alloc * sizeof rows[0]);
	}

	rows[num_rows].last = list_len;
	num_rows++;

	if (fields > num_cols)
		num_cols = fields;
}

static void
save_item(const struct item *src) {
	if (list_len >= list_alloc) {
		list_alloc *= 2;
		list = xrealloc(list, list_alloc * sizeof list[0]);
	}

	list[list_len] = *src;
	list_len++;
}

static void
parse_list(void) {
	size_t begin, end;
	struct item item;
	size_t fields;
	int eol;

	init_parse();

	if (words_only)
		begin = skip_whitespace(0);
	else
		begin = 0;

	if (table)
		fields = 0;

	while (begin < buf_len) {
		end = parse_item(begin, &item);

		if (words_only)
			end = skip_whitespace(end);

		if (table) {
			fields++;
			eol = 0;

			if (buf[end] == L'\n') {
				end_of_row(fields);
				eol = 1;
				fields = 0;
			}
		}

		if (!have_nuls)
			buf[begin + item.len] = L'\0';

		save_item(&item);

		if (words_only) {
			if (table && eol)
				begin = skip_whitespace(end + 1);
			else
				begin = end;
		}
		else {
			begin = end + 1;
		}
	}

	if (list_len == 0)
		exit(0);
}

static size_t
calc_from(size_t n) {
	size_t m;

	m = list_len / n;
	if (list_len % n)
		m++;

	return m;
}

/* Maps each item to the next item with greater width. This helps find the
 * widest item in a column quickly. */
static void
init_lut(void) {
	size_t i, j;

	wider_next = xmalloc(list_len * sizeof wider_next[0]);

	for (i = list_len; i-- > 0; ) {
		j = i + 1;
		while (j < list_len && list[i].width >= list[j].width)
			j = wider_next[j];

		wider_next[i] = j;
	}
}

static void
init_calc(void) {
	size_t max_cols;

	if (num_cols_given) {
		if (num_cols > list_len)
			max_cols = list_len;
		else if (across)
			max_cols = num_cols;
		else
			max_cols = calc_from(calc_from(num_cols));

		unused_space = (num_cols - max_cols) * padding;
	}
	else if (table) {
		max_cols = num_cols;
	}
	else {
		if (padding == 0) 
			max_cols = list_len;
		else
			max_cols = MIN((term_width / padding) + 1, list_len);
	}

	if (across) {
		num_cols = max_cols;
	}
	else if (!table) {
		init_lut();
		num_rows = calc_from(max_cols);
	}

	cols = xmalloc(max_cols * sizeof cols[0]);
}

static size_t
max_width(size_t col) {
	size_t i, j;

	i = col * num_rows;
	j = MIN(i + num_rows, list_len);

	while (wider_next[i] < j)
		i = wider_next[i];

	return list[i].width;
}

static void
init_cols(void) {
	size_t i;

	for (i = 0; i < num_cols; i++)
		cols[i].width = max_width(i);
}

static void
init_cols_across(void) {
	size_t i, j;

	for (i = 0; i < num_cols; i++)
		cols[i].width = list[i].width;

	j = 0;
	for (; i < list_len; i++) {
		if (list[i].width > cols[j].width)
			cols[j].width = list[i].width;

		j++;
		if (j >= num_cols)
			j = 0;
	}
}

static void
init_cols_table(void) {
	size_t i, j, k;

	for (i = 0; i < num_cols; i++)
		cols[i].width = 0;

	i = 0;
	for (j = 0; j < num_rows; j++) {
		k = 0;
		for (; i <= rows[j].last; i++) {
			if (list[i].width > cols[k].width)
				cols[k].width = list[i].width;

			k++;
		}
	}
}

static int
fits(void) {
	size_t width;
	size_t i;

	width = (num_cols - 1) * padding;
	if (width > term_width)
		return 0;

	for (i = 0; i < num_cols; i++) {
		width += max_width(i);
		if (width > term_width)
			return 0;
	}

	init_cols();
	unused_space = term_width - width;
	return 1;
}

static int
fits_across(void) {
	size_t width;
	size_t i, j;

	width = (num_cols - 1) * padding;
	if (width > term_width)
		return 0;

	for (i = 0; i < num_cols; i++)
		cols[i].width = 0;

	j = 0;
	for (i = 0; i < list_len; i++) {
		if (list[i].width > cols[j].width) {
			width += list[i].width - cols[j].width;
			if (width > term_width)
				return 0;

			cols[j].width = list[i].width;
		}

		j++;
		if (j >= num_cols)
			j = 0;
	}

	unused_space = term_width - width;
	return 1;
}

static void
calc_sizes(void) {
	init_calc();

	if (num_cols_given) {
		if (across) {
			num_rows = calc_from(num_cols);
			init_cols_across();
		}
		else {
			num_cols = calc_from(num_rows);
			init_cols();
		}
	}
	else if (table) {
		init_cols_table();
	}
	else if (across) {
		for (; num_cols >= 1; num_cols--) {
			num_rows = calc_from(num_cols);
			if (fits_across())
				break;
		}
	}
	else {
		for (; num_rows <= list_len; num_rows++) {
			num_cols = calc_from(num_rows);
			if (fits())
				break;
		}
	}
}

static void
init_print(void) {
	size_t i, j;
	size_t step, first, last, next;

	for (i = 0; i < num_cols; i++)
		cols[i].align_right = 0;

	for (i = 0; i < right_len; i++) {
		step = right[i].step;

		if (right[i].backward) {
			if (right[i].first > num_cols)
				continue;

			last = num_cols - right[i].first + 1;

			if (step == 0 || step >= last)
				first = last;
			else if (last % step == 0)
				first = step;
			else
				first = last % step;
		}
		else {
			first = right[i].first;
			last = num_cols;
		}

		for (j = first - 1; j < last; j = next) {
			cols[j].align_right = 1;

			next = j + step;
			if (next <= j)
				break;
		}
	}
}

static void
print_info(void) {
	size_t i;
	size_t width;

	printf("%zu", list_len);

	if (num_cols_given || table) {
		width = (num_cols - 1) * padding;
		for (i = 0; i < num_cols; i++)
			width += cols[i].width;

		printf(" %zu", width);
	}
	else {
		printf(" %zu", term_width);
	}

	printf(" %zu", num_rows);
	printf(" %zu", num_cols);
	printf(" %zu", unused_space);

	for (i = 0; i < num_cols; i++)
		printf(" %zu", cols[i].width);

	for (i = 0; i < num_cols; i++)
		if (cols[i].align_right)
			printf(" %zu", i + 1);

	putchar('\n');
}

static void
pad(size_t n) {
	static const wchar_t s[] = L"        ";
	static const size_t slen = (sizeof s / sizeof s[0]) - 1;

	for (; n > slen; n -= slen)
		fputws(s, stdout);

	fputws(&s[slen - n], stdout);
}

static void
print_data(const struct item *data) {
	size_t i;
	
	if (have_nuls)
		for (i = 0; i < data->len; i++)
			putwchar(data->str[i]);
	else
		fputws(data->str, stdout);
}

static void
print_nonempty_cell(size_t i, size_t col, size_t sep) {
	size_t unused;

	unused = cols[col].width - list[i].width;
	if (cols[col].align_right)
		pad(unused);
	else
		sep += unused;

	print_data(&list[i]);
	pad(sep);
}

static void
print_cell(size_t row, size_t col, size_t sep) {
	size_t i;

	if (across)
		i = (row * num_cols) + col;
	else
		i = (col * num_rows) + row;

	if (i >= list_len)
		pad(cols[col].width + sep);
	else
		print_nonempty_cell(i, col, sep);
}

static void
print_table(void) {
	size_t i, j, k, l;
	size_t unused;

	i = 0;
	for (j = 0; j < num_rows; j++) {
		k = 0;
		for (; i < rows[j].last; i++) {
			print_nonempty_cell(i, k, padding);
			k++;
		}

		unused = 0;
		for (l = k + 1; l < num_cols; l++)
			unused += padding + cols[l].width;

		print_nonempty_cell(i, k, unused);
		putwchar(L'\n');
		
		i++;
	}
}

static void
print_cols(void) {
	size_t i, j;

	init_print();

	if (info) {
		print_info();
	}
	else if (table) {
		print_table();
	}
	else {
		for (i = 0; i < num_rows; i++) {
			for (j = 0; j < num_cols - 1; j++)
				print_cell(i, j, padding);

			print_cell(i, j, unused_space);
			putwchar(L'\n');
		}
	}
}

int
main(int argc, char *argv[]) {
	setlocale(LC_ALL, "");
	set_defaults();
	parse_args(argc, argv);
	slurp_input();
	parse_list();
	calc_sizes();
	print_cols();
	return status;
}

static void
die(const char *s) {
	perror(s);
	exit(2);
}

static void *
xmalloc(size_t n) {
	void *ptr;

	ptr = malloc(n);
	if (ptr == NULL && n > 0)
		die(NULL);

	return ptr;
}

static void *
xrealloc(void *ptr, size_t n) {
	void *result;

	result = realloc(ptr, n);
	if (result == NULL && (ptr == NULL || n > 0))
		die(NULL);
	
	return result;
}
