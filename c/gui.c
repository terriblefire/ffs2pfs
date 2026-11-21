/* ffs2pfs for AmigaOS: the window (GadTools; Kickstart 2.04 and up).
 *
 * Lists the partitions of every RDB disk that has a partition in the DOS list,
 * with where each one stands (f2p_probe: reads only), and runs ffs2pfs on the
 * one picked: Analyse, Convert (which also carries on a conversion that was cut
 * off) and Roll back. A run is a second process doing what the Shell command
 * does (ffs2pfs_run); its messages and progress come back here. Stop sends it
 * Ctrl-C, so it stops at the next safe point, as in the Shell.
 *
 * Keys: cursor up/down picks a partition; A, C, R, S, L as underlined; Esc closes. */
#include <exec/exec.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <dos/dostags.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <libraries/asl.h>
#include <graphics/gfxbase.h>
#include <graphics/text.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
#include <proto/asl.h>
#include <clib/alib_protos.h>
#include <stdio.h>
#include <string.h>
#include "amiga.h"

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *GadToolsBase, *AslBase;

#define COLS 62                          /* characters in a list line */
#define PLINES 6                         /* partition list lines, at most */
#define LLINES 12                        /* log lines, at most */
#define MAXLOG 1000                      /* log lines kept */
#define MAXROWS 48

/* ------------------------------------------------------------------ the partitions */
typedef struct {
    struct Node node;                    /* the list line: ln_Name = line */
    char line[COLS + 2];
    char name[32];                       /* the DOS device name without the colon ("" if the PART block is bad) */
    char vol[32];                        /* volume name, if mounted */
    char dev[64];
    u32 unit, flags;
    int mounted, inuse, boot, progdir;   /* in the DOS list; has a volume; SYS:; ffs2pfs runs from it */
    f2p_part pp;
} prow;

typedef struct {                         /* a DOS list entry of a disk device, copied out */
    char name[32], dev[64];
    u32 unit, flags, memtype, maxtr, lowcyl;
    struct MsgPort *task;
} dent;

static prow rows[MAXROWS];
static int nrows, sel = -1;
static struct List plist;

/* row i, or NULL. Always through here: vbcc -O1 miscompiles "prow *r = &rows[i]" when the
   first use is r->pp.x (it keeps &rows[i].pp and reads the other fields from there) */
static prow *row(int i)
{
    return i >= 0 && i < nrows ? &rows[i] : NULL;
}

static void bstr(char *out, BSTR b, int max)
{
    UBYTE *s = BADDR(b);
    int n = s[0] < max - 1 ? s[0] : max - 1;
    memcpy(out, s + 1, n);
    out[n] = 0;
}

static struct MsgPort *lock_task(BPTR l)
{
    return l ? ((struct FileLock *)BADDR(l))->fl_Task : NULL;
}

static int is_dos(u32 dt)
{
    return (dt & 0xffffff00UL) == 0x444f5300UL;
}

/* what ffs2pfs converts: DOS\0 .. DOS\5 */
static int convertible(const prow *r)
{
    return r->pp.state == P_NONE && is_dos(r->pp.p.dostype) && (r->pp.p.dostype & 0xff) <= 5;
}

static const char *fsname(u32 dt)
{
    static const char *dos[] = { "OFS", "FFS", "OFS-IN", "FFS-IN", "OFS-DC", "FFS-DC", "OFS-LN", "FFS-LN" };
    static char other[8];
    int i;
    if (is_dos(dt) && (dt & 0xff) < 8)
        return dos[dt & 0xff];
    if (dt == DOSTYPE_PFS3 || dt == ID_PFS1 || dt == 0x50445303UL)   /* PFS\3, PFS\1, PDS\3 */
        return "PFS3";
    for (i = 0; i < 3; i++) {
        UBYTE c = (UBYTE)(dt >> (24 - 8 * i));
        other[i] = c > ' ' && c < 127 ? c : '?';
    }
    sprintf(other + 3, "\\%lu", UL(dt & 0xff));
    return other;
}

static void size_text(char *out, u32 blocks)
{
    u32 mb = blocks / 2048;
    if (mb >= 10240)
        sprintf(out, "%lu GB", UL(mb / 1024));
    else if (mb >= 1024)
        sprintf(out, "%lu.%lu GB", UL(mb / 1024), UL(mb % 1024 * 10 / 1024));
    else
        sprintf(out, "%lu MB", UL(mb));
}

static const char *state_text(const prow *r)
{
    static char s[24];
    switch (r->pp.state) {
    case P_NONE:
        return !convertible(r) ? "not OFS/FFS" : r->boot ? "boot volume" : "can be converted";
    case P_PFS3:
        return "";
    case P_STOPPED:
        sprintf(s, "stopped at step %lu/6", UL(r->pp.step));
        return s;
    case P_UNDO:
        return "rollback cut off";
    case P_FINISH:
        return "last write left";
    }
    return "cannot be read";
}

static void make_line(prow *r)
{
    char size[16];
    size_text(size, r->pp.p.blocks);
    snprintf(r->line, sizeof(r->line), "%-6.6s %-14.14s %-6.6s %8s  %s", r->name[0] ? r->name : "?",
             r->inuse ? r->vol : r->mounted ? "-" : "(not mounted)", fsname(r->pp.orig_dostype), size, state_text(r));
    r->node.ln_Name = r->line;
}

