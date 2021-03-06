/* echld_child.c
 *  epan working child internals
 *  Child process routines and definitions
 *
 * $Id$
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * Copyright (c) 2013 by Luis Ontanon <luis@ontanon.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */


#include "echld-int.h"
// echld_

typedef struct _child {
	child_state_t state;

	int pid;
	int ppid;
	int chld_id;
	int reqh_id;
	echld_reader_t parent;
	struct _fds {
		int pipe_to_parent;
		int pipe_from_dumpcap;
		int pipe_to_dumpcap;
		int file_being_read;
	} fds;

	struct timeval started;
	struct timeval now;

	child_encoder_t* enc;
	child_decoder_t* dec;

	// epan stuff
} echld_child_t;


static echld_child_t child;


#define CHILD_RESP(BYTEARR,TYPE) echld_write_frame(child.fds.pipe_to_parent, BYTEARR, child.chld_id, TYPE, child.reqh_id, NULL)

#ifdef DEBUG_CHILD
static int dbg_level = 0;

void child_debug(int level, const char* fmt, ...) {
	va_list ap;
	char* str;

	if (dbg_level<level) return;

    va_start(ap, fmt);
	str = g_strdup_vprintf(fmt,ap);
	va_end(ap);

	fprintf(stderr, "child[%d]: reqh_id=%d dbg_level=%d message='%s'", child.pid, child.reqh_id, level, str);
	g_free(str);
}

#define CHILD_DBG(attrs) ( child_debug attrs )
#else
#define CHILD_DBG(attrs)
#endif

void echld_child_initialize(int pipe_from_parent, int pipe_to_parent, int reqh_id) {
	child.state = IDLE;
	child.pid = getpid();
	child.ppid = getppid();
	child.chld_id = 0;
	child.reqh_id = reqh_id;
	echld_init_reader( &(child.parent), pipe_from_parent,4096);
	child.fds.pipe_to_parent = pipe_to_parent;
	child.fds.pipe_from_dumpcap = -1;
	child.fds.pipe_to_dumpcap = -1;
	child.fds.file_being_read = -1;
	gettimeofday(&child.started,NULL);
	gettimeofday(&child.now,NULL);
	echld_get_all_codecs(&(child.enc), &(child.dec), NULL, NULL);

		/* epan stuff */

	CHILD_DBG((5,"Child Initialized"));
}



void child_err(int e, unsigned reqh_id, const char* fmt, ...) {
	size_t len= 1024;
	gchar err_str[len];
	va_list ap;
	static GByteArray* ba;

	va_start(ap, fmt);
	g_vsnprintf(err_str,len,fmt,ap);
	va_end(ap);

	CHILD_DBG((0,"error='%s'",err_str));

	ba = child.enc->error(e, err_str);
	echld_write_frame(child.fds.pipe_to_parent, ba, child.chld_id, ECHLD_ERROR, reqh_id, NULL);
	g_byte_array_free(ba,TRUE);
}


