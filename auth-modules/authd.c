/*
 * Simple websocket server to acquire authentication tokens via ssh.
 *
 * Outline:
 * 1. an input of the form user@host:port is received via websockets
 * 2. try to ssh to user@host:port, and save whatever output the server
 *    returns (which should be the auth token)
 * 3. return the auth token back over the websocket
 *
 * NOTE: user@host has nothing to do with the local user's account
 * for the web service.  This account is used to multiplex many user
 * accounts, much like 'git@github.com'.  If the public key is known
 * by the webservice, a token for the associated account is returned.
 * */

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <assert.h>
#include <signal.h>

#include <syslog.h>
#include <sys/time.h>
#include <unistd.h>
#include <ctype.h> /* tolower */

#ifdef CMAKE_BUILD
#include "lws_config.h"
#endif

#include "libwebsockets.h"

#include "common.h" /* info on token size and such. */

#define CHECK_STATES 0

static volatile int force_exit = 0;

static char tfname[64] = "/tmp/.web-sshconf-XXXXXX";
static char keyfile[128] = "";
static char knownhostfile[128] = "~/.ssh/known_hosts-web";

enum req_states {
	rq_start,         /* connection just opened */
	rq_datarcvd,      /* we have the request from the client */
	rq_sending_token, /* response is queued for writing */
	rq_error,         /* protocol was violated */
	rq_done           /* data sent; closing connection */
};
struct per_session_data__auth {
	unsigned char buf[LWS_SEND_BUFFER_PRE_PADDING +
		MAX_TOKEN_LEN + 3 + /* first 3 bytes used for the return code */
		LWS_SEND_BUFFER_POST_PADDING];
	unsigned int len;
	unsigned int state;
};

