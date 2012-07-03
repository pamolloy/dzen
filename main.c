/*
 * (C)opyright 2007-2009 Robert Manea <rob dot manea at gmail dot com>
 * See LICENSE file for license details.
 *
 */

#include "dzen.h"
#include "action.h"
#include "opt.h"

#include <ctype.h>
#include <locale.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 255
#endif

Dzen dzen = {0};
static int last_cnt = 0;
typedef void sigfunc(int);
click_a sens_areas[MAX_CLICKABLE_AREAS];
int sens_areas_cnt=0;


static void
clean_up(void) {
	int i;

	free_event_list();
#ifndef DZEN_XFT
	if(dzen.font.set)
		XFreeFontSet(dzen.dpy, dzen.font.set);
	else
		XFreeFont(dzen.dpy, dzen.font.xfont);
#endif

	XFreePixmap(dzen.dpy, dzen.title_win.drawable);
	if(dzen.slave_win.max_lines) {
		for(i=0; i < dzen.slave_win.max_lines; i++) {
			XFreePixmap(dzen.dpy, dzen.slave_win.drawable[i]);
			XDestroyWindow(dzen.dpy, dzen.slave_win.line[i]);
		}
		free(dzen.slave_win.line);
		XDestroyWindow(dzen.dpy, dzen.slave_win.win);
	}
	XFreeGC(dzen.dpy, dzen.gc);
	XFreeGC(dzen.dpy, dzen.rgc);
	XFreeGC(dzen.dpy, dzen.tgc);
	XDestroyWindow(dzen.dpy, dzen.title_win.win);
	XCloseDisplay(dzen.dpy);
}

static void
catch_sigusr1(int s) {
	(void)s;
	do_action(sigusr1);
}

static void
catch_sigusr2(int s) {
	(void)s;
	do_action(sigusr2);
}

static void
catch_sigterm(int s) {
	(void)s;
	do_action(onexit);
}

static void
catch_alrm(int s) {
	(void)s;
	do_action(onexit);
	clean_up();
	exit(EXIT_SUCCESS);
}

static sigfunc *
setup_signal(int signr, sigfunc *shandler) {
	struct sigaction nh, oh;

	nh.sa_handler = shandler;
	sigemptyset(&nh.sa_mask);
	nh.sa_flags = 0;

	if(sigaction(signr, &nh, &oh) < 0)
		return SIG_ERR;

	return NULL;
}

char *rem=NULL;
static int
chomp(char *inbuf, char *outbuf, int start, int len) {
	int i=0;
	int off=start;

	if(rem) {
		strncpy(outbuf, rem, strlen(rem));
		i += strlen(rem);
		free(rem);
		rem = NULL;
	}
	while(off < len) {
		if(i > MAX_LINE_LEN) {
			outbuf[i] = '\0';
			return ++off;
		}
		if(inbuf[off] != '\n') {
			 outbuf[i++] = inbuf[off++];
		}
		else if(inbuf[off] == '\n') {
			outbuf[i] = '\0';
			return ++off;
		}
	}

	outbuf[i] = '\0';
	rem = estrdup(outbuf);
	return 0;
}

void
free_buffer(void) {
	int i;
	for(i=0; i<dzen.slave_win.tcnt; i++) {
		free(dzen.slave_win.tbuf[i]);
		dzen.slave_win.tbuf[i] = NULL;
	}
	dzen.slave_win.tcnt =
		dzen.slave_win.last_line_vis =
		last_cnt = 0;
}

/*
 * Read one line from stdin. If zero bits are read then return -1 if dzen is
 * not persistent, otherwise return -2 if it is persistent.
 */
//TODO (PM) The check for `ispersistent' is outside the logical scope of this
// function
static int
read_stdin(void) {
	char buf[MAX_LINE_LEN],
		 retbuf[MAX_LINE_LEN];
	ssize_t n, n_off=0;

	n = read(STDIN_FILENO, buf, sizeof buf);
	if(n == 0) {	// If 0 bits are read
		if(!dzen.ispersistent) {	// And dzen is not persistent
			dzen.running = False;	// Stop running
			//TODO (PM) The value of `running' will never be checked since the
			// event loop returns if this function returns -1
			return -1;
		}
		else
			return -2;	// Is persistent
	}
	else if (n > 0) {
		while((n_off = chomp(buf, retbuf, n_off, n))) {
			if(!dzen.slave_win.ishmenu
					&& dzen.tsupdate
					&& dzen.slave_win.max_lines
					&& ((dzen.cur_line == 0) || !(dzen.cur_line % (dzen.slave_win.max_lines+1))))
				drawheader(retbuf);
			else if(!dzen.slave_win.ishmenu
					&& !dzen.tsupdate
					&& ((dzen.cur_line == 0) || !dzen.slave_win.max_lines))
				drawheader(retbuf);
			else
				drawbody(retbuf);
			dzen.cur_line++;
		}
	}
	else {
		perror("read");	//TODO (PM) Consolidate error handling
		exit(EXIT_FAILURE);
	}
	return 0;
}

