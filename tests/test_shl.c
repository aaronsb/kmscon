/*
 * test_shl - Test shl library
 *
 * Copyright (c) 2022 Victor Westerhuis <victor@westerhu.is>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "shl/hashtable.h"
#include "shl/misc.h"
#include "test_common.h"

/* mallinfo2() is glibc's, from 2.33; elsewhere the heap is not measured */
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
#include <malloc.h>
#define HAVE_MALLINFO2 1
#else
#define HAVE_MALLINFO2 0
#endif

#define check_assert_string_list_eq(X, Y)                                                          \
	do {                                                                                       \
		unsigned int i;                                                                    \
		const char **x, **y;                                                               \
                                                                                                   \
		x = (X);                                                                           \
		y = (Y);                                                                           \
                                                                                                   \
		for (i = 0; x[i] && y[i]; ++i)                                                     \
			ck_assert_str_eq(x[i], y[i]);                                              \
		ck_assert_ptr_eq(x[i], NULL);                                                      \
		ck_assert_ptr_eq(y[i], NULL);                                                      \
	} while (0)

START_TEST(test_split_command_string)
{
	int ret;
	unsigned int i, n, n_list, n_expected;
	char **list;

	const char *invalid_command_strings[] = {
		"\"", "'", "\\", "\"/bin/true", "'/bin/true", "/bin/true\\", "ls -h \"*.c'",
	};
	n = sizeof(invalid_command_strings) / sizeof(invalid_command_strings[0]);

	for (i = 0; i < n; ++i) {
		list = TEST_INVALID_PTR;
		n_list = -10;

		ret = shl_split_command_string(invalid_command_strings[i], &list, &n_list);
		ck_assert_int_eq(ret, -EINVAL);
		ck_assert_ptr_eq(list, TEST_INVALID_PTR);
		ck_assert_uint_eq(n_list, (unsigned int)-10);
	}

	const char *expected_command_list[] = {"'/bin/command with space",
					       "\t\\argument=\"quoted\"",
					       "plain\3argument",
					       " an\tother='ere",
					       "\"ends with \\",
					       "\\\"more\\bquotes\\",
					       NULL};
	n_expected = sizeof(expected_command_list) / sizeof(expected_command_list[0]) - 1;
	const char *valid_command_strings[] = {
		"\\'/bin/command\\ with\\ space \\\t\\\\argument=\\\"quoted\\\" plain\3argument \\ "
		"an\\\tother=\\'ere \\\"ends\\ with\\ \\\\ \\\\\\\"more\\\\bquotes\\\\",
		"\"'/bin/command with space\" \"\t\\argument=\\\"quoted\\\"\" \"plain\3argument\" "
		"\" an\tother='ere\" \"\\\"ends with \\\\\" \"\\\\\\\"more\\bquotes\\\\\"",
		"\"'\"'/bin/command with space' '\t\\argument=\"quoted\"' 'plain\3argument' ' "
		"an\tother='\"'\"'ere' '\"ends with \\' '\\\"more\\bquotes\\'",
		"   \\'/bin/command\\ with\\ space\t\t\\\t\\\\argument=\\\"quoted\\\"\t "
		"plain\3argument  \t\\ an\\\tother=\\'ere   \\\"ends\\ with\\ \\\\   \t  "
		"\\\\\\\"more\\\\\\bquotes\\\\   \t \t",
	};
	n = sizeof(valid_command_strings) / sizeof(valid_command_strings[0]);

	for (i = 0; i < n; ++i) {
		list = TEST_INVALID_PTR;
		n_list = -10;

		ret = shl_split_command_string(valid_command_strings[i], &list, &n_list);
		ck_assert_int_eq(ret, 0);
		ck_assert_ptr_ne(list, TEST_INVALID_PTR);
		check_assert_string_list_eq((const char **)list, expected_command_list);
		ck_assert_uint_eq(n_list, n_expected);
		free(list);
		list = TEST_INVALID_PTR;
	}

	const char *empty_command_strings[] = {
		"",
		" ",
		"\t\t  \t",
	};
	n = sizeof(empty_command_strings) / sizeof(empty_command_strings[0]);

	for (i = 0; i < n; ++i) {
		list = TEST_INVALID_PTR;
		n_list = -10;

		ret = shl_split_command_string(empty_command_strings[i], &list, &n_list);
		ck_assert_int_eq(ret, 0);
		ck_assert_ptr_ne(list, TEST_INVALID_PTR);
		ck_assert_ptr_eq(list[0], NULL);
		ck_assert_uint_eq(n_list, 0);
		free(list);
		list = TEST_INVALID_PTR;
	}

	{
		list = TEST_INVALID_PTR;
		n_list = -10;

		ret = shl_split_command_string(valid_command_strings[0], &list, &n_list);
		ck_assert_int_eq(ret, 0);
		ck_assert_ptr_ne(list, TEST_INVALID_PTR);
		check_assert_string_list_eq((const char **)list, expected_command_list);
		ck_assert_uint_eq(n_list, n_expected);
		free(list);
	}

	{
		list = TEST_INVALID_PTR;

		ret = shl_split_command_string(valid_command_strings[0], &list, NULL);
		ck_assert_int_eq(ret, 0);
		ck_assert_ptr_ne(list, TEST_INVALID_PTR);
		check_assert_string_list_eq((const char **)list, expected_command_list);
		free(list);
	}

	{
		n_list = -10;

		ret = shl_split_command_string(valid_command_strings[0], NULL, &n_list);
		ck_assert_int_eq(ret, -EINVAL);
		ck_assert_uint_eq(n_list, (unsigned int)-10);
	}

	{
		list = TEST_INVALID_PTR;
		n_list = -10;

		ret = shl_split_command_string(NULL, &list, &n_list);
		ck_assert_int_eq(ret, -EINVAL);
		ck_assert_ptr_eq(list, TEST_INVALID_PTR);
		ck_assert_uint_eq(n_list, (unsigned int)-10);
	}

	{
		n_list = -10;

		ret = shl_split_command_string(NULL, NULL, &n_list);
		ck_assert_int_eq(ret, -EINVAL);
		ck_assert_uint_eq(n_list, (unsigned int)-10);
	}
}
END_TEST

