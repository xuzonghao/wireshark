/* echld_common.h
 *  common functions of ECHLD 
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



/**
 the "epan pipe" protocol 
**/

typedef void (*reader_realloc_t)(echld_reader_t*, size_t); 

static void child_realloc_buff(echld_reader_t* r, size_t needed) {
	size_t a = r->actual_len;
	size_t s = r->len;
	long rp_off = r->rp - r->data;

	if ( a < (s + needed) ) {
		guint8* data = r->data;
	
	   	do { 
			a *= 2;
		} while( a < (s + needed) );

	   data = (guint8*)g_realloc(data,a);

	   r->actual_len = a;
	   r->len = s;
	   r->data = data;
	   r->wp = data + s;
	   r->rp = data + rp_off;
	}
}

static reader_realloc_t reader_realloc_buff = child_realloc_buff;

#ifdef PARENT_THREADS
static void parent_realloc_buff(echld_reader_t* b, size_t needed) {
	// parent thread: obtain malloc mutex
	child_realloc_buff(b,needed);
	// parent thread: release malloc mutex
}	
#endif



void echld_init_reader(echld_reader_t* r, int fd, size_t initial) {
	r->fd = fd;
	if (fd >= 0) fcntl(fd, F_SETFL, O_NONBLOCK);

	if (r->data == NULL) {
		r->actual_len = initial;
		r->data = (guint8*)g_malloc0(initial);
		r->wp = r->data;
		r->rp = NULL;
		r->len = 0;
	}
}

void echld_reset_reader(echld_reader_t* r, int fd, size_t initial) {
	r->fd = fd;
	fcntl(fd, F_SETFL, O_NONBLOCK);

	if (r->data == NULL) {
		r->actual_len = initial;
		r->data =(guint8*) g_malloc0(initial);
		r->wp = r->data;
		r->rp = NULL;
		r->len = 0;
	} else {
		r->wp = r->data;
		r->rp = NULL;
		r->len = 0;		
	}
}

void free_reader(echld_reader_t* r) {
	free(r->data);
}

static long reader_readv(echld_reader_t* r, size_t len) {
	struct iovec iov;
	long nread;

	if ( (r->actual_len - r->len) < len ) 
		reader_realloc_buff(r, len);

	iov.iov_base = r->wp;
	iov.iov_len = len;

	nread = readv(0, 
		&iov,
		 (guint)len);

	if (nread >= 0) {
		r->wp += nread;
		r->len += nread;
	}

	return nread;
};


long echld_read_frame(echld_reader_t* r, read_cb_t cb, void* cb_data) {

    // it will use shared memory instead of inband communication
	do {
		hdr_t* h = (hdr_t*)r->rp;
		long nread;
		size_t fr_len;
		size_t missing;
		long off;

		if ( r->len < ECHLD_HDR_LEN) {
			/* read the header */
			goto incomplete_header;
		} else if ( ! reader_has_frame(r) ) {
			/* read the (rest of) the frame */
			goto incomplete_frame;
		}

		/* we've got a frame! */
		
		off = (fr_len = HDR_LEN(h)) + ECHLD_HDR_LEN;
			
		cb( &(r->rp[sizeof(hdr_t)]), HDR_LEN(h), h->h.chld_id, HDR_TYPE(h), h->h.reqh_id, cb_data);

		if ( ((long)r->len) >= off ) {
			/* shift the consumed frame */
			r->len -= off;
			memcpy(r->rp ,r->rp + off ,r->len);
			r->wp -= off;
			r->rp -= off;
		}

		continue;
		
	incomplete_header:
		missing = ECHLD_HDR_LEN - (r->len);

		nread = reader_readv(r,missing);


		if (nread < 0) {
			goto kaput; /*XXX*/
		} else if (nread < (long)missing) {
			goto again;
		} else {
			goto incomplete_frame;
		}

	incomplete_frame:
		fr_len = HDR_LEN(h) + ECHLD_HDR_LEN;
		missing = fr_len  - r->len;

		nread = reader_readv(r,missing);


		if (nread < 0) {
			goto kaput; /*XXX*/
		} else if (nread <= (long)missing) {
			goto again;
		}

	} while(1);

	return 0;
	again:	return 1;
	kaput:  return -1;
}




