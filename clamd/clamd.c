/*
 *  Copyright (C) 2002, 2003 Tomasz Kojm <zolw@konarski.edu.pl>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <pwd.h>
#include <grp.h>
#include <clamav.h>

#if defined(CLAMD_USE_SYSLOG) && !defined(C_AIX)
#include <syslog.h>
#endif

#include "options.h"
#include "cfgfile.h"
#include "others.h"
#include "tcpserver.h"
#include "localserver.h"
#include "others.h"
#include "defaults.h"

void help(void);
void daemonize(void);

void clamd(struct optstruct *opt)
{
	struct cfgstruct *copt, *cpt;
        struct passwd *user;
	time_t currtime;
	struct cl_node *root = NULL;
	const char *dbdir, *cfgfile;
	int ret, virnum, tcpsock;

    /* initialize some important variables */

    if(optc(opt, 'V')) {
	printf("clamd / ClamAV version "VERSION"\n");
	exit(0);
    }

    if(optc(opt, 'h')) {
    	help();
    }

    /* parse the config file */
    if(optc(opt, 'c'))
	cfgfile = getargc(opt, 'c');
    else
	cfgfile = CL_DEFAULT_CFG;

    if((copt = parsecfg(cfgfile)) == NULL) {
	fprintf(stderr, "ERROR: Can't parse the config file %s\n", cfgfile);
	exit(1);
    }

    /* initialize logger */

    if(cfgopt(copt, "LogFileUnlock"))
	loglock = 0;
    else
	loglock = 1;

    if(cfgopt(copt, "LogTime"))
	logtime = 1;
    else
	logtime = 0;

    if((cpt = cfgopt(copt, "LogFileMaxSize")))
	logsize = cpt->numarg;
    else
	logsize = CL_DEFAULT_LOGSIZE;

    if(cfgopt(copt, "Debug")) /* enable debug messages in libclamav */
	cl_debug();

    if(cfgopt(copt, "LogVerbose"))
	logverbose = 1;
    else
	logverbose = 0;

    if((cpt = cfgopt(copt, "LogFile"))) {
	logfile = cpt->strarg;
	if(logfile[0] != '/') {
	    fprintf(stderr, "ERROR: LogFile requires full path.\n");
	    exit(1);
	}
	time(&currtime);
	if(logg("+++ Started at %s", ctime(&currtime))) {
	    fprintf(stderr, "ERROR: Problem with internal logger. Please check the permissions on the %s file.\n", logfile);
	    exit(1);
	}
    } else
	logfile = NULL;


#if defined(CLAMD_USE_SYSLOG) && !defined(C_AIX)
    if((cpt = cfgopt(copt, "LogSyslog"))) {
	openlog("clamd", LOG_PID, LOG_LOCAL6);
	use_syslog = 1;
	syslog(LOG_INFO, "Daemon started.\n");
    } else
	use_syslog = 0;
#endif


    if(logsize)
	logg("Log file size limited to %d bytes.\n", logsize);
    else
	logg("Log file size limit disabled.\n");

    logg("*Verbose logging activated.\n");


    /* check socket type */

    if(cfgopt(copt, "TCPSocket") && cfgopt(copt, "LocalSocket")) {
	fprintf(stderr, "ERROR: You can select one mode only (local/TCP).\n");
	logg("!Two modes (local & TCP) selected.\n");
	exit(1);
    } else if(cfgopt(copt, "TCPSocket")) {
	tcpsock = 1;
    } else if(cfgopt(copt, "LocalSocket")) {
	tcpsock = 0;
    } else {
	fprintf(stderr, "ERROR: You must select server type (local/tcp).\n");
	logg("!Please select server type (local/TCP).\n");
	exit(1);
    }

    /* drop priviledges */
    if(getuid() == 0 && (cpt = cfgopt(copt, "User"))) {
	if((user = getpwnam(cpt->strarg)) == NULL) {
	    fprintf(stderr, "ERROR: Can't get information about user %s.\n", cpt->strarg);
	    logg("!Can't get information about user %s.\n", cpt->strarg);
	    exit(1);
	}

	if(cfgopt(copt, "AllowSupplementaryGroups")) {
	    initgroups(cpt->strarg, user->pw_gid);
	} else
	    setgroups(1, &user->pw_gid);

	setgid(user->pw_gid);
	setuid(user->pw_uid);
	logg("Running as user %s (UID %d, GID %d)\n", cpt->strarg, user->pw_uid, user->pw_gid);
    }

    /* load the database(s) */

    if((cpt = cfgopt(copt, "DataDirectory")))
	dbdir = cpt->strarg;
    else
	dbdir = cl_retdbdir();

    logg("Reading databases from %s\n", dbdir);

    if((ret = cl_loaddbdir(dbdir, &root, &virnum))) {
	fprintf(stderr, "ERROR: %s\n", cl_strerror(ret));
	logg("!%s\n", cl_strerror(ret));
	exit(1);
    }

    if(!root) {
	fprintf(stderr, "ERROR: Database initialization error.\n");
	logg("!Database initialization error.\n");
	exit(1);
    }

    logg("Protecting against %d viruses.\n", virnum);
    cl_buildtrie(root);


    /* fork into background */
    if(!cfgopt(copt, "Foreground"))
	daemonize();


    if(tcpsock)
	ret = tcpserver(opt, copt, root);
    else
	ret = localserver(opt, copt, root);

}

void help(void)
{

    printf("\n");
    printf("		   Clam AntiVirus Daemon, "VERSION"\n");
    printf("		   (c) 2002 Tomasz Kojm <zolw@konarski.edu.pl>\n");
    printf("	  \n");
    printf("    --help		    -h		Show help.\n");
    printf("    --version		    -V		Show version number.\n");
    printf("    --config-file=FILE	    -c FILE	Read configuration from FILE.\n\n");

    exit(0);
}

void daemonize(void)
{
	int i;

    for(i = 0; i <= 2; i++)
	close(i);

    umask(0);
    chdir("/");

    if(fork())
	exit(0);

    setsid();
}
