/*
 *  mod_auth_ntlm_winbind.c -- Apache auth_ntlm_winbind module
 *
 *  [Originally autogenerated via ``apxs -n ntlm_winbind -g'']
 *
 * Based on:
 *     mod_ntlm.c for Win32 by Tim Costello <tim.costello@bigfoot.com>
 *     pam_smb by Dave Airlie <Dave.Airlie@ul.ie>
 *
 * Copyright Andreas Gal <agal@uwsp.edu>,           2000
 * Copyright Sverre H. Huseby <sverrehu@online.no>, 2000
 * Copyright Tim Potter <tpot@samba.org>,           2001
 * Copyright Andrew Bartlett <abartlet@samba.org>,           2004
 * Apache2 code by Waider <waider@waider.ie>,       2006
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS`` AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES ARE DISCLAIMED.
 *
 * This code may be freely distributed, as long the above notices are
 * reproduced.
 */
/* (CGI fork code)
 * Copyright 1999-2004 The Apache Software Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


/*
 * The sections OVERVIEW, INSTALLATION, and CONFIGURATION are available from
 * the README file in this directory.
 */

#include "httpd.h"
#include "http_config.h"
#include "http_protocol.h"
#include "http_log.h"
#include "http_core.h"
#include "ap_config.h"
#include "util_script.h" /* for ap_call_exec */
#include <assert.h>

#ifdef APACHE2
#include "http_request.h"
#include "http_connection.h"
#include "apr_strings.h"
#include "apr_pools.h"
#include "apr_tables.h"
#include "apr_base64.h"

#define RDEBUG( x... ) ap_log_rerror( APLOG_MARK, NTLM_DEBUG, APR_SUCCESS, r, x )
#define RERROR( c, x... ) ap_log_rerror( APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, c, r, x )
#define CLEANUP(x) NULL
#else

/* compat stuff */
#define apr_pool_t ap_pool
#define apr_pstrcat(x...) ap_pstrcat(x)
#define apr_pstrdup(x...) ap_pstrdup(x)
#define apr_psprintf(x...) ap_psprintf(x)
#define apr_table_add(x...) ap_table_add(x)
#define apr_table_get(x...) ap_table_get(x)
#define apr_table_setn(x...) ap_table_setn(x)
#define apr_pcalloc(x...) ap_pcalloc(x)
#define apr_pool_destroy(x...) ap_destroy_pool(x)

#define RDEBUG( x... ) ap_log_rerror( APLOG_MARK, LOG_DEBUG, r, x )
#define RERROR( c, x... ) ap_log_rerror( APLOG_MARK, NTLM_DEBUG|APLOG_NOERRNO, r, x )
#define CLEANUP(x) x
#endif

/* The name of the NTLM authentication scheme.  This appears in the
   'WWW-Authenticate' header in the initial HTTP request. */

#define NTLM_AUTH_NAME "NTLM"
#define NEGOTIATE_AUTH_NAME "Negotiate"

/* A structure to hold information about the configuration for the
   mod_auth_ntlm_winbind apache module. */

typedef struct _ntlm_config_struct {
    unsigned int ntlm_on;
    unsigned int negotiate_on;
    unsigned int ntlm_basic_on;
    char *ntlm_basic_realm;
    unsigned int authoritative;
    char *ntlm_auth_helper;
    char *negotiate_ntlm_auth_helper;
    char *ntlm_plaintext_helper;
} ntlm_config_rec;

/* A structure to hold per-connection information about authentications
   that are in progress. */

struct _ntlm_auth_helper {
    int sent_challenge;
    int helper_pid;
#ifdef APACHE2
    apr_proc_t *proc;
#else
    BUFF *out_to_helper, *in_from_helper;
#endif
    apr_pool_t *pool;
};

struct _connected_user_authenticated {
    char *user;
    char *auth_type;
    apr_pool_t *pool;
    int keepalives; /* used to detect redirected auths */
};

struct _ntlm_child_stuff {
    request_rec *r;
    char *argv0;
};

typedef struct _conn_context {
    struct _connected_user_authenticated *connected_user_authenticated;
} ntlm_connection_context_t;

typedef struct _ntlm_context {
    struct _ntlm_auth_helper *ntlm_auth_helper;
    struct _ntlm_auth_helper *negotiate_ntlm_auth_helper;
    struct _ntlm_auth_helper *ntlm_plaintext_helper;
} ntlm_context_t;

#ifdef APACHE2
module AP_MODULE_DECLARE_DATA auth_ntlm_winbind_module;
#else
module MODULE_VAR_EXPORT auth_ntlm_winbind_module;
#endif

#define NTLM_DEBUG (APLOG_DEBUG | APLOG_NOERRNO)

/* If we have already authenticated then allow all subsequence accesses.
   This appears to be what IE and IIS do when talking to each other.  I
   don't think there are any security problems with this, unless someone
   hijacks the connection, but then MS is susceptible to exactly the same
   problem. */

