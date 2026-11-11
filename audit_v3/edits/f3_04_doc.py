# FILE3-04 (documentation half): fm_text.h advertises "arbitrarily large
# files" for the streaming rewrites and says nothing about the per-line
# buffer, so a caller cannot know that a line longer than
# FM_INTERNAL_BUFFER_SIZE - 1 is refused.  Measure first (do not guess):
# Text_Replace / Text_InsertLine / Text_DeleteLine all use
# char line[FM_INTERNAL_BUFFER_SIZE] and pass the buffer size to
# Text_ReadLine, which fills at most line_size - 1 characters -- the same
# 511-character limit.  The header now states it, and states what
# CountLines/Find do at that limit (which the code now does).
import io

p = 'include/fm_text.h'
s = io.open(p, 'r', encoding='utf-8', newline='').read()

old = """/* Streaming replace: every occurrence of old_sub in each line is
 * replaced by new_sub.  Works on arbitrarily large files. */
FM_Result Text_Replace(const char *path, const char *old_sub,
                       const char *new_sub);

/* Insert / delete a line (1-based). */
FM_Result Text_InsertLine(const char *path, size_t at_line, const char *line);
FM_Result Text_DeleteLine(const char *path, size_t line);

/* Count lines in a file. */
FM_Result Text_CountLines(const char *path, size_t *count);
"""

new = """/* Streaming replace: every occurrence of old_sub in each line is
 * replaced by new_sub.  Works on arbitrarily large files, one line at a
 * time -- but not on arbitrarily long lines: a line that does not fit
 * the internal line buffer (FM_INTERNAL_BUFFER_SIZE - 1 characters, 511
 * by default) makes the whole call fail with FM_ERR_TOO_LARGE and the
 * file is left as it was.  See \"Line length\" below. */
FM_Result Text_Replace(const char *path, const char *old_sub,
                       const char *new_sub);

/* Insert / delete a line (1-based).  Same line-length limit:
 * FM_ERR_TOO_LARGE, file unchanged. */
FM_Result Text_InsertLine(const char *path, size_t at_line, const char *line);
FM_Result Text_DeleteLine(const char *path, size_t line);

/* Count lines in a file.  Does not stop at a long line: a line longer
 * than the internal buffer is counted (its tail is not examined, which
 * counting does not need), so the count is exact for any input. */
FM_Result Text_CountLines(const char *path, size_t *count);

/* Line length.  Text_Replace / Text_InsertLine / Text_DeleteLine rewrite
 * the file through a line buffer of FM_INTERNAL_BUFFER_SIZE bytes and
 * refuse a line of FM_INTERNAL_BUFFER_SIZE characters or more with
 * FM_ERR_TOO_LARGE, leaving the file untouched -- they never truncate.
 * Text_Find searches the part of a long line that fits; an occurrence
 * past that point is not found (the call still returns FM_ERR_NOT_FOUND
 * rather than FM_ERR_TOO_LARGE).  Raise FM_INTERNAL_BUFFER_SIZE for
 * longer lines, at the cost of that many bytes of stack per call. */
"""

old = old.replace('\n', '\r\n')
new = new.replace('\n', '\r\n')

if old not in s:
    raise SystemExit('f3_04_doc: needle not found in %s' % p)
if s.count(old) != 1:
    raise SystemExit('f3_04_doc: needle is not unique in %s' % p)

io.open(p, 'w', encoding='utf-8', newline='').write(s.replace(old, new, 1))
print('f3_04_doc: patched %s' % p)

# ------------------------------------------------------------------ #
# README.md: the same limit, where the other "file rules" live       #
# ------------------------------------------------------------------ #
p = 'README.md'
s = io.open(p, 'r', encoding='utf-8', newline='').read()

old = """* **`Text_ReadLine`** removes the `\\r` of a `\\r\\n` line end; a `\\r`
  anywhere else is part of the line.
"""
new = """* **`Text_ReadLine`** removes the `\\r` of a `\\r\\n` line end; a `\\r`
  anywhere else is part of the line.
* **Long lines.** `Text_Replace`, `Text_InsertLine` and `Text_DeleteLine`
  rewrite the file through a 512-byte line buffer
  (`FM_INTERNAL_BUFFER_SIZE`) and answer `FM_ERR_TOO_LARGE` for a line of
  511 characters or more, leaving the file as it was.  `Text_CountLines`
  counts long lines and `Text_Find` searches the part of one that fits.
"""

old = old.replace('\n', '\r\n')
new = new.replace('\n', '\r\n')
if old not in s:
    raise SystemExit('f3_04_doc: README needle not found')
if s.count(old) != 1:
    raise SystemExit('f3_04_doc: README needle is not unique')
io.open(p, 'w', encoding='utf-8', newline='').write(s.replace(old, new, 1))
print('f3_04_doc: patched %s' % p)