/* every RDB disk with a partition in the DOS list: its partitions, and where they stand */
static void scan(void)
{
    static dent d[64];
    struct DosList *dl;
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;
    struct MsgPort *sys = NULL, *pd = NULL;
    BPTR l;
    int nd = 0, i, j, n;
    NewList(&plist);
    nrows = 0;
    dl = LockDosList(LDF_DEVICES | LDF_READ);
    while ((dl = NextDosEntry(dl, LDF_DEVICES)) && nd < 64) {
        struct FileSysStartupMsg *fssm = BADDR(dl->dol_misc.dol_handler.dol_Startup);
        struct DosEnvec *env;
        if ((ULONG)fssm <= 1024 || !TypeOfMem(fssm) || !TypeOfMem(BADDR(fssm->fssm_Device)))
            continue;
        env = BADDR(fssm->fssm_Environ);
        if (!TypeOfMem(env) || env->de_TableSize < DE_DOSTYPE || env->de_TableSize > 64)
            continue;
        bstr(d[nd].name, dl->dol_Name, sizeof(d[nd].name));
        bstr(d[nd].dev, fssm->fssm_Device, sizeof(d[nd].dev));
        d[nd].unit = fssm->fssm_Unit;
        d[nd].flags = fssm->fssm_Flags;
        d[nd].memtype = env->de_TableSize >= DE_MEMBUFTYPE ? env->de_BufMemType : MEMF_PUBLIC;
        d[nd].maxtr = env->de_TableSize >= DE_MAXTRANSFER ? env->de_MaxTransfer : 0x1fe00;
        d[nd].lowcyl = env->de_LowCyl;
        d[nd].task = dl->dol_Task;
        nd++;
    }
    UnLockDosList(LDF_DEVICES | LDF_READ);
    me->pr_WindowPtr = (APTR)-1;               /* no requesters for SYS: */
    if ((l = Lock((STRPTR)"SYS:", SHARED_LOCK))) {
        sys = lock_task(l);
        UnLock(l);
    }
    me->pr_WindowPtr = oldwin;
    pd = lock_task(GetProgramDir());
    for (i = 0; i < nd; i++) {
        disk k;
        blockdev bd;
        f2p_part pp[32];
        /* each disk once; floppies, RAD: and the like (from cylinder 0, no RDB) are left out */
        for (j = 0; j < i && (d[j].unit != d[i].unit || strcmp(d[j].dev, d[i].dev)); j++)
            ;
        if (j < i || !d[i].lowcyl)
            continue;
        if (disk_open(&k, d[i].dev, d[i].unit, d[i].flags, d[i].memtype, d[i].maxtr)) {
            disk_close(&k);
            continue;
        }
        disk_blockdev(&k, &bd);
        n = f2p_probe(&bd, 0, pp, 32);       /* a disk with a torn RDSK mounts nothing: not here */
        for (j = 0; j < n && nrows < MAXROWS; j++) {
            prow *r = &rows[nrows++];
            int x;
            memset(r, 0, sizeof(*r));
            r->pp = pp[j];
            if (!pp[j].p.invalid)
                strcpy(r->name, pp[j].p.name);
            strcpy(r->dev, d[i].dev);
            r->unit = d[i].unit;
            r->flags = d[i].flags;
            for (x = 0; x < nd; x++)
                if (r->name[0] && !strcmp(d[x].name, r->name) && d[x].unit == r->unit && !strcmp(d[x].dev, r->dev)) {
                    r->mounted = 1;
                    if (d[x].task) {
                        struct DosList *v = LockDosList(LDF_VOLUMES | LDF_READ);
                        while ((v = NextDosEntry(v, LDF_VOLUMES)))
                            if (v->dol_Task == d[x].task) {
                                bstr(r->vol, v->dol_Name, sizeof(r->vol));
                                r->inuse = 1;
                                break;
                            }
                        UnLockDosList(LDF_VOLUMES | LDF_READ);
                        r->boot = sys == d[x].task;
                        r->progdir = pd == d[x].task;
                    }
                }
            make_line(r);
            AddTail(&plist, &r->node);
        }
        disk_close(&k);
    }
}

/* ------------------------------------------------------------------ the run (a second process) */
#define QN 64
static struct SignalSemaphore sem;
static struct Task *gui_task;
static ULONG gui_sig;
static char q[QN][200];                  /* message lines from the run, not yet in the log */
static int qhead, qtail;
static struct {
    const char *step;
    u32 done, total;
    char item[80];
    int changed;
} pg;
static unsigned long long pg_sent;
static volatile int w_done;
static int w_rc, running, run_mode, run_row, run_remounted;
static struct Process *worker;
static runopts w_opts;

static unsigned long long now_ticks(void)
{
    struct DateStamp ds;
    DateStamp(&ds);
    return ((unsigned long long)ds.ds_Days * 1440 + ds.ds_Minute) * 3000 + ds.ds_Tick;
}

static void w_log(const char *text)
{
    while (*text) {
        const char *e = strchr(text, '\n');
        int n = e ? (int)(e - text) : (int)strlen(text);
        for (;;) {
            ObtainSemaphore(&sem);
            if ((qhead + 1) % QN != qtail)
                break;
            ReleaseSemaphore(&sem);
            Signal(gui_task, gui_sig);
            Delay(2);                           /* the window is behind: let it catch up */
        }
        if (n > (int)sizeof(q[0]) - 1)
            n = sizeof(q[0]) - 1;
        memcpy(q[qhead], text, n);
        q[qhead][n] = 0;
        qhead = (qhead + 1) % QN;
        ReleaseSemaphore(&sem);
        text += e ? n + 1 : n;
    }
    Signal(gui_task, gui_sig);
}