/* Extra apache configuration directives defined for this module */
static const command_rec ntlm_winbind_cmds[] = {
#ifdef APACHE2
    /* NTLM authentication commands */
    AP_INIT_FLAG( "NTLMAuth", ap_set_flag_slot,
                  (void *) APR_OFFSETOF( ntlm_config_rec, ntlm_on ),
                  OR_AUTHCFG,
                  "set to 'on' to activate NTLM authentication" ),

    AP_INIT_FLAG( "NegotiateAuth", ap_set_flag_slot,
                  (void *) APR_OFFSETOF(ntlm_config_rec, negotiate_on),
                  OR_AUTHCFG,
                  "set to 'on' to activate Negotiate authentication" ),

    AP_INIT_FLAG( "NTLMBasicAuthoritative", ap_set_flag_slot,
                  (void *) APR_OFFSETOF(ntlm_config_rec, authoritative),
                  OR_AUTHCFG,
                  "set to 'off' to allow access control to be passed along to lower "
                  "modules if the UserID is not known to this module" ),
    /* ntlm_auth location */
    AP_INIT_TAKE1( "NTLMAuthHelper", ap_set_string_slot,
                   (void *) APR_OFFSETOF(ntlm_config_rec, ntlm_auth_helper),
                   OR_AUTHCFG,
                   "location and arguments to the Samba ntlm_auth utility" ),

    AP_INIT_TAKE1( "NegotiateAuthHelper", ap_set_string_slot,
                   (void *) APR_OFFSETOF(ntlm_config_rec, negotiate_ntlm_auth_helper),
                   OR_AUTHCFG,
                   "location and arguments to the Samba ntlm_auth utility" ),

    AP_INIT_TAKE1( "PlaintextAuthHelper", ap_set_string_slot,
                   (void *) APR_OFFSETOF(ntlm_config_rec, ntlm_plaintext_helper ),
                   OR_AUTHCFG,
                   "location and arguments to the Samba ntlm_auth utility" ),

    /* Basic Authentication transport for non-IE browsers */
    AP_INIT_FLAG( "NTLMBasicAuth", ap_set_flag_slot,
                  (void *) APR_OFFSETOF(ntlm_config_rec, ntlm_basic_on),
                  OR_AUTHCFG,
                  "set to 'on' to allow Basic authentication too" ),

    AP_INIT_TAKE1( "NTLMBasicRealm", ap_set_string_slot,
                   (void *) APR_OFFSETOF(ntlm_config_rec, ntlm_basic_realm),
                   OR_AUTHCFG, "realm to use for Basic authentication" ),

#else
    /* NTLM authentication commands */

    { "NTLMAuth", ap_set_flag_slot,
      (void *) XtOffsetOf(ntlm_config_rec, ntlm_on),
      OR_AUTHCFG, FLAG, "set to 'on' to activate NTLM authentication" },

    { "NegotiateAuth", ap_set_flag_slot,
      (void *) XtOffsetOf(ntlm_config_rec, negotiate_on),
      OR_AUTHCFG, FLAG, "set to 'on' to activate Negotiate authentication" },

    { "NTLMBasicAuthoritative", ap_set_flag_slot,
      (void *) XtOffsetOf(ntlm_config_rec, authoritative),
      OR_AUTHCFG, FLAG,
      "set to 'off' to allow access control to be passed along to lower "
      "modules if the UserID is not known to this module" },

    /* ntlm_auth location */

    { "NTLMAuthHelper", ap_set_string_slot,
      (void *) XtOffsetOf(ntlm_config_rec, ntlm_auth_helper), OR_AUTHCFG,
      TAKE1, "location and arguments to the Samba ntlm_auth utility"},

    { "NegotiateAuthHelper", ap_set_string_slot,
      (void *) XtOffsetOf(ntlm_config_rec, negotiate_ntlm_auth_helper), OR_AUTHCFG,
      TAKE1, "location and arguments to the Samba ntlm_auth utility"},

    { "PlaintextAuthHelper", ap_set_string_slot,
      (void *) XtOffsetOf(ntlm_config_rec, ntlm_plaintext_helper), OR_AUTHCFG,
      TAKE1, "location and arguments to the Samba ntlm_auth utility"},

    /* Basic Authentcation transport for non-IE browsers */

    { "NTLMBasicAuth", ap_set_flag_slot,
      (void *) XtOffsetOf(ntlm_config_rec, ntlm_basic_on),
      OR_AUTHCFG, FLAG, "set to 'on' to allow Basic authentication too" },

    { "NTLMBasicRealm", ap_set_string_slot,
      (void *) XtOffsetOf(ntlm_config_rec, ntlm_basic_realm),
      OR_AUTHCFG, TAKE1, "realm to use for Basic authentication" },
#endif

    { NULL }
};

/* both apache1 and apache2 use this to maintain per-child context */
static ntlm_context_t global_ntlm_context;

#ifndef APACHE2
/* apache 1 doesn't seem to have a connection context I can use */
static ntlm_connection_context_t global_connection_context;

static void cleanup_ntlm_auth_helper(void *ntlm_conn_v)
{
    struct _ntlm_auth_helper *ntlm_conn = ntlm_conn_v;

    ap_log_error( APLOG_MARK, NTLM_DEBUG, NULL, "freeing ntlm_helper" );

    ap_bclose(ntlm_conn->out_to_helper);
    ap_bclose(ntlm_conn->in_from_helper);

    global_ntlm_context.ntlm_auth_helper = NULL;
}

static void cleanup_negotiate_ntlm_auth_helper(void *ntlm_conn_v)
{
    struct _ntlm_auth_helper *ntlm_conn = ntlm_conn_v;

    ap_log_error( APLOG_MARK, NTLM_DEBUG, NULL, "freeing negotiate_helper" );

    ap_bclose(ntlm_conn->out_to_helper);
    ap_bclose(ntlm_conn->in_from_helper);

    global_ntlm_context.negotiate_ntlm_auth_helper = NULL;
}