static void
x_hilight_line(int line) {
	drawtext(dzen.slave_win.tbuf[line + dzen.slave_win.first_line_vis], 1, line, dzen.slave_win.alignment);
	XCopyArea(dzen.dpy, dzen.slave_win.drawable[line], dzen.slave_win.line[line], dzen.gc,
			0, 0, dzen.slave_win.width, dzen.line_height, 0, 0);
}

static void
x_unhilight_line(int line) {
	drawtext(dzen.slave_win.tbuf[line + dzen.slave_win.first_line_vis], 0, line, dzen.slave_win.alignment);
	XCopyArea(dzen.dpy, dzen.slave_win.drawable[line], dzen.slave_win.line[line], dzen.rgc,
			0, 0, dzen.slave_win.width, dzen.line_height, 0, 0);
}

void
x_draw_body(void) {
	int i;
	dzen.x = 0;
	dzen.y = 0;
	dzen.w = dzen.slave_win.width;
	dzen.h = dzen.line_height;

	if(!dzen.slave_win.last_line_vis) {
		if(dzen.slave_win.tcnt < dzen.slave_win.max_lines) {
			dzen.slave_win.first_line_vis = 0;
			dzen.slave_win.last_line_vis  = dzen.slave_win.tcnt;
		}
		else {
			dzen.slave_win.first_line_vis = dzen.slave_win.tcnt - dzen.slave_win.max_lines;
			dzen.slave_win.last_line_vis  = dzen.slave_win.tcnt;
		}
	}

	for(i=0; i < dzen.slave_win.max_lines; i++) {
		if(i < dzen.slave_win.last_line_vis)
			drawtext(dzen.slave_win.tbuf[i + dzen.slave_win.first_line_vis],
					0, i, dzen.slave_win.alignment);
	}
	for(i=0; i < dzen.slave_win.max_lines; i++)
		XCopyArea(dzen.dpy, dzen.slave_win.drawable[i], dzen.slave_win.line[i], dzen.gc,
				0, 0, dzen.slave_win.width, dzen.line_height, 0, 0);
}

static void
x_check_geometry(XRectangle scr) {
	TWIN *t = &dzen.title_win;
	SWIN *s = &dzen.slave_win;

	t->x = t->x < 0 ? scr.width  + t->x + scr.x : t->x + scr.x;
	t->y = t->y < 0 ? scr.height + t->y + scr.y : t->y + scr.y;

	if(t->x < scr.x || scr.x + scr.width < t->x)
		t->x = scr.x;

	if(!t->width)
		t->width = scr.width;

	if((t->x + t->width) > (scr.x + scr.width) && (t->expand != left))
		t->width = scr.width - (t->x - scr.x);

	if(t->expand == left) {
		t->x_right_corner = t->x + t->width;
		t->x = t->width ? t->x_right_corner - t->width : scr.x;
	}

	if(!s->width) {
		s->x = scr.x;
		s->width = scr.width;
	}
	if(t->width == s->width)
		s->x = t->x;

	if(s->width != scr.width) {
		s->x = t->x + (t->width - s->width)/2;
		if(s->x < scr.x)
			s->x = scr.x;
		if(s->x + s->width > scr.x + scr.width)
			s->x = scr.x + (scr.width - s->width);
	}

	if(!dzen.line_height)
		dzen.line_height = dzen.font.height + 2;

	if(t->y + dzen.line_height > scr.y + scr.height)
		t->y = scr.y + scr.height - dzen.line_height;
}

static void
qsi_no_xinerama(Display *dpy, XRectangle *rect) {
	rect->x = 0;
	rect->y = 0;
	rect->width  = DisplayWidth( dpy, DefaultScreen(dpy));
	rect->height = DisplayHeight(dpy, DefaultScreen(dpy));
}