long echld_write_frame(int fd, GByteArray* ba, guint16 chld_id, echld_msg_type_t type, guint16 reqh_id, void* data) {
	static guint8* write_buf = NULL;
	static long wb_len = 4096;
	hdr_t* h;
	struct iovec iov;
	long fr_len = ba->len+ECHLD_HDR_LEN;

	data = data; //

    // it will use shared memory instead of inband communication

	if (! write_buf) {
		// lock if needed
		write_buf = (guint8*)g_malloc0(wb_len);
		// unlock if needed
	}

	if (fr_len > wb_len) {
		do {
			wb_len *= 2;
		} while (fr_len > wb_len);

		// lock if needed
		write_buf = (guint8*)g_realloc(write_buf,wb_len);
		// unlock if needed
	}

	h = (hdr_t*)write_buf;
	h->h.type_len  = (type<<24) | (((guint32)ba->len) & 0x00ffffff) ;
	h->h.chld_id = chld_id;
	h->h.reqh_id = reqh_id;

	memcpy(write_buf+ECHLD_HDR_LEN,ba->data,ba->len);

	iov.iov_base = write_buf;
	iov.iov_len = fr_len;

	return (long) writev(fd, &iov, (unsigned)fr_len);
}



/* encoders and decoders */





/* binary encoders and decoders used for parent->child communication */

static enc_msg_t* str_enc(const char* s) {
	GByteArray* ba = g_byte_array_new();
	g_byte_array_append(ba,s,(guint)(strlen(s)+1));
	return (enc_msg_t*)ba;
}

static gboolean str_dec(guint8* b, size_t bs, char** text) {
	guint8* end = b+bs;
	b[bs-1] = '\0'; /* null terminate the buffer to avoid strlen running */
	*text = (char*)b;
	if (b+(strlen(b)+1) > end) return FALSE;
	return TRUE;
}

static gboolean str_deca(enc_msg_t* ba, char** text) {
	return str_dec(ba->data,ba->len,text);
}

static enc_msg_t* int_str_enc(int i, const char* s) {
	GByteArray* ba = g_byte_array_new();
	g_byte_array_append(ba,(guint8*)&i,sizeof(int));
	g_byte_array_append(ba,s,(guint)(strlen(s)+1));
	return (enc_msg_t*)ba;
}

static gboolean int_str_dec(guint8* b, size_t bs, int* ip, char** text) {
	guint8* end = b+bs;
	b[bs-1] = '\0'; /* null terminate the buffer to avoid strlen running */

	if ((sizeof(int)) > bs) return FALSE;
	*ip = *((int*)b);
	b += (sizeof(int));
	*text = (char*)b;
	if ((b += (strlen(b)+1)) > end) return FALSE;

	return TRUE;
}

static gboolean int_str_deca(enc_msg_t* ba, int* ip, char** text) {
	return int_str_dec(ba->data,ba->len,ip,text);
}

static enc_msg_t* int_enc(int i) {
	GByteArray* ba = g_byte_array_new();
	g_byte_array_append(ba,(guint8*)&i,sizeof(int));
	return (enc_msg_t*)ba;
}

static gboolean int_dec(guint8* b, size_t bs, int* ip) {
	if ((sizeof(int)) > bs) return FALSE;
	*ip = *((int*)b);
	return TRUE;
}

static gboolean int_deca(enc_msg_t* ba, int* ip) {
	return int_dec(ba->data,ba->len,ip);
}

