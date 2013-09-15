// Copyright 2010-2013 Wincent Colaiuta. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "match.h"
#include "ext.h"
#include "ruby_compat.h"

// use a struct to make passing params during recursion easier
typedef struct
{
    char    *haystack_p;            // pointer to the path string to be searched
    long    haystack_len;           // length of same
    char    *needle_p;              // pointer to search string (needle)
    long    needle_len;             // length of same
    double  max_score_per_char;
    int     dot_file;               // boolean: true if str is a dot-file
    int     always_show_dot_files;  // boolean
    int     never_show_dot_files;   // boolean
} matchinfo_t;

double recursive_match(matchinfo_t *m,    // sharable meta-data
                       long haystack_idx, // where in the path string to start
                       long needle_idx,   // where in the needle string to start
                       long last_idx,     // location of last matched character
                       double score)      // cumulative score so far
{
    double seen_score = 0;      // remember best score seen via recursion
    int dot_file_match = 0;     // true if needle matches a dot-file
    int dot_search = 0;         // true if searching for a dot

    // bail early if not enough room (left) in haystack for (rest of) needle
    if (m->haystack_len - haystack_idx < m->needle_len - needle_idx) {
        score = 0.0;
        goto memoize;
    }

    for (long i = needle_idx; i < m->needle_len; i++)
    {
        char c = m->needle_p[i];
        if (c == '.')
            dot_search = 1;
        int found = 0;
        for (long j = haystack_idx; j < m->haystack_len; j++, haystack_idx++)
        {
            char d = m->haystack_p[j];
            if (d == '.')
            {
                if (j == 0 || m->haystack_p[j - 1] == '/')
                {
                    m->dot_file = 1;        // this is a dot-file
                    if (dot_search)         // and we are searching for a dot
                        dot_file_match = 1; // so this must be a match
                }
            }
            else if (d >= 'A' && d <= 'Z')
                d += 'a' - 'A'; // add 32 to downcase
            if (c == d)
            {
                found = 1;
                dot_search = 0;

                // calculate score
                double score_for_char = m->max_score_per_char;
                long distance = j - last_idx;
                if (distance > 1)
                {
                    double factor = 1.0;
                    char last = m->haystack_p[j - 1];
                    char curr = m->haystack_p[j]; // case matters, so get again
                    if (last == '/')
                        factor = 0.9;
                    else if (last == '-' ||
                            last == '_' ||
                            last == ' ' ||
                            (last >= '0' && last <= '9'))
                        factor = 0.8;
                    else if (last >= 'a' && last <= 'z' &&
                            curr >= 'A' && curr <= 'Z')
                        factor = 0.8;
                    else if (last == '.')
                        factor = 0.7;
                    else
                        // if no "special" chars behind char, factor diminishes
                        // as distance from last matched char increases
                        factor = (1.0 / distance) * 0.75;
                    score_for_char *= factor;
                }

                if (++j < m->haystack_len)
                {
                    // bump cursor one char to the right and
                    // use recursion to try and find a better match
                    double sub_score = recursive_match(m, j, i, last_idx, score);
                    if (sub_score > seen_score)
                        seen_score = sub_score;
                }

                score += score_for_char;
                last_idx = haystack_idx++;
                break;
            }
        }

        if (!found)
        {
            score = 0.0;
            goto memoize;
        }
    }

    if (m->dot_file)
    {
        if (m->never_show_dot_files ||
            (!dot_file_match && !m->always_show_dot_files)) {
            score = 0.0;
            goto memoize;
        }
    }
    score = score > seen_score ? score : seen_score;

memoize:
    return score;
}

// Match.new needle, string, options = {}
VALUE CommandTMatch_initialize(int argc, VALUE *argv, VALUE self)
{
    // process arguments: 2 mandatory, 1 optional
    VALUE str, needle, options;
    if (rb_scan_args(argc, argv, "21", &str, &needle, &options) == 2)
        options = Qnil;
    str             = StringValue(str);
    needle          = StringValue(needle); // already downcased by caller

    // check optional options hash for overrides
    VALUE always_show_dot_files = CommandT_option_from_hash("always_show_dot_files", options);
    VALUE never_show_dot_files = CommandT_option_from_hash("never_show_dot_files", options);

    matchinfo_t m;
    m.haystack_p            = RSTRING_PTR(str);
    m.haystack_len          = RSTRING_LEN(str);
    m.needle_p              = RSTRING_PTR(needle);
    m.needle_len            = RSTRING_LEN(needle);
    m.max_score_per_char    = (1.0 / m.haystack_len + 1.0 / m.needle_len) / 2;
    m.dot_file              = 0;
    m.always_show_dot_files = always_show_dot_files == Qtrue;
    m.never_show_dot_files  = never_show_dot_files == Qtrue;

    // calculate score
    double score = 1.0;
    if (m.needle_len == 0) // special case for zero-length search string
    {
        // filter out dot files
        if (!m.always_show_dot_files)
        {
            for (long i = 0; i < m.haystack_len; i++)
            {
                char c = m.haystack_p[i];
                if (c == '.' && (i == 0 || m.haystack_p[i - 1] == '/'))
                {
                    score = 0.0;
                    break;
                }
            }
        }
    }
    else // normal case
        score = recursive_match(&m, 0, 0, 0, 0.0);

    // clean-up and final book-keeping
    rb_iv_set(self, "@score", rb_float_new(score));
    rb_iv_set(self, "@str", str);
    return Qnil;
}

VALUE CommandTMatch_matches(VALUE self)
{
    double score = NUM2DBL(rb_iv_get(self, "@score"));
    return score > 0 ? Qtrue : Qfalse;
}

VALUE CommandTMatch_to_s(VALUE self)
{
    return rb_iv_get(self, "@str");
}