#ifdef DZEN_XINERAMA
static void
queryscreeninfo(Display *dpy, XRectangle *rect, int screen) {
	XineramaScreenInfo *xsi = NULL;
	int nscreens = 1;

	if(XineramaIsActive(dpy))
		xsi = XineramaQueryScreens(dpy, &nscreens);

	if(xsi == NULL || screen > nscreens || screen <= 0) {
		qsi_no_xinerama(dpy, rect);
	}
	else {
		rect->x      = xsi[screen-1].x_org;
		rect->y      = xsi[screen-1].y_org;
		rect->width  = xsi[screen-1].width;
		rect->height = xsi[screen-1].height;
	}
}
#endif

static void
set_docking_ewmh_info(Display *dpy, Window w, int dock) {
	unsigned long strut[12] = { 0 };
	unsigned long strut_s[4] = { 0 };
	XWindowAttributes wa;
	Atom type;
	unsigned int desktop;
	pid_t cur_pid;
	char *host_name;
	XTextProperty txt_prop;
	XRectangle si;
#ifdef DZEN_XINERAMA
	XineramaScreenInfo *xsi;
	int screen_count,i,max_height;
#endif

	host_name = emalloc(HOST_NAME_MAX);
	if( (gethostname(host_name, HOST_NAME_MAX) > -1) &&
			(cur_pid = getpid()) ) {

		XStringListToTextProperty(&host_name, 1, &txt_prop);
		XSetWMClientMachine(dpy, w, &txt_prop);
		XFree(txt_prop.value);

		XChangeProperty(
				dpy,
				w,
				XInternAtom(dpy, "_NET_WM_PID", False),
				XInternAtom(dpy, "CARDINAL", False),
				32,
				PropModeReplace,
				(unsigned char *)&cur_pid,
				1
				);

	}
	free(host_name);


	XGetWindowAttributes(dpy, w, &wa);
#ifdef DZEN_XINERAMA
	queryscreeninfo(dpy,&si,dzen.xinescreen);
#else
	qsi_no_xinerama(dpy,&si);
#endif
	if(wa.y - si.y == 0) {
		strut[2] = si.y + wa.height;
		strut[8] = wa.x;
		strut[9] = wa.x + wa.width - 1;

		strut_s[2] = strut[2];
	}
	else if((wa.y - si.y + wa.height) == si.height) {
#ifdef DZEN_XINERAMA
		max_height = si.height;
		xsi = XineramaQueryScreens(dpy,&screen_count);
		for(i=0; i < screen_count; i++) {
			if(xsi[i].height > max_height)
				max_height = xsi[i].height;
		}
		XFree(xsi);
		/* Adjust strut value if there is a larger screen */ 
		strut[3] = max_height - (si.height + si.y) + wa.height;
#else
		strut[3] = wa.height;
#endif
		strut[10] = wa.x;
		strut[11] = wa.x + wa.width - 1;

		strut_s[3] = strut[3];
	}

	if(strut[2] != 0 || strut[3] != 0) {
		XChangeProperty(
				dpy,
				w,
				XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL", False),
				XInternAtom(dpy, "CARDINAL", False),
				32,
				PropModeReplace,
				(unsigned char *)&strut,
				12
				);
		XChangeProperty(
				dpy,
				w,
				XInternAtom(dpy, "_NET_WM_STRUT", False),
				XInternAtom(dpy, "CARDINAL", False),
				32,
				PropModeReplace,
				(unsigned char *)&strut,
				4
				);
	}

	if(dock) {
		type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
		XChangeProperty(
				dpy,
				w,
				XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False),
				XInternAtom(dpy, "ATOM", False),
				32,
				PropModeReplace,
				(unsigned char *)&type,
				1
				);

		/* some window managers honor this properties*/
		type = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
		XChangeProperty(
				dpy,
				w,
				XInternAtom(dpy, "_NET_WM_STATE", False),
				XInternAtom(dpy, "ATOM", False),
				32,
				PropModeReplace,
				(unsigned char *)&type,
				1
				);

		type = XInternAtom(dpy, "_NET_WM_STATE_STICKY", False);
		XChangeProperty(
				dpy,
				w,
				XInternAtom(dpy, "_NET_WM_STATE", False),
				XInternAtom(dpy, "ATOM", False),
				32,
				PropModeAppend,
				(unsigned char *)&type,
				1
				);


		desktop = 0xffffffff;
		XChangeProperty(
				dpy,
				w,
				XInternAtom(dpy, "_NET_WM_DESKTOP", False),
				XInternAtom(dpy, "CARDINAL", False),
				32,
				PropModeReplace,
				(unsigned char *)&desktop,
				1
				);
	}

}