static void w_progress(const char *step, u32 done, u32 total, const char *item)
{
    unsigned long long t = now_ticks();
    int news;
    ObtainSemaphore(&sem);
    news = step != pg.step;
    pg.step = step;
    pg.done = done;
    pg.total = total;
    strncpy(pg.item, item ? item : "", sizeof(pg.item) - 1);
    pg.item[sizeof(pg.item) - 1] = 0;
    pg.changed = 1;
    ReleaseSemaphore(&sem);
    if (news || done == total || t - pg_sent >= 5) {   /* ten times a second at most */
        pg_sent = t;
        Signal(gui_task, gui_sig);
    }
}

static void w_entry(void)
{
    w_rc = ffs2pfs_run(&w_opts);
    Forbid();                                   /* the window may go only once this process has */
    worker = NULL;
    w_done = 1;
    Signal(gui_task, gui_sig);
}

/* ------------------------------------------------------------------ the window */
enum { G_PARTS, G_ANALYSE, G_CONVERT, G_ROLLBACK, G_STOP, G_LOG, G_SAVE, G_N };
static struct Window *win;
static struct Screen *scr;
static struct DrawInfo *dri;
static APTR vi;
static struct Gadget *glist, *gad[G_N];
static struct TextFont *tfont;           /* the screen font: labels, buttons, texts */
static struct TextAttr tattr, mattr;     /* mattr: the system default font (fixed width): the lists */
static int fh;                           /* text height */
static struct { int x, y, w, h; } r_plabel, r_info, r_step, r_bar, r_item, r_llabel;
static struct List loglist;
static int nlog;
static char step_text[120], end_text[80];
static const char *bar_step;
static unsigned long long bar_t0;
static char wtitle[80];
static int plines, llines, win_top;      /* list lines: as many as the screen has room for */
static int mw, mh;                       /* the list font */

static UWORD pen(int i)
{
    return dri->dri_Pens[i];
}

static void put_text(int x, int y, int w, const char *s, int p)
{
    struct RastPort *rp = win->RPort;
    struct TextExtent te;
    ULONG n;
    SetFont(rp, tfont);
    SetAPen(rp, pen(BACKGROUNDPEN));
    RectFill(rp, x, y, x + w - 1, y + fh - 1);
    if (!s || !*s)
        return;
    n = TextFit(rp, (STRPTR)s, strlen(s), &te, NULL, 1, w, fh + 1);
    SetAPen(rp, pen(p));
    SetDrMd(rp, JAM1);
    Move(rp, x, y + rp->TxBaseline);
    Text(rp, (STRPTR)s, n);
}

static void draw_info(void)
{
    char a[120], b[120], size[16];
    prow *r = row(sel);
    a[0] = b[0] = 0;
    if (!nrows)
        strcpy(a, "No partitions found on a disk with an RDB.");
    else if (!r)
        strcpy(a, "Pick a partition.");
    else {
        size_text(size, r->pp.p.blocks);
        snprintf(a, sizeof(a), "%s:%s%s, %s, %s on %s unit %lu%s", r->name[0] ? r->name : "?", r->inuse ? " " : "",
                 r->inuse ? r->vol : "", fsname(r->pp.orig_dostype), size, r->dev, UL(r->unit), r->mounted ? "" : " (not mounted)");
        switch (r->pp.state) {
        case P_NONE:
            strcpy(b, !convertible(r) ? "Not OFS or FFS: nothing to convert."
                      : !r->boot ? "Analyse only reads. Convert makes it PFS3, keeping all files."
                      : r->progdir ? "The boot volume: copy ffs2pfs to RAM: and start it from there."
                      : "The boot volume: close what uses it, Convert, then reboot.");
            break;
        case P_PFS3:
            strcpy(b, "PFS3 already: nothing to convert.");
            break;
        case P_STOPPED:
            sprintf(b, "Stopped at step %lu of 6: Convert goes on, Roll back undoes it.", UL(r->pp.step));
            break;
        case P_UNDO:
            strcpy(b, "A rollback was cut off: Roll back finishes it.");
            break;
        case P_FINISH:
            strcpy(b, "Only the last write is left: Convert finishes it.");
            break;
        default:
            snprintf(b, sizeof(b), "Cannot be read: %s", r->pp.why);
        }
    }
    put_text(r_info.x + 6, r_info.y + 3, r_info.w - 12, a, TEXTPEN);
    put_text(r_info.x + 6, r_info.y + 4 + fh, r_info.w - 12, b, TEXTPEN);
}