static char* intflist2json(GList* if_list) {
	/* blatantly stolen from print_machine_readable_interfaces in dumpcap.c */
#define ADDRSTRLEN 46 /* Covers IPv4 & IPv6 */

    int         i;
    GList       *if_entry;
    if_info_t   *if_info;
    GSList      *addr;
    if_addr_t   *if_addr;
    char        addr_str[ADDRSTRLEN];
    GString     *str = g_string_new("={ ");
    char* s;

    i = 1;  /* Interface id number */
    for (if_entry = g_list_first(if_list); if_entry != NULL;
         if_entry = g_list_next(if_entry)) {
        if_info = (if_info_t *)if_entry->data;
        g_string_append_printf(str,"%d={ intf='%s',", i++, if_info->name);

        /*
         * Print the contents of the if_entry struct in a parseable format.
         * Each if_entry element is tab-separated.  Addresses are comma-
         * separated.
         */
        /* XXX - Make sure our description doesn't contain a tab */
        if (if_info->vendor_description != NULL)
            g_string_append_printf(str," vnd_desc='%s',", if_info->vendor_description);

        /* XXX - Make sure our friendly name doesn't contain a tab */
        if (if_info->friendly_name != NULL)
            g_string_append_printf(str," name='%s', addrs=[ ", if_info->friendly_name);

        for (addr = g_slist_nth(if_info->addrs, 0); addr != NULL;
                    addr = g_slist_next(addr)) {

            if_addr = (if_addr_t *)addr->data;
            switch(if_addr->ifat_type) {
            case IF_AT_IPv4:
                if (inet_ntop(AF_INET, &if_addr->addr.ip4_addr, addr_str,
                              ADDRSTRLEN)) {
                    g_string_append_printf(str,"%s", addr_str);
                } else {
                    g_string_append(str,"<unknown IPv4>");
                }
                break;
            case IF_AT_IPv6:
                if (inet_ntop(AF_INET6, &if_addr->addr.ip6_addr,
                              addr_str, ADDRSTRLEN)) {
                    g_string_append_printf(str,"%s", addr_str);
                } else {
                    g_string_append(str,"<unknown IPv6>");
                }
                break;
            default:
                g_string_append_printf(str,"<type unknown %u>", if_addr->ifat_type);
            }
        }

        g_string_append(str," ]"); /* addrs */


        if (if_info->loopback)
            g_string_append(str,", loopback=1");
        else
            g_string_append(str,", loopback=0");

        g_string_append(str,"}, ");
    }

    g_string_truncate(str,str->len - 2); /* the comma and space */
    g_string_append(str,"}");

    s=str->str;
    g_string_free(str,FALSE);
    return s;
}

static void child_start_interface_listing(void) {

}

static gboolean child_open_file(int chld_id _U_, int reqh_id _U_, const char* filename, guint8* buff, int buff_len) {
	const char* reason = "Unimplemented"; // this ain't a good reason to fail!
	g_snprintf(buff,buff_len,"Cannot open file=%s reason=%s",filename,reason);
	return FALSE;
}

static gboolean child_open_interface(int chld_id _U_, int reqh_id _U_, const char* intf_name, const char* params _U_, guint8* err_buf, int errbuff_len) {
	const char* reason = "Unimplemented"; // this ain't a good reason to fail!
	g_snprintf(err_buf,errbuff_len,"Cannot open interface=%s reason=%s",intf_name,reason);
	return FALSE;
}


static char* param_get_cwd(char** err ) {
	char* pwd = getcwd(NULL, 128);

	if (!pwd) {
		*err = g_strdup(strerror(errno));
	}
	return pwd;
}

static echld_bool_t param_set_cwd(char* val , char** err ) {
	/* XXX SANITIZE */
	if (chdir(val) != 0) {
		*err = g_strdup_printf("(%d)'%s'",errno,strerror(errno));
		return FALSE;
	}

	return TRUE;
}

#define COOKIE_SIZE 1024
static char* cookie = NULL;

static char* param_get_cookie(char** err ) {
	if (cookie)
		return g_strdup(cookie);

	*err = g_strdup("cookie not set");
	return NULL;
}

static echld_bool_t param_set_cookie(char* val , char** err _U_) {

	if (cookie) g_free(cookie);

	cookie = g_strdup(val);
	return TRUE;
}

#ifdef DEBUG_CHILD
static char* param_get_dbg_level(char** err _U_) {
	return g_strdup_printf("%d",dbg_level);
}

static echld_bool_t param_set_dbg_level(char* val , char** err ) {
	char* p;
	int lvl = (int)strtol(val, &p, 10);

	if (p<=val) {
		*err = g_strdup("not an integer");
		return FALSE;
	} else if (lvl < 0 || lvl > 5) {
		*err = g_strdup_printf("invalid level=%d (min=0 max=5)",lvl);
		return FALSE;
	}

	dbg_level = lvl;
	return TRUE;
}
#endif


static param_t params[] = {
#ifdef DEBUG_CHILD
	{"dbg_level", param_get_dbg_level, param_set_dbg_level},
# endif
	{"cookie",param_get_cookie,param_set_cookie},
	{"cwd",param_get_cwd,param_set_cwd},
	{NULL,NULL,NULL}
};

static param_t* get_paramset(char* name) {
	int i;
	for (i = 0; params[i].name != NULL;i++) {
		if (strcmp(name,params[i].name) == 0 ) return &(params[i]);
	}
	return NULL;
} 