static void
x_create_gcs(void) {
	XGCValues gcv;
	gcv.graphics_exposures = 0;

	/* normal GC */
	dzen.gc  = XCreateGC(dzen.dpy, RootWindow(dzen.dpy, dzen.screen), GCGraphicsExposures, &gcv);
	XSetForeground(dzen.dpy, dzen.gc, dzen.norm[ColFG]);
	XSetBackground(dzen.dpy, dzen.gc, dzen.norm[ColBG]);
	/* reverse color GC */
	dzen.rgc = XCreateGC(dzen.dpy, RootWindow(dzen.dpy, dzen.screen), GCGraphicsExposures, &gcv);
	XSetForeground(dzen.dpy, dzen.rgc, dzen.norm[ColBG]);
	XSetBackground(dzen.dpy, dzen.rgc, dzen.norm[ColFG]);
	/* temporary GC */
	dzen.tgc = XCreateGC(dzen.dpy, RootWindow(dzen.dpy, dzen.screen), GCGraphicsExposures, &gcv);
}

static void
x_connect(void) {
	dzen.dpy = XOpenDisplay(0);
	if(!dzen.dpy)
		eprint("dzen: cannot open display\n");
	dzen.screen = DefaultScreen(dzen.dpy);
}

/* Read display styles from X resources. */
static void
x_read_resources(void) {
	XrmDatabase xdb;
	char* xrm;
	char* datatype[20];
	XrmValue xvalue;

	XrmInitialize();
	xrm = XResourceManagerString(dzen.dpy);
	if( xrm != NULL ) {
		xdb = XrmGetStringDatabase(xrm);
		if( XrmGetResource(xdb, "dzen2.font", "*", datatype, &xvalue) == True )
			dzen.fnt = estrdup(xvalue.addr);
		if( XrmGetResource(xdb, "dzen2.foreground", "*", datatype, &xvalue) == True )
			dzen.fg  = estrdup(xvalue.addr);
		if( XrmGetResource(xdb, "dzen2.background", "*", datatype, &xvalue) == True )
			dzen.bg  = estrdup(xvalue.addr);
		if( XrmGetResource(xdb, "dzen2.titlename", "*", datatype, &xvalue) == True )
			dzen.title_win.name  = estrdup(xvalue.addr);
		if( XrmGetResource(xdb, "dzen2.slavename", "*", datatype, &xvalue) == True )
			dzen.slave_win.name  = estrdup(xvalue.addr);
		XrmDestroyDatabase(xdb);
	}
}