static void cleanup_ntlm_plaintext_helper( void *ntlm_conn_v ) {
    struct _ntlm_auth_helper *ntlm_conn = ntlm_conn_v;

    ap_log_error( APLOG_MARK, NTLM_DEBUG, NULL, "freeing plaintext_helper" );

    ap_bclose( ntlm_conn->out_to_helper );
    ap_bclose( ntlm_conn->in_from_helper );

    global_ntlm_context.ntlm_plaintext_helper = NULL;
}

/* Dispose of a connected user */

static void cleanup_connected_user_authenticated(void *unused)
{
    ap_log_error( APLOG_MARK, NTLM_DEBUG, NULL, "freeing user" );
    global_connection_context.connected_user_authenticated = NULL;
}
#endif

/* helper function to pull out the context data */
static ntlm_connection_context_t *get_connection_context( struct conn_rec *connection ) {
    ntlm_connection_context_t *retval = NULL;

#ifdef APACHE2
    retval = (ntlm_connection_context_t *)ap_get_module_config( connection->conn_config,
                                                                &auth_ntlm_winbind_module );
#else
    retval = &global_connection_context;
#endif
    return retval;
}

/* Authorisation has failed - we set some headers so the client can
   get the hint and prompt for a password from the user. */

static int
note_auth_failure(request_rec * r, const char *negotiate_auth_line)
{
    ntlm_config_rec *crec
        = (ntlm_config_rec *) ap_get_module_config(r->per_dir_config,
                                                   &auth_ntlm_winbind_module);
    char *line;
    ntlm_connection_context_t *ctxt = get_connection_context( r->connection );

    /* MSIE will simply reply to the first, not strongest, protocol listed */
    if (crec->negotiate_on) {
        line = apr_pstrcat(r->pool, NEGOTIATE_AUTH_NAME, " ",
                           negotiate_auth_line, NULL);
        apr_table_add(r->err_headers_out,
                      (PROXYREQ_PROXY == r->proxyreq) ? "Proxy-Authenticate" : "WWW-Authenticate",
                      line);
    }

    /* Set header for NTLM authentication */

    if (crec->ntlm_on) {
        apr_table_add(r->err_headers_out,
                      (PROXYREQ_PROXY == r->proxyreq) ? "Proxy-Authenticate" : "WWW-Authenticate",
                      NTLM_AUTH_NAME);
    }

    /* Set header for basic authentication so client can use this if
       supported. */

    if (crec->ntlm_basic_on) {
        line = apr_pstrcat(r->pool,
                           "Basic realm=\"", crec->ntlm_basic_realm, "\"",
                           NULL);
        apr_table_add(r->err_headers_out,
                      (PROXYREQ_PROXY == r->proxyreq) ? "Proxy-Authenticate" : "WWW-Authenticate",
                      line);
    }

    if ( ctxt->connected_user_authenticated &&
         ctxt->connected_user_authenticated->pool ) {
        apr_pool_destroy( ctxt->connected_user_authenticated->pool );
	ctxt->connected_user_authenticated = NULL;
    }

    return HTTP_UNAUTHORIZED;
}


const char *
get_auth_header(request_rec * r, ntlm_config_rec * crec, const char *auth_scheme)
{
    const char *auth_line = apr_table_get(r->headers_in,
                                          (PROXYREQ_PROXY == r->proxyreq) ? "Proxy-Authorization"
                                          : "Authorization");

    if (!auth_line) {
        RERROR( APR_EINIT, "no auth line present" );
        return NULL;
    }
    if (strcmp(ap_getword_white(r->pool, &auth_line), auth_scheme)) {
        RERROR( APR_EINIT, "%s auth name not present", auth_scheme );
        return NULL;
    }
    return auth_line;
}

#ifndef APACHE2
static int helper_child(void *child_stuff, child_info *pinfo)
{
    struct _ntlm_child_stuff *cld = (struct _ntlm_child_stuff *) child_stuff;
    request_rec *r = cld->r;
    char *argv0 = cld->argv0;
    int child_pid;

    RAISE_SIGSTOP(CGI_CHILD);

    /* Transmute outselves into the helper
     */

    ap_cleanup_for_exec();

    child_pid = ap_call_exec(r, pinfo, argv0, NULL, 1);

    /* Uh oh.  Still here.  Where's the kaboom?  There was supposed to be an
     * EARTH-shattering kaboom!
     *
     * Oh, well.  Muddle through as best we can...
     *
     * Note that only stderr is available at this point, so don't pass in
     * a server to aplog_error.
     */

    RERROR( errno, "exec of %s failed", r->filename);
    exit(0);
    /* NOT REACHED */
    return (0);
}
#endif


static int
send_auth_reply(request_rec * r, const char *auth_scheme, const char *reply)
{
    RDEBUG( "sending back %s", reply );
    /* Read negotiate from ntlm_auth */

    apr_table_setn(r->err_headers_out,
                  (PROXYREQ_PROXY == r->proxyreq) ? "Proxy-Authenticate" : "WWW-Authenticate",
                  apr_psprintf(r->pool, "%s %s", auth_scheme, reply));

    /* This is to make sure that when receiving later messages
     * that the r->connection is still alive after sending the
     * nonce to the client.  This code is written, and the problem
     * was pointed out by Michael Cai.
     */
    if(r->connection->keepalives >= r->server->keep_alive_max)
    {
        RDEBUG("Decrement the connection request count to keep it alive");
        r->connection->keepalives -= 1;
    }

    return HTTP_UNAUTHORIZED;
}