/* bytes in use on the heap, the blocks malloc maps on its own included */
static size_t heap_in_use(void)
{
#if HAVE_MALLINFO2
	struct mallinfo2 mi = mallinfo2();

	return mi.uordblks + mi.hblkhd;
#else
	return 0;
#endif
}

static unsigned int values_freed;

static void count_free(void *value)
{
	values_freed++;
	free(value);
}

/*
 * #40: shl_hashtable_remove frees its entry and leaves the value to the
 * caller. HASHTABLE_CYCLES inserts each followed by its remove hold no more
 * heap at the end than one entry's worth per thousand; an entry left behind
 * on each remove is a thousand times that. The table's own free_value is not
 * called for a removed value, and is called for the one left at the end.
 * Under ASan mallinfo2() measures nothing and the heap check passes whatever
 * happens; LeakSanitizer (make asan) catches the leak there, a single entry
 * as surely as all of them. The count fits check's default timeout of 4
 * seconds on a slow runner; a million did not on GitHub's.
 */
#define HASHTABLE_CYCLES 100000

START_TEST(test_hashtable_remove)
{
	struct shl_hashtable *tbl;
	size_t before, after;
	unsigned int i;
	void *value, *found;
	int ret;

	ret = shl_hashtable_new(&tbl, shl_direct_hash, shl_direct_equal, count_free);
	ck_assert_int_eq(ret, 0);
	/* the table at its working size before measuring */
	ck_assert_int_eq(shl_hashtable_insert(tbl, 0, malloc(1)), 0);

	before = heap_in_use();
	for (i = 1; i <= HASHTABLE_CYCLES; i++) {
		value = malloc(1);
		ck_assert_int_eq(shl_hashtable_insert(tbl, i, value), 0);
		ck_assert(shl_hashtable_find(tbl, &found, i));
		ck_assert_ptr_eq(found, value);
		shl_hashtable_remove(tbl, i);
		ck_assert(!shl_hashtable_find(tbl, &found, i));
		free(value);
	}
	after = heap_in_use();
	ck_assert_uint_eq(values_freed, 0);
	if (HAVE_MALLINFO2)
		ck_assert_uint_le(after,
				  before + HASHTABLE_CYCLES / 1000 * sizeof(struct shl_hashentry));

	shl_hashtable_free(tbl);
	ck_assert_uint_eq(values_freed, 1);
}
END_TEST

TEST_DEFINE_CASE(misc)
TEST(test_split_command_string)
TEST(test_hashtable_remove)
TEST_END_CASE

TEST_DEFINE(TEST_SUITE(shl, TEST_CASE(misc), TEST_END))