static void
x_create_windows(int use_ewmh_dock) {
	XSetWindowAttributes wa;
	Window root;
	int i;
	XRectangle si;
	XClassHint *class_hint;

	root = RootWindow(dzen.dpy, dzen.screen);

	/* style */
	if((dzen.norm[ColBG] = getcolor(dzen.bg)) == ~0lu)
		eprint("dzen: error, cannot allocate color '%s'\n", dzen.bg);
	if((dzen.norm[ColFG] = getcolor(dzen.fg)) == ~0lu)
		eprint("dzen: error, cannot allocate color '%s'\n", dzen.fg);
	setfont(dzen.fnt);

	x_create_gcs();

	/* window attributes */
	wa.override_redirect = (use_ewmh_dock ? 0 : 1);
	wa.background_pixmap = ParentRelative;
	wa.event_mask = ExposureMask | ButtonReleaseMask | ButtonPressMask | ButtonMotionMask | EnterWindowMask | LeaveWindowMask | KeyPressMask;

#ifdef DZEN_XINERAMA
	queryscreeninfo(dzen.dpy, &si, dzen.xinescreen);
#else
	qsi_no_xinerama(dzen.dpy, &si);
#endif
	x_check_geometry(si);

	/* title window */
	dzen.title_win.win = XCreateWindow(dzen.dpy, root,
			dzen.title_win.x, dzen.title_win.y, dzen.title_win.width, dzen.line_height, 0,
			DefaultDepth(dzen.dpy, dzen.screen), CopyFromParent,
			DefaultVisual(dzen.dpy, dzen.screen),
			CWOverrideRedirect | CWBackPixmap | CWEventMask, &wa);
	/* set class property */
	class_hint = XAllocClassHint();
	class_hint->res_name  = "dzen2";
	class_hint->res_class = "dzen";
	XSetClassHint(dzen.dpy, dzen.title_win.win, class_hint);
	XFree(class_hint);

	/* title */
	XStoreName(dzen.dpy, dzen.title_win.win, dzen.title_win.name);

	dzen.title_win.drawable = XCreatePixmap(dzen.dpy, root, dzen.title_win.width,
			dzen.line_height, DefaultDepth(dzen.dpy, dzen.screen));
	XFillRectangle(dzen.dpy, dzen.title_win.drawable, dzen.rgc, 0, 0, dzen.title_win.width, dzen.line_height);

	/* set some hints for windowmanagers*/
	set_docking_ewmh_info(dzen.dpy, dzen.title_win.win, use_ewmh_dock);

	/* TODO: Smarter approach to window creation so we can reduce the
	 *       size of this function.
	 */

	if(dzen.slave_win.max_lines) {
		dzen.slave_win.first_line_vis = 0;
		dzen.slave_win.last_line_vis  = 0;
		dzen.slave_win.line     = emalloc(sizeof(Window) * dzen.slave_win.max_lines);
		dzen.slave_win.drawable =  emalloc(sizeof(Drawable) * dzen.slave_win.max_lines);

		/* horizontal menu mode */
		if(dzen.slave_win.ishmenu) {
			/* calculate width of menuentries - this is a very simple
			 * approach but works well for general cases.
			 */
			int ew = dzen.slave_win.width / dzen.slave_win.max_lines;
			int r = dzen.slave_win.width - ew * dzen.slave_win.max_lines;
			dzen.slave_win.issticky = True;
			dzen.slave_win.y = dzen.title_win.y;

			dzen.slave_win.win = XCreateWindow(dzen.dpy, root,
					dzen.slave_win.x, dzen.slave_win.y, dzen.slave_win.width, dzen.line_height, 0,
					DefaultDepth(dzen.dpy, dzen.screen), CopyFromParent,
					DefaultVisual(dzen.dpy, dzen.screen),
					CWOverrideRedirect | CWBackPixmap | CWEventMask, &wa);
			XStoreName(dzen.dpy, dzen.slave_win.win, dzen.slave_win.name);

			for(i=0; i < dzen.slave_win.max_lines; i++) {
				dzen.slave_win.drawable[i] = XCreatePixmap(dzen.dpy, root, ew+r,
						dzen.line_height, DefaultDepth(dzen.dpy, dzen.screen));
			XFillRectangle(dzen.dpy, dzen.slave_win.drawable[i], dzen.rgc, 0, 0,
					ew+r, dzen.line_height);
			}


			/* windows holding the lines */
			for(i=0; i < dzen.slave_win.max_lines; i++)
				dzen.slave_win.line[i] = XCreateWindow(dzen.dpy, dzen.slave_win.win,
						i*ew, 0, (i == dzen.slave_win.max_lines-1) ? ew+r : ew, dzen.line_height, 0,
						DefaultDepth(dzen.dpy, dzen.screen), CopyFromParent,
						DefaultVisual(dzen.dpy, dzen.screen),
						CWOverrideRedirect | CWBackPixmap | CWEventMask, &wa);

			/* As we don't use the title window in this mode,
			 * we reuse its width value
			 */
			dzen.title_win.width = dzen.slave_win.width;
			dzen.slave_win.width = ew+r;
		}

		/* vertical slave window */
		else {
			dzen.slave_win.issticky = False;
			dzen.slave_win.y = dzen.title_win.y + dzen.line_height;

			if(dzen.title_win.y + dzen.line_height*dzen.slave_win.max_lines > si.y + si.height)
				dzen.slave_win.y = (dzen.title_win.y - dzen.line_height) - dzen.line_height*(dzen.slave_win.max_lines) + dzen.line_height;

			dzen.slave_win.win = XCreateWindow(dzen.dpy, root,
					dzen.slave_win.x, dzen.slave_win.y, dzen.slave_win.width, dzen.slave_win.max_lines * dzen.line_height, 0,
					DefaultDepth(dzen.dpy, dzen.screen), CopyFromParent,
					DefaultVisual(dzen.dpy, dzen.screen),
					CWOverrideRedirect | CWBackPixmap | CWEventMask, &wa);
			XStoreName(dzen.dpy, dzen.slave_win.win, dzen.slave_win.name);

			for(i=0; i < dzen.slave_win.max_lines; i++) {
				dzen.slave_win.drawable[i] = XCreatePixmap(dzen.dpy, root, dzen.slave_win.width,
						dzen.line_height, DefaultDepth(dzen.dpy, dzen.screen));
				XFillRectangle(dzen.dpy, dzen.slave_win.drawable[i], dzen.rgc, 0, 0,
						dzen.slave_win.width, dzen.line_height);
			}

			/* windows holding the lines */
			for(i=0; i < dzen.slave_win.max_lines; i++)
				dzen.slave_win.line[i] = XCreateWindow(dzen.dpy, dzen.slave_win.win,
						0, i*dzen.line_height, dzen.slave_win.width, dzen.line_height, 0,
						DefaultDepth(dzen.dpy, dzen.screen), CopyFromParent,
						DefaultVisual(dzen.dpy, dzen.screen),
						CWOverrideRedirect | CWBackPixmap | CWEventMask, &wa);
		}
	}

}