static void draw_progress(void)
{
    struct RastPort *rp = win->RPort;
    int x = r_bar.x + 2, y = r_bar.y + 1, w = r_bar.w - 4, h = r_bar.h - 2, fill = 0, tl;
    char t[120], eta[24] = "", item[100];
    u32 done, total;
    const char *step;
    ObtainSemaphore(&sem);
    step = pg.step;
    done = pg.done;
    total = pg.total;
    strcpy(item, pg.item);
    pg.changed = 0;
    ReleaseSemaphore(&sem);
    if (step != bar_step) {
        bar_step = step;
        bar_t0 = now_ticks();
    }
    t[0] = 0;
    if (step && total) {
        unsigned long long el = now_ticks() - bar_t0;
        fill = (int)((unsigned long long)w * done / total);
        if (done && done < total && el > 100) {
            unsigned long long left = el / 50 * (total - done) / done;
            sprintf(eta, ", %lu:%02lu left", UL(left / 60), UL(left % 60));
        }
        sprintf(t, "%s %lu%%%s", step, UL((unsigned long long)done * 100 / total), eta);
    } else if (step)
        sprintf(t, "%s %lu", step, UL(done));
    else
        strcpy(t, end_text);
    if (fill > w)
        fill = w;
    SetAPen(rp, pen(FILLPEN));
    if (fill)
        RectFill(rp, x, y, x + fill - 1, y + h - 1);
    SetAPen(rp, pen(BACKGROUNDPEN));
    if (fill < w)
        RectFill(rp, x + fill, y, x + w - 1, y + h - 1);
    tl = TextLength(rp, (STRPTR)t, strlen(t));
    if (tl > w)
        tl = w;
    SetAPen(rp, pen(TEXTPEN));
    SetDrMd(rp, JAM1);
    if (*t) {
        struct TextExtent te;
        ULONG n = TextFit(rp, (STRPTR)t, strlen(t), &te, NULL, 1, w, fh + 1);
        Move(rp, x + (w - tl) / 2, y + (h - fh) / 2 + rp->TxBaseline);
        Text(rp, (STRPTR)t, n);
    }
    put_text(r_step.x, r_step.y, r_step.w, step_text, step_text[0] ? HIGHLIGHTTEXTPEN : TEXTPEN);
    put_text(r_item.x, r_item.y, r_item.w, step ? item : "", TEXTPEN);
}

static void draw_all(void)
{
    struct RastPort *rp = win->RPort;
    SetFont(rp, tfont);
    put_text(r_plabel.x, r_plabel.y, r_plabel.w, "Partitions", HIGHLIGHTTEXTPEN);
    put_text(r_llabel.x, r_llabel.y, r_llabel.w, "Log", HIGHLIGHTTEXTPEN);
    DrawBevelBox(rp, r_info.x, r_info.y, r_info.w, r_info.h, GT_VisualInfo, (ULONG)vi, GTBB_Recessed, TRUE, TAG_END);
    DrawBevelBox(rp, r_bar.x, r_bar.y, r_bar.w, r_bar.h, GT_VisualInfo, (ULONG)vi, GTBB_Recessed, TRUE, TAG_END);
    draw_info();
    draw_progress();
}

static void disable(int g, int off)
{
    GT_SetGadgetAttrs(gad[g], win, NULL, GA_Disabled, off ? TRUE : FALSE, TAG_END);
}

static void set_gadgets(void)
{
    prow *r = row(sel);
    int st = r ? r->pp.state : -1;
    disable(G_ANALYSE, running || !r || !convertible(r));
    disable(G_CONVERT, running || !r || !((convertible(r) && !(r->boot && r->progdir)) || st == P_STOPPED
                                          || st == P_FINISH || st == P_BAD));
    disable(G_ROLLBACK, running || !r || !(st == P_STOPPED || st == P_UNDO || st == P_BAD));
    disable(G_STOP, !running);
    disable(G_SAVE, running || !nlog);
    if (GadToolsBase->lib_Version >= 39)       /* a disabled list view needs V39 */
        disable(G_PARTS, running);
}

/* one message line into the log; long lines wrap at a space */
static void log_line(const char *s)
{
    char part[COLS + 1];
    int first = 1;
    do {
        int n = strlen(s), ind = first ? 0 : 2, max = COLS - ind;
        struct Node *nd;
        if (n > max) {
            int k = max;
            while (k > max / 2 && s[k] != ' ')
                k--;
            n = s[k] == ' ' ? k : max;
        }
        memset(part, ' ', ind);
        memcpy(part + ind, s, n);
        part[ind + n] = 0;
        s += n;
        while (*s == ' ')
            s++;
        first = 0;
        if (!(nd = AllocVec(sizeof(struct Node) + strlen(part) + 1, MEMF_PUBLIC | MEMF_CLEAR)))
            return;
        nd->ln_Name = (char *)(nd + 1);
        strcpy(nd->ln_Name, part);
        AddTail(&loglist, nd);
        if (++nlog > MAXLOG) {
            FreeVec(RemHead(&loglist));
            nlog--;
        }
    } while (*s);
}

/* the log list is changed only while it is off its list view */
static void log_detach(void)
{
    GT_SetGadgetAttrs(gad[G_LOG], win, NULL, GTLV_Labels, ~0, TAG_END);
}

static void log_attach(void)
{
    GT_SetGadgetAttrs(gad[G_LOG], win, NULL, GTLV_Labels, (ULONG)&loglist, GTLV_Top, nlog > llines ? nlog - llines : 0,
                      GTLV_MakeVisible, nlog ? nlog - 1 : 0, TAG_END);
}

/* the run's message lines into the log; the step line from "Step n/6: ..." */
static void drain(void)
{
    int any;
    ObtainSemaphore(&sem);
    any = qtail != qhead;
    ReleaseSemaphore(&sem);
    if (!any) {
        if (pg.changed)
            draw_progress();
        return;
    }
    log_detach();
    ObtainSemaphore(&sem);
    while (qtail != qhead) {
        char *l = q[qtail];
        unsigned int n, of;
        if (sscanf(l, "Step %u/%u: ", &n, &of) == 2 && strchr(l, ':'))
            snprintf(step_text, sizeof(step_text), "Step %u of %u: %s", n, of, strchr(l, ':') + 2);
        else if (!strncmp(l, "Rolling back", 12) || !strncmp(l, "rolling back", 12) || !strncmp(l, "finishing the rollback", 22))
            strcpy(step_text, "Rolling back");
        if (strstr(l, " mounted as PFS3"))             /* remount() after the commit */
            run_remounted = 1;
        log_line(l);
        qtail = (qtail + 1) % QN;
    }
    ReleaseSemaphore(&sem);
    log_attach();
    draw_progress();
}