/* get the current request's auth helper or fork one */
static struct _ntlm_auth_helper *get_auth_helper( request_rec *r, struct _ntlm_auth_helper *auth_helper, char *cmd, void (*cleanup)(void *)) {
#ifdef APACHE2
    apr_procattr_t *attr;
#endif

    if ( auth_helper == NULL ) {
        struct _ntlm_child_stuff cld;
        apr_pool_t *pool;
#ifdef APACHE2
        char **argv_out;
        apr_pool_create_ex( &pool, NULL, NULL, NULL ); /* xxx return code */
#else
        pool = ap_make_sub_pool( NULL );
#endif
        auth_helper = apr_pcalloc( pool, sizeof( struct _ntlm_auth_helper ));
        auth_helper->pool = pool;
        auth_helper->helper_pid = 0;

#ifdef APACHE2
        apr_tokenize_to_argv( cmd, &argv_out, pool );
#else
        ap_register_cleanup( pool, auth_helper, cleanup, ap_null_cleanup );
#endif
        cld.argv0 = cmd;
        cld.r = r;

#ifdef APACHE2
        apr_procattr_create( &attr, pool );
        apr_procattr_io_set( attr, APR_FULL_BLOCK, APR_FULL_BLOCK, APR_NO_PIPE );
        apr_procattr_error_check_set( attr, 1 );
        auth_helper->proc = (apr_proc_t *)apr_pcalloc(pool, sizeof(apr_proc_t)) ;
        if ( apr_proc_create( auth_helper->proc, argv_out[0], (const char * const *)argv_out, NULL, attr, pool ) != APR_SUCCESS ) {
            RERROR( errno, "couldn't spawn child ntlm helper process: %s", argv_out[0]);
            return NULL;
        }
        auth_helper->helper_pid = auth_helper->proc->pid;
#else
        auth_helper->helper_pid = ap_bspawn_child(pool, helper_child,
                                                  (void *) &cld, just_wait,
                                                  &auth_helper->out_to_helper,
                                                  &auth_helper->in_from_helper,
                                                  NULL);

        if (auth_helper->helper_pid == -1) {
            RERROR( errno, "couldn't spawn child ntlm helper process: %s", cld.argv0);
            return NULL;
        }
#endif

        RDEBUG( "Launched ntlm_helper, pid %d", auth_helper->helper_pid );
    } else {
        RDEBUG( "Using existing auth helper %d", auth_helper->helper_pid );
    }

    return auth_helper;
}

/* Call winbind to authenticate a (user, password)
   pair */