static void
x_map_window(Window win) {
	XMapRaised(dzen.dpy, win);
	XSync(dzen.dpy, False);
}

static void
x_redraw(Window w) {
	int i;

	if(!dzen.slave_win.ishmenu
			&& w == dzen.title_win.win)
		drawheader(NULL);
	if(!dzen.tsupdate && w == dzen.slave_win.win) {
		for(i=0; i < dzen.slave_win.max_lines; i++)
			XCopyArea(dzen.dpy, dzen.slave_win.drawable[i], dzen.slave_win.line[i], dzen.gc,
					0, 0, dzen.slave_win.width, dzen.line_height, 0, 0);
	}
	else {
		for(i=0; i < dzen.slave_win.max_lines; i++)
			if(w == dzen.slave_win.line[i]) {
				XCopyArea(dzen.dpy, dzen.slave_win.drawable[i], dzen.slave_win.line[i], dzen.gc,
						0, 0, dzen.slave_win.width, dzen.line_height, 0, 0);
			}
	}
}

static void
handle_xev(void) {
	XEvent ev;
	int i, sa_clicked=0;
	char buf[32];
	KeySym ksym;

	XNextEvent(dzen.dpy, &ev);
	switch(ev.type) {
		case Expose:
			if(ev.xexpose.count == 0)
				x_redraw(ev.xexpose.window);
			break;
		case EnterNotify:
			if(dzen.slave_win.ismenu) {
				for(i=0; i < dzen.slave_win.max_lines; i++)
					if(ev.xcrossing.window == dzen.slave_win.line[i])
						x_hilight_line(i);
			}
			if(!dzen.slave_win.ishmenu
					&& ev.xcrossing.window == dzen.title_win.win)
				do_action(entertitle);
			if(ev.xcrossing.window == dzen.slave_win.win)
				do_action(enterslave);
			break;
		case LeaveNotify:
			if(dzen.slave_win.ismenu) {
				for(i=0; i < dzen.slave_win.max_lines; i++)
					if(ev.xcrossing.window == dzen.slave_win.line[i])
						x_unhilight_line(i);
			}
			if(!dzen.slave_win.ishmenu
					&& ev.xcrossing.window == dzen.title_win.win)
				do_action(leavetitle);
			if(ev.xcrossing.window == dzen.slave_win.win) {
				do_action(leaveslave);
			}
			break;
		case ButtonRelease:
			if(dzen.slave_win.ismenu) {
				for(i=0; i < dzen.slave_win.max_lines; i++)
					if(ev.xbutton.window == dzen.slave_win.line[i])
						dzen.slave_win.sel_line = i;
			}

			/* clickable areas */
             for(i=sens_areas_cnt; i>=0; i--) {
				if(ev.xbutton.window == dzen.title_win.win &&
						ev.xbutton.button == sens_areas[i].button &&
						(ev.xbutton.x >=  sens_areas[i].start_x+xorig &&
						ev.xbutton.x <=  sens_areas[i].end_x+xorig) &&
						(ev.xbutton.y >=  sens_areas[i].start_y &&
						ev.xbutton.y <=  sens_areas[i].end_y) &&
                        sens_areas[i].active) {
					spawn(sens_areas[i].cmd);
					sa_clicked++;
					break;
				}
			}
			if(!sa_clicked) {
				switch(ev.xbutton.button) {
					case Button1:
						do_action(button1);
						break;
					case Button2:
						do_action(button2);
						break;
					case Button3:
						do_action(button3);
						break;
					case Button4:
						do_action(button4);
						break;
					case Button5:
						do_action(button5);
						break;
					case Button6:
						do_action(button6);
						break;
					case Button7:
						do_action(button7);
						break;
				}
			}
			break;
		case KeyPress:
			XLookupString(&ev.xkey, buf, sizeof buf, &ksym, 0);
			do_action(ksym+keymarker);
			break;

		/* TODO: XRandR rotation and size  */
	}
}

