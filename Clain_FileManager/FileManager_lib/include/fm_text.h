/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_text.h - Text file API (ASCII / UTF-8 line based)
 *
 * Line based operations stream through the file and never require the
 * whole file in RAM.  Lines are terminated with '\n'.  On read, a '\r'
 * directly before the '\n' is removed with it (CRLF files); a '\r'
 * anywhere else is part of the line and is kept.
 */

#ifndef FM_TEXT_H
#define FM_TEXT_H

#include "fm_types.h"
#include "fm_file.h"

FM_BEGIN_DECLS

/* Read one line from an open file.  *read = chars excluding the '\n'
 * and NUL.  Returns FM_ERR_EOF when the end of file is reached. */
FM_Result Text_ReadLine(FM_File *f, char *line, size_t line_size,
                        size_t *read);

/* Write one line (appends '\n'). */
FM_Result Text_WriteLine(FM_File *f, const char *line);

/* Whole-file operations */
FM_Result Text_ReadAll(const char *path, char *buffer, size_t buffer_size,
                       size_t *read);
FM_Result Text_WriteAll(const char *path, const char *text);
FM_Result Text_AppendLine(const char *path, const char *line);

/* Find the first line containing needle; *line_out is 1-based.
 * Returns FM_ERR_NOT_FOUND when absent. */
FM_Result Text_Find(const char *path, const char *needle, size_t *line_out);

/* Streaming replace: every occurrence of old_sub in each line is
 * replaced by new_sub.  Works on arbitrarily large files.
 *
 * Text_Replace, Text_InsertLine and Text_DeleteLine write the new text
 * to "<path>.fm_tmp" and rename it over the file.  A path so long that
 * this name does not fit in FM_PATH_MAX (longer than FM_PATH_MAX - 8
 * characters) is FM_ERR_TOO_LARGE and nothing is touched.  (The name
 * used to be cut to fit: the temporary file then WAS the file being
 * edited, and the edit emptied it.) */
FM_Result Text_Replace(const char *path, const char *old_sub,
                       const char *new_sub);

/* Insert / delete a line (1-based). */
FM_Result Text_InsertLine(const char *path, size_t at_line, const char *line);
FM_Result Text_DeleteLine(const char *path, size_t line);

/* Count lines in a file. */
FM_Result Text_CountLines(const char *path, size_t *count);

FM_END_DECLS

#endif /* FM_TEXT_H */