static int ask(const char *gadgets, const char *text)
{
    struct EasyStruct es;
    es.es_StructSize = sizeof(es);
    es.es_Flags = 0;
    es.es_Title = (UBYTE *)"ffs2pfs";
    es.es_TextFormat = (UBYTE *)"%s";
    es.es_GadgetFormat = (UBYTE *)gadgets;
    return EasyRequest(win, &es, NULL, (ULONG)text);
}

static void select_row(int i)
{
    if (i < 0 || i >= nrows)
        return;
    sel = i;
    GT_SetGadgetAttrs(gad[G_PARTS], win, NULL, GTLV_Selected, sel, GTLV_MakeVisible, sel,
                      GTLV_Top, sel >= plines ? sel - plines + 1 : 0, TAG_END);
    draw_info();
    set_gadgets();
}

/* the list again (after a run, the states have changed); the same partition stays picked */
static void rescan(void)
{
    char keep[32];
    int i;
    strcpy(keep, row(sel) ? row(sel)->name : "");
    GT_SetGadgetAttrs(gad[G_PARTS], win, NULL, GTLV_Labels, ~0, TAG_END);
    scan();
    GT_SetGadgetAttrs(gad[G_PARTS], win, NULL, GTLV_Labels, (ULONG)&plist, TAG_END);
    for (i = 0; i < nrows && strcmp(rows[i].name, keep); i++)
        ;
    sel = -1;
    select_row(i < nrows ? i : 0);
}

static void start(int mode)
{
    prow *r = row(sel);
    char t[400];
    const char *was, *n;
    if (!r)
        return;
    was = fsname(r->pp.orig_dostype);
    n = r->name[0] ? r->name : "the partition";
    if (mode == F2P_CONVERT && convertible(r)) {
        snprintf(t, sizeof(t), "Convert %s:%s%s to PFS3?\n\n"
                 "Every file is kept. If anything goes wrong,\n%s: is put back as it was.\n\n"
                 "Close the windows and programs that use it first:\nit is locked while ffs2pfs works on it.%s",
                 n, r->inuse ? " " : "", r->inuse ? r->vol : "", n,
                 r->boot ? "\n\nThis is the boot volume: reboot when it is done." : "");
        if (!ask("Convert|Cancel", t))
            return;
    } else if (mode == F2P_CONVERT) {
        snprintf(t, sizeof(t), "Carry on converting %s: to PFS3?", n);
        if (!ask("Carry on|Cancel", t))
            return;
    } else if (mode == F2P_ROLLBACK) {
        snprintf(t, sizeof(t), "Put %s: back to %s, as it was before the conversion?", n, was);
        if (!ask("Roll back|Cancel", t))
            return;
    }
    memset(&w_opts, 0, sizeof(w_opts));
    w_opts.part = r->name;
    w_opts.mode = mode;
    w_opts.crash_after = -1;
    w_opts.boot = r->boot;
    if (!r->mounted) {                         /* nothing mounts a partition being converted: the disk directly */
        w_opts.device = r->dev;
        w_opts.unit = r->unit;
    }
    ObtainSemaphore(&sem);
    memset(&pg, 0, sizeof(pg));
    ReleaseSemaphore(&sem);
    step_text[0] = end_text[0] = 0;
    strcpy(step_text, mode == F2P_DRY ? "Analysing" : mode == F2P_ROLLBACK ? "Rolling back" : "Converting");
    snprintf(t, sizeof(t), "--- %s %s:", mode == F2P_DRY ? "Analyse" : mode == F2P_ROLLBACK ? "Roll back" : "Convert", n);
    log_detach();
    if (nlog)
        log_line("");
    log_line(t);
    log_attach();
    run_mode = mode;
    run_row = sel;
    run_remounted = 0;
    w_done = 0;
    fe_log = w_log;
    fe_progress = w_progress;
    running = 1;
    set_gadgets();
    draw_progress();
    worker = CreateNewProcTags(NP_Entry, (ULONG)w_entry, NP_Name, (ULONG)"ffs2pfs run", NP_StackSize, 65536, TAG_END);
    if (!worker) {
        fe_log = NULL;
        fe_progress = NULL;
        running = 0;
        log_detach();
        log_line("cannot start the run: out of memory");
        log_attach();
        step_text[0] = 0;
        draw_progress();
        set_gadgets();
    }
}