static void
handle_newl(void) {
	XWindowAttributes wa;


	if(dzen.slave_win.max_lines && (dzen.slave_win.tcnt > last_cnt)) {
		do_action(onnewinput);

		if (XGetWindowAttributes(dzen.dpy, dzen.slave_win.win, &wa),
				wa.map_state != IsUnmapped
				/* autoscroll and redraw only if  we're
				 * currently viewing the last line of input
				 */
				&& (dzen.slave_win.last_line_vis == last_cnt)) {
			dzen.slave_win.first_line_vis = 0;
			dzen.slave_win.last_line_vis = 0;
			x_draw_body();
		}
		/* needed for a_scrollhome */
		else if(wa.map_state != IsUnmapped
				&& dzen.slave_win.last_line_vis == dzen.slave_win.max_lines)
			x_draw_body();
		/* forget state if window was unmapped */
		else if(wa.map_state == IsUnmapped || !dzen.slave_win.last_line_vis) {
			dzen.slave_win.first_line_vis = 0;
			dzen.slave_win.last_line_vis = 0;
			x_draw_body();
		}
		last_cnt = dzen.slave_win.tcnt;
	}
}

static void
event_loop(void) {
	int xfd, nbits, dr=0;
	fd_set rmask;

	// Assign connection number for the specified display
	xfd = ConnectionNumber(dzen.dpy);
	while(dzen.running) {
		FD_ZERO(&rmask);	// Clear newly declared set
		FD_SET(xfd, &rmask);	// Assign the fd to a set
		if(dr != -2)	//TODO (PM) Won't this statement always return true?
			FD_SET(STDIN_FILENO, &rmask);
			//TODO (PM) This would make a second call to the same function.
			// Can more than one fd be assigned to a set?

		while(XPending(dzen.dpy))
			handle_xev();

		nbits = select(xfd+1, &rmask, NULL, NULL, NULL);
		if (nbits != -1) {
			//TODO (PM) Again, dr has only been assigned the value, zero
			if (dr != -2 && FD_ISSET(STDIN_FILENO, &rmask)) {
				if ((dr = read_stdin()) == -1)
					return;
				handle_newl();
			}
			if (dr == -2 && dzen.timeout > 0) {
				/* set an alarm to kill us after the timeout */
				struct itimerval t;
				memset(&t, 0, sizeof t);
				t.it_value.tv_sec = dzen.timeout;
				t.it_value.tv_usec = 0;
				setitimer(ITIMER_REAL, &t, NULL);
			}
			if (FD_ISSET(xfd, &rmask))
				handle_xev();
		}
		else {
			perror("select");	//TODO (PM) Consolidate error handling
			exit(EXIT_FAILURE);
		}
	}
	return;
}

static void
x_preload(const char *fontstr, int p) {
	char *def, **missing;
	int i, n;

	missing = NULL;

	dzen.fnpl[p].set = XCreateFontSet(dzen.dpy, fontstr, &missing, &n, &def);
	if(missing)
		XFreeStringList(missing);

	if(dzen.fnpl[p].set) {
		XFontSetExtents *font_extents;
		XFontStruct **xfonts;
		char **font_names;
		dzen.fnpl[p].ascent = dzen.fnpl[p].descent = 0;
		font_extents = XExtentsOfFontSet(dzen.fnpl[p].set);
		n = XFontsOfFontSet(dzen.fnpl[p].set, &xfonts, &font_names);
		for(i = 0, dzen.fnpl[p].ascent = 0, dzen.fnpl[p].descent = 0; i < n; i++) {
			if(dzen.fnpl[p].ascent < (*xfonts)->ascent)
				dzen.fnpl[p].ascent = (*xfonts)->ascent;
			if(dzen.fnpl[p].descent < (*xfonts)->descent)
				dzen.fnpl[p].descent = (*xfonts)->descent;
			xfonts++;
		}
	}
	else {
		if(dzen.fnpl[p].xfont)
			XFreeFont(dzen.dpy, dzen.fnpl[p].xfont);
		dzen.fnpl[p].xfont = NULL;
		if(!(dzen.fnpl[p].xfont = XLoadQueryFont(dzen.dpy, fontstr)))
			eprint("dzen: error, cannot load font: '%s'\n", fontstr);
		dzen.fnpl[p].ascent = dzen.fnpl[p].xfont->ascent;
		dzen.fnpl[p].descent = dzen.fnpl[p].xfont->descent;
	}
	dzen.fnpl[p].height = dzen.fnpl[p].ascent + dzen.fnpl[p].descent;
}

