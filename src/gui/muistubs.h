/*
 * muistubs.h - muimaster.library calls for vbcc (register inlines, offsets
 * from muimaster_lib.fd) and the varargs helpers the MUI macros use.
 * Only functions available since MUI 3.8 are used.
 */
#ifndef A26_MUISTUBS_H
#define A26_MUISTUBS_H

#include <exec/types.h>
#include <utility/tagitem.h>
#include <intuition/classes.h>
#include <libraries/mui.h>

extern struct Library *MUIMasterBase;

Object *__MUI_NewObjectA(__reg("a6") struct Library *, __reg("a0") CONST_STRPTR cl,
                         __reg("a1") struct TagItem *tags) = "\tjsr\t-30(a6)";
#define MUI_NewObjectA(cl, tags) __MUI_NewObjectA(MUIMasterBase, (cl), (tags))

VOID __MUI_DisposeObject(__reg("a6") struct Library *, __reg("a0") Object *obj) = "\tjsr\t-36(a6)";
#define MUI_DisposeObject(obj) __MUI_DisposeObject(MUIMasterBase, (obj))

LONG __MUI_RequestA(__reg("a6") struct Library *, __reg("d0") APTR app, __reg("d1") APTR win,
                    __reg("d2") ULONG flags, __reg("a0") CONST_STRPTR title,
                    __reg("a1") CONST_STRPTR gadgets, __reg("a2") CONST_STRPTR format,
                    __reg("a3") APTR params) = "\tjsr\t-42(a6)";
#define MUI_RequestA(app, win, flags, title, gad, fmt, par) \
    __MUI_RequestA(MUIMasterBase, (app), (win), (flags), (title), (gad), (fmt), (par))

APTR __MUI_AllocAslRequest(__reg("a6") struct Library *, __reg("d0") ULONG type,
                           __reg("a0") struct TagItem *tags) = "\tjsr\t-48(a6)";
#define MUI_AllocAslRequest(type, tags) __MUI_AllocAslRequest(MUIMasterBase, (type), (tags))

BOOL __MUI_AslRequest(__reg("a6") struct Library *, __reg("a0") APTR req,
                      __reg("a1") struct TagItem *tags) = "\tjsr\t-54(a6)";
#define MUI_AslRequest(req, tags) __MUI_AslRequest(MUIMasterBase, (req), (tags))

VOID __MUI_FreeAslRequest(__reg("a6") struct Library *, __reg("a0") APTR req) = "\tjsr\t-60(a6)";
#define MUI_FreeAslRequest(req) __MUI_FreeAslRequest(MUIMasterBase, (req))

VOID __MUI_Redraw(__reg("a6") struct Library *, __reg("a0") Object *obj,
                  __reg("d0") ULONG flags) = "\tjsr\t-102(a6)";
#define MUI_Redraw(obj, flags) __MUI_Redraw(MUIMasterBase, (obj), (flags))

struct MUI_CustomClass *__MUI_CreateCustomClass(__reg("a6") struct Library *,
        __reg("a0") struct Library *base, __reg("a1") CONST_STRPTR supername,
        __reg("a2") struct MUI_CustomClass *supermcc, __reg("d0") LONG datasize,
        __reg("a3") APTR dispatcher) = "\tjsr\t-108(a6)";
#define MUI_CreateCustomClass(b, sn, smcc, ds, disp) \
    __MUI_CreateCustomClass(MUIMasterBase, (b), (sn), (smcc), (ds), (disp))

BOOL __MUI_DeleteCustomClass(__reg("a6") struct Library *,
                             __reg("a0") struct MUI_CustomClass *mcc) = "\tjsr\t-114(a6)";
#define MUI_DeleteCustomClass(mcc) __MUI_DeleteCustomClass(MUIMasterBase, (mcc))

Object *__MUI_MakeObjectA(__reg("a6") struct Library *, __reg("d0") LONG type,
                          __reg("a0") ULONG *params) = "\tjsr\t-120(a6)";
#define MUI_MakeObjectA(type, params) __MUI_MakeObjectA(MUIMasterBase, (type), (params))

/* varargs versions (arguments are contiguous on the stack with vbcc) */
Object *MUI_NewObject(CONST_STRPTR cl, Tag tag1, ...);
Object *MUI_MakeObject(LONG type, ...);
LONG    MUI_Request(APTR app, APTR win, ULONG flags, CONST_STRPTR title,
                    CONST_STRPTR gadgets, CONST_STRPTR format, ...);

#endif