static void finished(void)
{
    char t[400], n[40], was[12];
    prow *r = row(run_row);
    int rc = w_rc, boot = r && r->boot;
    strcpy(n, r && r->name[0] ? r->name : "the partition");
    strcpy(was, r ? fsname(r->pp.orig_dostype) : "FFS");
    fe_log = NULL;
    fe_progress = NULL;
    running = 0;
    drain();
    ObtainSemaphore(&sem);
    memset(&pg, 0, sizeof(pg));
    ReleaseSemaphore(&sem);
    strcpy(end_text, rc == 0 ? "Done" : rc == 5 ? "Stopped" : "Not done");
    step_text[0] = 0;
    draw_progress();
    rescan();                                   /* where it stands now */
    r = row(sel);
    if (rc == 5 && r && r->pp.state == P_STOPPED)
        snprintf(t, sizeof(t), "Stopped at step %lu of 6.\n\nConvert carries on from there;\nRoll back puts %s: back as it was.",
                 UL(r->pp.step), n);
    else if (rc == 5)
        snprintf(t, sizeof(t), "Stopped before anything was written:\n%s: is as it was.", n);
    else if (run_mode == F2P_DRY)
        snprintf(t, sizeof(t), rc ? "%s: cannot be converted:\n%s" : "%s: can be converted to PFS3.\n\nThe log has the details.",
                 n, run_why);
    else if (run_mode == F2P_ROLLBACK)
        snprintf(t, sizeof(t), rc ? "The rollback of %s: did not finish:\n%s\n\nRoll back again to finish it."
                                  : "%s: is %s again, as it was before.", n, rc ? run_why : was);
    else if (rc == 0)
        snprintf(t, sizeof(t), "%s: is PFS3 now.\n\n%s", n, boot ? "Reboot to start using it."
                 : run_remounted ? "It is mounted read-only until the next reboot." : "Reboot to use it.");
    else if (rc == 10)
        snprintf(t, sizeof(t), "Nothing was converted:\n%s\n\n%s: is as it was.", run_why, n);
    else
        snprintf(t, sizeof(t), "ffs2pfs did not finish:\n%s\n\nThe log has the details.", run_why[0] ? run_why : "an error");
    ask("OK", t);
}

static void stop(void)
{
    Forbid();
    if (worker)
        Signal((struct Task *)worker, SIGBREAKF_CTRL_C);
    Permit();
    strcpy(step_text, "Stopping at the next safe point...");
    draw_progress();
    disable(G_STOP, 1);
}

static void save_log(void)
{
    struct FileRequester *fr;
    struct Node *nd;
    char path[300];
    BPTR f;
    if (!AslBase && !(AslBase = OpenLibrary((STRPTR)"asl.library", 37)))
        return;
    fr = AllocAslRequestTags(ASL_FileRequest, ASLFR_Window, (ULONG)win, ASLFR_TitleText, (ULONG)"Save the log as",
                             ASLFR_InitialDrawer, (ULONG)"RAM:", ASLFR_InitialFile, (ULONG)"ffs2pfs.log",
                             ASLFR_DoSaveMode, TRUE, TAG_END);
    if (!fr)
        return;
    if (AslRequest(fr, NULL) && fr->fr_File[0]) {
        strncpy(path, (char *)fr->fr_Drawer, sizeof(path) - 1);
        path[sizeof(path) - 1] = 0;
        AddPart((STRPTR)path, fr->fr_File, sizeof(path));
        if ((f = Open((STRPTR)path, MODE_NEWFILE))) {
            for (nd = loglist.lh_Head; nd->ln_Succ; nd = nd->ln_Succ) {
                FPuts(f, (STRPTR)nd->ln_Name);
                FPutC(f, '\n');
            }
            Close(f);
        } else
            ask("OK", "Cannot write the log there.");
    }
    FreeAslRequest(fr);
}

static int text_len(const char *s)
{
    struct RastPort rp;
    char t[40];
    int n = 0;
    InitRastPort(&rp);
    SetFont(&rp, tfont);
    for (; *s && n < 39; s++)                   /* without the underscore */
        if (*s != '_')
            t[n++] = *s;
    return TextLength(&rp, (STRPTR)t, n);
}


