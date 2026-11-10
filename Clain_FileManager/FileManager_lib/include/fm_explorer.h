/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_explorer.h - File explorer / navigation system
 *
 * Maintains a cached listing of the current directory and lets the UI
 * navigate with Enter / Up / Back.  Ideal for UART / display menus.
 */

#ifndef FM_EXPLORER_H
#define FM_EXPLORER_H

#include "fm_types.h"
#include "fm_directory.h"

FM_BEGIN_DECLS

typedef enum
{
    FM_EXPLORER_SORT_NONE = 0,
    FM_EXPLORER_SORT_NAME = 1,
    FM_EXPLORER_SORT_SIZE = 2
} FM_ExplorerSort;

typedef struct
{
    char      cwd[FM_PATH_MAX];
    FM_DirEntry items[FM_MAX_EXPLORER_ITEMS];
    uint32_t  count;              /* valid entries                      */
    uint32_t  cursor;             /* currently selected entry           */
    FM_Bool   only_files;
    FM_Bool   only_dirs;
    char      filter_ext[16];     /* "" = no extension filter           */
    FM_ExplorerSort sort;
    char      history[FM_EXPLORER_MAX_DEPTH][FM_PATH_MAX];
    uint32_t  depth;              /* history depth (0 = at root)        */
} FM_Explorer;

FM_Result Explorer_Open(FM_Explorer *ex, const char *path);
FM_Result Explorer_Refresh(FM_Explorer *ex);
FM_Result Explorer_Up(FM_Explorer *ex);                 /* to parent   */
FM_Result Explorer_Enter(FM_Explorer *ex);              /* into cursor */
FM_Result Explorer_Back(FM_Explorer *ex);               /* history     */
FM_Result Explorer_Next(FM_Explorer *ex);               /* move cursor */
FM_Result Explorer_Prev(FM_Explorer *ex);

FM_Result Explorer_GetCurrentPath(const FM_Explorer *ex, char *out,
                                  size_t out_size);
const FM_DirEntry *Explorer_GetCurrentItem(const FM_Explorer *ex);
const FM_DirEntry *Explorer_GetItem(const FM_Explorer *ex, uint32_t index);
uint32_t Explorer_GetItemCount(const FM_Explorer *ex);
FM_Bool  Explorer_IsDir(const FM_Explorer *ex, uint32_t index);

FM_Result Explorer_SetFilters(FM_Explorer *ex, FM_Bool only_files,
                              FM_Bool only_dirs, const char *ext,
                              FM_ExplorerSort sort);
FM_Result Explorer_Select(FM_Explorer *ex, uint32_t index);

FM_END_DECLS

#endif /* FM_EXPLORER_H */
