/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/* gck-rpc-daemon-standalone.c - A sample daemon.

   Copyright (C) 2008, Stef Walter

   The Gnome Keyring Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   The Gnome Keyring Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with the Gnome Library; see the file COPYING.LIB.  If not,
   write to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.

   Author: Stef Walter <stef@memberwebs.com>
*/

#include "config.h"

#include "pkcs11/pkcs11.h"

#include "gck-rpc-layer.h"
#include "gck-rpc-tls-psk.h"

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdbool.h>

#include <dlfcn.h>
#include <pthread.h>

#include <syslog.h>

#ifdef __MINGW32__
# include <winsock2.h>
#endif

#define SOCKET_PATH "tcp://127.0.0.1"

#ifdef SECCOMP
#include <seccomp.h>
//#include "seccomp-bpf.h"
#ifdef DEBUG_SECCOMP
# include "syscall-reporter.h"
#endif /* DEBUG_SECCOMP */
#include <fcntl.h> /* for seccomp init */
#endif /* SECCOMP */


static int install_syscall_filter(const int sock, const char *tls_psk_keyfile, const char *path)
{
#ifdef SECCOMP
	int rc = -1;
	scmp_filter_ctx ctx;

#ifdef DEBUG_SECCOMP
	ctx = seccomp_init(SCMP_ACT_TRAP);
#else
	ctx = seccomp_init(SCMP_ACT_KILL);
#endif /* DEBUG_SECCOMP */
	if (ctx == NULL)
		goto failure_scmp;
	/*
	 * These are the basic syscalls needed to be able to use
	 * the syscall-reporter to figure out the rest
	 */
	seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(write), 0);
#ifdef DEBUG_SECCOMP
	seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(rt_sigreturn), 0);
# ifdef __NR_sigreturn
	seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(sigreturn), 0);
# endif
#endif /* DEBUG_SECCOMP */
	seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(exit), 0);
	seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(exit_group), 0);

	/*
	 * Network related syscalls.
	 */
	seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(read), 0);
	seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(select), 0);
	if (sock)
		/* Allow accept() only for the listening socket */
		seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(accept), 1,
				 SCMP_A0(SCMP_CMP_EQ, sock));
	seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(sendto), 0);
	if (path[0] &&
	    strncmp(path, "tcp://", strlen("tcp://")) == 0) {
		/* TCP socket - not needed for TLS */
		seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(recvfrom), 0);
	}

	/*
	 * These are probably pthreads-related.
	 */
	seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(mmap), 0);
	seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(munmap), 0);
	seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(mprotect), 0);
	seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(clone), 0);
	seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(set_robust_list), 0);
	seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(madvise), 0);
	seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(munlock), 0);

	/*
	 * Both pthreads (? file is "/sys/devices/system/cpu/online") and TLS-PSK open files.
	 */
	seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(open), 1,
			 SCMP_A1(SCMP_CMP_EQ, O_RDONLY | O_CLOEXEC));

	seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(close), 0);

	/*
	 * UNIX domain socket
	 */
	if (path[0] &&
	    strncmp(path, "tcp://", strlen("tcp://")) != 0 &&
	    strncmp(path, "tls://", strlen("tls://")) != 0) {
		seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(unlink), 0);
	}

	/*
	 * Allow spawned threads to initialize a new seccomp policy (subset of this).
	 */
	seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(prctl), 0);

	/*
	 * SoftHSM 1.3.0 required syscalls
	 */
	seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(getcwd), 0);
	seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(stat), 0);
	seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(open), 0);
	seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(fcntl), 0);
	seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(fstat), 0);
	seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(lseek), 0);
	seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(access), 0);
	seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(fsync), 0);
	seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(unlink), 0);
	seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(ftruncate), 0);
	seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(select), 0);
	seccomp_rule_add(ctx,SCMP_ACT_ALLOW, SCMP_SYS(futex), 0);

#ifdef DEBUG_SECCOMP
	/* Dumps the generated BPF rules in sort-of human readable syntax. */
	seccomp_export_pfc(ctx,STDERR_FILENO);

	/* Print the name of syscalls stopped by seccomp. Should not be used in production. */
        if (install_syscall_reporter())
                return 1;
#endif /* DEBUG_SECCOMP */

	rc = seccomp_load(ctx);
	if (rc < 0)
		goto failure_scmp;
	seccomp_release(ctx);

	return 0;

failure_scmp:
	errno = -rc;
	fprintf(stderr, "Seccomp filter initialization failed, errno = %u\n", errno);
	return errno;
#else /* SECCOMP */
        return 0;
#endif /* SECCOMP */
}


#if 0
/* Sample configuration for loading NSS remotely */
static CK_C_INITIALIZE_ARGS p11_init_args = {
	NULL,
	NULL,
	NULL,
	NULL,
	CKF_OS_LOCKING_OK,
	"init-string = configdir='/tmp' certPrefix='' keyPrefix='' secmod='/tmp/secmod.db' flags="
};
#endif