/* everything placed for the two fonts; make: the gadgets too. The inner size in pw, ph */
static int place(int make, int *pw, int *ph)
{
    static const char *blab[] = { "_Analyse", "_Convert", "_Roll back" };
    struct NewGadget ng;
    struct Gadget *g = NULL;
    int L = scr->WBorLeft, T = scr->WBorTop + scr->Font->ta_YSize + 1, M = 6, S = 3, i;
    int bw = 0, bh, lw, W, y, sw, svw;
    fh = tfont->tf_YSize;
    bh = fh + 6;
    for (i = 0; i < 3; i++)
        if (text_len(blab[i]) + 20 > bw)
            bw = text_len(blab[i]) + 20;
    sw = text_len("_Stop") + 28;
    svw = text_len("Save _log...") + 20;
    lw = COLS * mw + 24;
    W = lw + 2 * M;
    if (W < 3 * bw + 2 * S + 2 * M)
        W = 3 * bw + 2 * S + 2 * M;
    lw = W - 2 * M;
    y = T + M;
    r_plabel.x = L + M; r_plabel.y = y; r_plabel.w = lw;
    y += fh + 2;
    memset(&ng, 0, sizeof(ng));
    ng.ng_VisualInfo = vi;
    if (make)
        g = CreateContext(&glist);
    ng.ng_TextAttr = &mattr;
    ng.ng_LeftEdge = L + M; ng.ng_TopEdge = y; ng.ng_Width = lw; ng.ng_Height = plines * mh + 4;
    ng.ng_GadgetID = G_PARTS;
    if (make)
        g = gad[G_PARTS] = CreateGadget(LISTVIEW_KIND, g, &ng, GTLV_Labels, (ULONG)&plist, GTLV_ShowSelected, 0,
                                        GTLV_Selected, sel, TAG_END);
    y += ng.ng_Height + S;
    r_info.x = L + M; r_info.y = y; r_info.w = lw; r_info.h = 2 * fh + 6;
    y += r_info.h + S;
    ng.ng_TextAttr = &tattr;
    ng.ng_Flags = PLACETEXT_IN;
    for (i = 0; i < 3; i++) {
        ng.ng_LeftEdge = L + M + i * (bw + S); ng.ng_TopEdge = y; ng.ng_Width = bw; ng.ng_Height = bh;
        ng.ng_GadgetText = (UBYTE *)blab[i];
        ng.ng_GadgetID = G_ANALYSE + i;
        if (make)
            g = gad[G_ANALYSE + i] = CreateGadget(BUTTON_KIND, g, &ng, GT_Underscore, '_', TAG_END);
    }
    y += bh + 2 * S + 2;
    r_step.x = L + M; r_step.y = y; r_step.w = lw;
    y += fh + 3;
    r_bar.x = L + M; r_bar.y = y; r_bar.w = lw - sw - S; r_bar.h = bh;
    ng.ng_LeftEdge = L + M + lw - sw; ng.ng_TopEdge = y; ng.ng_Width = sw; ng.ng_Height = bh;
    ng.ng_GadgetText = (UBYTE *)"_Stop";
    ng.ng_GadgetID = G_STOP;
    if (make)
        g = gad[G_STOP] = CreateGadget(BUTTON_KIND, g, &ng, GT_Underscore, '_', GA_Disabled, TRUE, TAG_END);
    y += bh + 2;
    r_item.x = L + M; r_item.y = y; r_item.w = lw;
    y += fh + S + 2;
    r_llabel.x = L + M; r_llabel.y = y; r_llabel.w = lw - svw - S;
    ng.ng_LeftEdge = L + M + lw - svw; ng.ng_TopEdge = y - 3; ng.ng_Width = svw; ng.ng_Height = bh;
    ng.ng_GadgetText = (UBYTE *)"Save _log...";
    ng.ng_GadgetID = G_SAVE;
    if (make)
        g = gad[G_SAVE] = CreateGadget(BUTTON_KIND, g, &ng, GT_Underscore, '_', GA_Disabled, TRUE, TAG_END);
    y += bh - 1;
    ng.ng_TextAttr = &mattr;
    ng.ng_LeftEdge = L + M; ng.ng_TopEdge = y; ng.ng_Width = lw; ng.ng_Height = llines * mh + 4;
    ng.ng_GadgetText = NULL;
    ng.ng_GadgetID = G_LOG;
    if (make)
        g = gad[G_LOG] = CreateGadget(LISTVIEW_KIND, g, &ng, GTLV_Labels, (ULONG)&loglist, GTLV_ReadOnly, TRUE, TAG_END);
    y += ng.ng_Height + M;
    *pw = W;
    *ph = y - T;
    if (make && !g) {
        FreeGadgets(glist);
        glist = NULL;
        return 0;
    }
    return 1;
}

/* the list sizes for the screen, then the gadgets; 0: the window does not fit */
static int layout(int *pw, int *ph)
{
    struct TextFont *mf = OpenFont(&mattr);
    int T = scr->WBorTop + scr->Font->ta_YSize + 1, avail, t;
    if (!mf)
        return 0;
    mw = mf->tf_XSize;
    mh = mf->tf_YSize;
    CloseFont(mf);
    for (t = 0; t < 2; t++) {                  /* below the screen's title bar, or over it if need be */
        win_top = t ? 0 : scr->BarHeight + 1;
        avail = scr->Height - win_top - T - scr->WBorBottom;
        for (plines = PLINES; plines >= 3; plines--) {
            llines = 0;
            place(0, pw, ph);
            llines = (avail - *ph) / mh;
            if (llines >= 3)
                break;
        }
        if (llines >= 3)
            break;
    }
    if (llines < 3)
        return 0;
    if (llines > LLINES)
        llines = LLINES;
    if (*pw + scr->WBorLeft + scr->WBorRight > scr->Width || !place(1, pw, ph))
        return 0;
    return 1;
}

static int open_window(void)
{
    struct TextFont *df = GfxBase->DefaultFont;
    static struct TextAttr topaz = { (STRPTR)"topaz.font", 8, 0, 0 };
    int w, h, i;
    if (!(scr = LockPubScreen(NULL)) || !(vi = GetVisualInfo(scr, TAG_END)) || !(dri = GetScreenDrawInfo(scr)))
        return 0;
    tattr = *scr->Font;
    mattr.ta_Name = (STRPTR)df->tf_Message.mn_Node.ln_Name;
    mattr.ta_YSize = df->tf_YSize;
    mattr.ta_Style = df->tf_Style;
    mattr.ta_Flags = df->tf_Flags;
    for (i = 0; i < 2; i++) {                   /* the screen's fonts, or topaz 8 if they do not fit */
        if (i) {
            tattr = topaz;
            mattr = topaz;
        }
        if (!(tfont = OpenFont(&tattr)))
            continue;
        if (layout(&w, &h))
            break;
        CloseFont(tfont);
        tfont = NULL;
    }
    if (!tfont)
        return 0;
    {   /* "ffs2pfs 0.2: OFS/FFS to PFS3": the version string without its date */
        const char *v = ffs2pfs_version(), *d = strstr(v, " (");
        snprintf(wtitle, sizeof(wtitle), "%.*s: OFS/FFS to PFS3", d ? (int)(d - v) : (int)strlen(v), v);
    }
    win = OpenWindowTags(NULL, WA_Left, (scr->Width - w) / 2 > 0 ? (scr->Width - w) / 2 : 0,
                         WA_Top, win_top, WA_InnerWidth, w, WA_InnerHeight, h,
                         WA_Title, (ULONG)wtitle, WA_ScreenTitle, (ULONG)wtitle, WA_PubScreen, (ULONG)scr,
                         WA_Gadgets, (ULONG)glist, WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE,
                         WA_Activate, TRUE, WA_SmartRefresh, TRUE, WA_AutoAdjust, TRUE,
                         WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_VANILLAKEY | IDCMP_RAWKEY
                                   | LISTVIEWIDCMP | BUTTONIDCMP, TAG_END);
    if (!win)
        return 0;
    GT_RefreshWindow(win, NULL);
    draw_all();
    return 1;
}

