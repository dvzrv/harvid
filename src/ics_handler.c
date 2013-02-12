/*
   This file is part of harvid

   Copyright (C) 2008-2013 Robin Gareus <robin@gareus.org>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

#include "daemon_log.h"
#include "ffcompat.h"
#include "httprotocol.h"
#include "ics_handler.h"
#include "enums.h"

extern int cfg_noindex;
extern int cfg_adminmask;

/** Compare Transport Protocol request */
#define CTP(CMPPATH) \
	(  strncasecmp(protocol,  "HTTP/", 5) == 0 \
	&& strncasecmp(path, CMPPATH, strlen(CMPPATH)) == 0 \
	&& strcasecmp (method_str, "GET") == 0 )

#ifndef HAVE_WINDOWS
#define CSEND(FD,STATUS) write(FD, STATUS, strlen(STATUS))
#else
#define CSEND(FD,STATUS) send(FD, STATUS, strlen(STATUS),0)
#endif

#define SEND200(MSG) \
	send_http_status_fd(c->fd, 200); \
	send_http_header_fd(c->fd, 200, NULL); \
	CSEND(c->fd, MSG);


/**
 * check for invalid or potentially malicious path.
 */
static int check_path(char *f) {
	int len = strlen(f);
	// TODO: f is an url_unescape()d value and may contain
	// malicious non-ASCII chars.

	/* check for possible 'escape docroot' trickery */
	if ( f[0] == '/' || strcmp( f, ".." ) == 0 || strncmp( f, "../", 3 ) == 0
			|| strstr( f, "/../" ) != (char*) 0
			|| strcmp( &(f[len-3]), "/.." ) == 0 ) return -1;
	return 0;
}

///////////////////////////////////////////////////////////////////

struct queryparserstate {
	ics_request_args *a;
	char *fn;
	int doit;
};

void parse_param(struct queryparserstate *qps, char *kvp) {
	char *sep;
	if (!(sep=strchr(kvp,'='))) return;
	*sep='\0';
	char *val = sep+1;
	if (!val || strlen(val) < 1 || strlen(kvp) <1) return;

	//dlog(DLOG_DEBUG, "QUERY '%s'->'%s'\n",kvp,val);

	if (!strcmp (kvp, "frame")) {
		qps->a->frame = atoi(val);
		qps->doit |= 1;
	} else if (!strcmp (kvp, "w")) {
		qps->a->out_width  = atoi(val);
	} else if (!strcmp (kvp, "h")) {
		qps->a->out_height = atoi(val);
	} else if (!strcmp (kvp, "file")) {
		qps->fn = url_unescape(val, 0, NULL);
		qps->doit |= 2;
	} else if (!strcmp (kvp, "flatindex")) {
		qps->a->idx_option|=OPT_FLAT;
	} else if (!strcmp (kvp, "format")) {
					 if (!strcmp(val,"jpg") )  qps->a->render_fmt=FMT_JPG;
			else if (!strcmp(val,"jpeg"))  qps->a->render_fmt=FMT_JPG;
			else if (!strcmp(val,"png") )  qps->a->render_fmt=FMT_PNG;
			else if (!strcmp(val,"ppm") )  qps->a->render_fmt=FMT_PPM;
			else if (!strcmp(val,"raw") )  qps->a->render_fmt=FMT_RAW;
			else if (!strcmp(val,"rgb") ) {qps->a->render_fmt=FMT_RAW; qps->a->decode_fmt=PIX_FMT_RGB24;}
			else if (!strcmp(val,"rgba")) {qps->a->render_fmt=FMT_RAW; qps->a->decode_fmt=PIX_FMT_RGBA;}
			else if (!strcmp(val,"html"))  qps->a->render_fmt=OUT_HTML;
			else if (!strcmp(val,"xhtml")) qps->a->render_fmt=OUT_HTML;
			else if (!strcmp(val,"json"))  qps->a->render_fmt=OUT_JSON;
			else if (!strcmp(val,"csv"))  {qps->a->render_fmt=OUT_CSV; qps->a->idx_option|=OPT_CSV;}
			else if (!strcmp(val,"plain")) qps->a->render_fmt=OUT_PLAIN;
	}
}

static void parse_http_query_params(struct queryparserstate *qps, char *query) {
	char *t, *s = query;
	while(s && (t=strpbrk(s,"&?"))) {
		*t='\0';
		parse_param(qps, s);
		s=t+1;
	}
	if (s) parse_param(qps, s);
}