static int is_running = 1;

static int MAX_MODULES = 5 ;
static int LINELEN = 255 ;
static CK_FUNCTION_LIST_PTR *pos = NULL;
static void **modhandles = NULL;

static int usage(void)
{
	fprintf(stderr, "usage: pkcs11-daemon pkcs11-module [<socket>|\"-\"]\n\tUsing \"-\" results in a single-thread inetd-type daemon\n");
	exit(2);
}

void termination_handler (int signum)
{
	is_running = 0;
}

enum {
	/* Used to un-confuse clang checker */
	GCP_RPC_DAEMON_MODE_INETD = 0,
	GCP_RPC_DAEMON_MODE_SOCKET
};

void truncate_newline(char *string){
	size_t len = strlen(string);
	
	if (len > 0 && string[len-1] == '\n') {
		string[--len] = '\0';
	}
	return ;
}

int read_config(const char *config_filename, char **paths)
{
	FILE *fptr;
	int i = 0 ;
	// Open a file in read mode
	fptr = fopen(config_filename, "r");
	
	// If the file exists
	if(fptr != NULL) {
		
		// Read the content and print it
		paths[0] = malloc(LINELEN * sizeof(char)) ;
		while(fgets(paths[i++], LINELEN, fptr) &&
		      i <= MAX_MODULES) {
			truncate_newline(paths[i-1]) ;
			// allocate memory for the next pathname
			paths[i] = malloc(LINELEN * sizeof(char)) ;
			printf("PATH: %s\n", paths[i-1]);
		}

	} else {
		printf("Not able to open the file.");
		return -1 ;
	}

	// Close the file
	fclose(fptr);
	printf("Leaving read_config\n") ;
	return i-1 ;
}

void *p11dlopen(const char *filename)
{
	fprintf(stderr, "dlopen: trying file %s\n", filename) ;
	return dlopen(filename, RTLD_LAZY);
}

void *p11dlsym(void *handle, const char *symbol)
{
	return dlsym(handle, symbol);
}

const char *p11dlerror()
{
	return dlerror();
}

int p11dlclose(void **handle)
{
	return dlclose(&handle);
}

#define LOADFLAG			0x11011011

struct p11_module 
{
	unsigned int loadFlag;
	void *handle;
};

typedef struct p11_module p11_module_t;

CK_RV unloadp11module(void *module)
{
	p11_module_t *mod = (p11_module_t *) module;

	if(!mod || mod->loadFlag != LOADFLAG)
		return CKR_ARGUMENTS_BAD;

	if(p11dlclose(&mod->handle) < 0)
		return CKR_FUNCTION_FAILED;

	memset(mod, 0, sizeof(*mod));

	free(mod);

	return CKR_OK;
}

int init(){
	if(pos && pos != NULL){
		free(pos);
		pos = NULL;
	}

	if(modhandles && modhandles != NULL){
		free(modhandles);
		modhandles = NULL;
	}

	return 0 ;
}

void *loadp11module(const char *mspec, CK_FUNCTION_LIST_PTR_PTR funcs)
{
	p11_module_t *mod;
	CK_RV rv, (*c_get_function_list)(CK_FUNCTION_LIST_PTR_PTR);
	mod = (p11_module_t *) calloc(1, sizeof(*mod));
	mod->loadFlag = LOADFLAG;

	if(mspec == NULL)
		return NULL;

	mod->handle = p11dlopen(mspec);

	if(mod->handle == NULL)
	{
		printf("loadp11module: p11dlopen failed: %s\n", p11dlerror());
		goto failed;
	}

	c_get_function_list = (CK_RV (*)(CK_FUNCTION_LIST_PTR_PTR)) p11dlsym(mod->handle, "C_GetFunctionList");

	if(!c_get_function_list)
		goto failed;

	rv = c_get_function_list(funcs);
	
	if (rv == CKR_OK)
		return (void *) mod;
	else
		printf("C_GetFunctionList failed %lx", rv);

failed:

	unloadp11module((void *) mod);
	return NULL;
}