static enc_msg_t* x2str_enc(const char* s1, const char* s2) {
	GByteArray* ba = g_byte_array_new();
	g_byte_array_append(ba,s1,(guint)(strlen(s1)+1));
	g_byte_array_append(ba,s2,(guint)(strlen(s2)+1));
	return (enc_msg_t*)ba;
}

static gboolean x2str_dec(guint8* b, size_t blen, char** str1, char** str2) {
	guint8* end = b+blen;
	b[blen-1] = '\0'; /* null terminate the buffer to avoid strlen running */

	*str1  = (char*)b;
	if ((b += (strlen(b)+1)) > end) return FALSE;
	*str2 = (char*)(b);
	if ((b += (strlen(b)+1)) > end) return FALSE;
	return TRUE;
}

static gboolean x2str_deca(enc_msg_t* ba, char** str1, char** str2) {
	return x2str_dec(ba->data,ba->len,str1,str2);
}

static gboolean int_3str_dec (guint8* b, size_t len, int* i, char** s1, char** s2, char** s3) {
	guint8* end = b+len;
	b[len-1] = '\0';

	if ((sizeof(int)) > len) return FALSE;
	*i = *((int*)b);
	b += sizeof(int);

	*s1 = (char*)b;
	if ((b += (strlen(b)+1)) > end) return FALSE;
	*s2 = (char*)(b);
	if ((b += (strlen(b)+1)) > end) return FALSE;
	*s3 = (char*)b;
	if ((b += (strlen(b)+1)) > end) return FALSE;
	return TRUE;
}

static enc_msg_t* int_3str_enc(int i,  const char* s1, const char* s2, const char* s3) {
	GByteArray* ba = g_byte_array_new();
	g_byte_array_append(ba,(guint8*)&i,sizeof(int));
	g_byte_array_append(ba,s1,(guint)(strlen(s1)+1));
	g_byte_array_append(ba,s2,(guint)(strlen(s2)+1));
	g_byte_array_append(ba,s3,(guint)(strlen(s3)+1));
	return (enc_msg_t*)ba;
}

static gboolean int_3str_deca (enc_msg_t* e, int* i, char** s1, char** s2, char** s3) {
	return int_3str_dec(e->data,e->len,i,s1,s2,s3);
}

static gboolean x3str_dec (guint8* b, size_t len, char** s1, char** s2, char** s3) {
	guint8* end = b+len;
	b[len-1] = '\0';


	*s1 = (char*)b;
	if ((b += (strlen(b)+1)) > end) return FALSE;
	*s2 = (char*)(b);
	if ((b += (strlen(b)+1)) > end) return FALSE;
	*s3 = (char*)b;
	if ((b += (strlen(b)+1)) > end) return FALSE;
	return TRUE;
}

static gboolean x3str_deca (enc_msg_t* e, char** s1, char** s2, char** s3) {
	return x3str_dec(e->data,e->len,s1,s2,s3);
}


static enc_msg_t* x3str_enc(const char* s1, const char* s2, const char* s3) {
	GByteArray* ba = g_byte_array_new();
	g_byte_array_append(ba,s1,(guint)(strlen(s1)+1));
	g_byte_array_append(ba,s2,(guint)(strlen(s2)+1));
	g_byte_array_append(ba,s3,(guint)(strlen(s3)+1));
	return (enc_msg_t*)ba;
}

static echld_parent_encoder_t parent_encoder = {
	int_str_enc,
	x2str_enc,
	int_enc,
	str_enc,
	x2str_enc,
	str_enc,
	str_enc,
	str_enc,
	int_str_enc,
	str_enc,
	x2str_enc
};

echld_parent_encoder_t* echld_get_encoder(void) {
	return &parent_encoder;
}

static child_decoder_t child_decoder = {
	int_str_dec,
	x2str_dec,
	str_dec,
	int_dec,
	str_dec,
	x2str_dec,
	str_dec,
	str_dec,
	int_str_dec,
	str_dec,
	x2str_dec 
};

