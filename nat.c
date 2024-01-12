/* Copyright 2022, 2023, 2024 Oğuz İsmail Uysal <oguzismailuysal@gmail.com>
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

#define _GNU_SOURCE

#if defined(__GNUC__) && __GNUC__ < 5
#define restrict __restrict
#endif

#if defined(__sun) || defined(__USLC__)
#include <stropts.h>
#include <termios.h>
#endif

#if !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 600
#endif

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

#if defined(__GLIBC__)
#define getwchar getwchar_unlocked
#define putwchar putwchar_unlocked
#define fputws fputws_unlocked
#endif

#define MIN(x, y) ((x) < (y) ? (x) : (y))

struct seq {
	int backward;
	size_t first;
	size_t step;
};

struct item {
	const wchar_t *text;
	size_t len;
	size_t width;
};

struct col {
	int right_aligned;
	size_t width;
};

struct row {
	size_t last;
};

static wchar_t delim;
static int words;
static int sentences;
static int nuls;
static int colors;
static int cols_fixed;
static int table;
static size_t tail;

static size_t term_width = 80;
static size_t padding;
static int across;
static int info;
static struct seq right[16];
static size_t right_len;

static wchar_t *buf;
static size_t buf_len;
static size_t buf_alloc = 512;

static struct item *list;
static size_t list_len;
static size_t list_alloc = 32;

static size_t num_rows;
static size_t num_cols;
static struct col *cols;
static struct row *rows;
static size_t rows_alloc = 8;
static size_t surplus;
static size_t *wider;
static int status;

static void die(const char *);
static void *xmalloc(size_t);
static void *xrealloc(void *, size_t);
static int xwcwidth(wchar_t);

static void
usage_error(void) {
	fputs("Usage:\
\tnat [-d delimiter|-s|-S] [-R] [-w width|-c columns] [-p padding] [-a]\n\
\t    [-r column[,column]...] [-I]\n\
\tnat -t [-d delimiter|-s|-S] [-R] [-c columns] [-p padding]\n\
\t    [-r column[,column]...] [-I]\n", stderr);
	exit(2);
}

static int
parse_size(const char *s, char **endp, size_t *dst) {
	intmax_t i;
	uintmax_t u;
	char *end;

	errno = 0;
	i = strtoimax(s, &end, 10);

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
		u = strtoumax(s, &end, 10);

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

	if (end == s) {
		errno = EINVAL;
		return 0;
	}

	while (isspace(*end))
		end++;

	*dst = u;
	*endp = end;

	return 1;
}

static int
to_size(const char *s, size_t *dst) {
	char *end;

	if (!parse_size(s, &end, dst)) {
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

	if (ioctl(2, TIOCGWINSZ, &ws) == 0)
		term_width = ws.ws_col;
	else if ((env = getenv("COLUMNS")))
		to_size(env, &term_width);

	delim = L'\n';
	padding = 2;
}

static int
parse_width(const char *s) {
	size_t x;

	if (*s == '-') {
		if (!to_size(&s[1], &x)) {
			return 0;
		}
		else if (x > term_width) {
			errno = EINVAL;
			return 0;
		}

		term_width -= x;
	}
	else if (!to_size(s, &term_width)) {
		return 0;
	}

	return 1;
}

static int
parse_seq(const char *p, char **endp, struct seq *dst) {
	int backward;
	size_t first, step;
	char *end;

	backward = 0;
	step = 0;

	if (*p == '-') {
		backward = 1;
		p++;
	}

	if (!parse_size(p, &end, &first)) {
		return 0;
	}
	else if (first == 0) {
		errno = EINVAL;
		return 0;
	}

	if (*end == '~') {
		p = end+1;
		if (!parse_size(p, &end, &step))
			return 0;
	}

	dst->backward = backward;
	dst->first = first;
	dst->step = step;
	*endp = end;

	return 1;
}

static int
parse_right(const char *p) {
	char *end;

	for (;;) {
		if (right_len >= sizeof right/sizeof right[0]) {
			errno = ENOMEM;
			return 0;
		}

		if (!parse_seq(p, &end, &right[right_len]))
			return 0;

		right_len++;

		if (*end == '\0') {
			return 1;
		}
		else if (*end != ',') {
			errno = EINVAL;
			return 0;
		}

		p = end+1;
	}
}

static void
parse_args(int argc, char *argv[]) {
	int opt;
	size_t x;

	while ((opt = getopt(argc, argv, ":d:sSRw:c:p:axn:r:tI")) != -1)
		switch (opt) {
		case 'd':
			if (mbtowc(&delim, optarg, strlen(optarg)+1) == -1) {
				die("-d");
			}
			else if (table && delim == L'\n') {
				errno = EINVAL;
				die("-d");
			}

			words = 0;
			break;
		case 's':
			words = 1;
			sentences = 0;
			break;
		case 'S':
			words = 1;
			sentences = 1;
			break;
		case 'R':
			colors = 1;
			break;
		case 'w':
			if (table)
				usage_error();

			if (!parse_width(optarg))
				die(optarg);

			cols_fixed = 0;
			break;
		case 'c':
			if (!to_size(optarg, &x)) {
				die(optarg);
			}
			else if (x == 0) {
				errno = EINVAL;
				die("-c");
			}

			if (table) {
				tail = x;
			}
			else {
				num_cols = x;
				cols_fixed = 1;
			}

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
			if (!parse_right(optarg))
				die(optarg);

			break;
		case 't':
			if (!words && delim == L'\n')
				delim = L'\t';

			if (cols_fixed && num_cols != 0) {
				tail = num_cols;
				num_cols = 0;
			}
			else {
				tail = SIZE_MAX;
			}

			across = 0;
			table = 1;
			cols_fixed = 1;
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

	buf = xmalloc(buf_alloc*sizeof buf[0]);

	while ((c = getwchar()) != WEOF) {
		if (buf_len >= buf_alloc) {
			buf_alloc *= 2;
			buf = xrealloc(buf, buf_alloc*sizeof buf[0]);
		}

		buf[buf_len++] = c;
	}

	if (ferror(stdin))
		die("stdin");

	if (buf_len == 0)
		exit(0);
}

static void
fix_eof(void) {
	wchar_t found, expected;

	found = buf[buf_len-1];

	if (table) {
		expected = L'\n';
	}
	else if (words) {
		return;
	}
	else if (delim != L'\n' && found == L'\n') {
		buf[buf_len-1] = delim;
		return;
	}
	else {
		expected = delim;
	}

	if (found != expected) {
		if (buf_len >= buf_alloc) {
			buf_alloc++;
			buf = xrealloc(buf, buf_alloc*sizeof buf[0]);
		}

		buf[buf_len++] = expected;
	}
}

static void
init_parse(void) {
	if ((words || delim != L'\0') && wmemchr(buf, L'\0', buf_len))
		nuls = 1;

	fix_eof();
	list = xmalloc(list_alloc*sizeof list[0]);

	if (table)
		rows = xmalloc(rows_alloc*sizeof rows[0]);
}

static size_t
skip_spaces(size_t i) {
	for (; i < buf_len; i++)
		if (!iswspace(buf[i]) || (table && buf[i] == L'\n'))
			break;

	return i;
}

static size_t
skip_color(size_t i) {
	size_t j;

	if (buf_len-i < 3 || wmemcmp(&buf[i], L"\33[", 2))
		return i;

	for (j = i+2; j < buf_len; j++)
		if (!wcschr(L"0123456789;", buf[j]))
			break;

	if (j == buf_len || buf[j] != L'm')
		return i;

	return j;
}

static int
is_delim(size_t i) {
	if (words) {
		if (sentences && buf[i] == L' ') {
			if (i >= buf_len-1 || iswspace(buf[i+1]))
				return 1;
		}
		else if (iswspace(buf[i])) {
			return 1;
		}
	}
	else if (buf[i] == delim || (table && buf[i] == L'\n')) {
		return 1;
	}

	return 0;
}

static size_t
parse_tail(size_t begin, struct item *dst) {
	size_t i, j;
	size_t len, width;

	len = 0;
	width = 0;

	for (i = begin; i < buf_len; i++) {
		if (colors) {
			j = skip_color(i);
			if (j != i) {
				len += j-i + 1;
				i = j;
				continue;
			}
		}

		if (buf[i] == L'\n')
			break;

		len++;
		width += xwcwidth(buf[i]);
	}

	dst->text = &buf[begin];
	dst->len = len;
	dst->width = width;

	return i;
}

static size_t
parse_item(size_t begin, struct item *dst) {
	size_t len, width;
	int truncated;
	size_t i, j;
	int x;

	len = 0;
	width = 0;
	truncated = 0;

	for (i = begin; i < buf_len; i++) {
		if (colors && (j = skip_color(i)) != i) {
			if (!truncated)
				len += j-i + 1;

			i = j;
			continue;
		}

		if (is_delim(i))
			break;

		if (truncated)
			continue;

		x = xwcwidth(buf[i]);
		if (!cols_fixed && width+x > term_width) {
			truncated = 1;
			continue;
		}

		len++;
		width += x;
	}

	dst->text = &buf[begin];
	dst->len = len;
	dst->width = width;

	if (truncated)
		status = 1;

	return i;
}

static void
end_of_row(size_t fields) {
	if (num_rows >= rows_alloc) {
		rows_alloc *= 2;
		rows = xrealloc(rows, rows_alloc*sizeof rows[0]);
	}

	rows[num_rows++].last = list_len;

	if (fields > num_cols)
		num_cols = fields;
}

static void
save_item(const struct item *p) {
	if (list_len >= list_alloc) {
		list_alloc *= 2;
		list = xrealloc(list, list_alloc*sizeof list[0]);
	}

	list[list_len++] = *p;
}

static void
parse_list(void) {
	size_t i, end;
	struct item item;
	size_t fields;
	int eol;

	init_parse();

	if (words)
		i = skip_spaces(0);
	else
		i = 0;

	fields = 0;

	while (i < buf_len) {
		if (table && fields >= tail-1)
			end = parse_tail(i, &item);
		else
			end = parse_item(i, &item);

		if (words)
			end = skip_spaces(end);

		if (table) {
			fields++;
			eol = 0;

			if (buf[end] == L'\n') {
				end_of_row(fields);
				fields = 0;
				eol = 1;
			}
		}

		if (!nuls)
			buf[i+item.len] = L'\0';

		save_item(&item);

		if (words) {
			if (table && eol)
				i = skip_spaces(end+1);
			else
				i = end;
		}
		else {
			i = end+1;
		}
	}

	if (list_len == 0)
		exit(0);
}

static size_t
calc_from(size_t x) {
	size_t y;

	y = list_len/x;
	if (list_len%x)
		y++;

	return y;
}

/* Maps each item to the next item with greater width. This helps find the
 * widest item in a column quickly. */