static long child_receive(guint8* b, size_t len, echld_chld_id_t chld_id, echld_msg_type_t type, echld_reqh_id_t reqh_id, void* data _U_) {
	GByteArray* ba = NULL;

	child.chld_id = chld_id;
	child.reqh_id = reqh_id;

	CHILD_DBG((2,"Message Received type='%c' len='%d'",type,len));

	// gettimeofday(&(child.now), NULL);

	if (child.chld_id != chld_id) {
		if (child.chld_id == 0) {
			if ( type == ECHLD_NEW_CHILD) {
				child.chld_id = chld_id;
				// more init needed for sure
				CHILD_DBG((1,"chld_id set, sending HELLO"));
				CHILD_RESP(ba,ECHLD_HELLO);
				return 0;
			} else {
				child_err(ECHLD_ERR_WRONG_MSG,reqh_id,
					"not yet initialized: chld_id:%d msg_type='%c'",chld_id,type);
				return 0;
			}
		}
	
		child_err(ECHLD_ERR_WRONG_MSG,reqh_id,
			"chld_id: own:%d given:%d msg_type='%c'",child.chld_id,chld_id,type);
		return 0;
	}


	switch(type) {
		case ECHLD_PING:
			CHILD_DBG((1,"PONG"));
			CHILD_RESP(ba,ECHLD_PONG);
			break;
		case ECHLD_SET_PARAM:{
			char* param;
			char* value;
			if ( child.dec->set_param && child.dec->set_param(b,len,&param,&value) ) {
				param_t* p = get_paramset(param);
				char* err;
				if (!p) {
					child_err(ECHLD_CANNOT_SET_PARAM,reqh_id,"no such param='%s'",param);					
					break;
				}

				if (!p->set) {
					child_err(ECHLD_CANNOT_GET_PARAM,reqh_id,"reason='read only'");
					break;
				}

				if (! p->set(value,&err) ) {
					child_err(ECHLD_CANNOT_SET_PARAM,reqh_id,"reason='%s'",err);
					g_free(err);
					break;
				}

				ba = child.enc->param(param,value);
				CHILD_RESP(ba,ECHLD_PARAM);
				g_byte_array_free(ba,TRUE);
				CHILD_DBG((1,"Set Param: param='%s' value='%s'",param,value));

				break;
			} else {
				goto misencoded;
			}
		}
		case ECHLD_GET_PARAM: {
			char* param;
			if ( child.dec->get_param && child.dec->get_param(b,len,&param) ) {
				char* err;
				char* val;

				param_t* p = get_paramset(param);

				if (!p) {
					child_err(ECHLD_CANNOT_GET_PARAM,reqh_id,"no such param='%s'",param);					
					break;
				}

				if (!p->get) {
					child_err(ECHLD_CANNOT_GET_PARAM,reqh_id,"reason='write only'");					
					break;
				}

				if (!(val = p->get(&err))) {
					child_err(ECHLD_CANNOT_GET_PARAM,reqh_id,"reason='%s'",err);
					g_free(err);
					break;
				}
				
				ba = child.enc->param(param,val);
				CHILD_RESP(ba,ECHLD_PARAM);
				g_byte_array_free(ba,TRUE);
				CHILD_DBG((2,"Get Param: param='%s' value='%s'",param,val));
				break;
			} else {
				goto misencoded;
			}
		}
		case ECHLD_CLOSE_CHILD:
			CHILD_RESP(ba,ECHLD_CLOSING);
			CHILD_DBG((3,"Closing"));

			// select(0,NULL,NULL,NULL,sleep_time);
			CHILD_DBG((1,"Bye"));
			exit(0);
			break;
		case ECHLD_OPEN_INTERFACE:
		case ECHLD_OPEN_FILE:
			if (child.state != IDLE) goto wrong_state;
			goto not_implemented;			
		case ECHLD_START_CAPTURE:
			if (child.state != READY) goto wrong_state;
			goto not_implemented;					
		case ECHLD_STOP_CAPTURE:
			if (child.state != IDLE) goto wrong_state;
			goto not_implemented;			
		case ECHLD_GET_SUM:
		case ECHLD_GET_TREE:
		case ECHLD_GET_BUFFER:
			if (child.state != READING && child.state != CAPTURING ) goto wrong_state;
			goto not_implemented;		
		case ECHLD_ADD_NOTE:
		case ECHLD_APPLY_FILTER:
		case ECHLD_SAVE_FILE:
			if (child.state != DONE ) goto wrong_state;
			goto not_implemented;
		default:
			child_err(ECHLD_ERR_WRONG_MSG,reqh_id,"chld_id=%d msg_type='%c'",chld_id,type);
			break;
	}

	return 0;

	misencoded:
	// dump the misencoded message (b,blen)
	child_err(ECHLD_ERR_WRONG_MSG,reqh_id,"misencoded msg msg_type='%c'",type);
	return 0;

	wrong_state:
	child_err(ECHLD_ERR_WRONG_MSG,reqh_id,"unexpected message: received in wrong state='%c', msg_type='%c'",child.state,type);
	return 0;

	not_implemented:
	child_err(ECHLD_ERR_UNIMPLEMENTED,reqh_id,"unimplemented message: received in wrong state='%c', msg_type='%c'",child.state,type);
	return 0;

}