/* store the origin header, and the corresponding websocket: */
char origin[1024]; /* who is connecting to us? */
struct libwebsocket* origin_ws;
static int
callback_acquire(struct libwebsocket_context *context,
		struct libwebsocket *wsi,
		enum libwebsocket_callback_reasons reason, void *user,
							   void *in, size_t len)
{
	struct per_session_data__auth *psd = (struct per_session_data__auth *)user;
	int n;
	FILE* tsshconf;
	char headerbuf[1024];

	switch (reason) {

	case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION:
		fprintf(stderr, "During FILTER_PROTOCOL_CONNECTION callback:\n");
		bzero(headerbuf,1024);
		lws_hdr_copy(wsi, headerbuf, 1024, WSI_TOKEN_ORIGIN);
		fprintf(stderr, "WSI_TOKEN_ORIGIN: %s.\n",headerbuf);
		/* save these for later: */
		strncpy(origin,headerbuf,1024); origin_ws = wsi;
		lws_hdr_copy(wsi, headerbuf, 1024, WSI_TOKEN_SWORIGIN);
		fprintf(stderr, "WSI_TOKEN_SWORIGIN: %s.\n",headerbuf);
		lws_hdr_copy(wsi, headerbuf, 1024, WSI_TOKEN_HOST);
		fprintf(stderr, "WSI_TOKEN_HOST: %s.\n",headerbuf);
		lws_hdr_copy(wsi, headerbuf, 1024, WSI_TOKEN_PROTOCOL);
		fprintf(stderr, "WSI_TOKEN_PROTOCOL: %s.\n",headerbuf);
		lws_hdr_copy(wsi, headerbuf, 1024, WSI_TOKEN_CONNECTION);
		fprintf(stderr, "WSI_TOKEN_CONNECTION: %s.\n",headerbuf);
		break;

	#if CHECK_STATES
	case LWS_CALLBACK_WSI_CREATE:
		// fprintf(stderr, "we got a wsi create callback.\n");
		/* this seems to happen on every new connection (session).
		 * I guess this is the place to initialize the session data.
		 * AFAICT, it has already been malloc'd. */
		/* XXX guess again!  following line gives a seg fault.  */
		psd->state = rq_start;
		break;
	#endif

	case LWS_CALLBACK_SERVER_WRITEABLE:
		#if CHECK_STATES
		if (psd->state != rq_sending_token) {
			/* we shouldn't be here -- this is the only thing
			 * we ever write. */
			fprintf(stderr,"Writing at the wrong time\n");
			psd->state = rq_error;
			return 1;
		}
		#endif
		n = libwebsocket_write(wsi, &psd->buf[LWS_SEND_BUFFER_PRE_PADDING],
				psd->len, LWS_WRITE_BINARY);
		if (n < 0) {
			lwsl_err("ERROR %d writing to socket, hanging up\n", n);
			return 1;
		}
		if (n < (int)psd->len) {
			lwsl_err("Partial write\n");
			return -1;
		}
		/* XXX: the bs with the req_states seems unnecessary */
		#if CHECK_STATES
		psd->state = rq_done;
		#endif
		break;

	case LWS_CALLBACK_RECEIVE:
		/* check psd->state to know what message to expect.  At the moment,
		 * there should only be one message to us from the browser: */
		#if CHECK_STATES
		if (psd->state != rq_start) {
			psd->state = rq_error;
			lwsl_err("Unexpected packet. Hanging up.");
			return 1;
		}
		psd->state = rq_datarcvd;
		#endif
		/* user / port info should be on the wire, host is in the 'origin'
		 * header (but it might take a little parsing to extract it). */
		/* For user id's, we'll stick to something like C identifiers.
		 * For clarity, the message should have the following format:
		 * user:<username>
		 * port:<port>
		 * These lines can come in any order, but must have one key:value
		 * pair per line.
		 * */
		/* quick sanity check on len: should be less than 12+32+5 = 49
		 * and has to be at least 7 chars (user is one char, port was left out
		 * completely). */
		if (len < 7 || 49 < len) {
			#if CHECK_STATES
			psd->state = rq_error;
			#endif
			lwsl_err("Strange sized packet (%lu bytes) o_O Hanging up.",len);
			return 1;
		}
		char* citem = (char*)in;
		char* in_char = (char*)in;
		citem[len-1] = 0;  /* make sure input is a c-string */

#define nparams 2
		char* hostname; /* we get this from origin header. */
		char *username, *port;
		char* labels[nparams] = { "user:","port:" };
		const int longestLabel = 5;
		char** pointers[nparams] = { &username,&port };
		int i,llen;
		for (i = 0; i < nparams; i++) *pointers[i] = 0;
		while ((in_char + len) - citem > longestLabel) {
			for (i = 0; i < nparams; i++) {
				llen = strlen(labels[i]);
				if (!strncmp(labels[i],citem,llen)) {
					*pointers[i] = citem + llen;
					break;
				}
			}
			/* scan for \n || \0.  NOTE: this will silently ignore lines
			 * that don't start with one of our known labels. */
			while (citem - in_char < len && *citem != '\n' && *citem) citem++;
			if (citem - in_char == len) break; /* end of input. */
			*citem = 0; /* ensure each param is a c-string. */
			citem++;
		}

		/* get hostname from origin. */
		/* XXX XXX XXX
		 * The origin token has at this point been cleared.  So we have
		 * to record it in the FILTER_PROTOCOL above.  But then we also
		 * need to make sure the recorded version corresponds to the same
		 * connection as this one... :\ not very convenient.
		 * */
		if (wsi != origin_ws) {
			fprintf(stderr, "======> wsi struct changed before callback\n");
		}
		/* for local host (mainly used for debugging), webkit sets origin
		 * to file://, and firefox sets origin to be "null". */
		if (!strncmp(origin,"file://",7) ||
				(strncmp(origin,"null",4) && !origin[4])) {
			hostname = "localhost";
		} else if (!strncmp(origin,"http://",7)) {
			hostname = origin + 7;
		} else if (!strncmp(origin,"https://",8)) {
			hostname = origin + 8;
		} else {
			fprintf(stderr, "No protocol specified in origin.\n");
			hostname = origin;
		}
		/* remove the port, if present */
		char* colon = strchr(hostname,':');
		if (colon) *colon = 0;
		/* TODO: read the rfc and make sure these are the only cases
		 * you have to deal with.  Also need to make sure there aren't
		 * more url components patched on to the end. */

		/* now verify contents of parameters: */
		/* https://en.wikipedia.org/wiki/Hostname#Restrictions_on_valid_host_names
		 * says a hostname must be < 256 chars, and in a-z,0-9 and '-' */
		char c;
		int paramOK = 1;
		size_t paramLen = strnlen(hostname,256);
		if (paramLen > 255) paramOK = 0;
		else {
			for (i = 0; i < paramLen; i++) {
				c = tolower(hostname[i]);
				if (!(('0' <= c && c <= '9') ||
						('a' <= c && c <= 'z') ||
						c == '-' || c == '.')) {
					paramOK = 0;
					break;
				}
			}
		}
		if (!paramOK) {
			#if CHECK_STATES
			psd->state = rq_error;
			#endif
			lwsl_err("Invalid hostname / origin: %s. Hanging up.",hostname);
			return 1;
		}
		/* now the username.  should be <= 32 chars and only a-z,0-9,_ */
		paramOK = 1;
		paramLen = strnlen(username,33);
		if (paramLen > 32) paramOK = 0;
		else {
			for (i = 0; i < paramLen; i++) {
				c = tolower(username[i]);
				if (!(('0' <= c && c <= '9') ||
						('a' <= c && c <= 'z') || c == '_')) {
					paramOK = 0;
					break;
				}
			}
		}
		if (!paramOK) {
			#if CHECK_STATES
			psd->state = rq_error;
			#endif
			lwsl_err("Invalid username: %s. Hanging up.",username);
			return 1;
		}
		/* finally, check the port.  if null, set to "22". */
		if (!port[0]) port = "22";
		paramOK = atoi(port);
		if (!paramOK) {
			/* XXX: 0 is technically a valid port number. */
			#if CHECK_STATES
			psd->state = rq_error;
			#endif
			lwsl_err("Invalid port: %s. Hanging up.",port);
			return 1;
		}

		/* at this point hostname, username, port should be strings
		 * that make some kind of sense, and have no newlines (thus
		 * preventing introduction of new ssh options in the config file).
		 * So now write the config file:
		 * */
		tsshconf = fopen(tfname,"wb");
		if (!tsshconf) {
			#if CHECK_STATES
			psd->state = rq_error;
			#endif
			lwsl_err("Couldn't write config file %s.",tfname);
			return 1;
		}
		fprintf(tsshconf,
				"Host webauth\n"
				"	HostName = %s\n"
				"	User	 = %s\n"
				"	Port	 = %s\n"
				"	UserKnownHostsFile	= %s\n"
				"	StrictHostKeyChecking = no\n",
				hostname,username,port,knownhostfile);
		if (keyfile[0]) {
			fprintf(tsshconf,
				"	IdentityFile = %s\n", keyfile);
		}
		fclose(tsshconf);
		/* NOTE: since this thing is single threaded, we reuse the
		 * same tempfile, irrespective of the connection. */

		char command[128];
		sprintf(command,"ssh -F '%s' webauth",tfname);
		fprintf(stderr, "Running command: %s\n",command);
		unsigned char* token =
			(unsigned char*)(psd->buf + LWS_SEND_BUFFER_PRE_PADDING + 3);
		/* remember: first 3 bytes are for the return code. */
		FILE* output = popen(command,"r");
		if (!output) {
			fprintf(stderr, "popen call failed.\n");
			return 1;
		}
		size_t bread = fread(token,1,MAX_TOKEN_LEN,output);
		/* we expect eof. */
		if (!feof(output)) {
			if (ferror(output)) {
				fprintf(stderr, "error reading output.\n");
			} else {
				fprintf(stderr, "incomplete read of token,"
						" or token too long.\n");
			}
			return 1;
		}
		/* else entire token (or error message) successfully read. */
		int rcode = WEXITSTATUS(pclose(output));
		*(token - 3) = (unsigned char)rcode;
		*(token - 2) = 0;
		*(token - 1) = 0;
		switch (rcode) {
			case success:
				fprintf(stderr, "token acquired :D\n");
				/* we stick with binary for now, so no formatting. */
				#if 0
				/* strip any newlines and make sure it is a c-string. */
				token[bread] = 0;
				while (bread && token[bread-1] == '\n')
					token[--bread] = 0;
				#endif
				//fprintf(stderr, "we got %s for the token\n",token);
				break;
			case unknown_key:
			case misc_badness:
			case server_error:
			case ssh_error:
				fprintf(stderr, "Return code %i from server.\n",rcode);
				break;
		}
		#if CHECK_STATES
		psd->state = rq_sending_token;
		#endif
		/* queue it for sending over websock. */
		psd->len = bread + 3; /* +3 is the return code. */
		libwebsocket_callback_on_writable(context,wsi);

		break;

	default:
		break;
	}

	return 0;
}