static void
init_lut(void) {
	size_t i, j;

	wider = xmalloc(list_len*sizeof wider[0]);

	for (i = list_len; i-- > 0; ) {
		for (j = i+1; j < list_len; j = wider[j])
			if (list[j].width > list[i].width)
				break;

		wider[i] = j;
	}
}

static void
init_calc(void) {
	size_t max_cols;

	if (table) {
		max_cols = num_cols;
	}
	else if (cols_fixed) {
		if (num_cols > list_len)
			max_cols = list_len;
		else if (across)
			max_cols = num_cols;
		else
			max_cols = calc_from(calc_from(num_cols));

		surplus = (num_cols-max_cols)*padding;
	}
	else {
		if (padding == 0) 
			max_cols = list_len;
		else
			max_cols = MIN(term_width/padding + 1, list_len);
	}

	if (across) {
		num_cols = max_cols;
	}
	else if (!table) {
		init_lut();
		num_rows = calc_from(max_cols);
	}

	cols = xmalloc(max_cols*sizeof cols[0]);
}

static size_t
max_width(size_t col) {
	size_t i, j;

	i = col*num_rows;
	j = MIN(i+num_rows, list_len);

	while (wider[i] < j)
		i = wider[i];

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
	size_t col, i;

	for (i = 0; i < num_cols; i++)
		cols[i].width = list[i].width;

	col = 0;
	for (; i < list_len; i++) {
		if (list[i].width > cols[col].width)
			cols[col].width = list[i].width;

		col++;
		if (col >= num_cols)
			col = 0;
	}
}