static void
font_preload(char *s) {
	int k = 0;
	char *buf = strtok(s,",");
	while( buf != NULL ) {
		if(k<64)
			x_preload(buf, k++);
		buf = strtok(NULL,",");
	}
}

static int use_ewmh_dock = 0;
static char *action_string, *fnpre = NULL;

static void set_dzen()
/*
 * Set the default values for the dzen object
 */
{
	dzen.title_win.name = "dzen title";
	dzen.slave_win.name = "dzen slave";
	dzen.cur_line  = 0;
	dzen.ret_val   = 0;
	dzen.title_win.x = dzen.slave_win.x = 0;
	dzen.title_win.y = 0;
	dzen.title_win.width = dzen.slave_win.width = 0;
	dzen.title_win.alignment = ALIGNCENTER;
	dzen.slave_win.alignment = ALIGNLEFT;
	dzen.fnt = FONT;
	dzen.bg  = BGCOLOR;
	dzen.fg  = FGCOLOR;
	dzen.slave_win.max_lines  = 0;
	dzen.running = True;
	dzen.xinescreen = 0;
	dzen.tsupdate = 0;
	dzen.line_height = 0;
	dzen.title_win.expand = noexpand;
}

int main( int ac, char *av[] )
{
	int i;

	set_dzen();	// Default values
	x_connect();
	x_read_resources();
	parse_opts(ac, av);

	if(dzen.tsupdate && !dzen.slave_win.max_lines)
		dzen.tsupdate = False;

	if(!dzen.title_win.width)
		dzen.title_win.width = dzen.slave_win.width;

	if(!setlocale(LC_ALL, "") || !XSupportsLocale())
		puts("dzen: locale not available, expect problems with fonts.\n");

	if(action_string)
		fill_ev_table(action_string);
	else {
		if(!dzen.slave_win.max_lines) {
			char edef[] = "button3=exit:13";
			fill_ev_table(edef);
		}
		else if(dzen.slave_win.ishmenu) {
			char edef[] = "enterslave=grabkeys;leaveslave=ungrabkeys;"
				"button4=scrollup;button5=scrolldown;"
				"key_Left=scrollup;key_Right=scrolldown;"
				"button1=menuexec;button3=exit:13;"
				"key_Escape=ungrabkeys,exit";
			fill_ev_table(edef);
		}
		else {
			char edef[]  = "entertitle=uncollapse,grabkeys;"
				"enterslave=grabkeys;leaveslave=collapse,ungrabkeys;"
				"button1=menuexec;button2=togglestick;button3=exit:13;"
				"button4=scrollup;button5=scrolldown;"
				"key_Up=scrollup;key_Down=scrolldown;"
				"key_Escape=ungrabkeys,exit";
			fill_ev_table(edef);
		}
	}

	if((find_event(onexit) != -1)
			&& (setup_signal(SIGTERM, catch_sigterm) == SIG_ERR))
		fprintf(stderr, "dzen: error hooking SIGTERM\n");

	if((find_event(sigusr1) != -1)
			&& (setup_signal(SIGUSR1, catch_sigusr1) == SIG_ERR))
		fprintf(stderr, "dzen: error hooking SIGUSR1\n");

	if((find_event(sigusr2) != -1)
		&& (setup_signal(SIGUSR2, catch_sigusr2) == SIG_ERR))
		fprintf(stderr, "dzen: error hooking SIGUSR2\n");

	if(setup_signal(SIGALRM, catch_alrm) == SIG_ERR)
		fprintf(stderr, "dzen: error hooking SIGALARM\n");

	x_create_windows(use_ewmh_dock);

	if(!dzen.slave_win.ishmenu)
		x_map_window(dzen.title_win.win);
	else {
		XMapRaised(dzen.dpy, dzen.slave_win.win);
		for(i=0; i < dzen.slave_win.max_lines; i++)
			XMapWindow(dzen.dpy, dzen.slave_win.line[i]);
	}

	if( fnpre != NULL )
		font_preload(fnpre);

	do_action(onstart);

	event_loop();	// Main loop

	do_action(onexit);
	clean_up();

	if(dzen.ret_val)
		return dzen.ret_val;

	exit(EXIT_SUCCESS);
}