static struct libwebsocket_protocols protocols[] = {
	/* first protocol must always be HTTP handler */
	{
		"default",		/* name */
		callback_acquire,		/* callback */
		sizeof(struct per_session_data__auth)	/* per_session_data_size */
	},
	{
		NULL, NULL, 0		/* End of list */
	}
};

void sighandler(int sig)
{
	force_exit = 1;
}

static struct option long_opts[] = {
	{ "help",	no_argument,		NULL, 'h' },
	{ "debug",	required_argument,	NULL, 'd' },
	{ "port",	required_argument,	NULL, 'p' },
	{ "keyfile",	required_argument,	NULL, 'k' },
	{ "knownhosts",	required_argument,	NULL, 'K' },
	{ "interface",  required_argument,	NULL, 'i' },
	{ "daemonize", 	no_argument,		NULL, 'D' },
	{ NULL, 0, 0, 0 }
};

int main(int argc, char **argv)
{
	struct libwebsocket_context *context;
	int opts = 0;
	char interface_name[128] = "";
	const char *interface = NULL;
	int syslog_options = LOG_PID | LOG_PERROR;
	int listen_port = 7681;
	struct lws_context_creation_info info;
	int debug_level = 7;
	int daemonize = 0;

	memset(&info, 0, sizeof info);

	char c;
	int opt_index = 0;
	while ((c = getopt_long(argc, argv, "hd:k:p:i:DK:", long_opts,
					&opt_index)) != -1) {
		switch (c) {
		case 'D':
			daemonize = 1;
			syslog_options &= ~LOG_PERROR;
			break;
		case 'd':
			debug_level = atoi(optarg);
			break;
		case 'p':
			listen_port = atoi(optarg);
			break;
		case 'k':
			strncpy(keyfile,optarg,sizeof(keyfile));
			keyfile[sizeof(keyfile)-1] = 0;
			break;
		case 'K':
			strncpy(knownhostfile,optarg,sizeof(knownhostfile));
			knownhostfile[sizeof(knownhostfile)-1] = 0;
			break;
		case 'i':
			strncpy(interface_name, optarg, sizeof interface_name);
			interface_name[(sizeof interface_name) - 1] = '\0';
			interface = interface_name;
			break;
		case '?':
		case 'h':
			fprintf(stderr,
				"Usage: %s [OPTIONS]...\n"
				"Websocket-speaking daemon to acquire authentication tokens\n"
				"from compatible websites.\n\n"
				"   -p,--port       NUM     listen on port NUM. (default:%i)\n"
				"   -d,--debug      NUM     set debug level to NUM.\n"
				"   -k,--keyfile    FILE    specify IdentityFile for ssh.\n"
				"   -K,--knownhosts FILE    specify special ssh known_hosts file\n"
				"                           to store host keys acquired by authd\n"
				"                           (defaults to %s)\n"
				"   -D,--daemonize          run in background.\n"
				"   --help                  show this message and exit.\n",
					argv[0],listen_port,knownhostfile);
			exit(1);
		}
	}

	/*
	 * normally lock path would be /var/lock/lwsts or similar, to
	 * simplify getting started without having to take care about
	 * permissions or running as root, set to /tmp/.lwsts-lock
	 */
	if (daemonize && lws_daemonize("/tmp/.lwstecho-lock")) {
		fprintf(stderr, "Failed to daemonize\n");
		return 1;
	}

	/* we will only try to log things according to our debug_level */
	setlogmask(LOG_UPTO (LOG_DEBUG));
	openlog("lwsts", syslog_options, LOG_DAEMON);

	/* tell the library what debug level to emit and to send it to syslog */
	lws_set_log_level(debug_level, lwsl_emit_syslog);

	info.port = listen_port;
	info.iface = interface;
	info.protocols = protocols;
#ifndef LWS_NO_EXTENSIONS
	info.extensions = libwebsocket_get_internal_extensions();
#endif
	info.gid = -1;
	info.uid = -1;
	info.options = opts;

	context = libwebsocket_create_context(&info);

	if (context == NULL) {
		lwsl_err("libwebsocket init failed\n");
		return -1;
	}

	signal(SIGINT, sighandler);

	/* setup temp file: */
	int fd = mkstemp(tfname);
	if (fd == -1) {
		fprintf(stderr, "Couldn't create temp file %s\n",tfname);
		return 1;
	}
	close(fd); /* file created.  we'll use it later. */

	int n = 0;
	while (n >= 0 && !force_exit) {
		/* The timeout parameter can be used to compromise between
		 * responsiveness and CPU usage. We use 2 tenths of a second: */
		n = libwebsocket_service(context, 200);
	}
	libwebsocket_context_destroy(context);

	unlink(tfname); /* remove temp file. */

	closelog();

	return 0;
}