static void
init_cols_table(void) {
	size_t row, col, i;

	for (i = 0; i < num_cols; i++)
		cols[i].width = 0;

	i = 0;
	for (row = 0; row < num_rows; row++) {
		for (col = 0; i <= rows[row].last; col++) {
			if (list[i].width > cols[col].width)
				cols[col].width = list[i].width;

			i++;
		}
	}
}

static int
fits(void) {
	size_t width;
	size_t i;

	width = (num_cols-1)*padding;
	if (width > term_width)
		return 0;

	for (i = 0; i < num_cols; i++) {
		width += max_width(i);
		if (width > term_width)
			return 0;
	}

	init_cols();
	surplus = term_width-width;

	return 1;
}

static int
fits_across(void) {
	size_t width;
	size_t col, i;

	width = (num_cols-1)*padding;
	if (width > term_width)
		return 0;

	for (i = 0; i < num_cols; i++)
		cols[i].width = 0;

	col = 0;
	for (i = 0; i < list_len; i++) {
		if (list[i].width > cols[col].width) {
			width += list[i].width-cols[col].width;
			if (width > term_width)
				return 0;

			cols[col].width = list[i].width;
		}

		col++;
		if (col >= num_cols)
			col = 0;
	}

	surplus = term_width-width;

	return 1;
}