static int winbind_authenticate_plaintext( request_rec *r, ntlm_config_rec * crec, char *user, char *pass)
{
    ntlm_connection_context_t *ctxt = get_connection_context( r->connection );
    char *newline;
    char args_to_helper[HUGE_STRING_LEN];
    char args_from_helper[HUGE_STRING_LEN];
    size_t bytes_written;
    int bytes_read;

    if (( global_ntlm_context.ntlm_plaintext_helper = get_auth_helper( r, global_ntlm_context.ntlm_plaintext_helper, crec->ntlm_plaintext_helper, CLEANUP(cleanup_ntlm_plaintext_helper))) == NULL ) {
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    if ( ctxt->connected_user_authenticated == NULL ) {
        apr_pool_t *pool;

        RDEBUG( "creating auth user" );

#ifdef APACHE2
        apr_pool_create_ex( &pool, r->connection->pool, NULL, NULL );
#else
        pool = ap_make_sub_pool(r->connection->pool);
#endif

        ctxt->connected_user_authenticated =
            apr_pcalloc(pool, sizeof( struct _connected_user_authenticated));

#ifndef APACHE2
        ap_register_cleanup(pool,ctxt->connected_user_authenticated,
                            cleanup_connected_user_authenticated, ap_null_cleanup );
#endif

        ctxt->connected_user_authenticated->pool = pool;
        ctxt->connected_user_authenticated->user = NULL;
        ctxt->connected_user_authenticated->auth_type = NULL;
    } else {
        /* what, we're already authenticated? */
        return OK;
    }

    snprintf( args_to_helper, HUGE_STRING_LEN, "%s %s\n", user, pass );

#ifdef APACHE2
    bytes_written = strlen( args_to_helper );
    apr_file_write( global_ntlm_context.ntlm_plaintext_helper->proc->in, args_to_helper, &bytes_written );
#else
    bytes_written = ap_bwrite( global_ntlm_context.ntlm_plaintext_helper->out_to_helper, args_to_helper, strlen( args_to_helper ));
#endif

    if ( bytes_written < strlen( args_to_helper )) {
        RDEBUG( "failed to write user/pass to helper - wrote %d bytes", (int) bytes_written );
        apr_pool_destroy( global_ntlm_context.ntlm_plaintext_helper->pool );
        apr_pool_destroy( ctxt->connected_user_authenticated->pool );
        return HTTP_INTERNAL_SERVER_ERROR;
    }

#ifdef APACHE2
    apr_file_flush( global_ntlm_context.ntlm_plaintext_helper->proc->in );
    if ( apr_file_gets( args_from_helper, HUGE_STRING_LEN, global_ntlm_context.ntlm_plaintext_helper->proc->out ) == APR_SUCCESS ) {
        bytes_read = strlen( args_from_helper );
    } else {
        bytes_read = 0;
    }
#else
    ap_bflush( global_ntlm_context.ntlm_plaintext_helper->out_to_helper );
    bytes_read = ap_bgets( args_from_helper, HUGE_STRING_LEN, global_ntlm_context.ntlm_plaintext_helper->in_from_helper );
#endif
    if ( bytes_read == 0 ) {
        RERROR( errno, "early EOF from helper" );
        apr_pool_destroy(global_ntlm_context.ntlm_plaintext_helper->pool);
        apr_pool_destroy(ctxt->connected_user_authenticated->pool);

        return HTTP_INTERNAL_SERVER_ERROR;
    } else if (bytes_read == -1) {
        RERROR( errno, "helper died!" );
        apr_pool_destroy(global_ntlm_context.ntlm_plaintext_helper->pool);
        apr_pool_destroy(ctxt->connected_user_authenticated->pool);

        return HTTP_INTERNAL_SERVER_ERROR;
    } else if (bytes_read < 2) {
        RERROR( errno, "failed to read NTLMSSP string from helper - only got %d bytes", bytes_read);
        apr_pool_destroy(global_ntlm_context.ntlm_plaintext_helper->pool);
        apr_pool_destroy(ctxt->connected_user_authenticated->pool);

        return HTTP_INTERNAL_SERVER_ERROR;
    }

    newline = strchr(args_from_helper, '\n');
    if (newline != NULL) {
        *newline = '\0';
    }

    RDEBUG( "got response: %s", args_from_helper );

    if ( strncmp( args_from_helper, "OK", 2 ) == 0 ) {
        RDEBUG( "authentication succeeded!" );
        ctxt->connected_user_authenticated->user = apr_pstrdup(ctxt->connected_user_authenticated->pool, user);
        ctxt->connected_user_authenticated->keepalives = r->connection->keepalives;
#ifdef APACHE2
        r->user = ctxt->connected_user_authenticated->user;
        r->ap_auth_type = apr_pstrdup(r->connection->pool, "Basic");
        /* disconnect the child process */
        /*        apr_proc_kill( global_ntlm_context.ntlm_plaintext_helper->proc, 9 );
                  apr_proc_wait( global_ntlm_context.ntlm_plaintext_helper->proc, &exit, &why, APR_WAIT );*/
#else
        r->connection->user = ctxt->connected_user_authenticated->user;
        r->connection->ap_auth_type = ap_pstrdup(r->connection->pool, "Basic");
#endif
        RDEBUG( "authenticated %s", ctxt->connected_user_authenticated->user );
        return  OK;
    } else {
        if ( strncmp( args_from_helper, "ERR", 3 ) == 0 ) {
            RDEBUG( "username/password incorrect" );
            return note_auth_failure( r, NULL );
        } else {
            RDEBUG( "unknown helper response %s", args_from_helper );
            return HTTP_INTERNAL_SERVER_ERROR;
        }
    }
}

/* Process a message received from the client.  This can be a request for a
   challenge (type 1) or a request to authenticate a challenge/response
   (type 3). */

static int
process_msg(request_rec * r, ntlm_config_rec * crec, const char *auth_type)
{
    const char *client_msg;
    const char *message_type;
    char *childarg;
    char *newline;
    char args_to_helper[HUGE_STRING_LEN];
    char args_from_helper[HUGE_STRING_LEN];
    ntlm_connection_context_t *ctxt = get_connection_context( r->connection );
    size_t bytes_written;
    int bytes_read;
    struct _ntlm_auth_helper *auth_helper;

    /* If this is the first request with this connection, then create
     * a ntlm_auth_helper entry for it. It will be cleaned up when the
     * connection is dropped */

    if (strcmp(auth_type, NEGOTIATE_AUTH_NAME) == 0) {
        auth_helper = get_auth_helper( r, global_ntlm_context.negotiate_ntlm_auth_helper, crec->negotiate_ntlm_auth_helper, CLEANUP(cleanup_negotiate_ntlm_auth_helper));
        global_ntlm_context.negotiate_ntlm_auth_helper = auth_helper;
    } else if (strcmp(auth_type, NTLM_AUTH_NAME) == 0) {
        auth_helper = get_auth_helper( r, global_ntlm_context.ntlm_auth_helper, crec->ntlm_auth_helper, CLEANUP(cleanup_ntlm_auth_helper));
        global_ntlm_context.ntlm_auth_helper = auth_helper;
    } else {
        auth_helper = NULL;
    }

    if ( auth_helper == NULL ) {
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    if ( ctxt->connected_user_authenticated == NULL ) {
        apr_pool_t *pool;

        RDEBUG( "creating auth user" );

#ifdef APACHE2
        apr_pool_create_ex( &pool, r->connection->pool, NULL, NULL );
#else
        pool = ap_make_sub_pool(r->connection->pool);
#endif

        ctxt->connected_user_authenticated =
            apr_pcalloc(pool, sizeof( struct _connected_user_authenticated));

#ifndef APACHE2
        ap_register_cleanup(pool,ctxt->connected_user_authenticated,
                            cleanup_connected_user_authenticated, ap_null_cleanup );
#endif

        ctxt->connected_user_authenticated->pool = pool;
        ctxt->connected_user_authenticated->user = NULL;
        ctxt->connected_user_authenticated->auth_type = NULL;

        message_type = "YR";
    } else {
        message_type = "KK";
    }

    /* Decode the information the WWW-Authenticate header */
    if ((client_msg = get_auth_header(r, crec, auth_type)) == NULL) {
        RDEBUG( "client did not return NTLM authentication header");
        return note_auth_failure(r, NULL);
    }

    /* Pipe to helper */
    snprintf(args_to_helper, HUGE_STRING_LEN, "%s %s\n", message_type, client_msg);

#ifdef APACHE2
    bytes_written = strlen( args_to_helper );
    apr_file_write( auth_helper->proc->in, args_to_helper, &bytes_written );
#else
    bytes_written = ap_bwrite(auth_helper->out_to_helper, args_to_helper, strlen(args_to_helper));
#endif
    if (bytes_written < strlen(args_to_helper)) {
        RDEBUG("failed to write NTLMSSP string to helper - wrote %d bytes", (int) bytes_written);
        apr_pool_destroy(auth_helper->pool);
        apr_pool_destroy(ctxt->connected_user_authenticated->pool);

        return HTTP_INTERNAL_SERVER_ERROR;
    }

#ifdef APACHE2
    apr_file_flush( auth_helper->proc->in );

    RDEBUG( "parsing reply from helper to %s", args_to_helper );

    if ( apr_file_gets(args_from_helper, HUGE_STRING_LEN, auth_helper->proc->out ) == APR_SUCCESS ) {
        bytes_read = strlen( args_from_helper );
    } else {
        bytes_read = 0;
    }
#else
    ap_bflush(auth_helper->out_to_helper);

    bytes_read = ap_bgets(args_from_helper, HUGE_STRING_LEN, auth_helper->in_from_helper);
#endif

    if (bytes_read == 0) {
        RERROR( errno, "early EOF from helper" );
        apr_pool_destroy(auth_helper->pool);
        apr_pool_destroy(ctxt->connected_user_authenticated->pool);

        return HTTP_INTERNAL_SERVER_ERROR;
    } else if (bytes_read == -1) {
        RERROR( errno, "helper died!");
        apr_pool_destroy(auth_helper->pool);
        apr_pool_destroy(ctxt->connected_user_authenticated->pool);

        return HTTP_INTERNAL_SERVER_ERROR;
    } else if (bytes_read < 2) {
        RERROR( errno, "failed to read NTLMSSP string from helper - only got %d bytes", bytes_read );
        apr_pool_destroy(auth_helper->pool);
        apr_pool_destroy(ctxt->connected_user_authenticated->pool);

        return HTTP_INTERNAL_SERVER_ERROR;
    }

    newline = strchr(args_from_helper, '\n');
    if (newline != NULL) {
        *newline = '\0';
    }

    RDEBUG( "got response: %s", args_from_helper );

    /* inspect message type */

    childarg = strchr(args_from_helper, ' ');
    if (childarg == NULL) {
        RERROR( errno, "failed to parse response from helper");
        apr_pool_destroy(auth_helper->pool);
        apr_pool_destroy(ctxt->connected_user_authenticated->pool);

        return HTTP_INTERNAL_SERVER_ERROR;
    }
    childarg++;

    if (strcasecmp(auth_type, NTLM_AUTH_NAME) == 0) {
        /* if TT, send to client */

        if (strncmp(args_from_helper, "TT ", 3) == 0) {
            return send_auth_reply(r, auth_type, childarg);
        }

        /* if NA, not authenticated */

        if (strncmp(args_from_helper, "NA ", 3) == 0) {
            RDEBUG("user not authenticated: %s", childarg);
            return note_auth_failure(r, NULL);
        }

        /* if AF, record username */
        if (strncmp(args_from_helper, "AF ", 3) == 0) {
            ctxt->connected_user_authenticated->user =
                apr_pstrdup(ctxt->connected_user_authenticated->pool,
                            childarg);
            ctxt->connected_user_authenticated->keepalives =
                r->connection->keepalives;
#ifdef APACHE2
            r->user = ctxt->connected_user_authenticated->user;
            r->ap_auth_type = apr_pstrdup(r->connection->pool, auth_type);
            /* disconnect the child process */
            /*            apr_proc_kill( auth_helper->proc, 9 );
                          apr_proc_wait( auth_helper->proc, &exit, &why, APR_WAIT );*/
#else
            r->connection->user = ctxt->connected_user_authenticated->user;
            r->connection->ap_auth_type = ap_pstrdup(r->connection->pool, auth_type);
#endif
            RDEBUG( "authenticated %s",
                    ctxt->connected_user_authenticated->user );
            return OK;
        }
    } else if (strcasecmp(auth_type, NEGOTIATE_AUTH_NAME) == 0) {

    /* The child's reply contains 3 parts:
       - The code: TT, AF or NA
       - The blob to send to the client, coded in base64
       - The argument:
             For TT it's a dummy '*'
         For AF it's domain\\user
         For NA it's the NT error code
    */

        char *childarg3 = strchr(childarg, ' ');
        if (childarg3 == NULL) {
            RERROR( errno, "failed to parse response from helper");
            apr_pool_destroy(auth_helper->pool);
            apr_pool_destroy(ctxt->connected_user_authenticated->pool);

            return HTTP_INTERNAL_SERVER_ERROR;
        }
        *childarg3 = '\0';
        childarg3++;

        /* if TT, send to client */

        if (strncmp(args_from_helper, "TT ", 3) == 0) {
            return send_auth_reply(r, auth_type, childarg);
        }

        /* if NA, not authenticated */

        if (strncmp(args_from_helper, "NA ", 3) == 0) {
            RDEBUG("user not authenticated: %s", childarg3);
            return note_auth_failure(r, childarg);
        }

        /* if AF, record username */
        if (strncmp(args_from_helper, "AF ", 3) == 0) {
            ctxt->connected_user_authenticated->user =
                apr_pstrdup(ctxt->connected_user_authenticated->pool,
                            childarg3);
#ifdef APACHE2
            r->user = ctxt->connected_user_authenticated->user;
            ctxt->connected_user_authenticated->auth_type =
                apr_pstrdup(r->connection->pool, auth_type);
            r->ap_auth_type = ctxt->connected_user_authenticated->auth_type;
#else
            r->connection->user = ctxt->connected_user_authenticated->user;
            ctxt->connected_user_authenticated->auth_type = ap_pstrdup(r->connection->pool, auth_type);
            r->connection->ap_auth_type = ctxt->connected_user_authenticated->auth_type;
#endif

            if (strcmp("*", childarg) != 0) {
                /* Send last leg (possible mutual authentication token) */
                apr_table_setn(r->headers_out,
                              (PROXYREQ_PROXY == r->proxyreq) ? "Proxy-Authenticate" : "WWW-Authenticate",
                              apr_psprintf(r->pool, "%s %s", auth_type, childarg));
            }

            RDEBUG( "wow, we're all happy here" );

            return OK;
        }
    }

    /* Helper failed */

    /* if BH, helper is busted */

    if (strncmp(args_from_helper, "BH ", 3) == 0) {
        RERROR( APR_EGENERAL, "ntlm_auth reports Broken Helper: %s", args_from_helper);
    } else {
        RERROR( APR_EGENERAL, "could not parse %s helper callback: %s", auth_type, args_from_helper);
    }

    apr_pool_destroy(auth_helper->pool);
    apr_pool_destroy(ctxt->connected_user_authenticated->pool);

    return HTTP_INTERNAL_SERVER_ERROR;
}

/* Called to create a configuration structure for each <Directory> section
   that uses the ntlm auth module. */

static void *
ntlm_winbind_dir_config(apr_pool_t * p, char *d)
{
    ntlm_config_rec *crec
        = (ntlm_config_rec *) apr_pcalloc(p, sizeof(ntlm_config_rec));

    /* Set the defaults. */

    crec->authoritative = 1;
    crec->ntlm_on = 0;
    crec->negotiate_on = 0;
    crec->ntlm_basic_on = 0;
    crec->ntlm_basic_realm = "REALM";
    crec->ntlm_auth_helper = "ntlm_auth --helper-protocol=squid-2.5-ntlmssp";
    crec->negotiate_ntlm_auth_helper = "ntlm_auth --helper-protocol=gss-spnego";
    crec->ntlm_plaintext_helper = "ntlm_auth --helper-protocol=squid-2.5-basic";

    return crec;
}

/* Authenticate a user using basic authentication */
static int
authenticate_basic_user(request_rec * r, ntlm_config_rec * crec,
                        const char *auth_line_after_Basic)
{
    char *sent_user = NULL, *sent_pw;
    int result = HTTP_UNAUTHORIZED;

    while (*auth_line_after_Basic == ' ' || *auth_line_after_Basic == '\t')
        auth_line_after_Basic++;

#ifdef APACHE2
    sent_user = apr_pcalloc( r->pool, apr_base64_decode_len( auth_line_after_Basic ));
    apr_base64_decode( sent_user, auth_line_after_Basic );
#else
    sent_user = ap_pbase64decode(r->pool, auth_line_after_Basic);
#endif

    if (sent_user != NULL) {
        char *s;

        if ((sent_pw = strchr(sent_user, ':')) != NULL) {
            *sent_pw = '\0';
            ++sent_pw;
        } else
            sent_pw = "";

        result = winbind_authenticate_plaintext( r, crec, sent_user, sent_pw);
    } else {
        RDEBUG("can't extract user from %s", auth_line_after_Basic );
        sent_user = sent_pw = "";
    }

    RDEBUG("authenticate user %s: %s", sent_user,
           (result == OK) ? "OK" : "FAILED");
    return result;
}

/* Check the user id from a http request */
static int check_user_id(request_rec * r) {
    ntlm_config_rec *crec =
        (ntlm_config_rec *) ap_get_module_config(r->per_dir_config,
                                                 &auth_ntlm_winbind_module);
    ntlm_connection_context_t *ctxt = get_connection_context( r->connection );
    const char *auth_line = apr_table_get(r->headers_in,
                                          (PROXYREQ_PROXY == r->proxyreq) ? "Proxy-Authorization"
                                          : "Authorization");
    const char *auth_line2;

    /* Trust the authentication on an existing connection */
    if (ctxt->connected_user_authenticated && ctxt->connected_user_authenticated->user) {
        /* internal redirects cause this to get called more than once
           per request on Apache 1.x. This compensates by checking if
           the connection is the same as the one we authed against */
        if ( !auth_line || ( ctxt->connected_user_authenticated->keepalives == r->connection->keepalives )) {
            /* silently accept login with same credentials */
            RDEBUG( "retaining user %s",
                    ctxt->connected_user_authenticated->user );
            RDEBUG( "keepalives: %d", r->connection->keepalives );
#ifdef APACHE2
            r->user = ctxt->connected_user_authenticated->user;
            r->ap_auth_type = ctxt->connected_user_authenticated->auth_type;
#else
            r->connection->user = ctxt->connected_user_authenticated->user;
            r->connection->ap_auth_type = ctxt->connected_user_authenticated->auth_type;
#endif
            return OK;
        } else {
            RDEBUG( "reauth" );
            /* client wishes to re-authenticate this TCP socket */
            if ( ctxt->connected_user_authenticated->pool ) {
                apr_pool_destroy(ctxt->connected_user_authenticated->pool);
                ctxt->connected_user_authenticated = NULL;
            }
        }
    }

    /* No authentication line given.  Return a 401 and a WWW-Authenticate
       header so authentication can commence. */

    if (!auth_line) {
        note_auth_failure(r, NULL);
        return HTTP_UNAUTHORIZED;
    }

    /* If basic authentication is requested and enabled, try to
       authenticate the user with basic */

    auth_line2 = auth_line;
    if (crec->ntlm_basic_on
        && strcasecmp(ap_getword(r->pool, &auth_line2, ' '), "Basic") == 0) {
        RDEBUG( "trying basic auth" );
        return authenticate_basic_user(r, crec, &auth_line[5]); /* seems clunky */
    }

    /* Process a 'Negotiate' SPNEGO over http message */
    auth_line2 = auth_line;
    if (strcasecmp(ap_getword(r->pool, &auth_line2, ' '), NEGOTIATE_AUTH_NAME) == 0) {
        if (!crec->negotiate_on) {
            RDEBUG("Negotiate authentication is not enabled");
            return DECLINED;
        } else {
            return process_msg(r, crec, NEGOTIATE_AUTH_NAME);
        }
    }

    /* Process a NTLM over http message */

    auth_line2 = auth_line;
    if (strcasecmp(ap_getword(r->pool, &auth_line2, ' '), NTLM_AUTH_NAME) == 0) {
        if (!crec->ntlm_on) {
            RDEBUG("NTLM authentication is not enabled");
            return DECLINED;
        } else {
            RDEBUG( "doing ntlm auth dance" );
            return process_msg(r, crec, NTLM_AUTH_NAME);
        }
    }

    if (ctxt->connected_user_authenticated && ctxt->connected_user_authenticated->pool ) {
        apr_pool_destroy(ctxt->connected_user_authenticated->pool);
	ctxt->connected_user_authenticated = NULL;
    }

    RDEBUG( "declined" );

    return DECLINED;
}

/* Dispatch list for API hooks */
#ifdef APACHE2
static int ntlm_pre_conn(conn_rec *c, void *csd) {
    ntlm_connection_context_t *ctxt = apr_pcalloc(c->pool, sizeof(ntlm_connection_context_t));

    ap_set_module_config(c->conn_config, &auth_ntlm_winbind_module, ctxt);

    return OK;
}


static void register_hooks(apr_pool_t *pool)
{
    ap_hook_pre_connection(ntlm_pre_conn,NULL,NULL,APR_HOOK_MIDDLE);
    ap_hook_check_user_id(check_user_id,NULL,NULL,APR_HOOK_MIDDLE);
};

module AP_MODULE_DECLARE_DATA auth_ntlm_winbind_module = {
    STANDARD20_MODULE_STUFF,
    ntlm_winbind_dir_config, /* create per-dir    config structures */
    NULL,                    /* merge  per-dir    config structures */
    NULL,                    /* create per-server config structures */
    NULL,                    /* merge  per-server config structures */
    ntlm_winbind_cmds,       /* table of config file commands       */
    register_hooks,          /* register hooks */
};
#else
module MODULE_VAR_EXPORT auth_ntlm_winbind_module = {
    STANDARD_MODULE_STUFF,
    NULL,                    /* module initializer                  */
    ntlm_winbind_dir_config, /* create per-dir    config structures */
    NULL,                    /* merge  per-dir    config structures */
    NULL,                    /* create per-server config structures */
    NULL,                    /* merge  per-server config structures */
    ntlm_winbind_cmds,       /* table of config file commands       */
    NULL,                    /* [#8] MIME-typed-dispatched handlers */
    NULL,                    /* [#1] URI to filename translation    */
    check_user_id,           /* [#4] validate user id from request  */
    NULL,                    /* [#5] check if the user is ok _here_ */
    NULL,                    /* [#3] check access by host address   */
    NULL,                    /* [#6] determine MIME type            */
    NULL,                    /* [#7] pre-run fixups                 */
    NULL,                    /* [#9] log a transaction              */
    NULL,                    /* [#2] header parser                  */
    NULL,                    /* child_init                          */
    NULL,                    /* child_exit                          */
    NULL                     /* [#0] post read-request              */
#ifdef EAPI
   ,NULL,                    /* EAPI: add_module                    */
    NULL,                    /* EAPI: remove_module                 */
    NULL,                    /* EAPI: rewrite_command               */
    NULL                     /* EAPI: new_connection                */
#endif
};
#endif

/*
 * Local Variables:
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