static int parse_http_query(CONN *c, char *query, httpheader *h, ics_request_args *a) {
	struct queryparserstate qps = {a, NULL, 0};

	a->decode_fmt = PIX_FMT_RGB24; // TODO - this is yet unused
	a->render_fmt = FMT_PNG;
	a->frame=0;
	a->out_width = a->out_height = -1; // auto-set

	parse_http_query_params(&qps, query);

	/* check for illegal paths */
	if (!qps.fn || check_path(qps.fn)) {
		httperror(c->fd, 404, "File not found.", "File not found." );
		return(-1);
	}

	/* sanity checks */
	if (qps.doit&3) {
		if (qps.fn) {
			a->file_name = malloc(1+strlen(c->d->docroot)+strlen(qps.fn)*sizeof(char));
			sprintf(a->file_name,"%s%s",c->d->docroot,qps.fn);
		}
		free(qps.fn);

		/* test if file exists or send 404 */
		struct stat sb;
		if (stat(a->file_name, &sb) == -1) {
			dlog(DLOG_WARNING, "CON: file not found: '%s'\n", a->file_name);
			httperror(c->fd, 404, "Not Found", "file not found." );
			return(-1);
		}

		/* check file permissions */
		if (access(a->file_name, R_OK)) {
			dlog(DLOG_WARNING, "CON: permission denied for file: '%s'\n", a->file_name);
			httperror(c->fd, 403, NULL, NULL);
			return(-1);
		}

		if (h) h->mtime = sb.st_mtime; // XXX - check  - only used with 'hdl_decode_frame' for now.

		dlog(DLOG_DEBUG, "CON: serving '%s' f:%lu @%dx%d\n",a->file_name,a->frame,a->out_width,a->out_height);
	}
	return qps.doit;
}

/////////////////////////////////////////////////////////////////////
// Callbacks -- request handlers

// harvid.c
int   hdl_decode_frame (int fd, httpheader *h, ics_request_args *a);
char *hdl_server_status_html (CONN *c);
char *hdl_file_info (CONN *c, ics_request_args *a);
char *hdl_server_info (CONN *c, ics_request_args *a);
void  hdl_clear_cache();

// fileindex.c
char *hdl_index_dir (const char *root, char *base_url, const char *path, int opt);

// logo.o
#ifndef HAVE_WINDOWS
#define BINPFX _binary
#else
#define BINPFX binary
#endif

#define XXEXTLD(ARCHPFX,NAME) \
	extern const unsigned char ARCHPFX ## ____ ## NAME ## _start[]; \
	extern const unsigned char ARCHPFX ## ____ ## NAME ## _end[];

#define XXLDVAR(ARCHPFX,NAME) \
	ARCHPFX ## ____ ## NAME ## _start