static void close_window(void)
{
    if (win)
        CloseWindow(win);
    if (glist)
        FreeGadgets(glist);
    if (tfont)
        CloseFont(tfont);
    if (dri)
        FreeScreenDrawInfo(scr, dri);
    if (vi)
        FreeVisualInfo(vi);
    if (scr)
        UnlockPubScreen(NULL, scr);
}

int gui_main(void)
{
    BYTE bit;
    int quit = 0, i;
    struct Node *nd;
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 37);
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 37);
    GadToolsBase = OpenLibrary((STRPTR)"gadtools.library", 37);
    if (!IntuitionBase || !GfxBase || !GadToolsBase || (bit = AllocSignal(-1)) < 0) {
        if (GadToolsBase) CloseLibrary(GadToolsBase);
        if (GfxBase) CloseLibrary((struct Library *)GfxBase);
        if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
        return 20;                              /* needs Kickstart 2.04 */
    }
    gui_task = FindTask(NULL);
    gui_sig = 1UL << bit;
    InitSemaphore(&sem);
    NewList(&loglist);
    scan();
    for (i = 0; i < nrows && sel < 0; i++)      /* first pick: one that was cut off, else one to convert */
        if (rows[i].pp.state == P_STOPPED || rows[i].pp.state == P_UNDO || rows[i].pp.state == P_FINISH)
            sel = i;
    for (i = 0; i < nrows && sel < 0; i++)
        if (convertible(&rows[i]) && !rows[i].boot)
            sel = i;
    if (sel < 0 && nrows)
        sel = 0;
    if (!open_window()) {
        close_window();
        quit = 1;
    } else {
        set_gadgets();
        select_row(sel);
    }
    while (!quit) {
        struct IntuiMessage *m;
        ULONG s = Wait((1UL << win->UserPort->mp_SigBit) | gui_sig | SIGBREAKF_CTRL_C);
        if (s & gui_sig) {
            drain();
            if (running && w_done)
                finished();
        }
        if ((s & SIGBREAKF_CTRL_C) && running)
            stop();
        else if ((s & SIGBREAKF_CTRL_C))
            quit = 1;
        while (win && (m = GT_GetIMsg(win->UserPort))) {
            ULONG cl = m->Class;
            UWORD code = m->Code;
            struct Gadget *g = (struct Gadget *)m->IAddress;
            GT_ReplyIMsg(m);
            if (cl == IDCMP_REFRESHWINDOW) {
                GT_BeginRefresh(win);
                draw_all();
                GT_EndRefresh(win, TRUE);
                continue;
            }
            if (cl == IDCMP_VANILLAKEY) {
                code = code >= 'A' && code <= 'Z' ? code + 32 : code;
                cl = IDCMP_GADGETUP;
                g = code == 'a' ? gad[G_ANALYSE] : code == 'c' ? gad[G_CONVERT] : code == 'r' ? gad[G_ROLLBACK]
                  : code == 's' ? gad[G_STOP] : code == 'l' ? gad[G_SAVE] : NULL;
                if (code == 27)
                    cl = IDCMP_CLOSEWINDOW;
                else if (!g || (g->Flags & GFLG_DISABLED))
                    continue;
            }
            if (cl == IDCMP_RAWKEY && !running && (code == 0x4c || code == 0x4d))
                select_row(sel + (code == 0x4c ? -1 : 1));
            else if (cl == IDCMP_CLOSEWINDOW) {
                if (!running)
                    quit = 1;
                else if (ask("Stop|Keep going", "A run is under way.\n\nStop it at the next safe point?\n"
                                                "Convert carries on from there later,\nor Roll back undoes it."))
                    stop();
            } else if (cl == IDCMP_GADGETUP)
                switch (g->GadgetID) {
                case G_PARTS:
                    if (!running)
                        select_row(code);
                    else
                        select_row(run_row);
                    break;
                case G_ANALYSE:
                    start(F2P_DRY);
                    break;
                case G_CONVERT:
                    start(F2P_CONVERT);
                    break;
                case G_ROLLBACK:
                    start(F2P_ROLLBACK);
                    break;
                case G_STOP:
                    stop();
                    break;
                case G_SAVE:
                    save_log();
                    break;
                }
        }
    }
    close_window();
    while ((nd = RemHead(&loglist)))
        FreeVec(nd);
    FreeSignal(bit);
    if (AslBase)
        CloseLibrary(AslBase);
    CloseLibrary(GadToolsBase);
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary((struct Library *)IntuitionBase);
    return 0;
}