static child_encoder_t  child_encoder = {
	int_str_enc,
	str_enc,
	x2str_enc,
	str_enc,
	int_str_enc,
	int_str_enc,
	int_3str_enc,
	x3str_enc
};

static parent_decoder_t parent_decoder = {
	int_str_deca,
	str_deca,
	x2str_deca,
	str_deca,
	int_str_deca,
	int_str_deca,
	int_3str_deca,
	x3str_deca
};

void echld_get_all_codecs( child_encoder_t **e, child_decoder_t **d, echld_parent_encoder_t **pe, parent_decoder_t** pd) {
	e && (*e = &child_encoder);
	d && (*d = &child_decoder);
	pe && (*pe = &parent_encoder);
	pd && (*pd = &parent_decoder);
}



/* output encoders, used in the switch */


static char* packet_summary_json(GByteArray* ba _U_) {
	/* dummy */
	return g_strdup("{type='packet_summary', packet_summary={}");
}

static char* tree_json(GByteArray* ba _U_) {
	/* dummy */
	return g_strdup("{type='tree', tree={}");
}

char* tvb_json(GByteArray* ba  _U_, tvb_t* tvb  _U_, const char* name) {
	/* dummy */
	return g_strdup_printf("{type='buffer', buffer={name='%s', range='0-2', data=[0x12,0xff] }",name);
}

static char* error_json(GByteArray* ba) {
	char* s = (char*)(ba->data + sizeof(int));
	int i = *((int*)s);

	s = g_strdup_printf("{type='error', error={errnum=%d, message='%s'}}",i,s);

	return s;
}

static char* child_dead_json(GByteArray* ba) {
	char* s = (char*)(ba->data + sizeof(int));
	int i = *((int*)s);

	s = g_strdup_printf("{type='child_dead', child_dead={childnum=%d, message='%s'}}",i,s);

	return s;
}

static char* closing_json(GByteArray* ba) {
	char* s = (char*)(ba->data);
	s = g_strdup_printf("{type='closing', closing={reason='%s'}}",s);

	return s;
}



static char* note_added_json(GByteArray* ba) {
	char* s = (char*)(ba->data);
	s = g_strdup_printf("{ type='note_added', note_added={msg='%s'}}",s);

	return s;
}

static char* packet_list_json(GByteArray* ba _U_) {
	return g_strdup("{}");
}

static char* file_saved_json(GByteArray* ba) {
	char* s = (char*)(ba->data);

	s = g_strdup_printf("{ type='file_saved', file_saved={msg='%s'}}",s);

	return s;
}



static char* param_set_json(GByteArray* ba) {
	char* s1 = (char*)(ba->data);
	char* s2 = ((char*)(ba->data)) + strlen(s1);

	s1 = g_strdup_printf("{type='param_set', param_set={param='%s' value='%s'}}",s1,s2);


	return s1;
}

static char* set_param_json(GByteArray* ba) {
	char* s1 = (char*)(ba->data);
	char* s2 = ((char*)(ba->data)) + strlen(s1);

	s1 = g_strdup_printf("{type='set_param', set_param={param='%s' value='%s'}}",s1,s2);


	return s1;
}

static char* get_param_json(GByteArray* ba) {
	char* s1 = (char*)(ba->data);

	s1 = g_strdup_printf("{type='get_param', get_param={param='%s'}}",s1);


	return s1;
}

static char* file_opened_json(GByteArray* ba _U_) {
	return g_strdup("");
}

static char* open_file_json(GByteArray* ba _U_) {
	return g_strdup("");
}

static char* open_interface_json(GByteArray* ba _U_) {
	return g_strdup("");
}


static char* interface_opened_json(GByteArray* ba _U_) {
	return g_strdup("");
}

static char* notify_json(GByteArray* ba _U_) {
	return g_strdup("");
}

static char* get_tree_json(GByteArray* ba _U_) {
	return g_strdup("");
}