#define XXLDLEN(ARCHPFX,NAME) \
	((ARCHPFX ## ____ ## NAME ## _end) - (ARCHPFX ## ____ ## NAME ## _start))

/* evaluator macros */
#define XEXTLD(ARCHPFX,NAME) XXEXTLD(ARCHPFX,NAME)
#define XLDVAR(ARCHPFX,NAME) XXLDVAR(ARCHPFX,NAME)
#define XLDLEN(ARCHPFX,NAME) XXLDLEN(ARCHPFX,NAME)

#define LDVAR(NAME) XLDVAR(BINPFX,NAME)
#define EXTLD(NAME) XEXTLD(BINPFX,NAME)
#define LDLEN(NAME) XLDLEN(BINPFX,NAME)

EXTLD(doc_harvid_jpg)

/////////////////////////////////////////////////////////////////////

/** main http request handler / dispatch requests */
void ics_http_handler(
		CONN *c,
		char *host, char *protocol,
		char *path, char *method_str,
		char *query, char *cookie
		) {

	if (CTP("/status")) {
		char *status = hdl_server_status_html(c);
		SEND200(status);
		free(status);
		c->run=0;
	} else if (CTP("/favicon.ico")) {
		#include "favicon.h"
		httpheader h;
		memset(&h, 0, sizeof(httpheader));
		h.ctype="image/x-icon";
		h.length=sizeof(favicon_data);
		http_tx(c->fd, 200, &h, sizeof(favicon_data), favicon_data);
		c->run=0;
	} else if (CTP("/logo.jpg")) {
		httpheader h;
		memset(&h, 0, sizeof(httpheader));
		h.ctype="image/jpeg";
		h.length= LDLEN(doc_harvid_jpg);
		http_tx(c->fd, 200, &h, h.length, LDVAR(doc_harvid_jpg));
		c->run=0;
	} else if (CTP("/info")) { /* /info -> /file/info !! */
		ics_request_args a;
		memset(&a, 0, sizeof(ics_request_args));
		int rv = parse_http_query(c, query, NULL, &a);
		if (rv < 0) {
			;
		} else if (rv&2) {
			char *info = hdl_file_info(c,&a);
			SEND200(info);
			free(info);
		} else {
			httperror(c->fd, 400, "Bad Request", "<p>Insufficient parse query parameters.</p>");
		}
		if (a.file_name) free(a.file_name);
		c->run=0;
	} else if (CTP("/rc")) {
		ics_request_args a;
		struct queryparserstate qps = {&a, NULL, 0};
		memset(&a, 0, sizeof(ics_request_args));
		parse_http_query_params(&qps, query);
		char *info = hdl_server_info(c, &a);
		SEND200(info);
		free(info);
	} else if (CTP("/index/")) { /* /index/  -> /file/index/ ?! */
		char *dp = url_unescape(&(path[7]), 0, NULL);
		if (cfg_noindex) {
			httperror(c->fd, 403, NULL, NULL);
		} else if (!dp || check_path(dp)) {
			httperror(c->fd, 400, "Bad Request", "Illegal filename." );
		} else {
			ics_request_args a;
			char base_url[1024];
			struct queryparserstate qps = {&a, NULL, 0};
			memset(&a, 0, sizeof(ics_request_args));
			parse_http_query_params(&qps, query);
			snprintf(base_url,1024, "http://%s%s", host, path);
			char *msg = hdl_index_dir(c->d->docroot, base_url, dp, a.idx_option);
			send_http_status_fd(c->fd, 200);
			if (a.idx_option & OPT_CSV) {
				httpheader h;
				memset(&h, 0, sizeof(httpheader));
				h.ctype="text/csv";
				send_http_header_fd(c->fd, 200, &h);
			} else {
				send_http_header_fd(c->fd, 200, NULL);
			}
			CSEND(c->fd, msg);
			free(dp);
			free(msg);
		}
		c->run=0;
	} else if (CTP("/admin")) { /* /admin/ */
		if (strncasecmp(path,  "/admin/flush_cache", 18) == 0 ) {
			if (cfg_adminmask&1) {
				hdl_clear_cache();
				SEND200("ok");
			} else {
				httperror(c->fd, 403, NULL, NULL);
			}
		}	else if (strncasecmp(path,  "/admin/shutdown", 15) == 0 ) {
			if (cfg_adminmask&2) {
				SEND200("ok");
				c->d->run=0;
			} else {
				httperror(c->fd, 403, NULL, NULL);
			}
		} else {
			httperror(c->fd, 400, "Bad Request", "Nonexistant admin command." );
		}
		c->run=0;
	} else if (CTP("/") && !strcmp(path, "/") && strlen(query)==0) { /* HOMEPAGE */
#define HPSIZE 1024 // max size of homepage in bytes.
		char msg[HPSIZE]; int off =0;
		off+=snprintf(msg+off, HPSIZE-off, DOCTYPE HTMLOPEN);
		off+=snprintf(msg+off, HPSIZE-off, "<title>ICS</title></head>\n<body>\n<h2>ICS</h2>\n\n");
		off+=snprintf(msg+off, HPSIZE-off, "<p>Hello World,</p>\n");
		off+=snprintf(msg+off, HPSIZE-off, "<ul>");
		off+=snprintf(msg+off, HPSIZE-off, "<li><a href=\"status/\">Server Status</a></li>\n");
		if (!cfg_noindex) {
			off+=snprintf(msg+off, HPSIZE-off, "<li><a href=\"index/\">File Index</a></li>\n");
		}
		off+=snprintf(msg+off, HPSIZE-off, "</ul>");
		off+=snprintf(msg+off, HPSIZE-off, "<hr/><p>%s at %s:%i</p>", SERVERVERSION, c->d->local_addr, c->d->local_port);
		off+=snprintf(msg+off, HPSIZE-off, "\n</body>\n</html>");
		SEND200(msg);
		c->run=0; // close connection
	}
	else if (  (strncasecmp(protocol,  "HTTP/", 5) == 0 ) /* /?file= -> /file/frame?.. !! */
			     &&(strcasecmp (method_str, "GET") == 0 )
			    )
	{
		ics_request_args a;
		httpheader h;
		memset(&a, 0, sizeof(ics_request_args));
		memset(&h, 0, sizeof(httpheader));
		int rv = parse_http_query(c, query, NULL, &a);
		if (rv < 0) {
			;
		} else if (rv==3) {
			hdl_decode_frame(c->fd, &h, &a);
		} else {
			httperror(c->fd, 400, "Bad Request", "<p>Insufficient parse query parameters.</p>");
		}
		if (a.file_name) free(a.file_name);
		c->run=0;
	}
	else
	{
		httperror(c->fd,500, "", "server does not know what to make of this.\n");
		c->run=0;
	}
}