static void child_dumpcap_read(void) {
	// this folk manages the reading of dumpcap's pipe
	// it has to read interface descriptions when doing so
	// and managing capture during capture
				CHILD_DBG((2,"child_dumpcap_read"));
}

static void child_read_file(void) {
	// this folk manages the reading of the file after open file has opened it
	CHILD_DBG((2,"child_read_file"));
}

int echld_child_loop(void) {
	int parent_fd = child.fds.pipe_to_parent;

#ifdef DEBUG_CHILD
	int step = 0;
#endif

	CHILD_DBG((0,"child_loop()"));

	do {
		fd_set rfds;
		fd_set wfds;
		fd_set efds;
		struct timeval timeout;
		int nfds = 0;


		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		FD_ZERO(&efds);

		FD_SET(parent_fd,&rfds);

		if (child.fds.pipe_from_dumpcap > 0) {
			FD_SET(child.fds.pipe_from_dumpcap,&rfds);
			nfds++;
		}

		if (child.fds.file_being_read > 0) {
			FD_SET(child.fds.file_being_read,&rfds);
			nfds++;
		}

		CHILD_DBG((4,"child_loop: before select() step=%d",step++));
		nfds = select(nfds, &rfds, &wfds, &efds, &timeout);
		CHILD_DBG((5,"child_loop: after select() step=%d",step++));

		if ( FD_ISSET(parent_fd,&efds) ) {
			CHILD_DBG((0,"Broken Parent Pipe step=%d",step++));
			break;
		}
		if (child.fds.pipe_from_dumpcap > 0 &&  FD_ISSET(child.fds.pipe_from_dumpcap,&efds) ) {
			CHILD_DBG((0,"Broken Dumpcap Pipe step=%d",step++));
			break;
		}
		if (child.fds.file_being_read > 0 &&  FD_ISSET(child.fds.file_being_read,&efds) ) {
			CHILD_DBG((0,"Broken Readfile Pipe step=%d",step++));
			break;
		}

		if (FD_ISSET(parent_fd, &rfds)) {

			long st = echld_read_frame(&(child.parent), child_receive, &child);

			if (st < 0) {
				CHILD_DBG((0,"Read Frame Failed step=%d",step++));
				return (int)st;
			}
		}

		if (child.fds.pipe_from_dumpcap > 0 && FD_ISSET(child.fds.pipe_from_dumpcap,&rfds) ) {
			child_dumpcap_read();
		}

		if (child.fds.file_being_read > 0 && FD_ISSET(child.fds.pipe_from_dumpcap,&rfds) ) {
			child_read_file();
		}
	} while(1);


	CHILD_RESP(NULL,ECHLD_CLOSING);
	CHILD_DBG((3,"Closing"));
	return 222;
}


extern void echld_unused(void) {
	intflist2json(NULL);
	child_start_interface_listing();
	child_open_file(0, 0, NULL, NULL, 0);
	child_open_interface(0, 0, NULL, NULL, NULL, 0);
}