static void
calc_sizes(void) {
	init_calc();

	if (table) {
		init_cols_table();
	}
	else if (cols_fixed) {
		if (across) {
			num_rows = calc_from(num_cols);
			init_cols_across();
		}
		else {
			num_cols = calc_from(num_rows);
			init_cols();
		}
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
		cols[i].right_aligned = 0;

	for (i = 0; i < right_len; i++) {
		step = right[i].step;

		if (right[i].backward) {
			if (right[i].first > num_cols)
				continue;

			last = num_cols-right[i].first + 1;

			if (step == 0 || step >= last)
				first = last;
			else if (last%step)
				first = last%step;
			else
				first = step;
		}
		else {
			first = right[i].first;
			last = num_cols;
		}

		for (j = first-1; j < last; j = next) {
			cols[j].right_aligned = 1;

			next = j+step;
			if (next <= j)
				break;
		}
	}
}

static void
print_info(void) {
	size_t i;
	size_t width;

	if (cols_fixed) {
		width = (num_cols-1)*padding;
		for (i = 0; i < num_cols; i++)
			width += cols[i].width;
	}
	else {
		width = term_width;
	}

	printf("%zu", list_len);
	printf(" %zu", width);
	printf(" %zu", num_rows);
	printf(" %zu", num_cols);
	printf(" %zu", surplus);

	for (i = 0; i < num_cols; i++)
		printf(" %zu", cols[i].width);

	for (i = 0; i < num_cols; i++)
		if (cols[i].right_aligned)
			printf(" %zu", i+1);

	putchar('\n');
}

static void
pad(size_t n) {
	static const wchar_t s[] = L"        ";
	static const size_t slen = sizeof s/sizeof s[0] - 1;

	for (; n > slen; n -= slen)
		fputws(s, stdout);

	fputws(&s[slen-n], stdout);
}

static void
print_item(const struct item *restrict p, size_t col, size_t space) {
	size_t empty;
	size_t i;

	empty = cols[col].width - p->width;
	if (cols[col].right_aligned)
		pad(empty);
	else
		space += empty;

	if (nuls)
		for (i = 0; i < p->len; i++)
			putwchar(p->text[i]);
	else
		fputws(p->text, stdout);

	pad(space);
}

static void
print_cell(size_t row, size_t col, size_t space) {
	size_t i;

	if (across)
		i = row*num_cols + col;
	else
		i = col*num_rows + row;

	if (i >= list_len)
		pad(cols[col].width+space);
	else
		print_item(&list[i], col, space);
}

static void
print_table(void) {
	size_t row, col, i, j;
	size_t empty;

	i = 0;
	for (row = 0; row < num_rows; row++) {
		col = 0;
		for (; i < rows[row].last; i++) {
			print_item(&list[i], col, padding);
			col++;
		}

		empty = 0;
		for (j = col+1; j < num_cols; j++)
			empty += padding+cols[j].width;

		print_item(&list[i], col, empty);
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
			for (j = 0; j < num_cols-1; j++)
				print_cell(i, j, padding);

			print_cell(i, j, surplus);
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
	void *p;

	p = malloc(n);
	if (p == NULL && n > 0)
		die(NULL);

	return p;
}

static void *
xrealloc(void *p, size_t n) {
	void *q;

	q = realloc(p, n);
	if (q == NULL && (p == NULL || n > 0))
		die(NULL);
	
	return q;
}

static int
xwcwidth(wchar_t c) {
	int x;

	x = wcwidth(c);
	if (x == -1)
		return 0;

	return x;
}