int main(int argc, char *argv[])
{
	CK_C_GetFunctionList func_get_list;
	CK_FUNCTION_LIST_PTR funcs;
	void *module;
	const char *path, *tls_psk_keyfile;
	fd_set read_fds;
	int sock, ret, mode;
	CK_RV rv;
	CK_C_INITIALIZE_ARGS init_args;
	GckRpcTlsPskState *tls;
	char **module_paths = calloc(MAX_MODULES, LINELEN) ;

	if (init() != 0){
		fprintf(stderr,"could not initialise pointers\n") ;
		exit(1) ;
	}

	int number_modules = read_config(argv[1], module_paths) ;
	
	if ( number_modules <= 0){
		fprintf(stderr, "could not load module paths\n") ;
		exit(1) ;
	} else {
		fprintf(stderr, "GOT %d MODULES from config\n", number_modules) ;
	}

	/* The path to config file is the first argument argv[1] */
	if (argc != 2 && argc != 3)
		usage();

        openlog("pkcs11-proxy",LOG_CONS|LOG_PID,LOG_DAEMON);

	modhandles = (void **) calloc(nCounter, sizeof(void *));

	if(!modhandles){
		return CKR_GENERAL_ERROR;
	}

	pos = new CK_FUNCTION_LIST_PTR[number_modules];

	if(!pos){
		return CKR_GENERAL_ERROR;
	}

	for(int i = 0; i < number_modules; i++){
		/* Load the library */
		//module = dlopen(argv[1], RTLD_NOW);
		
		// @@TODO: this loads only the first library and gets funcs
		// into exactly one ptr, not an array of them
		module = loadp11module(module_paths[0], &funcs) ;
		
		if (!module) {
			fprintf(stderr, "couldn't open library: %s: %s\n", argv[1],
				dlerror());
			exit(1);
		}
	}
	
	/* Lookup the appropriate function in library */
	/*
	func_get_list =
	    (CK_C_GetFunctionList) dlsym(module, "C_GetFunctionList");
	if (!func_get_list) {
		fprintf(stderr,
			"couldn't find C_GetFunctionList in library: %s: %s\n",
			argv[1], dlerror());
		exit(1);
	}
	*/
	/* Get the function list */
	/*rv = (func_get_list) (&funcs);
	if (rv != CKR_OK || !funcs) {
		fprintf(stderr,
			"couldn't get function list from C_GetFunctionList"
			"in libary: %s: 0x%08x\n",
			argv[1], (int)rv);
		exit(1);
	}
	*/
	
	/* RPC layer expects initialized module */
	memset(&init_args, 0, sizeof(init_args));
	init_args.flags = CKF_OS_LOCKING_OK;

	rv = (funcs->C_Initialize) (&init_args);
	if (rv != CKR_OK) {
		fprintf(stderr, "couldn't initialize module: %s: 0x%08x\n",
			argv[1], (int)rv);
		exit(1);
	}

	path = getenv("PKCS11_DAEMON_SOCKET");
	if (!path && argc == 3)
           path = argv[2];
        if (!path)
	   path = SOCKET_PATH;

	/* Initialize TLS, if appropriate */
	tls = NULL;
	tls_psk_keyfile = NULL;
	if (! strncmp("tls://", path, 6)) {
		tls_psk_keyfile = getenv("PKCS11_PROXY_TLS_PSK_FILE");
		if (! tls_psk_keyfile || ! tls_psk_keyfile[0]) {
			fprintf(stderr, "key file must be specified for tls:// socket.\n");
			exit(1);
		}

		tls = calloc(1, sizeof(GckRpcTlsPskState));
		if (tls == NULL) {
			fprintf(stderr, "can't allocate memory for TLS-PSK");
			exit(1);
		}

		if (! gck_rpc_init_tls_psk(tls, tls_psk_keyfile, NULL, GCK_RPC_TLS_PSK_SERVER)) {
			fprintf(stderr, "TLS-PSK initialization failed");
			exit(1);
		}
	}

	if (strcmp(path,"-") == 0) {
		/* inetd mode */
		sock = 0;
		mode = GCP_RPC_DAEMON_MODE_INETD;
	} else {
		/* Do some initialization before enabling seccomp. */
		sock = gck_rpc_layer_initialize(path, funcs);
		if (sock == -1)
			exit(1);

		/* Shut down gracefully on SIGTERM. */
		if (signal (SIGTERM, termination_handler) == SIG_IGN)
			signal (SIGTERM, SIG_IGN);

		mode = GCP_RPC_DAEMON_MODE_SOCKET;
	}

	/*
	 * Enable seccomp. This is essentially a whitelist containing all the syscalls
	 * we expect to call from here on. Anything not whitelisted will cause the
	 * process to terminate.
	 */
        if (install_syscall_filter(sock, tls_psk_keyfile, path))
        	return 1;

        if (mode == GCP_RPC_DAEMON_MODE_INETD) {
           gck_rpc_layer_inetd(funcs);
        } else if (mode == GCP_RPC_DAEMON_MODE_SOCKET) {
	   is_running = 1;
	   while (is_running) {
		FD_ZERO(&read_fds);
		FD_SET(sock, &read_fds);
		ret = select(sock + 1, &read_fds, NULL, NULL, NULL);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "error watching socket: %s\n",
				strerror(errno));
			exit(1);
		}

		if (FD_ISSET(sock, &read_fds))
			gck_rpc_layer_accept(tls);
	   }

	   gck_rpc_layer_uninitialize();
        } else {
		/* Not reached */
		exit(-1);
	}

	rv = (funcs->C_Finalize) (NULL);
	if (rv != CKR_OK)
		fprintf(stderr, "couldn't finalize module: %s: 0x%08x\n",
			argv[1], (int)rv);

	dlclose(module);

	if (tls) {
		gck_rpc_close_tls(tls);
		free(tls);
		tls = NULL;
	}

	return 0;
}