static char* get_sum_json(GByteArray* ba _U_) {
	return g_strdup("");
}

static char* get_buffer_json(GByteArray* ba _U_) {
	return g_strdup("");
}

static char* buffer_json(GByteArray* ba _U_) {
	return g_strdup("");
}

static char* add_note_json(GByteArray* ba _U_) {
	return g_strdup("");
}

static char* apply_filter_json(GByteArray* ba _U_) {
	return g_strdup("");
}

static char* save_file_json(GByteArray* ba _U_) {
	return g_strdup("");
}


/* this to be used only at the parent */
static char* decode_json(echld_msg_type_t type, enc_msg_t* m) {
  	GByteArray* ba = (GByteArray*)m;

	switch(type) {
		case ECHLD_ERROR: return error_json(ba);
		case ECHLD_TIMED_OUT: return g_strdup("{type='timed_out'}");
		case ECHLD_NEW_CHILD: return g_strdup("{type='new_child'}");
		case ECHLD_HELLO: return g_strdup("{type='helo'}");
		case ECHLD_CHILD_DEAD: return child_dead_json(ba);
		case ECHLD_CLOSE_CHILD: return g_strdup("{type='close_child'}");
		case ECHLD_CLOSING: return closing_json(ba);
		case ECHLD_SET_PARAM: return set_param_json(ba);
		case ECHLD_GET_PARAM: return get_param_json(ba);
		case ECHLD_PARAM: return param_set_json(ba);
		case ECHLD_PING: return g_strdup("{type='ping'}");
		case ECHLD_PONG: return g_strdup("{type='pong'}");
		case ECHLD_OPEN_FILE: return open_file_json(ba);
		case ECHLD_FILE_OPENED: return file_opened_json(ba);
		case ECHLD_OPEN_INTERFACE: return open_interface_json(ba);
		case ECHLD_INTERFACE_OPENED: return interface_opened_json(ba);
		case ECHLD_START_CAPTURE: return g_strdup("{type='start_capture'}");
		case ECHLD_CAPTURE_STARTED: return g_strdup("{type='capture_started'}");
		case ECHLD_NOTIFY: return notify_json(ba);
		case ECHLD_GET_SUM: return get_sum_json(ba);
		case ECHLD_PACKET_SUM: return packet_summary_json(ba);
		case ECHLD_GET_TREE: return get_tree_json(ba);
		case ECHLD_TREE: return tree_json(ba);
		case ECHLD_GET_BUFFER: return get_buffer_json(ba);
		case ECHLD_BUFFER: return buffer_json(ba);
		case ECHLD_EOF: return g_strdup("{type='eof'}");
		case ECHLD_STOP_CAPTURE: return g_strdup("{type='stop_capture'}");
		case ECHLD_CAPTURE_STOPPED: return g_strdup("{type='capture_stopped'}");
		case ECHLD_ADD_NOTE: return add_note_json(ba);
		case ECHLD_NOTE_ADDED: return note_added_json(ba);
		case ECHLD_APPLY_FILTER: return apply_filter_json(ba);
		case ECHLD_PACKET_LIST: return packet_list_json(ba);
		case ECHLD_SAVE_FILE: return save_file_json(ba);
		case ECHLD_FILE_SAVED: return file_saved_json(ba);
		case EC_ACTUAL_ERROR: return g_strdup("{type='actual_error'}");
		default: break;
	}

	return NULL;
}
char* echld_decode(echld_msg_type_t t, enc_msg_t* m ) {
	return decode_json(t,m);
}



