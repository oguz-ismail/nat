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

#define TERM_WIDTH 80
#define BUF_ALLOC 512
#define LIST_ALLOC 32

#define MIN(x, y) ((x) < (y) ? (x) : (y))

struct item {
	const wchar_t *str;
	size_t len;
	size_t width;
};

struct col {
	size_t width;
};

static wchar_t *buf;
static size_t buf_len;
static size_t buf_alloc;

static struct item *list;
static size_t list_len;
static size_t list_alloc;

static wchar_t delim;
static size_t term_width;
static int num_cols_fixed;
static size_t padding;
static int across;

static size_t num_rows;
static size_t num_cols;
static struct col *cols;
static size_t blank_space;

static int binary;
static int status;

static void die(const char *);
static void *xmalloc(size_t);
static void *xrealloc(void *, size_t);

static int
parse_size(const char *src, char **endp, int base, size_t *dest) {
	intmax_t i;
	uintmax_t u;

	errno = 0;
	i = strtoimax(src, endp, base);

	if (errno == 0 && i >= 0 && i <= SIZE_MAX) {
		*dest = i;
		return 1;
	}

#if SIZE_MAX > INTMAX_MAX
	if (errno == ERANGE && i == INTMAX_MAX) {
		errno = 0;
		u = strtoumax(src, endp, base);

		if (errno == 0 && u <= SIZE_MAX) {
			*dest = u;
			return 1;
		}
	}
#endif

	if (errno == 0)
		errno = ERANGE;

	return 0;
}

static int
parse_size_arg(const char *src, size_t *dest) {
	char *end;

	if (!parse_size(src, &end, 10, dest)) {
		return 0;
	}
	else if (end == src || *end != '\0') {
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
		if (!env || !parse_size_arg(env, &term_width))
			term_width = TERM_WIDTH;
	}
}

static void
parse_args(int argc, char *argv[]) {
	int opt;
	size_t minus_one;

	minus_one = term_width - 1;

	while ((opt = getopt(argc, argv, ":d:w:c:p:a")) != -1)
		switch (opt) {
		case 'd':
			if (mbtowc(&delim, optarg, strlen(optarg) + 1) == -1)
				die("-d");

			break;
		case 'w':
			if (strcmp(optarg, "-1") == 0 && minus_one != SIZE_MAX)
				term_width = minus_one;
			else if (!parse_size_arg(optarg, &term_width))
				die(optarg);

			num_cols_fixed = 0;
			break;
		case 'c':
			if (!parse_size_arg(optarg, &num_cols)) {
				die(optarg);
			}
			else if (num_cols == 0) {
				errno = EINVAL;
				die("-c");
			}

			num_cols_fixed = 1;
			term_width = SIZE_MAX;
			break;
		case 'p':
			if (!parse_size_arg(optarg, &padding))
				die(optarg);

			break;
		case 'a':
			across = 1;
			break;
		default:
			fprintf(stderr, "Usage: \
%s [-d delimiter] [-w width|-c columns] [-p padding] [-a]\n", argv[0]);
			exit(2);
		}
}

static void
slurp_input(void) {
	wint_t c;

	buf_alloc = BUF_ALLOC;
	buf = xmalloc(buf_alloc * sizeof buf[0]);

	while ((c = getwchar()) != WEOF) {
		if (c == L'\0')
			binary = 1;

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

static size_t
parse_item(size_t pos, struct item *itemp) {
	size_t len, width;
	int truncated;
	size_t i;
	int w;

	len = 0;
	width = 0;
	truncated = 0;

	for (i = pos; i < buf_len; i++) {
		if (buf[i] == delim)
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

	itemp->str = &buf[pos];
	itemp->len = len;
	itemp->width = width;

	return i;
}

static void
parse_list(void) {
	size_t i, end;
	struct item item;

	if (delim == L'\0')
		binary = 0;

	list_alloc = LIST_ALLOC;
	list = xmalloc(list_alloc * sizeof list[0]);

	for (i = 0; i < buf_len; i = end + 1) {
		end = parse_item(i, &item);

		if (!binary)
			buf[i + item.len] = L'\0';

		if (list_len >= list_alloc) {
			list_alloc *= 2;
			list = xrealloc(list, list_alloc * sizeof list[0]);
		}

		list[list_len] = item;
		list_len++;
	}
}

static size_t *next_wider;

/* Maps each item to the next item with greater width. This helps find the
 * widest item in a column quickly. */
static void
init_lut(void) {
	size_t i, j;

	next_wider = xmalloc(list_len * sizeof next_wider[0]);

	for (i = list_len; i-- > 0; ) {
		j = i + 1;
		while (j < list_len && list[i].width >= list[j].width)
			j = next_wider[j];

		next_wider[i] = j;
	}
}

static size_t
max_width(size_t col) {
	size_t i, n;

	i = col * num_rows;
	n = MIN(i + num_rows, list_len);

	while (next_wider[i] < n)
		i = next_wider[i];

	return list[i].width;
}

static size_t
calc_from(size_t n) {
	size_t m;

	m = list_len / n;
	if (list_len % n)
		m++;

	return m;
}

static void
init_calc(void) {
	size_t max_cols;

	if (num_cols_fixed) {
		if (num_cols > list_len)
			max_cols = list_len;
		else if (across)
			max_cols = num_cols;
		else
			max_cols = calc_from(calc_from(num_cols));

		blank_space = (num_cols - max_cols) * padding;
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
	else {
		init_lut();
		num_rows = calc_from(max_cols);
	}

	cols = xmalloc(max_cols * sizeof cols[0]);
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

	for (i = 0; i < num_cols; i++)
		cols[i].width = max_width(i);
		
	if (!num_cols_fixed)
		blank_space = term_width - width;

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

	if (!num_cols_fixed)
		blank_space = term_width - width;

	return 1;
}

static void
calc_dims(void) {
	init_calc();

	if (across)
		for (; num_cols >= 1; num_cols--) {
			num_rows = calc_from(num_cols);
			if (fits_across())
				return;
		}
	else
		for (; num_rows <= list_len; num_rows++) {
			num_cols = calc_from(num_rows);
			if (fits())
				return;
		}
}

static void
print_item(const struct item *itemp) {
	size_t i;
	
	if (binary)
		for (i = 0; i < itemp->len; i++)
			putwchar(itemp->str[i]);
	else
		fputws(itemp->str, stdout);
}

static void
pad(size_t n) {
	static const wchar_t s[] = L"        ";
	static const size_t step = (sizeof s / sizeof s[0]) - 1;

	for (; n > step; n -= step)
		fputws(s, stdout);

	fputws(&s[step - n], stdout);
}

static void
print_cell(size_t row, size_t col, size_t space) {
	size_t i;
	size_t width;

	if (across)
		i = (row * num_cols) + col;
	else
		i = (col * num_rows) + row;

	if (i >= list_len) {
		width = 0;
	}
	else {
		print_item(&list[i]);
		width = list[i].width;
	}

	if (width < cols[col].width)
		space += cols[col].width - width;

	pad(space);
}

static void
print_cols(void) {
	size_t i, j;

	for (i = 0; i < num_rows; i++) {
		for (j = 0; j < num_cols - 1; j++)
			print_cell(i, j, padding);

		print_cell(i, j, blank_space);
		putwchar(L'\n');
	}
}

int
main(int argc, char *argv[]) {
	setlocale(LC_ALL, "");
	set_defaults();
	parse_args(argc, argv);
	slurp_input();
	parse_list();
	calc_dims();
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