extern void dummy_switch(echld_msg_type_t type) {
	switch(type) {
		case ECHLD_ERROR: break; //
		case ECHLD_TIMED_OUT: break;
		case ECHLD_NEW_CHILD: break;
		case ECHLD_HELLO: break; 
		case ECHLD_CHILD_DEAD: break; //S msg
		case ECHLD_CLOSE_CHILD: break;
		case ECHLD_CLOSING: break; //
		case ECHLD_SET_PARAM: break; 
		case ECHLD_GET_PARAM: break;
		case ECHLD_PARAM: break; //SS param,val
		case ECHLD_PING: break;
		case ECHLD_PONG: break; //
		case ECHLD_OPEN_FILE: break; 
		case ECHLD_FILE_OPENED: break; //
		case ECHLD_OPEN_INTERFACE: break;
		case ECHLD_INTERFACE_OPENED: break; //
		case ECHLD_START_CAPTURE: break;
		case ECHLD_CAPTURE_STARTED: break; //
		case ECHLD_NOTIFY: break; //S notification (pre-encoded) 
		case ECHLD_GET_SUM: break;
		case ECHLD_PACKET_SUM: break; //S (pre-encoded)
		case ECHLD_GET_TREE: break;
		case ECHLD_TREE: break; //IS framenum,tree (pre-encoded)
		case ECHLD_GET_BUFFER: break;
		case ECHLD_BUFFER: break; //SSIS name,range,totlen,data
		case ECHLD_EOF: break; //
		case ECHLD_STOP_CAPTURE: break;
		case ECHLD_CAPTURE_STOPPED: break; //
		case ECHLD_ADD_NOTE: break;
		case ECHLD_NOTE_ADDED: break; //IS
		case ECHLD_APPLY_FILTER: break;
		case ECHLD_PACKET_LIST: break; //SS name,range
		case ECHLD_SAVE_FILE: break;
		case ECHLD_FILE_SAVED: break;
		case EC_ACTUAL_ERROR: break;
	}

	switch(type) {
		case ECHLD_NEW_CHILD: break;
		case ECHLD_CLOSE_CHILD: break;
		case ECHLD_SET_PARAM: break; // set_param(p,v)
		case ECHLD_GET_PARAM: break; // get_param(p)
		case ECHLD_PING: break;
		case ECHLD_OPEN_FILE: break; // open_file(f,mode)
		case ECHLD_OPEN_INTERFACE: break; // open_interface(if,param)
		case ECHLD_START_CAPTURE: break;
		case ECHLD_GET_SUM: break; // get_sum(rng)
		case ECHLD_GET_TREE: break; // get_tree(rng)
		case ECHLD_GET_BUFFER: break; // get_buffer(rng)
		case ECHLD_STOP_CAPTURE: break;
		case ECHLD_ADD_NOTE: break; // add_note(framenum,note)
		case ECHLD_APPLY_FILTER: break; // apply_filter(df)
		case ECHLD_SAVE_FILE: break; // save_file(f,mode)


		case ECHLD_ERROR: break; // error(err,reason)
		case ECHLD_TIMED_OUT: break;
		case ECHLD_HELLO: break; 
		case ECHLD_CHILD_DEAD: break; // child_dead(msg)
		case ECHLD_CLOSING: break;
		case ECHLD_PARAM: break;
		case ECHLD_PONG: break;
		case ECHLD_FILE_OPENED: break; 
		case ECHLD_INTERFACE_OPENED: break;
		case ECHLD_CAPTURE_STARTED: break;
		case ECHLD_NOTIFY: break; // notify(pre-encoded) 
		case ECHLD_PACKET_SUM: break; // packet_sum(pre-encoded)
		case ECHLD_TREE: break; //tree(framenum, tree(pre-encoded) ) 
		case ECHLD_BUFFER: break; // buffer (name,range,totlen,data)
		case ECHLD_EOF: break; 
		case ECHLD_CAPTURE_STOPPED: break; 
		case ECHLD_NOTE_ADDED: break; 
		case ECHLD_PACKET_LIST: break; // packet_list(name,filter,range);
		case ECHLD_FILE_SAVED: break;

		case EC_ACTUAL_ERROR: break;
	}
}

static void* unused = int_deca;

extern void unused_things(void) {
	unused = NULL;
}
